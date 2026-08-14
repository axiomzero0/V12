// =============================================================================
// src/vm/objects/primitives.h
// =============================================================================
// Singleton primitive heap objects: Undefined, Null, True, False.
//
// These are special: there is exactly one of each per Isolate. They are
// allocated during Isolate::InitializeRoots() and never freed (they live
// for the lifetime of the isolate).
//
// We expose them as HeapObjects with a dedicated shape so that
// HeapObject::kind() returns the right HeapObjectKind. This means the
// fast type-tests in value.cc (Value::IsUndefined, IsNull, IsBoolean)
// all reduce to a shape-kind comparison — no vtable, no string compare.
//
// Layout (all four):
//   [ HeapObject header ]
//   [ bool value_       ]   (only meaningful for booleans)
//
// The shape's object_kind() distinguishes them; the body is identical
// for Undefined/Null (no body at all) and identical for True/False
// (one bool).

#ifndef V12_VM_OBJECTS_PRIMITIVES_H_
#define V12_VM_OBJECTS_PRIMITIVES_H_

#include <cstdint>

#include "vm/values/value.h"

namespace v12 {

class Isolate;
class Shape;

class JSUndefined : public HeapObject {
public:
    static JSUndefined* New(Isolate* iso);
};

class JSNull : public HeapObject {
public:
    static JSNull* New(Isolate* iso);
};

class JSBoolean : public HeapObject {
public:
    static JSBoolean* New(Isolate* iso, bool value);
    bool value() const { return value_; }
private:
    bool value_;
};

}  // namespace v12

#endif  // V12_VM_OBJECTS_PRIMITIVES_H_
