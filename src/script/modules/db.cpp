// db: the app's database (overview §3, Middle tier, Tier 2, SQL dialects).
//
//     db.query(name, values=null)   runs the named query; returns its rows,
//                                   a list of maps by column name
//
// `name` is a file in code/queries/ ("findUser" for findUser.sql, or
// "admin/users"); in a migration, one of the migration's own queries. Its
// {{key}} placeholders take `values`: a map, or a list when every key is an
// index ({{0}}, {{1}}). A number key in the map (0) is used before the same
// key as a string ("0"). A name slot, {{sort: title, created}}, takes one of
// its own names. Failures come back as error values. The query runs on the
// worker pool, with the VM parked.

#include <cmath>

#include "../bridge.h"
#include "../host_impl.h"
#include "data_work.h"
#include "modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

namespace {

// The values, as a Datum. A map's number keys (0, 1...) become strings,
// winning over the same key written as a string.
std::optional<Datum> valuesArg(Value v, std::string& why) {
    if (!v.IsMap()) return toDatum(v, why);
    Datum::MapItems items, numbered;
    MiniScript::MapIterator it = v.Iterator();
    Value key, item;
    while (MiniScript::map_iterator_next(&it, &key, &item)) {
        std::optional<Datum> d = toDatum(item, why);
        if (!d) return std::nullopt;
        if (key.IsString()) {
            items[toStdString(key)] = std::move(*d);
        } else if (key.IsNumber() && key.DoubleValue() >= 0 && key.DoubleValue() < 1e9 &&
                   std::floor(key.DoubleValue()) == key.DoubleValue()) {
            numbered[std::to_string(static_cast<int64_t>(key.DoubleValue()))] = std::move(*d);
        }
        // Any other key can't name a placeholder; it's ignored, like an
        // unused one.
    }
    for (auto& [k, d] : numbered) items[k] = std::move(d);
    return Datum::map(std::move(items));
}

Intrinsic makeQuery() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("name");
    f.AddParam("values", Value::Null);
    f.set_Code([](Context ctx, IntrinsicResult partial) {
        Run* run = Run::of(ctx);
        Value nameValue = ctx.GetArg(0);
        std::string label = "db.query" + (nameValue.IsString() ? "(\"" + toStdString(nameValue) + "\")" : "");
        if (!partial.done) return finishWork(*run, label);   // the query is done

        if (!run || !run->canPark()) return intrinsicError("db.query can't be used at a page's top level");
        QueryRunner* db = run->context.db;
        const QuerySet* queries = run->context.queries;
        if (!db || !queries) {
            return intrinsicError(run->kind == Run::Kind::Migration
                                      ? "db isn't available in a single-file migration: make it a folder, with its "
                                        "own queries"
                                      : "db isn't available here: the app has no database");
        }
        if (!nameValue.IsString()) {
            return intrinsicError("db.query needs a query's name, like db.query(\"findUser\", [email])");
        }
        std::string name = toStdString(nameValue);
        auto found = queries->find(name);
        if (found == queries->end()) {
            if (run->kind == Run::Kind::Migration) {
                return intrinsicError(label + ": this migration has no query named " + name +
                                      " (a migration uses only the .sql files in its own folder)");
            }
            return intrinsicError(label + ": there's no query named " + name + " (code/queries/" + name + ".sql)");
        }
        std::string why;
        std::optional<Datum> values = valuesArg(ctx.GetArg(1), why);
        if (!values) {
            return intrinsicError(label + ": the values " + why +
                                  " (only numbers, strings, lists and maps can be passed)");
        }
        std::shared_ptr<const NamedQuery> query = found->second;
        auto bound = bindQuery(query->sql, *values);
        if (!bound) return intrinsicError(label + ": " + bound.error().message);
        if (!query->sql.slots.empty()) {
            // Its name slots filled in, from the query's own lists of names.
            auto filled = std::make_shared<NamedQuery>(*query);
            filled->sql.sql = bound.value().sql;
            query = std::move(filled);
        }
        return startWork(ctx, *run, [db, query, p = std::move(bound.value().params)] {
            auto rows = db->query(*query, p);
            if (!rows) return fail(rows.error());
            AsyncResult r;
            r.value = std::move(rows).value();
            return r;
        });
    });
    return f;
}

}  // namespace

void addDb() { addModule("db", {{"query", makeQuery()}}); }

}  // namespace fourfold
