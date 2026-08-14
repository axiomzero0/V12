// =============================================================================
// src/base/arena.h
// =============================================================================
// Bump-allocated arena for IR nodes, AST nodes, and other short-lived
// compiler-internal structures.
//
// Design:
//   - Allocate-only. No individual frees. Whole arena is released at once.
//   - Allocation is pointer-bump with alignment.
//   - Memory comes from a linked list of fixed-size chunks (default 64 KiB).
//   - NOT thread-safe. Each compilation thread owns its own arena.
//
// Why an arena?
//   IR graphs have hundreds of thousands of small objects with the same
//   lifetime. malloc/free per node is 10-50x slower than arena bumping and
//   fragments the heap. We also need deterministic destruction order
//   (arena destroys all nodes at once, in reverse order if needed).
//
// Safety:
//   - We never return nullptr; on OOM we abort with a diagnostic.
//   - Allocations are always naturally aligned.
//   - The arena does NOT call destructors. Objects placed in the arena must
//     have trivial destructors, or must be explicitly destroyed by the owner.

#ifndef V12_BASE_ARENA_H_
#define V12_BASE_ARENA_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "base/macros.h"

namespace v12 {

class Arena {
public:
    static constexpr size_t kDefaultChunkSize = 64 * 1024;  // 64 KiB

    explicit Arena(size_t chunk_size = kDefaultChunkSize)
        : chunk_size_(chunk_size), head_(nullptr), total_allocated_(0) {}

    ~Arena() { ReleaseAll(); }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&& other) noexcept
        : chunk_size_(other.chunk_size_),
          head_(other.head_),
          total_allocated_(other.total_allocated_) {
        other.head_ = nullptr;
        other.total_allocated_ = 0;
    }
    Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            ReleaseAll();
            chunk_size_ = other.chunk_size_;
            head_ = other.head_;
            total_allocated_ = other.total_allocated_;
            other.head_ = nullptr;
            other.total_allocated_ = 0;
        }
        return *this;
    }

    // Allocate `size` bytes with `alignment`. Alignment must be a power of 2.
    V12_NODISCARD void* Allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        V12_DCHECK(size > 0, "zero-size allocation");
        V12_DCHECK((alignment & (alignment - 1)) == 0, "alignment must be power of 2");

        // Try the current chunk first.
        if (head_ != nullptr) {
            if (void* p = TryAllocateIn(head_, size, alignment)) {
                total_allocated_ += size;
                return p;
            }
        }

        // Need a new chunk. The chunk must be large enough to hold this
        // allocation with alignment padding.
        size_t needed = size + alignment - 1;
        size_t this_chunk_size = needed > chunk_size_ ? needed : chunk_size_;
        AllocateChunk(this_chunk_size);

        void* p = TryAllocateIn(head_, size, alignment);
        V12_CHECK(p != nullptr, "arena allocation failed even after fresh chunk");
        total_allocated_ += size;
        return p;
    }

    // Allocate a default-constructed T.
    template <typename T, typename... Args>
    V12_NODISCARD T* New(Args&&... args) {
        void* mem = Allocate(sizeof(T), alignof(T));
        return new (mem) T(std::forward<Args>(args)...);
    }

    // Allocate an array of T with `count` elements, value-initialized.
    template <typename T>
    V12_NODISCARD T* NewArray(size_t count) {
        void* mem = Allocate(sizeof(T) * count, alignof(T));
        return new (mem) T[count]();
    }

    size_t total_allocated() const { return total_allocated_; }

    // Release all memory. After this, the arena is empty and can be reused.
    void ReleaseAll() {
        Chunk* c = head_;
        while (c != nullptr) {
            Chunk* next = c->next;
            std::free(c);
            c = next;
        }
        head_ = nullptr;
        total_allocated_ = 0;
    }

private:
    struct Chunk {
        Chunk* next;
        size_t capacity;      // bytes available after this header
        size_t used;          // bytes already used
        // The actual data follows immediately after this header.
        char* data() { return reinterpret_cast<char*>(this + 1); }
        char* end() { return data() + capacity; }
    };

    V12_NODISCARD void* TryAllocateIn(Chunk* c, size_t size, size_t alignment) {
        char* base = c->data() + c->used;
        size_t space = c->capacity - c->used;
        void* p = base;
        if (std::align(alignment, size, p, space)) {
            c->used = static_cast<char*>(p) - c->data() + size;
            return p;
        }
        return nullptr;
    }

    void AllocateChunk(size_t min_capacity) {
        size_t total = sizeof(Chunk) + min_capacity;
        void* raw = std::malloc(total);
        V12_CHECK(raw != nullptr, "arena out of memory (requested %zu bytes)", total);
        Chunk* c = static_cast<Chunk*>(raw);
        c->next = head_;
        c->capacity = min_capacity;
        c->used = 0;
        head_ = c;
    }

    size_t chunk_size_;
    Chunk* head_;
    size_t total_allocated_;
};

// STL-compatible allocator that wraps an Arena. Lets you use Arena with
// std::vector, std::unordered_map, etc.
template <typename T>
class ArenaAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = size_t;
    using difference_type = ptrdiff_t;

    template <typename U>
    struct rebind { using other = ArenaAllocator<U>; };

    explicit ArenaAllocator(Arena* arena) : arena_(arena) {}

    template <typename U>
    ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena_(other.arena_) {}

    T* allocate(size_t n) {
        return static_cast<T*>(arena_->Allocate(sizeof(T) * n, alignof(T)));
    }

    void deallocate(T*, size_t) noexcept {
        // No-op: arena owns everything.
    }

    template <typename U>
    bool operator==(const ArenaAllocator<U>& other) const noexcept {
        return arena_ == other.arena_;
    }
    template <typename U>
    bool operator!=(const ArenaAllocator<U>& other) const noexcept {
        return arena_ != other.arena_;
    }

    Arena* arena() const { return arena_; }

private:
    Arena* arena_;

    template <typename U>
    friend class ArenaAllocator;
};

}  // namespace v12

#endif  // V12_BASE_ARENA_H_
