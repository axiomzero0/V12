// =============================================================================
// src/base/zone.h
// =============================================================================
// Zone: like Arena, but supports per-object destructors via a destructor list.
//
// Why have both Arena and Zone?
//   - Arena is for trivially-destructible objects (IR nodes, AST nodes).
//   - Zone is for objects that need destructors (e.g. containers with
//     non-trivially-destructible members).
//
// The Zone keeps a linked list of "destructor records" and invokes them
// all in reverse-order on Release(). This is cheaper than per-object new/delete
// but slower than Arena's bump pointer. Use Arena when you can.

#ifndef V12_BASE_ZONE_H_
#define V12_BASE_ZONE_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

#include "base/arena.h"
#include "base/macros.h"

namespace v12 {

class Zone {
public:
    explicit Zone(size_t chunk_size = Arena::kDefaultChunkSize)
        : arena_(chunk_size), dtors_(nullptr) {}

    ~Zone() { ReleaseAll(); }

    Zone(const Zone&) = delete;
    Zone& operator=(const Zone&) = delete;
    Zone(Zone&&) = default;
    Zone& operator=(Zone&&) = default;

    template <typename T, typename... Args>
    V12_NODISCARD T* New(Args&&... args) {
        void* mem = arena_.Allocate(sizeof(T), alignof(T));
        T* obj = new (mem) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            RegisterDestructor(obj);
        }
        return obj;
    }

    template <typename T>
    V12_NODISCARD T* NewArray(size_t count) {
        void* mem = arena_.Allocate(sizeof(T) * count, alignof(T));
        T* arr = new (mem) T[count]();
        if constexpr (!std::is_trivially_destructible_v<T>) {
            RegisterArrayDestructor(arr, count);
        }
        return arr;
    }

    void* Allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        return arena_.Allocate(size, alignment);
    }

    void ReleaseAll() {
        // Run destructors in reverse-registration order.
        DestructorNode* n = dtors_;
        while (n != nullptr) {
            n->run(n->object);
            n = n->next;
        }
        dtors_ = nullptr;
        arena_.ReleaseAll();
    }

    Arena* arena() { return &arena_; }
    size_t total_allocated() const { return arena_.total_allocated(); }

private:
    struct DestructorNode {
        DestructorNode* next;
        void* object;
        void (*run)(void*);
    };

    template <typename T>
    void RegisterDestructor(T* obj) {
        auto* node = static_cast<DestructorNode*>(
            arena_.Allocate(sizeof(DestructorNode), alignof(DestructorNode)));
        node->next = dtors_;
        node->object = obj;
        node->run = [](void* p) { static_cast<T*>(p)->~T(); };
        dtors_ = node;
    }

    template <typename T>
    void RegisterArrayDestructor(T* arr, size_t count) {
        auto* node = static_cast<DestructorNode*>(
            arena_.Allocate(sizeof(DestructorNode), alignof(DestructorNode)));
        node->next = dtors_;
        node->object = arr;
        node->run = [count](void* p) {
            T* a = static_cast<T*>(p);
            for (size_t i = 0; i < count; ++i) a[i].~T();
        };
        dtors_ = node;
    }

    Arena arena_;
    DestructorNode* dtors_;
};

}  // namespace v12

#endif  // V12_BASE_ZONE_H_
