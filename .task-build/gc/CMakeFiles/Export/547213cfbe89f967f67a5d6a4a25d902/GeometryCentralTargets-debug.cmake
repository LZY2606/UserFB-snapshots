#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "geometry-central::geometry-central" for configuration "Debug"
set_property(TARGET geometry-central::geometry-central APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(geometry-central::geometry-central PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libgeometry-central.a"
  )

list(APPEND _cmake_import_check_targets geometry-central::geometry-central )
list(APPEND _cmake_import_check_files_for_geometry-central::geometry-central "${_IMPORT_PREFIX}/lib/libgeometry-central.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
