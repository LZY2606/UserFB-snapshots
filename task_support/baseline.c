#include "dr_wav.h"
#include <stdio.h>
#include <string.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"baseline failed at %d: %s\n",__LINE__,#x); return 1; } } while (0)
static size_t put(void *u, const void *p, size_t n) { return fwrite(p,1,n,(FILE*)u); }
static drwav_bool32 seek_out(void *u, int n, drwav_seek_origin o) {
    return fseek((FILE*)u,n,o==DRWAV_SEEK_SET?SEEK_SET:SEEK_CUR)==0;
}
int main(void) {
    const unsigned char pcm[]={0x34,0x12,0xfe,0xff,0,0x80};
    unsigned char got[6]={0},umid[64]={0};
    drwav w,r; drwav_metadata m[2]; drwav_cue_point cp;
    drwav_data_format f={drwav_container_riff,DR_WAVE_FORMAT_PCM,1,48000,16};
    FILE *out=fopen(".task-build/base.wav","wb+");
    unsigned i,found=0;
    REQUIRE(out!=NULL);
    memset(m,0,sizeof(m)); memset(&cp,0,sizeof(cp));
    cp.id=7; cp.sampleOffset=1; memcpy(cp.dataChunkId,"data",4);
    m[0].type=drwav_metadata_type_bext;
    m[0].data.bext.timeReference=((drwav_uint64)1<<40)+123;
    m[0].data.bext.pDescription="three frame baseline";
    m[0].data.bext.pOriginatorName="";
    m[0].data.bext.pOriginatorReference="";
    m[0].data.bext.pUMID=umid;
    m[1].type=drwav_metadata_type_cue;
    m[1].data.cue.cuePointCount=1; m[1].data.cue.pCuePoints=&cp;
    REQUIRE(drwav_init_write_with_metadata(&w,&f,put,seek_out,out,NULL,m,2));
    REQUIRE(drwav_write_pcm_frames_le(&w,3,pcm)==3);
    REQUIRE(drwav_uninit(&w)==DRWAV_SUCCESS); REQUIRE(fclose(out)==0);
    REQUIRE(drwav_init_file_with_metadata(&r,".task-build/base.wav",0,NULL));
    REQUIRE(r.totalPCMFrameCount==3 && r.channels==1 && r.bitsPerSample==16);
    for(i=0;i<r.metadataCount;i++) {
        if(r.pMetadata[i].type==drwav_metadata_type_bext) {
            REQUIRE(r.pMetadata[i].data.bext.timeReference==((drwav_uint64)1<<40)+123); found|=1;
        }
        if(r.pMetadata[i].type==drwav_metadata_type_cue) {
            REQUIRE(r.pMetadata[i].data.cue.cuePointCount==1);
            REQUIRE(r.pMetadata[i].data.cue.pCuePoints[0].id==7);
            REQUIRE(r.pMetadata[i].data.cue.pCuePoints[0].sampleOffset==1); found|=2;
        }
    }
    REQUIRE(found==3); REQUIRE(drwav_read_pcm_frames_le(&r,3,got)==3);
    REQUIRE(memcmp(pcm,got,6)==0); REQUIRE(drwav_seek_to_pcm_frame(&r,1));
    REQUIRE(drwav_read_pcm_frames_le(&r,1,got)==1); REQUIRE(memcmp(got,pcm+2,2)==0);
    REQUIRE(drwav_uninit(&r)==DRWAV_SUCCESS);
    puts("BASELINE BWF frames=3 origin=1099511627899 cue=7@1 pcm_exact=1 seek=1");
    return 0;
}
