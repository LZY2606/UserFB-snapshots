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

#include <cmath>

#include "gtest/gtest.h"
#include "ozz/animation/runtime/inertialization_job.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/vec_float.h"

using ozz::animation::InertializationJob;
using ozz::math::Float3;
using ozz::math::Quaternion;
using ozz::math::SoaTransform;

namespace {

// Continuity tolerances, as specified for normal-scale data.
const float kPosTol = 1e-4f;
const float kRotTol = 1e-4f;
const float kVelTol = 2e-3f;

// A per-joint local pose and its time derivative, in plain scalars.
struct JointTarget {
  Float3 translation;
  Quaternion rotation;
  Float3 scale;
  Float3 vel_translation;
  Float3 vel_angular;
  Float3 vel_log_scale;
};

// A motion with constant linear velocity, constant parent-frame angular
// velocity and constant log-scale velocity:
// p(t) = p0 + v t, q(t) = Exp(w t) * q0, s(t) = s0 * exp(k t).
struct Motion {
  Float3 p0;
  Float3 v;
  Quaternion q0;
  Float3 w;
  Float3 s0;
  Float3 k;

  JointTarget At(float _t) const {
    JointTarget jt;
    jt.translation = p0 + v * _t;
    const float angle = ozz::math::Length(w) * _t;
    jt.rotation =
        angle != 0.f
            ? Quaternion::FromAxisAngle(ozz::math::Normalize(w), angle) * q0
            : q0;
    jt.scale = Float3(s0.x * std::exp(k.x * _t), s0.y * std::exp(k.y * _t),
                      s0.z * std::exp(k.z * _t));
    jt.vel_translation = v;
    jt.vel_angular = w;
    jt.vel_log_scale = k;
    return jt;
  }
};

float LaneAt(const ozz::math::SimdFloat4& _v, int _lane) {
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

void SetLaneRaw(ozz::math::SimdFloat4& _v, int _lane, float _f) {
  float f[4] = {ozz::math::GetX(_v), ozz::math::GetY(_v), ozz::math::GetZ(_v),
                ozz::math::GetW(_v)};
  f[_lane] = _f;
  _v = ozz::math::simd_float4::Load(f[0], f[1], f[2], f[3]);
}

// Packs _count (1 to 4) joint poses into a single SoA element. Remaining
// lanes are filled with the identity pose.
SoaTransform PackPose(const JointTarget* _j, int _count) {
  float tx[4] = {0.f, 0.f, 0.f, 0.f}, ty[4] = {0.f, 0.f, 0.f, 0.f},
        tz[4] = {0.f, 0.f, 0.f, 0.f};
  float qx[4] = {0.f, 0.f, 0.f, 0.f}, qy[4] = {0.f, 0.f, 0.f, 0.f},
        qz[4] = {0.f, 0.f, 0.f, 0.f}, qw[4] = {1.f, 1.f, 1.f, 1.f};
  float sx[4] = {1.f, 1.f, 1.f, 1.f}, sy[4] = {1.f, 1.f, 1.f, 1.f},
        sz[4] = {1.f, 1.f, 1.f, 1.f};
  for (int i = 0; i < _count; ++i) {
    tx[i] = _j[i].translation.x;
    ty[i] = _j[i].translation.y;
    tz[i] = _j[i].translation.z;
    qx[i] = _j[i].rotation.x;
    qy[i] = _j[i].rotation.y;
    qz[i] = _j[i].rotation.z;
    qw[i] = _j[i].rotation.w;
    sx[i] = _j[i].scale.x;
    sy[i] = _j[i].scale.y;
    sz[i] = _j[i].scale.z;
  }
  SoaTransform t;
  t.translation.x = ozz::math::simd_float4::Load(tx[0], tx[1], tx[2], tx[3]);
  t.translation.y = ozz::math::simd_float4::Load(ty[0], ty[1], ty[2], ty[3]);
  t.translation.z = ozz::math::simd_float4::Load(tz[0], tz[1], tz[2], tz[3]);
  t.rotation.x = ozz::math::simd_float4::Load(qx[0], qx[1], qx[2], qx[3]);
  t.rotation.y = ozz::math::simd_float4::Load(qy[0], qy[1], qy[2], qy[3]);
  t.rotation.z = ozz::math::simd_float4::Load(qz[0], qz[1], qz[2], qz[3]);
  t.rotation.w = ozz::math::simd_float4::Load(qw[0], qw[1], qw[2], qw[3]);
  t.scale.x = ozz::math::simd_float4::Load(sx[0], sx[1], sx[2], sx[3]);
  t.scale.y = ozz::math::simd_float4::Load(sy[0], sy[1], sy[2], sy[3]);
  t.scale.z = ozz::math::simd_float4::Load(sz[0], sz[1], sz[2], sz[3]);
  return t;
}

InertializationJob::Velocity PackVelocity(const JointTarget* _j, int _count) {
  float tx[4] = {0.f, 0.f, 0.f, 0.f}, ty[4] = {0.f, 0.f, 0.f, 0.f},
        tz[4] = {0.f, 0.f, 0.f, 0.f};
  float ax[4] = {0.f, 0.f, 0.f, 0.f}, ay[4] = {0.f, 0.f, 0.f, 0.f},
        az[4] = {0.f, 0.f, 0.f, 0.f};
  float lx[4] = {0.f, 0.f, 0.f, 0.f}, ly[4] = {0.f, 0.f, 0.f, 0.f},
        lz[4] = {0.f, 0.f, 0.f, 0.f};
  for (int i = 0; i < _count; ++i) {
    tx[i] = _j[i].vel_translation.x;
    ty[i] = _j[i].vel_translation.y;
    tz[i] = _j[i].vel_translation.z;
    ax[i] = _j[i].vel_angular.x;
    ay[i] = _j[i].vel_angular.y;
    az[i] = _j[i].vel_angular.z;
    lx[i] = _j[i].vel_log_scale.x;
    ly[i] = _j[i].vel_log_scale.y;
    lz[i] = _j[i].vel_log_scale.z;
  }
  InertializationJob::Velocity v;
  v.translation.x = ozz::math::simd_float4::Load(tx[0], tx[1], tx[2], tx[3]);
  v.translation.y = ozz::math::simd_float4::Load(ty[0], ty[1], ty[2], ty[3]);
  v.translation.z = ozz::math::simd_float4::Load(tz[0], tz[1], tz[2], tz[3]);
  v.angular.x = ozz::math::simd_float4::Load(ax[0], ax[1], ax[2], ax[3]);
  v.angular.y = ozz::math::simd_float4::Load(ay[0], ay[1], ay[2], ay[3]);
  v.angular.z = ozz::math::simd_float4::Load(az[0], az[1], az[2], az[3]);
  v.log_scale.x = ozz::math::simd_float4::Load(lx[0], lx[1], lx[2], lx[3]);
  v.log_scale.y = ozz::math::simd_float4::Load(ly[0], ly[1], ly[2], ly[3]);
  v.log_scale.z = ozz::math::simd_float4::Load(lz[0], lz[1], lz[2], lz[3]);
  return v;
}

JointTarget GetJoint(const SoaTransform* _poses,
                     const InertializationJob::Velocity* _vels, int _j) {
  const int e = _j >> 2;
  const int l = _j & 3;
  JointTarget jt;
  jt.translation =
      Float3(LaneAt(_poses[e].translation.x, l), LaneAt(_poses[e].translation.y, l),
             LaneAt(_poses[e].translation.z, l));
  jt.rotation =
      Quaternion(LaneAt(_poses[e].rotation.x, l), LaneAt(_poses[e].rotation.y, l),
                 LaneAt(_poses[e].rotation.z, l), LaneAt(_poses[e].rotation.w, l));
  jt.scale = Float3(LaneAt(_poses[e].scale.x, l), LaneAt(_poses[e].scale.y, l),
                    LaneAt(_poses[e].scale.z, l));
  jt.vel_translation = Float3(LaneAt(_vels[e].translation.x, l),
                              LaneAt(_vels[e].translation.y, l),
                              LaneAt(_vels[e].translation.z, l));
  jt.vel_angular =
      Float3(LaneAt(_vels[e].angular.x, l), LaneAt(_vels[e].angular.y, l),
             LaneAt(_vels[e].angular.z, l));
  jt.vel_log_scale =
      Float3(LaneAt(_vels[e].log_scale.x, l), LaneAt(_vels[e].log_scale.y, l),
             LaneAt(_vels[e].log_scale.z, l));
  return jt;
}

// Textbook quaternion to rotation matrix conversion. This reference is
// independent from the rotation helpers of the implementation under test.
void QuatToMat3(const Quaternion& _q, float _m[3][3]) {
  const float x = _q.x, y = _q.y, z = _q.z, w = _q.w;
  _m[0][0] = 1.f - 2.f * (y * y + z * z);
  _m[0][1] = 2.f * (x * y - z * w);
  _m[0][2] = 2.f * (x * z + y * w);
  _m[1][0] = 2.f * (x * y + z * w);
  _m[1][1] = 1.f - 2.f * (x * x + z * z);
  _m[1][2] = 2.f * (y * z - x * w);
  _m[2][0] = 2.f * (x * z - y * w);
  _m[2][1] = 2.f * (y * z + x * w);
  _m[2][2] = 1.f - 2.f * (x * x + y * y);
}

void Mul33(const float _a[3][3], const float _b[3][3], float _out[3][3]) {
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      _out[i][j] = _a[i][0] * _b[0][j] + _a[i][1] * _b[1][j] + _a[i][2] * _b[2][j];
    }
  }
}

