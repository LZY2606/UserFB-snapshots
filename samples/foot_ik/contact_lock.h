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

#ifndef OZZ_SAMPLES_FOOT_IK_CONTACT_LOCK_H_
#define OZZ_SAMPLES_FOOT_IK_CONTACT_LOCK_H_

#include <functional>

#include "ozz/animation/runtime/track_triggering_job.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/vec_float.h"

namespace ozz {
namespace animation {
class FloatTrack;
}  // namespace animation
namespace sample {

// Contact driven foot planting ("contact-track" layer) for the foot IK sample.
//
// Without a contact track, the sample re-raycasts the ground from the animated
// ankle every frame, so the foot target keeps moving while the character
// translates or rotates. ContactLock fixes that: a FloatTrack per foot tells
// when the foot is supporting (track value greater than the 0.5 threshold) or
// swinging (any other value). On a rising edge, the ankle target and ground
// normal are captured once, in world space, at the edge time, and then reused
// for the whole support phase. The stored world anchor is not attached to the
// character root, so it stays put while the character moves. Each frame the
// caller transforms that fixed anchor back into its current model space and
// feeds it to the existing pelvis correction, two bone IK and ankle aim IK
// pipeline.
//
// Time is handled with un-wrapped (un-modded) playback ratios: Update() takes
// the raw ratio range [from, to) of the current update. A single update can
// span several animation loops (|to - from| > 1); every rising and falling
// edge in that range is processed in chronological order through
// ozz::animation::TrackTriggeringJob, which natively handles looping and both
// step and linear tracks. The interval is half-open: an edge landing exactly
// on _to is not processed here, it belongs to the next update. Updating with
// _from == _to (paused frame) processes no edge, so an anchor is never
// captured twice, though reachability is still re-evaluated (the root can
// move while time is paused).
//
// Per-foot, independently:
// - Falling edge: the lock is released immediately; a swinging foot never
//   gets a contact correction.
// - Rising edge: the event callback is invoked. If it reports a ground hit,
//   the anchor is captured; if it reports a miss, this whole support phase
//   runs unlocked.
// - Once locked, later raycast results never overwrite the anchor. If the
//   target becomes unreachable (see Update's _reachable argument), the lock is
//   released and stays released until the next rising edge: it is never
//   re-captured inside the same support phase, to avoid sliding.
// - Reset() (manual time jump, backward playback, animation switch...) clears
//   every anchor and state. There is no attempt to reconstruct past states.
class ContactLock {
 public:
  // Number of legs managed by the layer (left and right), kept as an enum for
  // indexing the per-foot arrays.
  enum Leg { kLeft = 0, kRight = 1, kLegsCount = 2 };

  // Information captured at a rising edge and held for the support phase.
  struct Anchor {
    // World-space ankle target at the edge time.
    ozz::math::Float3 target = ozz::math::Float3::zero();

    // World-space ground normal at the edge time.
    ozz::math::Float3 normal = ozz::math::Float3(0.f, 1.f, 0.f);

    // Un-wrapped playback ratio of the rising edge that created the anchor.
    float event_ratio = 0.f;

    // True while the anchor defines an active, usable lock.
    bool locked = false;
  };

  // Snapshot of the scene at a given (edge) playback ratio, queried by the
  // layer through its event callback. All positions and orientations are in
  // the same world space the layer stores anchors in.
  struct EventSnapshot {
    // Character root transformation at the event ratio.
    ozz::math::Float4x4 root;

    // Ankle model-space matrix (relative to the character root) at the event
    // ratio, sampled from the base posture, before any IK correction.
    ozz::math::Float4x4 ankle_model;

    // Ground hit for a ray cast downward from the event-time ankle.
    bool hit = false;
    ozz::math::Float3 hit_point = ozz::math::Float3::zero();
    ozz::math::Float3 hit_normal = ozz::math::Float3(0.f, 1.f, 0.f);

    // Desired world-space ankle target at the event time, derived by the
    // callback from the hit point/normal and its own foot-height setup. Only
    // read when hit is true; this is the value the anchor stores.
    ozz::math::Float3 ankle_target_ws = ozz::math::Float3::zero();
  };

  // Event callback. It is called for a rising edge, with the leg it applies to
  // and the un-wrapped playback ratio of the edge (can be outside [0,1] when
  // the update crossed loops).
  //
  // The implementation must fill the snapshot at that exact event ratio:
  // sample the base animation/root trajectory at _event_ratio (using its own
  // scratch buffers), raycast the ground from the resulting world ankle
  // position, and compute the desired world ankle target. It must not touch
  // the caller's current-frame SamplingJob context, local/model buffers or any
  // already corrected posture.
  //
  // On failure (no ground hit, or any situation where the foot must not be
  // planted) it must leave EventSnapshot::hit false (or return false); the
  // support phase then runs without a lock. Returning false never aborts the
  // edge processing of the other foot.
  typedef std::function<bool(Leg _leg, float _event_ratio,
                             EventSnapshot* _snapshot)>
      EventCallback;

  ContactLock();

  // Sets the contact track of a leg. A null track disables the layer for that
  // leg: it then behaves as if no contact-track option was provided, i.e. the
  // caller keeps raycasting the ground every frame.
  void SetTrack(Leg _leg, const ozz::animation::FloatTrack* _track);
  bool HasTrack(Leg _leg) const;

  // Sets the callback used to query the base posture, root transform and
  // ground hit at edge times. Must be set before Update while tracks are used.
  void set_event_callback(EventCallback _callback) {
    event_callback_ = std::move(_callback);
  }

  // Clears every anchor and per-foot state. Must be called on manual time
  // jumps, backward playback and animation switches; after Reset the layer
  // waits for a rising edge from the unlocked state.
  void Reset();

  // Processes the contact tracks over the un-wrapped ratio range [_from, _to).
  // Only forward ranges (_from < _to) yield edges; an edge landing exactly on
  // _to is left for the next update. A paused frame (_from == _to) captures
  // nothing, but _reachable is still applied so a root moved while paused can
  // release a lock.
  //
  // _reachable[l] reports whether the currently stored anchor can still be
  // reached by leg l's IK chain at the current frame (evaluated by the caller
  // from its actual model-space posture). It is only considered for locked
  // legs; an unreachable anchor is released for the rest of the support phase.
  //
  // Returns false only if an internal triggering job fails (invalid setup).
  bool Update(float _from, float _to, const bool _reachable[kLegsCount]);

  // Current lock state of a leg.
  bool locked(Leg _leg) const { return anchors_[_leg].locked; }

  // Current anchor of a leg. Only meaningful when locked() is true.
  const Anchor& anchor(Leg _leg) const { return anchors_[_leg]; }

 private:
  // Processes one edge (already in chronological order) for one leg.
  void ProcessEdge(Leg _leg,
                   const ozz::animation::TrackTriggeringJob::Edge& _edge);

  const ozz::animation::FloatTrack* tracks_[kLegsCount] = {nullptr, nullptr};
  Anchor anchors_[kLegsCount];
  EventCallback event_callback_;
};

}  // namespace sample
}  // namespace ozz
#endif  // OZZ_SAMPLES_FOOT_IK_CONTACT_LOCK_H_
