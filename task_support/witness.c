#include "bddisasm.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
 const unsigned char code[]={0xd9,0xe8,0xd9,0xc0,0xd9,0xc9,0xdd,0xd9,0xd9,0xf7};
 for(size_t off=0;off<sizeof(code);){INSTRUX i;ND_ACCESS_MAP m={0};char text[128];
 if(!ND_SUCCESS(NdDecodeEx(&i,code+off,sizeof(code)-off,ND_CODE_64,ND_DATA_64)))return 1;
 if(!ND_SUCCESS(NdToText(&i,0x1000+off,sizeof(text),text)))return 2;
 if(!ND_SUCCESS(NdGetFullAccessMap(&i,&m)))return 3;
 printf("X87 off=%zu len=%u %s ST0=%u ST1=%u\n",off,i.Length,text,m.FpuAccess[0],m.FpuAccess[1]);off+=i.Length;}
 FILE *f=fopen("bddisasm_test/x86/basic/fpu_64.test","rb");if(!f)return 4;
 unsigned char data[65536];size_t n=fread(data,1,sizeof(data),f);fclose(f);if(!n || n==sizeof(data))return 5;
 size_t off=0,count=0;while(off<n){INSTRUX i;if(!ND_SUCCESS(NdDecodeEx(&i,data+off,n-off,ND_CODE_64,ND_DATA_64)))return 6;if(!i.Length)return 7;off+=i.Length;count++;}
 printf("UPSTREAM_FPU_DECODE count=%zu bytes=%zu\n",count,n);puts("X87_DECODE_BASELINE_OK");return 0;
}
