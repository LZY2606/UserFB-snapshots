#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "y262api.h"
enum { W=64, H=64, N=14 };
static unsigned seen[N], types[4], count, units, bytes, errors;
static uint64_t sh=UINT64_C(1469598103934665603), rh=UINT64_C(1469598103934665603);
static uint64_t hash(uint64_t h,const uint8_t *p,size_t n){for(size_t k=0;k<n;k++){h^=p[k];h*=UINT64_C(1099511628211);}return h;}
static void error_cb(void *u,int32_t code,int8_t *text){(void)u;fprintf(stderr,"ENCODER_ERROR %d %s\n",code,text);errors++;}
static void result_cb(void *u,int32_t kind,y262_result_t *r){
 (void)u;
 if(kind==Y262_RESULT_BITSTREAM){assert(r->bitstream_unit.i_unit_length>0);units++;bytes+=(unsigned)r->bitstream_unit.i_unit_length;sh=hash(sh,r->bitstream_unit.pui8_unit,(size_t)r->bitstream_unit.i_unit_length);}
 if(kind==Y262_RESULT_RECON){int p=r->recon.i_pon,t=r->recon.i_frame_type;assert(p>=0&&p<N&&t>=1&&t<=3);assert(!seen[p]++);types[t]++;count++;rh=hash(rh,r->recon.pui8_luma,W*H);rh=hash(rh,r->recon.pui8_cb,W*H/4);rh=hash(rh,r->recon.pui8_cr,W*H/4);}
}
int main(void){
 y262_configuration_t c;void *e=y262_create(&c);assert(e);
 c.i_coded_width=c.i_display_width=W;c.i_coded_height=c.i_display_height=H;
 c.i_multithreading=0;c.i_num_threads=1;c.i_lookahead_pictures=10;c.i_bframes=2;c.i_keyframe_ref_distance=2;
 c.i_rcmode=BITRATE_CONTROL_CQ;c.i_quantizer=8;c.b_variance_aq=0;c.i_quality_for_speed=0;
 c.pf_result_callback=result_cb;c.pf_error_callback=error_cb;
 int rc=y262_initialize(e,&c);assert(rc==Y262_INIT_SUCCESS);
 uint8_t y[W*H],cb[W*H/4],cr[W*H/4];
 for(int f=0;f<N;f++){
  for(int j=0;j<H;j++)for(int i=0;i<W;i++)y[j*W+i]=(uint8_t)(32+(((i+f*2)/4*11+j/4*7)%192));
  for(int j=0;j<H/2;j++)for(int i=0;i<W/2;i++){cb[j*W/2+i]=(uint8_t)(96+(i/3+f)%40);cr[j*W/2+i]=(uint8_t)(112+(j/3+f)%40);}
  y262_input_picture_t p;memset(&p,0,sizeof(p));p.pui8_luma=y;p.pui8_cb=cb;p.pui8_cr=cr;p.i_frame_structure=Y262_INPUT_PICTURE_FRAME_PROGRESSIVE;
  assert(y262_push_input_picture(e,&p,f)==Y262_PUSH_INPUT_CONTINUE);
 }
 int done=0;for(int k=0;k<128;k++){rc=y262_push_input_picture(e,NULL,N);assert(rc==Y262_PUSH_INPUT_CONTINUE||rc==Y262_PUSH_INPUT_FLUSHED);if(rc==Y262_PUSH_INPUT_FLUSHED){done=1;break;}}
 assert(done&&count==N&&units>0&&bytes>0&&types[1]>0&&types[2]>0&&types[3]>0&&errors==0);
 for(int i=0;i<N;i++)assert(seen[i]==1);
 y262_deinitialize(e);
 printf("Y262_BASELINE frames=%u I=%u P=%u B=%u units=%u bytes=%u stream=%016llx recon=%016llx\n",count,types[1],types[2],types[3],units,bytes,(unsigned long long)sh,(unsigned long long)rh);
 puts("Y262_REAL_IPB_ENCODING_OK");return 0;
}
