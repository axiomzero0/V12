// =============================================================================
// tools/js-shell/builtins.cc
// =============================================================================
// Implementation of built-in host functions.

#include "tools/js-shell/builtins.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace v12 {

namespace {

// ----- print / console.log -----
Value HostPrint(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    for (uint32_t i = 0; i < argc; ++i) {
        if (i > 0) std::fputc(' ', stdout);
        Value s = ToString(iso, args[i]);
        if (s.IsString()) {
            auto* js = static_cast<JSString*>(s.AsHeapObject());
            std::fwrite(js->data(), 1, js->length(), stdout);
        }
    }
    std::fputc('\n', stdout);
    return iso->undefined_value();
}

// ----- parseInt(str, radix?) -----
Value HostParseInt(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return Value::FromSmi(0);
    Value s = ToString(iso, args[0]);
    int radix = 10;
    if (argc > 1) radix = static_cast<int>(ToDouble(iso, args[1]));
    if (radix == 0) radix = 10;
    if (!s.IsString()) return Value::FromSmi(0);
    auto* js = static_cast<JSString*>(s.AsHeapObject());
    std::string str(js->data(), js->length());
    try {
        long long v = std::stoll(str, nullptr, radix);
        return Value::FromSmi(static_cast<intptr_t>(v));
    } catch (...) {
        return Value::FromSmi(0);
    }
}

// ----- parseFloat(str) -----
Value HostParseFloat(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    Value s = ToString(iso, args[0]);
    if (!s.IsString()) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    auto* js = static_cast<JSString*>(s.AsHeapObject());
    std::string str(js->data(), js->length());
    try {
        double v = std::stod(str);
        return FromDouble(iso, v);
    } catch (...) {
        return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    }
}

// ----- isNaN(v) -----
Value HostIsNaN(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return iso->true_value();
    double v = ToDouble(iso, args[0]);
    return std::isnan(v) ? iso->true_value() : iso->false_value();
}

// ----- Array.isArray(v) -----
Value HostArrayIsArray(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return iso->false_value();
    return args[0].IsArray() ? iso->true_value() : iso->false_value();
}

// ----- Object.keys(obj) -----
Value HostObjectKeys(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    JSArray* arr = JSArray::New(iso, 4);
    if (argc > 0 && args[0].IsObject()) {
        JSObject* obj = args[0].AsObject();
        Shape* shape = obj->shape();
        for (uint16_t i = 0; i < shape->property_count(); ++i) {
            std::string_view name = shape->PropertyNameAt(i);
            Value key = Value::FromHeap(JSString::New(iso, name));
            arr->Push(iso, key);
        }
    } else if (argc > 0 && args[0].IsArray()) {
        JSArray* a = args[0].AsArray();
        for (uint32_t i = 0; i < a->length(); ++i) {
            Value key = Value::FromHeap(JSString::NewFromSmi(iso, static_cast<intptr_t>(i)));
            arr->Push(iso, key);
        }
    }
    return Value::FromHeap(arr);
}

// ----- Math object methods -----

Value MathAbs(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    return FromDouble(iso, std::abs(ToDouble(iso, args[0])));
}

Value MathFloor(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    return FromDouble(iso, std::floor(ToDouble(iso, args[0])));
}

Value MathCeil(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    return FromDouble(iso, std::ceil(ToDouble(iso, args[0])));
}

Value MathRound(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    double v = ToDouble(iso, args[0]);
    // JS round: round half up (0.5 -> 1, -0.5 -> 0)
    return FromDouble(iso, std::floor(v + 0.5));
}

Value MathSqrt(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    return FromDouble(iso, std::sqrt(ToDouble(iso, args[0])));
}

Value MathPow(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc < 2) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
    return FromDouble(iso, std::pow(ToDouble(iso, args[0]), ToDouble(iso, args[1])));
}

Value MathMin(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::infinity()));
    double result = ToDouble(iso, args[0]);
    for (uint32_t i = 1; i < argc; ++i) {
        double v = ToDouble(iso, args[i]);
        if (std::isnan(v)) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
        if (v < result) result = v;
    }
    return FromDouble(iso, result);
}

Value MathMax(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    if (argc == 0) return Value::FromHeap(JSNumber::New(iso, -std::numeric_limits<double>::infinity()));
    double result = ToDouble(iso, args[0]);
    for (uint32_t i = 1; i < argc; ++i) {
        double v = ToDouble(iso, args[i]);
        if (std::isnan(v)) return Value::FromHeap(JSNumber::New(iso, std::numeric_limits<double>::quiet_NaN()));
        if (v > result) result = v;
    }
    return FromDouble(iso, result);
}

