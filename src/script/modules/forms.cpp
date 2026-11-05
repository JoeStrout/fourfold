// Showing a declared form again (overview §4, "Showing errors and keeping
// input"):
//
//     formState(name)               what form.<name>'s fragment needs: each
//                                   field's value, and `errors` (by field, and
//                                   `errors.form`); empty, unless a reshow()
//                                   without JavaScript just left them
//     reshow(fragName, extraErrors) the form again, with the visitor's input
//                                   and the errors: with HTMX, the fragment
//                                   itself, replacing the form (422); without,
//                                   a redirect to the handler's page, whose
//                                   formState() picks them up, once
//
// And rate limits from code (overview §5, Rate limits):
//
//     ratelimit(key, rate)          counts this request against `key`; 1 if
//                                   within `rate` ("20/hour"), else 0
//
// The state a reshow() without JavaScript leaves is kept in the engine's
// part of the session, which app code can't reach, under "forms", by the
// form's key ("pages/guestbook.ms#sign").

#include "../bridge.h"
#include "../host_impl.h"
#include "data_work.h"
#include "modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

namespace {

const char* const kFormsKey = "forms";

bool isTextType(FormFieldType t) {
    return t == FormFieldType::Text || t == FormFieldType::Multiline || t == FormFieldType::Email ||
           t == FormFieldType::Url || t == FormFieldType::Number || t == FormFieldType::Int ||
           t == FormFieldType::Date;
}

bool contains(const Datum& list, const std::string& s) {
    if (list.kind() != Datum::Kind::List) return false;
    for (const Datum& d : list.asList()) {
        if (d.kind() == Datum::Kind::String && d.asString() == s) return true;
    }
    return false;
}

// The map a form's fragment fills its fields from: each declared field (but
// files) by name, and `errors`. Text is the value as typed ("" if none); a
// bool is 1 or 0; a choice or choices is a map from each option to 1 or 0,
// for `checked="{{mood.happy}}"` and `selected="{{mood.happy}}"`.
Value formFields(const FormSpec& spec, const Datum& values, const Datum& errors) {
    Value map = Value::make_map(static_cast<int>(spec.fields.size()) + 1);
    const Datum::MapItems* given = values.kind() == Datum::Kind::Map ? &values.asMap() : nullptr;
    for (const FormField& f : spec.fields) {
        if (f.type == FormFieldType::File) continue;
        Datum v;
        if (given) {
            auto it = given->find(f.name);
            if (it != given->end()) v = it->second;
        }
        if (isTextType(f.type)) {
            map.MapSet(toValue(f.name), toValue(v.kind() == Datum::Kind::String ? v.asString() : std::string()));
        } else if (f.type == FormFieldType::Bool) {
            bool on = v.kind() == Datum::Kind::String && !v.asString().empty();
            map.MapSet(toValue(f.name), on ? Value::one : Value::zero);
        } else {
            Value options = Value::make_map(static_cast<int>(f.options.size()));
            for (const std::string& o : f.options) {
                bool on = (v.kind() == Datum::Kind::String && v.asString() == o) || contains(v, o);
                options.MapSet(toValue(o), on ? Value::one : Value::zero);
            }
            map.MapSet(toValue(f.name), options);
        }
    }
    map.MapSet("errors", errors.kind() == Datum::Kind::Map ? toValue(errors) : Value::make_map(0));
    return map;
}

// The run, if it's a request run with an app; else null with `error` set.
Run* requestRun(Context& ctx, const char* name, IntrinsicResult& error) {
    Run* run = Run::of(ctx);
    if (!run || run->kind != Run::Kind::Request || !run->app) {
        error = intrinsicError(std::string(name) + " can't be used at a page's top level; call it in render or "
                                                   "a handler");
        return nullptr;
    }
    return run;
}

// The engine session's "forms" map, as a copy.
Datum::MapItems storedForms(const Run& run) {
    if (run.engineSession.kind() != Datum::Kind::Map) return {};
    auto it = run.engineSession.asMap().find(kFormsKey);
    if (it == run.engineSession.asMap().end() || it->second.kind() != Datum::Kind::Map) return {};
    return it->second.asMap();
}

void setStoredForms(Run& run, Datum::MapItems forms) {
    Datum::MapItems engine;
    if (run.engineSession.kind() == Datum::Kind::Map) engine = run.engineSession.asMap();
    if (forms.empty()) {
        engine.erase(kFormsKey);
    } else {
        engine[kFormsKey] = Datum::map(std::move(forms));
    }
    run.engineSession = Datum::map(std::move(engine));
    run.engineSessionChanged = true;
}

void addFormState() {
    Intrinsic f = Intrinsic::Create("formState");
    f.AddParam("name");
    f.set_Code([](Context ctx, IntrinsicResult) {
        IntrinsicResult error = IntrinsicResult::Null;
        Run* run = requestRun(ctx, "formState", error);
        if (!run) return error;
        Value nameValue = ctx.GetArg(0);
        if (!nameValue.IsString()) {
            return intrinsicError("formState needs the form's name, like formState(\"sign\") for form.sign");
        }
        std::string name = toStdString(nameValue);
        std::string key = run->handler.source.str() + "#" + name;
        auto spec = run->app->forms.find(key);
        if (spec == run->app->forms.end()) {
            return intrinsicError("formState: there's no form." + name + " in code/" + run->handler.source.str());
        }
        Datum values = Datum::map({});
        Datum errors = Datum::map({});
        Datum::MapItems forms = storedForms(*run);
        auto stored = forms.find(key);
        if (stored != forms.end()) {
            // Left by a reshow() without JavaScript: shown once.
            if (stored->second.kind() == Datum::Kind::Map) {
                const Datum::MapItems& m = stored->second.asMap();
                if (auto v = m.find("values"); v != m.end()) values = v->second;
                if (auto e = m.find("errors"); e != m.end()) errors = e->second;
            }
            forms.erase(stored);
            setStoredForms(*run, std::move(forms));
        }
        return IntrinsicResult(formFields(*spec->second, values, errors));
    });
}

// extraErrors as a Datum map of strings, or nullopt with `why` set.
std::optional<Datum::MapItems> extraErrorsFrom(Value v, const FormSpec& spec, std::string& why) {
    Datum::MapItems out;
    if (v.IsNull()) return out;
    if (!v.IsMap()) {
        why = "extraErrors must be a map from field names to messages, like {\"who\": \"That name is taken.\"}";
        return std::nullopt;
    }
    MiniScript::MapIterator it = v.Iterator();
    Value key, item;
    while (MiniScript::map_iterator_next(&it, &key, &item)) {
        std::string name = key.IsString() ? toStdString(key) : toStdString(key);
        if (name != "form" && !spec.find(name)) {
            why = "extraErrors has \"" + name + "\", which isn't a field of " + spec.label() +
                  " (use \"form\" for an error about the whole form)";
            return std::nullopt;
        }
        if (item.IsNull()) continue;
        if (!item.IsString()) {
            why = "extraErrors." + name + " must be a message string";
            return std::nullopt;
        }
        out[name] = Datum::string(toStdString(item));
    }
    return out;
}

void addReshow() {
    Intrinsic f = Intrinsic::Create("reshow");
    f.AddParam("fragName");
    f.AddParam("extraErrors", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult) {
        IntrinsicResult error = IntrinsicResult::Null;
        Run* run = requestRun(ctx, "reshow", error);
        if (!run) return error;
        const FormSpec* spec = run->context.form.get();
        if (!spec) {
            return intrinsicError("reshow can only be used in a handler with a form declaration (form." +
                                  run->handler.name + " = {...} for " + run->handler.label() + ")");
        }
        Value fragValue = ctx.GetArg(0);
        if (!fragValue.IsString()) return intrinsicError("reshow needs the form fragment's name, like reshow(\"signForm\")");
        std::string fragName = toStdString(fragValue);
        const CompiledTemplate* t = run->app->templates.fragment(fragName);
        if (!t) return intrinsicError("reshow: there's no code/frags/" + fragName + ".html");
        std::string why;
        auto extra = extraErrorsFrom(ctx.GetArg(1), *spec, why);
        if (!extra) return intrinsicError("reshow: " + why);

        Datum::MapItems errors;
        if (run->context.request.errors && run->context.request.errors->kind() == Datum::Kind::Map) {
            errors = run->context.request.errors->asMap();
        }
        for (auto& [k, v] : *extra) errors[k] = v;
        Datum errorMap = Datum::map(std::move(errors));
        const Datum& values = run->context.request.typed;

        bool htmx = false;
        for (const auto& [name, value] : run->context.request.headers) {
            htmx = htmx || (name == "hx-request" && value == "true");
        }
        if (htmx) {
            if (t->rootId().empty()) {
                return intrinsicError("reshow: the first element of code/" + t->source().str() +
                                      " needs an id (like <form id=\"" + fragName +
                                      "\" ...>), so HTMX can put the form back in its place");
            }
            Value fields = formFields(*spec, values, errorMap);
            RenderResult r = renderFragment(*t, ValueFields(fields), run->context.render);
            addWarnings(*run, r.warnings);
            if (!r.ok()) {
                return intrinsicError("code/" + r.error->file +
                                      (r.error->line ? " line " + std::to_string(r.error->line) : "") + ": " +
                                      r.error->message);
            }
            OutcomeHandle h;
            h.kind = OutcomeHandle::Kind::Status;
            h.status = 422;
            h.body = r.html.str();
            h.contentType = "text/html; charset=utf-8";
            h.hasScript = r.html.hasScript();
            h.headers = {{"HX-Retarget", "#" + t->rootId()}, {"HX-Reswap", "outerHTML"}};
            return IntrinsicResult(makeOutcomeHandle(std::move(h)));
        }

        // Without JavaScript: keep the input and errors for the page's
        // formState(), and go back to the page (redirect after POST).
        if (!run->context.session) {
            return intrinsicError("reshow needs sessions, to show the form again without JavaScript");
        }
        if (run->context.pageUrl.empty()) {
            return intrinsicError("reshow: code/" + run->handler.source.str() +
                                  " has no page to show the form on again (add a page beside it, or use "
                                  "status(422, ...) instead)");
        }
        Datum::MapItems forms = storedForms(*run);
        forms[spec->key()] = Datum::map({{"values", values}, {"errors", errorMap}});
        setStoredForms(*run, std::move(forms));
        OutcomeHandle h;
        h.kind = OutcomeHandle::Kind::Redirect;
        h.location = run->context.pageUrl;
        return IntrinsicResult(makeOutcomeHandle(std::move(h)));
    });
}

