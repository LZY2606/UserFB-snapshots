cmake_minimum_required(VERSION 2.8.4)
project(pathtracer)

if(NOT "${CMAKE_CXX_IDENTIFIER}" STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wno-c++11-narrowing -march=native -m64 -funroll-loops")
endif()

FIND_PACKAGE(OpenMP REQUIRED)
if (OPENMP_FOUND)
    message("OPENMP FOUND")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

set(SOURCE_FILES
    src/main.cpp
    src/material.cpp
    src/objects.cpp
    src/kdtree.cpp
    src/texture.cpp
    src/camera.cpp
    src/scene.cpp
    src/renderer.cpp
    src/beamfilm.cpp
    lib/rand48/rand48.c
    lib/lodepng/lodepng.cpp
    lib/tiny_obj_loader/tiny_obj_loader.cc)

add_executable(pathtracer ${SOURCE_FILES})

# Windowless analytic beam-film example.
set(EXAMPLE_FILES
    examples/beamfilm.cpp
    src/material.cpp
    src/objects.cpp
    src/kdtree.cpp
    src/texture.cpp
    src/camera.cpp
    src/scene.cpp
    src/renderer.cpp
    src/beamfilm.cpp
    lib/rand48/rand48.c
    lib/lodepng/lodepng.cpp
    lib/tiny_obj_loader/tiny_obj_loader.cc)

add_executable(beamfilm_example ${EXAMPLE_FILES})
target_include_directories(beamfilm_example PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/examples
    ${CMAKE_CURRENT_SOURCE_DIR}/lib)
if (OPENMP_FOUND)
    set_target_properties(beamfilm_example PROPERTIES
        COMPILE_FLAGS "${OpenMP_CXX_FLAGS}"
        LINK_FLAGS "${OpenMP_EXE_LINKER_FLAGS}")
endif()
