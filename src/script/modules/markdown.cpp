// markdown(text, profile="comment"): renders Markdown and returns the HTML
// (overview §4, Rendering Markdown from code). `text` may also be a list of
// lines, as file.readLines returns them. The default profile is the
// restricted one, since text that reaches this at runtime is usually a
// visitor's; "full" is an explicit, grep-able opt-in.

#include "../bridge.h"
#include "markdown/markdown.h"
#include "modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

namespace {

// The longest text markdown() accepts. Rendering runs outside the VM's
// instruction budget, so it's capped instead.
constexpr size_t kMaxMarkdownBytes = 256 * 1024;

}  // namespace

void addMarkdown() {
    MiniScript::Intrinsic f = MiniScript::Intrinsic::Create("markdown");
    f.AddParam("text");
    f.AddParam("profile", Value(MiniScript::String("comment")));
    f.set_Code([](Context ctx, IntrinsicResult) {
        Value textValue = ctx.GetArg(0);
        std::string text;
        if (textValue.IsString()) {
            text = toStdString(textValue);
        } else if (textValue.IsList()) {
            int count = textValue.ListCount();
            for (int i = 0; i < count; i++) {
                Value line = textValue.ListGet(i);
                if (!line.IsString()) return intrinsicError("markdown: line " + std::to_string(i) + " isn't a string");
                text += toStdString(line);
                text += '\n';
                if (text.size() > kMaxMarkdownBytes) break;   // renderMarkdown reports it
            }
        } else if (!textValue.IsNull()) {
            return intrinsicError("markdown needs a string of Markdown (or a list of lines)");
        }
        Value profileValue = ctx.GetArg(1);
        std::optional<MarkdownProfile> profile =
            profileValue.IsString() ? markdownProfileNamed(toStdString(profileValue)) : std::nullopt;
        if (!profile) return intrinsicError("markdown's profile must be \"comment\" (the default) or \"full\"");
        MarkdownOptions options;
        options.profile = *profile;
        options.maxBytes = kMaxMarkdownBytes;
        MarkdownResult r = renderMarkdown(text, options);
        if (!r.ok()) return intrinsicError("markdown: " + *r.error);
        return IntrinsicResult(makeHtmlHandle(std::move(r.html)));
    });
}

}  // namespace fourfold
