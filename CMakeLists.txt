# =============================================================================
# V12 - A JavaScript JIT Compiler
# =============================================================================
# Build configurations:
#   cmake -DCMAKE_BUILD_TYPE=Debug        # -O1 -g, no sanitizers
#   cmake -DCMAKE_BUILD_TYPE=Release      # -O3 -DNDEBUG
#   cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake -DCMAKE_BUILD_TYPE=Debug -DV12_ENABLE_ASAN=ON
#   cmake -DCMAKE_BUILD_TYPE=Debug -DV12_ENABLE_UBSAN=ON
#   cmake -DCMAKE_BUILD_TYPE=Debug -DV12_ENABLE_LSAN=ON
#   cmake -DCMAKE_BUILD_TYPE=Debug -DV12_ENABLE_TSAN=ON
#   cmake -DCMAKE_BUILD_TYPE=Debug -DV12_ENABLE_COVERAGE=ON
# =============================================================================

cmake_minimum_required(VERSION 3.20)

project(V12
    VERSION 0.1.0
    DESCRIPTION "A JavaScript JIT compiler with a Sea-of-Nodes IR"
    LANGUAGES CXX C
)

# -----------------------------------------------------------------------------
# Build options
# -----------------------------------------------------------------------------
option(V12_ENABLE_ASAN        "Enable AddressSanitizer"                 OFF)
option(V12_ENABLE_UBSAN       "Enable UndefinedBehaviorSanitizer"       OFF)
option(V12_ENABLE_LSAN        "Enable LeakSanitizer (standalone)"       OFF)
option(V12_ENABLE_TSAN        "Enable ThreadSanitizer"                  OFF)
option(V12_ENABLE_COVERAGE    "Enable code coverage instrumentation"   OFF)
option(V12_ENABLE_TESTS       "Build test binaries"                    ON)
option(V12_ENABLE_BENCHMARKS  "Build benchmark binaries"               OFF)
option(V12_ENABLE_TOOLS       "Build standalone tools"                 ON)
option(V12_WERROR             "Treat warnings as errors"               OFF)
option(V12_BUILD_SHARED_LIBS  "Build shared libraries"                 OFF)

# -----------------------------------------------------------------------------
# C++ standard - we use C++20 features (concepts, ranges, std::span)
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
# Default build type
# -----------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# -----------------------------------------------------------------------------
# Compiler flags
# -----------------------------------------------------------------------------
set(V12_WARNING_FLAGS
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
    -Wformat=2
    -Wmissing-include-dirs
    -Wundef
    -Wunused
    -Wno-unused-parameter  # too noisy in early development
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat-security
    -Wmisleading-indentation
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wrestrict
)

if(V12_WERROR)
    list(APPEND V12_WARNING_FLAGS -Werror)
endif()

add_library(v12_warning_flags INTERFACE)
target_compile_options(v12_warning_flags INTERFACE ${V12_WARNING_FLAGS})

# Always include debug symbols, even in Release (useful for backtraces)
add_compile_options(-g3 -fno-omit-frame-pointer)

# -----------------------------------------------------------------------------
# Sanitizer configuration
# -----------------------------------------------------------------------------
# Note: ASan + TSan are mutually exclusive. We don't enforce this in CMake
# because we want clear error messages from the runtime, not from the build
# system. The CI matrix builds each sanitizer in a separate job.
set(V12_SANITIZER_FLAGS "")

if(V12_ENABLE_ASAN)
    list(APPEND V12_SANITIZER_FLAGS -fsanitize=address -fno-common)
    message(STATUS "V12: AddressSanitizer enabled")
endif()

if(V12_ENABLE_UBSAN)
    list(APPEND V12_SANITIZER_FLAGS
        -fsanitize=undefined
        -fsanitize=float-cast-overflow
        -fsanitize=unsigned-integer-overflow
        -fno-sanitize-recover=undefined)
    message(STATUS "V12: UndefinedBehaviorSanitizer enabled")
endif()

if(V12_ENABLE_LSAN)
    # LSan is part of ASan on Linux. For a standalone LSan build we still
    # use -fsanitize=leak because that's the supported way.
    list(APPEND V12_SANITIZER_FLAGS -fsanitize=leak -fno-omit-frame-pointer)
    message(STATUS "V12: LeakSanitizer (standalone) enabled")
endif()

if(V12_ENABLE_TSAN)
    list(APPEND V12_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
    message(STATUS "V12: ThreadSanitizer enabled")
endif()

if(V12_ENABLE_COVERAGE)
    list(APPEND V12_SANITIZER_FLAGS --coverage -O0 -g)
    message(STATUS "V12: Coverage instrumentation enabled")
endif()

add_library(v12_sanitizer_flags INTERFACE)
target_compile_options(v12_sanitizer_flags INTERFACE ${V12_SANITIZER_FLAGS})
target_link_options(v12_sanitizer_flags INTERFACE ${V12_SANITIZER_FLAGS})

# -----------------------------------------------------------------------------
# Output directories - keep build clean
# -----------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# -----------------------------------------------------------------------------
# Third-party dependencies
# -----------------------------------------------------------------------------
# We use GoogleTest for unit testing. We don't vendor it - we either find it
# or fetch it via FetchContent. This keeps the repo lean.
include(cmake/Dependencies.cmake)

# -----------------------------------------------------------------------------
# Target architecture detection
# -----------------------------------------------------------------------------
include(cmake/Target.cmake)

# -----------------------------------------------------------------------------
# Subdirectories
# -----------------------------------------------------------------------------
add_subdirectory(src)

if(V12_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(V12_ENABLE_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(V12_ENABLE_TOOLS)
    add_subdirectory(tools)
endif()

# -----------------------------------------------------------------------------
# Install rules (kept minimal - this is primarily a dev project)
# -----------------------------------------------------------------------------
include(GNUInstallDirs)

# -----------------------------------------------------------------------------
# Configuration summary
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== V12 Configuration Summary ===")
message(STATUS "  Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:         ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Target arch:      ${V12_TARGET_ARCH}")
message(STATUS "  ASan:             ${V12_ENABLE_ASAN}")
message(STATUS "  UBSan:            ${V12_ENABLE_UBSAN}")
message(STATUS "  LSan:             ${V12_ENABLE_LSAN}")
message(STATUS "  TSan:             ${V12_ENABLE_TSAN}")
message(STATUS "  Coverage:         ${V12_ENABLE_COVERAGE}")
message(STATUS "  Tests:            ${V12_ENABLE_TESTS}")
message(STATUS "  Benchmarks:       ${V12_ENABLE_BENCHMARKS}")
message(STATUS "  Tools:            ${V12_ENABLE_TOOLS}")
message(STATUS "  Werror:           ${V12_WERROR}")
message(STATUS "===================================")
message(STATUS "")
