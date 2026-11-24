/* Only the existing in-bounds dumatest routines are called. */
#define main duma_original_test_main
#include "../dumatest.c"
#undef main
int main(void) {
    duma_init();
    if (testSizes() || protectAbove() || allocateMemory() || write0() || read0() || freeMemory()) return 2;
    if (protectBelow() || allocateMemory() || write0() || read0() || freeMemory()) return 3;
    puts("DUMA_OLD_HEALTHY_OK directions=2");
    return 0;
}
