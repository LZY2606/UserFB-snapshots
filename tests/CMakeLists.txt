# Regression tests.  These use libde265's internal headers/objects (the
# predicted blocks and the border helper are internal), so the library must be
# built without reduced symbol visibility (the top-level CMakeLists does that
# when ENABLE_TESTS is on).  Run them with `ctest` from the build directory.
add_executable(intra_border_reference
  intra_border_reference.cc
  intra_border_refmodel.cc
)
target_link_libraries(intra_border_reference PRIVATE de265 Threads::Threads)

add_test(NAME intra_border_reference COMMAND intra_border_reference)
