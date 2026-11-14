#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "capstone::capstone_static" for configuration ""
set_property(TARGET capstone::capstone_static APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(capstone::capstone_static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "C"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libcapstone.a"
  )

list(APPEND _cmake_import_check_targets capstone::capstone_static )
list(APPEND _cmake_import_check_files_for_capstone::capstone_static "${_IMPORT_PREFIX}/lib/libcapstone.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
