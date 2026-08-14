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

// Returns true if the value is "truthy" per JS semantics.
bool IsTruthy(Isolate* iso, Value v);

}  // namespace v12

#endif  // V12_VM_RUNTIME_RUNTIME_H_
