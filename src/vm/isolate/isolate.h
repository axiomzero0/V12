// =============================================================================
// src/vm/isolate/isolate.h
// =============================================================================
// Isolate: the central VM context.
//
// An Isolate is a single JS execution context. It owns:
//   - The heap (for objects, strings, shapes)
//   - The root set (singletons like undefined, null, true, false)
//   - The global object
//   - The shape tree (root shape, array shape, function shape)
//   - The compilation thread pool (for the JIT)
//   - Per-thread state (current Context, current Frame)
//
// We do NOT support multiple isolates in one process yet. The Isolate is
// a process-wide singleton accessed via Isolate::Current(). This is similar
// to V8's Isolate::GetCurrent.
//
// Threading:
//   The Isolate itself is not thread-safe. The interpreter runs on the
//   main thread. The JIT compiler runs on background threads, but those
//   threads do not touch the Isolate's mutable state directly - they
//   work on compilation jobs that produce CodeObjects, which are then
//   installed on the main thread.

#ifndef V12_VM_ISOLATE_ISOLATE_H_
#define V12_VM_ISOLATE_ISOLATE_H_

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/arena.h"
#include "base/hash-map.h"
#include "base/macros.h"
#include "vm/values/value.h"

namespace v12 {

class Heap;
class Shape;
class Context;
class JSObject;
class JSArray;
class JSString;
class JSFunction;
class JSNumber;
class JSBoolean;
class JSUndefined;
class JSNull;
class FunctionInfo;
class FeedbackVector;
class Interner;

class Isolate;

// String interner interface.
class Interner {
public:
    virtual ~Interner();
    virtual std::string_view Intern(std::string_view s) = 0;
    virtual bool IsInterned(std::string_view s) const = 0;
};

class Isolate {
public:
    Isolate();
    ~Isolate();

    Isolate(const Isolate&) = delete;
    Isolate& operator=(const Isolate&) = delete;

    // Process-wide current isolate.
    static Isolate* Current();

    // ----- Root access -----
    Value undefined_value() const { return undefined_; }
    Value null_value() const { return null_; }
    Value true_value() const { return true_; }
    Value false_value() const { return false_; }

    JSUndefined* undefined_object() const { return undefined_obj_; }
    JSNull* null_object() const { return null_obj_; }
    JSBoolean* true_object() const { return true_obj_; }
    JSBoolean* false_object() const { return false_obj_; }

    // Convert a primitive to its Value representation.
    Value BooleanValue(bool b) { return b ? true_ : false_; }

    // ----- Heap -----
    Heap* heap() { return heap_.get(); }

    // Allocate `size` bytes of raw heap memory. Returns a pointer that is
    // 8-byte aligned. The GC will scan this memory for tagged pointers.
    // The caller is responsible for initializing the header.
    void* Allocate(uint32_t size);

    // ----- Shape tree -----
    Shape* empty_shape() const { return empty_shape_; }
    Shape* array_shape() const { return array_shape_; }
    Shape* function_shape() const { return function_shape_; }

    // Allocate a new unique Shape ID.
    uint32_t NextShapeId() { return next_shape_id_++; }

    // ----- String interning -----
    // Interning is essential for property access: ICs compare shapes by
    // pointer, and shapes compare property names by pointer. Interning
    // strings ensures that the same literal string yields the same pointer.
    std::string_view Intern(std::string_view s);
    bool IsInterned(std::string_view s) const;

    // ----- Global object -----
    JSObject* global_object() const { return global_object_; }

    // ----- Arenas -----
    // Persistent arena for things that live as long as the Isolate
    // (e.g. FunctionInfos, Bytecode).
    Arena* permanent_arena() { return &permanent_arena_; }

    // ----- Source position lookup -----
    // Given a FunctionInfo and a bytecode offset, return the source line.
    // (Used for stack traces.)

    // ----- Statistics -----
    struct Stats {
        uint64_t bytes_allocated = 0;
        uint64_t allocations = 0;
        uint64_t gc_collections = 0;
        uint64_t gc_bytes_freed = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    void InitializeRoots();
    void InitializeShapes();
    void InitializeGlobals();

    Arena permanent_arena_;

    std::unique_ptr<Heap> heap_;
    std::unique_ptr<Interner> interner_;

    // Roots
    Value undefined_;
    Value null_;
    Value true_;
    Value false_;
    JSUndefined* undefined_obj_ = nullptr;
    JSNull* null_obj_ = nullptr;
    JSBoolean* true_obj_ = nullptr;
    JSBoolean* false_obj_ = nullptr;

    Shape* empty_shape_ = nullptr;
    Shape* array_shape_ = nullptr;
    Shape* function_shape_ = nullptr;
    uint32_t next_shape_id_ = 0;

    JSObject* global_object_ = nullptr;

    Stats stats_;
};

// Convenience accessor for the current isolate.
inline Isolate* iso() { return Isolate::Current(); }

}  // namespace v12

#endif  // V12_VM_ISOLATE_ISOLATE_H_
