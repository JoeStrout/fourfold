// http, secrets and hmac: reaching other servers (overview §6).
//
//     http.get(url, headers=null)
//     http.head(url, headers=null)
//     http.delete(url, headers=null)
//     http.post(url, body=null, headers=null)
//     http.put(url, body=null, headers=null)
//     http.patch(url, body=null, headers=null)
//     http.request(method, url, body=null, headers=null)
//
// Each returns a map: status, headers (names lowercased), body (the text,
// or null if it isn't text), raw (RawData, if body is null), and json (the
// body parsed, if it's JSON; else null). A refused or failed request
// returns an error value; a response with any status (404, 500) isn't one.
//
// A URL, or a header's value, is a string, a secret (secrets.name), or a
// list of strings and secrets, joined as the request goes out:
//
//     http.get(url, {"Authorization": ["Bearer ", secrets.weatherKey]})
//
// A body that's a map or list is sent as JSON; a string as text; RawData as
// bytes. Each gets a Content-Type unless the headers give one.
//
//     secrets.name     a secret's handle (only secrets app code may use)
//     hmac.verify(algorithm, secret, message, signature)
//                      whether `signature` (hex or base64) is the HMAC of
//                      `message` with the secret: for checking a webhook's
//                      caller. "sha256" only, for now.
//
// The requests run on the worker pool, with the VM parked.

#include "../bridge.h"
#include "../host_impl.h"
#include "base/utf8.h"
#include "data_work.h"
#include "modules.h"
#include "raw_data.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::Value;

namespace {

// The run and its outbound client, or null with `error` set.
Run* outboundRun(Context& ctx, const std::string& function, std::shared_ptr<OutboundClient>& client,
                 IntrinsicResult& error) {
    Run* run = Run::of(ctx);
    if (!run || !run->canPark()) {
        error = intrinsicError(function + " can't be used at a page's top level; call it in render or a handler");
        return nullptr;
    }
    client = run->context.outbound;
    if (!client) {
        error = intrinsicError(function + " isn't available here" +
                               (run->kind == Run::Kind::Migration ? std::string(" (not in migrations)") : ""));
        return nullptr;
    }
    return run;
}

// A URL or header value: a string, number, secret, or list of them.
bool partsOf(Value v, OutboundParts& out, std::string& why, bool top = true) {
    if (v.IsString() || v.IsNumber()) {
        out.emplace_back(toStdString(v));
        return true;
    }
    if (const SecretHandle* h = secretHandleOf(v)) {
        out.emplace_back(SecretRef{h->name});
        return true;
    }
    if (top && v.IsList()) {
        int n = v.ListCount();
        for (int i = 0; i < n; i++) {
            if (!partsOf(v.ListGet(i), out, why, false)) return false;
        }
        return true;
    }
    why = "must be a string, a secret, or a list of strings and secrets";
    return false;
}

bool hasHeader(const OutboundRequest& r, const std::string& name) {
    for (const auto& [n, parts] : r.headers) {
        std::string lower = n;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == name) return true;
    }
    return false;
}

// Builds the request from the arguments; an error message, or "".
std::string buildRequest(const std::string& function, Value url, Value body, Value headers, OutboundRequest& r) {
    std::string why;
    if (!partsOf(url, r.url, why)) return function + ": the URL " + why;
    if (!headers.IsNull()) {
        if (!headers.IsMap()) return function + ": the headers must be a map, like {\"Accept\": \"application/json\"}";
        MiniScript::MapIterator it = headers.Iterator();
        Value key, value;
        while (MiniScript::map_iterator_next(&it, &key, &value)) {
            if (!key.IsString()) return function + ": a header's name must be a string";
            OutboundParts parts;
            if (!partsOf(value, parts, why)) return function + ": the " + toStdString(key) + " header " + why;
            r.headers.emplace_back(toStdString(key), std::move(parts));
        }
    }
    if (body.IsNull()) return "";
    std::string type;
    if (body.IsString() || body.IsNumber()) {
        r.body = toStdString(body);
        type = "text/plain; charset=utf-8";
    } else if (const std::string* bytes = rawDataBytes(body)) {
        r.body = *bytes;
        type = "application/octet-stream";
    } else if (secretHandleOf(body)) {
        return function + ": a secret can't be a request's body; put it in a header";
    } else if (body.IsMap() || body.IsList()) {
        std::optional<Datum> d = toDatum(body, why);
        if (!d) return function + ": the body can't be sent as JSON: it " + why;
        r.body = toJson(*d);
        type = "application/json";
    } else {
        return function + ": the body must be a string, a map or list (sent as JSON), or RawData";
    }
    if (!hasHeader(r, "content-type")) r.headers.emplace_back("Content-Type", OutboundParts{type});
    return "";
}

