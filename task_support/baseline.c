#include "uvg266.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t mix(uint32_t h,uint8_t x){return (h^x)*16777619u;}
static int run(const char *gop,int owf,int bitrate){
 const uvg_api *a=uvg_api_get(8);if(!a)return 1;
 uvg_config *c=a->config_alloc();if(!c||!a->config_init(c))return 2;
 if(!a->config_parse(c,"preset","ultrafast")||!a->config_parse(c,"gop",gop))return 3;
 c->width=64;c->height=64;c->threads=2;c->owf=owf;c->wpp=0;c->intra_period=16;c->qp=30;
 if(bitrate){if(!a->config_parse(c,"rc-algorithm","lambda"))return 4;c->target_bitrate=bitrate;}
 uvg_encoder *e=a->encoder_open(c);if(!e)return 5;
 unsigned count=0,seen=0,inter=0;uint64_t bytes=0;uint32_t hash=2166136261u;
 for(unsigned k=0;k<45;++k){
  uvg_picture *in=NULL;
  if(k<13){
   in=a->picture_alloc(64,64);if(!in)return 6;
   in->pts=100+(int64_t)k*40;
   for(int y=0;y<64;++y)for(int x=0;x<64;++x)in->y[y*in->stride+x]=(uint8_t)(32+((x+2*y+3*k)%128));
   for(int y=0;y<32;++y)for(int x=0;x<32;++x){in->u[y*(in->stride/2)+x]=(uint8_t)(96+(x+k)%32);in->v[y*(in->stride/2)+x]=(uint8_t)(128+(y+2*k)%32);}
  }
  uvg_data_chunk *data=NULL;uint32_t len=0;uvg_picture *rec=NULL,*src=NULL;uvg_frame_info info;
  int ok=a->encoder_encode(e,in,&data,&len,&rec,&src,&info);a->picture_free(in);
  if(!ok)return 7;
  if(len){
   if(!data||!rec||!src)return 8;
   int64_t idx=(src->pts-100)/40;
   if(idx<0||idx>=13||src->pts!=100+idx*40||(seen&(1u<<idx)))return 9;
   seen|=1u<<idx;bytes+=len;++count;inter+=info.slice_type!=UVG_SLICE_I;
   for(uvg_data_chunk *p=data;p;p=p->next)for(unsigned n=0;n<p->len;++n)hash=mix(hash,p->data[n]);
   printf("FRAME gop=%s owf=%d src=%lld pts=%lld dts=%lld poc=%d nal=%d refs=%d/%d qp=%d bytes=%u\n",gop,owf,(long long)idx,(long long)src->pts,(long long)src->dts,info.poc,info.nal_unit_type,info.ref_list_len[0],info.ref_list_len[1],info.qp,len);
  }
  a->chunk_free(data);a->picture_free(rec);a->picture_free(src);
  if(k>=13&&!len)break;
 }
 a->encoder_close(e);a->config_destroy(c);
 if(count!=13||seen!=8191||!inter||!bytes)return 10;
 printf("BASELINE_ENCODER_OK gop=%s owf=%d bitrate=%d frames=%u inter=%u bytes=%llu hash=%08x\n",gop,owf,bitrate,count,inter,(unsigned long long)bytes,hash);
 return 0;
}
int main(void){int r=run("0",0,0);if(r)return r;r=run("8",2,0);if(r)return r;return run("lp-g4d3t1",0,40000);}
