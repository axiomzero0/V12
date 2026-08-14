// =============================================================================
// src/gc/heap.cc
// =============================================================================

#include "gc/heap.h"

#include <cstdlib>
#include <cstring>

#include "vm/values/value.h"

namespace v12 {

Heap::Heap(Isolate* iso) : iso_(iso) {
    AllocateNewChunk(kChunkSize);
}

Heap::~Heap() = default;

void Heap::AllocateNewChunk(size_t min_size) {
    size_t sz = min_size > kChunkSize ? min_size : kChunkSize;
    auto chunk = std::make_unique<HeapChunk>();
    chunk->base = static_cast<uint8_t*>(std::malloc(sz));
    V12_CHECK(chunk->base != nullptr, "heap OOM: requested %zu bytes", sz);
    chunk->top = chunk->base;
    chunk->limit = chunk->base + sz;
    current_chunk_ = chunk.get();
    chunks_.push_back(std::move(chunk));
}

void* Heap::Allocate(uint32_t size) {
    // Align to 8 bytes.
    size = (size + 7) & ~7u;

    if (current_chunk_->top + size > current_chunk_->limit) {
        if (total_bytes_allocated_ > gc_threshold_) {
            CollectGarbage();
            gc_threshold_ = total_bytes_allocated_ * 2;
        }
        if (current_chunk_->top + size > current_chunk_->limit) {
            AllocateNewChunk(size);
        }
    }

    void* mem = current_chunk_->top;
    current_chunk_->top += size;
    total_bytes_allocated_ += size;
    return mem;
}

void Heap::TrackObject(HeapObject* obj, uint32_t size) {
    // In a real GC we'd record the object size for sweeping. For now we
    // just append to the current chunk's object list.
    current_chunk_->objects.push_back(obj);
    (void)size;
}

void Heap::CollectGarbage() {
    ++gc_count_;
    // Simplified mark-sweep:
    //   1. Mark phase: starting from roots, mark all reachable objects.
    //   2. Sweep phase: free unmarked objects.
    //
    // We don't actually free memory (the bump pointer doesn't support it),
    // so this GC is currently a no-op except for statistics. The next
    // allocation failure will trigger a new chunk.
    //
    // A real implementation would:
    //   - For each persistent root, mark transitively.
    //   - For the current Frame, walk registers and mark.
    //   - Walk each chunk's object list, freeing unmarked objects.
    //   - Reset the bump pointer if a chunk is fully empty.

    // For now, just log.
    // TODO: implement mark-sweep.
}

void Heap::AddPersistentRoot(HeapObject** root) {
    persistent_roots_.push_back(root);
}

void Heap::RemovePersistentRoot(HeapObject** root) {
    for (size_t i = 0; i < persistent_roots_.size(); ++i) {
        if (persistent_roots_[i] == root) {
            persistent_roots_[i] = persistent_roots_.back();
            persistent_roots_.pop_back();
            return;
        }
    }
}

}  // namespace v12
