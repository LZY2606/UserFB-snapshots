#include "opentimelineio/track.h"
#include "opentimelineio/clip.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/transition.h"
#include "opentimelineio/trackAlgorithm.h"
#include <iostream>
namespace o=OTIO_NS;
using o::RationalTime;using o::TimeRange;
using o::SerializableObject;
int main(){
 o::ErrorStatus e;
 SerializableObject::Retainer<o::Track> t(new o::Track("edit"));
 auto media=[](const char* url){return new o::ExternalReference(url,TimeRange(RationalTime(0,24),RationalTime(500,24)));};
 auto a=new o::Clip("A",media("A.mov"),TimeRange(RationalTime(100,24),RationalTime(24,24)));
 auto b=new o::Clip("B",media("B.mov"),TimeRange(RationalTime(200,24),RationalTime(24,24)));
 t->append_child(a,&e);
 t->append_child(new o::Transition("dissolve",o::Transition::Type::SMPTE_Dissolve,RationalTime(6,24),RationalTime(6,24)),&e);
 t->append_child(b,&e);
 if(o::is_error(e))return 1;
 SerializableObject::Retainer<o::Track> normal(o::track_trimmed_to_range(t,TimeRange(RationalTime(0,24),RationalTime(48,24)),&e));
 if(o::is_error(e)||!normal||normal->duration()!=RationalTime(48,24))return 2;
 std::cout<<"BASELINE_NORMAL duration=48 children="<<normal->children().size()<<"\n";
 e=o::ErrorStatus();
 SerializableObject::Retainer<o::Track> partial(o::track_trimmed_to_range(t,TimeRange(RationalTime(21,24),RationalTime(6,24)),&e));
 if(!partial && e.outcome==o::ErrorStatus::CANNOT_TRIM_TRANSITION)
  std::cout<<"BASELINE_PARTIAL_REJECTED CANNOT_TRIM_TRANSITION\n";
 else if(partial&&!o::is_error(e))std::cout<<"BASELINE_PARTIAL_SUPPORTED duration="<<partial->duration().rescaled_to(24).value()<<"\n";
 else return 3;
 std::cout<<"BASELINE_OK\n";
}
