#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "freeaptx.h"
int main(void){unsigned char pcm[24000],bits[8192],out[24576];unsigned i;for(i=0;i<sizeof(pcm);i++)pcm[i]=(unsigned char)((i*37+i/19)%256);for(int hd=0;hd<2;hd++){struct aptx_context *e=aptx_init(hd),*d=aptx_init(hd);size_t n=0,z=0,a,b,f;uint32_t sum=0;a=aptx_encode(e,pcm,sizeof(pcm),bits,sizeof(bits),&n);if(a!=sizeof(pcm))return 1;if(!aptx_encode_finish(e,bits+n,sizeof(bits)-n,&f))return 2;n+=f;b=aptx_decode(d,bits,n,out,sizeof(out),&z);if(b!=n||z<sizeof(pcm))return 3;for(i=0;i<z;i++)sum=sum*33+out[i];printf("APTX_BASE hd=%d bytes=%zu pcm=%zu hash=%08x\n",hd,n,z,sum);aptx_finish(e);aptx_finish(d);}return 0;}
