#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "minih264e.h"
#include "h264bsd_decoder.h"
static int maxnal,callbacks;
static void nal(const unsigned char*p,int n,void*t){(void)p;(void)t;if(n>maxnal)maxnal=n;callbacks++;}
int main(void){
 H264E_create_param_t p={0};H264E_run_param_t r={0};H264E_io_yuv_t y;int a,b;
 p.width=p.height=64;p.gop=4;p.enableNEON=1;p.num_layers=1;
 assert(H264E_sizeof(&p,&a,&b)==0);void*enc=malloc(a),*scratch=malloc(b);assert(enc&&scratch&&H264E_init(enc,&p)==0);
 unsigned char input[6144],copy[65536],*coded;int n,frames=0;storage_t d;assert(h264bsdInit(&d,1)==0);
 y.yuv[0]=input;y.yuv[1]=input+4096;y.yuv[2]=input+5120;y.stride[0]=64;y.stride[1]=y.stride[2]=32;
 r.qp_min=r.qp_max=30;r.encode_speed=5;r.desired_nalu_bytes=128;r.nalu_callback=nal;
 for(int f=0;f<4;f++){
  for(int i=0;i<6144;i++)input[i]=(unsigned char)((i*37+i/64*51+f*13)&255);
  assert(H264E_encode(enc,scratch,&r,&y,&coded,&n)==0&&n>0&&n<65536);memcpy(copy,coded,n);
  unsigned used=0,offset=0;int stalls=0;
  while(offset<(unsigned)n){unsigned z=h264bsdDecode(&d,copy+offset,n-offset,f,&used);if(z>H264BSD_HDRS_RDY)fprintf(stderr,"decode_status=%u f=%d offset=%u read=%u n=%d first=%02x%02x%02x%02x%02x\n",z,f,offset,used,n,copy[0],copy[1],copy[2],copy[3],copy[4]);assert(z<=H264BSD_HDRS_RDY);offset+=used;
   assert(used||++stalls<8);if(z==H264BSD_PIC_RDY){unsigned id,key,errors;unsigned char*pic=h264bsdNextOutputPicture(&d,&id,&key,&errors);
    assert(pic&&errors==0&&!memcmp(pic,input,6144));frames++;}}
 }
 assert(frames==4&&maxnal>128);h264bsdShutdown(&d);free(enc);free(scratch);
 printf("MINIH264_BASELINE_OK frames=4 reconstruction=equal desired=128 actual_max=%d callbacks=%d\n",maxnal,callbacks);
}
