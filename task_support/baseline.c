#include "dumb.h"
#include "fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static int run(int channels,int quality,int release_row) {
    unsigned char data[1024];size_t length=fixture_it(data,sizeof(data),release_row);
    DUMBFILE *f;DUH *d;DUH_SIGRENDERER *s;DUMB_IT_SIGRENDERER *it;sample_t **buf;
    long got,total=0;unsigned long long magnitude=0;uint32_t hash=2166136261u;
    int i;
    if(!length)return 1;
    f=dumbfile_open_memory((const char*)data,length);if(!f)return 2;
    d=dumb_read_it_quick(f);dumbfile_close(f);if(!d)return 3;
    s=duh_start_sigrenderer(d,0,channels,0);if(!s)return 4;
    it=duh_get_it_sigrenderer(s);if(!it)return 5;
    dumb_it_set_resampling_quality(it,quality);
    dumb_it_set_loop_callback(it,dumb_it_callback_terminate,0);
    buf=allocate_sample_buffer(channels,257);if(!buf)return 6;
    while(total<4096){
        long n=4096-total;if(n>257)n=257;
        memset(buf[0],0,(size_t)n*channels*sizeof(sample_t));
        got=duh_sigrenderer_generate_samples(s,1.0f,65536.0f/12000,n,buf);
        if(got<=0)break;
        for(i=0;i<got*channels;i++){int32_t x=buf[0][i];magnitude+=(unsigned long long)(x<0?-(int64_t)x:x);hash=(hash^(uint32_t)x)*16777619u;}
        total+=got;
    }
    printf("IT_PLAY channels=%d quality=%d release_row=%d frames=%ld magnitude=%llu hash=%08x\n",channels,quality,release_row,total,magnitude,(unsigned)hash);
    destroy_sample_buffer(buf);duh_end_sigrenderer(s);unload_duh(d);
    return total==4096 && magnitude>0?0:7;
}
int main(void){
    if(run(1,DUMB_RQ_ALIASING,-1)||run(2,DUMB_RQ_LINEAR,-1)||run(2,DUMB_RQ_CUBIC,8))return 1;
    puts("BASELINE_OK real_it_playback");return 0;
}
