#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char**argv){int layer=argc>1?2:3;FILE*f=fopen(argc>1?argv[1]:"vectors/l3-sin1k0db.bit","rb");if(!f)return 1;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);unsigned char*b=malloc(n);if(!b||fread(b,1,n,f)!=(size_t)n)return 2;fclose(f);mp3dec_t d;mp3dec_init(&d);short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];int off=0,count=0,samples=0;unsigned hash=0;while(off<n&&count<3){mp3dec_frame_info_t i;int z=mp3dec_decode_frame(&d,b+off,n-off,pcm,&i);if(i.frame_bytes<=0)return 3;off+=i.frame_bytes;if(z){if(i.layer!=layer)return 4;count++;samples+=z;for(int k=0;k<z*i.channels;k++)hash=hash*33+(unsigned short)pcm[k];}}free(b);if(count!=3||!hash)return 5;printf("MINIMP3_BASE layer=%d frames=%d samples=%d consumed=%d hash=%08x\n",layer,count,samples,off,hash);return 0;}
