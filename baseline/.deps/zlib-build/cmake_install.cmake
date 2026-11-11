# Install script for directory: /Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/src/zlib-1.3.1

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix")
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
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib/libz.1.3.1.dylib;/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib/libz.1.dylib")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib" TYPE SHARED_LIBRARY FILES
    "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/zlib-build/libz.1.3.1.dylib"
    "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/zlib-build/libz.1.dylib"
    )
  foreach(file
      "$ENV{DESTDIR}/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib/libz.1.3.1.dylib"
      "$ENV{DESTDIR}/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib/libz.1.dylib"
      )
    if(EXISTS "${file}" AND
       NOT IS_SYMLINK "${file}")
      if(CMAKE_INSTALL_DO_STRIP)
        execute_process(COMMAND "/usr/bin/strip" -x "${file}")
      endif()
    endif()
  endforeach()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib/libz.dylib")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib" TYPE SHARED_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/zlib-build/libz.dylib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib/libz.a")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib" TYPE STATIC_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/zlib-build/libz.a")
  if(EXISTS "$ENV{DESTDIR}/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib/libz.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib/libz.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/lib/libz.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/include/zconf.h;/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/include/zlib.h")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/include" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/zlib-build/zconf.h"
    "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/src/zlib-1.3.1/zlib.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/share/man/man3/zlib.3")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/share/man/man3" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/src/zlib-1.3.1/zlib.3")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  list(APPEND CMAKE_ABSOLUTE_DESTINATION_FILES
   "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/share/pkgconfig/zlib.pc")
  if(CMAKE_WARN_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(WARNING "ABSOLUTE path INSTALL DESTINATION : ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  if(CMAKE_ERROR_ON_ABSOLUTE_INSTALL_DESTINATION)
    message(FATAL_ERROR "ABSOLUTE path INSTALL DESTINATION forbidden (by caller): ${CMAKE_ABSOLUTE_DESTINATION_FILES}")
  endif()
  file(INSTALL DESTINATION "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/prefix/share/pkgconfig" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/zlib-build/zlib.pc")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/zlib-build/install_local_manifest.txt"
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
  file(WRITE "/Users/lzy/pro/UserFB/workspace/X1068/attempts/20261111T152026502570Z/repo/baseline/.deps/zlib-build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
