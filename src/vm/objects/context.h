// =============================================================================
// src/vm/objects/context.h
// =============================================================================
// Context: the runtime representation of a JavaScript scope chain.
//
// A Context is a heap-allocated chain of variable slots. Each function
// activation that captures (or might capture) variables from an enclosing
// function creates a Context. The captured variables live in the context's
// slot array; reads and writes go through LoadContext / StoreContext
// bytecodes.
//
// Layout:
//   [ HeapObject header ]
//   [ Context* parent     ]   <-- the enclosing context (or nullptr)
//   [ uint16_t length     ]   <-- number of slots in this context
//   [ uint16_t capacity   ]
//   [ Value slots[length] ]   <-- inline slot array
//
// The chain is walked by index: a (depth, index) pair identifies a slot
// where `depth` is the number of parent links to follow and `index` is
// the slot within that context. The bytecode generator computes these
// pairs at compile time.
//
// Why a separate Context object (vs. just stack slots)?
//   - Closures outlive the activation that created them. If captured
//     variables lived on the stack, returning a closure would leave it
//     pointing at freed stack memory ("use after free" / dangling).
//   - Putting captured variables on the heap-allocated Context lets the
//     closure keep them alive as long as the closure itself lives.
//
// This is the same approach V8, JavaScriptCore, and SpiderMonkey use.

#ifndef V12_VM_OBJECTS_CONTEXT_H_
#define V12_VM_OBJECTS_CONTEXT_H_

#include <cstdint>

#include "base/macros.h"
#include "vm/values/value.h"

namespace v12 {

class Isolate;
class Shape;

class Context : public HeapObject {
public:
    // Allocate a fresh context with `slot_count` variable slots, linked to
    // `parent` (which may be nullptr for the top-level context).
    static Context* New(Isolate* iso, Context* parent, uint16_t slot_count);

    Context* parent() const { return parent_; }
    uint16_t length() const { return length_; }

    // Direct slot access. `index` must be < length().
    Value GetSlot(uint16_t index) const {
        V12_DCHECK(index < length_, "context slot out of range");
        return slots()[index];
    }
    void SetSlot(uint16_t index, Value v) {
        V12_DCHECK(index < length_, "context slot out of range");
        slots()[index] = v;
    }

    // Walk the parent chain `depth` hops, then return the slot at `index`
    // in that context. Used by LoadContext/StoreContext bytecodes, where
    // the (depth, index) pair is computed at bytecode-generation time.
    Value LoadAt(uint16_t depth, uint16_t index) const {
        const Context* c = this;
        while (depth > 0) {
            V12_DCHECK(c != nullptr, "context chain shorter than depth");
            c = c->parent_;
            --depth;
        }
        V12_DCHECK(c != nullptr, "LoadAt on null context");
        return c->GetSlot(index);
    }
    void StoreAt(uint16_t depth, uint16_t index, Value v) {
        Context* c = this;
        while (depth > 0) {
            V12_DCHECK(c != nullptr, "context chain shorter than depth");
            c = c->parent_;
            --depth;
        }
        V12_DCHECK(c != nullptr, "StoreAt on null context");
        c->SetSlot(index, v);
    }

    Value* slots() {
        return reinterpret_cast<Value*>(reinterpret_cast<char*>(this) + sizeof(Context));
    }
    const Value* slots() const {
        return reinterpret_cast<const Value*>(reinterpret_cast<const char*>(this) + sizeof(Context));
    }

private:
    Context* parent_;
    uint16_t length_;
    uint16_t capacity_;   // matches length_ for now; reserved for future growth
};

}  // namespace v12

#endif  // V12_VM_OBJECTS_CONTEXT_H_
