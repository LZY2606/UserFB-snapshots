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

// Windowless regression for the foot_ik contact-track locking layer.
//
// It builds everything procedurally (no FBX SDK, GLFW or media assets):
// - a 7 joints two-leg skeleton (7 is intentionally not a multiple of 4),
// - a static identity animation,
// - step and linear contact FloatTracks,
// - an analytic root translation/rotation trajectory,
// - a procedural flat ground with controllable ray-miss areas.
// The same SamplingJob, two bone IK and local-to-model jobs as the
// interactive sample are run for real; assertions read the actual IK'd
// skeleton rather than contact state booleans.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

#include "contact_lock.h"

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/raw_track.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/offline/track_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/track.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/simd_quaternion.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/memory/unique_ptr.h"

using ozz::animation::Animation;
using ozz::animation::FloatTrack;
using ozz::animation::IKTwoBoneJob;
using ozz::animation::LocalToModelJob;
using ozz::animation::SamplingJob;
using ozz::animation::Skeleton;
using ozz::animation::offline::AnimationBuilder;
using ozz::animation::offline::RawAnimation;
using ozz::animation::offline::RawFloatTrack;
using ozz::animation::offline::RawSkeleton;
using ozz::animation::offline::RawTrackInterpolation;
using ozz::animation::offline::SkeletonBuilder;
using ozz::animation::offline::TrackBuilder;
using ozz::math::Float3;
using ozz::math::Float4x4;
using ozz::math::SimdFloat4;
using ozz::math::SimdQuaternion;
using ozz::math::SoaTransform;
using ozz::sample::ContactLock;

static void Check(bool _ok, const char* _format, ...) {
  if (_ok) {
    return;
  }
  va_list args;
  va_start(args, _format);
  std::vfprintf(stderr, _format, args);
  va_end(args);
  std::fputc('\n', stderr);
  std::abort();
}

static bool Near(float _a, float _b, float _eps) {
  return std::fabs(_a - _b) <= _eps;
}

static bool Near3(const Float3& _a, const Float3& _b, float _eps) {
  return Near(_a.x, _b.x, _eps) && Near(_a.y, _b.y, _eps) &&
         Near(_a.z, _b.z, _eps);
}

// Skeleton joint indices, depth-first order. 7 joints, not a multiple of 4.
enum Joint {
  kRoot = 0,
  kLeftUpLeg = 1,
  kLeftLeg = 2,
  kLeftFoot = 3,
  kRightUpLeg = 4,
  kRightLeg = 5,
  kRightFoot = 6,
  kNumJoints = 7
};

static const float kChainLength = 2.2f;     // Two 1.1 long leg bones.
static const float kHipHeight = 2.0f;       // Rest ankle touches the floor.
static const float kHipSideOffset = .15f;
static const float kKneeBend = .4582575695f;  // sqrt(1.1^2 - 1^2).
static const float kFloorY = 0.f;
static const SimdFloat4 kKneeAxis = ozz::math::simd_float4::z_axis();

// Builds the two-leg skeleton. Each IK bone (hip->knee, knee->ankle) is 1.1
// long while dropping 1.0 vertically, so the 2.2 chain has 0.2 of slack over
// the 2.0 hip-to-floor distance. Knees are bent along z in the rest pose.
static ozz::unique_ptr<Skeleton> BuildSkeleton() {
  RawSkeleton raw;

  auto make_leg = [](const char* _hip, const char* _knee,
                     const char* _ankle, float _side) {
    RawSkeleton::Joint hip;
    hip.name = _hip;
    hip.transform.translation = Float3(_side, 0.f, 0.f);

    RawSkeleton::Joint knee;
    knee.name = _knee;
    knee.transform.translation = Float3(0.f, -1.f, kKneeBend);

    RawSkeleton::Joint ankle;
    ankle.name = _ankle;
    ankle.transform.translation = Float3(0.f, -1.f, -kKneeBend);

    knee.children.push_back(ankle);
    hip.children.push_back(knee);
    return hip;
  };

  RawSkeleton::Joint root;
  root.name = "root";
  root.transform.translation = Float3(0.f, kHipHeight, 0.f);
  root.children.push_back(
      make_leg("LeftUpLeg", "LeftLeg", "LeftFoot", kHipSideOffset));
  root.children.push_back(
      make_leg("RightUpLeg", "RightLeg", "RightFoot", -kHipSideOffset));
  raw.roots.push_back(root);

  Check(raw.num_joints() == kNumJoints, "expected %d joints, got %d",
        kNumJoints, raw.num_joints());
  Check(raw.num_joints() % 4 != 0, "joint count must not be a multiple of 4");

  return SkeletonBuilder()(raw);
}

