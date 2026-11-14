
add_executable(riscv_reloc riscv_reloc.cpp)
target_link_libraries(riscv_reloc PRIVATE elfio::elfio)
