#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libmpegts.h"

static uint8_t *trace;
static int64_t *ticks;
static size_t length, packets;

static void collect(ts_writer_t *w, ts_frame_t *f, int n) {
    uint8_t *out=NULL; int len=0; int64_t *clock=NULL;
    int rc = ts_write_frames(w,f,n,&out,&len,&clock);
    if (rc) { fprintf(stderr,"write failed %d\n",rc); exit(1); }
    if(len<0 || len%188) { fprintf(stderr,"bad len\n"); exit(1); }
    if(!len) return;
    trace=realloc(trace,length+len);
    ticks=realloc(ticks,(packets+len/188)*sizeof(*ticks));
    memcpy(trace+length,out,len);
    memcpy(ticks+packets,clock,(size_t)len/188*sizeof(*ticks));
    length+=len; packets+=len/188;
}

int main(void){
    ts_stream_t v1={0}, a1={0}, v2={0};
    v1.pid=0x100; v1.stream_format=LIBMPEGTS_VIDEO_MPEG2; v1.stream_id=LIBMPEGTS_STREAM_ID_MPEGVIDEO;
    a1.pid=0x101; a1.stream_format=LIBMPEGTS_AUDIO_MPEG1; a1.stream_id=LIBMPEGTS_STREAM_ID_MPEGAUDIO; a1.audio_frame_size=216;
    v2.pid=0x200; v2.stream_format=LIBMPEGTS_VIDEO_AVC; v2.stream_id=LIBMPEGTS_STREAM_ID_MPEGVIDEO;
    ts_stream_t s1[2]={v1,a1};
    ts_program_t p1={0}; p1.pmt_pid=0x1000; p1.program_num=1; p1.pcr_pid=0x110; p1.num_streams=2; p1.streams=s1;
    ts_program_t p2={0}; p2.pmt_pid=0x1001; p2.program_num=2; p2.pcr_pid=0x200; p2.num_streams=1; p2.streams=&v2;
    ts_program_t ps[2]={p1,p2};
    ts_main_t m={0}; m.num_programs=2; m.programs=ps; m.ts_id=23; m.muxrate=4000000; m.cbr=1; m.lowlatency=1; m.ts_type=TS_TYPE_GENERIC; m.pcr_period=20; m.pat_period=80;
    ts_writer_t *w=ts_create_writer();
    if(ts_setup_transport_stream(w,&m)) { printf("setup fail\n"); return 1; }
    if(ts_setup_mpegvideo_stream(w,0x100,LIBMPEGTS_MPEG2_LEVEL_MAIN,LIBMPEGTS_MPEG2_PROFILE_MAIN,2000000,200000,3)){printf("setup v1 fail\n");return 1;}
    if(ts_setup_mpegvideo_stream(w,0x200,30,AVC_HIGH,2000000,200000,1)){printf("setup v2 fail\n");return 1;}

    enum { NF=24 };
    static ts_frame_t frames[NF];
    static uint8_t bodies[NF][700];
    int idx=0;
    for(int i=0;i<8;i++){
        /* p1 video window base i*3600 */
        for(size_t j=0;j<640;j++) bodies[idx][j]=(uint8_t)(j*17+i*31);
        frames[idx]=(ts_frame_t){0};
        frames[idx].pid=0x100; frames[idx].data=bodies[idx]; frames[idx].size=640;
        frames[idx].dts=9000+i*3600; frames[idx].pts=frames[idx].dts;
        frames[idx].cpb_initial_arrival_time=(int64_t)(i*3600+900)*300;
        frames[idx].cpb_final_arrival_time=(int64_t)(i*3600+3600)*300;
        frames[idx].random_access=i==0;
        idx++;
        /* p1 audio */
        memset(bodies[idx],0xCD,200);
        frames[idx]=(ts_frame_t){0};
        frames[idx].pid=0x101; frames[idx].data=bodies[idx]; frames[idx].size=200;
        frames[idx].pts=frames[idx].dts=9000+i*3600+1800;
        idx++;
        /* p2 video overlaps: same windows shifted but distinct pattern */
        for(size_t j=0;j<500;j++) bodies[idx][j]=(uint8_t)(j*31+i*17);
        frames[idx]=(ts_frame_t){0};
        frames[idx].pid=0x200; frames[idx].data=bodies[idx]; frames[idx].size=500;
        frames[idx].dts=9000+i*3600+1800; frames[idx].pts=frames[idx].dts;
        frames[idx].cpb_initial_arrival_time=(int64_t)(i*3600+1200)*300;
        frames[idx].cpb_final_arrival_time=(int64_t)(i*3600+3600)*300;
        frames[idx].random_access=i==0;
        idx++;
    }
    collect(w,frames,NF);
    collect(w,NULL,0);
    if(ts_close_writer(w)){printf("close fail\n");return 1;}
    printf("packets=%zu bytes=%zu\n",packets,length);
    FILE *ff=fopen(".task-build/scratch.ts","wb"); fwrite(trace,1,length,ff); fclose(ff);
    free(trace); free(ticks);
    return 0;
}
