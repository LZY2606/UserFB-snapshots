add_executable(bpf_graph bpf_graph.cpp)
target_link_libraries(bpf_graph PRIVATE elfio::elfio)
