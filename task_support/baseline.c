#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
#include "tiny_stream.h"
#include <stdio.h>
/* Flat floor only: this proves real decoding, not interior floor1 coverage. */
int main(void) {
    unsigned char data[2048]; unsigned size=tiny_stream(data);
    FILE *raw=fopen(".task-build/tiny.ogg","wb");
    if (!raw || fwrite(data,1,size,raw)!=size) return 3;
    if (fclose(raw)) return 3;
    int err=0; stb_vorbis *f=stb_vorbis_open_memory(data,(int)size,&err,NULL);
    if (!f) { printf("OPEN_ERROR %d bytes=%u\n",err,size); return 1; }
    stb_vorbis_info info=stb_vorbis_get_info(f);
    float **pcm; int channels=0,n,total=0,frames=0; double energy=0;
    while ((n=stb_vorbis_get_frame_float(f,&channels,&pcm))>0) {
        for(int i=0;i<n;i++) energy+=(double)pcm[0][i]*pcm[0][i];
        total+=n; frames++;
    }
    err=stb_vorbis_get_error(f); stb_vorbis_close(f);
    printf("VORBIS_BASE bytes=%u channels=%d rate=%u frames=%d samples=%d energy=%.9g error=%d\n",
           size,channels,info.sample_rate,frames,total,energy,err);
    return channels==1 && info.sample_rate==8000 && frames==2 && total==256 &&
           isfinite(energy) && energy>0 && err==0 ? 0:2;
}
