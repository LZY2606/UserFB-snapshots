# When this file is processed by the ESP-IDF build system (i.e. TinyMPG is
# used as an ESP-IDF component - either dropped into a project's components/
# dir or referenced via EXTRA_COMPONENT_DIRS), register it the IDF way and
# stop: a component CMakeLists.txt must not call project()/
# cmake_minimum_required() itself (that's the enclosing IDF project's job).
# ESP_PLATFORM is the variable ESP-IDF's build system sets for exactly this
# kind of branch - see the IDF docs, "Behind the Scenes" section:
# "The CMake variable ESP_PLATFORM is set to 1 whenever the ESP-IDF build
# system is being used. Tests such as if (ESP_PLATFORM) can be used in
# generic CMake code if special IDF-specific logic is required."
if(ESP_PLATFORM)
    idf_component_register(
        SRCS "src/encoder/mpg_inter_encode.cpp"
        INCLUDE_DIRS "src" "src/common" "src/decoder" "src/encoder"
    )
    return()
endif()

cmake_minimum_required(VERSION 3.14)
project(TinyMPG CXX)

# TinyMPG is almost entirely header-only (see src/) - the single
# exception is the optional inter encoder's one non-template translation
# unit, src/encoder/mpg_inter_encode.cpp, which this CMake target compiles
# (and the Arduino/ESP-IDF builds pick up by glob/SRCS). The primary
# distribution mechanism is the Arduino Library Manager
# (library.properties), not CMake. This file exists for three things CMake
# is actually useful for: (1) letting a non-Arduino CMake-based project
# consume the library via add_subdirectory() + target_link_libraries(),
# (2) building/running the desktop native test suite (test/native/) - this
# project's correctness oracle, and (3) - handled by the ESP_PLATFORM
# branch above - being consumed as an ESP-IDF component. None of this is
# needed for, or affects, the Arduino/arduino-cli workflow.
#
# Structure mirrors the sibling TinyH264 project (same author, same
# constrained-microcontroller target, same no-hot-path-allocation
# approach) - src/encoder/ holds the intra-only encoder plus the optional
# zero-motion P mode (see docs/scope.md's Encoder section).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# TinyMPG is header-only apart from the optional inter encoder's one
# non-template translation unit (src/encoder/mpg_inter_encode.cpp - the
# rest of the codec stays header-only inline/templates), so the CMake
# target is a small static library carrying that one source while still
# propagating the include paths and C++17 requirement to everything that
# links it. The Arduino/ESP-IDF builds compile that same .cpp directly
# (Arduino globs src/, the ESP_PLATFORM branch above names it in SRCS).
add_library(TinyMPG STATIC src/encoder/mpg_inter_encode.cpp)
add_library(TinyMPG::TinyMPG ALIAS TinyMPG)
target_include_directories(TinyMPG PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/src/decoder
    ${CMAKE_CURRENT_SOURCE_DIR}/src/encoder
)
# Propagated to consumers too: the codebase actually requires C++17 (not
# just this build of it), so a project linking against TinyMPG without
# C++17 enabled should fail to configure rather than fail with confusing
# compiler errors deep inside a header.
target_compile_features(TinyMPG PUBLIC cxx_std_17)

# Only build the test suite when this is the top-level CMake project (i.e.
# not when some other project add_subdirectory()s this one just to get the
# TinyMPG target) - portable equivalent of CMake 3.21's
# PROJECT_IS_TOP_LEVEL, kept for compatibility with the wider range of
# CMake versions this cmake_minimum_required allows.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(TINYMPG_IS_TOP_LEVEL TRUE)
else()
    set(TINYMPG_IS_TOP_LEVEL FALSE)
endif()
option(TINYMPG_BUILD_TESTS "Build the desktop native test suite (test/native/)" ${TINYMPG_IS_TOP_LEVEL})

if(TINYMPG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test/native)
endif()