std::string rateSubject(const Run& run, RateRule::Per per) {
    switch (per) {
        case RateRule::Per::Ip: return "ip:" + run.context.rateIp;
        case RateRule::Per::User: return "user:" + run.context.rateUser;
        case RateRule::Per::Session: return "session:" + run.context.rateSession;
    }
    return "ip:" + run.context.rateIp;
}

}  // namespace

void addForms() {
    addFormState();
    addReshow();
}

void addRateLimit() {
    Intrinsic f = Intrinsic::Create("ratelimit");
    f.AddParam("key");
    f.AddParam("rate");
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        Run* run = Run::of(ctx);
        if (!run || run->kind != Run::Kind::Request) {
            return intrinsicError("ratelimit can't be used at a page's top level; call it in a handler");
        }
        if (!partial.done) return finishWork(*run, "ratelimit");
        Value key = ctx.GetArg(0);
        Value rate = ctx.GetArg(1);
        if (!key.IsString() || toStdString(key).empty()) {
            return intrinsicError("ratelimit needs a key string, like ratelimit(\"comment:\" + postId, \"20/hour\")");
        }
        if (!rate.IsString()) return intrinsicError("ratelimit needs a rate string, like \"20/hour\"");
        std::string why;
        std::optional<RateRule> rule = parseRate(toStdString(rate), why);
        if (!rule) return intrinsicError("ratelimit: " + why);
        RateCounter* rates = run->context.rates;
        if (!rates) return IntrinsicResult(Value::one);
        std::string full = "app:" + toStdString(key) + "|" + rateSubject(*run, rule->per);
        return startWork(ctx, *run, [rates, full, r = *rule] {
            AsyncResult result;
            result.value = Datum::number(rates->hit(full, r) ? 1 : 0);
            return result;
        });
    });
}

}  // namespace fourfold