void Transpose33(const float _a[3][3], float _out[3][3]) {
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      _out[i][j] = _a[j][i];
    }
  }
}

Float3 RotateVec(const Quaternion& _q, const Float3& _v) {
  float m[3][3];
  QuatToMat3(_q, m);
  return Float3(m[0][0] * _v.x + m[0][1] * _v.y + m[0][2] * _v.z,
                m[1][0] * _v.x + m[1][1] * _v.y + m[1][2] * _v.z,
                m[2][0] * _v.x + m[2][1] * _v.y + m[2][2] * _v.z);
}

void ExpectFloat3Near(const Float3& _a, const Float3& _b, float _tol) {
  EXPECT_NEAR(_a.x, _b.x, _tol);
  EXPECT_NEAR(_a.y, _b.y, _tol);
  EXPECT_NEAR(_a.z, _b.z, _tol);
}

void ExpectRotationNear(const Quaternion& _a, const Quaternion& _b,
                        float _tol) {
  float ma[3][3], mb[3][3];
  QuatToMat3(_a, ma);
  QuatToMat3(_b, mb);
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      EXPECT_NEAR(ma[i][j], mb[i][j], _tol);
    }
  }
}

void ExpectJointNear(const JointTarget& _a, const JointTarget& _b,
                     float _pos_tol, float _rot_tol, float _vel_tol) {
  ExpectFloat3Near(_a.translation, _b.translation, _pos_tol);
  ExpectRotationNear(_a.rotation, _b.rotation, _rot_tol);
  ExpectFloat3Near(_a.scale, _b.scale, _pos_tol);
  ExpectFloat3Near(_a.vel_translation, _b.vel_translation, _vel_tol);
  ExpectFloat3Near(_a.vel_angular, _b.vel_angular, _vel_tol);
  ExpectFloat3Near(_a.vel_log_scale, _b.vel_log_scale, _vel_tol);
}

void ExpectJointExact(const JointTarget& _a, const JointTarget& _b) {
  EXPECT_EQ(_a.translation.x, _b.translation.x);
  EXPECT_EQ(_a.translation.y, _b.translation.y);
  EXPECT_EQ(_a.translation.z, _b.translation.z);
  EXPECT_EQ(_a.rotation.x, _b.rotation.x);
  EXPECT_EQ(_a.rotation.y, _b.rotation.y);
  EXPECT_EQ(_a.rotation.z, _b.rotation.z);
  EXPECT_EQ(_a.rotation.w, _b.rotation.w);
  EXPECT_EQ(_a.scale.x, _b.scale.x);
  EXPECT_EQ(_a.scale.y, _b.scale.y);
  EXPECT_EQ(_a.scale.z, _b.scale.z);
  EXPECT_EQ(_a.vel_translation.x, _b.vel_translation.x);
  EXPECT_EQ(_a.vel_translation.y, _b.vel_translation.y);
  EXPECT_EQ(_a.vel_translation.z, _b.vel_translation.z);
  EXPECT_EQ(_a.vel_angular.x, _b.vel_angular.x);
  EXPECT_EQ(_a.vel_angular.y, _b.vel_angular.y);
  EXPECT_EQ(_a.vel_angular.z, _b.vel_angular.z);
  EXPECT_EQ(_a.vel_log_scale.x, _b.vel_log_scale.x);
  EXPECT_EQ(_a.vel_log_scale.y, _b.vel_log_scale.y);
  EXPECT_EQ(_a.vel_log_scale.z, _b.vel_log_scale.z);
}

bool IsFinite(const JointTarget& _j) {
  const float* f = &_j.translation.x;
  for (int i = 0; i < 19; ++i) {
    if (!std::isfinite(f[i])) {
      return false;
    }
  }
  return true;
}

// A rig wrapping a context and its input/output buffers.
struct Rig {
  explicit Rig(int _num_joints)
      : num_joints(_num_joints), num_soa((_num_joints + 3) / 4) {
    context.Resize(_num_joints);
    target_pose.resize(num_soa);
    target_vel.resize(num_soa);
    out_pose.resize(num_soa);
    out_vel.resize(num_soa);
  }

  void SetTarget(const JointTarget* _joints) {
    for (int e = 0; e < num_soa; ++e) {
      const int count = num_joints - e * 4 < 4 ? num_joints - e * 4 : 4;
      target_pose[e] = PackPose(_joints + e * 4, count);
      target_vel[e] = PackVelocity(_joints + e * 4, count);
    }
  }

  InertializationJob Job(float _dt, float _duration) {
    InertializationJob job;
    job.context = &context;
    job.target = {target_pose.data(), target_pose.size()};
    job.target_velocity = {target_vel.data(), target_vel.size()};
    job.output = {out_pose.data(), out_pose.size()};
    job.output_velocity = {out_vel.data(), out_vel.size()};
    job.delta_time = _dt;
    job.transition_duration = _duration;
    return job;
  }

  JointTarget GetOutput(int _j) const {
    return GetJoint(out_pose.data(), out_vel.data(), _j);
  }

  int num_joints;
  int num_soa;
  InertializationJob::Context context;
  ozz::vector<SoaTransform> target_pose;
  ozz::vector<InertializationJob::Velocity> target_vel;
  ozz::vector<SoaTransform> out_pose;
  ozz::vector<InertializationJob::Velocity> out_vel;
};

// Spec-defined cubic residual curve: x(u) = (2u^3-3u^2+1) x0 + (u^3-2u^2+u)
// T v0, and its analytic time derivative.
void Cubic(float _u, float _duration, float _x0, float _v0, float& _x,
           float& _dx) {
  const float u2 = _u * _u;
  const float u3 = u2 * _u;
  _x = (2.f * u3 - 3.f * u2 + 1.f) * _x0 + (u3 - 2.f * u2 + _u) * _duration * _v0;
  _dx = (6.f * u2 - 6.f * _u) / _duration * _x0 +
        (3.f * u2 - 4.f * _u + 1.f) * _v0;
}

