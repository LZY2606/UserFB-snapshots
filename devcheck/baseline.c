#include "minimp3_ex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define REQUIRE(x) do{if(!(x)){fprintf(stderr,"baseline:%d: %s\n",__LINE__,#x);return 1;}}while(0)
static uint64_t protected_at=UINT64_MAX;static int protected_len;
static int visit(void*u,const uint8_t*f,int len,int ff,size_t available,uint64_t off,mp3dec_frame_info_t*i){
 (void)u;(void)ff;(void)available;if(i->layer==3&&!(f[1]&1)){protected_at=off;protected_len=len;return 1;}return 0;
}
int main(void){
 const char*path="vectors/l3-hecommon.bit";FILE*f=fopen(path,"rb");REQUIRE(f);REQUIRE(!fseek(f,0,SEEK_END));long sz=ftell(f);REQUIRE(sz>0&&sz<INT_MAX);rewind(f);
 unsigned char*a=malloc((size_t)sz),*b=malloc((size_t)sz);REQUIRE(a&&b);REQUIRE(fread(a,1,sz,f)==(size_t)sz);REQUIRE(!fclose(f));
 (void)mp3dec_iterate_buf(a,sz,visit,NULL);REQUIRE(protected_at!=UINT64_MAX&&protected_len>6);
 memcpy(b,a,sz);b[protected_at+4]^=1;
 mp3dec_t da,db;mp3dec_init(&da);mp3dec_init(&db);size_t at=0,frames=0,scalars=0;unsigned long long energy=0;
 while(at<(size_t)sz){mp3dec_frame_info_t ia={0},ib={0};short pa[MINIMP3_MAX_SAMPLES_PER_FRAME],pb[MINIMP3_MAX_SAMPLES_PER_FRAME];
 int na=mp3dec_decode_frame(&da,a+at,(int)(sz-at),pa,&ia),nb=mp3dec_decode_frame(&db,b+at,(int)(sz-at),pb,&ib);
 REQUIRE(na==nb&&ia.channels==ib.channels&&ia.frame_bytes==ib.frame_bytes);if(!ia.frame_bytes)break;
 REQUIRE(!memcmp(pa,pb,(size_t)na*ia.channels*sizeof(short)));
 for(int j=0;j<na*ia.channels;j++)energy+=(unsigned long long)abs((int)pa[j]);
 scalars+=(size_t)na*ia.channels;frames+=(na>0);at+=(size_t)ia.frame_bytes;
 }
 REQUIRE(frames>0&&scalars>0&&energy>0);
 printf("PROTECTED_FRAME offset=%llu bytes=%d crc_byte_flipped=1\n",(unsigned long long)protected_at,protected_len);
 printf("MINIMP3_BASELINE_OK decoded_frames=%zu scalars=%zu abs_sum=%llu crc_ignored_pcm_equal=1\n",frames,scalars,energy);
 free(a);free(b);return 0;
}
