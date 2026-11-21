#include <cstdlib>
#include <cstdint>
unsigned int task_packet_freed = 0;
void packet_free(void* p, uint32_t) {
    if (p) { ++task_packet_freed; std::free(p); }
}