Motion MotionA() {
  const Motion m = {Float3(1.f, 2.f, 3.f),
                    Float3(.5f, -.2f, .1f),
                    Quaternion::FromAxisAngle(Float3::z_axis(), .4f),
                    Float3(0.f, 0.f, .3f),
                    Float3(1.1f, .9f, 1.2f),
                    Float3(.1f, -.05f, .02f)};
  return m;
}

Motion MotionB() {
  const Motion m = {Float3(-1.f, .5f, 2.f),
                    Float3(-.3f, .4f, .2f),
                    Quaternion::FromAxisAngle(Float3::z_axis(), 1.f),
                    Float3(0.f, 0.f, -.4f),
                    Float3(.8f, 1.3f, 1.f),
                    Float3(.03f, 0.f, -.1f)};
  return m;
}

}  // namespace

TEST(InertializationValidity, JobValidity) {
  Rig rig(2);
  const Motion a = MotionA();
  const JointTarget j0 = a.At(0.f);
  const JointTarget joints[2] = {j0, j0};
  rig.SetTarget(joints);

  // Default job: no context.
  {
    InertializationJob job;
    EXPECT_FALSE(job.Validate());
    EXPECT_FALSE(job.Run());
  }
  // Context not resized.
  {
    InertializationJob::Context context;
    InertializationJob job = rig.Job(0.f, 0.f);
    job.context = &context;
    EXPECT_FALSE(job.Validate());
    EXPECT_FALSE(job.Run());
  }
  // Valid initialization call.
  {
    InertializationJob job = rig.Job(0.f, 0.f);
    EXPECT_TRUE(job.Validate());
    EXPECT_TRUE(job.Run());
  }
  // Missing input/output ranges.
  {
    InertializationJob job = rig.Job(0.f, 0.f);
    job.target = {};
    EXPECT_FALSE(job.Validate());
    job = rig.Job(0.f, 0.f);
    job.target_velocity = {};
    EXPECT_FALSE(job.Validate());
    job = rig.Job(0.f, 0.f);
    job.output = {};
    EXPECT_FALSE(job.Validate());
    job = rig.Job(0.f, 0.f);
    job.output_velocity = {};
    EXPECT_FALSE(job.Validate());
  }
  // Too small ranges.
  {
    InertializationJob job = rig.Job(0.f, 0.f);
    job.target = {rig.target_pose.data(), static_cast<size_t>(0)};
    EXPECT_FALSE(job.Validate());
  }
  // Illegal times.
  {
    EXPECT_FALSE(rig.Job(-.1f, 0.f).Validate());
    EXPECT_FALSE(rig.Job(0.f, -.1f).Validate());
    EXPECT_FALSE(rig.Job(.1f, .5f).Validate());  // switch cannot advance time
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(rig.Job(nan, 0.f).Validate());
    EXPECT_FALSE(rig.Job(0.f, nan).Validate());
    EXPECT_FALSE(rig.Job(inf, 0.f).Validate());
    EXPECT_FALSE(rig.Job(0.f, inf).Validate());
  }
  // Non-finite target inputs.
  {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    JointTarget bad[2] = {j0, j0};
    bad[0].translation.x = nan;
    rig.SetTarget(bad);
    EXPECT_FALSE(rig.Job(0.f, 0.f).Validate());
    bad[0] = j0;
    bad[1].vel_angular.y = nan;
    rig.SetTarget(bad);
    EXPECT_FALSE(rig.Job(0.f, 0.f).Validate());
    bad[1] = j0;
    bad[0].vel_log_scale.z = nan;
    rig.SetTarget(bad);
    EXPECT_FALSE(rig.Job(0.f, 0.f).Validate());
  }
  // Non-positive scale.
  {
    JointTarget bad[2] = {j0, j0};
    bad[0].scale.y = 0.f;
    rig.SetTarget(bad);
    EXPECT_FALSE(rig.Job(0.f, 0.f).Validate());
    bad[0].scale.y = -1.f;
    rig.SetTarget(bad);
    EXPECT_FALSE(rig.Job(0.f, 0.f).Validate());
  }
  // Non-unit quaternion: squared length off by more than 1e-3.
  {
    JointTarget bad[2] = {j0, j0};
    bad[0].rotation = Quaternion(0.f, 0.f, 0.f, 1.1f);  // sq len 1.21
    rig.SetTarget(bad);
    EXPECT_FALSE(rig.Job(0.f, 0.f).Validate());
    bad[0].rotation = Quaternion(0.f, 0.f, 0.f, 1.f + 2.5e-2f);  // off ~5e-2
    rig.SetTarget(bad);
    EXPECT_FALSE(rig.Job(0.f, 0.f).Validate());
    // Within tolerance: sq len off by less than 1e-3.
    JointTarget ok[2] = {j0, j0};
    ok[0].rotation = Quaternion(0.f, 0.f, 0.f, 1.f + 4e-4f);
    rig.SetTarget(ok);
    EXPECT_TRUE(rig.Job(0.f, 0.f).Validate());
  }
  rig.SetTarget(joints);
}

TEST(InertializationValidity, ValidateHasNoSideEffect) {
  Rig rig(2);
  Rig control(2);
  const Motion a = MotionA();
  const JointTarget ja[2] = {a.At(0.f), a.At(.1f)};
  rig.SetTarget(ja);
  control.SetTarget(ja);

  // Pre-fill outputs with a sentinel.
  const float kSentinel = 12345.f;
  for (int e = 0; e < rig.num_soa; ++e) {
    rig.out_pose[e].translation.x = ozz::math::simd_float4::Load1(kSentinel);
    rig.out_vel[e].angular.z = ozz::math::simd_float4::Load1(kSentinel);
  }
  // Validate (valid and invalid jobs) must not touch outputs nor context.
  InertializationJob valid = rig.Job(0.f, 0.f);
  EXPECT_TRUE(valid.Validate());
  InertializationJob invalid = rig.Job(-1.f, 0.f);
  EXPECT_FALSE(invalid.Validate());
  EXPECT_TRUE(valid.Validate());
  EXPECT_EQ(LaneAt(rig.out_pose[0].translation.x, 0), kSentinel);
  EXPECT_EQ(LaneAt(rig.out_vel[0].angular.z, 0), kSentinel);

  // A run after the Validate calls matches the control rig.
  EXPECT_TRUE(valid.Run());
  EXPECT_TRUE(control.Job(0.f, 0.f).Run());
  for (int j = 0; j < 2; ++j) {
    ExpectJointExact(rig.GetOutput(j), control.GetOutput(j));
  }
}

TEST(InertializationFirstUse, AdoptsTarget) {
  Rig rig(2);
  const Motion a = MotionA();
  const JointTarget joints[2] = {a.At(.3f), a.At(-.7f)};
  rig.SetTarget(joints);
  EXPECT_TRUE(rig.Job(0.f, 0.f).Run());
  for (int j = 0; j < 2; ++j) {
    ExpectJointExact(rig.GetOutput(j), joints[j]);
  }
}

TEST(InertializationFirstUse, InvalidFirstCallsKeepState) {
  Rig rig(2);
  const Motion a = MotionA();
  const JointTarget joints[2] = {a.At(0.f), a.At(0.f)};
  rig.SetTarget(joints);

  // Pre-fill outputs with a sentinel.
  const float kSentinel = -777.f;
  for (int e = 0; e < rig.num_soa; ++e) {
    rig.out_pose[e].translation.y = ozz::math::simd_float4::Load1(kSentinel);
  }
  // First call cannot advance time nor request a transition.
  EXPECT_FALSE(rig.Job(.1f, 0.f).Run());
  EXPECT_FALSE(rig.Job(0.f, .5f).Run());
  EXPECT_EQ(LaneAt(rig.out_pose[0].translation.y, 0), kSentinel);
  EXPECT_EQ(LaneAt(rig.out_pose[0].translation.y, 1), kSentinel);
  // The context is still uninitialized: a transition is still refused.
  EXPECT_FALSE(rig.Job(0.f, .5f).Run());
  // The proper initialization call now works.
  EXPECT_TRUE(rig.Job(0.f, 0.f).Run());
  ExpectJointExact(rig.GetOutput(0), joints[0]);
  ExpectJointExact(rig.GetOutput(1), joints[1]);
}

