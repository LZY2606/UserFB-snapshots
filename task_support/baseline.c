#include "twolame.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define N 5003
static twolame_options *make(int in, int out) {
 twolame_options *o=twolame_init(); if(!o) exit(2);
 twolame_set_num_channels(o,2);twolame_set_in_samplerate(o,in);twolame_set_out_samplerate(o,out);
 twolame_set_bitrate(o,192);twolame_set_mode(o,TWOLAME_STEREO);twolame_set_psymodel(o,3);
 twolame_set_verbosity(o,0);return o;
}
static int encode(short *pcm,unsigned char *out,int split) {
 twolame_options *o=make(44100,44100);if(twolame_init_params(o))exit(3);
 int pos=0,total=0,k=0;const int sizes[]={1,37,2049,19};
 while(pos<N){int n=split?sizes[k++%4]:N;if(n>N-pos)n=N-pos;
 int b=twolame_encode_buffer_interleaved(o,pcm+2*pos,n,out+total,65536-total);
 if(b<0)exit(4);total+=b;pos+=n;}
 int b=twolame_encode_flush(o,out+total,65536-total);if(b<0)exit(5);total+=b;
 if(twolame_encode_flush(o,out+total,65536-total)!=0)exit(6);
 twolame_close(&o);return total;
}
int main(void){
 short pcm[N*2];unsigned char a[65536],b[65536];
 for(int i=0;i<N;i++){pcm[2*i]=(short)((i*193)%40001-20000);pcm[2*i+1]=(short)((i*127)%30001-15000);}
 int n=encode(pcm,a,0),m=encode(pcm,b,1);if(n<=0||m!=n||memcmp(a,b,n))return 7;
 int frames=0,pos=0;while(pos<n){unsigned char *h=a+pos;if(pos+4>n||h[0]!=255||(h[1]&0xfe)!=0xfc)return 8;
 int kb[]={0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0};
 int sr[]={44100,48000,32000,0};int rate=sr[(h[2]>>2)&3];if(!rate)return 9;
 int len=144000*kb[h[2]>>4]/rate+((h[2]>>1)&1);if(len<4||pos+len>n)return 10;pos+=len;frames++;}
 if(frames!=(N+1151)/1152)return 11;
 printf("BASELINE_ENCODE frames=%d bytes=%d chunk_equal=1\n",frames,n);
 twolame_options *o=make(48000,44100);int rc=twolame_init_params(o);printf("RESAMPLE_INIT result=%d\n",rc);twolame_close(&o);
 puts("BASELINE_OK");return 0;
}
