// =============================================================================
// src/vm/shapes/shape.cc
// =============================================================================

#include "vm/shapes/shape.h"

#include <cstdio>

#include "vm/isolate/isolate.h"

namespace v12 {

Shape::Shape(Isolate* iso, uint32_t id, Shape* parent,
             std::string_view added_name, Slot added_slot)
    : parent_(parent), id_(id) {
    if (parent != nullptr) {
        properties_ = parent->properties_;
        property_count_ = parent->property_count_;
        object_kind_ = parent->object_kind_;
    }
    if (!added_name.empty()) {
        PropertyEntry e;
        e.name = iso->Intern(added_name);
        e.slot = added_slot;
        properties_.push_back(e);
        property_count_ = static_cast<uint16_t>(properties_.size());
    }
}

Shape* Shape::Empty(Isolate* iso) {
    static Shape* empty = nullptr;
    if (empty == nullptr) {
        empty = new Shape(iso, iso->NextShapeId(), nullptr, {}, 0);
    }
    return empty;
}

Shape* Shape::NewWithKind(Isolate* iso, HeapObjectKind kind) {
    Shape* s = new Shape(iso, iso->NextShapeId(), nullptr, {}, 0);
    s->set_object_kind(kind);
    return s;
}

Shape* Shape::AddProperty(Isolate* iso, std::string_view name) {
    // Check if a transition already exists.
    if (Shape* existing = LookupTransition(name)) {
        return existing;
    }
    Slot new_slot = property_count_;
    Shape* child = new Shape(iso, iso->NextShapeId(), this, name, new_slot);
    std::string_view interned = iso->Intern(name);
    transitions_[interned] = child;
    return child;
}

Shape* Shape::LookupTransition(std::string_view name) const {
    // const_cast is safe because transitions_ is logically const here (we're
    // only reading), but our HashMap doesn't have a const find() that returns
    // const Value*. This is a known limitation of the HashMap API.
    Shape** found = const_cast<HashMap<std::string_view, Shape*>*>(&transitions_)->find(name);
    return found != nullptr ? *found : nullptr;
}

Shape::Slot Shape::Lookup(std::string_view name) const {
    // Property names in shapes are interned (same pointer for same string).
    // But `name` might not be interned, so we can't always use pointer
    // comparison. However, string_view comparison is still fast (size check
    // + memcmp). For hot paths like LoadGlobal, the interpreter should cache
    // the slot index instead of calling this every time.
    for (const auto& p : properties_) {
        if (p.name == name) return p.slot;
    }
    return kInvalidSlot;
}

Shape* Shape::ArrayShape(Isolate* iso) {
    static Shape* s = nullptr;
    if (s == nullptr) {
        s = new Shape(iso, iso->NextShapeId(), nullptr, {}, 0);
        s->is_array_shape_ = true;
        s->object_kind_ = HeapObjectKind::kArray;
    }
    return s;
}

Shape* Shape::FunctionShape(Isolate* iso) {
    static Shape* s = nullptr;
    if (s == nullptr) {
        s = new Shape(iso, iso->NextShapeId(), nullptr, {}, 0);
        s->is_function_shape_ = true;
        s->object_kind_ = HeapObjectKind::kFunction;
    }
    return s;
}

void Shape::Dump() const {
    std::fprintf(stderr, "Shape #%u (kind=%u, props=%u):\n", id_, 
                 static_cast<unsigned>(object_kind_), property_count_);
    for (const auto& p : properties_) {
        std::fprintf(stderr, "  slot %u: %.*s\n", p.slot,
                     static_cast<int>(p.name.size()), p.name.data());
    }
}

}  // namespace v12
