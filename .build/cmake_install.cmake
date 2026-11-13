# Install script for directory: /Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo

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
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/aribcaption" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/include/aribcc_config.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/aribcaption" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/aribcc_export.h"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/aribcaption.h"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/aribcaption.hpp"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/caption.h"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/caption.hpp"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/color.h"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/color.hpp"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/context.h"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/context.hpp"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/decoder.h"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/include/aribcaption/decoder.hpp"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/libaribcaption.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libaribcaption.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libaribcaption.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libaribcaption.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/aribcaption/aribcaption-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/aribcaption/aribcaption-targets.cmake"
         "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/CMakeFiles/Export/512e53a79209b8fd9bcae91ba07514d6/aribcaption-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/aribcaption/aribcaption-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/aribcaption/aribcaption-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/aribcaption" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/CMakeFiles/Export/512e53a79209b8fd9bcae91ba07514d6/aribcaption-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/aribcaption" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/CMakeFiles/Export/512e53a79209b8fd9bcae91ba07514d6/aribcaption-targets-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/aribcaption" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/aribcaption-config.cmake"
    "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/aribcaption-config-version.cmake"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/libaribcaption.pc")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/install_local_manifest.txt"
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
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Y1104/attempts/20261113T180557380003Z/repo/.build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
