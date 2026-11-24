#include <stdio.h>
#include <stdlib.h>
#include "check_api.h"
static unsigned checks,failed;
void evidence_check(int ok,const char*m){checks++;if(!ok){failed++;fprintf(stderr,"FAIL %s\n",m);}}
int main(void){evidence_tests();if(checks<8||failed){fprintf(stderr,"assertions=%u failures=%u (at least eight required)\n",checks,failed);return 1;}printf("EVIDENCE_TESTS_RETURNED checks=%u\n",checks);return 0;}
