// What the SQLite and Postgres databases share: turning column values into
// Datums, and holding results to the limits.
#pragma once

#include <string>
#include <vector>

#include "appdata/database.h"

namespace fourfold {

// A whole number that a double holds exactly (bound as an integer).
bool isWholeNumber(double x);

// A text column's value: invalid UTF-8 is repaired; a NUL is an error.
Result<Datum> textValue(const std::string& text, const std::string& column);

Error binaryColumn(const std::string& column);
Error timedOut(const DbLimits& limits);

// A query's rows, within the limits.
class RowCollector {
public:
    explicit RowCollector(const DbLimits& limits) : limits_(limits) {}
    Status add(Datum row);
    Datum result() { return Datum::list(std::move(rows_)); }

private:
    const DbLimits& limits_;
    Datum::ListItems rows_;
    size_t bytes_ = 0;
};

}  // namespace fourfold
