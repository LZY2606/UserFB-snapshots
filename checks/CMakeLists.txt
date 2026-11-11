cmake_minimum_required(VERSION 3.30)
project(ozz_contact_baseline LANGUAGES CXX)
set(ozz_build_tools OFF CACHE BOOL "" FORCE)
set(ozz_build_fbx OFF CACHE BOOL "" FORCE)
set(ozz_build_gltf OFF CACHE BOOL "" FORCE)
set(ozz_build_samples OFF CACHE BOOL "" FORCE)
set(ozz_build_howtos OFF CACHE BOOL "" FORCE)
set(ozz_build_tests OFF CACHE BOOL "" FORCE)
set(ozz_build_data OFF CACHE BOOL "" FORCE)
add_subdirectory(.. ozz)

# Original baseline regression, kept untouched.
add_executable(contact_baseline baseline.cc)
target_link_libraries(contact_baseline PRIVATE ozz_animation_offline)
target_compile_features(contact_baseline PRIVATE cxx_std_17)

# Windowless contact-lock regression. It shares the exact contact_lock layer
# used by the interactive foot_ik sample, but needs no window, FBX SDK or
# media asset: skeleton, animation, tracks and ground are all procedural.
add_executable(contact_lock_regression
  contact_lock_regression.cc
  ../samples/foot_ik/contact_lock.cc)
target_link_libraries(contact_lock_regression PRIVATE ozz_animation_offline)
target_include_directories(contact_lock_regression PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../samples"
  "${CMAKE_CURRENT_SOURCE_DIR}/../samples/foot_ik")
target_compile_features(contact_lock_regression PRIVATE cxx_std_17)
