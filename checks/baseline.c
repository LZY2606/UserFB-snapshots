#define MA_NO_DEVICE_IO
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 float w0[2]={1,0},w1[2]={1,0},w2[2]={1,0};
 float *weights[3]={w0,w1,w2};
 ma_channel_converter_config c=ma_channel_converter_config_init(ma_format_s16,3,NULL,2,NULL,ma_channel_mix_mode_custom_weights);
 ma_channel_converter x;ma_int16 a[3]={30000,30000,-30000},b[3]={30000,-30000,30000},o[2],p[2];
 c.ppWeights=weights;
 assert(ma_channel_converter_init(&c,NULL,&x)==MA_SUCCESS);
 assert(ma_channel_converter_process_pcm_frames(&x,o,a,1)==MA_SUCCESS);
 assert(ma_channel_converter_process_pcm_frames(&x,p,b,1)==MA_SUCCESS);
 /* Both orderings must now give the full cancellation result; the old per-add-clipping output was 2767. */
 assert(o[0]==30000 && p[0]==30000 && o[1]==0 && p[1]==0);
 printf("baseline: mix(30000,30000,-30000)=%d reordered=%d; ideal=30000\n",o[0],p[0]);
 ma_channel_converter_uninit(&x,NULL);
}
