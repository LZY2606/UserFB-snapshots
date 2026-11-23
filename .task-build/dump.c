#include "kvazaar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 32
#define H 32

static void fill(kvz_picture *p, int f) {
  for (int c=0;c<3;++c){int w=c?W/2:W,h=c?H/2:H,st=c?p->stride_c:p->stride;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)p->data[c][y*st+x]=(unsigned char)((13*x+7*y+f*31+c*17)&255);}
}

typedef struct { unsigned char *d; size_t n; } buf_t;

static int u_bits;
static unsigned long long u_acc;
static void rinit(void){u_bits=0;u_acc=0;}
static unsigned rget(const unsigned char *p, int n){
  unsigned v=0;
  for(int i=0;i<n;++i){v=(v<<1)|((p[(u_bits+i)/8]>>(7-((u_bits+i)%8)))&1);}
  u_bits+=n; return v;
}
static unsigned r_ue(const unsigned char *p){
  int z=0; while(!rget(p,1)) ++z;
  unsigned v=(1u<<z)-1;
  if(z) v+=rget(p,z);
  return v;
}
static int r_se(const unsigned char *p){ unsigned c=r_ue(p); return (int)((c+1)/2)*(c&1?-1:1); }

int main(void){
  const kvz_api *api=kvz_api_get(8);
  kvz_config *cfg=api->config_alloc(); api->config_init(cfg);
  const char *o[][2]={{"gop","0"},{"ref","1"},{"bipred","0"},{"wpp","0"},{"cpuid","0"},
    {"deblock","0"},{"sao","off"},{"info","0"},{"force-inter","1"},{"weighted-pred","1"},
    {"hash","none"},{"subme","0"}};
  for(size_t i=0;i<sizeof(o)/sizeof(o[0]);++i)api->config_parse(cfg,o[i][0],o[i][1]);
  cfg->width=W;cfg->height=H;cfg->owf=0;cfg->threads=1;cfg->enable_logging_output=0;
  kvz_encoder *enc=api->encoder_open(cfg); api->config_destroy(cfg);
  kvz_data_chunk *hdr=NULL; uint32_t hn=0;
  api->encoder_headers(enc,&hdr,&hn);
  size_t cap=1<<16; unsigned char *all=malloc(cap); size_t tot=0;
  for(kvz_data_chunk*c=hdr;c;c=c->next){memcpy(all+tot,c->data,c->len);tot+=c->len;}
  api->chunk_free(hdr);
  for(int f=0;f<2;++f){
    kvz_picture*in=api->picture_alloc(W,H); fill(in,f); in->pts=f;
    if(f){in->wp.enabled=1;in->wp.denom=1;in->wp.weight[0]=2-3;in->wp.weight[1]=2+5;in->wp.weight[2]=2;
      in->wp.offset[0]=7;in->wp.offset[1]=-4;in->wp.offset[2]=0;}
    kvz_picture*rec=0,*src=0;kvz_data_chunk*ch=0;uint32_t b=0;kvz_frame_info info;
    api->encoder_encode(enc,in,&ch,&b,&rec,&src,&info); api->picture_free(in);
    for(kvz_data_chunk*c=ch;c;c=c->next){memcpy(all+tot,c->data,c->len);tot+=c->len;}
    if(ch){api->chunk_free(ch);api->picture_free(rec);api->picture_free(src);}
  }
  // de-emulate: remove 0x03 after 00 00
  unsigned char *rb=malloc(tot); size_t rn=0; int z=0;
  for(size_t i=0;i<tot;++i){ if(z==2&&all[i]==3){z=0;continue;} rb[rn++]=all[i]; z=all[i]?0:z+1; }
  // scan NALs
  size_t i=0;
  while(i+3<rn){
    if(rb[i]==0&&rb[i+1]==0&&rb[i+2]==1){
      size_t s=i+3; size_t j=s;
      while(j+3<=rn && !(rb[j]==0&&rb[j+1]==0&&rb[j+2]==1)) ++j;
      int nal_type=rb[s]>>1;
      printf("NAL type=%d len=%zu\n",nal_type,j-s);
      i=j;
    } else ++i;
  }
  free(all);free(rb);api->encoder_close(enc);
  printf("DUMP_OK\n");
  return 0;
}