TEST(InertializationAdvance, Passthrough) {
  Rig rig(2);
  const Motion a = MotionA();
  // No transition was ever requested: the output is the target, verbatim.
  float time = 0.f;
  const JointTarget j_init[2] = {a.At(0.f), a.At(0.f)};
  rig.SetTarget(j_init);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  const float dts[] = {.1f, .15f, 0.f, .2f};
  for (float dt : dts) {
    time += dt;
    const JointTarget jt[2] = {a.At(time), a.At(time * .5f)};
    rig.SetTarget(jt);
    ASSERT_TRUE(rig.Job(dt, 0.f).Run());
    for (int j = 0; j < 2; ++j) {
      ExpectJointExact(rig.GetOutput(j), jt[j]);
    }
  }
}

TEST(InertializationAdvance, ZeroTimeCallsDontConsumeTransition) {
  Rig rig(1);
  Rig control(1);
  const Motion a = MotionA();
  const Motion b = MotionB();
  const JointTarget j0 = a.At(0.f);
  rig.SetTarget(&j0);
  control.SetTarget(&j0);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  ASSERT_TRUE(control.Job(0.f, 0.f).Run());
  const JointTarget jb0 = b.At(0.f);
  rig.SetTarget(&jb0);
  control.SetTarget(&jb0);
  ASSERT_TRUE(rig.Job(0.f, .5f).Run());
  ASSERT_TRUE(control.Job(0.f, .5f).Run());
  const JointTarget at_switch = rig.GetOutput(0);
  // Consecutive zero-time calls don't consume the transition: the output
  // stays the one of the switch instant.
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
    ExpectJointNear(rig.GetOutput(0), at_switch, 1e-6f, 1e-6f, 1e-6f);
  }
  // A later advance matches the control rig that never saw zero-time calls.
  const JointTarget jb = b.At(.25f);
  rig.SetTarget(&jb);
  control.SetTarget(&jb);
  ASSERT_TRUE(rig.Job(.25f, 0.f).Run());
  ASSERT_TRUE(control.Job(.25f, 0.f).Run());
  ExpectJointExact(rig.GetOutput(0), control.GetOutput(0));
}

TEST(InertializationCurve, ScalarResidualCurve) {
  // Single joint, rotations about a single fixed axis (Z): the rotation
  // vector math reduces to scalars, so the spec curve can be checked
  // against plain scalar expectations.
  Rig rig(1);
  const Motion a = MotionA();
  const Motion b = MotionB();
  const float kSwitchTime = .2f;
  const float kDuration = .5f;

  const JointTarget j0 = a.At(0.f);
  rig.SetTarget(&j0);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  const JointTarget j_switch = a.At(kSwitchTime);
  rig.SetTarget(&j_switch);
  ASSERT_TRUE(rig.Job(kSwitchTime, 0.f).Run());

  // Switches to B, frozen residuals are computed from the delivered pose.
  const JointTarget jb0 = b.At(kSwitchTime);
  rig.SetTarget(&jb0);
  ASSERT_TRUE(rig.Job(0.f, kDuration).Run());

  // Frozen residuals and their derivatives, per component.
  const float xt0 = j_switch.translation.x - jb0.translation.x;
  const float yt0 = j_switch.translation.y - jb0.translation.y;
  const float zt0 = j_switch.translation.z - jb0.translation.z;
  const float vxt0 = a.v.x - b.v.x;
  const float vyt0 = a.v.y - b.v.y;
  const float vzt0 = a.v.z - b.v.z;
  const float xs0 = std::log(j_switch.scale.x) - std::log(jb0.scale.x);
  const float ys0 = std::log(j_switch.scale.y) - std::log(jb0.scale.y);
  const float zs0 = std::log(j_switch.scale.z) - std::log(jb0.scale.z);
  const float vxs0 = a.k.x - b.k.x;
  const float vys0 = a.k.y - b.k.y;
  const float vzs0 = a.k.z - b.k.z;
  // Rotations are all about Z: rotation vectors are (0, 0, angle).
  const float angle_a = .4f + .3f * kSwitchTime;
  const float angle_b0 = 1.f - .4f * kSwitchTime;
  const float xr0 = angle_a - angle_b0;
  const float vxr0 = .3f - (-.4f);

  // Unequal steps, targets are always the ones of the same absolute time.
  const float dts[] = {.13f, .07f, .2f, .06f, .1f};
  float tau = 0.f;
  float time = kSwitchTime;
  for (float dt : dts) {
    tau += dt;
    time += dt;
    const JointTarget jb = b.At(time);
    rig.SetTarget(&jb);
    ASSERT_TRUE(rig.Job(dt, 0.f).Run());
    const JointTarget out = rig.GetOutput(0);
    if (tau < kDuration) {
      const float u = tau / kDuration;
      float x, dx, y, dy, z, dz;
      Cubic(u, kDuration, xt0, vxt0, x, dx);
      Cubic(u, kDuration, yt0, vyt0, y, dy);
      Cubic(u, kDuration, zt0, vzt0, z, dz);
      ExpectFloat3Near(out.translation,
                       jb.translation + Float3(x, y, z), kPosTol);
      ExpectFloat3Near(out.vel_translation, b.v + Float3(dx, dy, dz), kVelTol);
      Cubic(u, kDuration, xs0, vxs0, x, dx);
      Cubic(u, kDuration, ys0, vys0, y, dy);
      Cubic(u, kDuration, zs0, vzs0, z, dz);
      ExpectFloat3Near(
          out.scale,
          Float3(jb.scale.x * std::exp(x), jb.scale.y * std::exp(y),
                 jb.scale.z * std::exp(z)),
          kPosTol);
      ExpectFloat3Near(out.vel_log_scale, b.k + Float3(dx, dy, dz), kVelTol);
      Cubic(u, kDuration, xr0, vxr0, x, dx);
      const float angle = 1.f - .4f * time + x;
      ExpectRotationNear(
          out.rotation, Quaternion::FromAxisAngle(Float3::z_axis(), angle),
          kRotTol);
      ExpectFloat3Near(out.vel_angular, Float3(0.f, 0.f, -.4f + dx), kVelTol);
    } else {
      // Transition is over: the output is the target, exactly.
      ExpectJointExact(out, jb);
    }
  }
}

TEST(InertializationCurve, UnequalStepsMatchSingleStep) {
  // Advancing a transition in one step or in unequal sub-steps (with
  // targets of the same absolute times) leads to the same output.
  Rig one_shot(1);
  Rig split(1);
  const Motion a = MotionA();
  const Motion b = MotionB();
  for (Rig* rig : {&one_shot, &split}) {
    const JointTarget j0 = a.At(0.f);
    rig->SetTarget(&j0);
    ASSERT_TRUE(rig->Job(0.f, 0.f).Run());
    const JointTarget j1 = a.At(.2f);
    rig->SetTarget(&j1);
    ASSERT_TRUE(rig->Job(.2f, 0.f).Run());
    const JointTarget jb = b.At(.2f);
    rig->SetTarget(&jb);
    ASSERT_TRUE(rig->Job(0.f, .5f).Run());
  }
  const JointTarget jb = b.At(.5f);
  one_shot.SetTarget(&jb);
  ASSERT_TRUE(one_shot.Job(.3f, 0.f).Run());
  const float dts[] = {.1f, .05f, .15f};
  float time = .2f;
  for (float dt : dts) {
    time += dt;
    const JointTarget jt = b.At(time);
    split.SetTarget(&jt);
    ASSERT_TRUE(split.Job(dt, 0.f).Run());
  }
  ExpectJointNear(one_shot.GetOutput(0), split.GetOutput(0), 1e-6f, 1e-6f,
                  1e-6f);
}

