#include "ltc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
 LTCEncoder *e=ltc_encoder_create(48000,25,LTC_TV_625_50,0);
 LTCDecoder *d=ltc_decoder_create(1920,16);assert(e && d);
 SMPTETimecode t={0};t.hours=1;t.mins=2;t.secs=3;t.frame=4;
 ltc_encoder_set_timecode(e,&t);ltc_encoder_set_filter(e,0);
 ltcsnd_sample_t pcm[8000];int n=0;
 for(int i=0;i<3;i++){ltc_encoder_encode_frame(e);n+=ltc_encoder_copy_buffer(e,pcm+n);ltc_encoder_inc_timecode(e);}
 assert(ltc_encoder_end_encode(e)==0);n+=ltc_encoder_copy_buffer(e,pcm+n);
 const ltc_off_t origin=8589934592LL;
 for(int off=0;off<n;){int k=n-off<317?n-off:317;ltc_decoder_write(d,pcm+off,k,origin+off);off+=k;}
 LTCFrameExt f;int count=0;
 while(ltc_decoder_read(d,&f)==1){ltc_frame_to_time(&t,&f.ltc,0);
  assert(t.hours==1 && t.mins==2 && t.secs==3 && t.frame==4+count);
  assert(f.off_end>origin && f.off_end>=f.off_start && !f.reverse);
  printf("frame=%d start=%lld end=%lld\n",t.frame,(long long)f.off_start,(long long)f.off_end);count++;
 }
 assert(count==3);puts("BASELINE_LTC_THREE_FRAMES_64BIT_OFFSETS_OK");
 ltc_encoder_free(e);ltc_decoder_free(d);return 0;
}
