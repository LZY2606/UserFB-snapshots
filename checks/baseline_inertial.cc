// Original task fixture, MIT licensed; see ../LICENSE.md.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/blending_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/maths/quaternion.h"

static void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "BASELINE_FAILURE %s\n", what); std::exit(1); }
}
static ozz::unique_ptr<ozz::animation::Animation> Clip(bool second) {
  using namespace ozz::animation::offline;
  using namespace ozz::math;
  RawAnimation raw;
  raw.duration = 1.f;
  raw.tracks.resize(5);
  for (int j = 0; j < 5; ++j) {
    auto& t = raw.tracks[j];
    t.translations.push_back({0.f, j ? Float3(0,1,0) : Float3(second ? 8.f : 2.f,0,0)});
    t.translations.push_back({1.f, j ? Float3(0,1,0) : Float3(second ? 6.f : 4.f,0,0)});
    const Quaternion q = j ? Quaternion::identity() :
      Quaternion::FromAxisAngle(second ? Float3::y_axis() : Float3::x_axis(), second ? -.8f : .6f);
    t.rotations.push_back({0.f, q});
    t.rotations.push_back({1.f, q});
    t.scales.push_back({0.f, Float3::one()});
  }
  Check(raw.Validate(), "raw clip");
  return AnimationBuilder()(raw);
}
int main() {
  using namespace ozz::animation;
  using namespace ozz::animation::offline;
  using namespace ozz::math;
  RawSkeleton raw;
  raw.roots.resize(1);
  auto* joint = &raw.roots[0];
  for (int i = 0; i < 5; ++i) {
    joint->name = "joint";
    joint->transform = Transform::identity();
    joint->transform.translation = i ? Float3(0,1,0) : Float3::zero();
    if (i != 4) { joint->children.resize(1); joint = &joint->children[0]; }
  }
  auto skeleton = SkeletonBuilder()(raw);
  auto a = Clip(false), b = Clip(true);
  Check(skeleton && a && b, "builders");
  Check(skeleton->num_joints()==5 && skeleton->num_soa_joints()==2, "joint layout");
  SoaTransform pa[2], pb[2], mixed[2];
  SamplingJob::Context ca(5), cb(5);
  SamplingJob sample;
  sample.ratio=.25f; sample.animation=a.get(); sample.context=&ca; sample.output=pa;
  Check(sample.Run(), "sample A");
  sample.animation=b.get(); sample.context=&cb; sample.output=pb;
  Check(sample.Run(), "sample B");
  float root_x[3], tips[2][4];
  for (int i=0; i<3; ++i) {
    const float weight=i*.5f;
    BlendingJob::Layer layers[2];
    layers[0].weight=1.f-weight; layers[0].transform=pa;
    layers[1].weight=weight; layers[1].transform=pb;
    BlendingJob blend;
    blend.layers=layers; blend.rest_pose=skeleton->joint_rest_poses(); blend.output=mixed;
    Check(blend.Run(), "blend");
    root_x[i]=GetX(mixed[0].translation.x);
    Check(std::fabs(root_x[i]-(2.5f+2.5f*i))<1e-4f, "root translation");
    Float4x4 matrices[5];
    LocalToModelJob ltm; ltm.skeleton=skeleton.get(); ltm.input=mixed; ltm.output=matrices;
    Check(ltm.Run(), "local to model");
    if (i!=1) StorePtrU(matrices[4].cols[3], tips[i/2]);
  }
  float jump2=0;
  for (int k=0;k<3;++k) { const float d=tips[1][k]-tips[0][k]; jump2+=d*d; }
  Check(jump2>25.f, "hard layer switch witness");
  std::printf("POSE_BASELINE_OK joints=5 soa=2 root_x=%.3f,%.3f,%.3f\n",root_x[0],root_x[1],root_x[2]);
  std::printf("MODEL_SWITCH_WITNESS tip_jump=%.6f\n",std::sqrt(jump2));
  return 0;
}
