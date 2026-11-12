#include "lc3.h"
#include "mdct.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
    float x[480], y[480], d[480] = {0}, z[480], q[480] = {0};
    for (int i=0;i<480;i++) x[i]=.2f*sinf((float)i*.11f);
    lc3_mdct_forward(LC3_DT_10M,LC3_SRATE_48K,LC3_SRATE_16K,x,d,y);
    for (int i=160;i<480;i++) y[i]=0;
    lc3_mdct_inverse(LC3_DT_10M,LC3_SRATE_48K,LC3_SRATE_16K,y,q,z);
    double energy=0; for(int i=0;i<480;i++) { assert(isfinite(z[i]));energy+=z[i]*z[i]; }
    assert(energy>0); puts("MDCT_CROSS_RATE_BASELINE_OK");
    void *em=malloc(lc3_encoder_size(10000,48000));
    void *dm=malloc(lc3_decoder_size(10000,48000));
    lc3_encoder_t e=lc3_setup_encoder(10000,16000,48000,em);
    lc3_decoder_t h=lc3_setup_decoder(10000,16000,48000,dm);
    assert(e && h); unsigned char packet[40]; double out_energy=0;
    for (int f=0; f<8;f++) {
        for(int i=0;i<480;i++) x[i]=.2f*sinf((float)(f*480+i)*.11f);
        assert(lc3_encode(e,LC3_PCM_FORMAT_FLOAT,x,1,40,packet)==0);
        assert(lc3_decode(h,packet,40,LC3_PCM_FORMAT_FLOAT,y,1)==0);
        for(int i=0;i<480;i++) {assert(isfinite(y[i]));out_energy+=y[i]*y[i];}
    }
    assert(out_energy>1); free(em);free(dm);
    puts("LC3_16K_CODEC_48K_PCM_OK frames=8"); return 0;
}