TEST(InertializationSwitch, InterruptMidTransition) {
  // A transition can be interrupted by a new switch: the residual is
  // re-captured from the last delivered pose and velocity.
  Rig rig(1);
  const Motion a = MotionA();
  const Motion b = MotionB();
  Motion c;
  c.p0 = Float3(.2f, -1.f, .5f);
  c.v = Float3(.2f, .1f, -.3f);
  c.q0 = Quaternion::FromAxisAngle(ozz::math::Normalize(Float3(1.f, 2.f, 3.f)),
                                   .9f);
  c.w = Float3(.2f, -.5f, .9f);
  c.s0 = Float3(1.2f, .7f, .9f);
  c.k = Float3(-.1f, .2f, .05f);

  JointTarget j = a.At(0.f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  j = a.At(.2f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(.2f, 0.f).Run());
  // First switch: A -> B.
  j = b.At(.2f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, .5f).Run());
  // Advances .2s into the .5s transition.
  j = b.At(.4f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(.2f, 0.f).Run());
  const JointTarget delivered = rig.GetOutput(0);
  // Interrupts with a second switch: B -> C. The switch call returns the
  // pre-switch pose and velocity, exactly.
  j = c.At(.4f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, .4f).Run());
  ExpectJointExact(rig.GetOutput(0), delivered);
  // A zero-time call right after the switch stays continuous.
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  ExpectJointNear(rig.GetOutput(0), delivered, kPosTol, kRotTol, kVelTol);
  // Advances to the end of the second transition: catches up with C.
  j = c.At(.4f + .3f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(.3f, 0.f).Run());
  j = c.At(.4f + .45f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(.15f, 0.f).Run());
  ExpectJointExact(rig.GetOutput(0), j);
}

// The spec rotation combination: the new target left-multiplies its initial
// pose by an X axis rotation (angular velocity (1,0,0)), while the
// pre-switch output is the target left-multiplied by a 90 degrees Z
// rotation, with angular velocity (0,1,0). The relative rotation derivative
// at the switch is zero, even though the raw angular velocity difference
// isn't.
struct ComboRig {
  ComboRig() {
    q_b0 = Quaternion::FromAxisAngle(
        ozz::math::Normalize(Float3(1.f, 2.f, 3.f)), .7f);
    // New target B: q(t) = Exp((t,0,0)) * q_b0, angular velocity (1,0,0).
    b.p0 = Float3(.2f, -.1f, .3f);
    b.v = Float3(.1f, .05f, -.2f);
    b.q0 = q_b0;
    b.w = Float3(1.f, 0.f, 0.f);
    b.s0 = Float3(1.f, 1.f, 1.f);
    b.k = Float3(.05f, .1f, 0.f);
  }

  // Pre-switch delivered pose: q_b0 left-multiplied by 90 degrees about Z.
  JointTarget SourceA(const Float3& _angular) const {
    JointTarget j;
    j.translation = Float3(.5f, .1f, -.2f);
    j.rotation =
        Quaternion::FromAxisAngle(Float3::z_axis(), ozz::math::kPi_2) * q_b0;
    j.scale = Float3(1.2f, .8f, 1.1f);
    j.vel_translation = Float3(-.3f, .1f, .2f);
    j.vel_angular = _angular;
    j.vel_log_scale = Float3(.1f, 0.f, -.1f);
    return j;
  }

  // Static child joint, identical in the source and the target.
  JointTarget Child() const {
    JointTarget j;
    j.translation = Float3(0.f, 1.f, 0.f);
    j.rotation = Quaternion::identity();
    j.scale = Float3(1.f, 1.f, 1.f);
    j.vel_translation = Float3(0.f, 0.f, 0.f);
    j.vel_angular = Float3(0.f, 0.f, 0.f);
    j.vel_log_scale = Float3(0.f, 0.f, 0.f);
    return j;
  }

  Quaternion q_b0;
  Motion b;
};

// Model-space-like child endpoint (parent translation + rotated child
// offset) and its analytic velocity, from the delivered parent output.
Float3 TipPosition(const JointTarget& _parent) {
  return _parent.translation + RotateVec(_parent.rotation, Float3(0.f, 1.f, 0.f));
}

Float3 TipVelocity(const JointTarget& _parent) {
  const Float3 offset =
      RotateVec(_parent.rotation, Float3(0.f, 1.f, 0.f));
  return _parent.vel_translation +
         ozz::math::Cross(_parent.vel_angular, offset);
}

TEST(InertializationRotation, NonCoaxialCombo) {
  const ComboRig combo;
  Rig rig(2);
  const JointTarget child = combo.Child();
  {
    const JointTarget joints[2] = {combo.SourceA(Float3(0.f, 1.f, 0.f)), child};
    rig.SetTarget(joints);
    ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  }
  const JointTarget delivered = rig.GetOutput(0);
  // Switches to B. The switch call returns the pre-switch output, exactly.
  {
    const JointTarget joints[2] = {combo.b.At(0.f), child};
    rig.SetTarget(joints);
    ASSERT_TRUE(rig.Job(0.f, .6f).Run());
    ExpectJointExact(rig.GetOutput(0), delivered);
    ExpectJointExact(rig.GetOutput(1), child);
    // Zero-time call: rotation, angular velocity and child endpoint
    // velocity are all continuous across the switch.
    ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
    const JointTarget out = rig.GetOutput(0);
    ExpectJointNear(out, delivered, kPosTol, kRotTol, kVelTol);
    ExpectFloat3Near(TipPosition(out), TipPosition(delivered), kPosTol);
    ExpectFloat3Near(TipVelocity(out), TipVelocity(delivered), kVelTol);
  }
  // Advances: no jump between consecutive frames. Bounds are loose sanity
  // thresholds: the residual angular rate reaches ~4 rad/s here, ie ~0.25
  // rad per 60ms frame.
  JointTarget prev = rig.GetOutput(0);
  float time = 0.f;
  for (int i = 0; i < 9; ++i) {
    time += .06f;
    const JointTarget joints[2] = {combo.b.At(time), child};
    rig.SetTarget(joints);
    ASSERT_TRUE(rig.Job(.06f, 0.f).Run());
    const JointTarget out = rig.GetOutput(0);
    ExpectJointNear(out, prev, .5f, .5f, 3.f);
    prev = out;
  }
  // The transition ends at .6s: the output catches up with the target.
  time += .1f;
  const JointTarget joints[2] = {combo.b.At(time), child};
  rig.SetTarget(joints);
  ASSERT_TRUE(rig.Job(.1f, 0.f).Run());
  ExpectJointExact(rig.GetOutput(0), joints[0]);
}

TEST(InertializationRotation, NonCoaxialComboZeroSourceAngular) {
  const ComboRig combo;
  Rig rig(2);
  const JointTarget child = combo.Child();
  {
    // Same combination, but the pre-switch angular velocity is zero.
    const JointTarget joints[2] = {combo.SourceA(Float3(0.f, 0.f, 0.f)), child};
    rig.SetTarget(joints);
    ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  }
  const JointTarget delivered = rig.GetOutput(0);
  const JointTarget joints[2] = {combo.b.At(0.f), child};
  rig.SetTarget(joints);
  ASSERT_TRUE(rig.Job(0.f, .6f).Run());
  ExpectJointExact(rig.GetOutput(0), delivered);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  const JointTarget out = rig.GetOutput(0);
  ExpectJointNear(out, delivered, kPosTol, kRotTol, kVelTol);
  ExpectFloat3Near(TipVelocity(out), TipVelocity(delivered), kVelTol);
  // Still catches up with the target at the end of the transition.
  const JointTarget end[2] = {combo.b.At(.7f), child};
  rig.SetTarget(end);
  ASSERT_TRUE(rig.Job(.7f, 0.f).Run());
  ExpectJointExact(rig.GetOutput(0), end[0]);
}

