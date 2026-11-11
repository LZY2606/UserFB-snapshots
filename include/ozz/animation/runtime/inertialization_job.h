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

#ifndef OZZ_OZZ_ANIMATION_RUNTIME_INERTIALIZATION_JOB_H_
#define OZZ_OZZ_ANIMATION_RUNTIME_INERTIALIZATION_JOB_H_

#include "ozz/animation/runtime/export.h"
#include "ozz/base/containers/vector.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/platform.h"
#include "ozz/base/span.h"

namespace ozz {
namespace animation {

// ozz::animation::InertializationJob inserts an interruptible inertial
// transition layer between an existing blending output and a
// LocalToModelJob. When the animation target switches (a new action starts
// while a previous transition is still running), the job freezes the
// residual (position, log-scale and rotation offsets) and its time
// derivative between the last delivered pose and the new target, and decays
// it to exactly zero over a finite, deterministic duration. After a switch
// the job only reads the new target's current local pose and velocity: the
// animation that was left is never sampled again and does not keep any
// blending tree alive.
//
// The residual curve is a cubic Hermite spline. Noting x0 the residual and
// v0 its time derivative frozen at the switch instant, u = t / T the
// normalized time (t the elapsed time since the switch, T the transition
// duration), then for 0 <= t <= T:
//   x(t) = (2u^3 - 3u^2 + 1) * x0 + (u^3 - 2u^2 + u) * T * v0
// and the output velocity uses the analytic time derivative of this curve.
// For t >= T the residual and its derivative are exactly 0 and the output
// is the current target, verbatim.
//
// The residual is applied to the target as follows:
// - translation residual is added to the target translation.
// - log-scale residual is added to log(target scale), then exponentiated.
// - rotation residual r (a rotation vector, its length is the full rotation
//   angle) is applied as q_out = Exp(r) * q_target, using the library xyzw
//   quaternion convention.
//
// Time rules:
// - The very first call after Context creation (or Context::Reset) must use
//   delta_time = 0 and transition_duration = 0. The target is directly
//   adopted as the output, and the delivered pose and velocity are recorded.
// - Steady calls use transition_duration = 0 and delta_time >= 0. The
//   transition is advanced by delta_time seconds since the last successful
//   call; the input target pose and velocity must already be the ones of
//   this advanced time. Consecutive zero delta_time calls do not consume
//   transition time.
// - A switch is requested with transition_duration > 0 (delta_time must
//   then be 0). The residual is captured from the pose and velocity the
//   context last actually delivered, and this call still returns that very
//   same pre-switch pose and velocity. To switch at a later point in time,
//   first advance with the old target up to that time, then request the
//   switch with the new target of that same time.
// - A switch can be requested while a transition is still running: the
//   residual is re-captured from the last delivered pose and velocity (the
//   original source pose is not restored and velocities are not zeroed).
//
// Note that the shortest relative rotation is captured at each switch, and
// q and -q are considered the same rotation. The captured relative rotation
// angle, as well as the residual rotation angle during the whole
// transition, must stay below 3 radians: half-turn branch switches are not
// handled.
//
// Buffers:
// - Every input/output span must hold at least ceil(num_joints / 4)
//   elements. Only the actual joints are processed: invalid lanes of the
//   last SoA element are not validated, and the corresponding output values
//   are left untouched.
// - The target pose and the output pose can fully alias (same buffer), and
//   so can the target and output velocities. Results are then identical to
//   using independent buffers. Any other partial overlap is not supported.
//
// The job does not own any buffer (input/output) and will thus not delete
// them during job's destruction. Once the context is resized, Run() does
// not allocate nor deallocate heap memory.
struct OZZ_ANIMATION_DLL InertializationJob {
  // Validates job parameters. Returns true for a valid job, false otherwise:
  // -if context is nullptr or was not resized to a positive joint count.
  // -if any input/output range is smaller than ceil(num_joints / 4).
  // -if delta_time or transition_duration is not finite, if delta_time < 0,
  // if transition_duration < 0, or if transition_duration > 0 while
  // delta_time != 0.
  // -if the context was never initialized (first call) and delta_time or
  // transition_duration isn't 0. A transition cannot be requested before
  // the context delivered a first pose.
  // -if any valid joint of the target pose or velocity holds a non-finite
  // value, a non-positive scale, or a non-unit quaternion (squared length
  // differing from 1 by more than 1e-3).
  // Validate() has no side effect: it modifies neither the outputs nor the
  // context.
  bool Validate() const;

