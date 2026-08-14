// =============================================================================
// src/vm/runtime/runtime.cc
// =============================================================================

#include "vm/runtime/runtime.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include "base/macros.h"
#include "base/tagged-value.h"
#include "vm/isolate/isolate.h"
#include "vm/objects/object.h"
#include "vm/objects/primitives.h"

namespace v12 {

// Smi range — defined by TaggedValue but mirrored here for clarity.
static constexpr intptr_t kSmiMin = -(intptr_t{1} << 62);
static constexpr intptr_t kSmiMax =  (intptr_t{1} << 62) - 1;

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

namespace {

// Parse a string as a number, following ECMA-262 §7.1.4.1.
// Empty/whitespace -> 0. Hex literals supported. Otherwise strtod.
double StringToNumber(std::string_view s) {
    // Trim leading/trailing whitespace.
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' ||
                                 s[start] == '\n' || s[start] == '\r' ||
                                 s[start] == '\f' || s[start] == '\v')) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' ||
                            s[end-1] == '\n' || s[end-1] == '\r' ||
                            s[end-1] == '\f' || s[end-1] == '\v')) {
        --end;
    }
    if (start == end) return 0.0;  // empty -> 0

    std::string trimmed(s.data() + start, end - start);

    // Special literals.
    if (trimmed == "Infinity" || trimmed == "+Infinity") return std::numeric_limits<double>::infinity();
    if (trimmed == "-Infinity") return -std::numeric_limits<double>::infinity();
    if (trimmed == "NaN") return std::numeric_limits<double>::quiet_NaN();

    // Hex literal: 0x...
    if (trimmed.size() > 2 && trimmed[0] == '0' &&
        (trimmed[1] == 'x' || trimmed[1] == 'X')) {
        long long v = std::strtoll(trimmed.c_str() + 2, nullptr, 16);
        return static_cast<double>(v);
    }
    // Binary literal: 0b...
    if (trimmed.size() > 2 && trimmed[0] == '0' &&
        (trimmed[1] == 'b' || trimmed[1] == 'B')) {
        long long v = std::strtoll(trimmed.c_str() + 2, nullptr, 2);
        return static_cast<double>(v);
    }
    // Octal literal: 0o...
    if (trimmed.size() > 2 && trimmed[0] == '0' &&
        (trimmed[1] == 'o' || trimmed[1] == 'O')) {
        long long v = std::strtoll(trimmed.c_str() + 2, nullptr, 8);
        return static_cast<double>(v);
    }

    // Decimal — use strtod. If it doesn't consume the whole string, NaN.
    const char* str = trimmed.c_str();
    char* endp = nullptr;
    double v = std::strtod(str, &endp);
    if (endp == str) return std::numeric_limits<double>::quiet_NaN();
    // Allow trailing whitespace only (already trimmed, so endp should be at the end).
    while (*endp != '\0') {
        if (*endp != ' ' && *endp != '\t' && *endp != '\n' &&
            *endp != '\r' && *endp != '\f' && *endp != '\v') {
            return std::numeric_limits<double>::quiet_NaN();
        }
        ++endp;
    }
    return v;
}

// Convert a double to a JS number Value (Smi if it fits, HeapNumber otherwise).
Value NumberFromDouble(Isolate* iso, double d) {
    // Smi only if the double is integer-valued and within Smi range.
    if (std::isfinite(d) && d == std::floor(d) &&
        d >= static_cast<double>(kSmiMin) && d <= static_cast<double>(kSmiMax)) {
        return Value::FromSmi(static_cast<intptr_t>(d));
    }
    return Value::FromHeap(JSNumber::New(iso, d));
}

// Convert a double to its string representation per ECMA-262 §7.1.12.1.
// (Delegates to JSString::NewFromDouble for the actual formatting.)
Value StringFromDouble(Isolate* iso, double d) {
    return Value::FromHeap(JSString::NewFromDouble(iso, d));
}

}  // namespace

