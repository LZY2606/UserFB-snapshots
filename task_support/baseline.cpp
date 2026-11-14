#include "historical-fk.cpp"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/blending_job.h"
int main(){
 if(historical_fk())return 1;
 using namespace ozz;using namespace ozz::animation;using namespace ozz::math;
 offline::RawSkeleton skraw;skraw.roots.resize(1);skraw.roots[0].children.resize(1);skraw.roots[0].children[0].transform.translation=Float3(0,2,0);
 auto sk=offline::SkeletonBuilder()(skraw);if(!sk)return 2;
 offline::RawAnimation raw[2];
 for(int i=0;i<2;++i){raw[i].duration=float(i+1);raw[i].tracks.resize(2);raw[i].tracks[0].translations.push_back({0.f,Float3(0,0,0)});raw[i].tracks[0].translations.push_back({raw[i].duration,Float3(float(4*(i+1)),0,0)});raw[i].tracks[1].translations.push_back({0.f,Float3(0,2,0)});}
 auto a=offline::AnimationBuilder()(raw[0]);auto b=offline::AnimationBuilder()(raw[1]);if(!a||!b)return 3;
 SamplingJob::Context ca(2),cb(2);SoaTransform pa[1],pb[1],out[1];SamplingJob sample;sample.ratio=.5f;sample.animation=a.get();sample.context=&ca;sample.output=make_span(pa);if(!sample.Run())return 4;
 sample.animation=b.get();sample.context=&cb;sample.output=make_span(pb);if(!sample.Run())return 5;
 BlendingJob::Layer layers[2];layers[0].weight=.25f;layers[0].transform=make_span(pa);layers[1].weight=.75f;layers[1].transform=make_span(pb);
 BlendingJob blend;blend.layers=make_span(layers);blend.rest_pose=sk->joint_rest_poses();blend.output=make_span(out);if(!blend.Run())return 6;
 Float4x4 model[2];LocalToModelJob fk;fk.skeleton=sk.get();fk.input=make_span(out);fk.output=make_span(model);if(!fk.Run())return 7;
 if(std::abs(GetX(model[1].cols[3])-3.5f)>1e-3f||std::abs(GetY(model[1].cols[3])-2)>1e-5f)return 8;
 std::puts("SAMPLING_BLEND_FK_OK endpoint=3.5,2,0 contexts=independent");
}
