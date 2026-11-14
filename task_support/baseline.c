#include "layer3.h"
#include "types.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3/minimp3.h"

int main(void) {
  shine_config_t cfg;
  shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels = PCM_STEREO;
  cfg.wave.samplerate = 48000;
  cfg.mpeg.mode = JOINT_STEREO;
  shine_t enc = shine_initialise(&cfg);
  if (!enc) return 1;
  int16_t pcm[2304];
  unsigned char stream[16384];
  int size=0, written=0;
  for (int frame=0; frame<12; ++frame) {
    for (int i=0;i<1152;i++) {
      int t=frame*1152+i;
      pcm[2*i]=(int16_t)(9000*sin(t*0.075)+4000*sin(t*1.8));
      pcm[2*i+1]=(int16_t)(6000*cos(t*0.113)+3000*sin(t*1.7));
    }
    unsigned char *p=shine_encode_buffer_interleaved(enc,pcm,&written);
    if (!p || written<0 || size+written>(int)sizeof stream) return 2;
    memcpy(stream+size,p,written);size+=written;
  }
  unsigned char *tail=shine_flush(enc,&written);
  memcpy(stream+size,tail,written);size+=written;
  printf("BASELINE_ENCODE bytes=%d mode_ext=%d scalefac_compress=%u\n",
         size,((shine_global_config*)enc)->mpeg.mode_ext,
         ((shine_global_config*)enc)->side_info.gr[0].ch[1].tt.scalefac_compress);
  mp3dec_t dec;mp3dec_init(&dec);
  mp3d_sample_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];
  int at=0, frames=0;long long energy=0;
  while(at<size) {
    mp3dec_frame_info_t info;
    int n=mp3dec_decode_frame(&dec,stream+at,size-at,decoded,&info);
    if(info.frame_bytes<=0) return 3;
    at+=info.frame_bytes;
    if(n!=1152 || info.channels!=2 || info.hz!=48000) return 4;
    ++frames;
    for(int i=0;i<2*n;i++) energy+=(long long)decoded[i]*decoded[i];
  }
  shine_close(enc);
  if(frames!=12 || energy==0 || size!=12*384) return 5;
  printf("BASELINE_DECODE frames=%d samples_per_channel=%d nonzero=%d\n",frames,frames*1152,energy>0);
  puts("SHINE_BASELINE_OK");
  return 0;
}
