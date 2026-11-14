#include "include/distorm.h"
#include "inputs.h"
#include <stdio.h>
#include <string.h>
static int normal(const unsigned char*bytes,int len,const char*name){
 _CodeInfo ci;_DInst di[16];_DecodedInst text;unsigned n=0;
 memset(&ci,0,sizeof(ci));ci.code=bytes;ci.codeLen=len;ci.codeOffset=0x1000;ci.dt=Decode64Bits;
 if(distorm_decompose(&ci,di,16,&n)!=DECRES_SUCCESS||n!=1||di[0].size!=len||di[0].flags==FLAG_NOT_DECODABLE)return 1;
 distorm_format(&ci,di,&text);
 printf("NORMAL %s size=%u mnemonic=%s\n",name,di[0].size,text.mnemonic.p);return 0;
}
int main(void){
 _CodeInfo ci;_DInst di[16];unsigned n=0;
 if(normal(plain_mov,sizeof(plain_mov),"MOV")||normal(plain_pop,sizeof(plain_pop),"POP")||normal(plain_vex,sizeof(plain_vex),"VEX"))return 2;
 memset(&ci,0,sizeof(ci));ci.code=xop_registers;ci.codeLen=sizeof(xop_registers);ci.codeOffset=0x2000;ci.dt=Decode64Bits;
 if(distorm_decompose(&ci,di,16,&n)!=DECRES_SUCCESS||!n)return 3;
 printf("XOP observed_count=%u first_size=%u first_invalid=%d desired=VPCMOV,size6\n",n,di[0].size,di[0].flags==FLAG_NOT_DECODABLE);
 puts("BASELINE_OK");return 0;
}
