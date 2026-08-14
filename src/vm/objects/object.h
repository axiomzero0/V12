// =============================================================================
// src/vm/objects/object.h
// =============================================================================
// Concrete heap object types.
//
// We define each kind as a struct whose first member is the HeapObject header.
// This lets us reinterpret_cast between HeapObject* and the specific kind.
//
// Object kinds:
//   - JSObject:   plain { key: value }
//   - JSArray:    [] with indexed elements + length
//   - JSString:   immutable string
//   - JSFunction: closure with shared FunctionInfo
//   - JSNumber:   boxed HeapNumber (for non-Smi doubles)
//   - JSBoolean/JSUndefined/JSNull: see primitives.h
//   - HostFunction: a function implemented in C++ (e.g. print)
//
// Why not C++ inheritance?
//   We deliberately avoid `class JSObject : public HeapObject` with virtual
//   functions because:
//   1. It adds a vtable pointer, which we don't want (we use Shape dispatch).
//   2. It makes the layout non-POD, which complicates GC.
//   3. We want to allocate objects with exact size and alignment.
//
// Property storage:
//   JSObject stores its property slots in a separately-allocated backing
//   array (Value* properties_). This decouples the object header from the
//   property storage, so:
//     - The header has a fixed size regardless of property count.
//     - When a shape transition adds a slot, we only reallocate the
//       (small) properties array — the object itself does not move.
//       This fixes the long-standing "SetProperty doesn't update the
//       receiver pointer" bug.
//     - The JIT can still load properties directly: slot_ptr(slot) just
//       returns properties_ + slot.
//   This matches V8's "fast properties" model (out-of-line backing store).

#ifndef V12_VM_OBJECTS_OBJECT_H_
#define V12_VM_OBJECTS_OBJECT_H_

#include <cstdint>
#include <string_view>

#include "base/macros.h"
#include "base/small-vector.h"
#include "vm/shapes/shape.h"
#include "vm/values/value.h"

namespace v12 {

class FunctionInfo;
class Isolate;
class Context;
class Interp;

// JSObject - the basic object type.
// Layout:
//   [ HeapObject header ]
//   [ Value* properties_ ]   <-- backing store for property slots
//   [ uint16_t capacity_ ]   <-- current backing-store capacity (in Values)
class JSObject : public HeapObject {
public:
    static JSObject* New(Isolate* iso, Shape* initial_shape = nullptr);

    Value GetProperty(Isolate* iso, std::string_view name);
    Value GetPropertyBySlot(Shape::Slot slot) {
        V12_DCHECK(slot < shape()->property_count(), "slot out of range");
        return properties_[slot];
    }

    // Set `name` to `value`. Triggers a shape transition if `name` is new
    // to this object's shape; the properties backing store is reallocated
    // to fit the new slot. The object header does not move.
    void SetProperty(Isolate* iso, std::string_view name, Value value);

    void SetPropertyBySlot(Shape::Slot slot, Value value) {
        V12_DCHECK(slot < shape()->property_count(), "slot out of range");
        properties_[slot] = value;
    }

    bool HasProperty(std::string_view name) const;

    Value* properties() { return properties_; }
    const Value* properties() const { return properties_; }

    // Direct slot access for the JIT.
    Value* slot_ptr(Shape::Slot slot) { return properties_ + slot; }

    uint16_t property_capacity() const { return capacity_; }

private:
    Value* properties_;
    uint16_t capacity_;   // in Values; equals shape()->property_count() after New/SetProperty

    // Grow the properties_ backing store to at least `min_slots` slots.
    // Existing slot values are preserved; new slots are filled with undefined.
    void GrowProperties(Isolate* iso, uint16_t min_slots);
};

// JSArray - has indexed elements + length.
// Layout:
//   [ HeapObject header ]
//   [ Value* elements   ]
//   [ uint32_t length   ]
//   [ uint32_t capacity ]
class JSArray : public HeapObject {
public:
    static JSArray* New(Isolate* iso, uint32_t initial_capacity = 4);

    uint32_t length() const { return length_; }
    uint32_t capacity() const { return capacity_; }

    Value GetElement(uint32_t index) const {
        if (V12_UNLIKELY(index >= length_)) return Value::FromSmi(0);  // undefined sentinel
        return elements_[index];
    }

    void SetElement(Isolate* iso, uint32_t index, Value value);
    void Push(Isolate* iso, Value value);

    Value* elements() { return elements_; }
    const Value* elements() const { return elements_; }

    void EnsureCapacity(Isolate* iso, uint32_t needed);

private:
    Value* elements_;
    uint32_t length_;
    uint32_t capacity_;
};

// JSString - immutable string. We have a few storage strategies:
//   - Inline strings (small strings stored in the object)
//   - Cons strings (rope of two strings)
//   - External strings (backing buffer owned elsewhere)
//
// For now we just use a length + char data layout.
class JSString : public HeapObject {
public:
    static JSString* New(Isolate* iso, std::string_view str);
    static JSString* NewFromSmi(Isolate* iso, intptr_t smi);
    static JSString* NewFromDouble(Isolate* iso, double value);

    uint32_t length() const { return length_; }
    const char* data() const {
        return reinterpret_cast<const char*>(this + 1);
    }
    std::string_view view() const {
        return std::string_view(data(), length_);
    }

private:
    uint32_t length_;
    // char data follows immediately after.
};

// JSFunction - closure. Captures the FunctionInfo and the defining Context.
// Layout:
//   [ HeapObject header           ]
//   [ FunctionInfo* shared_info   ]
//   [ Context* closure_context    ]
class JSFunction : public HeapObject {
public:
    static JSFunction* New(Isolate* iso, FunctionInfo* info, Context* context);

    FunctionInfo* shared_info() const { return shared_info_; }
    Context* closure_context() const { return closure_context_; }

private:
    FunctionInfo* shared_info_;
    Context* closure_context_;
};

// HostFunction - a function implemented in C++. The interpreter calls it
// when invoking a JSFunction whose shared_info is null (or whose kind
// marks it as host). Used to expose `print`, `Math.*`, etc.
//
// Layout:
//   [ HeapObject header           ]
//   [ HostFn  fn_                 ]   <-- C++ function pointer
//   [ int32_t builtin_id_         ]   <-- for diagnostics / fast dispatch
using HostFn = Value (*)(Interp* interp, Value this_val, Value* args, uint32_t argc);

class HostFunction : public HeapObject {
public:
    static HostFunction* New(Isolate* iso, HostFn fn, int32_t builtin_id);

    HostFn fn() const { return fn_; }
    int32_t builtin_id() const { return builtin_id_; }

private:
    HostFn fn_;
    int32_t builtin_id_;
};

// JSNumber - boxed HeapNumber (for doubles that don't fit in a Smi).
class JSNumber : public HeapObject {
public:
    static JSNumber* New(Isolate* iso, double value);
    double value() const { return value_; }
private:
    double value_;
};

}  // namespace v12

#endif  // V12_VM_OBJECTS_OBJECT_H_
