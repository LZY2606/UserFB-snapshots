#include <mp4v2/mp4v2.h>
#include <cstdio>
int main(){
  MP4FileHandle s=MP4Read(".task-build/paired-source.mp4");
  MP4FileHandle d=MP4Create(".task-build/probe-out.mp4");
  MP4TrackId h=MP4FindTrackId(s,0,MP4_HINT_TRACK_TYPE);
  MP4TrackId m=0,o=0;
  bool ok=MP4CopyHintedTrack(s,h,d,&m,&o);
  MP4Close(d); MP4Close(s);
  MP4FileHandle v=MP4Read(".task-build/probe-out.mp4");
  MP4TrackId mm=MP4FindTrackId(v,0,MP4_VIDEO_TRACK_TYPE);
  MP4TrackId hh=MP4FindTrackId(v,0,MP4_HINT_TRACK_TYPE);
  std::printf("ok=%d mediaSamples=%u hintSamples=%u tref=%u\n",ok,
    MP4GetTrackNumberOfSamples(v,mm),MP4GetTrackNumberOfSamples(v,hh),
    MP4GetHintTrackReferenceTrackId(v,hh));
  uint16_t np=99; MP4ReadRtpHint(v,hh,1,&np);
  std::printf("hint sample1 packets=%u (expect 0)\n",np);
  MP4Close(v); return 0;
}
