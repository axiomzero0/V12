# =============================================================================
# Dependencies.cmake - Third-party dependency resolution
# =============================================================================
#
# Philosophy: we keep the repo LEAN. No vendored third-party code. Everything
# is either fetched on demand or expected to be system-provided.
#
# Current dependencies:
#   - GoogleTest   (test framework, fetched only when V12_ENABLE_TESTS=ON)
#
# External LIBRARIES that we *link against* (not vendor) at runtime:
#   - CraneLift/Rainbow/linear-regalloc   (register allocation)
#   - asmjit/Xbyak                        (machine code emission)
#   - concurrency libs (Taskflow/HPX)     (parallel compilation)
#
# These are NOT fetched here. They are expected to be found via find_package
# in their respective adapter CMakeLists. This enforces the "adapter boundary"
# rule: the core compiler never depends on a specific machine-code library.

include(FetchContent)

if(V12_ENABLE_TESTS)
    # Don't re-fetch if already available system-wide
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "GoogleTest not found system-wide; fetching from source.")
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
        )
        # On Windows we need gtest_force_shared_crt, but this is a Linux-first
        # project; the option is harmless elsewhere.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    else()
        message(STATUS "GoogleTest found system-wide.")
    endif()
endif()

# -----------------------------------------------------------------------------
# find_package for our three adapter-boundary libraries. These are OPTIONAL
# at CMake time; the adapters will fall back to a stub implementation when
# not present, so the rest of the compiler can build and be tested without
# the actual machine-code library.
# -----------------------------------------------------------------------------

# asmjit - https://github.com/asmjit/asmjit
find_package(asmjit CONFIG QUIET)
if(asmjit_FOUND)
    message(STATUS "asmjit found - machine emitter adapter will use it.")
else()
    message(STATUS "asmjit NOT found - machine emitter adapter will use stub.")
endif()

# We don't yet pin a specific regalloc library. The adapter is the contract.
# When we plug one in, its find_package call goes here.
