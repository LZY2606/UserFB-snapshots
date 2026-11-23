#include "kvazaar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 64
static void fill(kvz_picture*p,int f){for(int c=0;c<3;++c){int w=c?W/2:W,h=c?W/2:W,st=c?p->stride_c:p->stride;for(int y=0;y<h;++y)for(int x=0;x<w;++x)p->data[c][y*st+x]=(unsigned char)((13*x+7*y+29*((x/8)^(y/8))+c*47+f*31+19*c*f)&255);}}
int main(void){
  const kvz_api*api=kvz_api_get(8);
  kvz_config*cfg=api->config_alloc();api->config_init(cfg);
  const char*o[][2]={{"gop","0"},{"ref","1"},{"bipred","0"},{"wpp","0"},{"threads","1"},{"cpuid","0"},{"deblock","0"},{"sao","off"},{"info","0"},{"hash","none"},{"subme","4"},{"me","hexbs"},{"force-inter","1"},{"weighted-pred","1"}};
  for(size_t i=0;i<sizeof(o)/sizeof(o[0]);++i)api->config_parse(cfg,o[i][0],o[i][1]);
  cfg->width=W;cfg->height=W;cfg->owf=0;cfg->enable_logging_output=0;
  kvz_encoder*enc=api->encoder_open(cfg);api->config_destroy(cfg);
  unsigned char*buf=0;size_t len=0,cap=0;
  kvz_data_chunk*h=0;uint32_t hn=0;api->encoder_headers(enc,&h,&hn);
  for(kvz_data_chunk*c=h;c;c=c->next){if(len+c->len>cap){cap=cap?cap*2:4096;buf=realloc(buf,cap);}memcpy(buf+len,c->data,c->len);len+=c->len;}api->chunk_free(h);
  for(int f=0;f<2;++f){kvz_picture*in=api->picture_alloc(W,W);fill(in,f);if(f){in->wp.enabled=1;in->wp.denom=2;in->wp.weight[0]=4-7;in->wp.weight[1]=4+11;in->wp.weight[2]=4-1;in->wp.offset[0]=-23;in->wp.offset[1]=9;in->wp.offset[2]=40;}
    kvz_picture*r=0,*s=0;kvz_data_chunk*ch=0;uint32_t b=0;kvz_frame_info info;
    api->encoder_encode(enc,in,&ch,&b,&r,&s,&info);
    for(kvz_data_chunk*c=ch;c;c=c->next){if(len+c->len>cap){cap*=2;buf=realloc(buf,cap);}memcpy(buf+len,c->data,c->len);len+=c->len;}
    api->chunk_free(ch);api->picture_free(r);api->picture_free(s);api->picture_free(in);}
  unsigned char*rb=malloc(len);size_t rn=0;int z=0;
  for(size_t i=0;i<len;++i){if(z==2&&buf[i]==3){z=0;continue;}rb[rn++]=buf[i];z=buf[i]?0:z+1;}
  for(size_t i=0;i+3<=rn;++i)if(rb[i]==0&&rb[i+1]==0&&rb[i+2]==1)printf("NAL type=%d\n",rb[i+3]>>1);
  return 0;}
