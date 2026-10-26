#include "raw_data.h"

#include <cmath>
#include <cstring>
#include <unordered_set>

#include "../bridge.h"
#include "../host_impl.h"
#include "base/utf8.h"
#include "modules.h"

namespace fourfold {

using MiniScript::Context;
using MiniScript::Intrinsic;
using MiniScript::IntrinsicResult;
using MiniScript::String;
using MiniScript::Value;

namespace {

struct RawBuffer {
    std::string bytes;
    std::shared_ptr<RawBudget> budget;
};

Value g_class;   // the frozen class map, rooted for the life of the process

// Live buffers. Script thread only, like the handles in bridge.cpp.
std::unordered_set<const RawBuffer*>& liveBuffers() {
    static std::unordered_set<const RawBuffer*> live;
    return live;
}

void finalizeBuffer(void* p) {
    auto* b = static_cast<RawBuffer*>(p);
    liveBuffers().erase(b);
    b->budget->used -= b->bytes.size();
    delete b;
}

Value handleKey() { return Value(String("_handle")); }

RawBuffer* bufferOf(Value instance) {
    if (!instance.IsMap()) return nullptr;
    Value h;
    if (!instance.TryGet(handleKey(), &h) || !h.IsHandle()) return nullptr;
    auto* b = static_cast<RawBuffer*>(h.HandlePtr());
    return liveBuffers().count(b) ? b : nullptr;
}

std::shared_ptr<RawBudget> budgetOf(Run& run) {
    if (!run.rawBudget) {
        run.rawBudget = std::make_shared<RawBudget>();
        run.rawBudget->maxBuffer = run.host->options.budget.maxRawDataBytes;
        run.rawBudget->maxTotal = run.host->options.budget.maxRawDataTotal;
    }
    return run.rawBudget;
}

// Room for `more` bytes in the budget, or an error message. A full budget
// may be only garbage not yet collected, so that's tried once first.
std::optional<std::string> makeRoom(RawBudget& budget, size_t newSize, size_t more) {
    if (newSize > budget.maxBuffer) {
        return "RawData can't be over " + std::to_string(budget.maxBuffer) + " bytes";
    }
    if (budget.used + more > budget.maxTotal) MiniScript::GCManager::CollectGarbage();
    if (budget.used + more > budget.maxTotal) {
        return "a request's RawData can't total over " + std::to_string(budget.maxTotal) + " bytes";
    }
    return std::nullopt;
}

Value newHandle(std::string bytes, std::shared_ptr<RawBudget> budget) {
    budget->used += bytes.size();
    auto* b = new RawBuffer{std::move(bytes), std::move(budget)};
    liveBuffers().insert(b);
    return Value::NewHandle(b, finalizeBuffer);
}

Value newInstance(Value handle) {
    Value instance = Value::make_map(2);
    instance.MapSet(Value::magicIsA, g_class);
    instance.MapSet(handleKey(), handle);
    return instance;
}

// resize(n) and `len = n`. Grows (with zeros) or shrinks the buffer in
// place, or gives the instance its first one.
IntrinsicResult resize(Context& ctx, Value self, Value size) {
    if (!self.IsMap() || !isRawData(self)) return intrinsicError("resize must be called on a RawData");
    if (!size.IsNumber() || !std::isfinite(size.DoubleValue())) {
        return intrinsicError("RawData's size must be a number");
    }
    double n = std::floor(size.DoubleValue());
    size_t newSize = n < 0 ? 0 : n > 1e15 ? static_cast<size_t>(1e15) : static_cast<size_t>(n);
    Run* run = Run::of(ctx);
    if (!run) return intrinsicError("RawData isn't available here");
    RawBuffer* b = bufferOf(self);
    std::shared_ptr<RawBudget> budget = b ? b->budget : budgetOf(*run);
    size_t oldSize = b ? b->bytes.size() : 0;
    size_t more = newSize > oldSize ? newSize - oldSize : 0;
    if (auto why = makeRoom(*budget, newSize, more)) return intrinsicError(*why);
    if (b) {
        budget->used = budget->used - oldSize + newSize;
        b->bytes.resize(newSize, '\0');
        return IntrinsicResult::Null;
    }
    if (self.IsFrozen()) return intrinsicError("this RawData is frozen");
    self.MapSet(handleKey(), newHandle(std::string(newSize, '\0'), budget));
    return IntrinsicResult::Null;
}

bool littleEndian(Value self) {
    Value v;
    if (!self.Lookup(Value(String("littleEndian")), &v)) return true;
    return v.IsNull() || !v.IsNumber() || v.DoubleValue() != 0;
}

// The buffer and checked offset for an accessor of `width` bytes, or an
// error. A negative offset counts from the end, as in MiniScript's RawData.
RawBuffer* access(Value self, Value offsetValue, size_t width, size_t& offset, IntrinsicResult& error) {
    RawBuffer* b = bufferOf(self);
    if (!b) {
        error = intrinsicError("RawData has no buffer (use resize first)");
        return nullptr;
    }
    double len = static_cast<double>(b->bytes.size());
    double off = offsetValue.IsNumber() ? std::trunc(offsetValue.DoubleValue()) : 0;
    if (off < 0) off += len;
    if (!(off >= 0) || off + static_cast<double>(width) > len) {
        error = intrinsicError("RawData index out of bounds");
        return nullptr;
    }
    offset = static_cast<size_t>(off);
    return b;
}

uint64_t readUnsigned(const RawBuffer& b, size_t off, size_t width, bool le) {
    uint64_t v = 0;
    for (size_t i = 0; i < width; i++) {
        uint64_t byte = static_cast<uint8_t>(b.bytes[off + i]);
        v |= byte << (8 * (le ? i : width - 1 - i));
    }
    return v;
}

void writeUnsigned(RawBuffer& b, size_t off, size_t width, bool le, uint64_t v) {
    for (size_t i = 0; i < width; i++) {
        b.bytes[off + i] = static_cast<char>((v >> (8 * (le ? i : width - 1 - i))) & 0xFF);
    }
}

// A number as integer bits: truncated toward zero, wrapping like the casts
// in MiniScript's RawData (and 0 for NaN), without C++'s undefined
// out-of-range conversions.
uint64_t integerBits(Value v) {
    double d = v.IsNumber() ? std::trunc(v.DoubleValue()) : 0;
    if (!std::isfinite(d)) return 0;
    if (d >= 9.2e18) return static_cast<uint64_t>(INT64_MAX);
    if (d <= -9.2e18) return static_cast<uint64_t>(INT64_MIN);
    return static_cast<uint64_t>(static_cast<int64_t>(d));
}

enum class Kind { U8, I8, U16, I16, U32, I32, F32, F64 };

size_t widthOf(Kind k) {
    switch (k) {
        case Kind::U8: case Kind::I8: return 1;
        case Kind::U16: case Kind::I16: return 2;
        case Kind::U32: case Kind::I32: case Kind::F32: return 4;
        case Kind::F64: return 8;
    }
    return 1;
}

Value get(const RawBuffer& b, size_t off, Kind k, bool le) {
    uint64_t v = readUnsigned(b, off, widthOf(k), le);
    switch (k) {
        case Kind::U8: case Kind::U16: case Kind::U32: return Value(static_cast<double>(v));
        case Kind::I8: return Value(static_cast<double>(static_cast<int8_t>(v)));
        case Kind::I16: return Value(static_cast<double>(static_cast<int16_t>(v)));
        case Kind::I32: return Value(static_cast<double>(static_cast<int32_t>(v)));
        case Kind::F32: {
            uint32_t bits = static_cast<uint32_t>(v);
            float f;
            std::memcpy(&f, &bits, sizeof f);
            return Value(static_cast<double>(f));
        }
        case Kind::F64: {
            double d;
            std::memcpy(&d, &v, sizeof d);
            return Value(d);
        }
    }
    return Value::Null;
}

void set(RawBuffer& b, size_t off, Kind k, bool le, Value value) {
    uint64_t bits;
    if (k == Kind::F32) {
        float f = static_cast<float>(value.IsNumber() ? value.DoubleValue() : 0);
        uint32_t u;
        std::memcpy(&u, &f, sizeof u);
        bits = u;
    } else if (k == Kind::F64) {
        double d = value.IsNumber() ? value.DoubleValue() : 0;
        std::memcpy(&bits, &d, sizeof bits);
    } else {
        bits = integerBits(value);
    }
    writeUnsigned(b, off, widthOf(k), le, bits);
}

Intrinsic makeGetter(Kind k) {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("self", Value::Null);
    f.AddParam("offset", Value::zero);
    f.set_Code([k](Context ctx, IntrinsicResult) {
        Value self = ctx.GetArg(0);
        IntrinsicResult error = IntrinsicResult::Null;
        size_t off = 0;
        RawBuffer* b = access(self, ctx.GetArg(1), widthOf(k), off, error);
        if (!b) return error;
        return IntrinsicResult(get(*b, off, k, littleEndian(self)));
    });
    return f;
}

Intrinsic makeSetter(Kind k) {
    Intrinsic f = Intrinsic::Create("");
    f.set_AffectsState(true);
    f.AddParam("self", Value::Null);
    f.AddParam("offset", Value::zero);
    f.AddParam("value", Value::zero);
    f.set_Code([k](Context ctx, IntrinsicResult) {
        Value self = ctx.GetArg(0);
        IntrinsicResult error = IntrinsicResult::Null;
        size_t off = 0;
        RawBuffer* b = access(self, ctx.GetArg(1), widthOf(k), off, error);
        if (!b) return error;
        set(*b, off, k, littleEndian(self), ctx.GetArg(2));
        return IntrinsicResult::Null;
    });
    return f;
}

// Text from bytes: up to a NUL (as in MiniScript's RawData), with each byte
// that isn't part of valid UTF-8 shown as U+FFFD, since strings are text.
std::string textFrom(std::string_view bytes) {
    size_t nul = bytes.find('\0');
    if (nul != std::string_view::npos) bytes = bytes.substr(0, nul);
    if (isValidUtf8(bytes)) return std::string(bytes);
    std::string out;
    size_t i = 0;
    while (i < bytes.size()) {
        auto lead = static_cast<uint8_t>(bytes[i]);
        size_t len = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 0;
        if (len && i + len <= bytes.size() && isValidUtf8(bytes.substr(i, len))) {
            out.append(bytes.substr(i, len));
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            i++;
        }
    }
    return out;
}

Intrinsic makeUtf8() {
    Intrinsic f = Intrinsic::Create("");
    f.AddParam("self", Value::Null);
    f.AddParam("offset", Value::zero);
    f.AddParam("bytes", Value(-1.0));
    f.set_Code([](Context ctx, IntrinsicResult) {
        IntrinsicResult error = IntrinsicResult::Null;
        size_t off = 0;
        RawBuffer* b = access(ctx.GetArg(0), ctx.GetArg(1), 0, off, error);
        if (!b) return error;
        size_t available = b->bytes.size() - off;
        Value countValue = ctx.GetArg(2);
        double count = countValue.IsNumber() ? countValue.DoubleValue() : -1;
        size_t n = !(count >= 0) || count > static_cast<double>(available) ? available : static_cast<size_t>(count);
        return IntrinsicResult(toValue(textFrom(std::string_view(b->bytes).substr(off, n))));
    });
    return f;
}

Intrinsic makeSetUtf8() {
    Intrinsic f = Intrinsic::Create("");
    f.set_AffectsState(true);
    f.AddParam("self", Value::Null);
    f.AddParam("offset", Value::zero);
    f.AddParam("value", Value(String("")));
    f.set_Code([](Context ctx, IntrinsicResult) {
        IntrinsicResult error = IntrinsicResult::Null;
        size_t off = 0;
        RawBuffer* b = access(ctx.GetArg(0), ctx.GetArg(1), 0, off, error);
        if (!b) return error;
        std::string text = toStdString(ctx.GetArg(2));
        // As much as fits, as in MiniScript's RawData; returns how much.
        size_t n = std::min(text.size(), b->bytes.size() - off);
        b->bytes.replace(off, n, text, 0, n);
        return IntrinsicResult(Value(static_cast<double>(n)));
    });
    return f;
}

}  // namespace

Value newRawData(Run& run, std::string bytes) {
    std::shared_ptr<RawBudget> budget = budgetOf(run);
    if (auto why = makeRoom(*budget, bytes.size(), bytes.size())) {
        return MiniScript::ErrorTypes::RuntimeError(String(why->data(), why->size()));
    }
    return newInstance(newHandle(std::move(bytes), budget));
}

const std::string* rawDataBytes(Value v) {
    if (!isRawData(v)) return nullptr;
    static const std::string empty;
    RawBuffer* b = bufferOf(v);
    return b ? &b->bytes : &empty;
}

bool isRawData(Value v) {
    if (!v.IsMap() || g_class.IsNull()) return false;
    Value isa;
    return v.TryGet(Value::magicIsA, &isa) && isa.RefEquals(g_class);
}

void addRawData() {
    std::vector<std::pair<const char*, Intrinsic>> methods;

    Intrinsic len = Intrinsic::Create("");
    len.AddParam("self", Value::Null);
    len.set_Code([](Context ctx, IntrinsicResult) {
        RawBuffer* b = bufferOf(ctx.GetArg(0));
        return IntrinsicResult(Value(b ? static_cast<double>(b->bytes.size()) : 0.0));
    });
    methods.push_back({"len", len});

    Intrinsic resizeF = Intrinsic::Create("");
    resizeF.set_AffectsState(true);
    resizeF.AddParam("self", Value::Null);
    resizeF.AddParam("bytes", Value(32.0));
    resizeF.set_Code([](Context ctx, IntrinsicResult) { return resize(ctx, ctx.GetArg(0), ctx.GetArg(1)); });
    methods.push_back({"resize", resizeF});

    // `len = n`: the same as resize, spelled as an assignment.
    Intrinsic setLen = Intrinsic::Create("");
    setLen.set_AffectsState(true);
    setLen.AddParam("self", Value::Null);
    setLen.AddParam("value", Value::zero);
    setLen.set_Code([](Context ctx, IntrinsicResult) { return resize(ctx, ctx.GetArg(0), ctx.GetArg(1)); });
    methods.push_back({"len=", setLen});

    const std::pair<const char*, Kind> kinds[] = {
        {"byte", Kind::U8},    {"sbyte", Kind::I8}, {"ushort", Kind::U16}, {"short", Kind::I16},
        {"uint", Kind::U32},   {"int", Kind::I32},  {"float", Kind::F32},  {"double", Kind::F64},
    };
    static const char* const setterNames[] = {"setByte", "setSbyte", "setUshort", "setShort",
                                              "setUint", "setInt",   "setFloat",  "setDouble"};
    for (size_t i = 0; i < 8; i++) {
        methods.push_back({kinds[i].first, makeGetter(kinds[i].second)});
        methods.push_back({setterNames[i], makeSetter(kinds[i].second)});
    }
    methods.push_back({"utf8", makeUtf8()});
    methods.push_back({"setUtf8", makeSetUtf8()});

    g_class = Value::make_map(static_cast<int>(methods.size()) + 2);
    g_class.MapSet(handleKey(), Value::Null);
    g_class.MapSet(Value(String("littleEndian")), Value::one);
    for (auto& [key, f] : methods) g_class.MapSet(Value(String(key)), f.GetFunc());
    g_class.Freeze();
    MiniScript::GCManager::AddRoot(g_class);

    Value cls = g_class;
    Intrinsic::Create("RawData").set_Code([cls](Context, IntrinsicResult) { return IntrinsicResult(cls); });
}

}  // namespace fourfold
