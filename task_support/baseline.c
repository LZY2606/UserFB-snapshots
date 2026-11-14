#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "pl_mpeg.h"
#include "fixture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static void require_impl(int v,int line) {if(!v){fprintf(stderr,"BASELINE_ASSERT line=%d\n",line);exit(1);}}
#define require(v) require_impl(!!(v),__LINE__)
static void fill(fixture_t *es,unsigned w,unsigned h,unsigned rate,unsigned gray) {
  fixture_sequence(es,w,h,rate,1);
  fixture_intra(es,w,h,0,gray);fixture_intra(es,w,h,1,gray);fixture_end(es);
}
static void ended_source(plm_buffer_t *b,void *user) {(void)user;plm_buffer_signal_end(b);}
static plm_video_t *video(fixture_t *es) {
  plm_buffer_t *b=plm_buffer_create_with_capacity(64);
  plm_buffer_write(b,es->bytes,fixture_size(es));
  plm_buffer_set_load_callback(b,ended_source,NULL);
  plm_buffer_signal_end(b);
  return plm_video_create_with_buffer(b,1);
}
int main(int argc,char **argv) {
  fixture_t one={0},chain={0},ps={0};
  fill(&one,16,16,3,128);
  plm_video_t *v=video(&one);
  plm_video_set_no_delay(v,1);
  unsigned count=0;plm_frame_t *f;
  while((f=plm_video_decode(v))) {
    require(f->width==16 && f->height==16);
    for(unsigned i=0;i<f->y.width*f->y.height;i++)require(f->y.data[i]==128);
    for(unsigned i=0;i<f->cb.width*f->cb.height;i++)require(f->cb.data[i]==128 && f->cr.data[i]==128);
    require(fabs(f->time-count*.04)<1e-9);require(++count<10);
  }
  require(count==2);plm_video_destroy(v);
  printf("PLM_SINGLE_BASELINE frames=%u size=16x16 luma=128\n",count);
  if(argc==1) {
  fill(&chain,16,16,3,128);fill(&chain,32,16,5,144);
  v=video(&chain);count=0;unsigned width=0;
  while((f=plm_video_decode(v))) {width=f->width;require(++count<10);}
  /* Observe the missing feature without declaring it implemented. */
  require(count==4 && width==16);
  printf("PLM_CHAIN_CURRENT frames=%u last_width=%u requested_second_width=32\n",count,width);
  plm_video_destroy(v);
  }
  fixture_ps(&ps,&one);plm_t *p=plm_create_with_memory(ps.bytes,fixture_size(&ps),0);
  require(p!=NULL);plm_set_audio_enabled(p,0);count=0;
  while((f=plm_decode_video(p))) {require(f->width==16);require(++count<10);}
  require(count==2);plm_destroy(p);
  printf("PLM_PS_BASELINE frames=%u\n",count);
  (void)argv;return 0;
}
