#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/simd_math.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main(){
 ozz::animation::offline::RawAnimation raw;raw.duration=1;raw.tracks.resize(5);
 for(int i=0;i<5;++i){raw.tracks[i].translations.push_back({0,{0,0,0}});raw.tracks[i].translations.push_back({1,{float(i+1),0,0}});}
 auto animation=ozz::animation::offline::AnimationBuilder()(raw);assert(animation);
 ozz::animation::offline::RawSkeleton rs;rs.roots.resize(1);auto* joint=&rs.roots[0];
 for(int i=0;i<5;++i){joint->name="joint";joint->transform=ozz::math::Transform::identity();if(i<4){joint->children.resize(1);joint=&joint->children[0];}}
 auto skeleton=ozz::animation::offline::SkeletonBuilder()(rs);assert(skeleton);
 ozz::animation::SamplingJob::Context context(5);ozz::math::SoaTransform pose[2];
 ozz::animation::SamplingJob sampling;sampling.animation=animation.get();sampling.context=&context;sampling.output=ozz::make_span(pose);
 for(float t:{.75f,.25f,.5f}){sampling.ratio=t;assert(sampling.Run());}
 float x0[4],x1[4];ozz::math::StorePtrU(pose[0].translation.x,x0);ozz::math::StorePtrU(pose[1].translation.x,x1);
 assert(std::fabs(x0[0]-.5f)<1e-5 && std::fabs(x1[0]-2.5f)<1e-5);
 ozz::math::Float4x4 model[5];ozz::animation::LocalToModelJob ltm;ltm.skeleton=skeleton.get();ltm.input=ozz::make_span(pose);ltm.output=ozz::make_span(model);assert(ltm.Run());
 float endpoint[4];ozz::math::StorePtrU(model[4].cols[3],endpoint);assert(std::fabs(endpoint[0]-7.5f)<1e-5);
 std::cout<<"OZZ_SAMPLE_CURRENT_OK t0="<<x0[0]<<" t4="<<x1[0]<<"\nOZZ_MODEL_CHAIN_OK endpoint="<<endpoint[0]<<"\n";
}
