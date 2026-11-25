# https://github.com/dominiksalvet/cpurast

cmake_minimum_required(VERSION 3.0.0)
project(shaders CXX)

add_library(shaders
    lean_shader.cpp
)
target_include_directories(shaders PRIVATE
    ${CPURAST_INCLUDE_DIR}
)

set(SHADERS_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
