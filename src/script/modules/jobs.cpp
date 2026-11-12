// enqueue and publish: work for later, and messages for open pages
// (overview §2, Enqueuing; §7, Real-time push).
//
//     enqueue name, args=null, options=null
//
// Queues job.<name> (code/jobs/) to run with `args`, a map of inert values
// (checked now, like `session`). `options` may have "delay" (seconds) and
// "key" (a string: while a job with this key is waiting, enqueuing it again
// does nothing). The jobs a run enqueues are committed only if it succeeds.
//
//     publish channel, content, event="message"
//
// Sends `content` (frag(...), markdown(...), or a string, as text) to the
// pages subscribed to /live/<channel>, as a server-sent event. Also only if
// the run succeeds.
//
// Both collect into the run; the engine commits them after it.

#include <cmath>

#include "../bridge.h"
#include "../host_impl.h"
#include "modules.h"
#include "template/escape.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

namespace {

constexpr size_t kMaxArgsBytes = 64 * 1024;
constexpr size_t kMaxPerRun = 100;
constexpr double kMaxDelaySeconds = 366.0 * 24 * 3600;
constexpr size_t kMaxKeyBytes = 200;
constexpr size_t kMaxMessageBytes = 64 * 1024;

bool isEventName(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    }
    return true;
}

void addEnqueue() {
    Intrinsic f = Intrinsic::Create("enqueue");
    f.AddParam("name");
    f.AddParam("args", Value::Null);
    f.AddParam("options", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult) {
        const std::string function = "enqueue";
        Run* run = Run::of(ctx);
        if (!run || !run->canPark() || !run->app) {
            return intrinsicError("enqueue can't be used at a file's top level; call it in a handler or job");
        }
        if (!run->context.enqueue) return intrinsicError("enqueue isn't available here");
        Value nameValue = ctx.GetArg(0);
        if (!nameValue.IsString()) return intrinsicError("enqueue needs a job's name, like enqueue \"sendWelcome\"");
        std::string name = toStdString(nameValue);
        if (!run->app->jobs.count(name)) {
            return intrinsicError("enqueue: there's no job named " + name + " (job." + name + " in code/jobs/)");
        }
        JobRequest job;
        job.name = name;
        Value args = ctx.GetArg(1);
        if (!args.IsNull()) {
            if (!args.IsMap()) return intrinsicError("enqueue: the args must be a map, like {\"user\": id}");
            std::string why;
            std::optional<Datum> d = toDatum(args, why);
            if (!d) return intrinsicError("enqueue: the args " + why + " (only numbers, strings, lists and maps can be)");
            job.args = std::move(*d);
            if (toJson(job.args).size() > kMaxArgsBytes) {
                return intrinsicError("enqueue: the args are over " + std::to_string(kMaxArgsBytes / 1024) +
                                      " KB; pass an id, and look the rest up in the job");
            }
        }
        Value options = ctx.GetArg(2);
        if (!options.IsNull()) {
            if (!options.IsMap()) {
                return intrinsicError("enqueue: the options must be a map, like {\"delay\": 60, \"key\": \"x\"}");
            }
            MiniScript::MapIterator it = options.Iterator();
            Value key, value;
            while (MiniScript::map_iterator_next(&it, &key, &value)) {
                std::string option = key.IsString() ? toStdString(key) : "";
                if (option == "delay") {
                    double seconds = value.IsNumber() ? value.DoubleValue() : -1;
                    if (!(seconds >= 0 && seconds <= kMaxDelaySeconds)) {
                        return intrinsicError("enqueue: delay must be a number of seconds, from 0 to a year");
                    }
                    job.delayMs = static_cast<int64_t>(std::llround(seconds * 1000));
                } else if (option == "key") {
                    std::string k = value.IsString() ? toStdString(value) : "";
                    if (k.empty() || k.size() > kMaxKeyBytes) {
                        return intrinsicError("enqueue: key must be a string of 1 to " + std::to_string(kMaxKeyBytes) +
                                              " bytes");
                    }
                    job.key = k;
                } else {
                    return intrinsicError("enqueue: unknown option " + toStdString(key) +
                                          " (the options are \"delay\" and \"key\")");
                }
            }
        }
        if (run->enqueued.size() >= kMaxPerRun) {
            return intrinsicError("enqueue: at most " + std::to_string(kMaxPerRun) + " jobs per run");
        }
        run->enqueued.push_back(std::move(job));
        return IntrinsicResult::Null;
    });
}

void addPublish() {
    Intrinsic f = Intrinsic::Create("publish");
    f.AddParam("channel");
    f.AddParam("content");
    f.AddParam("event", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult) {
        Run* run = Run::of(ctx);
        if (!run || !run->canPark() || !run->app) {
            return intrinsicError("publish can't be used at a file's top level; call it in a handler or job");
        }
        if (!run->context.publish) {
            return intrinsicError("publish isn't available here (push needs `fourfold dev` or `fourfold serve`)");
        }
        Value channel = ctx.GetArg(0), content = ctx.GetArg(1), eventValue = ctx.GetArg(2);
        std::string name = channel.IsString() ? toStdString(channel) : "";
        if (!isValidChannelName(name)) {
            return intrinsicError("publish needs a channel name like \"orders\" or \"rooms/lobby\" (letters, digits, - "
                                  "and _, in parts joined by /)");
        }
        PushMessage m;
        m.channel = name;
        m.event = "message";
        if (!eventValue.IsNull()) {
            m.event = eventValue.IsString() ? toStdString(eventValue) : "";
            if (!isEventName(m.event)) {
                return intrinsicError("publish: the event name must be letters, digits, - and _ (like \"update\")");
            }
        }
        if (const Html* html = htmlHandleOf(content)) {
            if (html->hasScript()) {
                return intrinsicError("publish: the HTML has a <script>, which wouldn't run on the pages it goes to "
                                      "(it can't have their CSP nonce)");
            }
            m.data = html->str();
        } else if (content.IsString()) {
            m.data = escapeHtml(toStdString(content));
        } else {
            return intrinsicError("publish needs the content as frag(...), markdown(...) or a string");
        }
        // A form rendered for this visitor carries their CSRF token, which
        // mustn't go to every subscriber.
        const RenderOptions& render = run->context.render;
        for (const std::string* token : {&render.csrfToken, &render.formTimeToken}) {
            if (!token->empty() && m.data.find(*token) != std::string::npos) {
                return intrinsicError("publish: the HTML has a form with this visitor's CSRF token, which would go to "
                                      "every subscriber; publish HTML without forms (subscribers can load a form "
                                      "with hx-get)");
            }
        }
        if (m.data.size() > kMaxMessageBytes) {
            return intrinsicError("publish: the content is over " + std::to_string(kMaxMessageBytes / 1024) + " KB");
        }
        if (run->published.size() >= kMaxPerRun) {
            return intrinsicError("publish: at most " + std::to_string(kMaxPerRun) + " messages per run");
        }
        run->published.push_back(std::move(m));
        return IntrinsicResult::Null;
    });
}

}  // namespace

void addJobs() {
    addEnqueue();
    addPublish();
}

}  // namespace fourfold