// Builds a static animation holding the rest pose at every ratio.
static ozz::unique_ptr<Animation> BuildAnimation(const Skeleton& _skeleton) {
  RawAnimation raw;
  raw.duration = 1.f;
  raw.tracks.resize(_skeleton.num_joints());

  const ozz::span<const SoaTransform> rest = _skeleton.joint_rest_poses();
  for (int j = 0; j < _skeleton.num_joints(); ++j) {
    // Unpacks the joint rest transform from its SoA slot.
    SimdFloat4 tx[4], ty[4], tz[4];
    const SoaTransform& soa = rest[j / 4];
    ozz::math::Transpose4x4(&soa.translation.x, tx);
    ozz::math::Transpose4x4(&soa.translation.y, ty);
    ozz::math::Transpose4x4(&soa.translation.z, tz);
    Float3 translation;
    ozz::math::Store3PtrU(tx[j & 3], &translation.x);
    ozz::math::Store3PtrU(ty[j & 3], &translation.y);
    ozz::math::Store3PtrU(tz[j & 3], &translation.z);

    RawAnimation::JointTrack& track = raw.tracks[j];
    for (float time : {0.f, 1.f}) {
      track.translations.push_back({time, translation});
      track.rotations.push_back(
          {time, ozz::math::Quaternion::identity()});
      track.scales.push_back({time, Float3::one()});
    }
  }

  Check(raw.Validate(), "invalid raw animation");
  return AnimationBuilder()(raw);
}

// Builds a step contact track: support on [_rising, _falling) of every loop.
static ozz::unique_ptr<FloatTrack> BuildStepTrack(float _rising,
                                                  float _falling) {
  RawFloatTrack raw;
  raw.keyframes.push_back(
      {RawTrackInterpolation::kStep, 0.f, 0.f});
  raw.keyframes.push_back(
      {RawTrackInterpolation::kStep, _rising, 1.f});
  raw.keyframes.push_back(
      {RawTrackInterpolation::kStep, _falling, 0.f});
  Check(raw.Validate(), "invalid step contact track");
  return TrackBuilder()(raw);
}

// Builds a linear contact track. Threshold .5 is crossed (rising then
// falling) at midpoints of the ramps; the triggering job must locate those
// crossings itself.
static ozz::unique_ptr<FloatTrack> BuildLinearTrack() {
  RawFloatTrack raw;
  raw.keyframes.push_back(
      {RawTrackInterpolation::kLinear, 0.f, 0.f});
  raw.keyframes.push_back(
      {RawTrackInterpolation::kLinear, .4f, 1.f});  // Crosses .5 at .2.
  raw.keyframes.push_back(
      {RawTrackInterpolation::kLinear, .8f, 0.f});  // Crosses .5 at .6.
  Check(raw.Validate(), "invalid linear contact track");
  return TrackBuilder()(raw);
}

// Records one anchor capture, so runs with different update step sizes can be
// compared event by event.
struct Capture {
  ContactLock::Leg leg;
  float ratio;
  Float3 target;
};

// Drives one ContactLock instance exactly like the interactive sample, but
// with an analytic root trajectory and a procedural ground.
class Harness {
 public:
  Harness()
      : skeleton_(BuildSkeleton()),
        animation_(BuildAnimation(*skeleton_)) {
    const int num_joints = skeleton_->num_joints();
    locals_.resize(skeleton_->num_soa_joints());
    models_.resize(num_joints);
    scratch_locals_.resize(skeleton_->num_soa_joints());
    scratch_models_.resize(num_joints);
    context_.Resize(num_joints);
    scratch_context_.Resize(num_joints);

    lock_.set_event_callback(
        [this](ContactLock::Leg _leg, float _ratio,
               ContactLock::EventSnapshot* _snapshot) {
          const bool ok = BuildEventSnapshot(_leg, _ratio, _snapshot);
          if (ok && _snapshot->hit) {
            // Logs the capture at its exact event ratio, independently of how
            // many updates later read the anchor.
            captures_.push_back({_leg, _ratio, _snapshot->ankle_target_ws});
          }
          return ok;
        });
  }

