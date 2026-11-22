#include <opentimelineio/clip.h>
#include <opentimelineio/track.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/stackAlgorithm.h>
#include <iostream>
namespace o=opentimelineio::OPENTIMELINEIO_VERSION_NS;
using o::RationalTime;using o::TimeRange;
#define CHECK(x) do{if(!(x)){std::cerr<<"BASELINE_FAIL line="<<__LINE__<<"\n";return 1;}}while(0)
int main(){
 o::ErrorStatus e;
 auto range=[](double a,double n){return TimeRange(RationalTime(a,24),RationalTime(n,24));};
 o::SerializableObject::Retainer<o::Stack> scene(new o::Stack("scene"));
 auto bottom=new o::Track("bottom");bottom->append_child(new o::Clip("B",nullptr,range(100,20)),&e);
 auto nest=new o::Stack("nested",range(2,12));
 auto inside=new o::Track("inside");inside->append_child(new o::Gap(RationalTime(4,24)),&e);inside->append_child(new o::Clip("A",nullptr,range(200,8)),&e);inside->append_child(new o::Gap(RationalTime(8,24)),&e);nest->append_child(inside,&e);
 auto top=new o::Track("top");top->append_child(nest,&e);scene->append_child(bottom,&e);scene->append_child(top,&e);CHECK(!o::is_error(e));
 o::SerializableObject::Retainer<o::Track> flat(o::flatten_stack(scene,&e));CHECK(flat&&!o::is_error(e));CHECK(flat->duration(&e)==RationalTime(20,24));CHECK(flat->children().size()==2);
 auto nested=dynamic_cast<o::Stack*>(flat->children()[0].value);CHECK(nested);CHECK(nested->duration(&e)==RationalTime(12,24));
 auto tail=dynamic_cast<o::Clip*>(flat->children()[1].value);CHECK(tail);CHECK(tail->name()=="B");CHECK(tail->source_range()->start_time()==RationalTime(112,24));CHECK(tail->duration(&e)==RationalTime(8,24));
 auto json=flat->to_json_string(&e);CHECK(!o::is_error(e));o::SerializableObject::Retainer<o::SerializableObject> back(o::SerializableObject::from_json_string(json,&e));CHECK(back&&!o::is_error(e));
 std::cout<<"OTIO_NESTED_BASELINE_OK duration=20 children=2 first=Stack:12 tail=B:112..120 json=ok\n";
}
