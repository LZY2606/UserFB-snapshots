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

#include <cstdio>
#include <limits>

#include "contact_lock.h"
#include "framework/application.h"
#include "framework/imgui.h"
#include "framework/mesh.h"
#include "framework/renderer.h"
#include "framework/utils.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/ik_aim_job.h"
#include "ozz/animation/runtime/ik_two_bone_job.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/track.h"
#include "ozz/base/io/stream.h"
#include "ozz/base/log.h"
#include "ozz/base/maths/box.h"
#include "ozz/base/maths/math_ex.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/simd_quaternion.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/memory/unique_ptr.h"
#include "ozz/options/options.h"

using ozz::sample::ContactLock;

// Skeleton archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(skeleton,
                           "Path to the skeleton (ozz archive format).",
                           "media/skeleton.ozz", false)
// Animation archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(animation,
                           "Path to the animation (ozz archive format).",
                           "media/animation.ozz", false)

// Mesh archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(mesh,
                           "Path to the skinned mesh (ozz archive format).",
                           "media/mesh.ozz", false)

// Mesh archive can be specified as an option.
OZZ_OPTIONS_DECLARE_STRING(floor,
                           "Path to the floor mesh (ozz archive format).",
                           "media/floor.ozz", false)

// Optional contact tracks (ozz float track archives). A value greater than .5
// means the foot is supporting, any other value means it is swinging. When no
// track is provided, the sample keeps its original per-frame ground search.
OZZ_OPTIONS_DECLARE_STRING(left_contact,
                           "Path to the left foot contact track (optional).",
                           "", false)
OZZ_OPTIONS_DECLARE_STRING(right_contact,
                           "Path to the right foot contact track (optional).",
                           "", false)

// Optional path to a debug CSV logging contact state, anchors and ankles.
OZZ_OPTIONS_DECLARE_STRING(contact_csv,
                           "Writes the contact-lock debug CSV at that path "
                           "(optional, only used with contact tracks).",
                           "", false)

const char* kLeftJointNames[] = {"LeftUpLeg", "LeftLeg", "LeftFoot"};
const char* kRightJointNames[] = {"RightUpLeg", "RightLeg", "RightFoot"};

const ozz::math::SimdFloat4 kKneeAxis = ozz::math::simd_float4::z_axis();

const ozz::math::SimdFloat4 kAnkleForward = -ozz::math::simd_float4::x_axis();
const ozz::math::SimdFloat4 kAnkleUp = ozz::math::simd_float4::y_axis();

// Structure used to store each leg setup data.
struct LegSetup {
  int hip;
  int knee;
  int ankle;
};

struct LegRayInfo {
  ozz::math::Float3 start;
  ozz::math::Float3 dir;

  bool hit;
  ozz::math::Float3 hit_point;
  ozz::math::Float3 hit_normal;
};

// Constants
static const ozz::math::Float3 kDown(0.f, -1.f, 0.f);
static const ozz::math::Float3 kCharacterRayHeightOffset(0.f, 10.f, 0.f);
static const ozz::math::Float3 kFootRayHeightOffset(0.f, .5f, 0.f);

// Computes ankle target position (C), so that the foot is in contact with the
// floor. Because of floor slope (defined by raycast intersection normal),
// ankle position cannot be simply be offseted by foot offset. See geogebra
// diagram for more details: media/doc/samples/foot_ik_ankle.ggb
static ozz::math::Float3 ComputeAnkleTarget(const LegRayInfo& _ray,
                                            float _foot_height) {
  // Computes projection of the ray AI (from start to floor intersection
  // point) onto floor normal. This gives the length of segment AB.
  // Note that ray.hit_normal is normalized already.
  const float ABl = Dot(_ray.start - _ray.hit_point, _ray.hit_normal);

  // Knowing A, AB length and direction, we can compute B position.
  const ozz::math::Float3 B = _ray.start - _ray.hit_normal * ABl;

  // Computes sebgment IB and its length (IBl)
  const ozz::math::Float3 IB = B - _ray.hit_point;
  const float IBl = Length(IB);

  if (IBl <= 0.f) {
    // If B is at raycast intersection (I), then the corrected ankle
    // position (world-space) only takes foot height into account.
    return _ray.hit_point + _ray.hit_normal * _foot_height;
  }

  // HC length is known (as foot height). So we're using Thales theorem to
  // find H position.
  const float IHl = IBl * _foot_height / ABl;
  const ozz::math::Float3 IH = IB * (IHl / IBl);
  const ozz::math::Float3 H = _ray.hit_point + IH;

  // C (Corrected ankle) position can now be found.
  return H + _ray.hit_normal * _foot_height;
}

