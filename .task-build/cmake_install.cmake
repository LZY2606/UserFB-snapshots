# Install script for directory: /Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo

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
    set(CMAKE_INSTALL_CONFIG_NAME "")
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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/capstone" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/arm64.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/arm.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/capstone.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/thumb_ir.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/evm.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/wasm.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/mips.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/ppc.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/x86.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/sparc.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/systemz.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/xcore.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/m68k.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/tms320c64x.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/m680x.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/mos65xx.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/bpf.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/riscv.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/sh.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/tricore.h"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/include/capstone/platform.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/.task-build/capstone.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/.task-build/capstone-config.cmake"
    "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/.task-build/capstone-config-version.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/.task-build/libcapstone.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcapstone.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcapstone.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libcapstone.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone/capstone-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone/capstone-targets.cmake"
         "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/.task-build/CMakeFiles/Export/a9b5d84ad64c19ad8b017b7d94696c98/capstone-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone/capstone-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone/capstone-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/.task-build/CMakeFiles/Export/a9b5d84ad64c19ad8b017b7d94696c98/capstone-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^()$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/capstone" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/.task-build/CMakeFiles/Export/a9b5d84ad64c19ad8b017b7d94696c98/capstone-targets-noconfig.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/.task-build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Z1652/attempts/20261113T235915239563Z/repo/.task-build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
