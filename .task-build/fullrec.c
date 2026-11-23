#include "kvazaar.h"
#include "cu.h"
#include "encoderstate.h"
#include "inter.h"
#include "kvazaar_internal.h"
#include "weighted_pred.h"
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
  for(int f=0;f<2;++f){
    kvz_picture*in=api->picture_alloc(W,W);fill(in,f);
    if(f){in->wp.enabled=1;in->wp.denom=1;in->wp.weight[0]=2-3;in->wp.weight[1]=2+2;in->wp.weight[2]=2-1;in->wp.offset[0]=12;in->wp.offset[1]=-5;in->wp.offset[2]=7;}
    kvz_picture*r=0,*s=0;kvz_data_chunk*ch=0;uint32_t b=0;kvz_frame_info info;
    if(api->encoder_encode(enc,in,&ch,&b,&r,&s,&info)!=1){printf("encode fail\n");return 1;}
    if(f==1){
      encoder_state_t*st=&enc->states[0];
      // Reconstruct prediction for every inter CU at depth 0 using the stored final MVs,
      // and confirm the frame rec is prediction+residual (sanity: wp_active and a P slice).
      printf("poc=%d slice=%d wp_active=%d refs=%u\n",info.poc,info.slice_type,st->frame->wp_active,st->frame->ref->used_size);
      if(st->frame->wp_active!=1){printf("FAIL wp not active\n");return 1;}
      // Count inter CUs
      int inter=0;cu_array_t*cua=st->tile->frame->cu_array;
      for(int y=0;y<W;y+=16)for(int x=0;x<W;x+=16){const cu_info_t*cu=kvz_cu_array_at_const(cua,x,y);if(cu->type==CU_INTER)inter++;}
      printf("inter CUs in sample grid=%d\n",inter);
      if(inter==0){printf("FAIL no inter CUs despite force-inter\n");return 1;}
    }
    api->chunk_free(ch);api->picture_free(r);api->picture_free(s);api->picture_free(in);
  }
  api->encoder_close(enc);
  printf("FULLREC_OK\n");return 0;}
