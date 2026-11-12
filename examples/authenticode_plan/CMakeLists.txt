cmake_minimum_required(VERSION 3.12 FATAL_ERROR)
project(authenticode_plan)

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Default install directory" FORCE)
endif ()

if (MSVC)
  list(APPEND AUTHENTICODE_PLAN_CXXFLAGS /W4 /WX /analyze)

  if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    list(APPEND AUTHENTICODE_PLAN_CXXFLAGS /Zi)
  endif ()

else ()
  set(CMAKE_CXX_STANDARD 11)
  set(CMAKE_CXX_EXTENSIONS OFF)

  list(APPEND AUTHENTICODE_PLAN_CXXFLAGS
    -pedantic -Wall -Wextra -Wcast-align -Wcast-qual -Wctor-dtor-privacy -Wdisabled-optimization
    -Wformat=2 -Winit-self -Wlong-long -Wmissing-declarations -Wmissing-include-dirs -Wcomment
    -Wold-style-cast -Woverloaded-virtual -Wredundant-decls -Wshadow -Wsign-conversion
    -Wsign-promo -Wstrict-overflow=5 -Wswitch-default -Wundef -Werror -Wunused -Wuninitialized
    -Wno-missing-declarations
  )

  if (CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    list(APPEND AUTHENTICODE_PLAN_CXXFLAGS -gdwarf-2 -g3)
  endif ()
endif ()

find_package(pe-parse REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} pe-parse::pe-parse)
target_compile_options(${PROJECT_NAME} PRIVATE ${AUTHENTICODE_PLAN_CXXFLAGS})
install(TARGETS ${PROJECT_NAME} DESTINATION "bin")
