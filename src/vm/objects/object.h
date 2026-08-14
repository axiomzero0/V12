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
//   - JSString:   immutable string (we have a few specialized layouts)
//   - JSFunction: closure with shared FunctionInfo
//   - JSNumber, JSBoolean, JSUndefined, JSNull: boxed primitives
//
// Why not C++ inheritance?
//   We deliberately avoid `class JSObject : public HeapObject` because:
//   1. It adds a vtable pointer, which we don't want (we use Shape dispatch).
//   2. It makes the layout non-POD, which complicates GC.
//   3. We want to allocate objects with exact size and alignment.
//
// Instead, every object is a struct starting with HeapObject header, and we
// use static_cast / reinterpret_cast between the header and the specific
// kind. The Shape's object_kind() tells us which kind to cast to.

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

// JSObject - the basic object type.
// Layout in memory:
//   [ HeapObject header ]
//   [ Value prop0       ]
//   [ Value prop1       ]
//   [ ...               ]
// Properties are accessed by slot index from the Shape.
class JSObject : public HeapObject {
public:
    static JSObject* New(Isolate* iso, Shape* initial_shape = nullptr);

    Value GetProperty(Isolate* iso, std::string_view name);
    Value GetPropertyBySlot(Shape::Slot slot) {
        V12_DCHECK(slot < shape()->property_count(), "slot out of range");
        return slots()[slot];
    }

    void SetProperty(Isolate* iso, std::string_view name, Value value);
    void SetPropertyBySlot(Shape::Slot slot, Value value) {
        V12_DCHECK(slot < shape()->property_count(), "slot out of range");
        slots()[slot] = value;
    }

    bool HasProperty(std::string_view name) const;

    Value* slots() { return reinterpret_cast<Value*>(this + 1); }
    const Value* slots() const { return reinterpret_cast<const Value*>(this + 1); }

    // Direct slot access for the JIT.
    Value* slot_ptr(Shape::Slot slot) {
        return reinterpret_cast<Value*>(reinterpret_cast<char*>(this) + sizeof(JSObject))
               + slot;
    }
};

// JSArray - has indexed elements + length.
// Layout:
//   [ HeapObject header ]
//   [ Value* elements   ]   <-- pointer to elements storage
//   [ uint32_t length   ]
//   [ uint32_t capacity ]
class JSArray : public HeapObject {
public:
    static JSArray* New(Isolate* iso, uint32_t initial_capacity = 4);

    uint32_t length() const { return length_; }
    uint32_t capacity() const { return capacity_; }

    Value GetElement(uint32_t index) const {
        if (V12_UNLIKELY(index >= length_)) return Value::FromSmi(0);  // undefined
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

// Boxed primitives - HeapNumber, HeapBoolean, HeapUndefined, HeapNull.
// We use singletons for Undefined and Null (like V8 does).
class JSNumber : public HeapObject {
public:
    static JSNumber* New(Isolate* iso, double value);
    double value() const { return value_; }
private:
    double value_;
};

class JSBoolean : public HeapObject {
public:
    static JSBoolean* True(Isolate* iso);
    static JSBoolean* False(Isolate* iso);
    bool value() const { return value_; }
private:
    bool value_;
};

// Singletons live in the Isolate.
JSUndefined* GetUndefined(Isolate* iso);
JSNull* GetNull(Isolate* iso);

}  // namespace v12

#endif  // V12_VM_OBJECTS_OBJECT_H_
