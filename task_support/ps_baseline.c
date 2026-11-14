#include <stdio.h>
#include <math.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"CONTENT_MISMATCH %s:%d %s\n",__FILE__,__LINE__,#x);return 42;}}while(0)
int main(void){
 plm_demux_t*d=plm_demux_create(plm_buffer_create_with_filename("task_support/media/dual.ps"),1);CHECK(d&&plm_demux_has_headers(d));CHECK(plm_demux_get_num_audio_streams(d)==2);
 plm_audio_t*a[2];plm_buffer_t*b[2];int count[2]={0};double energy[2]={0};
 for(int c=0;c<2;c++){b[c]=plm_buffer_create_with_capacity(512);a[c]=plm_audio_create_with_buffer(b[c],1);CHECK(a[c]);}
 plm_packet_t*p;int rates[2]={32000,48000};int step[2]={3240,2160},offset[2]={0,1080};
 while((p=plm_demux_decode(d))){
  int c=p->type-0xc0;CHECK(c>=0&&c<2&&count[c]<8);
  CHECK(llround(p->pts*90000)==90000+offset[c]+step[c]*count[c]);
  plm_buffer_write(b[c],p->data,p->length);plm_samples_t*s=plm_audio_decode(a[c]);CHECK(s&&s->count==1152);CHECK(plm_audio_get_samplerate(a[c])==rates[c]);
  for(int n=0;n<2304;n++){CHECK(isfinite(s->interleaved[n]));energy[c]+=s->interleaved[n]*s->interleaved[n];}count[c]++;
 }
 CHECK(count[0]==8&&count[1]==8&&energy[0]>1&&energy[1]>1);
 plm_audio_destroy(a[0]);plm_audio_destroy(a[1]);plm_demux_destroy(d);puts("PS_AUDIO_BASELINE_OK tracks=2 frames=8,8 rates=32000,48000 pts=verified");return 0;
}
