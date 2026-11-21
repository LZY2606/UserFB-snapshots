# Install script for directory: /Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/.task-build/production/upstream/libsndfile.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libsndfile.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libsndfile.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libsndfile.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/include/sndfile.h"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/include/sndfile.hh"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/libsndfile" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/index.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/libsndfile.jpg"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/libsndfile.css"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/print.css"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/api.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/command.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/bugs.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/formats.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/sndfile_info.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/new_file_type_howto.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/win32.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/FAQ.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/lists.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/embedded_files.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/octave.md"
    "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/docs/tutorial.md"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Z02452/attempts/20261121T151845074512Z/repo/.task-build/production/upstream/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
