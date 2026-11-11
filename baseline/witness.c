#include <stdio.h>
#include <assert.h>
#include "distorm.h"
int main(void) {
 const unsigned char b[]={0xeb,0x02,0x90,0x90,0xc3};
 _DInst d[16]; unsigned int n=0;
 _CodeInfo ci={0}; ci.codeOffset=0x1000; ci.code=b; ci.codeLen=sizeof(b); ci.dt=Decode64Bits;
 assert(distorm_decompose64(&ci,d,16,&n)==DECRES_SUCCESS);
 assert(n==4 && d[0].addr==0x1000 && META_GET_FC(d[0].meta)==FC_UNC_BRANCH);
 assert(INSTRUCTION_GET_TARGET(&d[0])==0x1004 && d[1].addr==0x1002);
 assert(META_GET_FC(d[3].meta)==FC_RET);
 printf("LINEAR_SWEEP_OK count=%u branch=1004 includes=1002,1003\n",n);
 return 0;
}
