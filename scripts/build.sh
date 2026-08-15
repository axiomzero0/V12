#!/bin/bash
# =============================================================================
# scripts/build.sh
# =============================================================================
# Manual build script for environments without CMake. This is a fallback for
# development; the canonical build is via CMake (see CMakeLists.txt).
#
# Usage:
#   ./scripts/build.sh debug       # debug build with sanitizers
#   ./scripts/build.sh release     # optimized build
#   ./scripts/build.sh tests       # build and run tests
#   ./scripts/build.sh clean       # remove build artifacts

set -e

V12_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${V12_ROOT}/build"
SRC_DIR="${V12_ROOT}/src"
TESTS_DIR="${V12_ROOT}/tests"
TOOLS_DIR="${V12_ROOT}/tools"

MODE="${1:-debug}"

CXX=g++
CXXFLAGS_COMMON="-std=c++20 -I${SRC_DIR} -I${V12_ROOT} -I${V12_ROOT}/third_party/asmjit -DASMJIT_STATIC -DASMJIT_NO_FORMATTING -DASMJIT_NO_LOGGING -Wall -Wextra -Wpedantic -Wno-unused-parameter -fno-omit-frame-pointer -g3"

case "$MODE" in
    debug)
        CXXFLAGS="${CXXFLAGS_COMMON} -O0 -DV12_DEBUG=1"
        ;;
    release)
        CXXFLAGS="${CXXFLAGS_COMMON} -O3 -DNDEBUG"
        ;;
    release-opt)
        # Maximum optimization: LTO + march=native + fno-plt
        # ~1.5x faster than plain release on interpreter workloads.
        # Note: -flto=thin is Clang-only; GCC uses -flto (which is thin by default in GCC 14+).
        CXXFLAGS="${CXXFLAGS_COMMON} -O3 -DNDEBUG -flto -march=native -fno-plt"
        LDFLAGS="-flto"
        ;;
    pgo)
        # Profile-Guided Optimization: two-phase build.
        # Phase 1: build with profiling instrumentation.
        # Phase 2: run benchmark to collect profile.
        # Phase 3: rebuild using the collected profile.
        # Usage: ./scripts/build.sh pgo-gen && ./build/bin/bench && ./scripts/build.sh pgo-use
        echo "Use 'pgo-gen' then run benchmarks, then 'pgo-use'."
        exit 0
        ;;
    pgo-gen)
        CXXFLAGS="${CXXFLAGS_COMMON} -O3 -DNDEBUG -fprofile-generate -fprofile-dir=/tmp/v12-pgo"
        LDFLAGS="-fprofile-generate -fprofile-dir=/tmp/v12-pgo"
        ;;
    pgo-use)
        CXXFLAGS="${CXXFLAGS_COMMON} -O3 -DNDEBUG -flto -march=native -fno-plt -fprofile-use -fprofile-dir=/tmp/v12-pgo -fprofile-correction"
        LDFLAGS="-flto -fprofile-use -fprofile-dir=/tmp/v12-pgo -fprofile-correction"
        ;;
    asan)
        CXXFLAGS="${CXXFLAGS_COMMON} -O0 -fsanitize=address,undefined -fno-sanitize-recover=undefined -DV12_DEBUG=1"
        LDFLAGS="-fsanitize=address,undefined"
        ;;
    tests)
        CXXFLAGS="${CXXFLAGS_COMMON} -O0 -DV12_DEBUG=1"
        ;;
    clean)
        rm -rf "${BUILD_DIR}" /tmp/v12-pgo
        echo "Cleaned."
        exit 0
        ;;
    *)
        echo "Usage: $0 [debug|release|release-opt|asan|tests|pgo-gen|pgo-use|clean]"
        exit 1
        ;;
esac

mkdir -p "${BUILD_DIR}/obj" "${BUILD_DIR}/bin"

# Find all .cc source files in src/ and asmjit .cpp files.
SRCS=$(find "${SRC_DIR}" -name "*.cc" -o -name "*.cpp" | sort)
ASMJIT_SRCS=$(find "${V12_ROOT}/third_party/asmjit/asmjit/core" -name "*.cpp" | sort)
ASMJIT_SRCS="${ASMJIT_SRCS} $(find "${V12_ROOT}/third_party/asmjit/asmjit/x86" -name "*.cpp" | sort)"
ASMJIT_SRCS="${ASMJIT_SRCS} $(find "${V12_ROOT}/third_party/asmjit/asmjit/support" -name "*.cpp" | sort)"

