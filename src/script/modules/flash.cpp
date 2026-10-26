// flash(message): a one-time message for the next page (overview §2, Flash
// messages). The engine keeps it in the session; the next page with a
// {{flash}} field shows it, and then it's gone.
//
//     flash "Thanks for signing!"
//     return redirect("/guestbook")

#include "../bridge.h"
#include "../host_impl.h"
#include "modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

void addFlash() {
    MiniScript::Intrinsic f = MiniScript::Intrinsic::Create("flash");
    f.AddParam("message");
    f.set_Code([](Context ctx, IntrinsicResult) {
        Run* run = Run::of(ctx);
        if (!run || run->kind != Run::Kind::Request) {
            return intrinsicError("flash can't be used at a page's top level; call it in a handler");
        }
        if (!run->context.session) return intrinsicError("flash isn't available here (there's no session)");
        Value message = ctx.GetArg(0);
        if (!message.IsString() && !message.IsNumber()) return intrinsicError("flash needs a message string");
        run->flashSet = toStdString(message);
        return IntrinsicResult(Value::Null);
    });
}

}  // namespace fourfold