  void SetTrack(ContactLock::Leg _leg, const FloatTrack* _track) {
    lock_.SetTrack(_leg, _track);
  }

  // Analytic root trajectory: constant horizontal speed and yaw spin per
  // animation loop. Deterministic, so edge time queries are exact. The hip
  // height already lives in the skeleton root joint, so this external root
  // only carries horizontal motion and yaw.
  Float4x4 RootAt(float _ratio) const {
    const float yaw = yaw_speed_ * _ratio;
    const Float3 translation(translation_speed_ * _ratio, 0.f, 0.f);
    return Float4x4::Translation(translation) *
           Float4x4::FromEuler(Float3(yaw, 0.f, 0.f));
  }

  // Optional ground-miss window, expressed in un-wrapped ratio space.
  void SetMissWindow(ContactLock::Leg _leg, float _from, float _to) {
    miss_[_leg] = {_from, _to};
  }

  void SetMotion(float _translation_speed, float _yaw_speed) {
    translation_speed_ = _translation_speed;
    yaw_speed_ = _yaw_speed;
  }

  void Reset(float _ratio) {
    lock_.Reset();
    ratio_ = previous_ratio_ = _ratio;
    captures_.clear();
  }
  // Advances the simulation to un-wrapped ratio _to, processing the contact
  // layer, sampling the animation and running the leg IK.
  void Update(float _to) {
    previous_ratio_ = ratio_;
    ratio_ = _to;

    bool reachable[ContactLock::kLegsCount];
    for (int l = 0; l < ContactLock::kLegsCount; ++l) {
      reachable[l] = IsReachable(static_cast<ContactLock::Leg>(l));
    }
    Check(lock_.Update(previous_ratio_, ratio_, reachable),
          "contact lock update failed");

    // Samples the base animation for the current frame.
    SamplingJob sampling;
    sampling.animation = animation_.get();
    sampling.context = &context_;
    sampling.ratio = Wrap(ratio_);
    sampling.output = ozz::make_span(locals_);
    Check(sampling.Run(), "sampling job failed");

    LocalToModelJob ltm;
    ltm.skeleton = skeleton_.get();
    ltm.input = ozz::make_span(locals_);
    ltm.output = ozz::make_span(models_);
    Check(ltm.Run(), "local to model job failed");

    // Per-frame ground rays, used for unlocked legs (original behavior).
    LegRay rays[ContactLock::kLegsCount];
    for (int l = 0; l < ContactLock::kLegsCount; ++l) {
      Raycast(static_cast<ContactLock::Leg>(l), ratio_, &rays[l]);
    }

    const Float4x4 root = RootAt(ratio_);
    const Float4x4 inv_root = ozz::math::Invert(root);

    for (int l = 0; l < ContactLock::kLegsCount; ++l) {
      const ContactLock::Leg leg = static_cast<ContactLock::Leg>(l);
      const bool locked = lock_.locked(leg);
      if (!locked && !rays[l].hit) {
        continue;  // Swinging foot with no ground: no contact correction.
      }

      const Float3 target_ws =
          locked ? lock_.anchor(leg).target : rays[l].hit_point;
      ApplyLegIK(leg, target_ws, inv_root);
    }

    // Local rotations changed: rebuild every model matrix so the IK'd posture
    // is fully up to date before it is read back.
    LocalToModelJob full_ltm;
    full_ltm.skeleton = skeleton_.get();
    full_ltm.input = ozz::make_span(locals_);
    full_ltm.output = ozz::make_span(models_);
    Check(full_ltm.Run(), "final local to model job failed");
  }

  Float3 WorldAnkle(ContactLock::Leg _leg) const {
    const int ankle = _leg == ContactLock::kLeft ? kLeftFoot : kRightFoot;
    Float3 out;
    ozz::math::Store3PtrU(
        ozz::math::TransformPoint(RootAt(ratio_), models_[ankle].cols[3]),
        &out.x);
    return out;
  }

  bool locked(ContactLock::Leg _leg) const { return lock_.locked(_leg); }
  const ContactLock::Anchor& anchor(ContactLock::Leg _leg) const {
    return lock_.anchor(_leg);
  }
  float ratio() const { return ratio_; }
  const std::vector<Capture>& captures() const { return captures_; }

