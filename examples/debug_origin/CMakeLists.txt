cmake_minimum_required(VERSION 3.12 FATAL_ERROR)
project(debug_origin)

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}" CACHE PATH "Default install directory" FORCE)
endif ()

if (NOT MSVC)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_EXTENSIONS OFF)
endif ()

if (NOT TARGET pe-parse::pe-parse)
  find_package(pe-parse REQUIRED)
endif()

add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} pe-parse::pe-parse)

install(TARGETS ${PROJECT_NAME} DESTINATION "bin")
