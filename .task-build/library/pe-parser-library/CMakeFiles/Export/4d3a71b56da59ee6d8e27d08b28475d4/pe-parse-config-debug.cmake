#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "pe-parse::pe-parse" for configuration "Debug"
set_property(TARGET pe-parse::pe-parse APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(pe-parse::pe-parse PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libpe-parse.a"
  )

list(APPEND _cmake_import_check_targets pe-parse::pe-parse )
list(APPEND _cmake_import_check_files_for_pe-parse::pe-parse "${_IMPORT_PREFIX}/lib/libpe-parse.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
