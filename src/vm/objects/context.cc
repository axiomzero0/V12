// =============================================================================
// src/vm/objects/context.cc
// =============================================================================

#include "vm/objects/context.h"

#include "vm/isolate/isolate.h"
#include "vm/shapes/shape.h"

namespace v12 {

Context* Context::New(Isolate* iso, Context* parent, uint16_t slot_count) {
    uint32_t size = sizeof(Context) + static_cast<uint32_t>(sizeof(Value) * slot_count);
    void* mem = iso->Allocate(size);
    auto* ctx = static_cast<Context*>(mem);
    // Contexts reuse the kExternal HeapObjectKind (we don't have a dedicated
    // kContext kind yet). The shape just needs to be unique enough that
    // HeapObject::kind() returns kExternal for any context.
    ctx->set_shape(Shape::NewWithKind(iso, HeapObjectKind::kExternal));
    ctx->parent_ = parent;
    ctx->length_ = slot_count;
    ctx->capacity_ = slot_count;
    Value undef = iso->undefined_value();
    Value* s = ctx->slots();
    for (uint16_t i = 0; i < slot_count; ++i) {
        s[i] = undef;
    }
    return ctx;
}

}  // namespace v12