class FootIKSampleApplication : public ozz::sample::Application {
 protected:
  // Updates current animation time and foot ik.
  virtual bool OnUpdate(float _dt, float) {
    // 1. Updates character main animation.
    if (!UpdateBaseAnimation(_dt)) {
      return false;
    }

    // 2. Finds character height on the floor, evaluted at its root position.
    if (!UpdateCharacterHeight()) {
      return false;
    }

    // 3. For each leg, raycasts a vector going down from the ankle position.
    // This allows to find the intersection point with the floor.
    if (!RaycastLegs()) {
      return false;
    }

    // 4. Contact-track layer (optional): fixes supporting feet to the world
    // anchor captured at their contact rising edge. Without contact tracks,
    // this is a no-op and the sample behaves exactly as before.
    if (!UpdateContactLocks()) {
      return false;
    }

    // 5. Computes targetted ankles positions, taking floor steepness and foot
    // height in consideration.
    if (!UpdateAnklesTarget()) {
      return false;
    }

    // 6. Offsets the character down, so that the lowest ankle (lowest from its
    // original position) reaches its targetted position. The other leg(s) will
    // be ik-ed.
    if (!UpdatePelvisOffset()) {
      return false;
    }

    // 7. Updates legs and ankles transforms, so they reach thei targetted
    // position and orientation.
    if (!UpdateFootIK()) {
      return false;
    }

    WriteContactCsv();

    // Remembers the root of this frame for next update's edge-time root
    // interpolation.
    previous_root_translation_ = root_translation_;
    previous_root_yaw_ = root_yaw_;

    return true;
  }

  // Raycast down from the current position to find character height on the
  // floor. It directly updates root translation as output.
  bool UpdateCharacterHeight() {
    if (!auto_character_height_) {
      return true;
    }

    // Starts the ray from above (kCharacterRayHeightOffset) current character
    // position.
    ozz::sample::RayIntersectsMeshes(
        root_translation_ + kCharacterRayHeightOffset, kDown,
        make_span(floors_), &root_translation_, nullptr);

    return true;
  }

  bool UpdateBaseAnimation(float _dt) {
    // Updates current animation time.
    controller_.Update(animation_, _dt);

    // Maintains the un-wrapped (un-modded) playback ratio used by the contact
    // layer, so an update crossing a loop still reports every edge.
    previous_playback_ratio_ = playback_ratio_;
    const float advanced =
        _dt * controller_.playback_speed() / animation_.duration();
    if (advanced < 0.f) {
      // Backward playback: past states are not reconstructed. Locks are reset
      // and the ratio stays synced on the wrapped controller time, so the
      // contact update range is empty while playing backward.
      contact_lock_.Reset();
      playback_ratio_ = controller_.time_ratio();
      previous_playback_ratio_ = playback_ratio_;
    } else {
      playback_ratio_ += advanced;
      // With looping disabled the controller clamps time in [0,1]: the
      // un-wrapped ratio must not grow past the animation end either.
      if (!controller_.loop()) {
        playback_ratio_ = ozz::math::Clamp(0.f, playback_ratio_, 1.f);
      }
    }

    // Samples optimized animation at t = animation_time.
    ozz::animation::SamplingJob sampling_job;
    sampling_job.animation = &animation_;
    sampling_job.context = &context_;
    sampling_job.ratio = controller_.time_ratio();
    sampling_job.output = make_span(locals_);
    if (!sampling_job.Run()) {
      return false;
    }

    // Converts from local space to model space matrices.
    ozz::animation::LocalToModelJob ltm_job;
    ltm_job.skeleton = &skeleton_;
    ltm_job.input = make_span(locals_);
    ltm_job.output = make_span(models_);
    if (!ltm_job.Run()) {
      return false;
    }
    return true;
  }

  bool RaycastLegs() {
    // Pelvis offset isn't updated yet, it shouldn't be considered. So we're
    // using "unoffsetted" root transform.
    const ozz::math::Float4x4 root = GetRootTransform();

    // Raycast down for each leg to find the intersection point with the floor.
    for (size_t l = 0; l < kLegsCount; ++l) {
      const LegSetup& leg = legs_setup_[l];
      LegRayInfo& ray = rays_info_[l];

      // Finds ankle initial world space position
      ozz::math::Store3PtrU(TransformPoint(root, models_[leg.ankle].cols[3]),
                            &ankles_initial_ws_[l].x);

      // Builds ray, from above ankle (kFootRayHeightOffset) and going downward.
      ray.start = ankles_initial_ws_[l] + kFootRayHeightOffset;
      ray.dir = kDown;
      ray.hit = ozz::sample::RayIntersectsMeshes(
          ray.start, ray.dir, make_span(floors_), &ray.hit_point,
          &ray.hit_normal);
    }

    return true;
  }

  // Comptutes ankle target position (C), so that the foot is in contact with
  // the floor. Because of floor slope (defined by raycast intersection normal),
  // ankle position cannot be simply be offseted by foot offset. See geogebra
  // diagram for more details: media/doc/samples/foot_ik_ankle.ggb
  bool UpdateAnklesTarget() {
    for (size_t l = 0; l < kLegsCount; ++l) {
      // A locked leg keeps the anchor captured at its contact rising edge, so
      // the target must not be recomputed from this frame's raycast. This
      // applies even if this frame ray misses the ground.
      if (ContactTracksEnabled() && contact_lock_.locked(LegOf(l))) {
        ankles_target_ws_[l] = contact_lock_.anchor(LegOf(l)).target;
        continue;
      }

      const LegRayInfo& ray = rays_info_[l];
      if (!ray.hit) {
        continue;
      }

      // Override ankle position with result.
      ankles_target_ws_[l] = ComputeAnkleTarget(ray, foot_heigh_);
    }

    return true;
  }

