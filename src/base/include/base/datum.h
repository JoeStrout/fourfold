// Datum: an inert value in C++ (Arch §4). Null, number, string, list, or map,
// and nothing else: no functions, no handles, nothing that could run.
//
// Datum is how app data crosses threads and goes into storage. It's immutable
// and cheap to copy (nodes are shared and reference-counted), so it can be
// handed from the script thread to a worker without copying the tree.
//
// Map keys are strings, kept sorted, so equal data always serializes to equal
// bytes. Numbers are finite: the script bridge refuses NaN and infinities
// before they get here.
//
// M2 adds what json() needs; the canonical codec (and a binary form, if
// profiling asks for one) comes with sessions and data in M5.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fourfold {

class Datum {
public:
    enum class Kind { Null, Number, String, List, Map };
    using ListItems = std::vector<Datum>;
    using MapItems = std::map<std::string, Datum>;

    Datum() = default;   // null

    static Datum number(double value);
    static Datum string(std::string value);
    static Datum list(ListItems items);
    static Datum map(MapItems items);

    Kind kind() const;
    bool isNull() const { return kind() == Kind::Null; }

    // Each precondition: kind() matches.
    double asNumber() const;
    const std::string& asString() const;
    const ListItems& asList() const;
    const MapItems& asMap() const;

    bool operator==(const Datum& other) const;
    bool operator!=(const Datum& other) const { return !(*this == other); }

private:
    struct Node;
    explicit Datum(std::shared_ptr<const Node> node) : node_(std::move(node)) {}

    std::shared_ptr<const Node> node_;   // null for Kind::Null
};

// Compact JSON. Strings are UTF-8, and the encoder escapes '<', '>', '&',
// U+2028 and U+2029 as well as what JSON requires, so the output is also safe
// inside an HTML <script> element. Numbers use the shortest form that reads
// back exactly ("3", "0.1", "1e+300").
std::string toJson(const Datum& value);

}  // namespace fourfold
