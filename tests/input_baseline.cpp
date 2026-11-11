#include "xvc_enc_lib/xvcenc.h"
#include "xvc_dec_lib/xvcdec.h"
#include <cassert>
#include <cstdio>
#include <vector>
int main() {
 auto e=xvc_encoder_api_get(); auto d=xvc_decoder_api_get();
 auto ep=e->parameters_create(); assert(e->parameters_set_default(ep)==XVC_ENC_OK);
 ep->width=32;ep->height=24;ep->input_bitdepth=ep->internal_bitdepth=8;ep->sub_gop_length=1;
 ep->threads=0;ep->simd_mask=0;ep->qp=27;ep->speed_mode=2;
 auto enc=e->encoder_create(ep);assert(enc);e->parameters_destroy(ep);
 auto dp=d->parameters_create();assert(d->parameters_set_default(dp)==XVC_DEC_OK);
 dp->threads=0;dp->simd_mask=0;auto dec=d->decoder_create(dp);assert(dec);d->parameters_destroy(dp);
 std::vector<unsigned char> input(32*24*3/2,128);int frames=0,nals=0;
 auto collect=[&](xvc_enc_nal_unit *ns,int n) {
  for(int i=0;i<n;i++) {assert(d->decoder_decode_nal(dec,ns[i].bytes,ns[i].size,0)==XVC_DEC_OK);++nals;}
  xvc_decoded_picture out={};
  while(d->decoder_get_picture(dec,&out)==XVC_DEC_OK) {
   assert(out.stats.width==32 && out.stats.height==24 && out.planes[0]);++frames;
  }
 };
 for(int f=0;f<3;f++) {
  for(int i=0;i<32*24;i++)input[i]=(i+f*11)%200+16;
  xvc_enc_nal_unit *ns=nullptr;int n=0;xvc_enc_pic_buffer rec={};
  assert(e->encoder_encode(enc,input.data(),&ns,&n,&rec)==XVC_ENC_OK);collect(ns,n);
 }
 for(int k=0;k<20;k++) {
  xvc_enc_nal_unit *ns=nullptr;int n=0;xvc_enc_pic_buffer rec={};
  auto r=e->encoder_flush(enc,&ns,&n,&rec);collect(ns,n);
  if(r==XVC_ENC_NO_MORE_OUTPUT)break;assert(r==XVC_ENC_OK && k<19);
 }
 assert(d->decoder_flush(dec)==XVC_DEC_OK);xvc_decoded_picture out={};
 while(d->decoder_get_picture(dec,&out)==XVC_DEC_OK){assert(out.stats.width==32);++frames;}
 assert(frames==3 && nals>=4); e->encoder_destroy(enc);d->decoder_destroy(dec);
 std::printf("xvc packed encode/decode frames=%d: ok\n",frames);
}
