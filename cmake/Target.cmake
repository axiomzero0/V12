# =============================================================================
# Target.cmake - Host/target architecture detection
# =============================================================================
#
# We need to know the target architecture because:
#   - The machine IR has target-specific register files.
#   - Calling conventions differ per ISA.
#   - Codegen needs to know pointer size and endian-ness.
#
# We do NOT bake the target into source code. Source uses V12_TARGET_ARCH to
# select which header under src/contracts/ to include.

set(V12_TARGET_ARCH "unknown" CACHE STRING "Target architecture (x64, arm64, riscv64, wasm)")
set(V12_TARGET_ARCH_VALUES x64 arm64 riscv64 wasm)

# Auto-detect from host if user didn't specify
if(V12_TARGET_ARCH STREQUAL "unknown")
    set(_host_arch ${CMAKE_HOST_SYSTEM_PROCESSOR})
    if(_host_arch STREQUAL "x86_64" OR _host_arch STREQUAL "AMD64")
        set(V12_TARGET_ARCH "x64")
    elseif(_host_arch STREQUAL "aarch64" OR _host_arch STREQUAL "arm64")
        set(V12_TARGET_ARCH "arm64")
    elseif(_host_arch STREQUAL "riscv64")
        set(V12_TARGET_ARCH "riscv64")
    else()
        message(WARNING
            "Unknown host architecture '${_host_arch}'. "
            "Set -DV12_TARGET_ARCH=<x64|arm64|riscv64|wasm> explicitly.")
    endif()
endif()

set_property(CACHE V12_TARGET_ARCH PROPERTY STRINGS ${V12_TARGET_ARCH_VALUES})

# Expose as a compile definition so source code can switch on it.
add_library(v12_target_iface INTERFACE)
target_compile_definitions(v12_target_iface INTERFACE
    V12_TARGET_ARCH_${V12_TARGET_ARCH}=1
    V12_TARGET_ARCH_STR="${V12_TARGET_ARCH}"
)

# Pointer size and endian-ness
include(TestBigEndian)
test_big_endian(V12_BIG_ENDIAN)
if(V12_BIG_ENDIAN)
    target_compile_definitions(v12_target_iface INTERFACE V12_BIG_ENDIAN=1)
else()
    target_compile_definitions(v12_target_iface INTERFACE V12_LITTLE_ENDIAN=1)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(v12_target_iface INTERFACE V12_PTR_SIZE=8)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_compile_definitions(v12_target_iface INTERFACE V12_PTR_SIZE=4)
else()
    message(FATAL_ERROR "Unsupported pointer size: ${CMAKE_SIZEOF_VOID_P}")
endif()
