// =============================================================================
// src/base/hash-map.h
// =============================================================================
// Open-addressing hash map with linear probing, designed for compiler-internal
// use. Keys are required to be hashable with a free function `Hash(key)` and
// comparable with `==`.
//
// Why not std::unordered_map?
//   - std::unordered_map allocates a node per entry. In a compiler we have
//     thousands of tiny maps (one per function, per pass). The allocator
//     traffic dominates.
//   - This map stores entries inline in a flat array.
//   - It is also rehash-aware: we can move-construct without rehashing.
//
// Why not absl::flat_hash_map?
//   - We don't vendor absl.
//
// This is a minimal implementation: it covers the operations the compiler
// needs (insert, lookup, erase, iterate). It is NOT a complete replacement
// for std::unordered_map; notably, it does not support heterogeneous lookup
// or node extraction.

#ifndef V12_BASE_HASH_MAP_H_
#define V12_BASE_HASH_MAP_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "base/macros.h"
#include "base/small-vector.h"

namespace v12 {

// -----------------------------------------------------------------------------
// Default hash function. Specialize Hash<T> for custom key types.
// -----------------------------------------------------------------------------
template <typename T>
struct Hash {
    size_t operator()(const T& value) const {
        return std::hash<T>{}(value);
    }
};

// FNV-1a for raw byte sequences. Used for string hashing.
inline size_t Fnv1aHash(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t hash = 14695981039346656037ULL;  // FNV offset basis
    for (size_t i = 0; i < len; ++i) {
        hash ^= p[i];
        hash *= 1099511628211ULL;  // FNV prime
    }
    return hash;
}

template <typename Key, typename Value, typename HashFn = Hash<Key>>
class HashMap {
public:
    using key_type = Key;
    using mapped_type = Value;
    using value_type = std::pair<Key, Value>;
    using size_type = size_t;

private:
    // Each slot is either Empty, Occupied, or Tombstone.
    enum class SlotState : uint8_t {
        kEmpty,
        kOccupied,
        kTombstone,
    };

    struct Slot {
        SlotState state;
        alignas(Key)   char key_buf[sizeof(Key)];
        alignas(Value) char val_buf[sizeof(Value)];

        Key& key() { return *reinterpret_cast<Key*>(key_buf); }
        const Key& key() const { return *reinterpret_cast<const Key*>(key_buf); }
        Value& value() { return *reinterpret_cast<Value*>(val_buf); }
        const Value& value() const { return *reinterpret_cast<const Value*>(val_buf); }

        void set_occupied(const Key& k, const Value& v) {
            new (key_buf) Key(k);
            new (val_buf) Value(v);
            state = SlotState::kOccupied;
        }
        void set_occupied(Key&& k, Value&& v) {
            new (key_buf) Key(std::move(k));
            new (val_buf) Value(std::move(v));
            state = SlotState::kOccupied;
        }
        void destroy() {
            if (state == SlotState::kOccupied) {
                key().~Key();
                value().~Value();
            }
            state = SlotState::kTombstone;
        }
    };

public:
    class Iterator {
    public:
        Iterator(Slot* slot, Slot* end) : slot_(slot), end_(end) {
            advance_to_occupied();
        }
        value_type& operator*() { return *reinterpret_cast<value_type*>(slot_); }
        value_type* operator->() { return reinterpret_cast<value_type*>(slot_); }
        Iterator& operator++() { ++slot_; advance_to_occupied(); return *this; }
        Iterator operator++(int) { Iterator tmp = *this; ++(*this); return tmp; }
        bool operator==(const Iterator& o) const { return slot_ == o.slot_; }
        bool operator!=(const Iterator& o) const { return slot_ != o.slot_; }
    private:
        void advance_to_occupied() {
            while (slot_ != end_ && slot_->state != SlotState::kOccupied) ++slot_;
        }
        Slot* slot_;
        Slot* end_;
    };

    using iterator = Iterator;

