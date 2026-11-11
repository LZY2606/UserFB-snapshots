#include "opusenc.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static unsigned read32(const unsigned char *p) {return p[0]|p[1]<<8|p[2]<<16|(unsigned)p[3]<<24;}
static void packet(void *u,const unsigned char *p,opus_int32 n,opus_uint32 f) {(*(int*)u)++;assert(p && n>0 && f==0);}
int main(void) {
  OggOpusComments *c=ope_comments_create();int error,packets=0;
  OggOpusEnc *e=ope_encoder_create_pull(c,48000,2,0,&error);assert(e && error==0);
  assert(ope_encoder_ctl(e,OPE_SET_SERIALNO(1024))==0);
  assert(ope_encoder_ctl(e,OPE_SET_PACKET_CALLBACK(packet,&packets))==0);
  opus_int16 pcm[960*2];for(int i=0;i<960;i++){pcm[i*2]=(i%73)*200;pcm[i*2+1]=-(i%59)*200;}
  assert(ope_encoder_write(e,pcm,960)==0);assert(ope_encoder_drain(e)==0);
  unsigned char *p;opus_int32 n;int pages=0,eos=0,preskip=-1;uint64_t final=0;
  while(ope_encoder_get_page(e,&p,&n,1)>0){
    assert(n>=28 && !memcmp(p,"OggS",4) && read32(p+14)==1024);
    int off=27+p[26];if(n>=off+19 && !memcmp(p+off,"OpusHead",8))preskip=p[off+10]|p[off+11]<<8;
    if(p[5]&4){eos++;for(int j=7;j>=0;j--)final=(final<<8)|p[6+j];}pages++;
  }
  assert(pages>=3 && eos==1 && packets>=3 && preskip>=0 && final-(uint64_t)preskip==960);
  ope_encoder_destroy(e);ope_comments_destroy(c);puts("OPUSENC_ENCODE_GRANULE_BASELINE_OK");
}
