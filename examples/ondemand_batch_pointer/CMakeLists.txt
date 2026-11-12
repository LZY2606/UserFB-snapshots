# ondemand::pointer_batch projection example.
#
# Builds like the quickstart examples: one small translation unit linked
# against the simdjson target, compiled with warnings as errors.

add_executable(batch_pointer_projection batch_pointer_projection.cpp)
target_link_libraries(batch_pointer_projection PRIVATE simdjson)
if(NOT MSVC)
  target_compile_options(batch_pointer_projection PRIVATE -Werror)
endif()

add_test(
  NAME batch_pointer_projection
  COMMAND batch_pointer_projection
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/examples/ondemand_batch_pointer
)
set_property(
  TEST batch_pointer_projection
  APPEND PROPERTY LABELS acceptance ondemand examples
)
