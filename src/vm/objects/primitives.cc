// =============================================================================
// src/vm/objects/primitives.cc
// =============================================================================

#include "vm/objects/primitives.h"

#include "vm/isolate/isolate.h"
#include "vm/shapes/shape.h"

namespace v12 {

JSUndefined* JSUndefined::New(Isolate* iso) {
    void* mem = iso->Allocate(sizeof(JSUndefined));
    auto* u = static_cast<JSUndefined*>(mem);
    u->set_shape(Shape::NewWithKind(iso, HeapObjectKind::kUndefined));
    return u;
}

JSNull* JSNull::New(Isolate* iso) {
    void* mem = iso->Allocate(sizeof(JSNull));
    auto* n = static_cast<JSNull*>(mem);
    n->set_shape(Shape::NewWithKind(iso, HeapObjectKind::kNull));
    return n;
}

JSBoolean* JSBoolean::New(Isolate* iso, bool value) {
    void* mem = iso->Allocate(sizeof(JSBoolean));
    auto* b = static_cast<JSBoolean*>(mem);
    b->set_shape(Shape::NewWithKind(iso, HeapObjectKind::kBoolean));
    b->value_ = value;
    return b;
}

}  // namespace v12
