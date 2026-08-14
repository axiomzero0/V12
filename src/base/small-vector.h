// =============================================================================
// src/base/small-vector.h
// =============================================================================
// Inline-storage vector: like std::vector<T> but with N elements stored
// inline in the object (no heap allocation for small collections).
//
// Why not absl::InlinedVector or llvm::SmallVector?
//   - We don't want to vendor absl or LLVM. This is a small, self-contained
//     re-implementation that covers the cases we actually use.
//
// Why not std::vector?
//   - std::vector always heap-allocates. In the IR, we have millions of
//     tiny vectors (use lists, input lists, predecessor lists). The heap
//     traffic dominates. SmallVector<4> handles the common case inline.
//
// Limitations vs std::vector:
//   - No allocator support. Growth uses operator new/delete.
//   - No exception safety. We use CHECKs instead.
//   - Iterators are plain pointers, which is fine for our use cases.

#ifndef V12_BASE_SMALL_VECTOR_H_
#define V12_BASE_SMALL_VECTOR_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <type_traits>
#include <utility>

#include "base/macros.h"

namespace v12 {

template <typename T, size_t kInlineCapacity>
class SmallVector {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    SmallVector() : size_(0), capacity_(kInlineCapacity), data_(inline_storage()) {}

    explicit SmallVector(size_type n) : SmallVector() {
        reserve(n);
        for (size_type i = 0; i < n; ++i) {
            new (data_ + i) T();
        }
        size_ = n;
    }

    SmallVector(size_type n, const T& value) : SmallVector() {
        reserve(n);
        for (size_type i = 0; i < n; ++i) {
            new (data_ + i) T(value);
        }
        size_ = n;
    }

    SmallVector(std::initializer_list<T> init) : SmallVector() {
        reserve(init.size());
        for (const auto& v : init) {
            new (data_ + size_) T(v);
            ++size_;
        }
    }

    SmallVector(const SmallVector& other) : SmallVector() {
        reserve(other.size_);
        for (size_type i = 0; i < other.size_; ++i) {
            new (data_ + i) T(other.data_[i]);
        }
        size_ = other.size_;
    }

