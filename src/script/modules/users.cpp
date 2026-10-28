// users: the engine's users (overview §5, The user object), for trusted app
// code: looking users up, and granting and revoking roles.
//
//     users.get(id)              the user's map (id, name, email, roles), or null
//     users.find(email)          the same, by email address
//     users.grant(id, role)      gives the user a role
//     users.revoke(id, role)     takes it away
//
// Roles take effect on the user's next request: `req.user` is looked up as
// each request starts. Failures come back as error values. The work happens
// on the worker pool, with the VM parked.

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

// The run and its user directory, or null with `error` set.
Run* usersRun(Context& ctx, const std::string& function, UserDirectory*& users, IntrinsicResult& error) {
    Run* run = Run::of(ctx);
    if (!run || run->kind != Run::Kind::Request) {
        error = intrinsicError(function + " can't be used at a page's top level");
        return nullptr;
    }
    users = run->context.users;
    if (!users) {
        error = intrinsicError(function + " isn't available here");
        return nullptr;
    }
    return run;
}

Datum userDatum(const UserFacts& user) {
    Datum::ListItems roles;
    for (const std::string& role : user.roles) roles.push_back(Datum::string(role));
    return Datum::map({{"id", Datum::string(user.id)},
                       {"name", Datum::string(user.name)},
                       {"email", Datum::string(user.email)},
                       {"roles", Datum::list(std::move(roles))}});
}

AsyncResult userResult(Result<std::optional<UserFacts>> user) {
    if (!user) return fail(user.error());
    AsyncResult r;
    if (user.value()) r.value = userDatum(*user.value());
    return r;
}

// users.get(id) and users.find(email).
Intrinsic makeLookup(const char* name, const char* param, bool byEmail) {
    std::string function = std::string("users.") + name;
    Intrinsic f = Intrinsic::Create("");
    f.AddParam(param);
    f.set_Code([function, param, byEmail](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        UserDirectory* users = nullptr;
        Run* run = usersRun(ctx, function, users, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, function);
        Value arg = ctx.GetArg(0);
        if (!arg.IsString()) return intrinsicError(function + " needs " + param + " as a string");
        std::string key = toStdString(arg);
        return startWork(ctx, *run, [users, key, byEmail] {
            return userResult(byEmail ? users->findByEmail(key) : users->get(key));
        });
    });
    return f;
}

// users.grant(id, role) and users.revoke(id, role).
Intrinsic makeRoleChange(const char* name, bool grant) {
    std::string function = std::string("users.") + name;
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("id");
    f.AddParam("role");
    f.set_Code([function, grant](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        UserDirectory* users = nullptr;
        Run* run = usersRun(ctx, function, users, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, function);
        Value id = ctx.GetArg(0), role = ctx.GetArg(1);
        if (!id.IsString()) return intrinsicError(function + " needs the user's id as a string");
        std::string r = role.IsString() ? toStdString(role) : "";
        if (!isValidRoleName(r)) {
            return intrinsicError(function + " needs a role name: a letter, then letters, digits, _ or - "
                                             "(and not \"login\" or \"public\")");
        }
        std::string userId = toStdString(id);
        return startWork(ctx, *run, [users, userId, r, grant] {
            Status s = grant ? users->grant(userId, r) : users->revoke(userId, r);
            return s ? AsyncResult{} : fail(s.error());
        });
    });
    return f;
}

}  // namespace

void addUsers() {
    addModule("users", {{"get", makeLookup("get", "id", false)},
                        {"find", makeLookup("find", "email", true)},
                        {"grant", makeRoleChange("grant", true)},
                        {"revoke", makeRoleChange("revoke", false)}});
}

}  // namespace fourfold
