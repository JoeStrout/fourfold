// mail: email from app code (overview §6; decided for M9).
//
//     mail.send(to, subject, template, fields=null, options=null)
//
// `to` is a user's id (the engine finds their address), or an address that
// code/outbound.ms's mailTo allows (the site owner, say). `options` may
// have "bcc": a recipient, or a list of up to 10, by the same rules, sent
// the same message as blind copies (in no header, so no one sees them).
// There's no CC: it would show users' addresses to each other. `template` names
// a Markdown template in code/mail/ ("welcome" for code/mail/welcome.md),
// filled with `fields` like a fragment, and sent as text (its Markdown) and
// HTML. Structured fields only: nothing here can add a header or another
// recipient. The app may send config.mail.appPerHour messages an hour,
// counting each recipient; if any recipient is refused, nothing is sent.
//
// Returns null once the message is handed to the mail server, or an error
// value. Sent on the worker pool, with the VM parked.

#include "../bridge.h"
#include "../host_impl.h"
#include "data_work.h"
#include "modules.h"
#include "template/template.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

namespace {

constexpr size_t kMaxSubject = 200;
constexpr size_t kMaxBcc = 10;   // as the mail component's

std::string trimmedLower(std::string s) {
    size_t start = s.find_first_not_of(" \t");
    s = start == std::string::npos ? "" : s.substr(start);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A recipient from a string: an address (if it has an @), which mailTo must
// allow, or a user's id. "" if it's fine; else why not.
std::string recipientOf(const std::string& function, Run& run, Value v, const char* what, AppMailRecipient& out) {
    if (!v.IsString() || toStdString(v).empty()) {
        return function + " needs " + what + " as a user's id, or an address in code/outbound.ms's mailTo";
    }
    std::string text = toStdString(v);
    if (text.find('@') == std::string::npos) {
        out.userId = text;
        return "";
    }
    out.address = trimmedLower(text);
    if (!run.app->outbound.allowsMailTo(out.address)) {
        return function + ": " + out.address +
               " isn't in code/outbound.ms's mailTo; mail can go to a user (by their id), or to an address listed "
               "there";
    }
    return "";
}

// Fills the template once; false with `error` set if it failed.
bool fill(Run& run, const CompiledTemplate& t, Value fields, OutputFormat format, std::string& out,
          std::string& error) {
    RenderOptions options;
    options.format = format;
    RenderResult r = render(t, ValueFields(fields), options);
    addWarnings(run, r.warnings);
    if (!r.ok()) {
        error = "code/" + r.error->file + (r.error->line ? " line " + std::to_string(r.error->line) : "") + ": " +
                r.error->message;
        return false;
    }
    out = format == OutputFormat::Markdown ? r.markdown : r.html.str();
    return true;
}

}  // namespace

void addMail() {
    std::string function = "mail.send";
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("to");
    f.AddParam("subject");
    f.AddParam("template");
    f.AddParam("fields", Value::Null);
    f.AddParam("options", Value::Null);
    f.set_Code([function](Context ctx, IntrinsicResult partial) {
        Run* run = Run::of(ctx);
        if (!run || !run->canPark() || !run->app) {
            return intrinsicError(function + " can't be used at a page's top level; call it in a handler");
        }
        if (!partial.done) return finishWork(*run, function);
        std::shared_ptr<AppMailer> mailer = run->context.mailer;
        if (!mailer) return intrinsicError(function + " isn't available here");

        Value to = ctx.GetArg(0), subject = ctx.GetArg(1), name = ctx.GetArg(2), fields = ctx.GetArg(3),
              options = ctx.GetArg(4);
        AppMailMessage m;
        std::string why = recipientOf(function, *run, to, "who it's to", m.to);
        if (!why.empty()) return intrinsicError(why);
        if (!options.IsNull()) {
            if (!options.IsMap()) return intrinsicError(function + ": the options must be a map, like {\"bcc\": id}");
            MiniScript::MapIterator it = options.Iterator();
            Value key, value;
            while (MiniScript::map_iterator_next(&it, &key, &value)) {
                if (!key.IsString() || toStdString(key) != "bcc") {
                    return intrinsicError(function + ": unknown option " + toStdString(key) +
                                          " (the only one is \"bcc\"; there's no cc, which would show users' "
                                          "addresses to each other)");
                }
                std::vector<Value> items;
                if (value.IsList()) {
                    for (int i = 0; i < value.ListCount(); i++) items.push_back(value.ListGet(i));
                } else {
                    items.push_back(value);
                }
                if (items.size() > kMaxBcc) {
                    return intrinsicError(function + ": at most " + std::to_string(kMaxBcc) + " blind copies");
                }
                for (Value item : items) {
                    AppMailRecipient r;
                    why = recipientOf(function, *run, item, "each bcc", r);
                    if (!why.empty()) return intrinsicError(why);
                    m.bcc.push_back(std::move(r));
                }
            }
        }
        if (!subject.IsString()) return intrinsicError(function + " needs a subject, as a string");
        m.subject = toStdString(subject);
        for (unsigned char c : m.subject) {
            if (c < 0x20 || c == 0x7F) return intrinsicError(function + ": the subject can't have line breaks or tabs");
        }
        if (m.subject.empty() || m.subject.size() > kMaxSubject) {
            return intrinsicError(function + ": the subject must be 1 to " + std::to_string(kMaxSubject) + " bytes");
        }
        if (!name.IsString()) return intrinsicError(function + " needs a template's name, like \"welcome\"");
        auto t = run->app->mailTemplates.find(toStdString(name));
        if (t == run->app->mailTemplates.end()) {
            return intrinsicError(function + ": there's no code/mail/" + toStdString(name) + ".md");
        }
        if (!fields.IsNull() && !fields.IsMap()) return intrinsicError(function + ": the fields must be a map");
        std::string error, html;
        if (!fill(*run, t->second, fields, OutputFormat::Markdown, m.text, error)) return intrinsicError(error);
        if (!fill(*run, t->second, fields, OutputFormat::Html, html, error)) return intrinsicError(error);
        m.html = "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title></title></head>\n<body>\n" + html +
                 "</body></html>\n";
        return startNetworkWork(ctx, *run, [mailer, m = std::move(m)] {
            Result<std::string> sent = mailer->send(m);
            return sent ? AsyncResult{} : fail(sent.error());
        });
    });
    addModule("mail", {{"send", f}});
}

}  // namespace fourfold