    SmallVector(SmallVector&& other) noexcept : SmallVector() {
        if (other.is_inline()) {
            // Move-construct each inline element.
            for (size_type i = 0; i < other.size_; ++i) {
                new (data_ + i) T(std::move(other.data_[i]));
                other.data_[i].~T();
            }
            size_ = other.size_;
            other.size_ = 0;
        } else {
            // Steal the heap storage.
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.reset_to_inline();
        }
    }

    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            clear();
            reserve(other.size_);
            for (size_type i = 0; i < other.size_; ++i) {
                new (data_ + i) T(other.data_[i]);
            }
            size_ = other.size_;
        }
        return *this;
    }

    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) {
            clear();
            if (!is_inline()) free_heap();
            if (other.is_inline()) {
                data_ = inline_storage();
                capacity_ = kInlineCapacity;
                for (size_type i = 0; i < other.size_; ++i) {
                    new (data_ + i) T(std::move(other.data_[i]));
                    other.data_[i].~T();
                }
                size_ = other.size_;
                other.size_ = 0;
            } else {
                data_ = other.data_;
                size_ = other.size_;
                capacity_ = other.capacity_;
                other.reset_to_inline();
            }
        }
        return *this;
    }

    ~SmallVector() {
        clear();
        if (!is_inline()) free_heap();
    }

    // Capacity ----------------------------------------------------------------
    size_type size() const { return size_; }
    size_type capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }

    void reserve(size_type new_cap) {
        if (new_cap <= capacity_) return;
        grow(new_cap);
    }

    void resize(size_type new_size) {
        if (new_size < size_) {
            for (size_type i = new_size; i < size_; ++i) {
                data_[i].~T();
            }
        } else if (new_size > size_) {
            reserve(new_size);
            for (size_type i = size_; i < new_size; ++i) {
                new (data_ + i) T();
            }
        }
        size_ = new_size;
    }

    void resize(size_type new_size, const T& value) {
        if (new_size < size_) {
            for (size_type i = new_size; i < size_; ++i) {
                data_[i].~T();
            }
        } else if (new_size > size_) {
            reserve(new_size);
            for (size_type i = size_; i < new_size; ++i) {
                new (data_ + i) T(value);
            }
        }
        size_ = new_size;
    }

    // Element access ----------------------------------------------------------
    reference operator[](size_type i) {
        V12_DCHECK(i < size_, "SmallVector index out of range");
        return data_[i];
    }
    const_reference operator[](size_type i) const {
        V12_DCHECK(i < size_, "SmallVector index out of range");
        return data_[i];
    }
    reference front() { V12_DCHECK(size_ > 0, "front() on empty"); return data_[0]; }
    const_reference front() const { V12_DCHECK(size_ > 0, "front() on empty"); return data_[0]; }
    reference back() { V12_DCHECK(size_ > 0, "back() on empty"); return data_[size_ - 1]; }
    const_reference back() const { V12_DCHECK(size_ > 0, "back() on empty"); return data_[size_ - 1]; }
    pointer data() { return data_; }
    const_pointer data() const { return data_; }

    // Iteration ---------------------------------------------------------------
    iterator begin() { return data_; }
    const_iterator begin() const { return data_; }
    const_iterator cbegin() const { return data_; }
    iterator end() { return data_ + size_; }
    const_iterator end() const { return data_ + size_; }
    const_iterator cend() const { return data_ + size_; }

    // Modifiers ---------------------------------------------------------------
    void push_back(const T& value) {
        if (V12_UNLIKELY(size_ == capacity_)) grow(capacity_ * 2);
        new (data_ + size_) T(value);
        ++size_;
    }

    void push_back(T&& value) {
        if (V12_UNLIKELY(size_ == capacity_)) grow(capacity_ * 2);
        new (data_ + size_) T(std::move(value));
        ++size_;
    }

    template <typename... Args>
    reference emplace_back(Args&&... args) {
        if (V12_UNLIKELY(size_ == capacity_)) grow(capacity_ * 2);
        new (data_ + size_) T(std::forward<Args>(args)...);
        ++size_;
        return back();
    }

    void pop_back() {
        V12_DCHECK(size_ > 0, "pop_back on empty");
        --size_;
        data_[size_].~T();
    }

    void clear() {
        for (size_type i = 0; i < size_; ++i) {
            data_[i].~T();
        }
        size_ = 0;
    }

    iterator insert(const_iterator pos, const T& value) {
        size_type idx = pos - data_;
        V12_DCHECK(idx <= size_, "insert position out of range");
        if (size_ == capacity_) {
            size_type new_cap = capacity_ * 2;
            T* new_data = allocate_heap(new_cap);
            for (size_type i = 0; i < idx; ++i) {
                new (new_data + i) T(std::move(data_[i]));
                data_[i].~T();
            }
            new (new_data + idx) T(value);
            for (size_type i = idx; i < size_; ++i) {
                new (new_data + i + 1) T(std::move(data_[i]));
                data_[i].~T();
            }
            if (!is_inline()) free_heap();
            data_ = new_data;
            capacity_ = new_cap;
        } else {
            if (size_ > idx) {
                new (data_ + size_) T(std::move(data_[size_ - 1]));
                data_[size_ - 1].~T();
                for (size_type i = size_ - 1; i > idx; --i) {
                    new (data_ + i) T(std::move(data_[i - 1]));
                    data_[i - 1].~T();
                }
                data_[idx].~T();
            }
            new (data_ + idx) T(value);
        }
        ++size_;
        return data_ + idx;
    }

    iterator erase(const_iterator pos) {
        size_type idx = pos - data_;
        V12_DCHECK(idx < size_, "erase position out of range");
        data_[idx].~T();
        for (size_type i = idx; i + 1 < size_; ++i) {
            new (data_ + i) T(std::move(data_[i + 1]));
            data_[i + 1].~T();
        }
        --size_;
        return data_ + idx;
    }

    void shrink_to_fit() {
        if (is_inline()) return;
        if (size_ <= kInlineCapacity) {
            T* heap = data_;
            T* inline_buf = inline_storage();
            for (size_type i = 0; i < size_; ++i) {
                new (inline_buf + i) T(std::move(heap[i]));
                heap[i].~T();
            }
            std::free(heap);
            data_ = inline_buf;
            capacity_ = kInlineCapacity;
        } else if (size_ < capacity_) {
            T* new_data = allocate_heap(size_);
            for (size_type i = 0; i < size_; ++i) {
                new (new_data + i) T(std::move(data_[i]));
                data_[i].~T();
            }
            if (!is_inline()) std::free(data_);
            data_ = new_data;
            capacity_ = size_;
        }
    }

private:
    static T* allocate_heap(size_type n) {
        void* p = std::malloc(sizeof(T) * n);
        V12_CHECK(p != nullptr, "SmallVector out of memory (%zu elements)", n);
        return static_cast<T*>(p);
    }

    void free_heap() {
        std::free(data_);
    }

    void grow(size_type new_cap) {
        if (new_cap <= kInlineCapacity) new_cap = kInlineCapacity + 1;
        T* new_data = allocate_heap(new_cap);
        for (size_type i = 0; i < size_; ++i) {
            new (new_data + i) T(std::move(data_[i]));
            data_[i].~T();
        }
        if (!is_inline()) std::free(data_);
        data_ = new_data;
        capacity_ = new_cap;
    }

    bool is_inline() const { return data_ == inline_storage_const(); }
    T* inline_storage() {
        return reinterpret_cast<T*>(inline_storage_raw_);
    }
    const T* inline_storage_const() const {
        return reinterpret_cast<const T*>(inline_storage_raw_);
    }

    void reset_to_inline() {
        data_ = inline_storage();
        capacity_ = kInlineCapacity;
        size_ = 0;
    }

    size_type size_;
    size_type capacity_;
    T* data_;
    alignas(T) char inline_storage_raw_[sizeof(T) * kInlineCapacity == 0 ? 1 : sizeof(T) * kInlineCapacity];
};

// Deduction guide for the empty case (kInlineCapacity=0 is allowed but rare).
template <typename T, size_t N>
bool operator==(const SmallVector<T, N>& a, const SmallVector<T, N>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!(a[i] == b[i])) return false;
    }
    return true;
}

}  // namespace v12

#endif  // V12_BASE_SMALL_VECTOR_H_
