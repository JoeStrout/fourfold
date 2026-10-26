// file: any file in data/, read or written whole (M5.1), over DataFiles.
// The names follow MiniScript's standard file module; paths are relative to
// data/, and can't leave it.
//
//     file.readLines(path)          the text file's lines, or null if there's none
//     file.writeLines(path, lines)  replaces the file, all at once; returns the line count
//     file.loadRaw(path)            the file as RawData, or null if there's none
//     file.saveRaw(path, data)      replaces the file with a RawData's bytes
//     file.exists(path)             1 if there's a file or folder there, else 0
//     file.info(path)               {path, isDirectory, size, date}, or null
//     file.children(path="")        the names in a folder
//     file.makedir(path)            creates a folder (and any above it)
//     file.delete(path)             a file, or an empty folder; 1 if removed, else 0
//     file.name(path), file.parent(path), file.child(parent, name)
//                                   path strings, without touching the disk
//
// There are no open files, no current folder, and no appending (that's
// data.append). Writes and deletes take the file's lock, the same one `data`
// takes, so data.update never sees a file change under it. Failures come
// back as error values.

#include <ctime>

#include "../bridge.h"
#include "../host_impl.h"
#include "data_work.h"
#include "modules.h"
#include "raw_data.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

namespace {

std::optional<RelPath> pathArg(Context& ctx, const char* function, bool allowRoot, IntrinsicResult& error) {
    Value v = ctx.GetArg(0);
    if (!v.IsString()) {
        error = intrinsicError(std::string("file.") + function + " needs a path string, relative to data/");
        return std::nullopt;
    }
    auto path = DataFiles::checkPath(toStdString(v), allowRoot);
    if (!path) {
        error = intrinsicError(std::string("file.") + function + ": " + path.error().message);
        return std::nullopt;
    }
    return std::move(path).value();
}

// The finished work's result; null if there was nothing to read.
IntrinsicResult finishRead(Run& run, const char* function) {
    if (run.async && !run.async->error && !run.async->found) {
        run.async.reset();
        return IntrinsicResult::Null;
    }
    return finishWork(run, std::string("file.") + function);
}

// "yyyy-MM-dd HH:mm:ss", as MiniScript's file.info gives it, but in UTC.
std::string dateOf(int64_t ns) {
    time_t seconds = static_cast<time_t>(ns / 1000000000);
    struct tm t;
    gmtime_r(&seconds, &t);
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d", 1900 + t.tm_year, 1 + t.tm_mon, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    return buf;
}

Intrinsic makeReadLines() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("path");
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "file.readLines", data, error);
        if (!run) return error;
        if (!partial.done) return finishRead(*run, "readLines");
        auto path = pathArg(ctx, "readLines", false, error);
        if (!path) return error;
        return startWork(ctx, *run, [data, p = *path] {
            auto bytes = data->readFile(p);
            if (!bytes) return fail(bytes.error());
            AsyncResult r;
            if (!bytes.value()) {
                r.found = false;
                return r;
            }
            auto lines = DataFiles::splitLines(*bytes.value());
            if (!lines) return fail(makeError(ErrorCode::Invalid, "data/" + p.str() + " " + lines.error().message));
            Datum::ListItems items;
            for (std::string& line : lines.value()) items.push_back(Datum::string(std::move(line)));
            r.value = Datum::list(std::move(items));
            return r;
        });
    });
    return f;
}

