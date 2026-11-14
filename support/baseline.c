#include <stdio.h>
#include <stdint.h>
#include "enc_if.h"
int main(void){
 void *s=E_IF_init();if(!s)return 2;
 short pcm[320];unsigned char out[128];uint32_t h=2166136261u;unsigned bytes=0;
 for(unsigned f=0;f<12;f++){
  for(unsigned i=0;i<320;i++) pcm[i]=(short)(((f*320+i)*7919u%24001)-12000);
  int mode=1+f%4,n=E_IF_encode(s,mode,pcm,out,0);
  static const int size[]={0,24,33,37,41};
  if(n!=size[mode] || ((out[0]>>3)&15)!=mode)return 3;
  for(int i=0;i<n;i++)h=(h^out[i])*16777619u;bytes+=n;
 }
 E_IF_exit(s);printf("AMRWB_BASE frames=12 bytes=%u hash=%08x\n",bytes,h);return 0;
}
