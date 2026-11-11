cmake_minimum_required(VERSION 3.12...3.26)

get_property(is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT is_multi_config AND NOT (CMAKE_BUILD_TYPE OR DEFINED ENV{CMAKE_BUILD_TYPE}))
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Release default")
endif()

project(tinyrenderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

option(iwyu "Run include-what-you-use")
if(iwyu)
  find_program(IWYU_EXE NAMES include-what-you-use REQUIRED)
  set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE ${IWYU_EXE})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU|Intel")
  add_compile_options(-Wall)
endif()

find_package(OpenMP COMPONENTS CXX)

set(SOURCES main.cpp our_gl.cpp model.cpp tgaimage.cpp)

add_executable(${PROJECT_NAME} ${SOURCES})
target_link_libraries(${PROJECT_NAME} PRIVATE $<$<BOOL:${OpenMP_CXX_FOUND}>:OpenMP::OpenMP_CXX>)

file(GENERATE OUTPUT .gitignore CONTENT "*")

# Public synthetic baseline example, added for the clipped-pipeline work.
include(CTest)
if(BUILD_TESTING)
  add_executable(pipeline_smoke examples/pipeline_smoke.cpp our_gl.cpp tgaimage.cpp)
  target_include_directories(pipeline_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(pipeline_smoke PRIVATE $<$<BOOL:${OpenMP_CXX_FOUND}>:OpenMP::OpenMP_CXX>)
  add_test(NAME pipeline_smoke COMMAND pipeline_smoke)
endif()