TEST(InertializationRotation, SwitchTargetMidTransition) {
  // Same combination, but the target is switched again (to another
  // non-coaxial motion) in the middle of the first transition.
  const ComboRig combo;
  Rig rig(2);
  const JointTarget child = combo.Child();
  {
    const JointTarget joints[2] = {combo.SourceA(Float3(0.f, 1.f, 0.f)), child};
    rig.SetTarget(joints);
    ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  }
  {
    const JointTarget joints[2] = {combo.b.At(0.f), child};
    rig.SetTarget(joints);
    ASSERT_TRUE(rig.Job(0.f, .6f).Run());
  }
  // Advances .25s into the .6s transition.
  float time = .25f;
  {
    const JointTarget joints[2] = {combo.b.At(time), child};
    rig.SetTarget(joints);
    ASSERT_TRUE(rig.Job(.25f, 0.f).Run());
  }
  const JointTarget delivered = rig.GetOutput(0);
  // Second target C, rotating about yet another axis.
  Motion c;
  c.p0 = Float3(-.3f, .2f, .1f);
  c.v = Float3(-.1f, .2f, .15f);
  c.q0 = Quaternion::FromAxisAngle(
      ozz::math::Normalize(Float3(-2.f, 1.f, .5f)), 1.1f);
  c.w = Float3(.2f, -.5f, .9f);
  c.s0 = Float3(.9f, 1.1f, 1.2f);
  c.k = Float3(.1f, -.05f, .1f);
  {
    const JointTarget joints[2] = {c.At(time), child};
    rig.SetTarget(joints);
    ASSERT_TRUE(rig.Job(0.f, .5f).Run());
    ExpectJointExact(rig.GetOutput(0), delivered);
    // Continuity of rotation, angular velocity and child endpoint velocity
    // across the second switch.
    ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
    const JointTarget out = rig.GetOutput(0);
    ExpectJointNear(out, delivered, kPosTol, kRotTol, kVelTol);
    ExpectFloat3Near(TipPosition(out), TipPosition(delivered), kPosTol);
    ExpectFloat3Near(TipVelocity(out), TipVelocity(delivered), kVelTol);
  }
  // The second transition ends at time .75: catches up with C.
  const JointTarget end[2] = {c.At(.8f), child};
  rig.SetTarget(end);
  ASSERT_TRUE(rig.Job(.55f, 0.f).Run());
  ExpectJointExact(rig.GetOutput(0), end[0]);
}

TEST(InertializationVelocity, MatchesMatrixTimeDifference) {
  // The delivered velocities are checked against time differences of the
  // delivered poses: rotation matrices are differenced to extract the
  // angular velocity, independently from the implementation.
  Rig rig(1);
  Motion a;
  a.p0 = Float3(.3f, -.2f, .1f);
  a.v = Float3(.4f, .1f, -.2f);
  a.q0 = Quaternion::FromAxisAngle(ozz::math::Normalize(Float3(1.f, 1.f, 0.f)),
                                   .5f);
  a.w = Float3(.2f, .4f, -.1f);
  a.s0 = Float3(1.1f, .9f, 1.f);
  a.k = Float3(.2f, -.1f, .05f);
  Motion b;
  b.p0 = Float3(-.4f, .3f, .5f);
  b.v = Float3(-.2f, .3f, .1f);
  b.q0 = Quaternion::FromAxisAngle(ozz::math::Normalize(Float3(0.f, 1.f, 1.f)),
                                   1.2f);
  b.w = Float3(.3f, -.7f, .5f);
  b.s0 = Float3(.9f, 1.2f, 1.1f);
  b.k = Float3(-.15f, .1f, .2f);

  const float kDuration = .8f;
  const float h = 2e-3f;
  const int kSteps = 450;  // tau goes past the transition end.

  JointTarget j = a.At(0.f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  j = a.At(.2f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(.2f, 0.f).Run());
  j = b.At(.2f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, kDuration).Run());

  ozz::vector<JointTarget> frames;
  frames.push_back(rig.GetOutput(0));  // frame 0, tau = 0
  float time = .2f;
  for (int i = 0; i < kSteps; ++i) {
    time += h;
    j = b.At(time);
    rig.SetTarget(&j);
    ASSERT_TRUE(rig.Job(h, 0.f).Run());
    frames.push_back(rig.GetOutput(0));
    ASSERT_TRUE(IsFinite(rig.GetOutput(0)));
  }

  for (int i = 2; i + 1 < static_cast<int>(frames.size()); ++i) {
    const float tau = i * h;
    // The curve isn't C2 at the transition end: central differences don't
    // apply to the frames straddling it.
    if (std::fabs(tau - kDuration) <= 2.f * h) {
      continue;
    }
    // Angular velocity from the rotation matrix central difference:
    // [w]x = dR/dt * R^t.
    float rm[3][3], rp[3][3], r0[3][3];
    QuatToMat3(frames[i - 1].rotation, rm);
    QuatToMat3(frames[i + 1].rotation, rp);
    QuatToMat3(frames[i].rotation, r0);
    float dr[3][3], r0t[3][3], w[3][3];
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        dr[r][c] = (rp[r][c] - rm[r][c]) / (2.f * h);
      }
    }
    Transpose33(r0, r0t);
    Mul33(dr, r0t, w);
    ExpectFloat3Near(frames[i].vel_angular,
                     Float3(w[2][1], w[0][2], w[1][0]), kVelTol);
    // Translation and log-scale velocities from central differences.
    const Float3 vt =
        (frames[i + 1].translation - frames[i - 1].translation) * (1.f / (2.f * h));
    ExpectFloat3Near(frames[i].vel_translation, vt, kVelTol);
    const Float3 vls(
        (std::log(frames[i + 1].scale.x) - std::log(frames[i - 1].scale.x)) /
            (2.f * h),
        (std::log(frames[i + 1].scale.y) - std::log(frames[i - 1].scale.y)) /
            (2.f * h),
        (std::log(frames[i + 1].scale.z) - std::log(frames[i - 1].scale.z)) /
            (2.f * h));
    ExpectFloat3Near(frames[i].vel_log_scale, vls, kVelTol);
  }
}