  // Recomputes pelvis offset.
  // Strategy is to move the pelvis along "down" axis (ray axis), enough for
  // the lowest foot (lowest from its original position) to reaches ankle
  // target. The other foot will be ik-ed.
  bool UpdatePelvisOffset() {
    pelvis_offset_ = ozz::math::Float3(0.f, 0.f, 0.f);

    float max_dot = -std::numeric_limits<float>::max();
    if (pelvis_correction_) {
      for (size_t l = 0; l < kLegsCount; ++l) {
        const bool locked =
            ContactTracksEnabled() && contact_lock_.locked(LegOf(l));
        // An unlocked leg without a ground hit has no target this frame. A
        // locked leg always has its anchor target.
        if (!rays_info_[l].hit && !locked) {
          continue;
        }

        // Check if this ankle is lower (in down direction) compared to the
        // previous one.
        const ozz::math::Float3 ankle_to_target =
            ankles_target_ws_[l] - ankles_initial_ws_[l];
        const float dot = Dot(ankle_to_target, kDown);
        if (dot > max_dot) {
          max_dot = dot;

          // Compute offset using the maximum displacement that the legs should
          // have to touch ground.
          pelvis_offset_ = kDown * dot;
        }
      }
    }

    return true;
  }

  // Applies two bone IK to the leg, and aim IK to the ankle
  bool UpdateFootIK() {
    // Pelvis offset needs to be considered when converting to medel space. So
    // we're using "offsetted" root transform.
    const ozz::math::Float4x4 root = GetOffsettedRootTransform();
    const ozz::math::Float4x4 inv_root = Invert(root);

    ozz::animation::LocalToModelJob ltm_job;
    ltm_job.skeleton = &skeleton_;
    ltm_job.input = make_span(locals_);
    ltm_job.output = make_span(models_);

    // Perform IK
    for (size_t l = 0; l < kLegsCount; ++l) {
      const LegRayInfo& ray = rays_info_[l];
      const ContactLock::Leg leg = LegOf(l);
      const bool locked =
          ContactTracksEnabled() && contact_lock_.locked(leg);

      // A leg is driven either by its per-frame raycast, or by a contact
      // anchor. A locked leg keeps being corrected even if the current frame
      // ray misses the ground (the anchor stays valid for the support phase).
      if (!ray.hit && !locked) {
        continue;
      }
      const LegSetup& leg_setup = legs_setup_[l];

      // Updates leg joint chain so ankle reaches its targetted position.
      if (two_bone_ik_ &&
          !ApplyLegTwoBoneIK(leg_setup, ankles_target_ws_[l], inv_root)) {
        return false;
      }

      // Updates leg joints model-space transforms.
      // Update will go from hip to ankle. Ankle's siblings might no be updated
      // as local-to-model will stop as soon as ankle joint is reached.
      ltm_job.from = leg_setup.hip;
      ltm_job.to = leg_setup.ankle;
      if (!ltm_job.Run()) {
        return false;
      }

      // Computes ankle orientation so it's aligned to the floor normal. A
      // locked leg uses the normal captured at the contact edge.
      const ozz::math::Float3 normal =
          locked ? contact_lock_.anchor(leg).normal : ray.hit_normal;
      const ozz::math::Float3 aim_ik_target(ankles_target_ws_[l] + normal);
      if (aim_ik_ && !ApplyAnkleAimIK(leg_setup, aim_ik_target, inv_root)) {
        return false;
      }

      // Updates model-space transformation now ankle local changes is done.
      // Ankle rotation has already been updated, but its siblings (or it's
      // parent siblings) might are not. So we local-to-model update must
      // be complete starting from hip.
      ltm_job.from = leg_setup.hip;
      ltm_job.to = ozz::animation::Skeleton::kMaxJoints;
      if (!ltm_job.Run()) {
        return false;
      }
    }
    return true;
  }

  // This function will compute two bone IK on the leg, updating hip and knee
  // rotations so that ankle can reach its targetted position.
  bool ApplyLegTwoBoneIK(const LegSetup& _leg,
                         const ozz::math::Float3& _target_ws,
                         const ozz::math::Float4x4& _inv_root) {
    // Target position and pole vectors must be in model space.
    const ozz::math::SimdFloat4 target_ms = TransformPoint(
        _inv_root, ozz::math::simd_float4::Load3PtrU(&_target_ws.x));
    const ozz::math::SimdFloat4 pole_vector_ms = models_[_leg.knee].cols[1];

    // Builds two bone IK job.
    ozz::animation::IKTwoBoneJob ik_job;
    ik_job.target = target_ms;
    ik_job.pole_vector = pole_vector_ms;
    // Mid axis (knee) is constant (usualy), and arbitratry defined by
    // skeleton/rig setup.
    ik_job.mid_axis = kKneeAxis;
    ik_job.weight = weight_;
    ik_job.soften = soften_;
    ik_job.start_joint = &models_[_leg.hip];
    ik_job.mid_joint = &models_[_leg.knee];
    ik_job.end_joint = &models_[_leg.ankle];
    ozz::math::SimdQuaternion start_correction;
    ik_job.start_joint_correction = &start_correction;
    ozz::math::SimdQuaternion mid_correction;
    ik_job.mid_joint_correction = &mid_correction;
    if (!ik_job.Run()) {
      return false;
    }
    // Apply IK quaternions to their respective local-space transforms.
    // Model-space transformations needs to be updated after a call to this
    // function.
    ozz::sample::MultiplySoATransformQuaternion(_leg.hip, start_correction,
                                                make_span(locals_));
    ozz::sample::MultiplySoATransformQuaternion(_leg.knee, mid_correction,
                                                make_span(locals_));

    return true;
  }

