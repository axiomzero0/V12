// =============================================================================
// src/ir/types/type.h
// =============================================================================
// IR type system.
//
// The IR uses a type lattice for type propagation:
//
//                  Top (Any)
//                /  |    |  \
//            Number Object String Boolean ...
//            /  |   |
//          Int Float ...
//
// Types are used to:
//   - Drive representation selection (Int32 vs Float64 vs Tagged).
//   - Insert speculation guards (e.g. CheckSmi if type is Number|Undefined).
//   - Specialize operations (e.g. JSAdd -> Int32Add when both inputs are Int32).
//
// We use a simple bitset-based type representation because we have a small,
// fixed set of base types. This is similar to V8 TurboFan's early type
// system (before they moved to a more elaborate one).

#ifndef V12_IR_TYPES_TYPE_H_
#define V12_IR_TYPES_TYPE_H_

#include <cstdint>

#include "base/macros.h"

namespace v12 {

// Type bits
namespace type_bits {
constexpr uint32_t kNone      = 0;
constexpr uint32_t kSmi       = 1u << 0;     // small integer (tagged)
constexpr uint32_t kInt32     = 1u << 1;
constexpr uint32_t kUint32    = 1u << 2;
constexpr uint32_t kInt64     = 1u << 3;
constexpr uint32_t kFloat64   = 1u << 4;
constexpr uint32_t kNumber    = kSmi | kInt32 | kUint32 | kInt64 | kFloat64;
constexpr uint32_t kString    = 1u << 5;
constexpr uint32_t kBoolean   = 1u << 6;
constexpr uint32_t kNull      = 1u << 7;
constexpr uint32_t kUndefined = 1u << 8;
constexpr uint32_t kObject    = 1u << 9;       // heap object (non-string, non-number)
constexpr uint32_t kArray     = 1u << 10;
constexpr uint32_t kFunction  = 1u << 11;
constexpr uint32_t kSymbol    = 1u << 12;
constexpr uint32_t kHole      = 1u << 13;
constexpr uint32_t kBigInt    = 1u << 14;
constexpr uint32_t kAny       = 0xFFFFFFFFu;
}  // namespace type_bits

// A Type is just a bitset. We wrap it in a struct so we can add member
// functions and so it shows up nicely in a debugger.
struct Type {
    uint32_t bits;

    constexpr Type() : bits(type_bits::kNone) {}
    constexpr explicit Type(uint32_t b) : bits(b) {}

    static constexpr Type None()    { return Type(type_bits::kNone); }
    static constexpr Type Any()     { return Type(type_bits::kAny); }
    static constexpr Type Smi()     { return Type(type_bits::kSmi); }
    static constexpr Type Int32()   { return Type(type_bits::kInt32); }
    static constexpr Type Uint32()  { return Type(type_bits::kUint32); }
    static constexpr Type Int64()   { return Type(type_bits::kInt64); }
    static constexpr Type Float64() { return Type(type_bits::kFloat64); }
    static constexpr Type Number()  { return Type(type_bits::kNumber); }
    static constexpr Type String()  { return Type(type_bits::kString); }
    static constexpr Type Boolean() { return Type(type_bits::kBoolean); }
    static constexpr Type Null()    { return Type(type_bits::kNull); }
    static constexpr Type Undefined() { return Type(type_bits::kUndefined); }
    static constexpr Type Object()  { return Type(type_bits::kObject); }
    static constexpr Type Array()   { return Type(type_bits::kArray); }
    static constexpr Type Function() { return Type(type_bits::kFunction); }
    static constexpr Type Symbol()  { return Type(type_bits::kSymbol); }
    static constexpr Type Hole()    { return Type(type_bits::kHole); }

    bool IsNone() const { return bits == 0; }
    bool IsAny() const  { return bits == type_bits::kAny; }

    // "Is T" means: this type is a subset of T.
    bool Is(Type t) const { return (bits & ~t.bits) == 0; }

    // "Maybe T" means: this type and T have non-empty intersection.
    bool Maybe(Type t) const { return (bits & t.bits) != 0; }

    // Union / intersection
    Type Union(Type t) const { return Type(bits | t.bits); }
    Type Intersect(Type t) const { return Type(bits & t.bits); }

    bool operator==(Type t) const { return bits == t.bits; }
    bool operator!=(Type t) const { return bits != t.bits; }

    // Special predicates
    bool IsNumber() const  { return Is(Number()); }
    bool IsSmi() const     { return Is(Smi()); }
    bool IsInt32() const   { return Is(Int32()); }
    bool IsFloat64() const { return Is(Float64()); }
    bool IsString() const  { return Is(String()); }
    bool IsBoolean() const { return Is(Boolean()); }
    bool IsObject() const  { return Is(Object()); }
    bool IsNullOrUndefined() const {
        return Is(Null().Union(Undefined()));
    }
};

// Lattice operations for type propagation.
//   Top = Any
//   Bottom = None
//   Meet = Intersect
//   Join = Union
inline Type TypeLatticeTop() { return Type::Any(); }
inline Type TypeLatticeBottom() { return Type::None(); }
inline Type TypeMeet(Type a, Type b) { return a.Intersect(b); }
inline Type TypeJoin(Type a, Type b) { return a.Union(b); }

}  // namespace v12

#endif  // V12_IR_TYPES_TYPE_H_
