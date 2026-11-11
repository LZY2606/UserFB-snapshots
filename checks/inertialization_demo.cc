//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) Guillaume Blanc                                              //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

// InertializationJob demo: a 5-joint chain plays 3 small clips through
// AnimationBuilder / SamplingJob / InertializationJob / LocalToModelJob.
// Two switches between non-coaxial motions are triggered, the second one
// while the first transition is still running. The end-effector model-space
// position and velocity are dumped as CSV.

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/inertialization_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/memory/unique_ptr.h"

namespace {

const int kNumJoints = 5;
const int kNumSoa = (kNumJoints + 3) / 4;
const float kDuration = 2.f;

void Check(bool _ok, const char* _what) {
  if (!_ok) {
    std::fprintf(stderr, "DEMO_FAILURE %s\n", _what);
    std::exit(1);
  }
}

float GetLane(const ozz::math::SimdFloat4& _v, int _lane) {
  switch (_lane) {
    case 0:
      return ozz::math::GetX(_v);
    case 1:
      return ozz::math::GetY(_v);
    case 2:
      return ozz::math::GetZ(_v);
    default:
      return ozz::math::GetW(_v);
  }
}

void SetLane(ozz::math::SimdFloat4& _v, int _lane, float _f) {
  float f[4] = {ozz::math::GetX(_v), ozz::math::GetY(_v), ozz::math::GetZ(_v),
                ozz::math::GetW(_v)};
  f[_lane] = _f;
  _v = ozz::math::simd_float4::Load(f[0], f[1], f[2], f[3]);
}

// Shortest rotation vector (length is the full rotation angle) of a unit
// quaternion. Used to finite-difference sampled rotations into an angular
// velocity.
ozz::math::Float3 LogQuat(ozz::math::Quaternion _q) {
  using ozz::math::Float3;
  if (_q.w < 0.f) {
    _q = -_q;
  }
  const float s2 = _q.x * _q.x + _q.y * _q.y + _q.z * _q.z;
  if (s2 < 1e-16f) {
    return Float3(2.f * _q.x, 2.f * _q.y, 2.f * _q.z);
  }
  const float s = std::sqrt(s2);
  const float scale = 2.f * std::atan2(s, _q.w) / s;
  return Float3(_q.x * scale, _q.y * scale, _q.z * scale);
}

// Builds a small clip: the root translates and rotates (axis, angle), the
// children rotate around another axis, the root scales a bit.
ozz::unique_ptr<ozz::animation::Animation> BuildClip(
    const ozz::math::Float3& _root_t0, const ozz::math::Float3& _root_t1,
    const ozz::math::Float3& _root_axis, float _root_angle,
    const ozz::math::Float3& _child_axis, float _child_angle,
    float _root_scale1) {
  using namespace ozz::animation::offline;
  using namespace ozz::math;
  RawAnimation raw;
  raw.duration = kDuration;
  raw.tracks.resize(kNumJoints);
  for (int j = 0; j < kNumJoints; ++j) {
    RawAnimation::JointTrack& t = raw.tracks[j];
    if (j == 0) {
      t.translations.push_back({0.f, _root_t0});
      t.translations.push_back({kDuration, _root_t1});
      t.rotations.push_back({0.f, Quaternion::identity()});
      t.rotations.push_back(
          {kDuration, Quaternion::FromAxisAngle(_root_axis, _root_angle)});
      t.scales.push_back({0.f, Float3::one()});
      t.scales.push_back(
          {kDuration, Float3(_root_scale1, _root_scale1, _root_scale1)});
    } else {
      t.translations.push_back({0.f, Float3(0.f, 1.f, 0.f)});
      t.rotations.push_back({0.f, Quaternion::identity()});
      t.rotations.push_back(
          {kDuration, Quaternion::FromAxisAngle(_child_axis, _child_angle)});
      t.scales.push_back({0.f, Float3::one()});
    }
  }
  Check(raw.Validate(), "raw clip");
  return AnimationBuilder()(raw);
}

// Samples a clip at time _t into _pose (SoA, kNumSoa elements).
void Sample(const ozz::animation::Animation& _clip,
            ozz::animation::SamplingJob::Context* _context, float _t,
            ozz::math::SoaTransform* _pose) {
  ozz::animation::SamplingJob job;
  job.ratio = _t / kDuration;
  job.animation = &_clip;
  job.context = _context;
  job.output = {_pose, kNumSoa};
  Check(job.Run(), "sampling");
}

// Fills the inertialization target (pose and velocity) from a clip at time
// _t. The target velocity is computed by finite-differencing two samples.
void ComputeTarget(const ozz::animation::Animation& _clip,
                   ozz::animation::SamplingJob::Context* _context, float _t,
                   ozz::math::SoaTransform* _pose,
                   ozz::animation::InertializationJob::Velocity* _velocity) {
  using ozz::math::Float3;
  using ozz::math::Quaternion;
  const float h = 1e-3f;
  const float t1 = _t + h < kDuration ? _t + h : kDuration;
  ozz::math::SoaTransform next[kNumSoa];
  Sample(_clip, _context, _t, _pose);
  Sample(_clip, _context, t1, next);
  const float inv_h = 1.f / (t1 - _t);
  for (int e = 0; e < kNumSoa; ++e) {
    for (int l = 0; l < 4; ++l) {
      const Float3 p0(GetLane(_pose[e].translation.x, l),
                      GetLane(_pose[e].translation.y, l),
                      GetLane(_pose[e].translation.z, l));
      const Float3 p1(GetLane(next[e].translation.x, l),
                      GetLane(next[e].translation.y, l),
                      GetLane(next[e].translation.z, l));
      const Float3 vt = (p1 - p0) * inv_h;
      SetLane(_velocity[e].translation.x, l, vt.x);
      SetLane(_velocity[e].translation.y, l, vt.y);
      SetLane(_velocity[e].translation.z, l, vt.z);
      const Quaternion q0(GetLane(_pose[e].rotation.x, l),
                          GetLane(_pose[e].rotation.y, l),
                          GetLane(_pose[e].rotation.z, l),
                          GetLane(_pose[e].rotation.w, l));
      const Quaternion q1(GetLane(next[e].rotation.x, l),
                          GetLane(next[e].rotation.y, l),
                          GetLane(next[e].rotation.z, l),
                          GetLane(next[e].rotation.w, l));
      const Float3 va =
          LogQuat(ozz::math::Normalize(q1 * ozz::math::Conjugate(q0))) * inv_h;
      SetLane(_velocity[e].angular.x, l, va.x);
      SetLane(_velocity[e].angular.y, l, va.y);
      SetLane(_velocity[e].angular.z, l, va.z);
      const Float3 s0(GetLane(_pose[e].scale.x, l), GetLane(_pose[e].scale.y, l),
                      GetLane(_pose[e].scale.z, l));
      const Float3 s1(GetLane(next[e].scale.x, l), GetLane(next[e].scale.y, l),
                      GetLane(next[e].scale.z, l));
      const Float3 vls(std::log(s1.x / s0.x) * inv_h,
                       std::log(s1.y / s0.y) * inv_h,
                       std::log(s1.z / s0.z) * inv_h);
      SetLane(_velocity[e].log_scale.x, l, vls.x);
      SetLane(_velocity[e].log_scale.y, l, vls.y);
      SetLane(_velocity[e].log_scale.z, l, vls.z);
    }
  }
}

}  // namespace

