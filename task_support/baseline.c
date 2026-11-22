#include "lsmash.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"baseline line %d: %s\n",__LINE__,#x); return 1; } } while(0)
struct sink { FILE *fp; unsigned calls; uint64_t bytes; };
static int emit(void *opaque,uint8_t *buf,int size) {
    struct sink *s=opaque; ++s->calls;
    size_t n=fwrite(buf,1,(size_t)size,s->fp); s->bytes+=n;
    return n==(size_t)size ? size : -1;
}
static void params(lsmash_file_parameters_t *p,struct sink *s,int index) {
    memset(p,0,sizeof(*p));
    p->mode=LSMASH_FILE_MODE_WRITE|LSMASH_FILE_MODE_BOX|LSMASH_FILE_MODE_FRAGMENTED|
            LSMASH_FILE_MODE_INITIALIZATION|LSMASH_FILE_MODE_MEDIA;
    if(index) p->mode|=LSMASH_FILE_MODE_INDEX;
    p->opaque=s; p->write=emit; p->seek=NULL;
    p->major_brand=ISOM_BRAND_TYPE_ISO6;
    p->max_chunk_duration=0.01; p->max_async_tolerance=2.0;
    p->max_chunk_size=1048576; p->max_read_size=1048576;
}
static uint32_t be32(const uint8_t *b) {
    return (uint32_t)b[0]<<24 | (uint32_t)b[1]<<16 | (uint32_t)b[2]<<8 | b[3];
}
int main(void) {
    const char *path=".task-build/legacy-stream.mp4";
    struct sink sink={fopen(path,"wb"),0,0}; REQUIRE(sink.fp);
    lsmash_root_t *root=lsmash_create_root(); REQUIRE(root);
    lsmash_file_parameters_t p; params(&p,&sink,1);
    REQUIRE(lsmash_set_file(root,&p)==NULL); REQUIRE(sink.calls==0);
    lsmash_destroy_root(root);
    root=lsmash_create_root(); REQUIRE(root); params(&p,&sink,0);
    REQUIRE(lsmash_set_file(root,&p));
    lsmash_movie_parameters_t mv; lsmash_initialize_movie_parameters(&mv);
    mv.timescale=1000; REQUIRE(lsmash_set_movie_parameters(root,&mv)==0);
    uint32_t track=lsmash_create_track(root,ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK); REQUIRE(track);
    lsmash_track_parameters_t tp; lsmash_initialize_track_parameters(&tp);
    tp.mode=ISOM_TRACK_ENABLED|ISOM_TRACK_IN_MOVIE|ISOM_TRACK_IN_PREVIEW;
    REQUIRE(lsmash_set_track_parameters(root,track,&tp)==0);
    lsmash_media_parameters_t mp; lsmash_initialize_media_parameters(&mp);
    mp.timescale=48000; REQUIRE(lsmash_set_media_parameters(root,track,&mp)==0);
    lsmash_audio_summary_t *a=(lsmash_audio_summary_t*)lsmash_create_summary(LSMASH_SUMMARY_TYPE_AUDIO);
    REQUIRE(a); a->sample_type=ISOM_CODEC_TYPE_MP4A_AUDIO; a->aot=MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
    a->frequency=48000; a->channels=2; a->sample_size=16; a->samples_in_frame=1024;
    a->max_au_length=4; REQUIRE(lsmash_setup_AudioSpecificConfig(a)==0);
    int entry=lsmash_add_sample_entry(root,track,a); REQUIRE(entry>0);
    lsmash_cleanup_summary((lsmash_summary_t*)a);
    REQUIRE(lsmash_create_fragment_movie(root)==0);
    for(unsigned fragment=0;fragment<2;++fragment) {
        for(unsigned i=0;i<2;++i) {
            unsigned n=fragment*2+i;
            lsmash_sample_t *sample=lsmash_create_sample(4); REQUIRE(sample);
            for(unsigned k=0;k<4;k++) sample->data[k]=(uint8_t)(n*16+k+1);
            sample->dts=sample->cts=n*1024; sample->index=(uint32_t)entry;
            sample->prop.ra_flags=ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
            REQUIRE(lsmash_append_sample(root,track,sample)==0);
        }
        REQUIRE(lsmash_flush_pooled_samples(root,track,1024)==0);
        if(fragment==0) REQUIRE(lsmash_create_fragment_movie(root)==0);
    }
    REQUIRE(lsmash_finish_movie(root,NULL)==0);
    lsmash_destroy_root(root); REQUIRE(fclose(sink.fp)==0);
    FILE *in=fopen(path,"rb"); REQUIRE(in);
    unsigned moofs=0,mdats=0,sidx=0,seen=0; uint8_t header[8];
    while(fread(header,1,8,in)==8) {
        uint32_t size=be32(header); REQUIRE(size>=8);
        if(!memcmp(header+4,"moof",4)) ++moofs;
        if(!memcmp(header+4,"sidx",4)) ++sidx;
        if(!memcmp(header+4,"mdat",4)) {
            ++mdats; REQUIRE(size==16); uint8_t b[8]; REQUIRE(fread(b,1,8,in)==8);
            for(unsigned j=0;j<2;j++,seen++) for(unsigned k=0;k<4;k++) REQUIRE(b[j*4+k]==seen*16+k+1);
        } else REQUIRE(fseek(in,(long)size-8,SEEK_CUR)==0);
    }
    REQUIRE(fclose(in)==0); REQUIRE(moofs==2 && mdats==2 && sidx==0 && seen==4);
    root=lsmash_create_root(); REQUIRE(root);
    REQUIRE(lsmash_open_file(path,1,&p)==0);
    lsmash_file_t *file=lsmash_set_file(root,&p); REQUIRE(file);
    REQUIRE(lsmash_read_file(file,&p)>=0);
    REQUIRE(lsmash_construct_timeline(root,track)==0);
    REQUIRE(lsmash_get_sample_count_in_media_timeline(root,track)==4);
    for(unsigned i=0;i<4;i++) {
        lsmash_sample_t *sample=lsmash_get_sample_from_media_timeline(root,track,i+1);
        REQUIRE(sample && sample->length==4 && sample->dts==i*1024);
        for(unsigned k=0;k<4;k++) REQUIRE(sample->data[k]==i*16+k+1);
        lsmash_delete_sample(sample);
    }
    lsmash_destroy_root(root); REQUIRE(lsmash_close_file(&p)==0);
    printf("BASELINE seekless fragments=%u mdat=%u samples=%u sidx=%u payloads_and_timeline_verified\n",moofs,mdats,seen,sidx);
    puts("GAP legacy INDEX plus NULL seek rejected before writer; prefix-index mode absent");
    return 0;
}
