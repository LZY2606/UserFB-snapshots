#include <theora/theoraenc.h>
#include <theora/theoradec.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
  th_info info,parsed; th_comment comment; th_setup_info *setup=NULL;
  th_enc_ctx *enc; th_dec_ctx *dec; th_ycbcr_buffer input,output;
  ogg_packet packet; unsigned char y[96*144],u[48*72],v[48*72];
  unsigned long sums[4]; int i,f,n=0,headers=0,pp=0;
  th_info_init(&info);th_info_init(&parsed);th_comment_init(&comment);
  info.frame_width=info.pic_width=96;info.frame_height=info.pic_height=144;
  info.fps_numerator=25;info.fps_denominator=1;info.aspect_numerator=info.aspect_denominator=1;
  info.pixel_fmt=TH_PF_420;info.quality=32;info.keyframe_granule_shift=6;
  enc=th_encode_alloc(&info);assert(enc);
  while(th_encode_flushheader(enc,&comment,&packet)>0){assert(th_decode_headerin(&parsed,&comment,&setup,&packet)>0);headers++;}
  assert(headers==3);dec=th_decode_alloc(&parsed,setup);assert(dec);th_setup_free(setup);
  assert(th_decode_ctl(dec,TH_DECCTL_SET_PPLEVEL,&pp,sizeof(pp))==0);
  input[0]=(th_img_plane){96,144,96,y};input[1]=(th_img_plane){48,72,48,u};input[2]=(th_img_plane){48,72,48,v};
  for(f=0;f<4;f++){
    for(i=0;i<96*144;i++)y[i]=(unsigned char)(60+(i%96)/3+(i/96)/4+f*7);
    for(i=0;i<48*72;i++){u[i]=(unsigned char)(90+f*3+i%9);v[i]=(unsigned char)(170-f*4-i%11);}
    assert(th_encode_ycbcr_in(enc,input)==0);
    while(th_encode_packetout(enc,f==3,&packet)>0){
      assert(n<4);assert(th_decode_packetin(dec,&packet,NULL)==0);assert(th_decode_ycbcr_out(dec,output)==0);
      assert(output[0].width==96&&output[0].height==144);sums[n]=0;
      for(i=0;i<144;i++){int x;for(x=0;x<96;x++)sums[n]+=output[0].data[i*output[0].stride+x];}
      printf("THEORA_STREAM frame=%d bytes=%ld sum=%lu\n",n,packet.bytes,sums[n]);n++;
    }
  }
  assert(n==4&&sums[0]!=sums[3]);
  th_decode_free(dec);th_encode_free(enc);th_info_clear(&info);th_info_clear(&parsed);th_comment_clear(&comment);
  return 0;
}
