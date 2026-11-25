#include <mp4v2/mp4v2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x); return 1; } } while (0)

static int create_source(const char* path) {
    std::remove(path);
    MP4FileHandle f=MP4Create(path);
    REQUIRE(f!=MP4_INVALID_FILE_HANDLE);
    MP4SetTimeScale(f,1000);
    MP4TrackId m=MP4AddVideoTrack(f,1000,100,16,16,MP4_MPEG4_VIDEO_TYPE);
    REQUIRE(m!=MP4_INVALID_TRACK_ID);
    const uint8_t config[]={0x12,0x34,0x56};
    REQUIRE(MP4SetTrackESConfiguration(f,m,config,sizeof(config)));
    for (unsigned i=0;i<4;++i) {
        uint8_t bytes[]={static_cast<uint8_t>('A'+i),static_cast<uint8_t>(i)};
        REQUIRE(MP4WriteSample(f,m,bytes,sizeof(bytes),100,0,true));
    }
    MP4TrackId h=MP4AddHintTrack(f,m);
    REQUIRE(h!=MP4_INVALID_TRACK_ID);
    uint8_t payload=96;
    REQUIRE(MP4SetHintTrackRtpPayload(f,h,"MP4V-ES",&payload,1400));
    for (unsigned i=1;i<=4;++i) {
        REQUIRE(MP4AddRtpHint(f,h));
        REQUIRE(MP4AddRtpESConfigurationPacket(f,h));
        REQUIRE(MP4AddRtpPacket(f,h,true,-3));
        const uint8_t immediate[]={'!'};
        REQUIRE(MP4AddRtpImmediateData(f,h,immediate,1));
        REQUIRE(MP4AddRtpSampleData(f,h,i==3?4:i,0,1));
        REQUIRE(MP4WriteRtpHint(f,h,100,true));
    }
    REQUIRE(MP4AddTrackEdit(f,m,MP4_INVALID_EDIT_ID,UINT64_MAX,50)!=MP4_INVALID_EDIT_ID);
    REQUIRE(MP4AddTrackEdit(f,m,MP4_INVALID_EDIT_ID,200,200)!=MP4_INVALID_EDIT_ID);
    REQUIRE(MP4AddTrackEdit(f,m,MP4_INVALID_EDIT_ID,UINT64_MAX,50)!=MP4_INVALID_EDIT_ID);
    REQUIRE(MP4AddTrackEdit(f,m,MP4_INVALID_EDIT_ID,0,200)!=MP4_INVALID_EDIT_ID);
    REQUIRE(MP4AddTrackEdit(f,m,MP4_INVALID_EDIT_ID,200,200)!=MP4_INVALID_EDIT_ID);
    MP4Close(f);
    return 0;
}

static int read_payload(MP4FileHandle f, MP4TrackId h, unsigned sample, char expected) {
    uint16_t count=0;
    REQUIRE(MP4ReadRtpHint(f,h,sample,&count)); REQUIRE(count==2);
    uint8_t* p=NULL; uint32_t n=0;
    REQUIRE(MP4ReadRtpPacket(f,h,0,&p,&n,0,false,true));
    REQUIRE(n==3 && p[0]==0x12 && p[1]==0x34 && p[2]==0x56);
    MP4Free(p); p=NULL;
    REQUIRE(MP4ReadRtpPacket(f,h,1,&p,&n,0,false,true));
    REQUIRE(n==2 && p[0]=='!' && p[1]==static_cast<uint8_t>(expected));
    MP4Free(p);
    return 0;
}

int main(int argc,char** argv) {
    bool gap=argc>1 && std::strcmp(argv[1],"gap")==0;
    REQUIRE(create_source(".task-build/paired-source.mp4")==0);
    MP4FileHandle src=MP4Read(".task-build/paired-source.mp4");
    REQUIRE(src!=MP4_INVALID_FILE_HANDLE);
    MP4TrackId media=MP4FindTrackId(src,0,MP4_VIDEO_TRACK_TYPE);
    MP4TrackId hint=MP4FindTrackId(src,0,MP4_HINT_TRACK_TYPE);
    REQUIRE(read_payload(src,hint,3,'D')==0);
    REQUIRE(MP4GetTrackNumberOfEdits(src,media)==5);
    const char* out=gap?".task-build/old-reordered.mp4":".task-build/old-copy.mp4";
    std::remove(out);
    MP4FileHandle dst=MP4Create(out); REQUIRE(dst!=MP4_INVALID_FILE_HANDLE);
    MP4TrackId dm=MP4CloneTrack(src,media,dst);
    MP4TrackId dh=MP4CloneTrack(src,hint,dst,dm);
    REQUIRE(dm!=MP4_INVALID_TRACK_ID && dh!=MP4_INVALID_TRACK_ID);
    const unsigned order[]={3,4,1,2};
    for(unsigned i=0;i<4;++i) REQUIRE(MP4CopySample(src,media,gap?order[i]:i+1,dst,dm));
    // Same-order byte copying is supported; reordered hint references are not rebound.
    for(unsigned i=0;i<4;++i) REQUIRE(MP4CopySample(src,hint,i+1,dst,dh));
    MP4Close(dst); MP4Close(src);
    dst=MP4Read(out); REQUIRE(dst!=MP4_INVALID_FILE_HANDLE);
    REQUIRE(MP4GetHintTrackReferenceTrackId(dst,dh)==dm);
    // Self references remain valid here because hint sample numbers did not change.
    REQUIRE(read_payload(dst,dh,3,gap?'B':'D')==0);
    MP4Timestamp dts=MP4GetSampleTime(dst,dh,3);
    REQUIRE(dts==200);
    MP4Close(dst);
    if(gap) std::puts("GAP old_hint3_payload=!B desired=!D stale_media_sample_reference=4");
    else std::puts("NORMAL copied_media=4 copied_hints=4 embedded_config=123456 payload=!D hint3_dts=200");
    return 0;
}
