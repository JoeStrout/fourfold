// dropbox.write(name, text): appends a line to dropbox/<name>, the app's own
// append-only output (overview §1). The app can't read it back.
//
//     dropbox.write "signups.log", req.form.email

#include "../bridge.h"
#include "../host_impl.h"
#include "modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

void addDropbox() {
    Intrinsic write = Intrinsic::Create("");
    write.AddParam("name");
    write.AddParam("text", Value(""));
    write.set_Code([](Context ctx, IntrinsicResult partial) {
        Run* run = Run::of(ctx);
        if (!partial.done) {   // the append is done
            std::optional<AsyncResult> result = std::move(run->async);
            run->async.reset();
            if (result && result->error) return intrinsicError("dropbox.write: " + result->error->message);
            return IntrinsicResult(Value::Null);
        }
        if (!run || run->kind != Run::Kind::Request) {
            return intrinsicError("dropbox.write can't be used at a page's top level");
        }
        const Dropbox* dropbox = run->context.dropbox;
        if (!dropbox) return intrinsicError("dropbox isn't available here");
        Value nameValue = ctx.GetArg(0);
        auto name = nameValue.IsString() ? RelPath::parse(toStdString(nameValue))
                                         : Result<RelPath>(makeError(ErrorCode::Invalid, "it isn't a string"));
        if (!name || name.value().isRoot()) {
            return intrinsicError("dropbox.write needs a file name, like \"app.log\"" +
                                  (name ? std::string() : ": " + name.error().message));
        }
        std::string line = toStdString(ctx.GetArg(1)) + "\n";
        ScriptHost::Impl* host = run->host;
        uint64_t id = run->id;
        RelPath path = std::move(name).value();
        host->park(*run);
        host->workers->submit([host, id, dropbox, path, line] {
            AsyncResult r;
            if (Status s = dropbox->append(path, line); !s) r.error = s.error();
            host->complete(id, std::move(r));
        });
        ctx.vm.yielding = true;
        return IntrinsicResult(Value::one, false);
    });
    addModule("dropbox", {{"write", write}});
}

}  // namespace fourfold