  // Returns the capture recorded for a leg at a given edge ratio.
  const Capture& FindCapture(ContactLock::Leg _leg, float _ratio) const {
    for (const Capture& capture : captures_) {
      if (capture.leg == _leg && Near(capture.ratio, _ratio, 1e-6f)) {
        return capture;
      }
    }
    Check(false, "no capture found for leg %d at ratio %g",
          static_cast<int>(_leg), _ratio);
    return captures_.front();  // Unreachable, Check aborts.
  }

 private:
  struct LegRay {
    bool hit = false;
    Float3 hit_point = Float3::zero();
    Float3 hit_normal = Float3(0.f, 1.f, 0.f);
  };
  struct MissWindow {
    float from = 0.f;
    float to = -1.f;  // Disabled by default.
  };

  static float Wrap(float _ratio) {
    const float wrapped = _ratio - std::floor(_ratio);
    return wrapped == 1.f ? 0.f : wrapped;
  }

  static int HipOf(ContactLock::Leg _leg) {
    return _leg == ContactLock::kLeft ? kLeftUpLeg : kRightUpLeg;
  }
  static int KneeOf(ContactLock::Leg _leg) {
    return _leg == ContactLock::kLeft ? kLeftLeg : kRightLeg;
  }
  static int AnkleOf(ContactLock::Leg _leg) {
    return _leg == ContactLock::kLeft ? kLeftFoot : kRightFoot;
  }

  bool IsMissed(ContactLock::Leg _leg, float _ratio) const {
    const MissWindow& miss = miss_[_leg];
    return _ratio >= miss.from && _ratio < miss.to;
  }

  // Procedural flat ground. The ray starts above the ankle and goes down.
  void Raycast(ContactLock::Leg _leg, float _ratio, LegRay* _ray) const {
    if (IsMissed(_leg, _ratio)) {
      _ray->hit = false;
      return;
    }
    const int ankle = AnkleOf(_leg);
    Float3 ankle_ws;
    ozz::math::Store3PtrU(
        ozz::math::TransformPoint(RootAt(_ratio), models_[ankle].cols[3]),
        &ankle_ws.x);
    _ray->hit = true;
    _ray->hit_point = Float3(ankle_ws.x, kFloorY, ankle_ws.z);
    _ray->hit_normal = Float3(0.f, 1.f, 0.f);
  }

  // ContactLock callback. Uses dedicated scratch buffers, so the current
  // frame context and posture are never touched.
  bool BuildEventSnapshot(ContactLock::Leg _leg, float _ratio,
                          ContactLock::EventSnapshot* _snapshot) {
    SamplingJob sampling;
    sampling.animation = animation_.get();
    sampling.context = &scratch_context_;
    sampling.ratio = Wrap(_ratio);
    sampling.output = ozz::make_span(scratch_locals_);
    if (!sampling.Run()) {
      return false;
    }
    LocalToModelJob ltm;
    ltm.skeleton = skeleton_.get();
    ltm.input = ozz::make_span(scratch_locals_);
    ltm.output = ozz::make_span(scratch_models_);
    if (!ltm.Run()) {
      return false;
    }

    _snapshot->root = RootAt(_ratio);
    _snapshot->ankle_model = scratch_models_[AnkleOf(_leg)];

    LegRay ray;
    // The event-time ray must use the event-time posture, not the current
    // frame one.
    Float3 ankle_ws;
    ozz::math::Store3PtrU(
        ozz::math::TransformPoint(_snapshot->root,
                                  _snapshot->ankle_model.cols[3]),
        &ankle_ws.x);
    if (IsMissed(_leg, _ratio)) {
      _snapshot->hit = false;
      return true;
    }
    ray.hit = true;
    ray.hit_point = Float3(ankle_ws.x, kFloorY, ankle_ws.z);
    ray.hit_normal = Float3(0.f, 1.f, 0.f);

    _snapshot->hit = true;
    _snapshot->hit_point = ray.hit_point;
    _snapshot->hit_normal = ray.hit_normal;
    // Foot height is 0 in the harness: the ankle target is the ground point.
    _snapshot->ankle_target_ws = ray.hit_point;
    return true;
  }

