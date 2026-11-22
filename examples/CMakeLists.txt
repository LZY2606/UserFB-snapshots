# --- tools ---
add_executable(bw64_info bw64_info.cpp)
target_link_libraries(bw64_info bw64)

add_executable(bw64_extract_adm bw64_extract_adm.cpp)
target_link_libraries(bw64_extract_adm bw64)

add_executable(bw64_create_sine bw64_create_sine.cpp)
target_link_libraries(bw64_create_sine bw64)

add_executable(bw64_read_write bw64_read_write.cpp)
target_link_libraries(bw64_read_write bw64)

# metadata_edit.cpp is built as the metadata_edit_example target by
# task_support/CMakeLists.txt so it is linked into the task build as well.
