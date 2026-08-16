// =============================================================================
// src/vm/runtime/runtime.h
// =============================================================================
// Runtime helpers that implement JavaScript's abstract operations.
//
// The interpreter and bytecode generator both need to perform the same
// coercions defined by the JS spec: ToBoolean, ToNumber, ToString,
// ToObject, etc. Centralizing them here keeps the interpreter's dispatch
// loop small and the bytecode generator's constant-folding correct.
//
// References:
//   - ECMA-262 §7 (Type Conversion)
//   - ECMA-262 §13.5 (Addition operator — special string|number rules)
//
// Everything here is a free function taking an Isolate* (for heap
// allocation) and a Value (the input). The result is always a Value.

#ifndef V12_VM_RUNTIME_RUNTIME_H_
#define V12_VM_RUNTIME_RUNTIME_H_

#include <cmath>

#include "vm/objects/object.h"
#include "vm/objects/primitives.h"
#include "vm/values/value.h"

namespace v12 {

class Isolate;

// ----- Abstract operations (ECMA-262 §7) -----

// ToBoolean: undefined, null, false, 0, NaN, "" -> false; everything else -> true.
Value ToBoolean(Isolate* iso, Value v);

// ToNumber: Smi/HeapNumber pass through; true->1, false->0, null->0,
// undefined->NaN, strings parsed by string-to-number, objects throw
// (we approximate by calling their valueOf, but for now just return NaN).
Value ToNumber(Isolate* iso, Value v);

// ToString: Smi/HeapNumber formatted; strings pass through; true->"true",
// false->"false", undefined->"undefined", null->"null".
Value ToString(Isolate* iso, Value v);

// ToObject: wrap primitives in their object forms. For now we only need
// this for property access on primitives (e.g. "abc".length).
Value ToObject(Isolate* iso, Value v);

// Typeof: returns the JS typeof string as a JSString.
Value Typeof(Isolate* iso, Value v);

// ----- Comparison -----

// Strict equality (===): same type and same value. No coercion.
Value StrictEquals(Isolate* iso, Value a, Value b);

// Abstract equality (==): with coercion. ECMA-262 §7.2.13.
Value LooseEquals(Isolate* iso, Value a, Value b);

// Relational comparison (<, >, <=, >=). Returns true/false, or undefined
// (representing NaN) per spec — the interpreter turns undefined into false.
Value LessThan(Isolate* iso, Value a, Value b);
Value GreaterThan(Isolate* iso, Value a, Value b);
Value LessThanOrEqual(Isolate* iso, Value a, Value b);
Value GreaterThanOrEqual(Isolate* iso, Value a, Value b);

// ----- Arithmetic -----

// Binary addition (the + operator). If either operand is a string after
// ToPrimitive, performs string concatenation; otherwise numeric addition.
Value Add(Isolate* iso, Value a, Value b);

// Numeric binary ops. Both operands are coerced to Number first.
Value Sub(Isolate* iso, Value a, Value b);
Value Mul(Isolate* iso, Value a, Value b);
Value Div(Isolate* iso, Value a, Value b);
Value Mod(Isolate* iso, Value a, Value b);
Value Exp(Isolate* iso, Value a, Value b);

// Bitwise ops coerce to Int32, perform the op, return Smi if it fits.
Value BitOr(Isolate* iso, Value a, Value b);
Value BitAnd(Isolate* iso, Value a, Value b);
Value BitXor(Isolate* iso, Value a, Value b);
Value Shl(Isolate* iso, Value a, Value b);
Value Shr(Isolate* iso, Value a, Value b);    // signed >>
Value Ushr(Isolate* iso, Value a, Value b);   // unsigned >>>

// Unary ops.
Value Negate(Isolate* iso, Value v);
Value BitNot(Isolate* iso, Value v);

// Increment/decrement: same as `v + 1`.
Value Inc(Isolate* iso, Value v);
Value Dec(Isolate* iso, Value v);

// ----- Helpers -----

// Convert a Value to a C++ double (for internal arithmetic).
double ToDouble(Isolate* iso, Value v);

// Convert a Value to an Int32 (for bitwise ops). ECMA-262 §7.1.6.
int32_t ToInt32(Isolate* iso, Value v);

// Convert a Value to a Uint32 (for shifts). ECMA-262 §7.1.7.
uint32_t ToUint32(Isolate* iso, Value v);

// Box a double into either a Smi (if it fits) or a HeapNumber.
Value FromDouble(Isolate* iso, double d);

// ----- Inline Smi fast paths for the interpreter -----
// These handle the common case where both operands are Smis and the result
// fits in a Smi. They return true if they handled the operation (with the
// result in `out`); false if the caller should fall back to the slow path.
// Inlining avoids the function-call overhead of Add()/Sub()/etc. for the
// overwhelmingly common Smi case.

// Smi range check (from tagged-value.h, mirrored here for convenience).
inline bool SmiFitsFast(intptr_t v) {
    // Smis use 62 bits on 64-bit (1 tag bit + 63 value bits, but the top
    // bit is the sign, so 62 usable). This check is simplified.
    return v >= -(intptr_t{1} << 62) && v < (intptr_t{1} << 62);
}

// Try to add two Values as Smis. Returns true if both were Smis and the
// result fit (no overflow); false otherwise.
inline bool TrySmiAdd(Value a, Value b, Value* out) {
    if (V12_LIKELY(a.IsSmi() && b.IsSmi())) {
        intptr_t x = a.AsSmi();
        intptr_t y = b.AsSmi();
        // Use __builtin_add_overflow for robust overflow detection.
#if defined(V12_COMPILER_CLANG) || defined(V12_COMPILER_GCC)
        intptr_t sum;
        if (V12_LIKELY(!__builtin_add_overflow(x, y, &sum) && SmiFitsFast(sum))) {
            *out = Value::FromSmi(sum);
            return true;
        }
#else
        intptr_t sum = x + y;
        if (V12_LIKELY(SmiFitsFast(sum))) {
            *out = Value::FromSmi(sum);
            return true;
        }
#endif
    }
    return false;
}

inline bool TrySmiSub(Value a, Value b, Value* out) {
    if (V12_LIKELY(a.IsSmi() && b.IsSmi())) {
        intptr_t x = a.AsSmi();
        intptr_t y = b.AsSmi();
#if defined(V12_COMPILER_CLANG) || defined(V12_COMPILER_GCC)
        intptr_t diff;
        if (V12_LIKELY(!__builtin_sub_overflow(x, y, &diff) && SmiFitsFast(diff))) {
            *out = Value::FromSmi(diff);
            return true;
        }
#else
        intptr_t diff = x - y;
        if (V12_LIKELY(SmiFitsFast(diff))) {
            *out = Value::FromSmi(diff);
            return true;
        }
#endif
    }
    return false;
}

inline bool TrySmiMul(Value a, Value b, Value* out) {
    if (V12_LIKELY(a.IsSmi() && b.IsSmi())) {
        intptr_t x = a.AsSmi();
        intptr_t y = b.AsSmi();
#if defined(V12_COMPILER_CLANG) || defined(V12_COMPILER_GCC)
        intptr_t prod;
        if (V12_LIKELY(!__builtin_mul_overflow(x, y, &prod) && SmiFitsFast(prod))) {
            *out = Value::FromSmi(prod);
            return true;
        }
#else
        if (x == 0 || y == 0) { *out = Value::FromSmi(0); return true; }
        intptr_t prod = x * y;
        if (V12_LIKELY(prod / x == y && SmiFitsFast(prod))) {
            *out = Value::FromSmi(prod);
            return true;
        }
#endif
    }
    return false;
}

// Try to divide two Values as Smis. Returns true if both were Smis, b != 0,
// and a % b == 0 (i.e. the result is an exact integer that fits in a Smi).
// Otherwise returns false (caller falls back to the slow path which may
// produce a HeapNumber). This avoids heap allocation for the common case
// of integer division that divides evenly.
inline bool TrySmiDiv(Value a, Value b, Value* out) {
    if (V12_LIKELY(a.IsSmi() && b.IsSmi())) {
        intptr_t x = a.AsSmi();
        intptr_t y = b.AsSmi();
        if (V12_UNLIKELY(y == 0)) return false;  // division by zero → slow path (NaN/Infinity)
        intptr_t rem = x % y;
        if (rem != 0) return false;  // non-integer result → slow path (HeapNumber)
        // x / y is exact; no overflow possible for division.
        *out = Value::FromSmi(x / y);
        return true;
    }
    return false;
}

// Bitwise ops can't overflow (they're Int32), so they always succeed when
// both operands are Smis. But we still need to check for Smi-ness.
inline bool TrySmiBitOr(Value a, Value b, Value* out) {
    if (a.IsSmi() && b.IsSmi()) {
        *out = Value::FromSmi(a.AsSmi() | b.AsSmi());
        return true;
    }
    return false;
}
inline bool TrySmiBitAnd(Value a, Value b, Value* out) {
    if (a.IsSmi() && b.IsSmi()) {
        *out = Value::FromSmi(a.AsSmi() & b.AsSmi());
        return true;
    }
    return false;
}
inline bool TrySmiBitXor(Value a, Value b, Value* out) {
    if (a.IsSmi() && b.IsSmi()) {
        *out = Value::FromSmi(a.AsSmi() ^ b.AsSmi());
        return true;
    }
    return false;
}
inline bool TrySmiShl(Value a, Value b, Value* out) {
    if (a.IsSmi() && b.IsSmi()) {
        intptr_t shift = b.AsSmi() & 63;
        *out = Value::FromSmi(a.AsSmi() << shift);
        return true;
    }
    return false;
}
inline bool TrySmiShr(Value a, Value b, Value* out) {
    if (a.IsSmi() && b.IsSmi()) {
        intptr_t shift = b.AsSmi() & 63;
        *out = Value::FromSmi(a.AsSmi() >> shift);
        return true;
    }
    return false;
}

// Smi comparison fast paths. Return true and set `result` (a bool) if both
// operands are Smis. The caller converts the bool to a JS boolean Value.
// This avoids needing the Isolate (for true/false singletons) in the fast path.
inline bool TrySmiLessThan(Value a, Value b, bool* result) {
    if (a.IsSmi() && b.IsSmi()) {
        *result = a.AsSmi() < b.AsSmi();
        return true;
    }
    return false;
}
inline bool TrySmiGreaterThan(Value a, Value b, bool* result) {
    if (a.IsSmi() && b.IsSmi()) {
        *result = a.AsSmi() > b.AsSmi();
        return true;
    }
    return false;
}
inline bool TrySmiLessThanOrEqual(Value a, Value b, bool* result) {
    if (a.IsSmi() && b.IsSmi()) {
        *result = a.AsSmi() <= b.AsSmi();
        return true;
    }
    return false;
}
inline bool TrySmiGreaterThanOrEqual(Value a, Value b, bool* result) {
    if (a.IsSmi() && b.IsSmi()) {
        *result = a.AsSmi() >= b.AsSmi();
        return true;
    }
    return false;
}
inline bool TrySmiStrictEquals(Value a, Value b, bool* result) {
    if (a.IsSmi() && b.IsSmi()) {
        *result = a.AsSmi() == b.AsSmi();
        return true;
    }
    // If one is Smi and the other isn't, let the slow path handle it
    // (could be Smi vs HeapNumber — numerically equal but different types).
    return false;
}

// Returns true if the value is "truthy" per JS semantics.
// INLINE — used by every JumpIf* opcode handler (5 opcodes). Avoids a
// function call on the hot dispatch path.
inline bool IsTruthyFast(Value v) {
    if (v.IsSmi()) return v.AsSmi() != 0;
    if (v.IsHeapObject()) {
        HeapObject* h = v.AsHeapObject();
        switch (h->kind()) {
            case HeapObjectKind::kUndefined:
            case HeapObjectKind::kNull:
                return false;
            case HeapObjectKind::kBoolean:
                return static_cast<JSBoolean*>(h)->value();
            case HeapObjectKind::kNumber: {
                double d = static_cast<JSNumber*>(h)->value();
                return d != 0.0 && !std::isnan(d);
            }
            case HeapObjectKind::kString:
                return static_cast<JSString*>(h)->length() != 0;
            default:
                return true;
        }
    }
    return false;
}

// The original version (kept for external callers that pass Isolate).
bool IsTruthy(Isolate* iso, Value v);

// Flatten a Value that IsString() into a flat JSString*. If the value is
// already a flat JSString, returns it directly. If it's a ConsString,
// flattens it (materializes the full string) and returns the flat result.
// Call this before accessing ->data() or ->view() on a string.
inline JSString* FlattenString(Isolate* iso, Value v) {
    HeapObject* h = v.AsHeapObject();
    if (h->kind() == HeapObjectKind::kConsString) {
        return static_cast<ConsString*>(h)->Flatten(iso);
    }
    return static_cast<JSString*>(h);
}

}  // namespace v12

#endif  // V12_VM_RUNTIME_RUNTIME_H_