// -----------------------------------------------------------------------------
// ToBoolean
// -----------------------------------------------------------------------------
Value ToBoolean(Isolate* iso, Value v) {
    if (v.IsSmi()) {
        return v.AsSmi() == 0 ? iso->false_value() : iso->true_value();
    }
    if (v.IsHeapObject()) {
        HeapObject* h = v.AsHeapObject();
        switch (h->kind()) {
            case HeapObjectKind::kUndefined:
            case HeapObjectKind::kNull:
                return iso->false_value();
            case HeapObjectKind::kBoolean:
                return v;  // already a boolean
            case HeapObjectKind::kNumber: {
                double d = static_cast<JSNumber*>(h)->value();
                if (d == 0.0 || std::isnan(d)) return iso->false_value();
                return iso->true_value();
            }
            case HeapObjectKind::kString:
                return static_cast<JSString*>(h)->length() == 0
                       ? iso->false_value() : iso->true_value();
            default:
                // All other objects (including arrays, functions, etc.) are truthy.
                return iso->true_value();
        }
    }
    return iso->false_value();
}

// -----------------------------------------------------------------------------
// ToNumber
// -----------------------------------------------------------------------------
Value ToNumber(Isolate* iso, Value v) {
    if (v.IsSmi()) return v;
    if (!v.IsHeapObject()) return iso->undefined_value();  // unreachable
    HeapObject* h = v.AsHeapObject();
    switch (h->kind()) {
        case HeapObjectKind::kNumber:
            return v;
        case HeapObjectKind::kBoolean:
            return Value::FromSmi(static_cast<JSBoolean*>(h)->value() ? 1 : 0);
        case HeapObjectKind::kUndefined:
            return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
        case HeapObjectKind::kNull:
            return Value::FromSmi(0);
        case HeapObjectKind::kString:
            return NumberFromDouble(iso, StringToNumber(static_cast<JSString*>(h)->view()));
        default:
            // For now, objects coerce to NaN (real spec calls valueOf).
            return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    }
}

// -----------------------------------------------------------------------------
// ToString
// -----------------------------------------------------------------------------
Value ToString(Isolate* iso, Value v) {
    if (v.IsSmi()) {
        return Value::FromHeap(JSString::NewFromSmi(iso, v.AsSmi()));
    }
    if (!v.IsHeapObject()) return iso->undefined_value();
    HeapObject* h = v.AsHeapObject();
    switch (h->kind()) {
        case HeapObjectKind::kString:
            return v;
        case HeapObjectKind::kNumber:
            return StringFromDouble(iso, static_cast<JSNumber*>(h)->value());
        case HeapObjectKind::kBoolean:
            return Value::FromHeap(JSString::New(iso,
                static_cast<JSBoolean*>(h)->value() ? "true" : "false"));
        case HeapObjectKind::kUndefined:
            return Value::FromHeap(JSString::New(iso, "undefined"));
        case HeapObjectKind::kNull:
            return Value::FromHeap(JSString::New(iso, "null"));
        case HeapObjectKind::kArray: {
            // Join elements with "," per Array.prototype.toString.
            JSArray* arr = static_cast<JSArray*>(h);
            std::string out;
            for (uint32_t i = 0; i < arr->length(); ++i) {
                if (i > 0) out += ",";
                Value e = arr->GetElement(i);
                if (e.IsNullOrUndefined()) continue;  // empty
                Value s = ToString(iso, e);
                out += std::string(static_cast<JSString*>(s.AsHeapObject())->view());
            }
            return Value::FromHeap(JSString::New(iso, out));
        }
        default:
            // Objects -> "[object Object]" per spec.
            return Value::FromHeap(JSString::New(iso, "[object Object]"));
    }
}

// -----------------------------------------------------------------------------
// ToObject
// -----------------------------------------------------------------------------
Value ToObject(Isolate* iso, Value v) {
    (void)iso;
    // For now, primitives stay as primitives. Real ToObject would wrap them.
    return v;
}

// -----------------------------------------------------------------------------
// Typeof
// -----------------------------------------------------------------------------
Value Typeof(Isolate* iso, Value v) {
    const char* s;
    if (v.IsSmi()) {
        s = "number";
    } else if (v.IsHeapObject()) {
        switch (v.AsHeapObject()->kind()) {
            case HeapObjectKind::kNumber:     s = "number"; break;
            case HeapObjectKind::kString:
            case HeapObjectKind::kConsString: s = "string"; break;
            case HeapObjectKind::kBoolean:    s = "boolean"; break;
            case HeapObjectKind::kUndefined:  s = "undefined"; break;
            case HeapObjectKind::kFunction:
            case HeapObjectKind::kExternal:   s = "function"; break;
            case HeapObjectKind::kSymbol:     s = "symbol"; break;
            default:                          s = "object"; break;
        }
    } else {
        s = "undefined";
    }
    return Value::FromHeap(JSString::New(iso, s));
}

