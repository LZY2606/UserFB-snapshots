# Install script for directory: /Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src

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

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/.task-build/cmake/src/Examples/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/.task-build/cmake/src/libraw_pdb.a")
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libraw_pdb.a" AND
     NOT IS_SYMLINK "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libraw_pdb.a")
    execute_process(COMMAND "/usr/bin/ranlib" "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/libraw_pdb.a")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/raw_pdb" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Examples/ExampleMemoryMappedFile.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Examples/ExampleTimedScope.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Examples/ExampleTypeTable.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Examples/Examples_PCH.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_ArrayView.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Assert.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_BitOperators.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_BitUtil.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Forward.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Log.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Macros.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Memory.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Move.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Platform.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_PointerUtil.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_TypeTraits.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Warnings.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_CoalescedMSFStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_DBIStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_DBITypes.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_DirectMSFStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_ErrorCodes.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_GlobalSymbolStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_IPIStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_IPITypes.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_ImageSectionStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_InfoStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_ModuleDataSymbolAnalysis.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_ModuleInfoStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_ModuleLineStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_ModuleSymbolStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_NamesStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_PCH.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_PublicSymbolStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_RawFile.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_SectionContributionStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_SourceFileStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_TPIStream.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_TPITypes.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_Types.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/PDB_Util.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/raw_pdb/Foundation" TYPE FILE FILES
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_ArrayView.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Assert.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_BitOperators.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_BitUtil.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Forward.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Log.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Macros.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Memory.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Move.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Platform.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_PointerUtil.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_TypeTraits.h"
    "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/src/Foundation/PDB_Warnings.h"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/Users/lzy/pro/UserFB/workspace/UFB100026/attempts/20261124T160756528562Z/repo/.task-build/cmake/src/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
