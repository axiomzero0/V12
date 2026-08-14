# =============================================================================
# tests/CMakeLists.txt
# =============================================================================
# We use GoogleTest. Each test file is a separate executable so we can run
# them individually and see clear pass/fail.

# Collect all test sources
file(GLOB_RECURSE V12_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)

# Per-subsystem test targets. Each test links against v12 + gtest.
foreach(test_source ${V12_TEST_SOURCES})
    # Derive a target name from the source path.
    file(RELATIVE_PATH rel_path "${CMAKE_CURRENT_SOURCE_DIR}" "${test_source}")
    string(REPLACE "/" "_" target_name "v12_test_${rel_path}")
    string(REPLACE "." "_" target_name "${target_name}")

    add_executable(${target_name} ${test_source})
    target_link_libraries(${target_name}
        PRIVATE
            V12::v12
            gtest
            gtest_main
    )

    # Sanitizer flags
    target_link_libraries(${target_name}
        PRIVATE
            v12_sanitizer_flags
            v12_warning_flags
    )

    # Register with ctest
    gtest_discover_tests(${target_name})
endforeach()
