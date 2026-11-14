/* SPDX-License-Identifier: MIT; normal encoder smoke, no SAO target oracle. */
#include "kvazaar.h"
#include "kvazaar_internal.h"
#include "encoderstate.h"
#include "videoframe.h"
#include "sao.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"sao-smoke:%d %s\n",__LINE__,#x);return 1;}}while(0)
static void count(const encoder_state_t *s,int *nonzero,int *merge){
 if(s->is_leaf){videoframe_t*f=s->tile->frame;
  for(int i=0;i<f->width_in_lcu*f->height_in_lcu;i++){
   *nonzero+=f->sao_luma[i].type!=SAO_TYPE_NONE;
   *merge+=f->sao_luma[i].merge_left_flag||f->sao_luma[i].merge_up_flag;
  }
 }
 for(int i=0;s->children&&s->children[i].encoder_control;i++)count(&s->children[i],nonzero,merge);
}
int main(void){
 const kvz_api *a=kvz_api_get(8);CHECK(a);kvz_config*c=a->config_alloc();CHECK(c&&a->config_init(c));
 const char*opts[][2]={{"preset","ultrafast"},{"gop","0"},{"ref","1"},{"bipred","0"},{"threads","1"},{"owf","0"},{"wpp","0"},{"cpuid","0"},{"qp","35"},{"sao","full"},{"info","0"}};
 for(unsigned i=0;i<sizeof(opts)/sizeof(opts[0]);i++)CHECK(a->config_parse(c,opts[i][0],opts[i][1]));
 c->width=136;c->height=130;c->enable_logging_output=0;
 kvz_encoder*e=a->encoder_open(c);CHECK(e);a->config_destroy(c);
 int frames=0,nonzero=0,merge=0;uint64_t digest=14695981039346656037ULL;size_t bytes_total=0;
 for(int i=0;i<6;i++){
  kvz_picture*p=i<3?a->picture_alloc(136,130):NULL,*rec=NULL,*src=NULL;kvz_data_chunk*chunks=NULL;uint32_t bytes=0;kvz_frame_info info;
  if(p){for(int col=0;col<3;col++){int w=col?68:136,h=col?65:130,s=col?p->stride_c:p->stride;
   for(int y=0;y<h;y++)for(int x=0;x<w;x++)p->data[col][y*s+x]=(x*3+y*5+((x/9)^(y/7))*17+col*37+i*2)&255;
  }p->pts=40*i;}
  unsigned state=e->cur_state_num;
  CHECK(a->encoder_encode(e,p,&chunks,&bytes,&rec,&src,&info));if(p)a->picture_free(p);
  if(!chunks){CHECK(bytes==0&&!rec&&!src);continue;}
  CHECK(rec&&src&&rec->width==136&&rec->height==130&&info.poc==frames&&bytes>0);
  CHECK(info.slice_type==(frames?KVZ_SLICE_P:KVZ_SLICE_I));
  count(&e->states[state],&nonzero,&merge);
  for(int col=0;col<3;col++){int w=col?68:136,h=col?65:130,s=col?rec->stride_c:rec->stride;
   for(int y=0;y<h;y++)for(int x=0;x<w;x++){digest^=rec->data[col][y*s+x];digest*=1099511628211ULL;}}
  bytes_total+=bytes;frames++;a->chunk_free(chunks);a->picture_free(rec);a->picture_free(src);
 }
 a->encoder_close(e);CHECK(frames==3&&nonzero>0&&bytes_total>0);
 printf("SAO_SMOKE_COMPLETE frames=%d active=%d merge=%d bytes=%zu digest=%016llx\n",frames,nonzero,merge,bytes_total,(unsigned long long)digest);return 0;
}
