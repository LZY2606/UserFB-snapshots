#include <stdio.h>
#include <stdlib.h>
#include "check.h"
static unsigned checks;
void unit_check(int ok,const char *label){ if(!ok){fprintf(stderr,"ASSERT %s\n",label);exit(42);}checks++;}
int main(void){
 void (*groups[])(void)={test_idct,test_motion,test_stream};
 const char *names[]={"idct","motion","stream"};
 for(unsigned g=0;g<3;g++){unsigned before=checks;groups[g]();if(checks==before){fprintf(stderr,"EMPTY %s\n",names[g]);return 43;}printf("GROUP %s checks=%u\n",names[g],checks-before);}
 printf("MPEG2_NEON_OK checks=%u\n",checks);return 0;
}
