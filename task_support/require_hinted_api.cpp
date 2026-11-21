#include <mp4v2/mp4v2.h>
#include <cstdio>
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"required API probe failed: %s\n",#x);return 1;}}while(0)
int main(int argc,char**argv){
 REQUIRE(argc==3);
 MP4FileHandle src=MP4Read(argv[1]);REQUIRE(src!=MP4_INVALID_FILE_HANDLE);
 MP4TrackId oldHint=MP4FindTrackId(src,0,MP4_HINT_TRACK_TYPE);REQUIRE(oldHint!=MP4_INVALID_TRACK_ID);
 MP4FileHandle dst=MP4Create(argv[2]);REQUIRE(dst!=MP4_INVALID_FILE_HANDLE);
 MP4TrackId media=MP4_INVALID_TRACK_ID,hint=MP4_INVALID_TRACK_ID;
 REQUIRE(MP4CopyHintedTrack(src,oldHint,dst,&media,&hint));
 REQUIRE(media!=MP4_INVALID_TRACK_ID&&hint!=MP4_INVALID_TRACK_ID);
 MP4Close(dst);MP4Close(src);
 dst=MP4Read(argv[2]);REQUIRE(dst!=MP4_INVALID_FILE_HANDLE);
 REQUIRE(MP4GetNumberOfTracks(dst)==2);
 REQUIRE(MP4GetHintTrackReferenceTrackId(dst,hint)==media);
 REQUIRE(MP4GetTrackNumberOfSamples(dst,media)==6);
 REQUIRE(MP4GetTrackNumberOfSamples(dst,hint)==8);
 uint16_t packets=99;REQUIRE(MP4ReadRtpHint(dst,hint,1,&packets));REQUIRE(packets==0);
 MP4Close(dst);std::puts("REQUIRED_HINTED_API_CALL_OK");return 0;
}
