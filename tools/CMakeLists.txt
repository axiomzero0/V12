# =============================================================================
# tools/CMakeLists.txt
# =============================================================================

# js-shell: a REPL for running JS code through the interpreter.
add_executable(js-shell js-shell/js-shell.cc)
target_link_libraries(js-shell PRIVATE V12::v12 v12_warning_flags v12_sanitizer_flags)

# bytecode-dump: dump bytecode for a JS file.
add_executable(bytecode-dump bytecode-dump/bytecode-dump.cc)
target_link_libraries(bytecode-dump PRIVATE V12::v12 v12_warning_flags v12_sanitizer_flags)

# ir-dump: dump the IR for a JS function.
add_executable(ir-dump ir-dump/ir-dump.cc)
target_link_libraries(ir-dump PRIVATE V12::v12 v12_warning_flags v12_sanitizer_flags)
