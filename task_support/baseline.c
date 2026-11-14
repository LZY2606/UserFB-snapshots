#include "task_support/dv_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    long long energy=0;unsigned compared=0;
    for(int pal=0;pal<2;pal++) {
        int n=pal?1920:1600;
        uint8_t frame[144000];task_make_dv(frame,pal,0);
        int16_t left[DV_AUDIO_MAX_SAMPLES],right[DV_AUDIO_MAX_SAMPLES];
        int16_t out0[DV_AUDIO_MAX_SAMPLES],out1[DV_AUDIO_MAX_SAMPLES],out2[DV_AUDIO_MAX_SAMPLES],out3[DV_AUDIO_MAX_SAMPLES];
        for(int i=0;i<DV_AUDIO_MAX_SAMPLES;i++){left[i]=(int16_t)((i*73)%20001-10000);right[i]=(int16_t)((i*131)%16001-8000);}
        int16_t *pcm[]={left,right};int16_t *out[]={out0,out1,out2,out3};
        dv_encoder_t *enc=dv_encoder_new(0,0,0);assert(enc);enc->isPAL=pal;enc->samples_this_frame=n;
        assert(dv_encode_full_audio(enc,pcm,2,48000,frame)==0);
        dv_decoder_t *dec=dv_decoder_new(0,0,0);assert(dec);assert(dv_parse_header(dec,frame)>=0);
        assert(dv_decode_full_audio(dec,frame,out));assert(dv_get_num_samples(dec)==n);assert(dv_get_num_channels(dec)==2);
        for(int i=0;i<n;i++){assert(out0[i]==left[i]);assert(out1[i]==right[i]);energy+=abs(out0[i])+abs(out1[i]);compared+=2;}
        dv_decoder_free(dec);dv_encoder_free(enc);
    }
    assert(energy>100000);printf("DV_AUDIO16_OK samples=%u energy=%lld standards=2\n",compared,energy);
    return 0;
}