  bool IsReachable(ContactLock::Leg _leg) {
    if (!lock_.locked(_leg)) {
      return true;
    }
    const SimdFloat4 target_ms = ozz::math::TransformPoint(
        ozz::math::Invert(RootAt(ratio_)),
        ozz::math::simd_float4::Load3PtrU(&lock_.anchor(_leg).target.x));
    const float distance = ozz::math::GetX(ozz::math::Length3(
        target_ms - models_[HipOf(_leg)].cols[3]));
    return distance <= kChainLength;
  }

  void ApplyLegIK(ContactLock::Leg _leg, const Float3& _target_ws,
                  const Float4x4& _inv_root) {
    const int hip = HipOf(_leg);
    const int knee = KneeOf(_leg);
    const int ankle = AnkleOf(_leg);

    const SimdFloat4 target_ms = ozz::math::TransformPoint(
        _inv_root, ozz::math::simd_float4::Load3PtrU(&_target_ws.x));

    IKTwoBoneJob ik;
    ik.target = target_ms;
    ik.pole_vector = models_[knee].cols[1];
    ik.mid_axis = kKneeAxis;
    ik.weight = 1.f;
    ik.soften = 1.f;
    ik.start_joint = &models_[hip];
    ik.mid_joint = &models_[knee];
    ik.end_joint = &models_[ankle];
    SimdQuaternion start_correction;
    SimdQuaternion mid_correction;
    ik.start_joint_correction = &start_correction;
    ik.mid_joint_correction = &mid_correction;
    Check(ik.Run(), "two bone IK job failed");

    MultiplySoATransformQuaternion(hip, start_correction);
    MultiplySoATransformQuaternion(knee, mid_correction);

    LocalToModelJob partial_ltm;
    partial_ltm.skeleton = skeleton_.get();
    partial_ltm.input = ozz::make_span(locals_);
    partial_ltm.output = ozz::make_span(models_);
    partial_ltm.from = hip;
    partial_ltm.to = ankle;
    Check(partial_ltm.Run(), "partial local to model job failed");
  }

  // Local replica of sample framework's MultiplySoATransformQuaternion, so
  // the regression does not depend on the windowed framework target.
  void MultiplySoATransformQuaternion(int _index,
                                      const SimdQuaternion& _quat) {
    SoaTransform& soa = locals_[_index / 4];
    SimdQuaternion aos[4];
    ozz::math::Transpose4x4(&soa.rotation.x, &aos->xyzw);
    aos[_index & 3] = aos[_index & 3] * _quat;
    ozz::math::Transpose4x4(&aos->xyzw, &soa.rotation.x);
  }

  ozz::unique_ptr<Skeleton> skeleton_;
  ozz::unique_ptr<Animation> animation_;
  ozz::vector<SoaTransform> locals_;
  ozz::vector<Float4x4> models_;
  ozz::vector<SoaTransform> scratch_locals_;
  ozz::vector<Float4x4> scratch_models_;
  SamplingJob::Context context_;
  SamplingJob::Context scratch_context_;
  ContactLock lock_;
  MissWindow miss_[ContactLock::kLegsCount];

  float translation_speed_ = 0.f;  // Meters per animation loop.
  float yaw_speed_ = 0.f;          // Radians per animation loop.
  float ratio_ = 0.f;
  float previous_ratio_ = 0.f;
  std::vector<Capture> captures_;
};

// Drives _harness through _steps and writes one CSV row per update. Every
// locked row is also checked against the numerical tolerances required by the
// specification, reading the actual IK'd skeleton rather than state booleans.
static void RunScenario(Harness& _harness, const std::vector<float>& _steps,
                        FILE* _csv) {
  if (_csv) {
    std::fprintf(
        _csv,
        "ratio,left_contact,left_anchor_x,left_anchor_y,left_anchor_z,"
        "left_ankle_x,left_ankle_y,left_ankle_z,"
        "right_contact,right_anchor_x,right_anchor_y,right_anchor_z,"
        "right_ankle_x,right_ankle_y,right_ankle_z\n");
  }

  for (float step : _steps) {
    _harness.Update(step);
    if (_csv) {
      std::fprintf(_csv, "%.6f", _harness.ratio());
    }
    for (int l = 0; l < ContactLock::kLegsCount; ++l) {
      const ContactLock::Leg leg = static_cast<ContactLock::Leg>(l);
      const bool locked = _harness.locked(leg);
      const Float3& anchor = _harness.anchor(leg).target;
      const Float3 ankle = _harness.WorldAnkle(leg);

      if (locked) {
        // The anchor must still match what was captured at the edge.
        const Capture& capture =
            _harness.FindCapture(leg, _harness.anchor(leg).event_ratio);
        Check(Near3(anchor, capture.target, 1e-5f),
              "world anchor drift exceeds 1e-5 at ratio %g", _harness.ratio());
        // Full IK (weight 1, soften 1) must put the ankle on the anchor.
        Check(Near3(ankle, anchor, 1e-2f),
              "IK ankle error exceeds 1e-2 at ratio %g", _harness.ratio());
      }

      if (_csv) {
        std::fprintf(_csv,
                     ",%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f", locked ? 1 : 0,
                     anchor.x, anchor.y, anchor.z, ankle.x, ankle.y, ankle.z);
      }
    }
    if (_csv) {
      std::fputc('\n', _csv);
    }
  }
}