  // Runs the job's inertialization task.
  // The job is validated before any operation is performed, see Validate()
  // for more details. If validation fails, outputs and context are left
  // unchanged.
  // Returns false if *this job is not valid.
  bool Run() const;

  // Groups the 3 per-joint velocities, in SoA form, that describe the time
  // derivative of a local-space pose.
  struct Velocity {
    // Derivative of the joint local translation with respect to time, in
    // units per second.
    math::SoaFloat3 translation = math::SoaFloat3::zero();

    // Angular velocity expressed in the parent joint frame, in radians per
    // second. It is defined such that q(t + dt) = Exp(angular * dt) * q(t),
    // where the length of the Exp vector argument is the full rotation
    // angle.
    math::SoaFloat3 angular = math::SoaFloat3::zero();

    // Derivative of the natural logarithm of the (positive) local scale
    // with respect to time.
    math::SoaFloat3 log_scale = math::SoaFloat3::zero();
  };

  // Forward declares the context object used by the InertializationJob.
  class Context;

  // The context storing the per-character inertialization state (last
  // delivered pose and velocity, frozen residuals and transition time).
  // Must have been resized with the actual number of joints of the
  // character (not the SoA-padded capacity).
  Context* context = nullptr;

  // Job input: the target local-space pose (usually the output of a
  // SamplingJob or a BlendingJob) at the current (already advanced) time.
  span<const math::SoaTransform> target;

  // Job input: the target local-space velocities at the current time.
  span<const Velocity> target_velocity;

  // Job output: the inertialized local-space pose.
  span<math::SoaTransform> output;

  // Job output: the analytic time derivative of the output pose.
  span<Velocity> output_velocity;

  // Time advanced since the last successful call, in seconds. Must be 0
  // when requesting a transition (transition_duration > 0).
  float delta_time = 0.f;

  // Duration of the requested transition, in seconds. 0 means no switch is
  // requested (steady advance). When greater than 0, a new transition of
  // this duration starts from the last delivered pose and velocity.
  float transition_duration = 0.f;
};

// Declares the context object used by the InertializationJob to store the
// per-character transition state. Each character must own a dedicated
// context.
class OZZ_ANIMATION_DLL InertializationJob::Context {
 public:
  // Constructs an empty context. The context needs to be resized with the
  // actual number of joints before it can be used with an
  // InertializationJob.
  Context();

  // Disables copy and assignation.
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  Context(Context&&) = delete;
  Context& operator=(Context&&) = delete;

  // Deallocates the context.
  ~Context();

  // Resizes the context to the actual number of joints of the character
  // (not rounded up to the SoA lane count). This also clears any previously
  // recorded character history, as Reset() does.
  void Resize(int _num_joints);

  // Clears the recorded character history (last delivered pose and
  // velocity, frozen residuals, transition time). The next job call must
  // then be an initializing call (delta_time = 0, transition_duration = 0).
  void Reset();

  // The actual number of joints this context can process.
  int num_joints() const { return num_joints_; }

 private:
  friend struct InertializationJob;

  // The actual number of joints (not SoA aligned).
  int num_joints_ = 0;

  // Whether the context already delivered a first pose.
  bool initialized_ = false;

  // Elapsed time in the current transition, in seconds.
  float time_ = 0.f;

  // Duration of the current transition, in seconds. 0 means no transition
  // is running.
  float duration_ = 0.f;

  // Last pose and velocity actually delivered by the job.
  ozz::vector<math::SoaTransform> last_pose_;
  ozz::vector<Velocity> last_velocity_;

  // Residuals and their time derivatives, frozen at the last switch.
  ozz::vector<math::SoaFloat3> res_translation_;
  ozz::vector<math::SoaFloat3> res_translation_vel_;
  ozz::vector<math::SoaFloat3> res_log_scale_;
  ozz::vector<math::SoaFloat3> res_log_scale_vel_;
  ozz::vector<math::SoaFloat3> res_rotation_;
  ozz::vector<math::SoaFloat3> res_rotation_vel_;
};
}  // namespace animation
}  // namespace ozz
#endif  // OZZ_OZZ_ANIMATION_RUNTIME_INERTIALIZATION_JOB_H_
