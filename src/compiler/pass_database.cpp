// The database pass (overview §3, SQL dialects; Schema changes and
// migrations): named queries in code/queries/, migrations in
// code/migrations/, and code/seed.ms.
//
// A query or .sql migration may come as a plain .sql file, used on every
// database, and variants (.sqlite.sql, .pg.sql) that replace it on one. The
// pass picks each one's file for the app's database, prepares its SQL, and
// checks that every variant takes the same placeholder keys. Migrations'
// MiniScript goes to the scripts pass, with the seed.

#include <algorithm>
#include <cctype>
#include <set>

#include "base/crypto.h"
#include "passes.h"

namespace fourfold {

namespace {

bool isName(std::string_view s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// The files of one query (or .sql migration): a plain one, and variants.
struct SqlGroup {
    std::optional<RelPath> plain, sqlite, pg;

    std::optional<RelPath>& variant(SqlDialect d) { return d == SqlDialect::Sqlite ? sqlite : pg; }
};

// "x.sql" → name "x", plain; "x.pg.sql" → "x", Postgres. nullopt if `file`
// doesn't end in .sql.
struct SqlFileName {
    std::string stem;
    std::optional<SqlDialect> dialect;
};

std::optional<SqlFileName> sqlFileName(std::string_view file) {
    if (!endsWith(file, ".sql")) return std::nullopt;
    std::string_view stem = file.substr(0, file.size() - 4);
    if (endsWith(stem, ".sqlite")) return SqlFileName{std::string(stem.substr(0, stem.size() - 7)), SqlDialect::Sqlite};
    if (endsWith(stem, ".pg")) return SqlFileName{std::string(stem.substr(0, stem.size() - 3)), SqlDialect::Postgres};
    return SqlFileName{std::string(stem), std::nullopt};
}

void report(Diagnostics& diagnostics, const std::string& file, const std::vector<SqlProblem>& problems) {
    for (const SqlProblem& p : problems) diagnostics.error(file, p.message, p.line);
}

std::string keyList(const std::vector<std::string>& keys) {
    std::vector<std::string> sorted(keys);
    std::sort(sorted.begin(), sorted.end());
    if (sorted.empty()) return "no placeholders";
    std::string s;
    for (const std::string& k : sorted) s += (s.empty() ? "{{" : ", {{") + k + "}}";
    return s;
}

struct Chosen {
    RelPath path;
    std::string contents;
    PreparedSql sql;
};

// Picks the group's file for `dialect`, reads and prepares it, and checks
// the other files' keys against it. nullopt (with errors reported) if
// there's no usable file. `what` is how messages name it ("query
// findUser").
std::optional<Chosen> chooseSql(const SqlGroup& group, const std::string& what, SqlDialect dialect,
                                const CodeTree& code, Diagnostics& diagnostics) {
    SqlGroup g = group;
    std::optional<RelPath> path = g.variant(dialect) ? g.variant(dialect) : g.plain;
    if (!path) {
        const RelPath& other = g.sqlite ? *g.sqlite : *g.pg;
        diagnostics.error(codeFileName(other), what + " has no file for " + sqlDialectName(dialect) +
                                                   ", the app's database: add a plain .sql file, or a ." +
                                                   sqlDialectSuffix(dialect) + ".sql variant");
        return std::nullopt;
    }
    auto file = code.read(*path);
    if (!file) {
        diagnostics.error(codeFileName(*path), file.error().message);
        return std::nullopt;
    }
    std::vector<SqlProblem> problems;
    Chosen chosen{*path, file.value().contents(), prepareSql(file.value().contents(), dialect, problems)};
    if (!problems.empty()) {
        report(diagnostics, codeFileName(*path), problems);
        return std::nullopt;
    }

    // Every variant takes the same values (overview §3, Compile-time rules),
    // checked as far as each file scans cleanly as its own database reads it.
    std::vector<std::string> chosenKeys = chosen.sql.allKeys();
    std::set<std::string> keys(chosenKeys.begin(), chosenKeys.end());
    for (const auto& [other, otherDialect] : {std::make_pair(g.plain, dialect),
                                              std::make_pair(g.sqlite, SqlDialect::Sqlite),
                                              std::make_pair(g.pg, SqlDialect::Postgres)}) {
        if (!other || *other == *path) continue;
        auto otherFile = code.read(*other);
        if (!otherFile) continue;
        std::vector<SqlProblem> ignored;
        PreparedSql otherSql = prepareSql(otherFile.value().contents(), otherDialect, ignored);
        if (!ignored.empty()) continue;
        std::vector<std::string> otherKeys = otherSql.allKeys();
        if (std::set<std::string>(otherKeys.begin(), otherKeys.end()) != keys) {
            diagnostics.error(codeFileName(*other),
                              "uses " + keyList(otherKeys) + ", but " + std::string(path->name()) + " uses " +
                                  keyList(chosenKeys) + ": every variant of a query must use the same keys");
        }
    }
    return chosen;
}

// Adds `path` (a .sql file named `name` in `groups`), or reports why not.
bool addSqlFile(std::map<std::string, SqlGroup>& groups, const RelPath& path, const std::string& name,
                std::optional<SqlDialect> dialect) {
    SqlGroup& g = groups[name];
    std::optional<RelPath>& slot = dialect ? g.variant(*dialect) : g.plain;
    if (slot) return false;
    slot = path;
    return true;
}

std::optional<std::shared_ptr<const NamedQuery>> makeQuery(const std::string& name, const SqlGroup& group,
                                                           SqlDialect dialect, const CodeTree& code,
                                                           Diagnostics& diagnostics) {
    auto chosen = chooseSql(group, "query " + name, dialect, code, diagnostics);
    if (!chosen) return std::nullopt;
    if (chosen->sql.statements != 1) {
        diagnostics.error(codeFileName(chosen->path),
                          chosen->sql.statements == 0
                              ? std::string("is empty: a query file holds one SQL statement")
                              : "holds " + std::to_string(chosen->sql.statements) +
                                    " statements: a query file holds just one");
        return std::nullopt;
    }
    auto q = std::make_shared<NamedQuery>();
    q->name = name;
    q->source = chosen->path;
    q->sql = std::move(chosen->sql);
    return std::shared_ptr<const NamedQuery>(std::move(q));
}

// A migration's file or folder name: "003_backfill_entry_time" → 3.
std::optional<int> migrationNumber(std::string_view name) {
    size_t digits = 0;
    while (digits < name.size() && std::isdigit(static_cast<unsigned char>(name[digits]))) digits++;
    if (digits == 0 || digits > 9) return std::nullopt;
    if (digits < name.size()) {
        if (name[digits] != '_' || digits + 1 == name.size()) return std::nullopt;
        for (char c : name.substr(digits + 1)) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return std::nullopt;
        }
    }
    return std::stoi(std::string(name.substr(0, digits)));
}

std::string checksumOf(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string all;
    for (const auto& [name, contents] : files) {
        all += name;
        all += '\0';
        all += std::to_string(contents.size());
        all += '\0';
        all += contents;
    }
    return toHex(sha256(all));
}

// One migration as found: its files.
struct Found {
    std::string name;
    SqlGroup sql;                        // .sql files directly in migrations/
    std::optional<RelPath> script;       // a single .ms
    bool folder = false;
    std::vector<RelPath> folderFiles;    // inside the folder
};

const char* const kMigrationNames =
    "isn't a migration: migrations are numbered .sql or .ms files, or folders, like 001_create_entries.sql";

}  // namespace

DatabaseSources databasePass(const CodeListing& listing, const CodeTree& code, SqlDialect dialect, CompiledApp& app,
                             Diagnostics& diagnostics) {
    app.dialect = dialect;
    DatabaseSources out;
    const RelPath queries = RelPath::parse("queries").value();
    const RelPath migrations = RelPath::parse("migrations").value();

    // --- code/queries/ ---
    std::map<std::string, SqlGroup> queryGroups;
    for (const CodeEntry& entry : listing.files) {
        if (!entry.path.isWithin(queries)) continue;
        std::string file = codeFileName(entry.path);
        auto parsed = sqlFileName(entry.path.name());
        if (!parsed) {
            diagnostics.warning(file, "only .sql files belong in code/queries/; ignored");
            continue;
        }
        // The name: the path below queries/, without the extension.
        std::string rel = entry.path.str().substr(queries.str().size() + 1);
        std::string name = rel.substr(0, rel.size() - std::string(entry.path.name()).size()) + parsed->stem;
        bool ok = true;
        auto parsedName = RelPath::parse(name);   // segments() views into it
        if (parsedName) {
            for (std::string_view segment : parsedName.value().segments()) ok = ok && isName(segment);
        }
        if (!ok || name.empty()) {
            diagnostics.error(file, "a query's name (and each folder's) must be letters, digits and _, not starting "
                                    "with a digit; variants end in .sqlite.sql or .pg.sql");
            continue;
        }
        if (!addSqlFile(queryGroups, entry.path, name, parsed->dialect)) {
            diagnostics.error(file, "query " + name + " has two files for the same database");
        }
    }
    for (const auto& [name, group] : queryGroups) {
        if (auto q = makeQuery(name, group, dialect, code, diagnostics)) app.queries.emplace(name, *q);
    }

    // --- code/migrations/ ---
    std::map<std::string, Found> found;   // by name, which sorts by number for equal widths
    for (const CodeEntry& entry : listing.files) {
        if (!entry.path.isWithin(migrations)) continue;
        std::string file = codeFileName(entry.path);
        std::vector<std::string_view> segments = entry.path.segments();
        if (segments.size() == 2) {
            std::string_view base = segments[1];
            if (auto parsed = sqlFileName(base)) {
                if (!migrationNumber(parsed->stem)) {
                    diagnostics.error(file, kMigrationNames);
                    continue;
                }
                Found& f = found[parsed->stem];
                f.name = parsed->stem;
                std::optional<RelPath>& slot = parsed->dialect ? f.sql.variant(*parsed->dialect) : f.sql.plain;
                slot = entry.path;
            } else if (endsWith(base, ".ms") && migrationNumber(base.substr(0, base.size() - 3))) {
                std::string stem(base.substr(0, base.size() - 3));
                found[stem].name = stem;
                found[stem].script = entry.path;
            } else {
                diagnostics.error(file, kMigrationNames);
            }
            continue;
        }
        std::string folder(segments[1]);
        if (!migrationNumber(folder)) {
            diagnostics.error(file, std::string("is in code/migrations/") + folder + "/, which " + kMigrationNames);
            continue;
        }
        Found& f = found[folder];
        f.name = folder;
        f.folder = true;
        if (segments.size() > 3) {
            diagnostics.error(file, "a migration's folder can't have folders inside it");
            continue;
        }
        f.folderFiles.push_back(entry.path);
    }

    std::map<int, std::string> numbers;   // number → name, for duplicates
    for (auto& [name, f] : found) {
        int number = *migrationNumber(name);
        std::string label = "code/migrations/" + name;
        int forms = (f.folder ? 1 : 0) + (f.script ? 1 : 0) + (f.sql.plain || f.sql.sqlite || f.sql.pg ? 1 : 0);
        if (forms > 1) {
            diagnostics.error(label, "migration " + name + " is more than one of a .sql file, a .ms file and a folder; "
                                     "it must be just one");
            continue;
        }
        if (auto [it, added] = numbers.emplace(number, name); !added) {
            diagnostics.error(label, "two migrations are numbered " + std::to_string(number) + ": " + it->second +
                                         " and " + name + "; renumber one");
            continue;
        }

        Migration m;
        m.number = number;
        m.name = name;
        if (f.script) {
            m.kind = Migration::Kind::Script;
            m.source = *f.script;
            auto file = code.read(*f.script);
            if (!file) {
                diagnostics.error(codeFileName(*f.script), file.error().message);
                continue;
            }
            m.checksum = checksumOf({{std::string(f.script->name()), file.value().contents()}});
            out.migrations.push_back(MigrationScript{number, name, std::move(file).value(), {}, false});
        } else if (!f.folder) {
            m.kind = Migration::Kind::Sql;
            auto chosen = chooseSql(f.sql, "migration " + name, dialect, code, diagnostics);
            if (!chosen) continue;
            if (!chosen->sql.allKeys().empty()) {
                diagnostics.error(codeFileName(chosen->path),
                                  "a .sql migration runs as it is, with no values, so it can't have placeholders "
                                  "({{" + chosen->sql.allKeys()[0] + "}})");
                continue;
            }
            if (chosen->sql.statements == 0) {
                diagnostics.error(codeFileName(chosen->path), "is empty");
                continue;
            }
            m.source = chosen->path;
            m.sql = chosen->contents;
            m.checksum = checksumOf({{std::string(chosen->path.name()), chosen->contents}});
        } else {
            m.kind = Migration::Kind::Folder;
            m.source = RelPath::parse("migrations/" + name).value();
            std::optional<CodeFile> main;
            std::vector<CodeFile> helpers;
            std::map<std::string, SqlGroup> groups;
            std::vector<std::pair<std::string, std::string>> applied;   // for the checksum
            bool ok = true;
            for (const RelPath& path : f.folderFiles) {
                std::string file = codeFileName(path);
                std::string base(path.name());
                if (auto parsed = sqlFileName(base)) {
                    if (!isName(parsed->stem)) {
                        diagnostics.error(file, "a query's name must be letters, digits and _, not starting with "
                                                "a digit; variants end in .sqlite.sql or .pg.sql");
                        ok = false;
                    } else if (!addSqlFile(groups, path, parsed->stem, parsed->dialect)) {
                        diagnostics.error(file, "query " + parsed->stem + " has two files for the same database");
                        ok = false;
                    }
                    continue;
                }
                if (endsWith(base, ".ms") && isName(base.substr(0, base.size() - 3))) {
                    auto read = code.read(path);
                    if (!read) {
                        diagnostics.error(file, read.error().message);
                        ok = false;
                        continue;
                    }
                    applied.emplace_back(base, read.value().contents());
                    if (base == "migrate.ms") {
                        main = std::move(read).value();
                    } else {
                        helpers.push_back(std::move(read).value());
                    }
                    continue;
                }
                diagnostics.error(file, "doesn't belong in a migration's folder, which holds migrate.ms, other .ms "
                                        "files it imports, and its own .sql queries");
                ok = false;
            }
            if (!main) {
                diagnostics.error(label + "/", "a migration's folder needs a migrate.ms, which defines "
                                               "migrate = function()");
                ok = false;
            }
            for (const auto& [qname, group] : groups) {
                auto q = makeQuery(qname, group, dialect, code, diagnostics);
                if (!q) {
                    ok = false;
                    continue;
                }
                auto read = code.read((*q)->source);
                if (read) applied.emplace_back(std::string((*q)->source.name()), read.value().contents());
                m.queries.emplace(qname, *q);
            }
            if (!ok) continue;
            std::sort(applied.begin(), applied.end());
            m.checksum = checksumOf(applied);
            out.migrations.push_back(MigrationScript{number, name, std::move(*main), std::move(helpers), true});
        }
        app.migrations.push_back(std::move(m));
    }
    std::sort(app.migrations.begin(), app.migrations.end(),
              [](const Migration& a, const Migration& b) { return a.number < b.number; });

    // --- code/seed.ms ---
    for (const CodeEntry& entry : listing.files) {
        if (entry.path.str() != "seed.ms") continue;
        auto file = code.read(entry.path);
        if (!file) {
            diagnostics.error(codeFileName(entry.path), file.error().message);
            break;
        }
        app.seed = entry.path;
        out.seed = std::move(file).value();
    }
    return out;
}

}  // namespace fourfold
