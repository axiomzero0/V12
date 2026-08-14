# =============================================================================
# src/CMakeLists.txt
# =============================================================================
# We build one monolithic static library `v12` for the whole compiler.
# In a perfect world this would be split into v12_base, v12_frontend, v12_ir,
# v12_codegen, etc., to enforce dependency direction. For now we keep it
# monolithic but the source layout still enforces the layered architecture:
#
#   base  <-  frontend  <-  vm  <-  interpreter  <-  feedback  <-  ic
#                                                              <-  jit
#                                                              <-  ir
#                                                              <-  optimizer
#                                                              <-  lowering
#                                                              <-  codegen
#                                                              <-  deopt
#                                                              <-  gc
#                                                              <-  diagnostics
#
# `contracts/` is a header-only pseudo-layer: it contains the abstract
# interfaces that adapters plug into. It has no .cc files.

# -----------------------------------------------------------------------------
# Collect sources
# -----------------------------------------------------------------------------
file(GLOB_RECURSE V12_SOURCES_CONFIGURE
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# Don't compile files in *_test.cc separately into the main lib
list(FILTER V12_SOURCES_CONFIGURE EXCLUDE REGEX "_test\\.cc$")

add_library(v12 ${V12_SOURCES_CONFIGURE})
add_library(V12::v12 ALIAS v12)

target_include_directories(v12 PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(v12
    PUBLIC
        v12_warning_flags
        v12_sanitizer_flags
        v12_target_iface
    PRIVATE
        ${CMAKE_THREAD_LIBS_INIT}
)

# Threads
find_package(Threads REQUIRED)
target_link_libraries(v12 PUBLIC Threads::Threads)

# -----------------------------------------------------------------------------
# Allow compiling individual subsystems for granular testing
# -----------------------------------------------------------------------------
# (Useful when iterating on the IR without pulling in the VM.)
add_library(v12_base INTERFACE)
target_include_directories(v12_base INTERFACE ${CMAKE_SOURCE_DIR}/src)