  // This function will compute aim IK on the ankle, updating its rotations so
  // it can be aligned with the floor.
  // The strategy is to align ankle up vector in the direction of the floor
  // normal. The forward direction of the foot is then driven by the pole
  // vector, which polls the foot (ankle forward vector) toward it's original
  // (animated) direction.
  bool ApplyAnkleAimIK(const LegSetup& _leg,
                       const ozz::math::Float3& _target_ws,
                       const ozz::math::Float4x4& _inv_root) {
    // Target position and pole vectors must be in model space.
    const ozz::math::SimdFloat4 target_ms = TransformPoint(
        _inv_root, ozz::math::simd_float4::Load3PtrU(&_target_ws.x));

    ozz::animation::IKAimJob ik_job;
    // Forward and up vectors are constant (usualy), and arbitratry defined by
    // skeleton/rig setup.
    ik_job.forward = kAnkleForward;
    ik_job.up = kAnkleUp;

    // Model space targetted direction (floor normal in this case).
    ik_job.target = target_ms;

    // Uses constant ankle Y (skeleton/rig setup dependent) as pole vector. That
    // allows to maintain foot direction.
    ik_job.pole_vector = models_[_leg.ankle].cols[1];

    ik_job.joint = &models_[_leg.ankle];
    ik_job.weight = weight_;
    ozz::math::SimdQuaternion correction;
    ik_job.joint_correction = &correction;
    if (!ik_job.Run()) {
      return false;
    }
    // Apply IK quaternions to their respective local-space transforms.
    // Model-space transformations needs to be updated after a call to this
    // function.
    ozz::sample::MultiplySoATransformQuaternion(_leg.ankle, correction,
                                                make_span(locals_));

    return true;
  }

  virtual bool OnDisplay(ozz::sample::Renderer* _renderer) {
    const float kAxeScale = .1f;
    const ozz::math::Float4x4 kAxesScale =
        ozz::math::Float4x4::Scaling(ozz::math::simd_float4::Load1(kAxeScale));
    const ozz::math::Float4x4 identity = ozz::math::Float4x4::identity();
    const ozz::math::Float4x4 offsetted_root = GetOffsettedRootTransform();

    bool success = true;

    // Renders floor meshes.
    for (size_t i = 0; i < floors_.size(); ++i) {
      success &= _renderer->DrawMesh(floors_[i], identity);
    }

    // Renders character.
    if (show_skin_) {
      // Builds skinning matrices.
      // The mesh might not use (aka be skinned by) all skeleton joints. We
      // use the joint remapping table (available from the mesh object) to
      // reorder model-space matrices and build skinning ones.
      for (size_t m = 0; m < meshes_.size(); ++m) {
        const ozz::sample::Mesh& mesh = meshes_[m];
        for (size_t i = 0; i < mesh.joint_remaps.size(); ++i) {
          skinning_matrices_[i] =
              models_[mesh.joint_remaps[i]] * mesh.inverse_bind_poses[i];
        }

        success &= _renderer->DrawSkinnedMesh(
            mesh, make_span(skinning_matrices_), offsetted_root);
      }
    } else {
      // Renders skeleton only.
      success &=
          _renderer->DrawPosture(skeleton_, make_span(models_), offsetted_root);
    }

    // Showing joints
    if (show_joints_) {
      for (size_t l = 0; l < kLegsCount; ++l) {
        const LegSetup& leg = legs_setup_[l];
        for (size_t i = 0; i < 3; ++i) {
          const int joints[3] = {leg.hip, leg.knee, leg.ankle};
          const ozz::math::Float4x4& transform =
              offsetted_root * models_[joints[i]];
          success &= _renderer->DrawAxes(transform * kAxesScale);
        }
      }
    }

    // Shows raycast results
    if (show_raycast_) {
      for (size_t l = 0; l < kLegsCount; ++l) {
        const LegRayInfo& ray = rays_info_[l];
        if (ray.hit) {
          const ozz::math::Float3 ray_hit[] = {ray.start, ray.hit_point};
          success &=
              _renderer->DrawLines(ray_hit, ozz::sample::kGreen, identity);

          const ozz::math::Float3 ray_normal[] = {
              ray.hit_point, ray.hit_point + ray.hit_normal * .5f};
          success &=
              _renderer->DrawLines(ray_normal, ozz::sample::kRed, identity);
        } else {
          const ozz::math::Float3 ray_infinite[] = {ray.start,
                                                    ray.start + ray.dir * 10.f};
          success &=
              _renderer->DrawLines(ray_infinite, ozz::sample::kWhite, identity);
        }
      }
    }

    // Shows two bone ik ankle target
    if (show_ankle_target_) {
      for (size_t l = 0; l < kLegsCount; ++l) {
        const LegRayInfo& ray = rays_info_[l];
        if (ray.hit) {
          const ozz::math::Float4x4& transform =
              ozz::math::Float4x4::Translation(ankles_target_ws_[l]);
          success &= _renderer->DrawAxes(transform * kAxesScale);
        }
      }
    }

    // Shows the world-space anchors captured by the contact layer. They stay
    // fixed in the world for the whole support phase, independently of the
    // character root movement.
    if (show_contact_anchors_ && ContactTracksEnabled()) {
      for (int l = 0; l < ContactLock::kLegsCount; ++l) {
        const ContactLock::Leg leg = LegOf(l);
        if (!contact_lock_.locked(leg)) {
          continue;
        }
        const ContactLock::Anchor& anchor = contact_lock_.anchor(leg);
        const ozz::math::Float4x4 transform =
            ozz::math::Float4x4::Translation(anchor.target);
        success &= _renderer->DrawAxes(transform * kAxesScale);
        const ozz::math::Float3 normal[] = {
            anchor.target, anchor.target + anchor.normal * .5f};
        success &=
            _renderer->DrawLines(normal, ozz::sample::kYellow, identity);
      }
    }

    if (show_root_) {
      const ozz::math::Float4x4 root = GetRootTransform();
      success &= _renderer->DrawAxes(root);
    }
    if (show_offsetted_root_) {
      success &= _renderer->DrawAxes(offsetted_root);
    }

    return success;
  }

