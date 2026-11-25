cmake_minimum_required(VERSION 3.12 FATAL_ERROR)
project(reloc_preview_demo)

# Standalone example, mirroring examples/peaddrconv: it consumes an installed
# or otherwise discoverable pe-parse package and uses only the C++ standard
# library otherwise.
if (MSVC)
  set(DEMO_CXXFLAGS /W4 /WX)
else ()
  set(CMAKE_CXX_STANDARD 11)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(DEMO_CXXFLAGS -Wall -Wextra -pedantic)
endif ()

find_package(pe-parse REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} pe-parse::pe-parse)
target_compile_options(${PROJECT_NAME} PRIVATE ${DEMO_CXXFLAGS})
