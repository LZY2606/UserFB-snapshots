cmake_minimum_required(VERSION 2.8.12)

set(ENABLE_TESTS OFF CACHE BOOL "Build test binaries, needs libsndfile")
set(ENABLE_FUZZER OFF CACHE BOOL "Build fuzzer binary")

# The mix preview test and example only need the library itself.
include_directories(${EBUR128_INCLUDE_DIR})
add_executable(mix-preview-test mix_preview_test)
add_executable(mix-preview-example mix_preview_example)
if(MATH_LIBRARY)
  target_link_libraries(mix-preview-test ebur128 ${MATH_LIBRARY})
  target_link_libraries(mix-preview-example ebur128 ${MATH_LIBRARY})
else()
  target_link_libraries(mix-preview-test ebur128)
  target_link_libraries(mix-preview-example ebur128)
endif()

if(ENABLE_TESTS)
  find_package(PkgConfig REQUIRED)
  find_pkg_config(SNDFILE sndfile REQUIRED)

  include_directories(${EBUR128_INCLUDE_DIR})
  include_directories(SYSTEM ${SNDFILE_INCLUDE_DIRS})

  add_executable(r128-test-library tests)
  add_executable(minimal-example minimal-example)
  set_property(TARGET r128-test-library APPEND_STRING PROPERTY
      COMPILE_FLAGS " ${SNDFILE_CFLAGS}")
  set_property(TARGET minimal-example APPEND_STRING PROPERTY
      COMPILE_FLAGS " ${SNDFILE_CFLAGS}")
  target_link_libraries(r128-test-library ebur128 ${SNDFILE_LIBRARIES})
  target_link_libraries(minimal-example ebur128 ${SNDFILE_LIBRARIES})
endif()

if(ENABLE_FUZZER)
  include_directories(${EBUR128_INCLUDE_DIR})

  add_executable(fuzzer fuzzer)
  target_link_libraries(fuzzer ebur128)
  target_compile_options(fuzzer PUBLIC "${FUZZER_FLAGS}")
  target_link_libraries(fuzzer "${FUZZER_FLAGS}")
endif()