// The response, as plain data on the worker; made a map on resume.
AsyncResult responseResult(Result<OutboundResponse> response) {
    if (!response) return fail(response.error());
    OutboundResponse& resp = response.value();
    Datum::MapItems headers;
    std::string contentType;
    for (auto& [name, value] : resp.headers) {
        if (name == "content-type") contentType = value;
        headers[name] = Datum::string(toValidUtf8(value));
    }
    Datum::MapItems items;
    items["status"] = Datum::number(resp.status);
    items["headers"] = Datum::map(std::move(headers));
    if (isJsonMediaType(contentType)) {
        auto parsed = parseJson(resp.body);
        if (parsed) items["json"] = std::move(parsed).value();
    }
    AsyncResult r;
    r.value = Datum::map(std::move(items));
    r.bytes = std::move(resp.body);
    return r;
}

IntrinsicResult finishResponse(Run& run, const std::string& function) {
    std::optional<AsyncResult> result = std::move(run.async);
    run.async.reset();
    if (!result) return intrinsicError(function + ": no result (an engine bug)");
    if (result->error) return intrinsicError(function + ": " + result->error->message);
    Value map = toValue(result->value);
    if (map.MapGet(Value("json")).IsNull()) map.MapSet("json", Value::Null);
    const std::string& body = result->bytes;
    bool text = isValidUtf8(body) && body.find('\0') == std::string::npos;
    if (text) {
        map.MapSet("body", toValue(body));
        map.MapSet("raw", Value::Null);
    } else {
        Value raw = newRawData(run, body);
        if (raw.IsError()) return IntrinsicResult(raw);
        map.MapSet("body", Value::Null);
        map.MapSet("raw", raw);
    }
    map.Freeze();
    return IntrinsicResult(map);
}

// Sends `r` from the run, parked.
IntrinsicResult startRequest(Context& ctx, Run& run, std::shared_ptr<OutboundClient> client, OutboundRequest r) {
    // Copied: the app may be released before the worker is done, if the
    // run times out meanwhile.
    OutboundPolicy policy = run.app ? run.app->outbound : OutboundPolicy{};
    // Fixed now: a call that waits for a network thread has that much less.
    auto deadline = std::chrono::steady_clock::now() + run.host->timeLeft(run);
    return startNetworkWork(ctx, run, [client, policy = std::move(policy), r = std::move(r), deadline] {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        return responseResult(client->send(policy, r, std::max(left, std::chrono::milliseconds(0))));
    });
}

// http.get, http.head, http.delete: (url, headers).
Intrinsic makeNoBody(const char* method, const char* name) {
    std::string function = std::string("http.") + name;
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("url");
    f.AddParam("headers", Value::Null);
    f.set_Code([function, m = std::string(method)](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        std::shared_ptr<OutboundClient> client;
        Run* run = outboundRun(ctx, function, client, error);
        if (!run) return error;
        if (!partial.done) return finishResponse(*run, function);
        OutboundRequest r;
        r.method = m;
        std::string why = buildRequest(function, ctx.GetArg(0), Value::Null, ctx.GetArg(1), r);
        if (!why.empty()) return intrinsicError(why);
        return startRequest(ctx, *run, client, std::move(r));
    });
    return f;
}

// http.post, http.put, http.patch: (url, body, headers).
Intrinsic makeWithBody(const char* method, const char* name) {
    std::string function = std::string("http.") + name;
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("url");
    f.AddParam("body", Value::Null);
    f.AddParam("headers", Value::Null);
    f.set_Code([function, m = std::string(method)](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        std::shared_ptr<OutboundClient> client;
        Run* run = outboundRun(ctx, function, client, error);
        if (!run) return error;
        if (!partial.done) return finishResponse(*run, function);
        OutboundRequest r;
        r.method = m;
        std::string why = buildRequest(function, ctx.GetArg(0), ctx.GetArg(1), ctx.GetArg(2), r);
        if (!why.empty()) return intrinsicError(why);
        return startRequest(ctx, *run, client, std::move(r));
    });
    return f;
}

