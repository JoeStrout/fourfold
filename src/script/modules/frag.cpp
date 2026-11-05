// frag(name, value=null): renders code/frags/<name>.html (or .md) and returns
// the HTML (overview §4, Pages vs. fragments). `value` fills its fields: a
// map, or a list of maps to render it once per item. An .md fragment's
// result is Markdown too, so it can go in a Markdown page; when the run's
// output is Markdown (/x.md), its Markdown form is rendered with it.

#include "../bridge.h"
#include "../host_impl.h"
#include "modules.h"
#include "template/template.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::IntrinsicResult;
using MiniScript::String;
using MiniScript::Value;

IntrinsicResult intrinsicError(const std::string& message) {
    return IntrinsicResult(MiniScript::ErrorTypes::RuntimeError(String(message.data(), message.size())));
}

namespace {

// Renders one item into `out`; returns false with `error` set if it failed.
bool renderOne(Run& run, const CompiledTemplate& t, Value fields, Html& out, std::string& error) {
    RenderResult r = renderFragment(t, ValueFields(fields), run.context.render);
    addWarnings(run, r.warnings);
    if (!r.ok()) {
        error = "code/" + r.error->file + (r.error->line ? " line " + std::to_string(r.error->line) : "") + ": " +
                r.error->message;
        return false;
    }
    out = std::move(r.html);
    return true;
}

}  // namespace

void addFrag() {
    MiniScript::Intrinsic f = MiniScript::Intrinsic::Create("frag");
    f.AddParam("name");
    f.AddParam("value", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult) {
        Run* run = Run::of(ctx);
        if (!run || !run->app) {
            return intrinsicError("frag can't be used at a page's top level; call it in render or a handler");
        }
        Value nameValue = ctx.GetArg(0);
        if (!nameValue.IsString()) return intrinsicError("frag needs a fragment's name, like frag(\"entry\")");
        std::string name = toStdString(nameValue);
        const CompiledTemplate* t = run->app->templates.fragment(name);
        if (!t) return intrinsicError("frag: there's no code/frags/" + name + ".html (or .md)");

        Value value = ctx.GetArg(1);
        // An empty list renders nothing, of the fragment's kind.
        Html html = emptyRendering(*t, run->context.render);
        bool first = true;
        auto add = [&](Value item, std::string& error) {
            Html one;
            if (!renderOne(*run, *t, item, one, error)) return false;
            if (first) {
                html = std::move(one);
                first = false;
            } else {
                html += one;
            }
            return true;
        };
        std::string error;
        if (value.IsList()) {
            int count = value.ListCount();
            for (int i = 0; i < count; i++) {
                Value item = value.ListGet(i);
                if (!item.IsMap() && !item.IsNull()) {
                    return intrinsicError("frag(\"" + name + "\", list): item " + std::to_string(i) +
                                          " isn't a map; each item fills the fragment's fields once");
                }
                if (!add(item, error)) return intrinsicError(error);
            }
        } else if (value.IsMap() || value.IsNull()) {
            if (!add(value, error)) return intrinsicError(error);
        } else {
            return intrinsicError("frag(\"" + name + "\", value): the value must be a map of fields, or a list "
                                  "of maps");
        }
        return IntrinsicResult(makeHtmlHandle(std::move(html)));
    });
}

}  // namespace fourfold
