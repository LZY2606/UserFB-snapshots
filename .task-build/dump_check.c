#include "kvazaar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define W 32
#define H 32
static void fill(kvz_picture*p,int f){for(int c=0;c<3;++c){int w=c?W/2:W,h=c?H/2:H,st=c?p->stride_c:p->stride;for(int y=0;y<h;++y)for(int x=0;x<w;++x)p->data[c][y*st+x]=(unsigned char)((13*x+7*y+f*31+c*17)&255);}}
static unsigned pos;
static unsigned getb(const unsigned char*p,int n){unsigned v=0;for(int i=0;i<n;++i)v=(v<<1)|((p[(pos+i)/8]>>(7-((pos+i)%8)))&1);pos+=n;return v;}
static unsigned ue(const unsigned char*p){int z=0;while(!getb(p,1))++z;unsigned v=(1u<<z)-1;if(z)v+=getb(p,z);return v;}
static int se(const unsigned char*p){unsigned c=ue(p);return c&1?(int)((c+1)/2):-(int)(c/2);}
int main(void){
  const kvz_api*api=kvz_api_get(8);
  kvz_config*cfg=api->config_alloc();api->config_init(cfg);
  const char*o[][2]={{"gop","0"},{"ref","1"},{"bipred","0"},{"wpp","0"},{"cpuid","0"},{"deblock","0"},{"sao","off"},{"info","0"},{"force-inter","1"},{"weighted-pred","1"},{"hash","none"},{"subme","0"}};
  for(size_t i=0;i<sizeof(o)/sizeof(o[0]);++i)api->config_parse(cfg,o[i][0],o[i][1]);
  cfg->width=W;cfg->height=H;cfg->owf=0;cfg->threads=1;cfg->enable_logging_output=0;
  kvz_encoder*enc=api->encoder_open(cfg);api->config_destroy(cfg);
  kvz_data_chunk*hdr=0;uint32_t hn=0;api->encoder_headers(enc,&hdr,&hn);
  size_t cap=1<<16;unsigned char*all=malloc(cap);size_t tot=0;
  for(kvz_data_chunk*c=hdr;c;c=c->next){memcpy(all+tot,c->data,c->len);tot+=c->len;}api->chunk_free(hdr);
  for(int f=0;f<2;++f){kvz_picture*in=api->picture_alloc(W,H);fill(in,f);in->pts=f;
    if(f){in->wp.enabled=1;in->wp.denom=1;in->wp.weight[0]=2-3;in->wp.weight[1]=2+5;in->wp.weight[2]=2;in->wp.offset[0]=7;in->wp.offset[1]=-4;in->wp.offset[2]=0;}
    kvz_picture*rec=0,*src=0;kvz_data_chunk*ch=0;uint32_t b=0;kvz_frame_info info;
    api->encoder_encode(enc,in,&ch,&b,&rec,&src,&info);api->picture_free(in);
    for(kvz_data_chunk*c=ch;c;c=c->next){memcpy(all+tot,c->data,c->len);tot+=c->len;}
    if(ch){api->chunk_free(ch);api->picture_free(rec);api->picture_free(src);}}
  unsigned char*rb=malloc(tot);size_t rn=0;int z=0;
  for(size_t i=0;i<tot;++i){if(z==2&&all[i]==3){z=0;continue;}rb[rn++]=all[i];z=all[i]?0:z+1;}
  // find last NAL (P slice, type 1)
  size_t starts[16];int nst=0;
  for(size_t i=0;i+3<=rn;++i)if(rb[i]==0&&rb[i+1]==0&&rb[i+2]==1)starts[nst++]=i+3;
  size_t ps=starts[nst-1]; const unsigned char*p=rb+ps;
  pos=16; // nal header
  getb(p,1); // first_slice
  ue(p); // pps id
  unsigned stype=ue(p); printf("slice_type=%u (P=1)\n",stype);
  unsigned poc=getb(p,4); printf("poc_lsb=%u\n",poc);
  getb(p,1); // st_ref_pic_set_sps_flag
  unsigned nneg=ue(p); printf("num_neg=%u\n",nneg);
  for(unsigned k=0;k<nneg;++k){ue(p);getb(p,1);}
  getb(p,1); // slice_temporal_mvp_enabled_flag (tmvp default on)
  getb(p,1); // num_ref_idx_override
  unsigned nref=ue(p); printf("num_ref_l0_minus1=%u\n",nref);
  unsigned mrg=ue(p); printf("5-max_merge=%u\n",mrg);
  printf("slice_qp_delta=%d\n",se(p));
  // PRED WEIGHT TABLE
  unsigned ld=ue(p); printf("luma_log2_weight_denom=%u\n",ld);
  int dcd=se(p); printf("delta_chroma_log2_denom=%d\n",dcd);
  unsigned lf=getb(p,1); printf("luma_weight_l0_flag=%u\n",lf);
  if(lf){printf("  delta_luma_w=%d\n",se(p));printf("  luma_offset=%d\n",se(p));}
  unsigned cf=getb(p,1); printf("chroma_weight_l0_flag=%u\n",cf);
  if(cf)for(int c=0;c<2;++c){printf("  c%d delta_w=%d delta_off=%d\n",c,se(p),se(p));}
  printf("PARSE_OK\n");
  api->encoder_close(enc);return 0;
}
