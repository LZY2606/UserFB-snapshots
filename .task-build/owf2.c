#include "kvazaar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 64
#define H 64
#define N 4
static void fill(kvz_picture*p,int f){for(int c=0;c<3;++c){int w=c?W/2:W,h=c?H/2:H,st=c?p->stride_c:p->stride;for(int y=0;y<h;++y)for(int x=0;x<w;++x)p->data[c][y*st+x]=(unsigned char)((13*x+7*y+29*((x/8)^(y/8))+c*47+f*31)&255);}}
static int run(int owf){
  const kvz_api*api=kvz_api_get(8);
  kvz_config*cfg=api->config_alloc();api->config_init(cfg);
  const char*o[][2]={{"gop","0"},{"ref","1"},{"bipred","0"},{"wpp","0"},{"cpuid","0"},{"deblock","0"},{"sao","off"},{"info","0"},{"hash","none"},{"force-inter","1"},{"weighted-pred","1"}};
  for(size_t i=0;i<sizeof(o)/sizeof(o[0]);++i)api->config_parse(cfg,o[i][0],o[i][1]);
  cfg->width=W;cfg->height=H;cfg->owf=owf;cfg->threads=2;cfg->enable_logging_output=0;
  kvz_encoder*enc=api->encoder_open(cfg);api->config_destroy(cfg);
  int got=0;
  for(int f=0;f<N+2;++f){
    kvz_picture*in= f<N?api->picture_alloc(W,H):NULL;
    if(in){fill(in,f);if(f){in->wp.enabled=1;in->wp.denom=1;in->wp.weight[0]=2-2;in->wp.weight[1]=2+1;in->wp.weight[2]=2;in->wp.offset[0]=9;in->wp.offset[1]=-3;in->wp.offset[2]=2;}}
    kvz_picture*r=0,*s=0;kvz_data_chunk*ch=0;uint32_t b=0;kvz_frame_info info;
    if(api->encoder_encode(enc,in,&ch,&b,&r,&s,&info)!=1){printf("FAIL rc f=%d\n",f);return 1;}
    if(ch){printf("owf=%d out poc=%d slice=%d\n",owf,info.poc,info.slice_type);got++;}
    if(ch)api->chunk_free(ch);if(r)api->picture_free(r);if(s)api->picture_free(s);if(in)api->picture_free(in);
  }
  api->encoder_close(enc);
  return got==N?0:1;
}
int main(void){
  if(run(0)){printf("owf0 FAIL\n");return 1;}
  if(run(1)){printf("owf1 FAIL\n");return 1;}
  printf("OWF_DELAY_OK\n");return 0;
}