int main() {
  using namespace ozz::animation;
  using namespace ozz::animation::offline;
  using namespace ozz::math;

  // 5-joint parent-child chain.
  RawSkeleton raw_skeleton;
  raw_skeleton.roots.resize(1);
  RawSkeleton::Joint* joint = &raw_skeleton.roots[0];
  for (int i = 0; i < kNumJoints; ++i) {
    joint->name = "joint";
    joint->transform = Transform::identity();
    joint->transform.translation = i ? Float3(0.f, 1.f, 0.f) : Float3::zero();
    if (i != kNumJoints - 1) {
      joint->children.resize(1);
      joint = &joint->children[0];
    }
  }
  ozz::unique_ptr<Skeleton> skeleton = SkeletonBuilder()(raw_skeleton);
  Check(skeleton && skeleton->num_joints() == kNumJoints, "skeleton");

  // 3 clips with non-coaxial motions (X, Y and (1,1,0) rotation axes).
  ozz::unique_ptr<Animation> clip_a =
      BuildClip(Float3(0.f, 0.f, 0.f), Float3(3.f, 0.f, 0.f), Float3::x_axis(),
                1.2f, Float3::z_axis(), .4f, 1.15f);
  ozz::unique_ptr<Animation> clip_b =
      BuildClip(Float3(1.f, 0.f, 2.f), Float3(-1.f, 0.f, 3.f),
                Float3::y_axis(), 1.f, Float3::x_axis(), -.3f, .95f);
  ozz::unique_ptr<Animation> clip_c = BuildClip(
      Float3(0.f, 1.f, 1.f), Float3(2.f, 1.5f, 0.f),
      ozz::math::Normalize(Float3(1.f, 1.f, 0.f)), .9f, Float3::y_axis(), .35f,
      1.1f);
  Check(clip_a && clip_b && clip_c, "clips");

  SamplingJob::Context context_a(kNumJoints), context_b(kNumJoints),
      context_c(kNumJoints);

  InertializationJob::Context inertial_context;
  inertial_context.Resize(kNumJoints);

  ozz::math::SoaTransform target_pose[kNumSoa];
  InertializationJob::Velocity target_velocity[kNumSoa];
  ozz::math::SoaTransform output_pose[kNumSoa];
  InertializationJob::Velocity output_velocity[kNumSoa];
  Float4x4 models[kNumJoints];

  InertializationJob job;
  job.context = &inertial_context;
  job.target = {target_pose, kNumSoa};
  job.target_velocity = {target_velocity, kNumSoa};
  job.output = {output_pose, kNumSoa};
  job.output_velocity = {output_velocity, kNumSoa};

  const Animation* current = clip_a.get();
  SamplingJob::Context* current_context = &context_a;

  // Frame-based timeline: switch A -> B at frame 30 (.5s, transition of
  // .4s), switch B -> C at frame 42 (.7s, transition of .3s, while the
  // first transition is still running), end at frame 78 (1.3s).
  const int kSwitchFrame1 = 30;
  const int kSwitchFrame2 = 42;
  const int kEndFrame = 78;
  float prev_tip[3] = {0.f, 0.f, 0.f};
  bool have_prev_tip = false;
  bool checked_convergence = false;

  std::printf("time,tip_x,tip_y,tip_z,tip_vx,tip_vy,tip_vz\n");
  for (int frame = 0; frame <= kEndFrame; ++frame) {
    const float time = frame / 60.f;
    const float dt = frame == 0 ? 0.f : 1.f / 60.f;

    if (frame == kSwitchFrame1 || frame == kSwitchFrame2) {
      // To switch at this time, the old clip is first advanced up to it...
      ComputeTarget(*current, current_context, time, target_pose,
                    target_velocity);
      job.delta_time = dt;
      job.transition_duration = 0.f;
      Check(job.Run(), "advance before switch");
      // ...then the switch to the new target of that same time is
      // requested. This instantaneous call (delta_time = 0,
      // transition_duration > 0) still delivers the pre-switch pose.
      current = frame == kSwitchFrame1 ? clip_b.get() : clip_c.get();
      current_context = frame == kSwitchFrame1 ? &context_b : &context_c;
      ComputeTarget(*current, current_context, time, target_pose,
                    target_velocity);
      job.delta_time = 0.f;
      job.transition_duration = frame == kSwitchFrame1 ? .4f : .3f;
      Check(job.Run(), "switch");
    } else {
      // Steady advance with the current clip.
      ComputeTarget(*current, current_context, time, target_pose,
                    target_velocity);
      job.delta_time = dt;
      job.transition_duration = 0.f;
      Check(job.Run(), "advance");
    }

    // Local to model space, end-effector (last joint) position.
    LocalToModelJob ltm;
    ltm.skeleton = skeleton.get();
    ltm.input = {output_pose, kNumSoa};
    ltm.output = {models, kNumJoints};
    Check(ltm.Run(), "local to model");
    const float tip[3] = {GetX(models[kNumJoints - 1].cols[3]),
                          GetY(models[kNumJoints - 1].cols[3]),
                          GetZ(models[kNumJoints - 1].cols[3])};
    Check(std::isfinite(tip[0]) && std::isfinite(tip[1]) &&
              std::isfinite(tip[2]),
          "finite tip");

    float velocity[3] = {0.f, 0.f, 0.f};
    if (have_prev_tip) {
      float jump2 = 0.f;
      for (int k = 0; k < 3; ++k) {
        velocity[k] = (tip[k] - prev_tip[k]) / dt;
        Check(std::isfinite(velocity[k]), "finite tip velocity");
        const float d = tip[k] - prev_tip[k];
        jump2 += d * d;
      }
      // The inertial transition is continuous: no position jump.
      Check(jump2 < .5f * .5f, "continuous tip");
    }
    std::printf("%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", time, tip[0], tip[1],
                tip[2], velocity[0], velocity[1], velocity[2]);
    prev_tip[0] = tip[0];
    prev_tip[1] = tip[1];
    prev_tip[2] = tip[2];
    have_prev_tip = true;

    // Once the last transition is over (at 1.0s), the output must be the
    // clip C pose, verbatim.
    if (time >= 1.f && !checked_convergence) {
      ozz::math::SoaTransform expected[kNumSoa];
      Sample(*clip_c, &context_c, time, expected);
      for (int e = 0; e < kNumSoa; ++e) {
        const int valid = e == kNumSoa - 1 ? kNumJoints - 4 : 4;
        for (int l = 0; l < valid; ++l) {
          const ozz::math::SimdFloat4* out_components =
              &output_pose[e].translation.x;
          const ozz::math::SimdFloat4* expected_components =
              &expected[e].translation.x;
          for (int c = 0; c < 10; ++c) {
            Check(std::fabs(GetLane(out_components[c], l) -
                            GetLane(expected_components[c], l)) < 1e-5f,
                  "converged on clip C");
          }
        }
      }
      checked_convergence = true;
    }
  }
  Check(checked_convergence, "convergence checked");
  std::printf("INERTIALIZATION_DEMO_OK\n");
  return 0;
}