    HashMap() : slots_(nullptr), capacity_(0), size_(0), tombstones_(0) {
        rehash(kInitialCapacity);
    }

    explicit HashMap(size_t initial_capacity)
        : slots_(nullptr), capacity_(0), size_(0), tombstones_(0) {
        rehash(initial_capacity < kInitialCapacity ? kInitialCapacity : initial_capacity);
    }

    HashMap(const HashMap& other)
        : slots_(nullptr), capacity_(0), size_(0), tombstones_(0) {
        rehash(other.capacity_);
        for (size_type i = 0; i < other.capacity_; ++i) {
            if (other.slots_[i].state == SlotState::kOccupied) {
                insert(other.slots_[i].key(), other.slots_[i].value());
            }
        }
    }

    HashMap(HashMap&& other) noexcept
        : slots_(other.slots_),
          capacity_(other.capacity_),
          size_(other.size_),
          tombstones_(other.tombstones_) {
        other.slots_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
        other.tombstones_ = 0;
    }

    HashMap& operator=(const HashMap& other) {
        if (this != &other) {
            clear();
            rehash(other.capacity_);
            for (size_type i = 0; i < other.capacity_; ++i) {
                if (other.slots_[i].state == SlotState::kOccupied) {
                    insert(other.slots_[i].key(), other.slots_[i].value());
                }
            }
        }
        return *this;
    }

    HashMap& operator=(HashMap&& other) noexcept {
        if (this != &other) {
            destroy_all_and_free();
            slots_ = other.slots_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            tombstones_ = other.tombstones_;
            other.slots_ = nullptr;
            other.capacity_ = 0;
            other.size_ = 0;
            other.tombstones_ = 0;
        }
        return *this;
    }

    ~HashMap() { destroy_all_and_free(); }

    size_type size() const { return size_; }
    bool empty() const { return size_ == 0; }
    size_type capacity() const { return capacity_; }

    iterator begin() { return Iterator(slots_, slots_ + capacity_); }
    iterator end() { return Iterator(slots_ + capacity_, slots_ + capacity_); }

    // Lookup ------------------------------------------------------------------
    Value* find(const Key& key) {
        if (capacity_ == 0) return nullptr;
        size_type idx = hash_index(key);
        for (size_type probe = 0; probe < capacity_; ++probe) {
            Slot& s = slots_[idx];
            if (s.state == SlotState::kEmpty) return nullptr;
            if (s.state == SlotState::kOccupied && s.key() == key) {
                return &s.value();
            }
            idx = (idx + 1) & (capacity_ - 1);
        }
        return nullptr;
    }

    const Value* find(const Key& key) const {
        return const_cast<HashMap*>(this)->find(key);
    }

    bool contains(const Key& key) const { return find(key) != nullptr; }

    // Insertion ---------------------------------------------------------------
    std::pair<Value*, bool> insert(const Key& key, const Value& value) {
        maybe_grow();
        return insert_no_grow(key, value);
    }

    std::pair<Value*, bool> insert(Key&& key, Value&& value) {
        maybe_grow();
        size_type idx = hash_index(key);
        for (size_type probe = 0; probe < capacity_; ++probe) {
            Slot& s = slots_[idx];
            if (s.state == SlotState::kEmpty || s.state == SlotState::kTombstone) {
                s.set_occupied(std::move(key), std::move(value));
                ++size_;
                return {&s.value(), true};
            }
            if (s.key() == key) {
                return {&s.value(), false};
            }
            idx = (idx + 1) & (capacity_ - 1);
        }
        V12_FAIL("HashMap insert failed (table full after maybe_grow)");
    }

    Value& operator[](const Key& key) {
        auto [it, inserted] = insert(key, Value{});
        return *it;
    }