  virtual bool OnInitialize() {
    // Reading skeleton.
    if (!ozz::sample::LoadSkeleton(OPTIONS_skeleton, &skeleton_)) {
      return false;
    }

    // Reading animation.
    if (!ozz::sample::LoadAnimation(OPTIONS_animation, &animation_)) {
      return false;
    }

    // Allocates runtime buffers.
    const int num_soa_joints = skeleton_.num_soa_joints();
    locals_.resize(num_soa_joints);
    const int num_joints = skeleton_.num_joints();
    models_.resize(num_joints);

    // Allocates a context that matches animation requirements.
    context_.Resize(num_joints);

    // The contact event callback samples the base animation on its own
    // scratch buffers so that it never pollutes the current frame posture.
    scratch_locals_.resize(num_soa_joints);
    scratch_models_.resize(num_joints);
    scratch_context_.Resize(num_joints);

    // Finds left and right joints.
    if (!SetupLeg(skeleton_, kLeftJointNames, &legs_setup_[kLeft]) ||
        !SetupLeg(skeleton_, kRightJointNames, &legs_setup_[kRight])) {
      ozz::log::Err()
          << "At least a joint wasn't found in the skeleton hierarchy."
          << std::endl;
      return false;
    }

    // Reading character mesh.
    if (!ozz::sample::LoadMeshes(OPTIONS_mesh, &meshes_)) {
      return false;
    }

    // The number of joints of the mesh needs to match skeleton.
    for (size_t m = 0; m < meshes_.size(); ++m) {
      const ozz::sample::Mesh& mesh = meshes_[m];
      if (num_joints < mesh.highest_joint_index()) {
        ozz::log::Err() << "The provided mesh doesn't match skeleton "
                           "(joint count mismatch)."
                        << std::endl;
        return false;
      }
    }
    skinning_matrices_.resize(num_joints);

    // Reading collision/rendering floor mesh.
    if (!ozz::sample::LoadMeshes(OPTIONS_floor, &floors_)) {
      return false;
    }

    // Optional contact tracks. Loading failures (empty path included) simply
    // keep the original per-frame ground search for the related foot.
    has_left_contact_track_ =
        LoadContactTrack(ContactLock::kLeft, OPTIONS_left_contact,
                         &left_contact_track_);
    has_right_contact_track_ =
        LoadContactTrack(ContactLock::kRight, OPTIONS_right_contact,
                         &right_contact_track_);
    if (ContactTracksEnabled()) {
      contact_lock_.SetTrack(ContactLock::kLeft,
                             has_left_contact_track_ ? &left_contact_track_
                                                     : nullptr);
      contact_lock_.SetTrack(ContactLock::kRight,
                             has_right_contact_track_ ? &right_contact_track_
                                                      : nullptr);
      contact_lock_.set_event_callback(
          [this](ContactLock::Leg _leg, float _ratio,
                 ContactLock::EventSnapshot* _snapshot) {
            return BuildEventSnapshot(_leg, _ratio, _snapshot);
          });
      OpenContactCsv();
    }

    return true;
  }

  // Loads an optional contact track. An empty path is not an error: the foot
  // simply runs without contact locking. Returns true when a track was loaded.
  bool LoadContactTrack(ContactLock::Leg _leg, const char* _path,
                        ozz::animation::FloatTrack* _track) {
    if (_path == nullptr || _path[0] == '\0') {
      ozz::log::Out() << (_leg == ContactLock::kLeft ? "Left" : "Right")
                      << " foot has no contact track, using per-frame ground "
                         "search."
                      << std::endl;
      return false;
    }
    if (!ozz::sample::LoadTrack(_path, _track)) {
      ozz::log::Err() << "Failed to load contact track " << _path
                      << ", falling back to per-frame ground search."
                      << std::endl;
      return false;
    }
    return true;
  }

  bool ContactTracksEnabled() const {
    return has_left_contact_track_ || has_right_contact_track_;
  }

  static ContactLock::Leg LegOf(size_t _l) {
    return static_cast<ContactLock::Leg>(_l);
  }

