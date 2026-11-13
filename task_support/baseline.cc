#include <cstdio>
#include <cstdlib>
#include <memory>
#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"
#include "mkvparser/mkvparser.h"
#include "mkvparser/mkvreader.h"
#define REQUIRE(x) do { if(!(x)) {std::fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x); std::exit(1);} } while(0)
static void put(mkvmuxer::Segment& s, unsigned track, unsigned ms, unsigned char label) {
  unsigned char bytes[]={label,0x53,0x7e};
  REQUIRE(s.AddFrame(bytes,sizeof bytes,track,ms*1000000ULL,true));
}
static void inspect(const char* name, bool queries) {
  mkvparser::MkvReader r; REQUIRE(r.Open(name)==0);
  long long pos=0; mkvparser::EBMLHeader h; REQUIRE(h.Parse(&r,pos)==0);
  mkvparser::Segment* raw=NULL; REQUIRE(mkvparser::Segment::CreateInstance(&r,pos,raw)==0);
  std::unique_ptr<mkvparser::Segment> s(raw); REQUIRE(s->Load()>=0);
  int ci=0;
  for(const mkvparser::Cluster* c=s->GetFirst();c&&!c->EOS();c=s->GetNext(c)) {
    const mkvparser::BlockEntry* e=NULL; REQUIRE(c->GetFirst(e)==0);
    int bi=0;
    while(e&&!e->EOS()) {
      ++bi; const mkvparser::Block* b=e->GetBlock(); unsigned char data[3];
      REQUIRE(b->GetFrameCount()==1); REQUIRE(b->GetFrame(0).len==3);
      REQUIRE(b->GetFrame(0).Read(&r,data)==0);
      std::printf("%s cluster=%d block=%d track=%lld time_ms=%lld label=%u\n",name,ci,bi,b->GetTrackNumber(),b->GetTime(c)/1000000,data[0]);
      REQUIRE(c->GetNext(e,e)==0);
    }
    ++ci;
  }
  const mkvparser::Cues* cues=s->GetCues(); REQUIRE(cues);
  while(!cues->DoneParsing()) REQUIRE(cues->LoadCuePoint());
  unsigned seen[4]={0};
  for(const mkvparser::CuePoint* cp=cues->GetFirst();cp;cp=cues->GetNext(cp)) {
    for(unsigned t=1;t<=3;++t) {
      const mkvparser::Track* tr=s->GetTracks()->GetTrackByNumber(t);
      if(!tr) continue;
      const mkvparser::CuePoint::TrackPosition* tp=cp->Find(tr);
      if(tp) {++seen[t];std::printf("%s cue track=%u time_ms=%lld block=%lld cluster_pos=%lld\n",name,t,cp->GetTime(s.get())/1000000,tp->m_block,tp->m_pos);}
    }
  }
  if(!queries) {
    REQUIRE(seen[1]==2&&seen[2]==0&&seen[3]==0);
    std::puts("CURRENT single-track cues: video=2 audio2=0 audio3=0");
  } else {
    const mkvparser::CuePoint* cp=NULL; const mkvparser::CuePoint::TrackPosition* tp=NULL;
    bool a=cues->Find(16000000,s->GetTracks()->GetTrackByNumber(1),cp,tp);
    bool b=cues->Find(1000000,s->GetTracks()->GetTrackByNumber(2),cp,tp);
    REQUIRE(!a&&!b);
    std::printf("CURRENT sparse per-track lookup: A@16=%d B@1=%d (both miss)\n",a,b);
  }
}
int main() {
  {
    mkvmuxer::MkvWriter w; REQUIRE(w.Open("queued.webm"));
    mkvmuxer::Segment s; REQUIRE(s.Init(&w));
    REQUIRE(s.AddVideoTrack(16,16,1)==1); REQUIRE(s.AddAudioTrack(48000,1,2)==2); REQUIRE(s.AddAudioTrack(48000,1,3)==3);
    REQUIRE(s.CuesTrack(1));
    put(s,1,0,100); put(s,2,8,208); put(s,3,9,239); put(s,1,10,110);
    put(s,2,18,218); put(s,3,19,249);
    REQUIRE(s.Finalize()); w.Close();
  }
  inspect("queued.webm",false);
  {
    mkvmuxer::MkvWriter w; REQUIRE(w.Open("sparse.webm"));
    mkvmuxer::Segment s; REQUIRE(s.Init(&w));
    REQUIRE(s.AddAudioTrack(48000,1,1)==1); REQUIRE(s.AddAudioTrack(48000,1,2)==2);
    REQUIRE(s.CuesTrack(1));
    put(s,1,0,10); put(s,2,5,25); REQUIRE(s.AddCuePoint(5000000,2));
    put(s,1,10,20); REQUIRE(s.AddCuePoint(10000000,1));
    put(s,2,15,35); REQUIRE(s.AddCuePoint(15000000,2));
    REQUIRE(s.Finalize());w.Close();
  }
  inspect("sparse.webm",true);
}