int main(int _argc, const char** _argv) {
  const char* csv_path =
      _argc > 1 ? _argv[1] : "contact_lock_regression.csv";
  FILE* csv = std::fopen(csv_path, "w");
  Check(csv != nullptr, "cannot open CSV file %s", csv_path);

  // Left support [0.2, 0.7), right support [0.5, 0.9) of every loop: the
  // [0.5, 0.7) overlap is a two-foot handoff.
  ozz::unique_ptr<FloatTrack> left_step = BuildStepTrack(.2f, .7f);
  ozz::unique_ptr<FloatTrack> right_step = BuildStepTrack(.5f, .9f);
  ozz::unique_ptr<FloatTrack> linear = BuildLinearTrack();

  // ---------------------------------------------------------------------------
  // 1. Translation: anchors stay world-fixed, IK ankles reach them.
  // ---------------------------------------------------------------------------
  {
    Harness harness;
    harness.SetTrack(ContactLock::kLeft, left_step.get());
    harness.SetTrack(ContactLock::kRight, right_step.get());
    harness.SetMotion(/*translation*/ .35f, /*yaw*/ 0.f);

    std::fprintf(csv, "# translation scenario\n");
    std::vector<float> steps;
    for (float r = .05f; r <= .66f; r += .05f) {
      steps.push_back(r);
    }
    RunScenario(harness, steps, csv);

    Check(harness.locked(ContactLock::kLeft),
          "left foot should be locked inside its support phase");
    const Float3 left_anchor = harness.anchor(ContactLock::kLeft).target;
    Check(Near(harness.anchor(ContactLock::kLeft).event_ratio, .2f, 1e-6f),
          "left anchor captured at the rising edge ratio");

    // The anchor never moves after capture: compare against every later row.
    for (float r = .25f; r <= .65f; r += .1f) {
      Harness probe;
      probe.SetTrack(ContactLock::kLeft, left_step.get());
      probe.SetTrack(ContactLock::kRight, right_step.get());
      probe.SetMotion(.35f, 0.f);
      probe.Update(r);
      Check(probe.locked(ContactLock::kLeft), "expected lock at ratio %g", r);
      Check(Near3(probe.anchor(ContactLock::kLeft).target, left_anchor, 1e-5f),
            "world anchor drifted while the root translated");
      const Float3 ankle = probe.WorldAnkle(ContactLock::kLeft);
      Check(Near3(ankle, left_anchor, 1e-2f),
            "IK ankle error %.6f exceeds tolerance at ratio %g",
            std::fabs(ankle.x - left_anchor.x), r);
    }
    std::printf("PASS translation lock and IK reach\n");
  }

  // ---------------------------------------------------------------------------
  // 2. Rotation around the vertical axis: same guarantees while yawing.
  // ---------------------------------------------------------------------------
  {
    Harness harness;
    harness.SetTrack(ContactLock::kLeft, left_step.get());
    harness.SetTrack(ContactLock::kRight, right_step.get());
    harness.SetMotion(0.f, ozz::math::kPi);  // Half turn per loop.

    std::fprintf(csv, "# rotation scenario\n");
    std::vector<float> steps;
    for (float r = .05f; r <= .66f; r += .05f) {
      steps.push_back(r);
    }
    RunScenario(harness, steps, csv);

    const Float3 anchor = harness.anchor(ContactLock::kLeft).target;
    Check(harness.locked(ContactLock::kLeft), "left foot should stay locked");
    for (float r = .25f; r <= .65f; r += .1f) {
      Harness probe;
      probe.SetTrack(ContactLock::kLeft, left_step.get());
      probe.SetTrack(ContactLock::kRight, right_step.get());
      probe.SetMotion(0.f, ozz::math::kPi);
      probe.Update(r);
      Check(Near3(probe.anchor(ContactLock::kLeft).target, anchor, 1e-5f),
            "world anchor drifted while the root rotated");
      Check(Near3(probe.WorldAnkle(ContactLock::kLeft), anchor, 1e-2f),
            "IK ankle fails to follow the anchor while yawing");
    }
    std::printf("PASS rotation lock and IK reach\n");
  }

  // ---------------------------------------------------------------------------
  // 3. One big update vs several unequal steps: same events and anchors,
  //    including a cross-loop range and an edge landing exactly on a boundary.
  // ---------------------------------------------------------------------------
  {
    Harness once;
    once.SetTrack(ContactLock::kLeft, left_step.get());
    once.SetTrack(ContactLock::kRight, right_step.get());
    once.SetMotion(.2f, .3f);
    std::fprintf(csv, "# one-shot scenario\n");
    RunScenario(once, {1.3f}, csv);

    Harness split;
    split.SetTrack(ContactLock::kLeft, left_step.get());
    split.SetTrack(ContactLock::kRight, right_step.get());
    split.SetMotion(.2f, .3f);
    std::fprintf(csv, "# split scenario\n");
    // 0.7 is the left falling edge: an update ending exactly there must not
    // process it ([from,to) semantics), the next update starting at 0.7 does.
    RunScenario(split, {.13f, .37f, .7f, .8f, 1.05f, 1.3f}, csv);

    const auto& a = once.captures();
    const auto& b = split.captures();
    Check(a.size() == b.size(),
          "capture count mismatch one-shot=%zu split=%zu", a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
      Check(a[i].leg == b[i].leg && Near(a[i].ratio, b[i].ratio, 1e-6f),
            "capture %zu event mismatch", i);
      Check(Near3(a[i].target, b[i].target, 1e-6f),
            "capture %zu anchor mismatch", i);
    }
    Check(Near3(once.anchor(ContactLock::kLeft).target,
                split.anchor(ContactLock::kLeft).target, 1e-6f),
          "final left anchors differ between update strategies");
    Check(Near(once.anchor(ContactLock::kLeft).event_ratio,
               split.anchor(ContactLock::kLeft).event_ratio, 1e-6f),
          "final left event ratios differ");

    // Half-open interval check: at the update ending on 0.7 the left foot is
    // still locked; only the following update releases it.
    Harness boundary;
    boundary.SetTrack(ContactLock::kLeft, left_step.get());
    boundary.SetMotion(0.f, 0.f);
    boundary.Update(.7f);
    Check(boundary.locked(ContactLock::kLeft),
          "edge exactly on 'to' must belong to the next update");
    boundary.Update(.71f);
    Check(!boundary.locked(ContactLock::kLeft),
          "falling edge should release on the next update");
    std::printf("PASS cross-loop, half-open edge and unequal steps\n");
  }

  // ---------------------------------------------------------------------------
  // 4. Two-foot handoff: both feet locked at the same time during overlap.
  // ---------------------------------------------------------------------------
  {
    Harness harness;
    harness.SetTrack(ContactLock::kLeft, left_step.get());
    harness.SetTrack(ContactLock::kRight, right_step.get());
    harness.SetMotion(0.f, 0.f);
    harness.Update(.6f);
    Check(harness.locked(ContactLock::kLeft) &&
              harness.locked(ContactLock::kRight),
          "both feet should be locked during the handoff overlap");
    Check(Near3(harness.WorldAnkle(ContactLock::kLeft),
                harness.anchor(ContactLock::kLeft).target, 1e-2f),
          "left ankle off its anchor during handoff");
    Check(Near3(harness.WorldAnkle(ContactLock::kRight),
                harness.anchor(ContactLock::kRight).target, 1e-2f),
          "right ankle off its anchor during handoff");
    std::printf("PASS two-foot handoff\n");
  }

  // ---------------------------------------------------------------------------
  // 5. Ray miss at the rising edge: no lock for the whole support phase; the
  //    next rising edge captures normally.
  // ---------------------------------------------------------------------------
  {
    Harness harness;
    harness.SetTrack(ContactLock::kLeft, left_step.get());
    harness.SetTrack(ContactLock::kRight, right_step.get());
    harness.SetMotion(0.f, 0.f);
    harness.SetMissWindow(ContactLock::kLeft, .15f, .75f);
    harness.Update(.3f);
    Check(!harness.locked(ContactLock::kLeft),
          "ray miss at contact time must prevent locking");
    harness.Update(.6f);
    Check(!harness.locked(ContactLock::kLeft),
          "missed support phase must not re-capture later");
    // Second loop: the ground is back, the next rising edge locks.
    harness.Update(1.25f);
    Check(harness.locked(ContactLock::kLeft),
          "next rising edge should lock after the miss phase");
    std::printf("PASS ray miss handling\n");
  }

  // ---------------------------------------------------------------------------
  // 6. Unreachable anchor: released once, stays released until the next
  //    rising edge even if the target becomes reachable again (no sliding).
  // ---------------------------------------------------------------------------
  {
    Harness harness;
    harness.SetTrack(ContactLock::kLeft, left_step.get());
    harness.SetMotion(0.f, 0.f);
    harness.Update(.25f);
    Check(harness.locked(ContactLock::kLeft), "expected an initial lock");

    // Teleport the root far sideways: the fixed world anchor leaves reach.
    harness.SetMotion(8.f, 0.f);
    harness.Update(.35f);
    Check(!harness.locked(ContactLock::kLeft),
          "unreachable anchor must be released");

    // Bring the root back before the falling edge: no re-capture. Note the
    // analytic trajectory is speed * ratio, so zeroing the speed restores the
    // root to its origin position.
    harness.SetMotion(0.f, 0.f);
    harness.Update(.45f);
    Check(!harness.locked(ContactLock::kLeft),
          "released lock must not re-capture within the same support phase");

    // The next rising edge (second loop) arms a fresh capture.
    harness.Update(1.25f);
    Check(harness.locked(ContactLock::kLeft),
          "next rising edge should re-arm the lock");
    std::printf("PASS unreachable release without re-capture\n");
  }

  // ---------------------------------------------------------------------------
  // 7. Reset clears anchors; paused updates never re-capture.
  // ---------------------------------------------------------------------------
  {
    Harness harness;
    harness.SetTrack(ContactLock::kLeft, left_step.get());
    harness.SetMotion(0.f, 0.f);
    harness.Update(.3f);
    Check(harness.locked(ContactLock::kLeft), "expected a lock before reset");

    harness.Reset(.3f);
    Check(!harness.locked(ContactLock::kLeft), "reset must clear the lock");

    // Paused updates keep the unlocked state and do not capture.
    harness.Update(.3f);
    harness.Update(.3f);
    Check(!harness.locked(ContactLock::kLeft),
          "paused updates must not capture");

    // A later rising edge captures again from the clean state.
    harness.Update(1.25f);
    Check(harness.locked(ContactLock::kLeft),
          "lock should be acquired again after reset");
    const float event_ratio =
        harness.anchor(ContactLock::kLeft).event_ratio;
    harness.Update(1.25f);  // Paused while locked.
    harness.Update(1.25f);
    Check(harness.locked(ContactLock::kLeft) &&
              Near(harness.anchor(ContactLock::kLeft).event_ratio,
                   event_ratio, 0.f),
          "paused updates must not re-capture an existing lock");
    std::printf("PASS reset and paused updates\n");
  }

  // ---------------------------------------------------------------------------
  // 8. Linear track: threshold crossings are found on the ramps.
  // ---------------------------------------------------------------------------
  {
    Harness harness;
    harness.SetTrack(ContactLock::kLeft, linear.get());
    harness.SetMotion(0.f, 0.f);
    harness.Update(.25f);
    Check(harness.locked(ContactLock::kLeft), "linear rising edge not found");
    Check(Near(harness.anchor(ContactLock::kLeft).event_ratio, .2f, 1e-6f),
          "linear rising edge captured at %.6f, expected .2",
          harness.anchor(ContactLock::kLeft).event_ratio);
    harness.Update(.65f);
    Check(!harness.locked(ContactLock::kLeft),
          "linear falling edge not detected");
    std::printf("PASS linear track threshold crossings\n");
  }

  std::fclose(csv);
  std::printf("ALL CONTACT LOCK CHECKS PASSED (csv: %s)\n", csv_path);
  return 0;
}
