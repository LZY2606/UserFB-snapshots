find_package(Catch2 CONFIG REQUIRED)

add_executable(tests "test.cpp")
target_link_libraries(tests PRIVATE fg::FrameGraph Catch2::Catch2)

# Standalone subresource-range tests (own main, no Catch2 dependency).
add_executable(ranges "ranges.cpp")
target_link_libraries(ranges PRIVATE fg::FrameGraph)

include(CTest)
include(Catch)
catch_discover_tests(tests)