  // Evaluates the character root trajectory at an (un-wrapped) event ratio.
  // The interactive sample does not own an authored root motion track, so the
  // root transform is interpolated between the previous and current frame
  // roots across the update range.
  ozz::math::Float4x4 RootAtEventRatio(float _ratio) const {
    const float span = playback_ratio_ - previous_playback_ratio_;
    float alpha = 0.f;
    if (span != 0.f) {
      alpha = ozz::math::Clamp(0.f, (_ratio - previous_playback_ratio_) / span,
                               1.f);
    }
    const ozz::math::Float3 translation = ozz::math::Lerp(
        previous_root_translation_, root_translation_, alpha);
    const float yaw = ozz::math::Lerp(previous_root_yaw_, root_yaw_, alpha);
    return ozz::math::Float4x4::Translation(translation) *
           ozz::math::Float4x4::FromEuler(ozz::math::Float3(yaw, 0.f, 0.f));
  }

  // ContactLock event callback. Samples the base animation and raycasts the
  // ground at the exact edge ratio, using dedicated scratch buffers, so the
  // current frame SamplingJob context and corrected posture are untouched.
  bool BuildEventSnapshot(ContactLock::Leg _leg, float _ratio,
                          ContactLock::EventSnapshot* _snapshot) {
    const float local_ratio = _ratio - std::floor(_ratio);

    ozz::animation::SamplingJob sampling_job;
    sampling_job.animation = &animation_;
    sampling_job.context = &scratch_context_;
    sampling_job.ratio = local_ratio;
    sampling_job.output = make_span(scratch_locals_);
    if (!sampling_job.Run()) {
      return false;
    }

    ozz::animation::LocalToModelJob ltm_job;
    ltm_job.skeleton = &skeleton_;
    ltm_job.input = make_span(scratch_locals_);
    ltm_job.output = make_span(scratch_models_);
    if (!ltm_job.Run()) {
      return false;
    }

    const LegSetup& leg = legs_setup_[_leg];
    const ozz::math::Float4x4 root = RootAtEventRatio(_ratio);
    _snapshot->root = root;
    _snapshot->ankle_model = scratch_models_[leg.ankle];

    ozz::math::Float3 ankle_ws;
    ozz::math::Store3PtrU(
        TransformPoint(root, scratch_models_[leg.ankle].cols[3]),
        &ankle_ws.x);

    LegRayInfo ray;
    ray.start = ankle_ws + kFootRayHeightOffset;
    ray.dir = kDown;
    ray.hit = ozz::sample::RayIntersectsMeshes(
        ray.start, ray.dir, make_span(floors_), &ray.hit_point,
        &ray.hit_normal);
    if (!ray.hit) {
      _snapshot->hit = false;
      return true;  // A miss is a valid "do not lock" result.
    }

    _snapshot->hit = true;
    _snapshot->hit_point = ray.hit_point;
    _snapshot->hit_normal = ray.hit_normal;
    _snapshot->ankle_target_ws = ComputeAnkleTarget(ray, foot_heigh_);
    return true;
  }

  // Drives the optional contact layer over the current update range.
  bool UpdateContactLocks() {
    if (!ContactTracksEnabled()) {
      return true;
    }

    bool reachable[ContactLock::kLegsCount];
    for (int l = 0; l < ContactLock::kLegsCount; ++l) {
      reachable[l] = IsAnchorReachable(LegOf(l));
    }

    return contact_lock_.Update(previous_playback_ratio_, playback_ratio_,
                                reachable);
  }

  // Tests whether the stored anchor still fits inside the leg's reach, from
  // the current (uncorrected) model-space posture. Only called for locked
  // legs. Mirrors IKTwoBoneJob's bone-length reachability test.
  bool IsAnchorReachable(ContactLock::Leg _leg) {
    if (!contact_lock_.locked(_leg)) {
      return true;
    }
    const LegSetup& leg = legs_setup_[_leg];
    const ozz::math::Float4x4 root = GetRootTransform();
    const ozz::math::SimdFloat4 target_ms = TransformPoint(
        Invert(root),
        ozz::math::simd_float4::Load3PtrU(
            &contact_lock_.anchor(_leg).target.x));

    const float hip_ankle = ozz::math::GetX(ozz::math::Length3(
        models_[leg.ankle].cols[3] - models_[leg.hip].cols[3]));
    const float hip_target =
        ozz::math::GetX(ozz::math::Length3(target_ms - models_[leg.hip].cols[3]));
    return hip_target <= hip_ankle;
  }

  // Opens the optional debug CSV file and writes its header.
  void OpenContactCsv() {
    const char* path = OPTIONS_contact_csv;
    if (path == nullptr || path[0] == '\0') {
      return;
    }
    contact_csv_ = ozz::make_unique<ozz::io::File>(path, "w");
    if (!contact_csv_->opened()) {
      ozz::log::Err() << "Failed to open contact CSV file " << path << "."
                      << std::endl;
      contact_csv_.reset();
      return;
    }
    static const char kHeader[] =
        "ratio,left_contact,left_anchor_x,left_anchor_y,left_anchor_z,"
        "left_ankle_x,left_ankle_y,left_ankle_z,"
        "right_contact,right_anchor_x,right_anchor_y,right_anchor_z,"
        "right_ankle_x,right_ankle_y,right_ankle_z\n";
    contact_csv_->Write(kHeader, sizeof(kHeader) - 1);
  }

