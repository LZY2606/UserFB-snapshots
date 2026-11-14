# Install script for directory: /Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2

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

if(CMAKE_INSTALL_COMPONENT STREQUAL "development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/libxml2/libxml" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/c14n.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/catalog.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/chvalid.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/debugXML.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/dict.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/encoding.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/entities.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/globals.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/hash.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/HTMLparser.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/HTMLtree.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/list.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/nanoftp.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/nanohttp.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/parser.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/parserInternals.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/pattern.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/relaxng.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/SAX.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/SAX2.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/schemasInternals.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/schematron.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/threads.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/tree.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/uri.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/valid.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xinclude.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xlink.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlIO.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlautomata.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlerror.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlexports.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlmemory.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlmodule.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlreader.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlregexp.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlsave.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlschemas.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlschemastypes.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlstring.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlunicode.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xmlwriter.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xpath.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xpathInternals.h"
    "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/include/libxml/xpointer.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/libxml2.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libxml2.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libxml2.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libxml2.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "documentation" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/man/man1" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/doc/xml2-config.1")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "documentation" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/man/man1" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/doc/xmlcatalog.1")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "documentation" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/man/man1" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/doc/xmllint.1")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "documentation" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/doc/libxml2" TYPE DIRECTORY FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/doc/" REGEX "/makefile\\.[^/]*$" EXCLUDE REGEX "/[^/]*\\.1$" EXCLUDE REGEX "/[^/]*\\.py$" EXCLUDE REGEX "/[^/]*\\.res$" EXCLUDE REGEX "/[^/]*\\.xml$" EXCLUDE REGEX "/[^/]*\\.xsl$" EXCLUDE)
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libxml2-2.12.10" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/libxml2-config.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libxml2-2.12.10" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/libxml2-config-version.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "development" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libxml2-2.12.10/libxml2-export.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libxml2-2.12.10/libxml2-export.cmake"
         "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/CMakeFiles/Export/fed0998c4468defa09eb11975b78cb77/libxml2-export.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libxml2-2.12.10/libxml2-export-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/libxml2-2.12.10/libxml2-export.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libxml2-2.12.10" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/CMakeFiles/Export/fed0998c4468defa09eb11975b78cb77/libxml2-export.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/libxml2-2.12.10" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/CMakeFiles/Export/fed0998c4468defa09eb11975b78cb77/libxml2-export-release.cmake")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/libxml2/libxml" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/libxml/xmlversion.h")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/libxml-2.0.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "development" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE PROGRAM FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/xml2-config")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/aclocal" TYPE FILE FILES "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/vendor/libxml2/libxml.m4")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/install_local_manifest.txt"
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
  file(WRITE "/Users/lzy/pro/UserFB/workspace/Z2037/attempts/20261114T145659911529Z/repo/.task-build/xml/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