Intrinsic makeWriteLines() {
    Intrinsic f = Intrinsic::Create("");
    f.set_AffectsState(true);
    f.AddParam("path");
    f.AddParam("lines", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "file.writeLines", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "file.writeLines");
        auto path = pathArg(ctx, "writeLines", false, error);
        if (!path) return error;
        Value linesValue = ctx.GetArg(1);
        std::vector<std::string> lines;
        if (linesValue.IsString()) {
            lines.push_back(toStdString(linesValue));
        } else if (linesValue.IsList()) {
            int count = linesValue.ListCount();
            size_t total = 0;
            for (int i = 0; i < count; i++) {
                Value line = linesValue.ListGet(i);
                if (!line.IsString() && !line.IsNumber()) {
                    return intrinsicError("file.writeLines: line " + std::to_string(i) + " isn't a string");
                }
                lines.push_back(toStdString(line));
                total += lines.back().size() + 1;
                if (total > data->maxBytes()) {
                    return intrinsicError("file.writeLines: data/" + path->str() + " would be over " +
                                          std::to_string(data->maxBytes()) + " bytes");
                }
            }
        } else {
            return intrinsicError("file.writeLines needs a list of strings (or one string)");
        }
        double count = static_cast<double>(lines.size());
        return startLockedWork(ctx, *run, data, DataFiles::lockKey(*path),
                               [data, p = *path, text = DataFiles::joinLines(lines), count] {
                                   Status s = data->writeFile(p, text);
                                   if (!s) return fail(s.error());
                                   AsyncResult r;
                                   r.value = Datum::number(count);
                                   return r;
                               });
    });
    return f;
}

Intrinsic makeLoadRaw() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("path");
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "file.loadRaw", data, error);
        if (!run) return error;
        if (!partial.done) {
            std::optional<AsyncResult> result = std::move(run->async);
            run->async.reset();
            if (!result) return intrinsicError("file.loadRaw: no result (an engine bug)");
            if (result->error) return intrinsicError("file.loadRaw: " + result->error->message);
            if (!result->found) return IntrinsicResult::Null;
            Value raw = newRawData(*run, std::move(result->bytes));
            if (raw.IsError()) return intrinsicError("file.loadRaw: " + toStdString(raw));
            return IntrinsicResult(raw);
        }
        auto path = pathArg(ctx, "loadRaw", false, error);
        if (!path) return error;
        return startWork(ctx, *run, [data, p = *path] {
            auto bytes = data->readFile(p);
            if (!bytes) return fail(bytes.error());
            AsyncResult r;
            if (bytes.value()) r.bytes = std::move(*bytes.value());
            else r.found = false;
            return r;
        });
    });
    return f;
}

Intrinsic makeSaveRaw() {
    Intrinsic f = Intrinsic::Create("");
    f.set_AffectsState(true);
    f.AddParam("path");
    f.AddParam("data", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "file.saveRaw", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "file.saveRaw");
        auto path = pathArg(ctx, "saveRaw", false, error);
        if (!path) return error;
        const std::string* bytes = rawDataBytes(ctx.GetArg(1));
        if (!bytes) return intrinsicError("file.saveRaw needs a RawData (make one with new RawData)");
        // Copied now: the buffer belongs to the script thread.
        return startLockedWork(ctx, *run, data, DataFiles::lockKey(*path), [data, p = *path, b = *bytes] {
            Status s = data->writeFile(p, b);
            return s ? AsyncResult{} : fail(s.error());
        });
    });
    return f;
}

Intrinsic makeExists() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("path");
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "file.exists", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "file.exists");
        auto path = pathArg(ctx, "exists", true, error);
        if (!path) return error;
        return startWork(ctx, *run, [data, p = *path] {
            auto i = data->info(p);
            if (!i) return fail(i.error());
            AsyncResult r;
            r.value = Datum::number(i.value() ? 1 : 0);
            return r;
        });
    });
    return f;
}

Intrinsic makeInfo() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("path", Value(MiniScript::String("")));
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "file.info", data, error);
        if (!run) return error;
        if (!partial.done) return finishRead(*run, "info");
        auto path = pathArg(ctx, "info", true, error);
        if (!path) return error;
        return startWork(ctx, *run, [data, p = *path] {
            auto i = data->info(p);
            if (!i) return fail(i.error());
            AsyncResult r;
            if (!i.value()) {
                r.found = false;
                return r;
            }
            r.value = Datum::map({
                {"path", Datum::string(p.str())},
                {"isDirectory", Datum::number(i.value()->isFolder ? 1 : 0)},
                {"size", Datum::number(static_cast<double>(i.value()->size))},
                {"date", Datum::string(dateOf(i.value()->modifiedNs))},
            });
            return r;
        });
    });
    return f;
}

