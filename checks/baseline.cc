#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "ozz/animation/offline/raw_track.h"
#include "ozz/animation/offline/track_builder.h"
#include "ozz/animation/runtime/track.h"
#include "ozz/animation/runtime/track_triggering_job.h"
#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/simd_quaternion.h"
static void check(bool ok) {if(!ok) std::abort();}
int main() {
 using namespace ozz::animation; using namespace ozz::math;
 offline::RawFloatTrack raw;
 for(auto key: {offline::RawFloatTrack::Keyframe{offline::RawTrackInterpolation::kStep,0,0},
 {offline::RawTrackInterpolation::kStep,.25f,1},{offline::RawTrackInterpolation::kStep,.75f,0},
 {offline::RawTrackInterpolation::kStep,1,0}}) raw.keyframes.push_back(key);
 auto track=offline::TrackBuilder()(raw); check(bool(track));
 TrackTriggeringJob job; TrackTriggeringJob::Iterator it;
 job.track=track.get();job.threshold=.5f;job.from=0;job.to=1.5f;job.iterator=&it;check(job.Run());
 int count=0;for(;it!=job.end();++it) {std::printf("BASELINE edge=%.2f rising=%d\n",it->ratio,it->rising);++count;}
 check(count==3);
 Float4x4 start=Float4x4::identity(), mid=Float4x4::Translation(simd_float4::x_axis());
 Float4x4 end=Float4x4::Translation(simd_float4::Load(2,0,0,0));
 IKTwoBoneJob ik; SimdQuaternion a,b; bool reached=false;
 ik.start_joint=&start;ik.mid_joint=&mid;ik.end_joint=&end;
 ik.target=simd_float4::Load(1,1,0,0);ik.pole_vector=simd_float4::y_axis();
 ik.start_joint_correction=&a;ik.mid_joint_correction=&b;ik.reached=&reached;check(ik.Run());
 Float4x4 result=Float4x4::FromQuaternion(a.xyzw)*mid*Float4x4::FromQuaternion(b.xyzw)*mid;
 float error=GetX(Length3(result.cols[3]-ik.target));
 check(reached && error<1e-2f);
 std::printf("BASELINE events=%d ik_reached=%d endpoint_error=%.8g\n",count,reached,error);
}
