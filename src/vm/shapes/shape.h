// =============================================================================
// src/vm/shapes/shape.h
// =============================================================================
// Shapes (a.k.a. hidden classes).
//
// A Shape describes the layout of a JSObject:
//   - The set of property names
//   - The order in which they appear
//   - The slot offset of each property
//
// Two objects with the same property names in the same order share the same
// Shape. This is the basis of inline caches: a LoadIC checks that the
// receiver's Shape is the expected one; if so, the property offset is known
// at compile time.
//
// Shape transitions:
//   Adding a property to an object creates a new Shape that extends the old
//   one. We cache these transitions so that adding property "foo" to shape
//   S always yields the same shape S' (assuming the same property order).
//   This is called "shape tree".
//
// GC interaction:
//   Shapes are themselves heap-allocated. They are immutable after creation.
//   The GC treats them as roots-ish: they're rarely freed because they're
//   shared across many objects.

#ifndef V12_VM_SHAPES_SHAPE_H_
#define V12_VM_SHAPES_SHAPE_H_

#include <cstdint>
#include <string_view>

#include "base/hash-map.h"
#include "base/macros.h"
#include "base/small-vector.h"
#include "vm/values/value.h"

namespace v12 {

class Isolate;

class Shape {
public:
    // Property slot index.
    using Slot = uint16_t;

    // Create the empty shape (no properties). Singleton per Isolate.
    static Shape* Empty(Isolate* iso);

    // Create an arbitrary shape with the given HeapObjectKind and no
    // properties. Used by singleton heap objects (Undefined, Null, etc.)
    // and by Context to get a shape with a dedicated object_kind().
    static Shape* NewWithKind(Isolate* iso, HeapObjectKind kind);

    // Transition: add a property with the given name. Returns the new Shape.
    // If a transition already exists, returns the cached one.
    Shape* AddProperty(Isolate* iso, std::string_view name);

    // Transition: look up the shape that results from adding `name`.
    // Returns nullptr if no such transition exists yet.
    Shape* LookupTransition(std::string_view name) const;

    // Lookup the slot for a property name. Returns kInvalidSlot if not present.
    // Uses string_view::operator== (memcmp) — works for any string_view.
    static constexpr Slot kInvalidSlot = 0xFFFF;
    Slot Lookup(std::string_view name) const;

    // Fast lookup for interned names. Property names in shapes are interned
    // (stored as interned string_views via iso->Intern), so if the incoming
    // `name` is also interned, we can compare data() pointers directly —
    // a single 64-bit compare instead of memcmp. This is 2-3x faster on the
    // IC miss path. The caller MUST pass an interned string_view.
    Slot LookupInterned(std::string_view interned_name) const {
        const char* needle = interned_name.data();
        for (const auto& p : properties_) {
            if (p.name.data() == needle) return p.slot;
        }
        return kInvalidSlot;
    }

    // Number of properties.
    uint16_t property_count() const { return property_count_; }

    // Get the property name at a given slot index. Used by for-in iteration
    // and Object.keys().
    std::string_view PropertyNameAt(Slot slot) const {
        V12_DCHECK(slot < properties_.size(), "property slot out of range");
        return properties_[slot].name;
    }

    // Total size of the object body in bytes (excluding header).
    // Each property is one Value (8 bytes on 64-bit).
    uint32_t instance_size() const {
        return sizeof(Value) * property_count_;
    }

    HeapObjectKind object_kind() const { return object_kind_; }
    void set_object_kind(HeapObjectKind k) { object_kind_ = k; }

    // Unique ID for diagnostics / shape-equality in ICs.
    uint32_t id() const { return id_; }

    // For arrays: the shape that represents an array. Arrays have a special
    // shape that includes the "length" property and elements storage.
    static Shape* ArrayShape(Isolate* iso);

    bool IsArrayShape() const { return is_array_shape_; }

    // For function objects: the shape that includes the closure's fields.
    static Shape* FunctionShape(Isolate* iso);
    bool IsFunctionShape() const { return is_function_shape_; }

    // Pretty-print for diagnostics.
    void Dump() const;

private:
    Shape(Isolate* iso, uint32_t id, Shape* parent, std::string_view added_name,
          Slot added_slot);

    struct PropertyEntry {
        std::string_view name;
        Slot slot;
    };

    // Property table - linear scan for now. For objects with many properties
    // we'd switch to a hash table, but the common case is <8 properties.
    SmallVector<PropertyEntry, 4> properties_;

    // Transition table: name -> child shape.
    HashMap<std::string_view, Shape*> transitions_;

    Shape* parent_ = nullptr;
    uint16_t property_count_ = 0;
    HeapObjectKind object_kind_ = HeapObjectKind::kObject;
    uint32_t id_;
    bool is_array_shape_ = false;
    bool is_function_shape_ = false;

    friend class Isolate;
};

}  // namespace v12

#endif  // V12_VM_SHAPES_SHAPE_H_