    // Erasure -----------------------------------------------------------------
    void erase(const Key& key) {
        if (capacity_ == 0) return;
        size_type idx = hash_index(key);
        for (size_type probe = 0; probe < capacity_; ++probe) {
            Slot& s = slots_[idx];
            if (s.state == SlotState::kEmpty) return;
            if (s.state == SlotState::kOccupied && s.key() == key) {
                s.destroy();
                --size_;
                ++tombstones_;
                return;
            }
            idx = (idx + 1) & (capacity_ - 1);
        }
    }

    void clear() {
        for (size_type i = 0; i < capacity_; ++i) {
            if (slots_[i].state == SlotState::kOccupied) {
                slots_[i].destroy();
                --size_;
            }
            slots_[i].state = SlotState::kEmpty;
        }
        tombstones_ = 0;
    }

private:
    static constexpr size_type kInitialCapacity = 16;
    static constexpr double kMaxLoadFactor = 0.7;

    size_type hash_index(const Key& key) const {
        HashFn hasher;
        return hasher(key) & (capacity_ - 1);
    }

    void maybe_grow() {
        if ((size_ + tombstones_ + 1) * 10 > capacity_ * 7) {
            rehash(capacity_ * 2);
        }
    }

    std::pair<Value*, bool> insert_no_grow(const Key& key, const Value& value) {
        size_type idx = hash_index(key);
        for (size_type probe = 0; probe < capacity_; ++probe) {
            Slot& s = slots_[idx];
            if (s.state == SlotState::kEmpty || s.state == SlotState::kTombstone) {
                s.set_occupied(key, value);
                ++size_;
                return {&s.value(), true};
            }
            if (s.key() == key) {
                return {&s.value(), false};
            }
            idx = (idx + 1) & (capacity_ - 1);
        }
        V12_FAIL("HashMap insert_no_grow failed");
    }

    void rehash(size_type new_cap) {
        // Round up to power of 2
        --new_cap;
        new_cap |= new_cap >> 1;
        new_cap |= new_cap >> 2;
        new_cap |= new_cap >> 4;
        new_cap |= new_cap >> 8;
        new_cap |= new_cap >> 16;
        new_cap |= new_cap >> 32;
        ++new_cap;

        Slot* old_slots = slots_;
        size_type old_cap = capacity_;

        slots_ = static_cast<Slot*>(std::malloc(sizeof(Slot) * new_cap));
        V12_CHECK(slots_ != nullptr, "HashMap out of memory");
        for (size_type i = 0; i < new_cap; ++i) {
            slots_[i].state = SlotState::kEmpty;
        }
        capacity_ = new_cap;
        size_ = 0;
        tombstones_ = 0;

        for (size_type i = 0; i < old_cap; ++i) {
            if (old_slots[i].state == SlotState::kOccupied) {
                insert_no_grow(std::move(old_slots[i].key()),
                              std::move(old_slots[i].value()));
                old_slots[i].state = SlotState::kEmpty;
            }
        }
        std::free(old_slots);
    }

    void destroy_all_and_free() {
        if (slots_ == nullptr) return;
        for (size_type i = 0; i < capacity_; ++i) {
            if (slots_[i].state == SlotState::kOccupied) {
                slots_[i].destroy();
            }
        }
        std::free(slots_);
        slots_ = nullptr;
    }

    Slot* slots_;
    size_type capacity_;
    size_type size_;
    size_type tombstones_;
};

// -----------------------------------------------------------------------------
// HashSet - thin wrapper around HashMap with a dummy value type.
// -----------------------------------------------------------------------------
template <typename Key, typename HashFn = Hash<Key>>
class HashSet {
public:
    bool insert(const Key& key) {
        return map_.insert(key, char{0}).second;
    }
    bool contains(const Key& key) const { return map_.contains(key); }
    void erase(const Key& key) { map_.erase(key); }
    size_t size() const { return map_.size(); }
    bool empty() const { return map_.empty(); }
    void clear() { map_.clear(); }

private:
    HashMap<Key, char, HashFn> map_;
};

}  // namespace v12

#endif  // V12_BASE_HASH_MAP_H_
