#include "bridge.h"

#include <cmath>
#include <unordered_set>

#include "host_impl.h"
#include "value_map.h"

namespace fourfold {

using MiniScript::String;
using MiniScript::Value;

namespace {

constexpr int kMaxDepth = 64;

// Live OutcomeHandles. Every handle's pointer is checked against this before
// it's trusted, so a handle made by some other intrinsic is never misread.
// Script thread only.
std::unordered_set<const OutcomeHandle*>& liveHandles() {
    static std::unordered_set<const OutcomeHandle*> live;
    return live;
}

void finalizeOutcomeHandle(void* p) {
    auto* h = static_cast<OutcomeHandle*>(p);
    liveHandles().erase(h);
    delete h;
}

std::optional<Datum> toDatumAt(Value v, std::string& error, int depth) {
    if (depth > kMaxDepth) {
        error = "nested more than " + std::to_string(kMaxDepth) + " deep (or contains itself)";
        return std::nullopt;
    }
    if (v.IsNull()) return Datum();
    if (v.IsNumber()) {
        double n = v.DoubleValue();
        if (!std::isfinite(n)) {
            error = "contains a number that isn't finite";
            return std::nullopt;
        }
        return Datum::number(n);
    }
    if (v.IsString()) return Datum::string(toStdString(v));
    if (v.IsList()) {
        Datum::ListItems items;
        int count = v.ListCount();
        items.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; i++) {
            auto item = toDatumAt(v.ListGet(i), error, depth + 1);
            if (!item) return std::nullopt;
            items.push_back(std::move(*item));
        }
        return Datum::list(std::move(items));
    }
    if (v.IsMap()) {
        Datum::MapItems items;
        MiniScript::MapIterator it = v.Iterator();
        Value key, item;
        while (MiniScript::map_iterator_next(&it, &key, &item)) {
            if (!key.IsString()) {
                error = "contains a map with a key that isn't a string (" + toStdString(key) + ")";
                return std::nullopt;
            }
            std::string k = toStdString(key);
            if (k == "__isa") continue;
            auto d = toDatumAt(item, error, depth + 1);
            if (!d) return std::nullopt;
            items.emplace(std::move(k), std::move(*d));
        }
        return Datum::map(std::move(items));
    }
    if (v.IsFuncRef()) {
        error = "contains a function";
    } else if (v.IsError()) {
        error = "contains an error value";
    } else {
        error = "contains a handle (e.g. rendered HTML or a file)";
    }
    return std::nullopt;
}

Value pairsToMap(const RequestFacts::Pairs& pairs) {
    Value map = Value::make_map(static_cast<int>(pairs.size()));
    for (const auto& [name, value] : pairs) {
        Value key = toValue(name);
        if (!map.HasKey(key)) map.MapSet(key, toValue(value));
    }
    return map;
}

}  // namespace

std::string toStdString(Value v) {
    String s = v.ToString();
    return std::string(s.c_str(), static_cast<size_t>(s.LengthB()));
}

Value toValue(const std::string& s) {
    if (s.find('\0') == std::string::npos) return Value(String(s.data(), s.size()));
    // MiniScript strings end at a NUL byte; keep the rest, marking the spot.
    std::string copy;
    for (char c : s) {
        if (c == '\0') copy += "\xEF\xBF\xBD";   // U+FFFD
        else copy += c;
    }
    return Value(String(copy.data(), copy.size()));
}

std::optional<Datum> toDatum(Value v, std::string& error) {
    return toDatumAt(v, error, 0);
}

Value makeOutcomeHandle(OutcomeHandle outcome) {
    auto* h = new OutcomeHandle(std::move(outcome));
    liveHandles().insert(h);
    return Value::NewHandle(h, finalizeOutcomeHandle);
}

const OutcomeHandle* outcomeHandleOf(Value v) {
    if (!v.IsHandle()) return nullptr;
    auto* h = static_cast<const OutcomeHandle*>(v.HandlePtr());
    return liveHandles().count(h) ? h : nullptr;
}

