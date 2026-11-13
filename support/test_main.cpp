#include "test_support.h"
void run_tx3g_tests();
int main(){run_tx3g_tests();if(!unit_test::checks()){std::fputs("zero assertions\n",stderr);return 3;}std::printf("tx3g assertions=%u\n",unit_test::checks());return 0;}