// -----------------------------------------------------------------------------
// Comparison
// -----------------------------------------------------------------------------
Value StrictEquals(Isolate* iso, Value a, Value b) {
    (void)iso;
    // Different tag -> not equal.
    if (a.IsSmi() != b.IsSmi()) {
        // One is Smi, other is HeapObject. They're equal only if the heap
        // object is a HeapNumber with the same value as the Smi — but
        // strict equality requires same type, so they're NOT equal.
        return iso->false_value();
    }
    if (a.IsSmi()) {
        return a.AsSmi() == b.AsSmi() ? iso->true_value() : iso->false_value();
    }
    // Both heap objects.
    HeapObject* ha = a.AsHeapObject();
    HeapObject* hb = b.AsHeapObject();
    if (ha->kind() != hb->kind()) return iso->false_value();
    switch (ha->kind()) {
        case HeapObjectKind::kUndefined:
        case HeapObjectKind::kNull:
            return iso->true_value();
        case HeapObjectKind::kBoolean:
            return static_cast<JSBoolean*>(ha)->value() == static_cast<JSBoolean*>(hb)->value()
                   ? iso->true_value() : iso->false_value();
        case HeapObjectKind::kNumber: {
            double x = static_cast<JSNumber*>(ha)->value();
            double y = static_cast<JSNumber*>(hb)->value();
            if (std::isnan(x) || std::isnan(y)) return iso->false_value();
            return x == y ? iso->true_value() : iso->false_value();
        }
        case HeapObjectKind::kString:
            return static_cast<JSString*>(ha)->view() == static_cast<JSString*>(hb)->view()
                   ? iso->true_value() : iso->false_value();
        default:
            // Reference equality for objects.
            return ha == hb ? iso->true_value() : iso->false_value();
    }
}

Value LooseEquals(Isolate* iso, Value a, Value b) {
    // If same type, use strict equality.
    if (a.IsSmi() == b.IsSmi() &&
        (!a.IsHeapObject() ||
         a.AsHeapObject()->kind() == b.AsHeapObject()->kind())) {
        return StrictEquals(iso, a, b);
    }
    // null == undefined
    if (a.IsNullOrUndefined() && b.IsNullOrUndefined()) return iso->true_value();
    // Number vs String: coerce string to number.
    if (a.IsNumber() && b.IsString()) {
        return ToNumber(iso, a) == ToNumber(iso, b) ? iso->true_value() : iso->false_value();
    }
    if (a.IsString() && b.IsNumber()) {
        return ToNumber(iso, a) == ToNumber(iso, b) ? iso->true_value() : iso->false_value();
    }
    // Boolean vs non-boolean: coerce boolean to number.
    if (a.IsBoolean()) return LooseEquals(iso, ToNumber(iso, a), b);
    if (b.IsBoolean()) return LooseEquals(iso, a, ToNumber(iso, b));
    // Number/String vs Object: object to primitive (we approximate with ToNumber).
    if ((a.IsNumber() || a.IsString()) && b.IsObject()) {
        // For now, objects never loosely-equal primitives (except via reference).
        return iso->false_value();
    }
    if (a.IsObject() && (b.IsNumber() || b.IsString())) {
        return iso->false_value();
    }
    return iso->false_value();
}

