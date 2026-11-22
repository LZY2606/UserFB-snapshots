# C++ call examples for the recursive stack flattener.

add_executable(recursive_flatten_example recursive_flatten_example.cpp)
target_include_directories(
    recursive_flatten_example
    PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}")
target_link_libraries(recursive_flatten_example PRIVATE opentimelineio)