Intrinsic makeChildren() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("path", Value(MiniScript::String("")));
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "file.children", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "file.children");
        auto path = pathArg(ctx, "children", true, error);
        if (!path) return error;
        return startWork(ctx, *run, [data, p = *path] {
            auto names = data->children(p);
            if (!names) return fail(names.error());
            Datum::ListItems items;
            for (const std::string& n : names.value()) items.push_back(Datum::string(n));
            AsyncResult r;
            r.value = Datum::list(std::move(items));
            return r;
        });
    });
    return f;
}

Intrinsic makeMakedir() {
    Intrinsic f = Intrinsic::Create("");
    f.set_AffectsState(true);
    f.AddParam("path");
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "file.makedir", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "file.makedir");
        auto path = pathArg(ctx, "makedir", false, error);
        if (!path) return error;
        return startWork(ctx, *run, [data, p = *path] {
            Status s = data->makeFolder(p);
            return s ? AsyncResult{} : fail(s.error());
        });
    });
    return f;
}

Intrinsic makeDelete() {
    Intrinsic f = Intrinsic::Create("");
    f.set_AffectsState(true);
    f.AddParam("path");
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        DataFiles* data = nullptr;
        Run* run = dataRun(ctx, "file.delete", data, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, "file.delete");
        auto path = pathArg(ctx, "delete", false, error);
        if (!path) return error;
        return startLockedWork(ctx, *run, data, DataFiles::lockKey(*path), [data, p = *path] {
            auto removed = data->removeFile(p);
            if (!removed) return fail(removed.error());
            AsyncResult r;
            r.value = Datum::number(removed.value() ? 1 : 0);
            return r;
        });
    });
    return f;
}

// The path helpers: plain string work, as in MiniScript's file module.

std::string withoutTrailingSlash(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

Intrinsic makeName() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("path", Value(MiniScript::String("")));
    f.set_Code([](Context ctx, IntrinsicResult) {
        std::string path = withoutTrailingSlash(toStdString(ctx.GetArg(0)));
        size_t slash = path.rfind('/');
        return IntrinsicResult(toValue(slash == std::string::npos ? path : path.substr(slash + 1)));
    });
    return f;
}

Intrinsic makeParent() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("path", Value(MiniScript::String("")));
    f.set_Code([](Context ctx, IntrinsicResult) {
        std::string path = withoutTrailingSlash(toStdString(ctx.GetArg(0)));
        size_t slash = path.rfind('/');
        return IntrinsicResult(toValue(slash == std::string::npos ? std::string() : path.substr(0, slash)));
    });
    return f;
}

Intrinsic makeChild() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("parentPath", Value(MiniScript::String("")));
    f.AddParam("childName", Value(MiniScript::String("")));
    f.set_Code([](Context ctx, IntrinsicResult) {
        std::string parent = withoutTrailingSlash(toStdString(ctx.GetArg(0)));
        std::string name = toStdString(ctx.GetArg(1));
        return IntrinsicResult(toValue(parent.empty() ? name : parent + "/" + name));
    });
    return f;
}

}  // namespace

void addFile() {
    addModule("file", {
                          {"readLines", makeReadLines()},
                          {"writeLines", makeWriteLines()},
                          {"loadRaw", makeLoadRaw()},
                          {"saveRaw", makeSaveRaw()},
                          {"exists", makeExists()},
                          {"info", makeInfo()},
                          {"children", makeChildren()},
                          {"makedir", makeMakedir()},
                          {"delete", makeDelete()},
                          {"name", makeName()},
                          {"parent", makeParent()},
                          {"child", makeChild()},
                      });
}

}  // namespace fourfold