Value MathRandom(Interp* interp, Value /*this_val*/, Value* /*args*/, uint32_t /*argc*/) {
    Isolate* iso = interp->isolate();
    // Simple LCG — not crypto-secure, but fine for a shell.
    static uint64_t state = 1;
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    double v = static_cast<double>(state >> 11) / static_cast<double>(1ULL << 53);
    return FromDouble(iso, v);
}

// ----- String.fromCharCode(n) -----
Value StringFromCharCode(Interp* interp, Value /*this_val*/, Value* args, uint32_t argc) {
    Isolate* iso = interp->isolate();
    std::string out;
    for (uint32_t i = 0; i < argc; ++i) {
        int code = static_cast<int>(ToDouble(iso, args[i]));
        if (code < 128) {
            out += static_cast<char>(code);
        } else {
            // UTF-8 encode (simplified)
            if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
        }
    }
    return Value::FromHeap(JSString::New(iso, out));
}

}  // namespace

void RegisterBuiltins(Isolate* iso) {
    // Global functions.
    iso->SetGlobal("print", Value::FromHeap(HostFunction::New(iso, HostPrint, 0)));
    iso->SetGlobal("println", Value::FromHeap(HostFunction::New(iso, HostPrint, 0)));
    iso->SetGlobal("parseInt", Value::FromHeap(HostFunction::New(iso, HostParseInt, 1)));
    iso->SetGlobal("parseFloat", Value::FromHeap(HostFunction::New(iso, HostParseFloat, 2)));
    iso->SetGlobal("isNaN", Value::FromHeap(HostFunction::New(iso, HostIsNaN, 3)));

    // console.log
    JSObject* console = JSObject::New(iso);
    console->SetProperty(iso, "log", Value::FromHeap(HostFunction::New(iso, HostPrint, 4)));
    iso->SetGlobal("console", Value::FromHeap(console));

    // Array
    JSObject* ArrayObj = JSObject::New(iso);
    ArrayObj->SetProperty(iso, "isArray", Value::FromHeap(HostFunction::New(iso, HostArrayIsArray, 5)));
    iso->SetGlobal("Array", Value::FromHeap(ArrayObj));

    // Object
    JSObject* ObjectObj = JSObject::New(iso);
    ObjectObj->SetProperty(iso, "keys", Value::FromHeap(HostFunction::New(iso, HostObjectKeys, 6)));
    iso->SetGlobal("Object", Value::FromHeap(ObjectObj));

    // String
    JSObject* StringObj = JSObject::New(iso);
    StringObj->SetProperty(iso, "fromCharCode", Value::FromHeap(HostFunction::New(iso, StringFromCharCode, 7)));
    iso->SetGlobal("String", Value::FromHeap(StringObj));

    // Math
    JSObject* math = JSObject::New(iso);
    math->SetProperty(iso, "abs", Value::FromHeap(HostFunction::New(iso, MathAbs, 10)));
    math->SetProperty(iso, "floor", Value::FromHeap(HostFunction::New(iso, MathFloor, 11)));
    math->SetProperty(iso, "ceil", Value::FromHeap(HostFunction::New(iso, MathCeil, 12)));
    math->SetProperty(iso, "round", Value::FromHeap(HostFunction::New(iso, MathRound, 13)));
    math->SetProperty(iso, "sqrt", Value::FromHeap(HostFunction::New(iso, MathSqrt, 14)));
    math->SetProperty(iso, "pow", Value::FromHeap(HostFunction::New(iso, MathPow, 15)));
    math->SetProperty(iso, "min", Value::FromHeap(HostFunction::New(iso, MathMin, 16)));
    math->SetProperty(iso, "max", Value::FromHeap(HostFunction::New(iso, MathMax, 17)));
    math->SetProperty(iso, "random", Value::FromHeap(HostFunction::New(iso, MathRandom, 18)));
    math->SetProperty(iso, "PI", Value::FromHeap(JSNumber::New(iso, 3.141592653589793)));
    math->SetProperty(iso, "E", Value::FromHeap(JSNumber::New(iso, 2.718281828459045)));
    iso->SetGlobal("Math", Value::FromHeap(math));
}

}  // namespace v12
