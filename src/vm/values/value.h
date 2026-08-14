// =============================================================================
// src/vm/values/value.h
// =============================================================================
// V12 value representation.
//
// At the VM level we use TaggedValue (Smi or HeapObject*) as our on-stack
// representation. This header adds the high-level Value type and a bunch of
// type-tests.
//
// Heap object layout:
//   Every HeapObject starts with a HeapObjectHeader containing:
//     - The Shape* (a pointer to the layout descriptor)
//     - The GC mark bits (in the low bits of the shape pointer)
//
// This is similar to V8's "Map word". Using a Shape (hidden class) lets us:
//   - Inline-cache property accesses by Shape identity.
//   - Share property layouts across instances of the same kind.
//   - Deoptimize JIT code when a Shape changes.
//
// Note: HeapObject is a forward declaration here; concrete object kinds
// (JSObject, JSArray, JSString, JSFunction, ...) are defined in objects/.

#ifndef V12_VM_VALUES_VALUE_H_
#define V12_VM_VALUES_VALUE_H_

#include <cstdint>

#include "base/macros.h"
#include "base/tagged-value.h"

namespace v12 {

class HeapObject;
class Shape;
class JSObject;
class JSArray;
class JSString;
class JSFunction;
class JSNumber;
class JSBoolean;
class JSUndefined;
class JSNull;
class HostFunction;

// A Value is just a TaggedValue with high-level helpers.
class Value {
public:
    Value() : raw_(TaggedValue::Smi(0)) {}
    Value(TaggedValue v) : raw_(v) {}

    static Value FromSmi(intptr_t i) { return Value(TaggedValue::Smi(i)); }
    static Value FromHeap(HeapObject* h) { return Value(TaggedValue::Heap(h)); }

    bool IsSmi() const { return raw_.IsSmi(); }
    bool IsHeapObject() const { return raw_.IsHeapObject(); }

    // NOTE: The Is*() type-check functions are defined inline AFTER the
    // HeapObject class declaration (at the bottom of this file) because
    // they need to call HeapObject::kind(), which requires the full
    // HeapObject definition. See the "Inline type checks" section below.

    bool IsNumber() const;
    bool IsString() const;
    bool IsObject() const;
    bool IsArray() const;
    bool IsFunction() const;       // JSFunction or HostFunction
    bool IsHostFunction() const;
    bool IsBoolean() const;
    bool IsUndefined() const;
    bool IsNull() const;
    bool IsNullOrUndefined() const { return IsNull() || IsUndefined(); }

    intptr_t AsSmi() const { return raw_.AsSmi(); }
    HeapObject* AsHeapObject() const { return raw_.AsHeapObject(); }

    // These cast functions remain non-inline (they're cold paths with DCHECKs).
    double AsNumber() const;
    JSString* AsString() const;
    JSObject* AsObject() const;
    JSArray* AsArray() const;
    JSFunction* AsFunction() const;
    HostFunction* AsHostFunction() const;

    TaggedValue raw() const { return raw_; }

