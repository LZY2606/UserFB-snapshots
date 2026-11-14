#include <opentimelineio/clip.h>
#include <opentimelineio/track.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/trackAlgorithm.h>
#include <cmath>
#include <iostream>
namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;
using o::RationalTime; using o::TimeRange;
#define CHECK(x) do { if(!(x)){std::cerr<<"baseline failure "<<__LINE__<<"\n";return 1;} } while(0)
int main(){
 o::ErrorStatus e;
 o::SerializableObject::Retainer<o::Track> track(new o::Track("edit"));
 auto* ref=new o::ExternalReference("media.mov",TimeRange(RationalTime(0,24),RationalTime(1000,24)));
 auto* clip=new o::Clip("A",ref,TimeRange(RationalTime(100,24),RationalTime(48,24)));
 clip->effects().push_back(new o::LinearTimeWarp("legacy","LinearTimeWarp",2.0));
 CHECK(track->append_child(new o::Gap(RationalTime(10,24)),&e));
 CHECK(track->append_child(clip,&e));CHECK(!o::is_error(e));
 CHECK(clip->duration(&e)==RationalTime(48,24));
 CHECK(clip->transformed_time(RationalTime(140,24),track,&e)==RationalTime(50,24));
 o::SerializableObject::Retainer<o::Track> cut(o::track_trimmed_to_range(track,TimeRange(RationalTime(14,24),RationalTime(8,24)),&e));
 CHECK(cut && !o::is_error(e));CHECK(cut->children().size()==1);
 auto* part=dynamic_cast<o::Clip*>(cut->children()[0].value);
 CHECK(part);CHECK(part->source_range()->start_time()==RationalTime(104,24));
 CHECK(part->source_range()->duration()==RationalTime(8,24));
 auto json=track->to_json_string(&e);CHECK(!o::is_error(e));
 o::SerializableObject::Retainer<o::SerializableObject> decoded(o::SerializableObject::from_json_string(json,&e));
 CHECK(decoded && !o::is_error(e));
 std::cout<<"OTIO_BASELINE_OK legacy_duration=48 source140_parent=50 crop_source=104..112 json=ok\n";
}
