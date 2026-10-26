// FieldSource: where a template's field values come from (Arch §5 template).
// The script component implements it over a MiniScript map; pages with no
// code use EmptyFields; C++ callers and tests use MapFields. So the template
// component never sees a MiniScript value.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "base/datum.h"
#include "template/html.h"

namespace fourfold {

struct FieldValue {
    enum class Kind {
        Missing,     // the top-level name isn't in the source at all
        Null,        // null, or a dotted path through a null or missing key
        Number,      // `text` holds it, formatted the way the source's language does
        String,      // `text`
        Html,        // `html`: inserted as is in text, an error anywhere else
        Data,        // `data`: a value for a <script> (only when asked for one)
        Composite,   // a map or list outside a <script>; `text` names it ("a map")
        Error,       // the lookup failed; `text` says why
    };

    Kind kind = Kind::Missing;
    std::string text;
    fourfold::Html html;
    Datum data;

    static FieldValue missing() { return {}; }
    static FieldValue null() { return make(Kind::Null); }
    static FieldValue number(std::string text) { return make(Kind::Number, std::move(text)); }
    static FieldValue string(std::string text) { return make(Kind::String, std::move(text)); }
    static FieldValue markup(fourfold::Html html);
    static FieldValue datum(Datum data);
    static FieldValue composite(std::string what) { return make(Kind::Composite, std::move(what)); }
    static FieldValue error(std::string why) { return make(Kind::Error, std::move(why)); }

private:
    static FieldValue make(Kind kind, std::string text = "") {
        FieldValue v;
        v.kind = kind;
        v.text = std::move(text);
        return v;
    }
};

class FieldSource {
public:
    virtual ~FieldSource() = default;

    // The value at a dotted path ({"user", "name"} for {{user.name}}).
    // `forScript`: the field is inside a <script>, so maps, lists, strings
    // and numbers all come back as Data (to be written as JSON).
    virtual FieldValue lookup(const std::vector<std::string>& path, bool forScript) const = 0;

    // The top-level names, for the "unused field" warning.
    virtual std::vector<std::string> names() const = 0;
};

class EmptyFields : public FieldSource {
public:
    FieldValue lookup(const std::vector<std::string>&, bool) const override { return FieldValue::missing(); }
    std::vector<std::string> names() const override { return {}; }
};

// Literal values by top-level name. A dotted path descends into Data maps.
class MapFields : public FieldSource {
public:
    MapFields() = default;
    MapFields(std::map<std::string, FieldValue> values) : values_(std::move(values)) {}

    MapFields& set(const std::string& name, FieldValue value);

    FieldValue lookup(const std::vector<std::string>& path, bool forScript) const override;
    std::vector<std::string> names() const override;

private:
    std::map<std::string, FieldValue> values_;
};

// A Datum as a field value: strings and numbers as themselves (or as Data in
// a script), null as Null, maps and lists as Composite outside a script.
FieldValue fieldFromDatum(const Datum& value, bool forScript);

}  // namespace fourfold
