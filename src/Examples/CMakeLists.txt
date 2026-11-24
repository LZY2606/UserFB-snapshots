project(Examples)

set(SOURCES
	ExampleContributions.cpp
	ExampleFunctionSymbols.cpp
	ExampleFunctionVariables.cpp
	ExampleIPI.cpp
	ExampleLines.cpp
	ExampleMain.cpp
	ExampleMemoryMappedFile.cpp
	ExampleMemoryMappedFile.h
	ExamplePDBSize.cpp
	Examples_PCH.cpp
	Examples_PCH.h
	ExampleSymbols.cpp
	ExampleTimedScope.cpp
	ExampleTimedScope.h
	ExampleTypes.cpp
	ExampleTypeTable.cpp
	ExampleTypeTable.h
)

source_group(src FILES
    ${SOURCES}
)

add_executable(Examples
    ${SOURCES}
)

target_link_libraries(Examples
  PUBLIC
    raw_pdb
)

target_precompile_headers(Examples
  PUBLIC
    Examples_PCH.h
)

# Standalone offline report generator for Small MSF / module-symbol analysis.
# It has its own main() and reuses the task_support serialization-only fixtures.
add_executable(ModuleSymbolReport
  ExampleModuleSymbolReport.cpp
)

target_link_libraries(ModuleSymbolReport
  PRIVATE
    raw_pdb
)

target_include_directories(ModuleSymbolReport
  PRIVATE
    "${CMAKE_SOURCE_DIR}/task_support"
)

set_target_properties(ModuleSymbolReport PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)