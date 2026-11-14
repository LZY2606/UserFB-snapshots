# Install script for directory: /Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2/codec2-config.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2/codec2-config.cmake"
         "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/.task-build/codec2/src/CMakeFiles/Export/5bd4d45600a6b952779acd0bfa505c3f/codec2-config.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2/codec2-config-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2/codec2-config.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/.task-build/codec2/src/CMakeFiles/Export/5bd4d45600a6b952779acd0bfa505c3f/codec2-config.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/codec2" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/.task-build/codec2/src/CMakeFiles/Export/5bd4d45600a6b952779acd0bfa505c3f/codec2-config-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "lib" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/.task-build/codec2/src/libcodec2.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcodec2.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcodec2.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcodec2.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "dev" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/codec2" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/codec2.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/codec2_fdmdv.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/codec2_cohpsk.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/codec2_fm.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/codec2_ofdm.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/fsk.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/codec2_fifo.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/comp.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/modem_stats.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/freedv_api.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/reliable_text.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/src/codec2_math.h"
    "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/.task-build/codec2/codec2/version.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Z1836/attempts/20261114T162834850897Z/repo/.task-build/codec2/src/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
