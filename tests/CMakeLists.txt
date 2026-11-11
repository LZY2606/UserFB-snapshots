add_executable(OpenAL_Tests)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        52eb8108c5bdec04579160ae17225d66034bd723 # v1.17.0
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

target_link_libraries(OpenAL_Tests PRIVATE
	OpenAL
	GTest::gtest_main
)

target_sources(OpenAL_Tests PRIVATE
	example.t.cpp
)

# Offline high-order B-Format regression: loopback float PCM checked against
# an independent double-precision spherical-harmonic reference.
add_executable(ambi_contract
	ambi_contract.cpp
	ambi_reference.cpp
	ambi_reference.hpp
)
target_link_libraries(ambi_contract PRIVATE OpenAL)
target_include_directories(ambi_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(ambi_contract PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED YES
)
add_test(NAME ambi_contract COMMAND ambi_contract)

# This needs to come last
include(GoogleTest)
gtest_discover_tests(OpenAL_Tests)
