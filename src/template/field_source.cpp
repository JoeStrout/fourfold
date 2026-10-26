#include "template/field_source.h"

#include <cstdlib>

namespace fourfold {

FieldValue FieldValue::markup(fourfold::Html html) {
    FieldValue v = make(Kind::Html);
    v.html = std::move(html);
    return v;
}

FieldValue FieldValue::datum(Datum data) {
    FieldValue v = make(Kind::Data);
    v.data = std::move(data);
    return v;
}

FieldValue fieldFromDatum(const Datum& value, bool forScript) {
    if (value.isNull()) return FieldValue::null();
    if (forScript) return FieldValue::datum(value);
    switch (value.kind()) {
        case Datum::Kind::Number: return FieldValue::number(toJson(value));
        case Datum::Kind::String: return FieldValue::string(value.asString());
        case Datum::Kind::List: return FieldValue::composite("a list");
        case Datum::Kind::Map: return FieldValue::composite("a map");
        case Datum::Kind::Null: break;
    }
    return FieldValue::null();
}

MapFields& MapFields::set(const std::string& name, FieldValue value) {
    values_[name] = std::move(value);
    return *this;
}

FieldValue MapFields::lookup(const std::vector<std::string>& path, bool forScript) const {
    if (path.empty()) return FieldValue::missing();
    auto it = values_.find(path[0]);
    if (it == values_.end()) return FieldValue::missing();
    const FieldValue& top = it->second;
    if (path.size() == 1) {
        if (top.kind == FieldValue::Kind::Data) return fieldFromDatum(top.data, forScript);
        if (forScript && top.kind == FieldValue::Kind::String) return FieldValue::datum(Datum::string(top.text));
        if (forScript && top.kind == FieldValue::Kind::Number) {
            return FieldValue::datum(Datum::number(std::strtod(top.text.c_str(), nullptr)));
        }
        return top;
    }
    if (top.kind == FieldValue::Kind::Null) return FieldValue::null();
    if (top.kind != FieldValue::Kind::Data || top.data.kind() != Datum::Kind::Map) {
        return FieldValue::error(path[0] + " isn't a map, so it has no ." + path[1]);
    }
    const Datum* d = &top.data;
    for (size_t i = 1; i < path.size(); i++) {
        if (d->isNull()) return FieldValue::null();
        if (d->kind() != Datum::Kind::Map) {
            return FieldValue::error(path[i - 1] + " isn't a map, so it has no ." + path[i]);
        }
        auto found = d->asMap().find(path[i]);
        if (found == d->asMap().end()) return FieldValue::null();
        d = &found->second;
    }
    return fieldFromDatum(*d, forScript);
}

std::vector<std::string> MapFields::names() const {
    std::vector<std::string> result;
    for (const auto& entry : values_) result.push_back(entry.first);
    return result;
}

}  // namespace fourfold