Value makeRequestValue(const RequestFacts& facts) {
    Value req = Value::make_map(8);
    req.MapSet("method", toValue(facts.method));
    req.MapSet("path", toValue(facts.path));
    req.MapSet("query", pairsToMap(facts.query));
    req.MapSet("headers", pairsToMap(facts.headers));
    req.MapSet("form", pairsToMap(facts.form));
    req.Freeze();   // deep: handlers can read req, never change it
    return req;
}

ScriptError scriptErrorFrom(Value error, const std::string& fallbackFile) {
    ScriptError e;
    e.message = error.IsError() ? toStdString(error.Message()) : toStdString(error);
    e.file = fallbackFile;
    std::string location = error.IsError() ? toStdString(Value(MiniScript::ErrorTypes::ErrorLocation(error))) : "";
    size_t at = location.rfind(" line ");
    if (at != std::string::npos) {
        e.file = location.substr(0, at);
        e.line = std::atoi(location.c_str() + at + 6);
    }
    return e;
}

namespace {

HandlerOutcome errorOutcome(const Run& run, std::string message) {
    HandlerOutcome o;
    o.kind = HandlerOutcome::Kind::Error;
    o.error.message = std::move(message);
    o.error.file = run.fileName;
    return o;
}

const char* typeName(Value v) {
    if (v.IsNumber()) return "a number";
    if (v.IsList()) return "a list";
    if (v.IsMap()) return "a map";
    if (v.IsFuncRef()) return "a function";
    if (v.IsHandle()) return "a handle";
    return "a value of another kind";
}

}  // namespace

HandlerOutcome outcomeFromResult(Run& run) {
    Value result = run.interp.GetGlobalValue("_ff_result");
    HandlerOutcome o;
    std::string who = run.handler.label();

    if (result.IsNull()) {
        o.kind = HandlerOutcome::Kind::NoContent;
        return o;
    }
    if (result.IsError()) {
        o.kind = HandlerOutcome::Kind::Error;
        o.error = scriptErrorFrom(result, run.fileName);
        return o;
    }
    if (result.IsString()) {
        o.kind = HandlerOutcome::Kind::Text;
        o.body = toStdString(result);
        return o;
    }
    if (const OutcomeHandle* h = outcomeHandleOf(result)) {
        switch (h->kind) {
            case OutcomeHandle::Kind::Redirect:
                o.kind = HandlerOutcome::Kind::Redirect;
                o.location = h->location;
                break;
            case OutcomeHandle::Kind::Json:
                o.kind = HandlerOutcome::Kind::Json;
                o.body = h->body;
                break;
            case OutcomeHandle::Kind::Status:
                o.kind = HandlerOutcome::Kind::Status;
                o.status = h->status;
                o.body = h->body;
                o.contentType = h->contentType;
                break;
        }
        return o;
    }
    if (result.IsMap()) {
        if (run.handler.kind != HandlerRef::Kind::Render) {
            return errorOutcome(run, who + " returned a map; only render can (use json(...) to send JSON)");
        }
        o.kind = HandlerOutcome::Kind::Render;
        MiniScript::MapIterator it = result.Iterator();
        Value key, value;
        while (MiniScript::map_iterator_next(&it, &key, &value)) {
            if (!key.IsString()) {
                return errorOutcome(run, "render returned a map with a key that isn't a string (" +
                                             toStdString(key) + ")");
            }
            std::string name = toStdString(key);
            if (value.IsNull()) {
                o.fields[name] = std::nullopt;
            } else if (value.IsString() || value.IsNumber()) {
                o.fields[name] = toStdString(value);   // numbers in MiniScript's own format
            } else {
                return errorOutcome(run, "render's field '" + name + "' is " + typeName(value) +
                                             "; fields take strings, numbers, or null");
            }
        }
        return o;
    }
    return errorOutcome(run, who + " returned " + typeName(result) +
                                 "; return a string, redirect(...), json(...), status(...), or null" +
                                 (run.handler.kind == HandlerRef::Kind::Render ? ", or a map of fields" : ""));
}

}  // namespace fourfold
