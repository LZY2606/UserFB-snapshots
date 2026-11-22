#include "matroska/KaxSegment.h"
#include "matroska/KaxTracks.h"
#include "matroska/KaxCluster.h"
#include "matroska/KaxCues.h"
#include "matroska/KaxCuesData.h"
#include "matroska/KaxBlockData.h"
#include "matroska/KaxSemantic.h"
#include "ebml/MemIOCallback.h"
#include "ebml/EbmlStream.h"
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace libebml;
using namespace libmatroska;
static void check(bool v, const char *m) {if(!v) throw std::runtime_error(m);}
static uint64 value(const EbmlElement *e) {return e ? uint64(*static_cast<const EbmlUInteger *>(e)) : 0;}
static const KaxCueTrackPositions *positions(const KaxCuePoint &p) {return static_cast<const KaxCueTrackPositions *>(p.FindFirstElt(EBML_INFO(KaxCueTrackPositions)));}
static uint64 stateValue(const KaxCueTrackPositions &p) {
 uint64 result=0;unsigned nonzero=0;
 for(auto *e:p)if(EbmlId(*e)==EBML_ID(KaxCueCodecState)) {const auto v=value(e);if(v){result=v;++nonzero;}}
 check(nonzero<=1,"duplicate nondefault state");return result;
}
int main() {try {
 const uint64 scale=1000000;
 MemIOCallback output; KaxSegment segment;segment.WriteHead(output,5,false);
 KaxTrackEntry track;track.SetGlobalTimecodeScale(scale);GetChild<KaxTrackNumber>(track).SetValue(1);track.EnableLacing(false);
 KaxCluster cluster;cluster.SetParent(segment);cluster.InitTimecode(0,scale);
 binary bytesA[]={0x11,0x22,0x33},bytesB[]={0x44,0x55,0x66};
 KaxBlockGroup *a=nullptr,*b=nullptr;
 cluster.AddFrame(track,0,*new DataBuffer(bytesA,sizeof(bytesA)),a,LACING_NONE);
 cluster.AddFrame(track,10*scale,*new DataBuffer(bytesB,sizeof(bytesB)),b,LACING_NONE);
 check(a&&b&&a!=b,"groups");
 binary stateBytes[]={0xa1,0xb2,0xc3,0xd4};auto &state=GetChild<KaxCodecState>(*a);state.CopyBuffer(stateBytes,sizeof(stateBytes));
 KaxBlockBlob refA(BLOCK_BLOB_NO_SIMPLE),refB(BLOCK_BLOB_NO_SIMPLE);refA.SetBlockGroup(*a);refB.SetBlockGroup(*b);
 // The wrappers own their groups; detach them from the cluster on every exit.
 struct Detach {KaxCluster &c;~Detach(){for(size_t i=c.ListSize();i--;)if(EbmlId(*c[i])==EBML_ID(KaxBlockGroup))c.Remove(i);}} detach{cluster};
 KaxCues cues;cues.SetGlobalTimecodeScale(scale);check(cues.AddBlockBlob(refA)&&cues.AddBlockBlob(refB),"register");
 const auto size=cluster.Render(output,cues,false);check(size>0&&cues.ListSize()==2,"render");
 const uint64 anchor=segment.GetRelativePosition(state.GetElementPosition());check(anchor>0,"anchor");
 const auto *pa=positions(*static_cast<KaxCuePoint *>(cues[0]));const auto *pb=positions(*static_cast<KaxCuePoint *>(cues[1]));check(pa&&pb,"positions");
 const uint64 own=stateValue(*pa);const uint64 carried=stateValue(*pb);
 check(own==anchor,"own state address");
 MemIOCallback encoded;cues.Render(encoded,false);encoded.setFilePointer(0);EbmlStream stream(encoded);
 std::unique_ptr<EbmlElement> element(stream.FindNextID(EBML_INFO(KaxCues),encoded.GetDataBufferSize()));check(bool(element),"read cues");
 auto *parsed=dynamic_cast<KaxCues *>(element.get());check(parsed!=nullptr,"cue type");int upper=0;EbmlElement *found=nullptr;parsed->Read(stream,KaxCues::ClassInfos.Context,upper,found,true);check(parsed->ListSize()==2,"roundtrip count");
 const auto *rp=positions(*static_cast<KaxCuePoint *>((*parsed)[0]));check(rp&&stateValue(*rp)==anchor,"read state");
 std::cout<<"CLUSTER_RENDER blocks=2 cues=2 own_state_verified=1 bytes="<<size<<"\n";
 std::cout<<"STATE_INHERITANCE anchor="<<anchor<<" carried="<<carried<<" present="<<(carried==anchor)<<"\n";
 std::cout<<"CUES_READBACK entries=2 state_address_verified=1\n";
 return 0;
 }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}}
