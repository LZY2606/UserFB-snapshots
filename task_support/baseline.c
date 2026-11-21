#include "uavs3e.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static uint64_t mix(uint64_t h,const unsigned char *p,int n){while(n--)h=(h^*p++)*1099511628211ULL;return h;}
static int run(uint64_t *hash,int *bytes){
 enc_cfg_t c;uavs3e_load_default_cfg(&c);
 c.horizontal_size=128;c.vertical_size=256;c.fps_num=25;c.fps_den=1;c.bit_depth_input=8;c.bit_depth_internal=8;
 c.ctu_size=64;c.max_b_frames=0;c.frm_threads=1;c.wpp_threads=1;c.adaptive_gop=0;c.lookahead=0;c.i_period=16;c.speed_level=0;
 c.qp=34;c.rc_type=RC_TYPE_NULL;c.adaptive_dqp=0;c.alf_enable=0;c.sao_enable=0;c.use_pic_sign=0;c.use_deblock=1;
 int err=0;void *h=uavs3e_create(&c,&err);if(!h){fprintf(stderr,"create error %d\n",err);return 1;}
 *hash=1469598103934665603ULL;*bytes=0;int count=0;
 for(int f=0;f<3;++f){com_img_t *img=NULL;if(uavs3e_get_img(h,&img)!=COM_OK)return 2;
  for(int k=0;k<3;++k)for(int y=0;y<img->height[k];++y){unsigned char *p=(unsigned char*)img->planes[k]+y*img->stride[k];for(int x=0;x<img->width[k];++x)p[x]=(unsigned char)((x*13+y*7+(x*y)%37+f*11+k*61)&255);}
  img->pts=f;enc_stat_t s;memset(&s,0,sizeof(s));int ret=uavs3e_enc(h,&s,img);
  if(ret!=COM_OK||s.bytes<=0||!s.rec_img)return 3;*bytes+=s.bytes;*hash=mix(*hash,s.buf,s.bytes);
  for(int k=0;k<3;++k)for(int y=0;y<s.rec_img->height[k];++y)*hash=mix(*hash,(unsigned char*)s.rec_img->planes[k]+y*s.rec_img->stride[k],s.rec_img->width[k]);
  printf("FRAME poc=%lld type=%d bytes=%d qp=%.1f\n",s.poc,s.type,s.bytes,s.qp);++count;
 }
 enc_stat_t s;memset(&s,0,sizeof(s));int end=uavs3e_enc(h,&s,NULL);uavs3e_free(h);return count==3&&end==COM_OK_NO_MORE_FRM?0:4;
}
int main(void){uint64_t a,b;int x,y;int r=run(&a,&x);if(r)return r;r=run(&b,&y);if(r)return r;if(a!=b||x!=y)return 5;printf("ENCODE_BASELINE_OK frames=3 bytes=%d hash=%016llx\n",x,(unsigned long long)a);puts("BASELINE_FULL_FRAME_OUTPUT");return 0;}
