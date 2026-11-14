#include "wavpack.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define N 257
#define CH 2
#include "reference.h"
static int output(void *id, void *data, int32_t n) { return fwrite(data,1,n,(FILE *)id)==(size_t)n; }
static WavpackContext *open_audio(int flags) {
    char error[100];
    WavpackContext *w=WavpackOpenFileInput(".task-build/dsd.wv",error,flags,0);
    if(!w) fprintf(stderr,"open: %s\n",error);
    return w;
}
int main(void) {
    int32_t source[N*CH], native[N*CH], whole[N*CH], small[N*CH], seekbuf[24*CH];
    WavpackConfig cfg={0}; WavpackContext *w; FILE *f;
    uint32_t state=0x12345678u; int i, mismatches=0, seekdiff=0;
    for(i=0;i<N*CH;i++) { state=state*1664525u+1013904223u; source[i]=(state>>24)&255; }
    f=fopen(".task-build/dsd.wv","wb"); if(!f) return 10;
    w=WavpackOpenFileOutput(output,f,NULL); if(!w) return 11;
    cfg.num_channels=CH; cfg.channel_mask=3; cfg.sample_rate=352800;
    cfg.bytes_per_sample=1;cfg.bits_per_sample=8;cfg.qmode=QMODE_DSD_MSB_FIRST;cfg.block_samples=64;
    if(!WavpackSetConfiguration64(w,&cfg,N,NULL)||!WavpackPackInit(w)||!WavpackPackSamples(w,source,N)||!WavpackFlushSamples(w)) return 12;
    WavpackCloseFile(w);fclose(f);
    w=open_audio(OPEN_DSD_NATIVE); if(!w) return 13;
    if(WavpackUnpackSamples(w,native,N)!=N||memcmp(native,source,sizeof(source))||WavpackGetNumErrors(w)) return 14;
    WavpackCloseFile(w); puts("NATIVE_ROUNDTRIP_OK frames=257 channels=2");
    w=open_audio(OPEN_DSD_AS_PCM);if(!w) return 15;
    if(WavpackUnpackSamples(w,whole,N)!=N||memcmp(whole,dsd_pcm_reference,sizeof(whole))||WavpackGetNumErrors(w)) return 16;
    WavpackCloseFile(w);
    w=open_audio(OPEN_DSD_AS_PCM);if(!w) return 17;
    for(i=0;i<N;i++) if(WavpackUnpackSamples(w,small+i*CH,1)!=1||WavpackGetSampleIndex64(w)!=i+1) return 18;
    if(WavpackUnpackSamples(w,seekbuf,1)||WavpackGetNumErrors(w)) return 19;
    for(i=0;i<N*CH;i++) mismatches+=whole[i]!=small[i];
    if(!WavpackSeekSample64(w,3)||WavpackUnpackSamples(w,seekbuf,24)!=24) return 20;
    for(i=0;i<24*CH;i++) seekdiff+=whole[3*CH+i]!=seekbuf[i];
    WavpackCloseFile(w);
    printf("DSD_PCM_CHUNK_MISMATCHES=%d\nDSD_PCM_SEEK_MISMATCHES=%d\n",mismatches,seekdiff);
    puts("BASELINE_OK");return 0;
}
