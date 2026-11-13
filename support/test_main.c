#include "test_support.h"
unsigned checks;
void run_sample_curve_tests(void);
int main(void){run_sample_curve_tests();if(!checks){fputs("zero assertions\n",stderr);return 3;}printf("sample-curve assertions=%u\n",checks);return 0;}
