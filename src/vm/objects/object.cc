// =============================================================================
// src/vm/objects/object.cc
// =============================================================================

#include "vm/objects/object.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "vm/isolate/isolate.h"
#include "vm/shapes/shape.h"

namespace v12 {

// ----- JSObject -----
JSObject* JSObject::New(Isolate* iso, Shape* initial_shape) {
    if (initial_shape == nullptr) initial_shape = iso->empty_shape();
    void* mem = iso->Allocate(sizeof(JSObject));
    auto* obj = static_cast<JSObject*>(mem);
    obj->set_shape(initial_shape);

    uint16_t slot_count = initial_shape->property_count();
    obj->capacity_ = slot_count;
    if (slot_count > 0) {
        obj->properties_ = static_cast<Value*>(
            iso->Allocate(static_cast<uint32_t>(sizeof(Value) * slot_count)));
        Value undef = iso->undefined_value();
        for (uint16_t i = 0; i < slot_count; ++i) {
            obj->properties_[i] = undef;
        }
    } else {
        obj->properties_ = nullptr;
    }
    return obj;
}

void JSObject::GrowProperties(Isolate* iso, uint16_t min_slots) {
    uint16_t new_cap = capacity_ == 0 ? 4 : capacity_;
    while (new_cap < min_slots) {
        new_cap = new_cap * 2;
    }
    if (new_cap == capacity_) return;

    Value* new_props = static_cast<Value*>(
        iso->Allocate(static_cast<uint32_t>(sizeof(Value) * new_cap)));
    Value undef = iso->undefined_value();
    // Copy existing slots.
    for (uint16_t i = 0; i < capacity_; ++i) {
        new_props[i] = properties_[i];
    }
    // Initialize new slots to undefined.
    for (uint16_t i = capacity_; i < new_cap; ++i) {
        new_props[i] = undef;
    }
    properties_ = new_props;
    capacity_ = new_cap;
}

Value JSObject::GetProperty(Isolate* iso, std::string_view name) {
    (void)iso;
    Shape::Slot slot = shape()->Lookup(name);
    if (slot == Shape::kInvalidSlot) {
        return iso->undefined_value();
    }
    return properties_[slot];
}

void JSObject::SetProperty(Isolate* iso, std::string_view name, Value value) {
    Shape* cur_shape = shape();
    Shape::Slot slot = cur_shape->Lookup(name);
    if (slot == Shape::kInvalidSlot) {
        // Need to transition to a new shape with the additional property.
        Shape* new_shape = cur_shape->AddProperty(iso, name);
        // Switch the object's shape first. The new shape has property_count
        // equal to (old_count + 1).
        set_shape(new_shape);
        uint16_t needed = new_shape->property_count();
        if (needed > capacity_) {
            GrowProperties(iso, needed);
        }
        // The new property lives at the slot index `needed - 1`.
        properties_[needed - 1] = value;
        return;
    }
    properties_[slot] = value;
}

bool JSObject::HasProperty(std::string_view name) const {
    return shape()->Lookup(name) != Shape::kInvalidSlot;
}

// ----- JSArray -----
JSArray* JSArray::New(Isolate* iso, uint32_t initial_capacity) {
    void* mem = iso->Allocate(sizeof(JSArray));
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
    s->set_shape(Shape::NewWithKind(iso, HeapObjectKind::kString));
    s->length_ = static_cast<uint32_t>(str.size());
    std::memcpy(s + 1, str.data(), str.size());
    reinterpret_cast<char*>(s + 1)[str.size()] = '\0';
    return s;
}

JSString* JSString::NewFromSmi(Isolate* iso, intptr_t smi) {
    char buf[32];
    int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(smi));
    return New(iso, std::string_view(buf, static_cast<size_t>(n)));
}

JSString* JSString::NewFromDouble(Isolate* iso, double value) {
    char buf[64];
    int n;
    if (std::isnan(value)) {
        n = std::snprintf(buf, sizeof(buf), "NaN");
    } else if (std::isinf(value)) {
        n = std::snprintf(buf, sizeof(buf), value > 0 ? "Infinity" : "-Infinity");
    } else if (value == std::floor(value) && std::abs(value) < 1e21) {
        n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
    } else {
        n = std::snprintf(buf, sizeof(buf), "%.17g", value);
    }
    return New(iso, std::string_view(buf, static_cast<size_t>(n)));
}

// ----- ConsString -----
ConsString* ConsString::New(Isolate* iso, JSString* left, JSString* right) {
    void* mem = iso->Allocate(sizeof(ConsString));
    auto* cs = static_cast<ConsString*>(mem);
    cs->set_shape(Shape::NewWithKind(iso, HeapObjectKind::kConsString));
    cs->length_ = left->length() + right->length();
    cs->left_ = left;
    cs->right_ = right;
    cs->flattened_ = false;
    cs->flat_ = nullptr;
    return cs;
}

JSString* ConsString::Flatten(Isolate* iso) {
    if (flattened_) return flat_;
    std::string out;
    out.reserve(length_);
    // Walk the cons tree. left_ and right_ are stored as JSString* but
    // may actually be ConsString* (both inherit HeapObject). We check
    // kind() to dispatch.
    auto walk = [&](HeapObject* h, auto& self) -> void {
        if (h == nullptr) return;
        if (h->kind() == HeapObjectKind::kConsString) {
            ConsString* cs = static_cast<ConsString*>(h);
            self(cs->left_, self);
            self(cs->right_, self);
        } else {
            JSString* s = static_cast<JSString*>(h);
            out += std::string(s->data(), s->length());
        }
    };
    walk(left_, walk);
    walk(right_, walk);
    flat_ = JSString::New(iso, out);
    flattened_ = true;
    return flat_;
}

// ----- JSFunction -----
JSFunction* JSFunction::New(Isolate* iso, FunctionInfo* info, Context* context) {
    void* mem = iso->Allocate(sizeof(JSFunction));
    auto* fn = static_cast<JSFunction*>(mem);
    fn->set_shape(iso->function_shape());
    fn->shared_info_ = info;
    fn->closure_context_ = context;
    return fn;
}

// ----- HostFunction -----
HostFunction* HostFunction::New(Isolate* iso, HostFn fn, int32_t builtin_id) {
    void* mem = iso->Allocate(sizeof(HostFunction));
    auto* h = static_cast<HostFunction*>(mem);
    h->set_shape(Shape::NewWithKind(iso, HeapObjectKind::kExternal));
    h->fn_ = fn;
    h->builtin_id_ = builtin_id;
    return h;
}

// ----- JSNumber -----
JSNumber* JSNumber::New(Isolate* iso, double value) {
    void* mem = iso->Allocate(sizeof(JSNumber));
    auto* n = static_cast<JSNumber*>(mem);
    n->set_shape(Shape::NewWithKind(iso, HeapObjectKind::kNumber));
    n->value_ = value;
    return n;
}

}  // namespace v12