  // Appends one row describing contact state, anchors and the actual IK'd
  // ankle positions for the current frame.
  void WriteContactCsv() {
    if (!ContactTracksEnabled() || !contact_csv_ ||
        !contact_csv_->opened()) {
      return;
    }

    const ozz::math::Float4x4 root = GetOffsettedRootTransform();
    char line[512];
    int offset = std::snprintf(line, sizeof(line), "%.6f", playback_ratio_);
    for (int l = 0; l < ContactLock::kLegsCount; ++l) {
      const ContactLock::Leg leg = LegOf(l);
      const bool locked = contact_lock_.locked(leg);
      const ozz::math::Float3& anchor = contact_lock_.anchor(leg).target;
      ozz::math::Float3 ankle;
      ozz::math::Store3PtrU(
          TransformPoint(root, models_[legs_setup_[l].ankle].cols[3]),
          &ankle.x);
      offset += std::snprintf(
          line + offset, sizeof(line) - offset,
          ",%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f", locked ? 1 : 0, anchor.x,
          anchor.y, anchor.z, ankle.x, ankle.y, ankle.z);
    }
    offset += std::snprintf(line + offset, sizeof(line) - offset, "\n");
    if (offset > 0) {
      contact_csv_->Write(line, static_cast<size_t>(offset));
    }
  }

  bool SetupLeg(const ozz::animation::Skeleton& _skeleton,
                const char* _joint_names[3], LegSetup* _leg) {
    int found = 0;
    int joints[3] = {0};
    for (int i = 0; i < _skeleton.num_joints() && found != 3; i++) {
      const char* joint_name = _skeleton.joint_names()[i];
      if (std::strcmp(joint_name, _joint_names[found]) == 0) {
        joints[found] = i;
        ++found;
      }
    }
    _leg->hip = joints[0];
    _leg->knee = joints[1];
    _leg->ankle = joints[2];
    return found == 3;
  }

  virtual bool OnGui(ozz::sample::ImGui* _im_gui) {
    char label[32];

    // Main options
    {
      static bool opened = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Sample options", &opened);
      if (opened) {
        _im_gui->DoCheckBox("Auto character height", &auto_character_height_);
        _im_gui->DoCheckBox("Pelvis correction", &pelvis_correction_);
        _im_gui->DoCheckBox("Two bone IK (legs)", &two_bone_ik_);
        _im_gui->DoCheckBox("Aim IK (ankles)", &aim_ik_);
      }
    }

    // Exposes animation runtime playback controls.
    {
      static bool open = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Animation control", &open);
      if (open) {
        // A manual time jump cannot reconstruct past contact states: reset
        // the locks and re-sync the un-wrapped playback ratio.
        if (controller_.OnGui(animation_, _im_gui)) {
          contact_lock_.Reset();
          playback_ratio_ = controller_.time_ratio();
          previous_playback_ratio_ = playback_ratio_;
        }
      }
    }

    // Settings
    {
      static bool opened = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "IK settings", &opened);
      if (opened) {
        snprintf(label, sizeof(label), "Foot height %.2g", foot_heigh_);
        _im_gui->DoSlider(label, 0.f, .3f, &foot_heigh_);
        snprintf(label, sizeof(label), "Weight %.2g", weight_);
        _im_gui->DoSlider(label, 0.f, 1.f, &weight_);
        snprintf(label, sizeof(label), "Soften %.2g", soften_);
        _im_gui->DoSlider(label, 0.f, 1.f, &soften_, 1.f, two_bone_ik_);
      }
    }

    {  // Root
      static bool opened = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Root transformation", &opened);
      if (opened) {
        bool moved = false;
        // Translation
        _im_gui->DoLabel("Translation");
        snprintf(label, sizeof(label), "x %.2g", root_translation_.x);
        moved |= _im_gui->DoSlider(label, -10.f, 10.f, &root_translation_.x);
        snprintf(label, sizeof(label), "y %.2g", root_translation_.y);
        moved |= _im_gui->DoSlider(label, 0.f, 5.f, &root_translation_.y, 1.f,
                                   !auto_character_height_);
        snprintf(label, sizeof(label), "z %.2g", root_translation_.z);
        moved |= _im_gui->DoSlider(label, -10.f, 10.f, &root_translation_.z);

        // Rotation (in euler form)
        _im_gui->DoLabel("Rotation");
        snprintf(label, sizeof(label), "yaw %.3g",
                 root_yaw_ * ozz::math::kRadianToDegree);
        moved |= _im_gui->DoSlider(label, -ozz::math::kPi, ozz::math::kPi,
                                   &root_yaw_);

        // Character position shouldn't be changed after the update. In this
        // case, because UI is updated after "game" update, we need to recompute
        // character offset and IK.
        if (moved && auto_character_height_) {
          OnUpdate(0.f, 0.f);
        }
      }
    }

