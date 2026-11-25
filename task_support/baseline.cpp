#include <mp4v2/mp4v2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"baseline fail line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
    const char *path=".task-build/legacy.mp4";
    MP4FileHandle f=MP4Create(path); REQUIRE(f);
    MP4TrackId t=MP4AddAudioTrack(f,1000,MP4_INVALID_DURATION);
    REQUIRE(t!=MP4_INVALID_TRACK_ID);
    const uint8_t payloads[3][4]={{1,2,3,4},{5,6,7,8},{9,10,11,12}};
    REQUIRE(MP4SetTrackDurationPerChunk(f,t,1000));
    for(unsigned i=0;i<3;i++) REQUIRE(MP4WriteSample(f,t,payloads[i],4,11,3,true));
    REQUIRE(MP4GetTrackDuration(f,t)==33);
    REQUIRE(MP4GetSampleTime(f,t,3)==22);
    REQUIRE(MP4SetTrackTimeScale(f,t,900));
    REQUIRE(MP4GetTrackTimeScale(f,t)==900);
    REQUIRE(MP4GetSampleDuration(f,t,2)==11);
    REQUIRE(MP4GetSampleRenderingOffset(f,t,2)==3);
    MP4Close(f);
    f=MP4Read(path); REQUIRE(f);
    REQUIRE(MP4GetTrackDuration(f,t)==33);
    for(unsigned i=0;i<3;i++) {
        uint8_t *p=NULL; uint32_t n=0; MP4Timestamp d=0; MP4Duration u=0,c=0;
        REQUIRE(MP4ReadSample(f,t,i+1,&p,&n,&d,&u,&c));
        bool good=n==4 && !std::memcmp(p,payloads[i],4) && d==i*11 && u==11 && c==3;
        MP4Free(p); REQUIRE(good);
    }
    MP4Close(f);
    std::puts("BASELINE legacy_setter scale=900 duration=33 dts=0,11,22 offsets=3 payloads=3 verified");
    std::puts("GAP MP4SetTrackTimeScale preserves tick values; endpoint-rescale API is absent");
    return 0;
}
