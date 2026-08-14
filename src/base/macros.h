// =============================================================================
// src/base/macros.h
// =============================================================================
// Common macros used throughout the codebase. Keep this lean - every macro
// here must earn its place.

#ifndef V12_BASE_MACROS_H_
#define V12_BASE_MACROS_H_

#include <cassert>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

// -----------------------------------------------------------------------------
// Compiler detection
// -----------------------------------------------------------------------------
#if defined(__clang__)
    #define V12_COMPILER_CLANG 1
#elif defined(__GNUC__)
    #define V12_COMPILER_GCC 1
#elif defined(_MSC_VER)
    #define V12_COMPILER_MSVC 1
#else
    #define V12_COMPILER_UNKNOWN 1
#endif

// -----------------------------------------------------------------------------
// Platform detection
// -----------------------------------------------------------------------------
#if defined(__linux__)
    #define V12_OS_LINUX 1
#elif defined(__APPLE__)
    #define V12_OS_MACOS 1
#elif defined(_WIN32)
    #define V12_OS_WINDOWS 1
#endif

// -----------------------------------------------------------------------------
// Attributes
// -----------------------------------------------------------------------------
#if defined(V12_COMPILER_CLANG) || defined(V12_COMPILER_GCC)
    #define V12_NODISCARD            [[nodiscard]]
    #define V12_ALWAYS_INLINE        __attribute__((always_inline)) inline
    #define V12_NEVER_INLINE         __attribute__((noinline))
    #define V12_LIKELY(cond)         __builtin_expect(!!(cond), 1)
    #define V12_UNLIKELY(cond)       __builtin_expect(!!(cond), 0)
    #define V12_ASSUME_ALIGNED(ptr, align) \
        static_cast<std::remove_reference_t<decltype(ptr)>>(__builtin_assume_aligned(ptr, align))
    #define V12_UNREACHABLE()        __builtin_unreachable()
    #define V12_PRETTY_FUNCTION      __PRETTY_FUNCTION__
#elif defined(V12_COMPILER_MSVC)
    #define V12_NODISCARD            [[nodiscard]]
    #define V12_ALWAYS_INLINE        __forceinline
    #define V12_NEVER_INLINE         __declspec(noinline)
    #define V12_LIKELY(cond)         (cond)
    #define V12_UNLIKELY(cond)       (cond)
    #define V12_ASSUME_ALIGNED(ptr, align) (ptr)
    #define V12_UNREACHABLE()        __assume(false)
    #define V12_PRETTY_FUNCTION      __FUNCSIG__
#else
    #define V12_NODISCARD
    #define V12_ALWAYS_INLINE        inline
    #define V12_NEVER_INLINE
    #define V12_LIKELY(cond)         (cond)
    #define V12_UNLIKELY(cond)       (cond)
    #define V12_ASSUME_ALIGNED(ptr, align) (ptr)
    #define V12_UNREACHABLE()        std::abort()
    #define V12_PRETTY_FUNCTION      __func__
#endif

// -----------------------------------------------------------------------------
// Bit utilities
// -----------------------------------------------------------------------------
namespace v12 {

template <typename T>
constexpr bool IsPowerOfTwo(T x) {
    static_assert(std::is_unsigned_v<T>, "IsPowerOfTwo requires unsigned");
    return x != 0 && (x & (x - 1)) == 0;
}

template <typename T>
constexpr T AlignUp(T x, T align) {
    static_assert(std::is_unsigned_v<T>, "AlignUp requires unsigned");
    return (x + align - 1) & ~(align - 1);
}

template <typename T>
constexpr T AlignDown(T x, T align) {
    static_assert(std::is_unsigned_v<T>, "AlignDown requires unsigned");
    return x & ~(align - 1);
}

constexpr uintptr_t kPointerAlignment = alignof(void*);

constexpr size_t BitsNeeded(uint64_t x) {
    return x == 0 ? 0 : 64 - __builtin_clzll(x);
}

constexpr bool IsAligned(uintptr_t x, size_t align) {
    return (x & (align - 1)) == 0;
}

}  // namespace v12

// -----------------------------------------------------------------------------
// Assertions
// -----------------------------------------------------------------------------
// V12_DCHECK: debug-only assertion (compiled out in NDEBUG builds).
// V12_CHECK:  always-on assertion (for invariants that matter even in release).
//
// We deliberately don't use a generic "ASSERT" macro because the runtime
// semantics differ: a DCHECK failure indicates a compiler bug, while a
// CHECK failure might indicate a runtime invariant violation that should
// crash the process.
//
// In a JIT compiler, a CHECK failure often means we have to bail out
// (deoptimize) rather than abort. The IR verifier uses V12_CHECK because
// verifier failures are always compiler bugs.

#ifndef NDEBUG
    #define V12_DCHECK(cond, ...) \
        do { \
            if (V12_UNLIKELY(!(cond))) { \
                ::v12::internal::Die("DCHECK failed: %s\n  at %s:%d in %s\n  ", \
                    #cond, __FILE__, __LINE__, V12_PRETTY_FUNCTION); \
            } \
        } while (0)
#else
    #define V12_DCHECK(cond, ...) do { } while (0)
#endif

#define V12_CHECK(cond, ...) \
    do { \
        if (V12_UNLIKELY(!(cond))) { \
            ::v12::internal::Die("CHECK failed: %s\n  at %s:%d in %s\n  ", \
                #cond, __FILE__, __LINE__, V12_PRETTY_FUNCTION); \
        } \
    } while (0)

#define V12_UNREACHABLE_DCHECK() \
    do { \
        V12_DCHECK(false, "unreachable"); \
        V12_UNREACHABLE(); \
    } while (0)

#define V12_FAIL(...) ::v12::internal::Die(__VA_ARGS__)

// -----------------------------------------------------------------------------
// Diagnostic / abort machinery
// -----------------------------------------------------------------------------
namespace v12::internal {

[[noreturn]] inline void Die(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::abort();
}

}  // namespace v12::internal

#endif  // V12_BASE_MACROS_H_
