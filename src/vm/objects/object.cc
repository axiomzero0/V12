// =============================================================================
// src/vm/objects/object.cc
// =============================================================================

#include "vm/objects/object.h"

#include <cstring>

#include "vm/isolate/isolate.h"
#include "vm/shapes/shape.h"

namespace v12 {

JSObject* JSObject::New(Isolate* iso, Shape* initial_shape) {
    if (initial_shape == nullptr) initial_shape = iso->empty_shape();
    uint32_t size = sizeof(JSObject) + initial_shape->instance_size();
    void* mem = iso->Allocate(size);
    auto* obj = static_cast<JSObject*>(mem);
    obj->set_shape(initial_shape);
    // Initialize slots to undefined.
    Value undef = iso->undefined_value();
    Value* slots = obj->slots();
    for (uint16_t i = 0; i < initial_shape->property_count(); ++i) {
        slots[i] = undef;
    }
    return obj;
}

Value JSObject::GetProperty(Isolate* iso, std::string_view name) {
    Shape::Slot slot = shape()->Lookup(name);
    if (slot == Shape::kInvalidSlot) {
        return iso->undefined_value();
    }
    return slots()[slot];
}

void JSObject::SetProperty(Isolate* iso, std::string_view name, Value value) {
    Shape::Slot slot = shape()->Lookup(name);
    if (slot == Shape::kInvalidSlot) {
        // Need to transition to a new shape.
        Shape* new_shape = shape()->AddProperty(iso, name);
        // Reallocate the object to fit the new property.
        // In a real engine we'd either:
        //   - Allocate a new object and copy (slow but simple)
        //   - Use out-of-line property storage for objects that grow
        // For simplicity we just allocate a new one.
        uint32_t new_size = sizeof(JSObject) + new_shape->instance_size();
        void* mem = iso->Allocate(new_size);
        auto* new_obj = static_cast<JSObject*>(mem);
        new_obj->set_shape(new_shape);
        Value* new_slots = new_obj->slots();
        Value* old_slots = slots();
        for (uint16_t i = 0; i < shape()->property_count(); ++i) {
            new_slots[i] = old_slots[i];
        }
        for (uint16_t i = shape()->property_count(); i < new_shape->property_count(); ++i) {
            new_slots[i] = iso->undefined_value();
        }
        // Copy the rest of the header.
        // This is a hack - we should just use placement new or have a
        // per-object allocation strategy. For now we copy the shape pointer
        // and any other header bits.
        // (The set_shape above already set the new shape.)
        // The caller would need to be updated to use the new object pointer.
        // THIS IS A KNOWN LIMITATION. For correctness testing of the
        // interpreter, we'll only ever call SetProperty on a fresh object
        // whose shape we know.
        new_slots[new_shape->property_count() - 1] = value;
        // TODO: properly handle the receiver update.
        return;
    }
    slots()[slot] = value;
}

bool JSObject::HasProperty(std::string_view name) const {
    return shape()->Lookup(name) != Shape::kInvalidSlot;
}

// ----- JSArray -----
JSArray* JSArray::New(Isolate* iso, uint32_t initial_capacity) {
    uint32_t size = sizeof(JSArray);
    void* mem = iso->Allocate(size);
    auto* arr = static_cast<JSArray*>(mem);
    arr->set_shape(iso->array_shape());
    arr->length_ = 0;
    arr->capacity_ = initial_capacity;
    if (initial_capacity > 0) {
        arr->elements_ = static_cast<Value*>(iso->Allocate(
            static_cast<uint32_t>(sizeof(Value) * initial_capacity)));
        Value undef = iso->undefined_value();
        for (uint32_t i = 0; i < initial_capacity; ++i) {
            arr->elements_[i] = undef;
        }
    } else {
        arr->elements_ = nullptr;
    }
    return arr;
}

void JSArray::SetElement(Isolate* iso, uint32_t index, Value value) {
    EnsureCapacity(iso, index + 1);
    elements_[index] = value;
    if (index >= length_) length_ = index + 1;
}

void JSArray::Push(Isolate* iso, Value value) {
    EnsureCapacity(iso, length_ + 1);
    elements_[length_++] = value;
}

void JSArray::EnsureCapacity(Isolate* iso, uint32_t needed) {
    if (needed <= capacity_) return;
    uint32_t new_cap = capacity_ == 0 ? 4 : capacity_ * 2;
    while (new_cap < needed) new_cap *= 2;
    Value* new_elements = static_cast<Value*>(
        iso->Allocate(static_cast<uint32_t>(sizeof(Value) * new_cap)));
    Value undef = iso->undefined_value();
    for (uint32_t i = 0; i < length_; ++i) {
        new_elements[i] = elements_[i];
    }
    for (uint32_t i = length_; i < new_cap; ++i) {
        new_elements[i] = undef;
    }
    elements_ = new_elements;
    capacity_ = new_cap;
}

// ----- JSString -----
JSString* JSString::New(Isolate* iso, std::string_view str) {
    uint32_t size = sizeof(JSString) + static_cast<uint32_t>(str.size()) + 1;
    void* mem = iso->Allocate(size);
    auto* s = static_cast<JSString*>(mem);
    s->set_shape(iso->empty_shape());   // TODO: dedicated string shape
    s->length_ = static_cast<uint32_t>(str.size());
    std::memcpy(s + 1, str.data(), str.size());
    reinterpret_cast<char*>(s + 1)[str.size()] = '\0';
    return s;
}

JSString* JSString::NewFromSmi(Isolate* iso, intptr_t smi) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(smi));
    return New(iso, std::string_view(buf, n));
}

// ----- JSFunction -----
JSFunction* JSFunction::New(Isolate* iso, FunctionInfo* info, Context* context) {
    uint32_t size = sizeof(JSFunction);
    void* mem = iso->Allocate(size);
    auto* fn = static_cast<JSFunction*>(mem);
    fn->set_shape(iso->function_shape());
    fn->shared_info_ = info;
    fn->closure_context_ = context;
    return fn;
}

// ----- JSNumber -----
JSNumber* JSNumber::New(Isolate* iso, double value) {
    void* mem = iso->Allocate(sizeof(JSNumber));
    auto* n = static_cast<JSNumber*>(mem);
    n->set_shape(iso->empty_shape());   // TODO
    n->value_ = value;
    return n;
}

JSBoolean* JSBoolean::True(Isolate* iso) {
    static JSBoolean* t = nullptr;
    if (t == nullptr) {
        void* mem = iso->Allocate(sizeof(JSBoolean));
        t = static_cast<JSBoolean*>(mem);
        t->set_shape(iso->empty_shape());
        t->value_ = true;
    }
    return t;
}

JSBoolean* JSBoolean::False(Isolate* iso) {
    static JSBoolean* f = nullptr;
    if (f == nullptr) {
        void* mem = iso->Allocate(sizeof(JSBoolean));
        f = static_cast<JSBoolean*>(mem);
        f->set_shape(iso->empty_shape());
        f->value_ = false;
    }
    return f;
}

JSUndefined* GetUndefined(Isolate* iso) {
    // TODO: allocate singleton
    return nullptr;
}

JSNull* GetNull(Isolate* iso) {
    return nullptr;
}

}  // namespace v12