echo "==> Compiling V12 library (${MODE})..."
OBJ_FILES=""
for src in $SRCS; do
    rel="${src#${SRC_DIR}/}"
    obj="${BUILD_DIR}/obj/${rel%.cc}.o"
    mkdir -p "$(dirname "$obj")"
    if [ "${src}" -nt "${obj}" ] || [ ! -f "${obj}" ]; then
        echo "  CC  ${rel}"
        ${CXX} ${CXXFLAGS} -c "${src}" -o "${obj}" || exit 1
    fi
    OBJ_FILES="${OBJ_FILES} ${obj}"
done

# Compile asmjit sources.
for src in $ASMJIT_SRCS; do
    rel="asmjit/$(basename "${src}")"
    obj="${BUILD_DIR}/obj/${rel%.cpp}.o"
    mkdir -p "$(dirname "$obj")"
    if [ "${src}" -nt "${obj}" ] || [ ! -f "${obj}" ]; then
        echo "  CC  ${rel}"
        ${CXX} ${CXXFLAGS} -c "${src}" -o "${obj}" || exit 1
    fi
    OBJ_FILES="${OBJ_FILES} ${obj}"
done

# Build static library.
echo "==> Archiving libv12.a..."
ar rcs "${BUILD_DIR}/libv12.a" ${OBJ_FILES}

# Build tools.
echo "==> Building tools..."

# js-shell needs builtins.cc compiled in.
echo "  LD  js-shell"
${CXX} ${CXXFLAGS} ${LDFLAGS:-} \
    "${TOOLS_DIR}/js-shell/js-shell.cc" \
    "${TOOLS_DIR}/js-shell/builtins.cc" \
    -o "${BUILD_DIR}/bin/js-shell" \
    -L"${BUILD_DIR}" -lv12 -lpthread || echo "    (failed, continuing)"

# Other tools are single-file.
for tool_src in "${TOOLS_DIR}"/bytecode-dump/bytecode-dump.cc \
                "${TOOLS_DIR}"/ir-dump/ir-dump.cc; do
    if [ -f "${tool_src}" ]; then
        tool_name=$(basename "${tool_src}" .cc)
        echo "  LD  ${tool_name}"
        ${CXX} ${CXXFLAGS} ${LDFLAGS:-} "${tool_src}" -o "${BUILD_DIR}/bin/${tool_name}" \
            -L"${BUILD_DIR}" -lv12 -lpthread || echo "    (failed, continuing)"
    fi
done

# Build tests using the custom test framework.
if [ "$MODE" = "tests" ] || [ "$MODE" = "debug" ]; then
    echo "==> Building tests..."
    TEST_SRCS=$(find "${TESTS_DIR}" -name "*_test.cc" | sort)
    ALL_TEST_OBJS=""
    for test_src in $TEST_SRCS; do
        rel="${test_src#${TESTS_DIR}/}"
        obj="${BUILD_DIR}/obj/tests/${rel%.cc}.o"
        mkdir -p "$(dirname "$obj")"
        echo "  CC  tests/${rel}"
        ${CXX} ${CXXFLAGS} -I"${TESTS_DIR}" -c "${test_src}" -o "${obj}" || exit 1
        ALL_TEST_OBJS="${ALL_TEST_OBJS} ${obj}"
    done

    # Build the test runner.
    echo "  LD  test_runner"
    ${CXX} ${CXXFLAGS} ${LDFLAGS:-} \
        "${TESTS_DIR}/test_main.cc" \
        ${ALL_TEST_OBJS} \
        -o "${BUILD_DIR}/bin/test_runner" \
        -L"${BUILD_DIR}" -lv12 -lpthread || exit 1
    echo "==> Test runner built. Run with: ${BUILD_DIR}/bin/test_runner"
fi

echo "==> Build complete. Artifacts in ${BUILD_DIR}/"
