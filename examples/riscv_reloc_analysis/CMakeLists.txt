add_executable(riscv_reloc_analysis main.cpp)
target_link_libraries(riscv_reloc_analysis PRIVATE elfio::elfio)
