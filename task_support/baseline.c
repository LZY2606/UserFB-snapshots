#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "x264.h"
static uint64_t hash=UINT64_C(14695981039346656037);
int main(void){
 x264_param_t p; x264_picture_t in,out; x264_nal_t *nal; int nn,frames=0,bframes=0,bref=0,bytes=0,loss=0;
 if(x264_param_default_preset(&p,"veryfast",NULL)<0)return 2;
 p.i_width=64;p.i_height=64;p.i_threads=1;p.i_lookahead_threads=1;
 p.i_csp=X264_CSP_I420;p.i_bitdepth=8;p.b_vfr_input=0;p.i_fps_num=25;p.i_fps_den=1;
 p.i_bframe=3;p.i_bframe_adaptive=X264_B_ADAPT_NONE;p.i_bframe_pyramid=X264_B_PYRAMID_NORMAL;
 p.i_frame_reference=4;p.i_keyint_max=100;p.i_scenecut_threshold=0;
 p.rc.i_lookahead=4;p.rc.i_rc_method=X264_RC_ABR;p.rc.i_bitrate=120;p.rc.b_mb_tree=0;
 p.b_repeat_headers=1;p.b_annexb=1;p.i_log_level=X264_LOG_NONE;
 x264_t *h=x264_encoder_open(&p);if(!h)return 3;
 if(x264_picture_alloc(&in,X264_CSP_I420,64,64)<0)return 4;
 FILE *fp=fopen(".task-build/fixed.264","wb");if(!fp)return 5;
 for(int t=0;t<36||x264_encoder_delayed_frames(h);t++){
  if(t>=100)return 6;
  if(t<36){
   for(int c=0;c<3;c++){int w=c?32:64;for(int y=0;y<w;y++)for(int x=0;x<w;x++)
    in.img.plane[c][y*in.img.i_stride[c]+x]=(uint8_t)((x*7+y*3+((x+t*3)/9)*37+c*61+t*5)&255);}
   in.i_pts=t*40;in.i_type=X264_TYPE_AUTO;
  }
  int n=x264_encoder_encode(h,&nal,&nn,t<36?&in:NULL,&out);if(n<0)return 7;
  if(n){int sum=0;for(int k=0;k<nn;k++){
    if(fwrite(nal[k].p_payload,1,nal[k].i_payload,fp)!=(size_t)nal[k].i_payload)return 8;
    for(int j=0;j<nal[k].i_payload;j++)hash=(hash^nal[k].p_payload[j])*UINT64_C(1099511628211);
    sum+=nal[k].i_payload;}
   if(sum!=n)return 9;
   frames++;bytes+=n;bframes+=IS_X264_TYPE_B(out.i_type);bref+=out.i_type==X264_TYPE_BREF;
   if(frames==10){loss=x264_encoder_invalidate_reference(h,out.i_pts);if(loss!=-1)return 10;}
  }
 }
 if(fclose(fp))return 11;
 x264_picture_clean(&in);x264_encoder_close(h);
 if(frames!=36||!bframes||!bref||bytes<=0)return 12;
 printf("X264_BASELINE frames=%d B=%d BREF=%d bytes=%d fnv=%016llx legacy_loss=%d\n",frames,bframes,bref,bytes,(unsigned long long)hash,loss);
 if((fp=fopen("task_support/fixtures/fixed.264","rb"))!=NULL){
  FILE *now=fopen(".task-build/fixed.264","rb");if(!now)return 13;
  int a,b;do{a=fgetc(fp);b=fgetc(now);if(a!=b)return 14;}while(a!=EOF);
  fclose(fp);fclose(now);puts("LEGACY_BYTES_OK");
 }
 return 0;
}
