extern "C" __attribute__((noinline)) void observe(int* p) { asm volatile("" : : "r"(p) : "memory"); }