TEST(InertializationSignFlip, EquivalentQuaternionSign) {
  // q and -q are the same rotation: negating the target quaternions must
  // produce the same rotations and velocities.
  Rig rig(1);
  Rig negated(1);
  Motion a = MotionA();
  Motion b = MotionB();
  // Uses non-axis-aligned motions so rotations are fully 3D.
  a.q0 = Quaternion::FromAxisAngle(ozz::math::Normalize(Float3(1.f, 2.f, 3.f)),
                                   .8f);
  a.w = Float3(.3f, -.2f, .5f);
  b.q0 = Quaternion::FromAxisAngle(ozz::math::Normalize(Float3(-1.f, 1.f, 2.f)),
                                   1.3f);
  b.w = Float3(-.4f, .6f, .1f);
  Motion a_neg = a;
  a_neg.q0 = -a.q0;
  Motion b_neg = b;
  b_neg.q0 = -b.q0;

  const float kDuration = .5f;
  float time = 0.f;
  {
    JointTarget j = a.At(0.f);
    rig.SetTarget(&j);
    j = a_neg.At(0.f);
    negated.SetTarget(&j);
    ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
    ASSERT_TRUE(negated.Job(0.f, 0.f).Run());
  }
  {
    time += .2f;
    JointTarget j = a.At(time);
    rig.SetTarget(&j);
    j = a_neg.At(time);
    negated.SetTarget(&j);
    ASSERT_TRUE(rig.Job(.2f, 0.f).Run());
    ASSERT_TRUE(negated.Job(.2f, 0.f).Run());
    j = b.At(time);
    rig.SetTarget(&j);
    j = b_neg.At(time);
    negated.SetTarget(&j);
    ASSERT_TRUE(rig.Job(0.f, kDuration).Run());
    ASSERT_TRUE(negated.Job(0.f, kDuration).Run());
  }
  for (int i = 0; i < 8; ++i) {
    time += .1f;
    JointTarget j = b.At(time);
    rig.SetTarget(&j);
    j = b_neg.At(time);
    negated.SetTarget(&j);
    ASSERT_TRUE(rig.Job(.1f, 0.f).Run());
    ASSERT_TRUE(negated.Job(.1f, 0.f).Run());
    const JointTarget out = rig.GetOutput(0);
    const JointTarget out_neg = negated.GetOutput(0);
    ASSERT_TRUE(IsFinite(out_neg));
    ExpectRotationNear(out.rotation, out_neg.rotation, 1e-5f);
    ExpectFloat3Near(out.translation, out_neg.translation, 1e-6f);
    ExpectFloat3Near(out.scale, out_neg.scale, 1e-6f);
    ExpectFloat3Near(out.vel_angular, out_neg.vel_angular, 1e-5f);
    ExpectFloat3Near(out.vel_translation, out_neg.vel_translation, 1e-6f);
    ExpectFloat3Near(out.vel_log_scale, out_neg.vel_log_scale, 1e-6f);
  }
}

TEST(InertializationResidual, NearZeroResidual) {
  Rig rig(1);
  const Motion a = MotionA();
  // Target B is almost identical to the delivered pose: tiny residuals must
  // not produce NaN.
  JointTarget j = a.At(0.f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  Motion b = a;
  b.q0 = Quaternion::FromAxisAngle(Float3::x_axis(), 1e-7f) * a.q0;
  b.p0 = a.p0 + Float3(1e-9f, -1e-9f, 1e-9f);
  float time = 0.f;
  j = b.At(0.f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, .3f).Run());
  JointTarget prev = rig.GetOutput(0);
  const float dts[] = {.1f, .1f, .2f};
  for (float dt : dts) {
    time += dt;
    j = b.At(time);
    rig.SetTarget(&j);
    ASSERT_TRUE(rig.Job(dt, 0.f).Run());
    const JointTarget out = rig.GetOutput(0);
    ASSERT_TRUE(IsFinite(out));
    ExpectJointNear(out, prev, kPosTol + .1f, kRotTol + .1f, kVelTol + 1.f);
    prev = out;
  }
}

TEST(InertializationResidual, ZeroResidual) {
  Rig rig(1);
  const Motion a = MotionA();
  // Switches to the very same motion: the residual is exactly zero.
  JointTarget j = a.At(0.f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  j = a.At(.2f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(.2f, 0.f).Run());
  j = a.At(.2f);
  rig.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, .4f).Run());
  float time = .2f;
  const float dts[] = {.1f, .15f, .2f};
  for (float dt : dts) {
    time += dt;
    j = a.At(time);
    rig.SetTarget(&j);
    ASSERT_TRUE(rig.Job(dt, 0.f).Run());
    const JointTarget out = rig.GetOutput(0);
    ASSERT_TRUE(IsFinite(out));
    // A zero residual leaves the target (nearly) untouched.
    ExpectJointNear(out, j, 1e-6f, 1e-6f, 1e-5f);
  }
}

TEST(InertializationTailLane, FiveJointsInvalidLanes) {
  // 5 joints: the last SoA element has a single valid lane. Invalid lanes
  // are not validated, and their output values are preserved.
  const int kJoints = 5;
  Rig rig(kJoints);
  Rig control(kJoints);
  const Motion a = MotionA();
  const Motion b = MotionB();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float kSentinel = 12345.f;

  JointTarget joints[kJoints];
  for (int j = 0; j < kJoints; ++j) {
    joints[j] = a.At(.1f * j);
  }
  rig.SetTarget(joints);
  control.SetTarget(joints);

  // Poisons invalid lanes (lanes 1-3 of the second SoA element) of the
  // target with NaN.
  const auto poison_invalid_lanes = [&rig, &nan]() {
    for (int l = 1; l < 4; ++l) {
      SoaTransform& t = rig.target_pose[1];
      SetLaneRaw(t.translation.x, l, nan);
      SetLaneRaw(t.translation.y, l, nan);
      SetLaneRaw(t.translation.z, l, nan);
      SetLaneRaw(t.rotation.x, l, nan);
      SetLaneRaw(t.rotation.y, l, nan);
      SetLaneRaw(t.rotation.z, l, nan);
      SetLaneRaw(t.rotation.w, l, nan);
      SetLaneRaw(t.scale.x, l, nan);
      SetLaneRaw(t.scale.y, l, nan);
      SetLaneRaw(t.scale.z, l, nan);
      InertializationJob::Velocity& v = rig.target_vel[1];
      SetLaneRaw(v.translation.x, l, nan);
      SetLaneRaw(v.translation.y, l, nan);
      SetLaneRaw(v.translation.z, l, nan);
      SetLaneRaw(v.angular.x, l, nan);
      SetLaneRaw(v.angular.y, l, nan);
      SetLaneRaw(v.angular.z, l, nan);
      SetLaneRaw(v.log_scale.x, l, nan);
      SetLaneRaw(v.log_scale.y, l, nan);
      SetLaneRaw(v.log_scale.z, l, nan);
    }
  };
  poison_invalid_lanes();
  // Pre-fills output invalid lanes with a sentinel.
  for (int l = 1; l < 4; ++l) {
    SetLaneRaw(rig.out_pose[1].translation.x, l, kSentinel);
    SetLaneRaw(rig.out_pose[1].translation.y, l, kSentinel);
    SetLaneRaw(rig.out_pose[1].translation.z, l, kSentinel);
    SetLaneRaw(rig.out_pose[1].rotation.x, l, kSentinel);
    SetLaneRaw(rig.out_pose[1].rotation.y, l, kSentinel);
    SetLaneRaw(rig.out_pose[1].rotation.z, l, kSentinel);
    SetLaneRaw(rig.out_pose[1].rotation.w, l, kSentinel);
    SetLaneRaw(rig.out_pose[1].scale.x, l, kSentinel);
    SetLaneRaw(rig.out_pose[1].scale.y, l, kSentinel);
    SetLaneRaw(rig.out_pose[1].scale.z, l, kSentinel);
    SetLaneRaw(rig.out_vel[1].translation.x, l, kSentinel);
    SetLaneRaw(rig.out_vel[1].angular.z, l, kSentinel);
    SetLaneRaw(rig.out_vel[1].log_scale.y, l, kSentinel);
  }

  // NaN in invalid lanes doesn't fail validation.
  EXPECT_TRUE(rig.Job(0.f, 0.f).Validate());
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  ASSERT_TRUE(control.Job(0.f, 0.f).Run());

  // Runs a switch and some advances.
  for (int j = 0; j < kJoints; ++j) {
    joints[j] = b.At(.2f + .1f * j);
  }
  rig.SetTarget(joints);
  control.SetTarget(joints);
  poison_invalid_lanes();
  ASSERT_TRUE(rig.Job(0.f, .4f).Run());
  ASSERT_TRUE(control.Job(0.f, .4f).Run());
  for (int j = 0; j < kJoints; ++j) {
    joints[j] = b.At(.3f + .1f * j);
  }
  rig.SetTarget(joints);
  control.SetTarget(joints);
  poison_invalid_lanes();
  ASSERT_TRUE(rig.Job(.1f, 0.f).Run());
  ASSERT_TRUE(control.Job(.1f, 0.f).Run());

  // Valid joints match the control run, invalid output lanes are preserved.
  for (int j = 0; j < kJoints; ++j) {
    ExpectJointExact(rig.GetOutput(j), control.GetOutput(j));
  }
  for (int l = 1; l < 4; ++l) {
    EXPECT_EQ(LaneAt(rig.out_pose[1].translation.x, l), kSentinel);
    EXPECT_EQ(LaneAt(rig.out_pose[1].rotation.w, l), kSentinel);
    EXPECT_EQ(LaneAt(rig.out_pose[1].scale.y, l), kSentinel);
    EXPECT_EQ(LaneAt(rig.out_vel[1].translation.x, l), kSentinel);
    EXPECT_EQ(LaneAt(rig.out_vel[1].angular.z, l), kSentinel);
    EXPECT_EQ(LaneAt(rig.out_vel[1].log_scale.y, l), kSentinel);
  }
}

