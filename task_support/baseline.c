#include "H264SwDecApi.h"
#include "task_support/fixture.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned frames;
static int pull(H264SwDecInst dec,unsigned eos) {
  H264SwDecPicture pic;
  H264SwDecRet ret;
  while((ret=H264SwDecNextPicture(dec,&pic,eos))==H264SWDEC_PIC_RDY) {
    if(pic.nbrOfErrMBs)return 1;
    const unsigned char *p=(const unsigned char *)pic.pOutputPicture;
    for(unsigned i=0;i<384;i++)if(p[i]!=128)return 2;
    if(pic.picId!=frames)return 3;frames++;
  }return ret!=H264SWDEC_OK;
}
static int feed(H264SwDecInst dec,unsigned char *bytes,size_t len,unsigned id) {
  H264SwDecInput in={bytes,(u32)len,id,0};H264SwDecOutput out;
  unsigned tries=0;
  while(in.dataLen) {
    H264SwDecRet ret=H264SwDecDecode(dec,&in,&out);
    if(ret<0||!out.pStrmCurrPos)return 10;
    size_t used=(size_t)(out.pStrmCurrPos-in.pStream);
    if(used>in.dataLen||++tries>12)return 11;
    in.pStream+=used;in.dataLen-=(u32)used;
    if(pull(dec,0))return 12;
  }return 0;
}
static int run(unsigned with_sei) {
  H264SwDecInst dec=NULL;unsigned char bytes[16384];size_t n;frames=0;int code=0;
  if(H264SwDecInit(&dec,1)!=H264SWDEC_OK)return 20;
  n=ts_sps(bytes);if((code=feed(dec,bytes,n,0)))goto done;
  n=ts_pps(bytes);if((code=feed(dec,bytes,n,0)))goto done;
  for(unsigned f=0;f<3;f++) {
    if(f==1&&with_sei){n=ts_recovery(bytes,1);if((code=feed(dec,bytes,n,f)))goto done;}
    n=ts_picture(bytes,f);if((code=feed(dec,bytes,n,f)))goto done;
  }
  if(pull(dec,1)||frames!=3)code=21;
done:
  H264SwDecRelease(dec);return code;
}
int main(void) {
  int a=run(0),b=run(1);
  if(a||b){fprintf(stderr,"baseline failed plain=%d sei=%d frames=%u\n",a,b,frames);return 1;}
  puts("BASELINE plain=3 recovery_sei=3 yuv=128 errors=0 original_sei_ignored=1");return 0;
}
