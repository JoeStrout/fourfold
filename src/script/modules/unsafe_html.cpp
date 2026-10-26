// unsafeHTML(text): the string, trusted as HTML, with no escaping. The only
// way for code to make HTML that isn't from a template or Markdown (overview
// §4); its name is meant to stand out in review.

#include "../bridge.h"
#include "modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

void addUnsafeHtml() {
    MiniScript::Intrinsic f = MiniScript::Intrinsic::Create("unsafeHTML");
    f.AddParam("text");
    f.set_Code([](Context ctx, IntrinsicResult) {
        Value text = ctx.GetArg(0);
        if (!text.IsString()) return intrinsicError("unsafeHTML needs a string");
        return IntrinsicResult(makeHtmlHandle(unsafeHtml(toStdString(text))));
    });
}

}  // namespace fourfold