TEST(InertializationAliasing, InPlaceBuffers) {
  // Target pose can fully alias the output pose, and target velocity the
  // output velocity. Results are identical to using separate buffers.
  const int kJoints = 5;
  Rig control(kJoints);
  InertializationJob::Context context;
  context.Resize(kJoints);
  const int num_soa = (kJoints + 3) / 4;
  ozz::vector<SoaTransform> poses(num_soa);
  ozz::vector<InertializationJob::Velocity> vels(num_soa);
  const Motion a = MotionA();
  const Motion b = MotionB();

  JointTarget joints[kJoints];
  float time = 0.f;
  const float steps[] = {0.f, .2f, 0.f, .1f, .1f, .1f, .2f};
  const float durations[] = {0.f, 0.f, .5f, 0.f, 0.f, 0.f, 0.f};
  for (int step = 0; step < 7; ++step) {
    time += steps[step];
    const Motion& m = step < 2 ? a : b;
    for (int j = 0; j < kJoints; ++j) {
      joints[j] = m.At(time + .05f * j);
    }
    // Fills the aliased buffers with the new target (overwriting the
    // previous output), then runs in-place.
    for (int e = 0; e < num_soa; ++e) {
      const int count = kJoints - e * 4 < 4 ? kJoints - e * 4 : 4;
      poses[e] = PackPose(joints + e * 4, count);
      vels[e] = PackVelocity(joints + e * 4, count);
    }
    InertializationJob job;
    job.context = &context;
    job.target = {poses.data(), poses.size()};
    job.target_velocity = {vels.data(), vels.size()};
    job.output = {poses.data(), poses.size()};
    job.output_velocity = {vels.data(), vels.size()};
    job.delta_time = steps[step];
    job.transition_duration = durations[step];
    ASSERT_TRUE(job.Run());

    control.SetTarget(joints);
    ASSERT_TRUE(control.Job(steps[step], durations[step]).Run());
    for (int j = 0; j < kJoints; ++j) {
      ExpectJointExact(GetJoint(poses.data(), vels.data(), j),
                       control.GetOutput(j));
    }
  }
}

TEST(InertializationFailure, RetryAfterFailure) {
  // Failed calls leave the outputs and the context unchanged: the sequence
  // can be retried, and matches a control run that never saw the failures.
  Rig rig(1);
  Rig control(1);
  const Motion a = MotionA();
  const Motion b = MotionB();
  const float nan = std::numeric_limits<float>::quiet_NaN();

  JointTarget j = a.At(0.f);
  rig.SetTarget(&j);
  control.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  ASSERT_TRUE(control.Job(0.f, 0.f).Run());

  // Failing call: non-finite target.
  JointTarget bad = a.At(.1f);
  bad.translation.x = nan;
  rig.SetTarget(&bad);
  EXPECT_FALSE(rig.Job(.1f, 0.f).Run());
  ExpectJointExact(rig.GetOutput(0), control.GetOutput(0));

  // Failing call: illegal time.
  j = a.At(.1f);
  rig.SetTarget(&j);
  EXPECT_FALSE(rig.Job(-.1f, 0.f).Run());
  EXPECT_FALSE(rig.Job(.1f, .5f).Run());
  ExpectJointExact(rig.GetOutput(0), control.GetOutput(0));

  // Retried, now valid.
  control.SetTarget(&j);
  ASSERT_TRUE(rig.Job(.1f, 0.f).Run());
  ASSERT_TRUE(control.Job(.1f, 0.f).Run());
  ExpectJointExact(rig.GetOutput(0), control.GetOutput(0));

  // Failing switch request: non-unit quaternion.
  bad = b.At(.1f);
  bad.rotation = Quaternion(0.f, 0.f, 0.f, 2.f);
  rig.SetTarget(&bad);
  EXPECT_FALSE(rig.Job(0.f, .5f).Run());
  ExpectJointExact(rig.GetOutput(0), control.GetOutput(0));

  // Valid switch, then advance: still matches the control run.
  j = b.At(.1f);
  rig.SetTarget(&j);
  control.SetTarget(&j);
  ASSERT_TRUE(rig.Job(0.f, .5f).Run());
  ASSERT_TRUE(control.Job(0.f, .5f).Run());
  j = b.At(.3f);
  rig.SetTarget(&j);
  control.SetTarget(&j);
  ASSERT_TRUE(rig.Job(.2f, 0.f).Run());
  ASSERT_TRUE(control.Job(.2f, 0.f).Run());
  ExpectJointExact(rig.GetOutput(0), control.GetOutput(0));
}

TEST(InertializationContext, ResetAndResize) {
  Rig rig(2);
  const Motion a = MotionA();
  const Motion b = MotionB();
  JointTarget joints[2] = {a.At(0.f), a.At(.1f)};
  rig.SetTarget(joints);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  joints[0] = b.At(0.f);
  joints[1] = b.At(.1f);
  rig.SetTarget(joints);
  ASSERT_TRUE(rig.Job(0.f, .5f).Run());
  ASSERT_TRUE(rig.Job(.2f, 0.f).Run());

  // Reset clears the history: a transition cannot be requested anymore.
  rig.context.Reset();
  EXPECT_FALSE(rig.Job(0.f, .5f).Run());
  EXPECT_FALSE(rig.Job(.1f, 0.f).Run());
  // But a fresh initialization works, and behaves like a new context.
  Rig control(2);
  joints[0] = a.At(.3f);
  joints[1] = a.At(.4f);
  rig.SetTarget(joints);
  control.SetTarget(joints);
  ASSERT_TRUE(rig.Job(0.f, 0.f).Run());
  ASSERT_TRUE(control.Job(0.f, 0.f).Run());
  for (int j = 0; j < 2; ++j) {
    ExpectJointExact(rig.GetOutput(j), control.GetOutput(j));
  }

  // Resize changes the joint count and clears the history.
  rig.context.Resize(5);
  EXPECT_EQ(rig.context.num_joints(), 5);
  rig.num_joints = 5;
  rig.num_soa = 2;
  rig.target_pose.resize(2);
  rig.target_vel.resize(2);
  rig.out_pose.resize(2);
  rig.out_vel.resize(2);
  // The history was cleared: a transition cannot be requested.
  JointTarget joints5[5];
  for (int j = 0; j < 5; ++j) {
    joints5[j] = a.At(.1f * j);
  }
  rig.SetTarget(joints5);
  EXPECT_FALSE(rig.Job(0.f, .5f).Validate());
  EXPECT_FALSE(rig.Job(0.f, .5f).Run());
  // A fresh initialization works again.
  EXPECT_TRUE(rig.Job(0.f, 0.f).Run());
  for (int j = 0; j < 5; ++j) {
    ExpectJointExact(rig.GetOutput(j), joints5[j]);
  }
  rig.context.Resize(0);
  EXPECT_FALSE(rig.Job(0.f, 0.f).Validate());
}
