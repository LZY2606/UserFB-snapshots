cmake_minimum_required(VERSION 3.15)
project(task_audio C)
set(CMAKE_C_STANDARD 99)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
  # The task build selects the portable/generic QMF sources on this host.
  # AppleClang predefines __ARM_NEON__, which would otherwise make that
  # translation unit empty and leave the scalar helper symbols undefined.
  add_compile_options(-U__ARM_NEON__)
endif()
set(CMAKE_STATIC_LIBRARY_PREFIX "")
get_filename_component(XAAC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include_directories(${XAAC_ROOT}/common ${XAAC_ROOT}/encoder ${XAAC_ROOT}/encoder/drc_src)
add_compile_definitions(_X86_ LOUDNESS_LEVELING_SUPPORT)
include(${XAAC_ROOT}/common/common.cmake)
include(${XAAC_ROOT}/encoder/libxaacenc.cmake)
file(GLOB extra CONFIGURE_DEPENDS "${XAAC_ROOT}/encoder/task_*.c")
if(extra)
  target_sources(libxaacenc PRIVATE ${extra})
endif()

include(${XAAC_ROOT}/decoder/libxaacdec.cmake)
