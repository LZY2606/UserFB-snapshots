#include "opus.h"
#include "opus_multistream.h"
#include <stdio.h>
#include <math.h>
#include <assert.h>

int main(void) {
    int err, k, i, c, n;
    const unsigned char map[4] = {0,1,2,3};
    unsigned char packet[8192];
    opus_int16 pcm[960*4], out[960*4];
    double energy[4] = {0};
    OpusMSEncoder *enc = opus_multistream_encoder_create(48000,4,3,1,map,OPUS_APPLICATION_AUDIO,&err);
    assert(enc && err == OPUS_OK);
    OpusMSDecoder *dec = opus_multistream_decoder_create(48000,4,3,1,map,&err);
    assert(dec && err == OPUS_OK);
    assert(opus_multistream_encoder_ctl(enc,OPUS_SET_BITRATE(144000)) == OPUS_OK);
    for (k=0;k<6;++k) {
        for (i=0;i<960;++i) for (c=0;c<4;++c)
            pcm[4*i+c]=(opus_int16)(3000*sin((k*960+i)*(0.017+c*0.013)));
        n=opus_multistream_encode(enc,pcm,960,packet,sizeof packet);
        assert(n>0);
        assert(opus_multistream_decode(dec,packet,n,out,960,0)==960);
        for (i=0;i<960;++i) for (c=0;c<4;++c) energy[c]+=(double)out[4*i+c]*out[4*i+c];
    }
    for (c=0;c<4;++c) assert(energy[c]>10000);
    opus_multistream_encoder_destroy(enc);
    opus_multistream_decoder_destroy(dec);
    printf("BASELINE_MS_OK packets=6 frames=5760 streams=3 coupled=1 energy=%.0f,%.0f,%.0f,%.0f\n",energy[0],energy[1],energy[2],energy[3]);
    return 0;
}
