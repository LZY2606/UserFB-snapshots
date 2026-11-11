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

#include "contact_lock.h"

#include <algorithm>
#include <vector>

#include "ozz/animation/runtime/track.h"
#include "ozz/animation/runtime/track_triggering_job.h"

namespace ozz {
namespace sample {

ContactLock::ContactLock() = default;

void ContactLock::SetTrack(Leg _leg, const ozz::animation::FloatTrack* _track) {
  tracks_[_leg] = _track;
  // A track change invalidates any state captured from the previous one.
  anchors_[_leg] = Anchor();
}

bool ContactLock::HasTrack(Leg _leg) const {
  return tracks_[_leg] != nullptr;
}

void ContactLock::Reset() {
  for (int i = 0; i < kLegsCount; ++i) {
    anchors_[i] = Anchor();
  }
}

void ContactLock::ProcessEdge(
    Leg _leg, const ozz::animation::TrackTriggeringJob::Edge& _edge) {
  Anchor& anchor = anchors_[_leg];

  if (!_edge.rising) {
    // Falling edge: release immediately. A swinging foot gets no contact
    // correction until the next rising edge.
    anchor.locked = false;
    return;
  }

  // Rising edge: (re)arm the capture for this new support phase. Any previous
  // anchor is first dropped, so a failed capture leaves the leg unlocked for
  // the whole phase rather than reusing stale data.
  anchor.locked = false;

  EventSnapshot snapshot;
  if (!event_callback_ || !event_callback_(_leg, _edge.ratio, &snapshot) ||
      !snapshot.hit) {
    // No ground at contact time: this support phase runs unlocked.
    return;
  }

  anchor.target = snapshot.ankle_target_ws;
  anchor.normal = snapshot.hit_normal;
  anchor.event_ratio = _edge.ratio;
  anchor.locked = true;
}

bool ContactLock::Update(float _from, float _to,
                         const bool _reachable[kLegsCount]) {
  // Edges are only processed over a non-empty, forward range. A paused frame
  // (_from == _to) never captures an anchor again, but reachability is still
  // evaluated below: moving the root while paused can make an anchor
  // unreachable.
  if (_from < _to) {
    // Collects every edge of both legs, then replays them in chronological
    // order. TrackTriggeringJob natively loops over the track when the range
    // is wider than 1, handles backward ranges, and applies the library
    // threshold crossing rules for both step and linear tracks.
    struct TimedEdge {
      float ratio;
      bool rising;
      Leg leg;
    };
    std::vector<TimedEdge> edges;

    for (int l = 0; l < kLegsCount; ++l) {
      if (tracks_[l] == nullptr) {
        continue;
      }

      ozz::animation::TrackTriggeringJob job;
      job.track = tracks_[l];
      job.threshold = .5f;
      job.from = _from;
      job.to = _to;
      ozz::animation::TrackTriggeringJob::Iterator it;
      job.iterator = &it;
      if (!job.Run()) {
        return false;
      }

      for (; it != job.end(); ++it) {
        const ozz::animation::TrackTriggeringJob::Edge& edge = *it;
        edges.push_back(
            TimedEdge{edge.ratio, edge.rising, static_cast<Leg>(l)});
      }
    }

    // Stable sort keeps the left leg first if both feet switch at the exact
    // same ratio, which makes two-foot handoff processing deterministic.
    std::stable_sort(edges.begin(), edges.end(),
                     [](const TimedEdge& _a, const TimedEdge& _b) {
                       return _a.ratio < _b.ratio;
                     });

    for (const TimedEdge& timed_edge : edges) {
      ozz::animation::TrackTriggeringJob::Edge edge{timed_edge.ratio,
                                                    timed_edge.rising};
      ProcessEdge(timed_edge.leg, edge);
    }
  }

  // A locked target that the IK chain can no longer reach is released and
  // stays released until the next rising edge: capture only happens on edges,
  // so it cannot be re-armed inside the same support phase (no sliding).
  for (int l = 0; l < kLegsCount; ++l) {
    if (anchors_[l].locked && !_reachable[l]) {
      anchors_[l].locked = false;
    }
  }

  return true;
}

}  // namespace sample
}  // namespace ozz