    bool operator==(Value other) const { return raw_.raw_bits() == other.raw_.raw_bits(); }
    bool operator!=(Value other) const { return !(*this == other); }

private:
    TaggedValue raw_;
};

// HeapObject kind enum. Stored in the Shape.
enum class HeapObjectKind : uint8_t {
    kObject,
    kArray,
    kString,
    kFunction,
    kBoundFunction,
    kNumber,       // boxed HeapNumber
    kBoolean,      // boxed boolean
    kUndefined,
    kNull,
    kSymbol,
    kMap,
    kSet,
    kPromise,
    kError,
    kArrayBuffer,
    kTypedArray,
    kWeakRef,
    kExternal,     // for FFI / host objects
};

const char* HeapObjectKindName(HeapObjectKind kind);

// HeapObject header - first word of every heap object.
// We don't use C++ virtual functions; instead we dispatch on the
// HeapObjectKind stored directly in the header.
//
// Layout:
//   [ Shape*   | mark_bits ]   <- pointer (mark_bits in low 2 bits)
//   [ HeapObjectKind kind_ ]   <- cached kind (1 byte, no dereference)
//   [ ... padding ...        ]
//   [ ...object body...      ]
//
// The kind_ field is a cached copy of shape()->object_kind(). It is set
// by set_shape() and never changes after initialization. Caching it in
// the header means Value::IsString() / IsObject() / etc. can read the
// kind with a single byte load — no Shape pointer dereference needed.
// This is critical for interpreter performance: every type check in the
// dispatch loop becomes a single comparison instead of a function call
// + pointer dereference.
//
// mark_bits live in the low 2 bits of the shape pointer:
//   bit 0: GC mark
//   bit 1: remembered (in write barrier)
class HeapObject {
public:
    // Read the cached kind — INLINE, no pointer dereference.
    HeapObjectKind kind() const { return kind_; }
    // Read the shape pointer — INLINE (just masks off the mark bits).
    Shape* shape() const {
        return reinterpret_cast<Shape*>(shape_and_mark_ & ~uintptr_t{3});
    }

    // Set the shape. Non-inline because it calls Shape::object_kind()
    // (defined in shape.h, which can't be included here due to circular
    // dependency). Also caches the kind in the header.
    void set_shape(Shape* s);

    // Tagged pointer to self.
    Value AsValue() { return Value::FromHeap(this); }

    // Convenience casts - these check the kind in debug builds.
    JSObject* AsObject();
    JSArray* AsArray();
    JSString* AsString();
    JSFunction* AsFunction();

private:
    uintptr_t shape_and_mark_;   // Shape* | mark_bits
    HeapObjectKind kind_;        // cached from shape()->object_kind()
};

// Note: sizeof(HeapObject) is now 2 pointers (16 bytes on 64-bit) due to
// the added kind_ field. This is a deliberate trade-off: 8 extra bytes per
// object in exchange for eliminating a pointer dereference on every type
// check, which is the single hottest operation in the interpreter.

// -----------------------------------------------------------------------------
// Inline type checks for Value.
//
// These are defined here (after HeapObject) because they call
// HeapObject::kind(), which is an inline method that reads the cached
// kind_ byte from the header. The whole chain is:
//   Value::IsString() -> raw_.AsHeapObject()->kind() -> compare byte
// No function calls, no Shape pointer dereference. This is the single
// hottest path in the interpreter dispatch loop.
// -----------------------------------------------------------------------------
inline bool Value::IsNumber() const {
    if (raw_.IsSmi()) return true;
    return IsHeapObject() && raw_.AsHeapObject()->kind() == HeapObjectKind::kNumber;
}
inline bool Value::IsString() const {
    return IsHeapObject() && raw_.AsHeapObject()->kind() == HeapObjectKind::kString;
}
inline bool Value::IsObject() const {
    return IsHeapObject() && raw_.AsHeapObject()->kind() == HeapObjectKind::kObject;
}
inline bool Value::IsArray() const {
    return IsHeapObject() && raw_.AsHeapObject()->kind() == HeapObjectKind::kArray;
}
inline bool Value::IsFunction() const {
    if (!IsHeapObject()) return false;
    HeapObjectKind k = raw_.AsHeapObject()->kind();
    return k == HeapObjectKind::kFunction || k == HeapObjectKind::kExternal;
}
inline bool Value::IsHostFunction() const {
    return IsHeapObject() && raw_.AsHeapObject()->kind() == HeapObjectKind::kExternal;
}
inline bool Value::IsBoolean() const {
    return IsHeapObject() && raw_.AsHeapObject()->kind() == HeapObjectKind::kBoolean;
}
inline bool Value::IsUndefined() const {
    return IsHeapObject() && raw_.AsHeapObject()->kind() == HeapObjectKind::kUndefined;
}
inline bool Value::IsNull() const {
    return IsHeapObject() && raw_.AsHeapObject()->kind() == HeapObjectKind::kNull;
}

}  // namespace v12

#endif  // V12_VM_VALUES_VALUE_H_
