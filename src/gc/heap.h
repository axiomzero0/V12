// =============================================================================
// src/gc/heap.h
// =============================================================================
// Simple bump-pointer heap with a stop-the-world mark-sweep collector.
//
// This is intentionally minimal. A production JS engine would have:
//   - Generational GC (young / old generation)
//   - Concurrent marking
//   - Incremental compaction
//   - Write barriers remembered sets
//
// We have a bump-pointer allocator and a non-incremental mark-sweep. This
// is sufficient for correctness testing of the rest of the engine. The GC
// API (Allocate, CollectGarbage, AddRoot, etc.) is stable so we can swap in
// a better collector later without touching the rest of the engine.

#ifndef V12_GC_HEAP_H_
#define V12_GC_HEAP_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/macros.h"

namespace v12 {

class Isolate;
class HeapObject;

// A HeapChunk is a contiguous region of memory that the GC manages.
struct HeapChunk {
    uint8_t* base;
    uint8_t* top;
    uint8_t* limit;
    std::vector<HeapObject*> objects;   // for enumeration during sweep
};

class Heap {
public:
    explicit Heap(Isolate* iso);
    ~Heap();

    // Allocate `size` bytes. Returns 8-byte aligned memory.
    // Triggers a GC if the current chunk is full.
    void* Allocate(uint32_t size);

    // Track an object. Called by Allocate.
    void TrackObject(HeapObject* obj, uint32_t size);

    // Force a garbage collection.
    void CollectGarbage();

    // Root management. Roots are scanned by the GC; anything reachable
    // from a root is kept. Roots are typically:
    //   - The current Frame's registers (handled separately by the GC)
    //   - The global object
    //   - Persistent handles
    void AddPersistentRoot(HeapObject** root);
    void RemovePersistentRoot(HeapObject** root);

    Isolate* isolate() { return iso_; }

    uint64_t bytes_allocated() const { return total_bytes_allocated_; }
    uint64_t gc_count() const { return gc_count_; }

private:
    void AllocateNewChunk(size_t min_size);
    void Mark(HeapObject* obj);
    void Sweep();

    Isolate* iso_;
    std::vector<std::unique_ptr<HeapChunk>> chunks_;
    HeapChunk* current_chunk_ = nullptr;
    std::vector<HeapObject**> persistent_roots_;
    uint64_t total_bytes_allocated_ = 0;
    uint64_t gc_count_ = 0;
    size_t gc_threshold_ = 4 * 1024 * 1024;   // 4 MiB before first GC

    static constexpr size_t kChunkSize = 1 * 1024 * 1024;  // 1 MiB chunks
};

}  // namespace v12

#endif  // V12_GC_HEAP_H_
