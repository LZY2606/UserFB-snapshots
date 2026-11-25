# Desktop-only test suite: see docs/testing.md. Explicit list rather than a
# glob, same rationale as TinyH264's test/native/CMakeLists.txt: a glob
# silently misses newly-added test files without a manual re-configure.
set(TINYMPG_TESTS
    test_bitreader
    test_tables
    test_headers
    test_decode_iframe
    test_decode_pframe
    test_decode_ignore_pframes
    test_decode_streaming_input
    test_decode_bframe
    test_alloc_failure
    test_encode_iframe
    test_encode_rgb
    test_decode_resumed_input
)

foreach(test_name ${TINYMPG_TESTS})
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE TinyMPG)
    add_test(NAME ${test_name} COMMAND ${test_name})
    # Every test reads its assets via relative paths (e.g. "assets/foo.m1v"),
    # so it must run with test/native/ as the working directory regardless
    # of where the build tree actually lives.
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

# Native x86 counterpart of examples/DecodeFromProgmem's and
# examples/EncodeSyntheticFrame's built-in ESP32 benchmarks (see
# bench_native.cpp) - reports real desktop timing for
# docs/memory-budget.md's Performance section. Not a correctness test, so
# not added to ctest above; build/run it manually.
add_executable(bench_native EXCLUDE_FROM_ALL bench_native.cpp)
target_link_libraries(bench_native PRIVATE TinyMPG)
target_compile_options(bench_native PRIVATE -O2)
