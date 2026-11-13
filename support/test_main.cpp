#include "test_support.h"
void run_time_sequence_tests();
int main(){run_time_sequence_tests();if(!unit_test::checks()){std::fputs("zero assertions\n",stderr);return 3;}std::printf("time-sequence assertions=%u\n",unit_test::checks());return 0;}
