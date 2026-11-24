# C++ examples
#
# Enabled by -DOTIO_CXX_EXAMPLES=ON.  These are plain C++ programs that link
# against the opentimelineio library (no Python bindings required).

set(OTIO_CXX_EXAMPLE_TARGETS
    retimed_clip_example)

add_executable(retimed_clip_example retimedClipExample.cpp)
target_link_libraries(retimed_clip_example PRIVATE opentimelineio)
target_include_directories(retimed_clip_example PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(retimed_clip_example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
