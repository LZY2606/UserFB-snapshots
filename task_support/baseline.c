#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "minih264e.h"
#include "h264bsd_decoder.h"
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"BASELINE_FAIL line=%d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(void){
 H264E_create_param_t p={0}; H264E_run_param_t r={0}; H264E_io_yuv_t y;
 int ps,ss,n; unsigned char *encoded; unsigned char pixels[64*64*3/2];
 storage_t d; memset(&d,0,sizeof(d));
 p.width=64;p.height=64;p.gop=4;p.num_layers=1;p.fine_rate_control_flag=1;
 REQUIRE(!H264E_sizeof(&p,&ps,&ss));
 void *enc=malloc(ps),*scratch=malloc(ss);REQUIRE(enc&&scratch);REQUIRE(!H264E_init(enc,&p));REQUIRE(!h264bsdInit(&d,1));
 y.yuv[0]=pixels;y.yuv[1]=pixels+4096;y.yuv[2]=pixels+5120;y.stride[0]=64;y.stride[1]=y.stride[2]=32;
 unsigned outputs=0,total=0;uint64_t checksum=14695981039346656037ULL;
 for(int f=0;f<8;f++){
  for(int i=0;i<6144;i++)pixels[i]=(unsigned char)((i*13+(i/64)*17+f*7)^((i+f*3)>>3));
  memset(&r,0,sizeof(r));r.qp_min=18;r.qp_max=42;r.desired_frame_bytes=350;r.desired_nalu_bytes=160;r.encode_speed=5;
  REQUIRE(!H264E_encode(enc,scratch,&r,&y,&encoded,&n));REQUIRE(n>0);total+=(unsigned)n;
  unsigned char *copy=malloc((size_t)n);REQUIRE(copy);memcpy(copy,encoded,(size_t)n);
  u32 off=0,loops=0;while(off<(u32)n){u32 used=0;u32 result=h264bsdDecode(&d,copy+off,(u32)n-off,(u32)f,&used);REQUIRE(++loops<100);if(result>H264BSD_HDRS_RDY)fprintf(stderr,"decode f=%d off=%u used=%u result=%u bytes=%d\n",f,off,used,result,n);REQUIRE(result<=H264BSD_HDRS_RDY);REQUIRE(used<=(u32)n-off);off+=used;
   for(;;){u32 id,key,errors;u8 *out=h264bsdNextOutputPicture(&d,&id,&key,&errors);if(!out)break;REQUIRE(!errors);REQUIRE(id==(u32)f);REQUIRE(!memcmp(out,pixels,6144));outputs++;for(int i=0;i<6144;i++){checksum^=out[i];checksum*=1099511628211ULL;}}
  }
  free(copy);
 }
 h264bsdFlushBuffer(&d);for(;;){u32 id,key,errors;u8*out=h264bsdNextOutputPicture(&d,&id,&key,&errors);if(!out)break;REQUIRE(!errors);outputs++;}
 REQUIRE(outputs==8);printf("BASELINE frames=%u bytes=%u pixel_match=8 concealment=0 checksum=%016llx\n",outputs,total,(unsigned long long)checksum);
 h264bsdShutdown(&d);free(enc);free(scratch);return 0;
}
