#include "bridge.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "host_impl.h"
#include "template/template.h"
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

// The same, for Html handles.
std::unordered_set<const Html*>& liveHtml() {
    static std::unordered_set<const Html*> live;
    return live;
}

void finalizeHtmlHandle(void* p) {
    auto* h = static_cast<Html*>(p);
    liveHtml().erase(h);
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

Value makeHtmlHandle(Html html) {
    auto* h = new Html(std::move(html));
    liveHtml().insert(h);
    return Value::NewHandle(h, finalizeHtmlHandle);
}

const Html* htmlHandleOf(Value v) {
    if (!v.IsHandle()) return nullptr;
    auto* h = static_cast<const Html*>(v.HandlePtr());
    return liveHtml().count(h) ? h : nullptr;
}

namespace {

// A value as a field value (not yet in a <script>).
FieldValue fieldFrom(Value v, bool forScript) {
    if (v.IsNull()) return FieldValue::null();
    if (const Html* h = htmlHandleOf(v)) return FieldValue::markup(*h);
    if (forScript) {
        std::string why;
        std::optional<Datum> d = toDatum(v, why);
        if (!d) return FieldValue::error("can't go in a <script>: it " + why);
        return FieldValue::datum(std::move(*d));
    }
    if (v.IsString()) return FieldValue::string(toStdString(v));
    if (v.IsNumber()) return FieldValue::number(toStdString(v));   // MiniScript's own format
    if (v.IsMap()) return FieldValue::composite("a map");
    if (v.IsList()) return FieldValue::composite("a list");
    if (v.IsFuncRef()) return FieldValue::error("is a function (did you mean to call it?)");
    if (v.IsError()) return FieldValue::error("is an error value");
    return FieldValue::error("is a handle that isn't rendered HTML");
}

}  // namespace

FieldValue ValueFields::lookup(const std::vector<std::string>& path, bool forScript) const {
    if (path.empty() || !map_.IsMap()) return FieldValue::missing();
    Value v;
    if (!map_.TryGet(toValue(path[0]), &v)) return FieldValue::missing();
    for (size_t i = 1; i < path.size(); i++) {
        if (v.IsNull()) return FieldValue::null();
        if (!v.IsMap()) {
            return FieldValue::error("can't be looked up: " + path[i - 1] + " isn't a map, so it has no ." + path[i]);
        }
        Value next;
        if (!v.TryGet(toValue(path[i]), &next)) return FieldValue::null();
        v = next;
    }
    return fieldFrom(v, forScript);
}

std::vector<std::string> ValueFields::names() const {
    std::vector<std::string> result;
    if (!map_.IsMap()) return result;
    MiniScript::MapIterator it = map_.Iterator();
    Value key, item;
    while (MiniScript::map_iterator_next(&it, &key, &item)) {
        if (key.IsString()) result.push_back(toStdString(key));
    }
    return result;
}

Value makeRequestValue(const RequestFacts& facts) {
    Value req = Value::make_map(8);
    req.MapSet("method", toValue(facts.method));
    req.MapSet("path", toValue(facts.path));
    req.MapSet("query", pairsToMap(facts.query));
    req.MapSet("headers", pairsToMap(facts.headers));
    req.MapSet("form", pairsToMap(facts.form));
    // HTMX sends "HX-Request: true" with every request it makes.
    bool htmx = false;
    for (const auto& [name, value] : facts.headers) htmx = htmx || (name == "hx-request" && value == "true");
    req.MapSet("htmx", htmx ? Value::one : Value::zero);
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

void addWarnings(Run& run, const std::vector<RenderWarning>& more) {
    for (const RenderWarning& w : more) {
        if (std::find(run.warnings.begin(), run.warnings.end(), w) == run.warnings.end()) run.warnings.push_back(w);
    }
}

namespace {

// The page template a render fills: pages/x.html for pages/x.ms.
RelPath pairedTemplate(const RelPath& source) {
    std::string s = source.str();
    s.resize(s.size() - source.extension().size());
    return RelPath::parse(s + ".html").value();
}

HandlerOutcome renderPage(Run& run, Value fields) {
    RelPath page = pairedTemplate(run.handler.source);
    const CompiledTemplate* t = run.app ? run.app->templates.find(page) : nullptr;
    if (!t) {
        return errorOutcome(run, "render returned a map of fields, but there's no code/" + page.str() +
                                     " template to fill; return a string or json(...) instead");
    }
    RenderResult r = render(*t, ValueFields(fields), run.render);
    addWarnings(run, r.warnings);
    HandlerOutcome o;
    if (!r.ok()) {
        o.kind = HandlerOutcome::Kind::Error;
        o.error.message = r.error->message;
        o.error.file = "code/" + r.error->file;
        o.error.line = r.error->line;
        return o;
    }
    o.kind = HandlerOutcome::Kind::Render;
    o.body = r.html.str();
    return o;
}

HandlerOutcome outcomeFrom(Run& run) {
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
    if (const Html* html = htmlHandleOf(result)) {
        o.kind = HandlerOutcome::Kind::Fragment;
        o.body = html->str();
        o.hasScript = html->hasScript();
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
                o.hasScript = h->hasScript;
                break;
        }
        return o;
    }
    if (result.IsMap()) {
        if (run.handler.kind != HandlerRef::Kind::Render) {
            return errorOutcome(run, who + " returned a map; only render can (use json(...) to send JSON, or "
                                           "frag(...) to send HTML)");
        }
        return renderPage(run, result);
    }
    return errorOutcome(run, who + " returned " + typeName(result) +
                                 "; return a string, frag(...), redirect(...), json(...), status(...), or null" +
                                 (run.handler.kind == HandlerRef::Kind::Render ? ", or a map of fields" : ""));
}

}  // namespace

HandlerOutcome outcomeFromResult(Run& run) {
    HandlerOutcome o = outcomeFrom(run);
    o.warnings = std::move(run.warnings);
    return o;
}

}  // namespace fourfold