// http.request(method, url, body, headers).
Intrinsic makeRequest() {
    std::string function = "http.request";
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("method");
    f.AddParam("url");
    f.AddParam("body", Value::Null);
    f.AddParam("headers", Value::Null);
    f.set_Code([function](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        std::shared_ptr<OutboundClient> client;
        Run* run = outboundRun(ctx, function, client, error);
        if (!run) return error;
        if (!partial.done) return finishResponse(*run, function);
        Value method = ctx.GetArg(0);
        if (!method.IsString()) return intrinsicError(function + " needs a method, like \"GET\" or \"POST\"");
        OutboundRequest r;
        r.method = toStdString(method);
        for (char& c : r.method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::string why = buildRequest(function, ctx.GetArg(1), ctx.GetArg(2), ctx.GetArg(3), r);
        if (!why.empty()) return intrinsicError(why);
        return startRequest(ctx, *run, client, std::move(r));
    });
    return f;
}

// hmac.verify(algorithm, secret, message, signature).
Intrinsic makeVerify() {
    std::string function = "hmac.verify";
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("algorithm");
    f.AddParam("secret");
    f.AddParam("message");
    f.AddParam("signature");
    f.set_Code([function](Context ctx, IntrinsicResult partial) {
        IntrinsicResult error = IntrinsicResult::Null;
        std::shared_ptr<OutboundClient> client;
        Run* run = outboundRun(ctx, function, client, error);
        if (!run) return error;
        if (!partial.done) return finishWork(*run, function);
        Value algorithm = ctx.GetArg(0), secret = ctx.GetArg(1), message = ctx.GetArg(2), signature = ctx.GetArg(3);
        if (!algorithm.IsString()) return intrinsicError(function + " needs an algorithm: \"sha256\"");
        const SecretHandle* h = secretHandleOf(secret);
        if (!h) return intrinsicError(function + " needs a secret as its key, like secrets.webhookKey");
        std::string text;
        if (message.IsString()) {
            text = toStdString(message);
        } else if (const std::string* bytes = rawDataBytes(message)) {
            text = *bytes;
        } else {
            return intrinsicError(function + " needs the message as a string or RawData (usually req.rawBody)");
        }
        // A missing signature header is a failed check, not a mistake.
        std::string sig = signature.IsString() ? toStdString(signature) : "";
        return startWork(ctx, *run, [client, a = toStdString(algorithm), name = h->name, text, sig] {
            Result<bool> ok = client->verifyHmac(a, name, text, sig);
            if (!ok) return fail(ok.error());
            AsyncResult r;
            r.value = Datum::number(ok.value() ? 1 : 0);
            return r;
        });
    });
    return f;
}

}  // namespace

void addHttp() {
    addModule("http", {{"get", makeNoBody("GET", "get")},
                       {"head", makeNoBody("HEAD", "head")},
                       {"delete", makeNoBody("DELETE", "delete")},
                       {"post", makeWithBody("POST", "post")},
                       {"put", makeWithBody("PUT", "put")},
                       {"patch", makeWithBody("PATCH", "patch")},
                       {"request", makeRequest()}});
    addModule("hmac", {{"verify", makeVerify()}});

    // secrets: a map of handles, made for each run from its client's
    // secrets. Engine-only secrets are there too, so using one explains
    // itself (the client refuses it) rather than being a missing key.
    Intrinsic f = Intrinsic::Create("secrets");
    f.set_Code([](Context ctx, IntrinsicResult) {
        Run* run = Run::of(ctx);
        if (!run || !run->canPark()) {
            return intrinsicError("secrets can't be used at a page's top level; use them in render or a handler");
        }
        if (!run->context.outbound) return intrinsicError("secrets aren't available here");
        std::map<std::string, std::string> uses = run->context.outbound->secretUses();
        Value map = Value::make_map(static_cast<int>(uses.size()));
        for (const auto& [name, use] : uses) map.MapSet(toValue(name), makeSecretHandle(name));
        map.Freeze();
        return IntrinsicResult(map);
    });
}

}  // namespace fourfold
