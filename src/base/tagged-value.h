// =============================================================================
// src/base/tagged-value.h
// =============================================================================
// Tagged pointer / tagged immediate support for the VM.
//
// V8 popularized the idea of stuffing type information into the low bits
// of a machine word:
//
//   - For a pointer to a heap object: the low bit is 0 (heap objects are
//     4-byte or 8-byte aligned, so the low bits are always 0).
//   - For a small integer (Smi): the low bit is 1. The integer value is
//     shifted left by 1.
//
// We adopt the same scheme because it gives us:
//   - Free type discrimination on every value (one AND instruction).
//   - No allocation for small integers (the dominant case in JS).
//   - Compatible with GC (the GC only sees heap pointers, never Smis).
//
// TaggedValue is a thin wrapper around uintptr_t. It is intentionally
// trivially copyable and standard-layout so it can be stored in registers
// by the C++ compiler.
//
// We use this both in the interpreter's value stack and in the IR's value
// representation. The IR uses a higher-level Type system (see ir/types/),
// but the underlying representation is still TaggedValue.

#ifndef V12_BASE_TAGGED_VALUE_H_
#define V12_BASE_TAGGED_VALUE_H_

#include <cstdint>
#include <cstring>
#include <limits>

#include "base/macros.h"

namespace v12 {

class HeapObject;

// -----------------------------------------------------------------------------
// Tag bits
// -----------------------------------------------------------------------------
// Tag layout (little-endian):
//   bit 0 = 0  -> heap pointer (HeapObject*)
//   bit 0 = 1  -> Smi (small integer)
//
// We use only 1 tag bit so that Smi arithmetic remains efficient on x86.
// If we needed more types (e.g. boxed doubles), we'd add a second tag bit
// and reserve bit 1 as a "boxed" indicator - this is what V8 calls "Tagged
// doubles" or "HeapNumber".
//
namespace detail {
constexpr uintptr_t kHeapObjectTag  = 0;
constexpr uintptr_t kSmiTag          = 1;
constexpr uintptr_t kTagMask         = 1;
constexpr int       kSmiShift        = 1;
constexpr int       kSmiValueBits    = sizeof(uintptr_t) * 8 - 1;
}  // namespace detail

class TaggedValue {
public:
    TaggedValue() : bits_(0) {}

    // ----- Smi constructors -----
    static TaggedValue Smi(intptr_t value) {
        // Smis can hold integers in the range [-2^30, 2^30-1] on 32-bit,
        // [-2^62, 2^62-1] on 64-bit. We CHECK this on construction.
        V12_CHECK(value >= -(intptr_t{1} << (detail::kSmiValueBits - 1)) &&
                  value <   (intptr_t{1} << (detail::kSmiValueBits - 1)),
                  "Smi overflow: %lld", static_cast<long long>(value));
        TaggedValue v;
        v.bits_ = (static_cast<uintptr_t>(value) << detail::kSmiShift) | detail::kSmiTag;
        return v;
    }

    // ----- HeapObject constructors -----
    static TaggedValue Heap(HeapObject* obj) {
        TaggedValue v;
        uintptr_t p = reinterpret_cast<uintptr_t>(obj);
        V12_CHECK((p & detail::kTagMask) == detail::kHeapObjectTag,
                  "heap object pointer is not properly aligned (low bit set)");
        v.bits_ = p;
        return v;
    }

    // ----- Type queries -----
    bool IsSmi() const {
        return (bits_ & detail::kTagMask) == detail::kSmiTag;
    }

    bool IsHeapObject() const {
        return (bits_ & detail::kTagMask) == detail::kHeapObjectTag;
    }

    // ----- Value access -----
    intptr_t AsSmi() const {
        V12_DCHECK(IsSmi(), "AsSmi on non-Smi");
        return static_cast<intptr_t>(bits_) >> detail::kSmiShift;
    }

    HeapObject* AsHeapObject() const {
        V12_DCHECK(IsHeapObject(), "AsHeapObject on non-HeapObject");
        return reinterpret_cast<HeapObject*>(bits_);
    }

    // ----- Raw access (for the GC and IR) -----
    uintptr_t raw_bits() const { return bits_; }
    static TaggedValue FromRawBits(uintptr_t bits) {
        TaggedValue v; v.bits_ = bits; return v;
    }

    bool operator==(TaggedValue other) const { return bits_ == other.bits_; }
    bool operator!=(TaggedValue other) const { return bits_ != other.bits_; }

private:
    uintptr_t bits_;
};

static_assert(sizeof(TaggedValue) == sizeof(uintptr_t),
              "TaggedValue must be pointer-sized");
static_assert(std::is_trivially_copyable_v<TaggedValue>,
              "TaggedValue must be trivially copyable");

// Smi arithmetic helpers. These are used by both the interpreter and the
// optimizer when constant-folding Smi operations.
// Forward declaration
inline bool SmiFits(intptr_t value);

inline bool SmiAddOverflow(TaggedValue a, TaggedValue b, TaggedValue* result) {
    intptr_t x = a.AsSmi();
    intptr_t y = b.AsSmi();
    // We use __builtin_add_overflow when available for robust overflow detection.
#if defined(V12_COMPILER_CLANG) || defined(V12_COMPILER_GCC)
    intptr_t sum;
    if (__builtin_add_overflow(x, y, &sum)) return true;
    if (!SmiFits(sum)) return true;
    *result = TaggedValue::Smi(sum);
    return false;
#else
    // Fallback: rely on Smi range check.
    if (y > 0 && x > (intptr_t{1} << (detail::kSmiValueBits - 1)) - 1 - y) return true;
    if (y < 0 && x < -(intptr_t{1} << (detail::kSmiValueBits - 1)) - y) return true;
    *result = TaggedValue::Smi(x + y);
    return false;
#endif
}

inline bool SmiSubOverflow(TaggedValue a, TaggedValue b, TaggedValue* result) {
    intptr_t x = a.AsSmi();
    intptr_t y = b.AsSmi();
#if defined(V12_COMPILER_CLANG) || defined(V12_COMPILER_GCC)
    intptr_t diff;
    if (__builtin_sub_overflow(x, y, &diff)) return true;
    if (!SmiFits(diff)) return true;
    *result = TaggedValue::Smi(diff);
    return false;
#else
    if (y < 0 && x > (intptr_t{1} << (detail::kSmiValueBits - 1)) - 1 + y) return true;
    if (y > 0 && x < -(intptr_t{1} << (detail::kSmiValueBits - 1)) + y) return true;
    *result = TaggedValue::Smi(x - y);
    return false;
#endif
}

inline bool SmiMulOverflow(TaggedValue a, TaggedValue b, TaggedValue* result) {
    intptr_t x = a.AsSmi();
    intptr_t y = b.AsSmi();
#if defined(V12_COMPILER_CLANG) || defined(V12_COMPILER_GCC)
    intptr_t prod;
    if (__builtin_mul_overflow(x, y, &prod)) return true;
    if (!SmiFits(prod)) return true;
    *result = TaggedValue::Smi(prod);
    return false;
#else
    if (x == 0 || y == 0) { *result = TaggedValue::Smi(0); return false; }
    intptr_t prod = x * y;
    if (prod / x != y) return true;
    if (!SmiFits(prod)) return true;
    *result = TaggedValue::Smi(prod);
    return false;
#endif
}

inline bool SmiFits(intptr_t value) {
    return value >= -(intptr_t{1} << (detail::kSmiValueBits - 1)) &&
           value <   (intptr_t{1} << (detail::kSmiValueBits - 1));
}

}  // namespace v12

#endif  // V12_BASE_TAGGED_VALUE_H_
