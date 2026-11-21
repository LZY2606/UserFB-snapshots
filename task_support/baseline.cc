#include "mkvmuxer/mkvmuxer.h"
#include "mkvmuxer/mkvwriter.h"
#include "mkvparser/mkvparser.h"
#include "mkvparser/mkvreader.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>
#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"baseline line %d: %s\n",__LINE__,#x); return false; } } while(0)
static bool produce(const char* path,bool live) {
  mkvmuxer::MkvWriter writer; REQUIRE(writer.Open(path));
  mkvmuxer::Segment segment; REQUIRE(segment.Init(&writer));
  segment.set_mode(live?mkvmuxer::Segment::kLive:mkvmuxer::Segment::kFile);
  segment.GetSegmentInfo()->set_timecode_scale(100000);
  const uint64_t video=segment.AddVideoTrack(16,16,1);
  const uint64_t audio=segment.AddAudioTrack(48000,2,2);
  REQUIRE(video==1 && audio==2);
  segment.GetTrackByNumber(video)->set_codec_id("V_VP8");
  segment.GetTrackByNumber(audio)->set_codec_id("A_OPUS");
  const unsigned char head[19]={'O','p','u','s','H','e','a','d',1,2,0,0,0x80,0xbb,0,0,0,0,0};
  REQUIRE(segment.GetTrackByNumber(audio)->SetCodecPrivate(head,sizeof(head)));
  const unsigned char frames[4][3]={{11,12,13},{21,22,23},{31,32,33},{41,42,43}};
  REQUIRE(segment.AddFrame(frames[0],3,video,0,true));
  REQUIRE(segment.AddFrame(frames[1],3,audio,1000000,true));
  segment.ForceNewClusterOnNextFrame();
  REQUIRE(segment.AddFrame(frames[2],3,video,10000000,true));
  REQUIRE(segment.AddFrame(frames[3],3,audio,11000000,true));
  REQUIRE(segment.Finalize());writer.Close();return true;
}
static bool inspect(const char* path,int& clusters,int& frames) {
  mkvparser::MkvReader reader; REQUIRE(reader.Open(path)==0);
  long long pos=0; mkvparser::EBMLHeader header;REQUIRE(header.Parse(&reader,pos)>=0);
  mkvparser::Segment* raw=NULL;REQUIRE(mkvparser::Segment::CreateInstance(&reader,pos,raw)==0);
  std::unique_ptr<mkvparser::Segment> segment(raw);REQUIRE(segment->Load()>=0);
  const long long stamps[4]={0,1000000,10000000,11000000};
  const unsigned char first[4]={11,21,31,41};clusters=0;frames=0;
  for(const mkvparser::Cluster* cluster=segment->GetFirst();cluster && !cluster->EOS();cluster=segment->GetNext(cluster)) {
    ++clusters;const mkvparser::BlockEntry* entry=NULL;REQUIRE(cluster->GetFirst(entry)>=0);
    while(entry && !entry->EOS()) {
      const mkvparser::Block* block=entry->GetBlock();REQUIRE(block && block->GetFrameCount()==1);
      REQUIRE(frames<4 && block->GetTime(cluster)==stamps[frames]);
      unsigned char bytes[3];REQUIRE(block->GetFrame(0).len==3);
      REQUIRE(block->GetFrame(0).Read(&reader,bytes)==0);
      REQUIRE(bytes[0]==first[frames] && bytes[1]==first[frames]+1 && bytes[2]==first[frames]+2);
      ++frames;const mkvparser::BlockEntry* next=NULL;REQUIRE(cluster->GetNext(entry,next)>=0);entry=next;
    }
  }
  REQUIRE(clusters==2 && frames==4);return true;
}
static uint32_t crc(const std::vector<unsigned char>& bytes,size_t begin,size_t end) {
  uint32_t state=0xffffffffU;
  for(size_t i=begin;i<end;++i) { state^=bytes[i]; for(int b=0;b<8;++b) state=(state>>1)^((state&1)?0xedb88320U:0); }
  return state^0xffffffffU;
}
static bool insert_wrong_crc() {
  std::ifstream in(".task-build/live.webm",std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
  const unsigned char id[4]={0x1f,0x43,0xb6,0x75};
  std::vector<unsigned char>::iterator first=std::search(bytes.begin(),bytes.end(),id,id+4);
  REQUIRE(first!=bytes.end());size_t start=static_cast<size_t>(first-bytes.begin());
  REQUIRE(start+12<bytes.size() && bytes[start+4]==1);
  std::vector<unsigned char>::iterator next=std::search(bytes.begin()+start+12,bytes.end(),id,id+4);
  REQUIRE(next!=bytes.end());start=static_cast<size_t>(next-bytes.begin());
  const size_t stop=bytes.size();
  REQUIRE(start+12<stop && bytes[start+4]==1);
  for(size_t i=start+5;i<start+12;++i) REQUIRE(bytes[i]==255);
  uint32_t value=crc(bytes,start+12,stop)^1U;
  unsigned char element[6]={0xbf,0x84,static_cast<unsigned char>(value),static_cast<unsigned char>(value>>8),static_cast<unsigned char>(value>>16),static_cast<unsigned char>(value>>24)};
  bytes.insert(bytes.begin()+start+12,element,element+6);
  std::ofstream out(".task-build/wrong-crc.webm",std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));out.close();REQUIRE(out.good());
  int clusters=0,frames=0;REQUIRE(inspect(".task-build/wrong-crc.webm",clusters,frames));
  std::printf("WEBM_BAD_CRC_IGNORED clusters=%d frames=%d\n",clusters,frames);return true;
}
int main() {
  int clusters=0,frames=0;
  if(!produce(".task-build/normal.webm",false) || !inspect(".task-build/normal.webm",clusters,frames))return 1;
  std::printf("WEBM_ROUNDTRIP clusters=%d frames=%d\n",clusters,frames);
  if(!produce(".task-build/live.webm",true) || !insert_wrong_crc())return 1;
  return 0;
}