    // Options
    {
      static bool opened = true;
      ozz::sample::ImGui::OpenClose oc(_im_gui, "Debug options", &opened);
      if (opened) {
        _im_gui->DoCheckBox("Show skin", &show_skin_);
        _im_gui->DoCheckBox("Show joints", &show_joints_);
        _im_gui->DoCheckBox("Show raycasts", &show_raycast_);
        _im_gui->DoCheckBox("Show ankle target", &show_ankle_target_);
        _im_gui->DoCheckBox("Show contact anchors", &show_contact_anchors_);
        _im_gui->DoCheckBox("Show root", &show_root_);
        _im_gui->DoCheckBox("Show offsetted root", &show_offsetted_root_);
      }
    }
    return true;
  }

  virtual bool GetCameraInitialSetup(ozz::math::Float3* _center,
                                     ozz::math::Float2* _angles,
                                     float* _distance) const {
    *_center = ozz::math::Float3(4.7f, 2.3f, -.13f);
    *_angles = ozz::math::Float2(-.14f, -2.1f);
    *_distance = 5.9f;
    return true;
  }

  ozz::math::Float4x4 GetRootTransform() const {
    return ozz::math::Float4x4::Translation(root_translation_) *
           ozz::math::Float4x4::FromEuler(ozz::math::Float3(root_yaw_, 0, 0));
  }

  ozz::math::Float4x4 GetOffsettedRootTransform() const {
    if (!pelvis_correction_) {
      return GetRootTransform();
    }

    const ozz::math::Float3 offsetted_translation =
        pelvis_offset_ + root_translation_;

    return ozz::math::Float4x4::Translation(offsetted_translation) *
           ozz::math::Float4x4::FromEuler(ozz::math::Float3(root_yaw_, 0, 0));
  }

 private:
  // Playback animation controller. This is a utility class that helps with
  // controlling animation playback time.
  ozz::sample::PlaybackController controller_;

  // Runtime skeleton.
  ozz::animation::Skeleton skeleton_;

  // Runtime animation.
  ozz::animation::Animation animation_;

  // Sampling context.
  ozz::animation::SamplingJob::Context context_;

  // Buffer of local transforms as sampled from animation_.
  ozz::vector<ozz::math::SoaTransform> locals_;

  // Buffer of model space matrices.
  ozz::vector<ozz::math::Float4x4> models_;

  // Dedicated sampling context and posture buffers used by the contact event
  // callback, so querying an edge time never pollutes the current frame
  // SamplingJob context or the already corrected posture.
  ozz::animation::SamplingJob::Context scratch_context_;
  ozz::vector<ozz::math::SoaTransform> scratch_locals_;
  ozz::vector<ozz::math::Float4x4> scratch_models_;

  // Optional contact-track foot planting layer, plus the tracks it reads.
  // has_*_contact_track_ false means the foot uses its original per-frame
  // behavior.
  ContactLock contact_lock_;
  ozz::animation::FloatTrack left_contact_track_;
  ozz::animation::FloatTrack right_contact_track_;
  bool has_left_contact_track_ = false;
  bool has_right_contact_track_ = false;

  // Un-wrapped playback ratio (not reduced modulo 1) and its previous-frame
  // value. They define the half-open [from, to) range given to the contact
  // layer, including updates that cross one or more animation loops.
  float playback_ratio_ = 0.f;
  float previous_playback_ratio_ = 0.f;

  // Root transform at the previous frame, used to interpolate the root
  // trajectory at contact edge times.
  ozz::math::Float3 previous_root_translation_ = {2.17f, 2.f, -2.06f};
  float previous_root_yaw_ = -2.f;

  // Optional debug CSV output.
  ozz::unique_ptr<ozz::io::File> contact_csv_;

  // Buffer of skinning matrices, result of the joint multiplication of the
  // inverse rest pose with the model space matrix.
  ozz::vector<ozz::math::Float4x4> skinning_matrices_;

  // The mesh used by the sample.
  ozz::vector<ozz::sample::Mesh> meshes_;

  enum { kLeft, kRight };
  enum { kLegsCount = 2 };

  LegSetup legs_setup_[kLegsCount];
  LegRayInfo rays_info_[kLegsCount];
  ozz::math::Float3 ankles_initial_ws_[kLegsCount];
  ozz::math::Float3 ankles_target_ws_[kLegsCount];

  LegRayInfo capsule;

  ozz::math::Float3 pelvis_offset_ = {0.f, 0.f, 0.f};

  // The floor meshes used by the sample (collision and rendering).
  ozz::vector<ozz::sample::Mesh> floors_;

  // Root transformation.
  ozz::math::Float3 root_translation_ = {2.17f, 2.f, -2.06f};
  float root_yaw_ = -2.f;

  // Foot height setting
  float foot_heigh_ = .12f;

  float weight_ = 1.f;
  float soften_ = 1.f;

  bool auto_character_height_ = true;
  bool pelvis_correction_ = true;
  bool two_bone_ik_ = true;
  bool aim_ik_ = true;

  bool show_skin_ = true;
  bool show_joints_ = false;
  bool show_raycast_ = false;
  bool show_ankle_target_ = false;
  bool show_contact_anchors_ = true;
  bool show_root_ = false;
  bool show_offsetted_root_ = false;
};

int main(int _argc, const char** _argv) {
  const char* title = "Ozz-animation sample: Foot IK";
  return FootIKSampleApplication().Run(_argc, _argv, "1.0", title);
}
