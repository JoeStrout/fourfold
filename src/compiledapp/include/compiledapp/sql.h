// SQL from the code partition (overview §3, SQL dialects): named queries
// and migrations, with Fourfold's one placeholder syntax, {{key}}.
//
// prepareSql scans a file's SQL as one database reads it (its strings,
// quoted identifiers and comments), checks the placeholders, and rewrites
// them to the database's own parameters (?1 on SQLite, $1 on Postgres).
// bindValues then turns the values a script passed (a list or a map) into
// the parameters, in order. Both are pure: no database is needed.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/datum.h"
#include "base/result.h"

namespace fourfold {

enum class SqlDialect { Sqlite, Postgres };

// "SQLite", "Postgres": for messages.
const char* sqlDialectName(SqlDialect dialect);
// "sqlite", "pg": a variant file's suffix (findUser.pg.sql).
const char* sqlDialectSuffix(SqlDialect dialect);

// An identifier slot (overview §3, Tier 2): {{sort: title, created}},
// where the value picks one name from the list, which goes into the SQL as
// written (it was checked at compile time to be a plain name, like
// `title` or `notes.title`). For a sort column, a sort direction (asc,
// desc), and the like: what a value placeholder can't do.
struct SqlSlot {
    std::string key;
    std::vector<std::string> options;
};

// One file's SQL, ready to run on one database.
struct PreparedSql {
    // The SQL with each {{key}} replaced by the database's parameter for it
    // (?1, ?2... on SQLite; $1, $2... on Postgres). With identifier slots,
    // the SQL between them: pieces[0], slot 0, pieces[1], slot 1, ... (and
    // `sql` is empty until bindQuery fills the slots in).
    std::string sql;
    std::vector<std::string> pieces;
    std::vector<SqlSlot> slots;   // in order of appearance; a key may repeat
    // The distinct keys, in order of first use: parameter n (counting from
    // 1) takes the value for keys[n - 1]. A key is a name ("title") or an
    // index ("0").
    std::vector<std::string> keys;
    // The statements in the file (a named query must have exactly one).
    int statements = 0;

    // Every key (and slot key) is an index, so the values may be a list.
    bool allIndexes() const;
    // The value keys and slot keys, for comparing variants.
    std::vector<std::string> allKeys() const;
};

struct SqlProblem {
    int line = 0;
    std::string message;
};

// Scans `source` as `dialect` reads it. Problems (a malformed placeholder,
// one inside quotes, a parameter written the database's own way, an
// unterminated string or comment) are added to `problems`; the result is
// usable only if none were.
PreparedSql prepareSql(std::string_view source, SqlDialect dialect, std::vector<SqlProblem>& problems);

// Whether `key` (between the braces, trimmed) is an index: "0", or digits
// not starting with 0.
bool isSqlIndexKey(const std::string& key);

// The parameters for `sql`, in order, from the values a script passed:
//  - a map, for any query: a name looks up its string key; an index looks
//    up the number key first, then its string ("0");
//  - a list, when every key is an index: {{n}} is item n.
// Extra keys and items are ignored. Invalid, with a message for the
// developer, if a value is missing or the values are the wrong kind.
Result<std::vector<Datum>> bindValues(const PreparedSql& sql, const Datum& values);

// The SQL to run and its parameters, for these values: bindValues, and each
// identifier slot filled with the name its value picks (matched ignoring
// case). Invalid if a slot's value isn't one of its names.
struct BoundQuery {
    std::string sql;
    std::vector<Datum> params;
};
Result<BoundQuery> bindQuery(const PreparedSql& sql, const Datum& values);

}  // namespace fourfold
