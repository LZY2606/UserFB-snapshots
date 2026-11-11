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

#include "ozz/animation/runtime/inertialization_job.h"

#include <cmath>

#include "ozz/base/maths/math_ex.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/vec_float.h"

namespace ozz {
namespace animation {
namespace {

using math::Float3;
using math::Quaternion;
using math::SimdFloat4;

// Per-lane (scalar) pose, velocity and residual representations. The job
// processes actual joints one by one, so invalid lanes of the last SoA
// element are never read nor written.
struct LanePose {
  Float3 translation;
  Quaternion rotation;
  Float3 scale;
};

struct LaneVelocity {
  Float3 translation;
  Float3 angular;
  Float3 log_scale;
};

struct LaneResidual {
  Float3 translation;
  Float3 translation_vel;
  Float3 log_scale;
  Float3 log_scale_vel;
  Float3 rotation;
  Float3 rotation_vel;
};

float GetLane(const SimdFloat4& _v, int _lane) {
  switch (_lane) {
    case 0:
      return math::GetX(_v);
    case 1:
      return math::GetY(_v);
    case 2:
      return math::GetZ(_v);
    default:
      return math::GetW(_v);
  }
}

void SetLane(SimdFloat4& _v, int _lane, float _f) {
  float f[4] = {math::GetX(_v), math::GetY(_v), math::GetZ(_v),
                math::GetW(_v)};
  f[_lane] = _f;
  _v = math::simd_float4::Load(f[0], f[1], f[2], f[3]);
}

LanePose GetPose(const math::SoaTransform& _t, int _lane) {
  const LanePose p = {{GetLane(_t.translation.x, _lane),
                       GetLane(_t.translation.y, _lane),
                       GetLane(_t.translation.z, _lane)},
                      {GetLane(_t.rotation.x, _lane), GetLane(_t.rotation.y, _lane),
                       GetLane(_t.rotation.z, _lane), GetLane(_t.rotation.w, _lane)},
                      {GetLane(_t.scale.x, _lane), GetLane(_t.scale.y, _lane),
                       GetLane(_t.scale.z, _lane)}};
  return p;
}

void SetPose(math::SoaTransform& _t, int _lane, const LanePose& _p) {
  SetLane(_t.translation.x, _lane, _p.translation.x);
  SetLane(_t.translation.y, _lane, _p.translation.y);
  SetLane(_t.translation.z, _lane, _p.translation.z);
  SetLane(_t.rotation.x, _lane, _p.rotation.x);
  SetLane(_t.rotation.y, _lane, _p.rotation.y);
  SetLane(_t.rotation.z, _lane, _p.rotation.z);
  SetLane(_t.rotation.w, _lane, _p.rotation.w);
  SetLane(_t.scale.x, _lane, _p.scale.x);
  SetLane(_t.scale.y, _lane, _p.scale.y);
  SetLane(_t.scale.z, _lane, _p.scale.z);
}

LaneVelocity GetVelocity(const InertializationJob::Velocity& _v, int _lane) {
  const LaneVelocity v = {{GetLane(_v.translation.x, _lane),
                           GetLane(_v.translation.y, _lane),
                           GetLane(_v.translation.z, _lane)},
                          {GetLane(_v.angular.x, _lane), GetLane(_v.angular.y, _lane),
                           GetLane(_v.angular.z, _lane)},
                          {GetLane(_v.log_scale.x, _lane),
                           GetLane(_v.log_scale.y, _lane),
                           GetLane(_v.log_scale.z, _lane)}};
  return v;
}

void SetVelocity(InertializationJob::Velocity& _v, int _lane,
                 const LaneVelocity& _s) {
  SetLane(_v.translation.x, _lane, _s.translation.x);
  SetLane(_v.translation.y, _lane, _s.translation.y);
  SetLane(_v.translation.z, _lane, _s.translation.z);
  SetLane(_v.angular.x, _lane, _s.angular.x);
  SetLane(_v.angular.y, _lane, _s.angular.y);
  SetLane(_v.angular.z, _lane, _s.angular.z);
  SetLane(_v.log_scale.x, _lane, _s.log_scale.x);
  SetLane(_v.log_scale.y, _lane, _s.log_scale.y);
  SetLane(_v.log_scale.z, _lane, _s.log_scale.z);
}

// Bundles the context residual buffers, so that helpers below don't need
// access to Context private members.
struct ResidualBuffers {
  math::SoaFloat3* translation;
  math::SoaFloat3* translation_vel;
  math::SoaFloat3* log_scale;
  math::SoaFloat3* log_scale_vel;
  math::SoaFloat3* rotation;
  math::SoaFloat3* rotation_vel;
};

// Returns the quaternion Exp of rotation vector _r. The length of _r is the
// full rotation angle.
Quaternion Exp(const Float3& _r) {
  const float angle_sq = math::LengthSqr(_r);
  if (angle_sq < 1e-16f) {
    // q = (r / 2, 1) at the first order, normalized. Error is O(angle^2).
    return math::Normalize(
        Quaternion(_r.x * .5f, _r.y * .5f, _r.z * .5f, 1.f));
  }
  const float angle = std::sqrt(angle_sq);
  const float half_angle = .5f * angle;
  const float s = std::sin(half_angle) / angle;
  return Quaternion(_r.x * s, _r.y * s, _r.z * s, std::cos(half_angle));
}

// Returns the shortest rotation vector (log) of unit quaternion _q. The
// length of the returned vector is the full rotation angle, in [0, pi].
Float3 Log(const Quaternion& _q) {
  // q and -q are the same rotation: selects the shortest one.
  const Quaternion q = _q.w < 0.f ? -_q : _q;
  const float sin_half_sq = q.x * q.x + q.y * q.y + q.z * q.z;
  if (sin_half_sq < 1e-16f) {
    // Small angle approximation, error is O(angle^2).
    return Float3(2.f * q.x, 2.f * q.y, 2.f * q.z);
  }
  const float sin_half = std::sqrt(sin_half_sq);
  const float scale = 2.f * std::atan2(sin_half, q.w) / sin_half;
  return Float3(q.x * scale, q.y * scale, q.z * scale);
}

// Multiplies _v by the left Jacobian of the SO(3) exponential map at _r:
// Jl(r) = I + a [r]x + b [r]x^2, with a = (1 - cos(t)) / t^2 and
// b = (t - sin(t)) / t^3, t being the length of _r.
Float3 LeftJacobianMul(const Float3& _r, const Float3& _v) {
  const float theta_sq = math::LengthSqr(_r);
  float a, b;
  if (theta_sq < 1e-8f) {
    // Series expansions around 0.
    a = .5f - theta_sq / 24.f;
    b = 1.f / 6.f - theta_sq / 120.f;
  } else {
    const float theta = std::sqrt(theta_sq);
    a = (1.f - std::cos(theta)) / theta_sq;
    b = (theta - std::sin(theta)) / (theta_sq * theta);
  }
  const Float3 rxv = math::Cross(_r, _v);
  const Float3 rxrxv = math::Cross(_r, rxv);
  return _v + rxv * a + rxrxv * b;
}

// Multiplies _v by the inverse of the left Jacobian of the SO(3)
// exponential map at _r: Jl(r)^-1 = I - 1/2 [r]x + c [r]x^2, with
// c = 1 / t^2 - (1 + cos(t)) / (2 t sin(t)), t being the length of _r.
Float3 LeftJacobianInvMul(const Float3& _r, const Float3& _v) {
  const float theta_sq = math::LengthSqr(_r);
  float c;
  if (theta_sq < 1e-8f) {
    // Series expansion around 0.
    c = 1.f / 12.f + theta_sq / 720.f;
  } else {
    const float theta = std::sqrt(theta_sq);
    c = 1.f / theta_sq -
        (1.f + std::cos(theta)) / (2.f * theta * std::sin(theta));
  }
  const Float3 rxv = math::Cross(_r, _v);
  const Float3 rxrxv = math::Cross(_r, rxv);
  return _v - rxv * .5f + rxrxv * c;
}

Float3 Log3(const Float3& _v) {
  return Float3(std::log(_v.x), std::log(_v.y), std::log(_v.z));
}

Float3 Exp3(const Float3& _v) {
  return Float3(std::exp(_v.x), std::exp(_v.y), std::exp(_v.z));
}

// Freezes the residual and its time derivative between the last delivered
// pose/velocity and the new target.
void Capture(const LanePose& _last, const LaneVelocity& _last_vel,
             const LanePose& _target, const LaneVelocity& _target_vel,
             LaneResidual& _res) {
  _res.translation = _last.translation - _target.translation;
  _res.translation_vel = _last_vel.translation - _target_vel.translation;
  _res.log_scale = Log3(_last.scale) - Log3(_target.scale);
  _res.log_scale_vel = _last_vel.log_scale - _target_vel.log_scale;

  // Rotation residual r0 such that q_last = Exp(r0) * q_target, using the
  // shortest relative rotation.
  const Quaternion rel =
      math::Normalize(_last.rotation * math::Conjugate(_target.rotation));
  _res.rotation = Log(rel);

  // The output pose is q_out(t) = Exp(r(t)) * q_target(t). Its parent-frame
  // angular velocity is w_out = Jl(r) * dr/dt + Rot(Exp(r)) * w_target.
  // dr/dt at t = 0 is solved so that w_out matches the last delivered
  // angular velocity. Note that w_last - w_target is not the right initial
  // derivative: the residual is a left-multiplied rotation, so the target
  // angular velocity must first be rotated into the residual frame.
  const Float3 w_residual =
      _last_vel.angular - math::TransformVector(rel, _target_vel.angular);
  _res.rotation_vel = LeftJacobianInvMul(_res.rotation, w_residual);
}

// Evaluates the cubic residual curve and its analytic derivative at
// normalized time _u of a transition of _duration seconds, and applies them
// to the target pose and velocity.
void Evaluate(const LaneResidual& _res, float _u, float _duration,
              const LanePose& _target, const LaneVelocity& _target_vel,
              LanePose& _out, LaneVelocity& _out_vel) {
  const float u2 = _u * _u;
  const float u3 = u2 * _u;
  // Cubic Hermite basis: x(u) = h00 * x0 + h10 * T * v0.
  const float h00 = 2.f * u3 - 3.f * u2 + 1.f;
  const float h10 = (u3 - 2.f * u2 + _u) * _duration;
  // Analytic time derivatives: dx/dt = d00 * x0 + d10 * v0.
  const float d00 = (6.f * u2 - 6.f * _u) / _duration;
  const float d10 = 3.f * u2 - 4.f * _u + 1.f;

  const Float3 xt = _res.translation * h00 + _res.translation_vel * h10;
  const Float3 dxt = _res.translation * d00 + _res.translation_vel * d10;
  _out.translation = _target.translation + xt;
  _out_vel.translation = _target_vel.translation + dxt;

  const Float3 xs = _res.log_scale * h00 + _res.log_scale_vel * h10;
  const Float3 dxs = _res.log_scale * d00 + _res.log_scale_vel * d10;
  _out.scale = Exp3(Log3(_target.scale) + xs);
  _out_vel.log_scale = _target_vel.log_scale + dxs;

  const Float3 r = _res.rotation * h00 + _res.rotation_vel * h10;
  const Float3 dr = _res.rotation * d00 + _res.rotation_vel * d10;
  const Quaternion e = math::Normalize(Exp(r));
  _out.rotation = math::Normalize(e * _target.rotation);
  _out_vel.angular =
      LeftJacobianMul(r, dr) + math::TransformVector(e, _target_vel.angular);
}

bool IsFinite(const Float3& _v) {
  return std::isfinite(_v.x) && std::isfinite(_v.y) && std::isfinite(_v.z);
}

bool IsFinite(const Quaternion& _q) {
  return std::isfinite(_q.x) && std::isfinite(_q.y) && std::isfinite(_q.z) &&
         std::isfinite(_q.w);
}

LaneResidual GetResidual(const ResidualBuffers& _buf, int _soa, int _lane) {
  const LaneResidual r = {
      {GetLane(_buf.translation[_soa].x, _lane),
       GetLane(_buf.translation[_soa].y, _lane),
       GetLane(_buf.translation[_soa].z, _lane)},
      {GetLane(_buf.translation_vel[_soa].x, _lane),
       GetLane(_buf.translation_vel[_soa].y, _lane),
       GetLane(_buf.translation_vel[_soa].z, _lane)},
      {GetLane(_buf.log_scale[_soa].x, _lane),
       GetLane(_buf.log_scale[_soa].y, _lane),
       GetLane(_buf.log_scale[_soa].z, _lane)},
      {GetLane(_buf.log_scale_vel[_soa].x, _lane),
       GetLane(_buf.log_scale_vel[_soa].y, _lane),
       GetLane(_buf.log_scale_vel[_soa].z, _lane)},
      {GetLane(_buf.rotation[_soa].x, _lane),
       GetLane(_buf.rotation[_soa].y, _lane),
       GetLane(_buf.rotation[_soa].z, _lane)},
      {GetLane(_buf.rotation_vel[_soa].x, _lane),
       GetLane(_buf.rotation_vel[_soa].y, _lane),
       GetLane(_buf.rotation_vel[_soa].z, _lane)}};
  return r;
}

void SetResidual(ResidualBuffers& _buf, int _soa, int _lane,
                 const LaneResidual& _r) {
  SetLane(_buf.translation[_soa].x, _lane, _r.translation.x);
  SetLane(_buf.translation[_soa].y, _lane, _r.translation.y);
  SetLane(_buf.translation[_soa].z, _lane, _r.translation.z);
  SetLane(_buf.translation_vel[_soa].x, _lane, _r.translation_vel.x);
  SetLane(_buf.translation_vel[_soa].y, _lane, _r.translation_vel.y);
  SetLane(_buf.translation_vel[_soa].z, _lane, _r.translation_vel.z);
  SetLane(_buf.log_scale[_soa].x, _lane, _r.log_scale.x);
  SetLane(_buf.log_scale[_soa].y, _lane, _r.log_scale.y);
  SetLane(_buf.log_scale[_soa].z, _lane, _r.log_scale.z);
  SetLane(_buf.log_scale_vel[_soa].x, _lane, _r.log_scale_vel.x);
  SetLane(_buf.log_scale_vel[_soa].y, _lane, _r.log_scale_vel.y);
  SetLane(_buf.log_scale_vel[_soa].z, _lane, _r.log_scale_vel.z);
  SetLane(_buf.rotation[_soa].x, _lane, _r.rotation.x);
  SetLane(_buf.rotation[_soa].y, _lane, _r.rotation.y);
  SetLane(_buf.rotation[_soa].z, _lane, _r.rotation.z);
  SetLane(_buf.rotation_vel[_soa].x, _lane, _r.rotation_vel.x);
  SetLane(_buf.rotation_vel[_soa].y, _lane, _r.rotation_vel.y);
  SetLane(_buf.rotation_vel[_soa].z, _lane, _r.rotation_vel.z);
}

}  // namespace

InertializationJob::Context::Context() = default;
InertializationJob::Context::~Context() = default;

void InertializationJob::Context::Resize(int _num_joints) {
  num_joints_ = _num_joints > 0 ? _num_joints : 0;
  const size_t num_soa = static_cast<size_t>(num_joints_ + 3) / 4;
  last_pose_.resize(num_soa);
  last_velocity_.resize(num_soa);
  res_translation_.resize(num_soa);
  res_translation_vel_.resize(num_soa);
  res_log_scale_.resize(num_soa);
  res_log_scale_vel_.resize(num_soa);
  res_rotation_.resize(num_soa);
  res_rotation_vel_.resize(num_soa);
  Reset();
}

void InertializationJob::Context::Reset() {
  initialized_ = false;
  time_ = 0.f;
  duration_ = 0.f;
  const math::SoaTransform identity = math::SoaTransform::identity();
  for (auto& pose : last_pose_) {
    pose = identity;
  }
  for (auto& velocity : last_velocity_) {
    velocity = Velocity{};
  }
  const math::SoaFloat3 zero = math::SoaFloat3::zero();
  for (int i = 0; i < static_cast<int>(res_translation_.size()); ++i) {
    res_translation_[i] = zero;
    res_translation_vel_[i] = zero;
    res_log_scale_[i] = zero;
    res_log_scale_vel_[i] = zero;
    res_rotation_[i] = zero;
    res_rotation_vel_[i] = zero;
  }
}

bool InertializationJob::Validate() const {
  if (!context) {
    return false;
  }
  const int num_joints = context->num_joints_;
  if (num_joints <= 0) {
    return false;
  }
  const size_t num_soa = static_cast<size_t>(num_joints + 3) / 4;
  if (target.size() < num_soa || target_velocity.size() < num_soa ||
      output.size() < num_soa || output_velocity.size() < num_soa) {
    return false;
  }
  if (!std::isfinite(delta_time) || !std::isfinite(transition_duration)) {
    return false;
  }
  if (delta_time < 0.f || transition_duration < 0.f) {
    return false;
  }
  // A switch is an instantaneous request: it cannot advance time.
  if (transition_duration > 0.f && delta_time != 0.f) {
    return false;
  }
  // The first call must adopt the target directly. A transition cannot be
  // requested before the context delivered a first pose.
  if (!context->initialized_ &&
      (delta_time != 0.f || transition_duration != 0.f)) {
    return false;
  }

  // Per-joint input validation. Invalid lanes of the last SoA element are
  // not part of the validation.
  for (int j = 0; j < num_joints; ++j) {
    const int soa = j >> 2;
    const int lane = j & 3;
    const LanePose pose = GetPose(target[soa], lane);
    if (!IsFinite(pose.translation) || !IsFinite(pose.rotation) ||
        !IsFinite(pose.scale)) {
      return false;
    }
    if (pose.scale.x <= 0.f || pose.scale.y <= 0.f || pose.scale.z <= 0.f) {
      return false;
    }
    const float norm_sq = pose.rotation.x * pose.rotation.x +
                          pose.rotation.y * pose.rotation.y +
                          pose.rotation.z * pose.rotation.z +
                          pose.rotation.w * pose.rotation.w;
    if (std::abs(norm_sq - 1.f) > 1e-3f) {
      return false;
    }
    const LaneVelocity velocity = GetVelocity(target_velocity[soa], lane);
    if (!IsFinite(velocity.translation) || !IsFinite(velocity.angular) ||
        !IsFinite(velocity.log_scale)) {
      return false;
    }
  }
  return true;
}

bool InertializationJob::Run() const {
  if (!Validate()) {
    return false;
  }
  Context& ctx = *context;
  const int num_joints = ctx.num_joints_;
  const int num_soa = (num_joints + 3) / 4;
  ResidualBuffers residuals = {ctx.res_translation_.data(),
                               ctx.res_translation_vel_.data(),
                               ctx.res_log_scale_.data(),
                               ctx.res_log_scale_vel_.data(),
                               ctx.res_rotation_.data(),
                               ctx.res_rotation_vel_.data()};

  if (!ctx.initialized_) {
    // First use: the target is directly adopted as the output, and recorded
    // as the last delivered pose and velocity.
    for (int e = 0; e < num_soa; ++e) {
      const int valid = math::Min(num_joints - e * 4, 4);
      for (int l = 0; l < valid; ++l) {
        const LanePose pose = GetPose(target[e], l);
        const LaneVelocity velocity = GetVelocity(target_velocity[e], l);
        SetPose(output[e], l, pose);
        SetVelocity(output_velocity[e], l, velocity);
        SetPose(ctx.last_pose_[e], l, pose);
        SetVelocity(ctx.last_velocity_[e], l, velocity);
      }
    }
    ctx.initialized_ = true;
    ctx.time_ = 0.f;
    ctx.duration_ = 0.f;
    return true;
  }

  if (transition_duration > 0.f) {
    // Switch request: freezes the residual between the last delivered pose
    // and the new target. This call still delivers the pre-switch pose and
    // velocity.
    for (int e = 0; e < num_soa; ++e) {
      const int valid = math::Min(num_joints - e * 4, 4);
      for (int l = 0; l < valid; ++l) {
        const LanePose last = GetPose(ctx.last_pose_[e], l);
        const LaneVelocity last_vel = GetVelocity(ctx.last_velocity_[e], l);
        LaneResidual residual;
        Capture(last, last_vel, GetPose(target[e], l),
                GetVelocity(target_velocity[e], l), residual);
        SetResidual(residuals, e, l, residual);
        SetPose(output[e], l, last);
        SetVelocity(output_velocity[e], l, last_vel);
      }
    }
    ctx.time_ = 0.f;
    ctx.duration_ = transition_duration;
    return true;
  }

  // Steady advance.
  ctx.time_ += delta_time;
  const bool active = ctx.duration_ > 0.f && ctx.time_ < ctx.duration_;
  if (!active) {
    // The transition is over (or none was running): residual and derivative
    // are exactly 0, the output is the current target.
    ctx.duration_ = 0.f;
  }
  const float u = active ? ctx.time_ / ctx.duration_ : 0.f;
  for (int e = 0; e < num_soa; ++e) {
    const int valid = math::Min(num_joints - e * 4, 4);
    for (int l = 0; l < valid; ++l) {
      LanePose pose;
      LaneVelocity velocity;
      if (active) {
        Evaluate(GetResidual(residuals, e, l), u, ctx.duration_,
                 GetPose(target[e], l), GetVelocity(target_velocity[e], l),
                 pose, velocity);
      } else {
        pose = GetPose(target[e], l);
        velocity = GetVelocity(target_velocity[e], l);
      }
      SetPose(output[e], l, pose);
      SetVelocity(output_velocity[e], l, velocity);
      SetPose(ctx.last_pose_[e], l, pose);
      SetVelocity(ctx.last_velocity_[e], l, velocity);
    }
  }
  return true;
}
}  // namespace animation
}  // namespace ozz
