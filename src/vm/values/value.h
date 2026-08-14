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

// A Value is just a TaggedValue with high-level helpers.
class Value {
public:
    Value() : raw_(TaggedValue::Smi(0)) {}
    Value(TaggedValue v) : raw_(v) {}

    static Value FromSmi(intptr_t i) { return Value(TaggedValue::Smi(i)); }
    static Value FromHeap(HeapObject* h) { return Value(TaggedValue::Heap(h)); }

    bool IsSmi() const { return raw_.IsSmi(); }
    bool IsHeapObject() const { return raw_.IsHeapObject(); }

    bool IsNumber() const;      // Smi or HeapNumber
    bool IsString() const;
    bool IsObject() const;
    bool IsArray() const;
    bool IsFunction() const;
    bool IsBoolean() const;
    bool IsUndefined() const;
    bool IsNull() const;
    bool IsNullOrUndefined() const { return IsNull() || IsUndefined(); }

    intptr_t AsSmi() const { return raw_.AsSmi(); }
    HeapObject* AsHeapObject() const { return raw_.AsHeapObject(); }

    double AsNumber() const;
    JSString* AsString() const;
    JSObject* AsObject() const;
    JSArray* AsArray() const;
    JSFunction* AsFunction() const;

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
// We don't use C++ virtual functions; instead we dispatch on Shape*.
//
// Layout:
//   [ Shape*   | mark_bits ]   <- header (one pointer)
//   [ ...object body...      ]
//
// mark_bits live in the low 2 bits of the shape pointer:
//   bit 0: GC mark (white/grey/black encoding uses 2 bits)
//   bit 1: remembered (in write barrier)
//
// Shapes are aligned to 4 bytes so the low 2 bits are free.
class HeapObject {
public:
    HeapObjectKind kind() const;
    Shape* shape() const;

    // Set the shape. For internal use by the GC and the object model.
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
};

static_assert(sizeof(HeapObject) == sizeof(uintptr_t),
              "HeapObject header must be one pointer");

}  // namespace v12

#endif  // V12_VM_VALUES_VALUE_H_
