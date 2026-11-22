#include <lc3.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"BASELINE_FAIL line=%d %s\n",__LINE__,#x); return 1; } } while (0)
static int run(int dt, int sr, int pcmrate) {
    int ns=lc3_frame_samples(dt,pcmrate), bytes=lc3_frame_bytes(dt,64000);
    void *em=calloc(1,lc3_encoder_size(dt,pcmrate));
    void *am=calloc(1,lc3_decoder_size(dt,pcmrate));
    void *bm=calloc(1,lc3_decoder_size(dt,pcmrate));
    CHECK(em && am && bm && ns>0 && ns<=480 && bytes>0);
    lc3_encoder_t e=lc3_setup_encoder(dt,sr,pcmrate,em);
    lc3_decoder_t a=lc3_setup_decoder(dt,sr,pcmrate,am);
    lc3_decoder_t b=lc3_setup_decoder(dt,sr,pcmrate,bm);
    CHECK(e && a && b);
    int16_t in[480], outa[960], outb[960]; unsigned char packet[400];
    long energy=0; int changed=0;
    for(int frame=0;frame<12;frame++) {
        for(int i=0;i<ns;i++) in[i]=(int16_t)(((i+frame*ns)*173%16001)-8000);
        CHECK(lc3_encode(e,LC3_PCM_FORMAT_S16,in,1,bytes,packet)==0);
        for(int i=0;i<2*ns;i++) outa[i]=outb[i]=12345;
        int lost=frame==6 || frame==7;
        CHECK(lc3_decode(a,lost?NULL:packet,lost?0:bytes,LC3_PCM_FORMAT_S16,outa,2)==lost);
        CHECK(lc3_decode(b,packet,bytes,LC3_PCM_FORMAT_S16,outb,2)==0);
        for(int i=0;i<ns;i++) {
            CHECK(outa[2*i+1]==12345 && outb[2*i+1]==12345);
            energy+=abs(outb[2*i]);
            if(frame>=6 && outa[2*i]!=outb[2*i])changed++;
        }
    }
    CHECK(energy>1000 && changed>0);
    printf("LC3_PRODUCTION dt=%d sr=%d pcm=%d frames=12 samples=%d energy=%ld plc_changed=%d\n",dt,sr,pcmrate,12*ns,energy,changed);
    free(em); free(am); free(bm); return 0;
}
int main(void) {
    const int rates[]={8000,16000,24000,32000,48000};
    for(int d=0;d<2;d++)for(int r=0;r<5;r++)if(run(d?10000:7500,rates[r],48000))return 1;
    puts("LC3_BASELINE_OK configs=10 frames=120"); return 0;
}
