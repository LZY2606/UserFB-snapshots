#include "kvazaar.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static void require(int ok,const char*what){if(!ok){fprintf(stderr,"BASELINE_FAIL %s\n",what);exit(1);}}
static void fold(uint64_t*hash,unsigned v){*hash^=v;*hash*=UINT64_C(1099511628211);}
static void encode(int rate,int owf,int wpp){
 const kvz_api*a=kvz_api_get(8);require(a!=NULL,"api");kvz_config*c=a->config_alloc();require(c&&a->config_init(c),"config");
 require(a->config_parse(c,"preset","ultrafast"),"preset");require(a->config_parse(c,"gop","0"),"gop");
 c->width=128;c->height=128;c->framerate_num=30000;c->framerate_denom=1001;c->framerate=30000.0/1001;
 c->target_bitrate=rate;c->qp=28;c->threads=2;c->owf=owf;c->wpp=wpp;c->cpuid=0;c->add_encoder_info=0;c->intra_period=0;c->enable_logging_output=0;c->intra_bit_allocation=0;c->vaq=0;c->rc_algorithm=rate?KVZ_LAMBDA:KVZ_NO_RC;
 kvz_encoder*enc=a->encoder_open(c);require(enc!=NULL,"open");a->config_destroy(c);
 uint64_t hash=UINT64_C(14695981039346656037),pixels=UINT64_C(14695981039346656037),total=0;int nout=0,minqp=52,maxqp=-1;
 for(int n=0;n<64;++n){
  kvz_picture*in=NULL;
  if(n<24){in=a->picture_alloc(128,128);require(in!=NULL,"picture");in->pts=n;
   for(int y=0;y<128;++y)for(int x=0;x<128;++x)in->y[y*in->stride+x]=(kvz_pixel)((x*7+y*11+(x*y)%31+n*19+(n%3)*(x^y))&255);
   for(int y=0;y<64;++y)for(int x=0;x<64;++x){in->u[y*(in->stride/2)+x]=(kvz_pixel)((80+x*3+n*9+y)&255);in->v[y*(in->stride/2)+x]=(kvz_pixel)((160+y*2+n*5+x)&255);}
  }
  kvz_data_chunk*chunks=NULL;uint32_t bytes=0;kvz_picture*rec=NULL,*src=NULL;kvz_frame_info info;
  require(a->encoder_encode(enc,in,&chunks,&bytes,&rec,&src,&info),"encode");if(in)a->picture_free(in);
  if(bytes){require(chunks&&rec&&src,"output pointers");require(src->pts==nout,"source identity");uint64_t sum=0;
   for(kvz_data_chunk*p=chunks;p;p=p->next){sum+=p->len;for(unsigned j=0;j<p->len;++j)fold(&hash,p->data[j]);}require(sum==bytes,"chunk length");total+=bytes;
   require(info.qp>=0&&info.qp<=51,"qp");if(info.qp<minqp)minqp=info.qp;if(info.qp>maxqp)maxqp=info.qp;
   for(int y=0;y<128;++y)for(int x=0;x<128;++x)fold(&pixels,rec->y[y*rec->stride+x]);
   for(int y=0;y<64;++y)for(int x=0;x<64;++x){fold(&pixels,rec->u[y*(rec->stride/2)+x]);fold(&pixels,rec->v[y*(rec->stride/2)+x]);}++nout;
  }else require(chunks==NULL&&rec==NULL&&src==NULL,"empty output");
  if(chunks)a->chunk_free(chunks);if(rec)a->picture_free(rec);if(src)a->picture_free(src);
  if(n>=24&&!bytes)break;
 }
 require(nout==24&&total>0,"real outputs");a->encoder_close(enc);
 printf("ENCODE bitrate=%d owf=%d wpp=%d frames=%d bytes=%llu qp=%d..%d annexb=%016llx pixels=%016llx\n",rate,owf,wpp,nout,(unsigned long long)total,minqp,maxqp,(unsigned long long)hash,(unsigned long long)pixels);
}
int main(void){encode(0,0,0);encode(100000,0,0);encode(100000,2,1);puts("BASELINE_COMPLETE real_encodes=3 frames_each=24");return 0;}