// Helper for relational comparison: returns -1, 0, 1, or NaN (for "undefined").
static int CompareNumbers(double x, double y, bool* is_nan) {
    if (std::isnan(x) || std::isnan(y)) {
        *is_nan = true;
        return 0;
    }
    *is_nan = false;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

Value LessThan(Isolate* iso, Value a, Value b) {
    // If both strings, lexicographic comparison.
    if (a.IsString() && b.IsString()) {
        std::string_view sa = static_cast<JSString*>(a.AsHeapObject())->view();
        std::string_view sb = static_cast<JSString*>(b.AsHeapObject())->view();
        return sa < sb ? iso->true_value() : iso->false_value();
    }
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    bool nan;
    int c = CompareNumbers(x, y, &nan);
    if (nan) return iso->false_value();
    return c < 0 ? iso->true_value() : iso->false_value();
}

Value GreaterThan(Isolate* iso, Value a, Value b) {
    if (a.IsString() && b.IsString()) {
        std::string_view sa = static_cast<JSString*>(a.AsHeapObject())->view();
        std::string_view sb = static_cast<JSString*>(b.AsHeapObject())->view();
        return sa > sb ? iso->true_value() : iso->false_value();
    }
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    bool nan;
    int c = CompareNumbers(x, y, &nan);
    if (nan) return iso->false_value();
    return c > 0 ? iso->true_value() : iso->false_value();
}

Value LessThanOrEqual(Isolate* iso, Value a, Value b) {
    if (a.IsString() && b.IsString()) {
        std::string_view sa = static_cast<JSString*>(a.AsHeapObject())->view();
        std::string_view sb = static_cast<JSString*>(b.AsHeapObject())->view();
        return sa <= sb ? iso->true_value() : iso->false_value();
    }
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    bool nan;
    int c = CompareNumbers(x, y, &nan);
    if (nan) return iso->false_value();
    return c <= 0 ? iso->true_value() : iso->false_value();
}

Value GreaterThanOrEqual(Isolate* iso, Value a, Value b) {
    if (a.IsString() && b.IsString()) {
        std::string_view sa = static_cast<JSString*>(a.AsHeapObject())->view();
        std::string_view sb = static_cast<JSString*>(b.AsHeapObject())->view();
        return sa >= sb ? iso->true_value() : iso->false_value();
    }
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    bool nan;
    int c = CompareNumbers(x, y, &nan);
    if (nan) return iso->false_value();
    return c >= 0 ? iso->true_value() : iso->false_value();
}

// -----------------------------------------------------------------------------
// Arithmetic
// -----------------------------------------------------------------------------
Value Add(Isolate* iso, Value a, Value b) {
    // String concatenation takes priority if either side is a string.
    // Use ConsString for O(1) concatenation instead of O(n) copy.
    if (a.IsString() || b.IsString()) {
        Value sa = ToString(iso, a);
        Value sb = ToString(iso, b);
        // If both are flat JSStrings, use ConsString (O(1) instead of O(n)).
        // If either is already a ConsString, we could chain it, but for
        // simplicity we just flatten and re-cons.
        JSString* sa_flat = sa.AsHeapObject()->kind() == HeapObjectKind::kConsString
                            ? static_cast<ConsString*>(sa.AsHeapObject())->Flatten(iso)
                            : static_cast<JSString*>(sa.AsHeapObject());
        JSString* sb_flat = sb.AsHeapObject()->kind() == HeapObjectKind::kConsString
                            ? static_cast<ConsString*>(sb.AsHeapObject())->Flatten(iso)
                            : static_cast<JSString*>(sb.AsHeapObject());
        // For very small strings, just copy (ConsString overhead not worth it).
        if (sa_flat->length() + sb_flat->length() < 16) {
            std::string out;
            out.reserve(sa_flat->length() + sb_flat->length());
            out += std::string(sa_flat->data(), sa_flat->length());
            out += std::string(sb_flat->data(), sb_flat->length());
            return Value::FromHeap(JSString::New(iso, out));
        }
        return Value::FromHeap(ConsString::New(iso, sa_flat, sb_flat));
    }
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    return NumberFromDouble(iso, x + y);
}

Value Sub(Isolate* iso, Value a, Value b) {
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    return NumberFromDouble(iso, x - y);
}
Value Mul(Isolate* iso, Value a, Value b) {
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    return NumberFromDouble(iso, x * y);
}
Value Div(Isolate* iso, Value a, Value b) {
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    return NumberFromDouble(iso, x / y);
}
Value Mod(Isolate* iso, Value a, Value b) {
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    if (std::isnan(x) || std::isnan(y) || std::isinf(x) || y == 0.0) {
        return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    }
    if (std::isinf(y) || x == 0.0) {
        return NumberFromDouble(iso, x);
    }
    // JS % uses truncated division (fmod), not floored.
    double r = std::fmod(x, y);
    return NumberFromDouble(iso, r);
}
Value Exp(Isolate* iso, Value a, Value b) {
    double x = ToDouble(iso, a);
    double y = ToDouble(iso, b);
    return NumberFromDouble(iso, std::pow(x, y));
}

// -----------------------------------------------------------------------------
// Bitwise ops (ToInt32 / ToUint32)
// -----------------------------------------------------------------------------
int32_t ToInt32(Isolate* iso, Value v) {
    double d = ToDouble(iso, v);
    if (!std::isfinite(d)) return 0;
    // ToInt32: take floor (toward zero), then mod 2^32, then interpret as signed.
    double t = std::trunc(d);
    constexpr double two32 = 4294967296.0;
    t = std::fmod(t, two32);  // -2^32 < t < 2^32
    if (t < 0) t += two32;    // 0 <= t < 2^32
    if (t >= 2147483648.0) t -= two32;  // signed wrap
    return static_cast<int32_t>(t);
}

uint32_t ToUint32(Isolate* iso, Value v) {
    return static_cast<uint32_t>(ToInt32(iso, v));
}

Value BitOr(Isolate* iso, Value a, Value b) {
    return Value::FromSmi(static_cast<intptr_t>(
        static_cast<uint32_t>(ToInt32(iso, a) | ToInt32(iso, b))));
}
Value BitAnd(Isolate* iso, Value a, Value b) {
    return Value::FromSmi(static_cast<intptr_t>(
        static_cast<uint32_t>(ToInt32(iso, a) & ToInt32(iso, b))));
}
Value BitXor(Isolate* iso, Value a, Value b) {
    return Value::FromSmi(static_cast<intptr_t>(
        static_cast<uint32_t>(ToInt32(iso, a) ^ ToInt32(iso, b))));
}
Value Shl(Isolate* iso, Value a, Value b) {
    uint32_t x = ToUint32(iso, a);
    uint32_t s = ToUint32(iso, b) & 0x1F;
    return Value::FromSmi(static_cast<intptr_t>(x << s));
}
Value Shr(Isolate* iso, Value a, Value b) {
    int32_t x = ToInt32(iso, a);
    uint32_t s = ToUint32(iso, b) & 0x1F;
    return Value::FromSmi(static_cast<intptr_t>(x >> s));
}
Value Ushr(Isolate* iso, Value a, Value b) {
    uint32_t x = ToUint32(iso, a);
    uint32_t s = ToUint32(iso, b) & 0x1F;
    return Value::FromSmi(static_cast<intptr_t>(x >> s));
}

// -----------------------------------------------------------------------------
// Unary ops
// -----------------------------------------------------------------------------
Value Negate(Isolate* iso, Value v) {
    if (v.IsSmi()) {
        intptr_t x = v.AsSmi();
        // Negation may overflow Smi range (e.g. -INT64_MIN/2). Fall back to double.
        if (x == kSmiMin) {
            return Value::FromHeap(JSNumber::New(iso, -static_cast<double>(x)));
        }
        return Value::FromSmi(-x);
    }
    double d = ToDouble(iso, v);
    return NumberFromDouble(iso, -d);
}

Value BitNot(Isolate* iso, Value v) {
    int32_t x = ToInt32(iso, v);
    return Value::FromSmi(static_cast<intptr_t>(static_cast<uint32_t>(~x)));
}

Value Inc(Isolate* iso, Value v) {
    return Add(iso, v, Value::FromSmi(1));
}
Value Dec(Isolate* iso, Value v) {
    return Sub(iso, v, Value::FromSmi(1));
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
double ToDouble(Isolate* iso, Value v) {
    if (v.IsSmi()) return static_cast<double>(v.AsSmi());
    if (v.IsHeapObject()) {
        HeapObject* h = v.AsHeapObject();
        switch (h->kind()) {
            case HeapObjectKind::kNumber:
                return static_cast<JSNumber*>(h)->value();
            case HeapObjectKind::kBoolean:
                return static_cast<JSBoolean*>(h)->value() ? 1.0 : 0.0;
            case HeapObjectKind::kUndefined:
                return std::numeric_limits<double>::quiet_NaN();
            case HeapObjectKind::kNull:
                return 0.0;
            case HeapObjectKind::kString:
                return StringToNumber(static_cast<JSString*>(h)->view());
            default:
                return std::numeric_limits<double>::quiet_NaN();
        }
    }
    (void)iso;
    return std::numeric_limits<double>::quiet_NaN();
}

Value FromDouble(Isolate* iso, double d) {
    return NumberFromDouble(iso, d);
}

bool IsTruthy(Isolate* iso, Value v) {
    return ToBoolean(iso, v).AsHeapObject() == iso->true_object();
}

}  // namespace v12
