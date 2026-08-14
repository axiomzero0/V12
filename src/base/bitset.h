// =============================================================================
// src/base/bitset.h
// =============================================================================
// Compact bitset with dynamic sizing. Used for:
//   - Use lists on IR nodes (which inputs are live)
//   - Live-set / reaching-def bitsets in the optimizer
//   - Register liveness in the register allocator
//
// Storage policy: a Bitset is sized to a maximum number of bits at
// construction time. We store bits in 64-bit words ("Words"). Bits beyond
// the size are always 0 and reading them is well-defined.
//
// This is deliberately NOT std::bitset<N> because N is dynamic here.

#ifndef V12_BASE_BITSET_H_
#define V12_BASE_BITSET_H_

#include <cstdint>
#include <cstring>
#include <utility>

#include "base/arena.h"
#include "base/macros.h"

namespace v12 {

class Bitset {
public:
    using Word = uint64_t;
    static constexpr size_t kBitsPerWord = 64;

    Bitset() : bits_(nullptr), num_bits_(0), num_words_(0) {}

    Bitset(Arena* arena, size_t num_bits)
        : num_bits_(num_bits),
          num_words_((num_bits + kBitsPerWord - 1) / kBitsPerWord) {
        bits_ = arena->NewArray<Word>(num_words_ == 0 ? 1 : num_words_);
        if (num_words_ == 0) num_words_ = 1;
    }

    size_t size() const { return num_bits_; }
    bool empty() const { return num_bits_ == 0; }

    bool Get(size_t i) const {
        V12_DCHECK(i < num_bits_, "bit index out of range");
        return (bits_[i / kBitsPerWord] >> (i % kBitsPerWord)) & 1;
    }

    void Set(size_t i) {
        V12_DCHECK(i < num_bits_, "bit index out of range");
        bits_[i / kBitsPerWord] |= (Word{1} << (i % kBitsPerWord));
    }

    void Clear(size_t i) {
        V12_DCHECK(i < num_bits_, "bit index out of range");
        bits_[i / kBitsPerWord] &= ~(Word{1} << (i % kBitsPerWord));
    }

    void Set(size_t i, bool value) {
        if (value) Set(i); else Clear(i);
    }

    void SetAll() {
        std::memset(bits_, 0xff, num_words_ * sizeof(Word));
        // Zero out the bits beyond num_bits_ in the last word.
        size_t tail = num_bits_ % kBitsPerWord;
        if (tail != 0) {
            bits_[num_words_ - 1] &= (Word{1} << tail) - 1;
        }
    }

    void ClearAll() {
        std::memset(bits_, 0, num_words_ * sizeof(Word));
    }

    // Number of set bits. Uses __builtin_popcountll for speed.
    size_t PopCount() const {
        size_t count = 0;
        for (size_t i = 0; i < num_words_; ++i) {
            count += __builtin_popcountll(bits_[i]);
        }
        return count;
    }

    // Returns true if any bit is set.
    bool Any() const {
        for (size_t i = 0; i < num_words_; ++i) {
            if (bits_[i] != 0) return true;
        }
        return false;
    }

    // Returns true if this and `other` have any common set bit.
    bool Intersects(const Bitset& other) const {
        V12_DCHECK(num_words_ == other.num_words_, "size mismatch");
        for (size_t i = 0; i < num_words_; ++i) {
            if (bits_[i] & other.bits_[i]) return true;
        }
        return false;
    }

    // this |= other
    void Union(const Bitset& other) {
        V12_DCHECK(num_words_ == other.num_words_, "size mismatch");
        for (size_t i = 0; i < num_words_; ++i) {
            bits_[i] |= other.bits_[i];
        }
    }

    // this &= other
    void Intersect(const Bitset& other) {
        V12_DCHECK(num_words_ == other.num_words_, "size mismatch");
        for (size_t i = 0; i < num_words_; ++i) {
            bits_[i] &= other.bits_[i];
        }
    }

    // this &= ~other
    void Subtract(const Bitset& other) {
        V12_DCHECK(num_words_ == other.num_words_, "size mismatch");
        for (size_t i = 0; i < num_words_; ++i) {
            bits_[i] &= ~other.bits_[i];
        }
    }

    // Iterate over set bits. Callback receives the bit index.
    template <typename F>
    void ForEachSet(F&& fn) const {
        for (size_t w = 0; w < num_words_; ++w) {
            Word bits = bits_[w];
            while (bits != 0) {
                int bit = __builtin_ctzll(bits);
                size_t idx = w * kBitsPerWord + bit;
                if (idx < num_bits_) fn(idx);
                bits &= bits - 1;
            }
        }
    }

    Word* words() { return bits_; }
    const Word* words() const { return bits_; }
    size_t num_words() const { return num_words_; }

private:
    Word* bits_;
    size_t num_bits_;
    size_t num_words_;
};

}  // namespace v12

#endif  // V12_BASE_BITSET_H_
