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
// The above copyright notice and the above copyright notice shall be        //
// included in all copies or substantial portions of the Software.           //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS    //
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF                 //
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.     //
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY      //
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,      //
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE       //
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                    //
//                                                                            //
//----------------------------------------------------------------------------//

// SparseSamplingJob tests. Built through devcheck/registered.cpp (which
// includes this file) as well as the regular unit test target.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "devcheck/test-harness.h"

#include "ozz/animation/offline/animation_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/animation.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/sampling_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/animation/runtime/sparse_sampling_job.h"
#include "ozz/base/io/archive.h"
#include "ozz/base/io/stream.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/memory/allocator.h"
#include "ozz/base/memory/unique_ptr.h"
#include "ozz/base/span.h"

namespace {

using ozz::animation::Animation;
using ozz::animation::SparseSamplingJob;
using ozz::animation::offline::AnimationBuilder;
using ozz::animation::offline::RawAnimation;
using ozz::animation::offline::RawSkeleton;
using ozz::animation::offline::SkeletonBuilder;

constexpr float kEps = 1e-5f;

uint32_t Bits(float _f) {
  uint32_t bits;
  std::memcpy(&bits, &_f, sizeof(bits));
  return bits;
}

// One lane (one joint) of a SoaTransform, as plain floats.
struct LaneTransform {
  float tx, ty, tz;
  float rx, ry, rz, rw;
  float sx, sy, sz;
};

LaneTransform ReadLane(const ozz::math::SoaTransform& _soa, int _lane) {
  float xyzw[4];
  LaneTransform lane;
  ozz::math::StorePtrU(_soa.translation.x, xyzw);
  lane.tx = xyzw[_lane];
  ozz::math::StorePtrU(_soa.translation.y, xyzw);
  lane.ty = xyzw[_lane];
  ozz::math::StorePtrU(_soa.translation.z, xyzw);
  lane.tz = xyzw[_lane];
  ozz::math::StorePtrU(_soa.rotation.x, xyzw);
  lane.rx = xyzw[_lane];
  ozz::math::StorePtrU(_soa.rotation.y, xyzw);
  lane.ry = xyzw[_lane];
  ozz::math::StorePtrU(_soa.rotation.z, xyzw);
  lane.rz = xyzw[_lane];
  ozz::math::StorePtrU(_soa.rotation.w, xyzw);
  lane.rw = xyzw[_lane];
  ozz::math::StorePtrU(_soa.scale.x, xyzw);
  lane.sx = xyzw[_lane];
  ozz::math::StorePtrU(_soa.scale.y, xyzw);
  lane.sy = xyzw[_lane];
  ozz::math::StorePtrU(_soa.scale.z, xyzw);
  lane.sz = xyzw[_lane];
  return lane;
}

// Direction-aware quaternion comparison: q and -q represent the same
// rotation, so a global sign flip is allowed.
void ExpectQuaternionSameDirection(float _ax, float _ay, float _az, float _aw,
                                   float _bx, float _by, float _bz, float _bw) {
  const float direct = std::fabs(_ax - _bx) + std::fabs(_ay - _by) +
                       std::fabs(_az - _bz) + std::fabs(_aw - _bw);
  const float flipped = std::fabs(_ax + _bx) + std::fabs(_ay + _by) +
                        std::fabs(_az + _bz) + std::fabs(_aw + _bw);
  SPARSE_CHECK(std::min(direct, flipped) < 4.f * kEps);
}

void ExpectLaneEq(const LaneTransform& _a, const LaneTransform& _b) {
  SPARSE_CHECK(std::fabs(_a.tx - _b.tx) < kEps);
  SPARSE_CHECK(std::fabs(_a.ty - _b.ty) < kEps);
  SPARSE_CHECK(std::fabs(_a.tz - _b.tz) < kEps);
  ExpectQuaternionSameDirection(_a.rx, _a.ry, _a.rz, _a.rw, _b.rx, _b.ry,
                                _b.rz, _b.rw);
  SPARSE_CHECK(std::fabs(_a.sx - _b.sx) < kEps);
  SPARSE_CHECK(std::fabs(_a.sy - _b.sy) < kEps);
  SPARSE_CHECK(std::fabs(_a.sz - _b.sz) < kEps);
}

// Runs the reference, full SamplingJob and returns its posture.
std::vector<ozz::math::SoaTransform> ReferenceSample(
    const Animation* _animation, float _ratio) {
  std::vector<ozz::math::SoaTransform> output(
      static_cast<size_t>(_animation->num_soa_tracks()));
  ozz::animation::SamplingJob::Context context(_animation->num_tracks());
  ozz::animation::SamplingJob job;
  job.animation = _animation;
  job.context = &context;
  job.ratio = _ratio;
  job.output = ozz::make_span(output);
  SPARSE_CHECK(job.Run());
  return output;
}

// Runs SparseSamplingJob for _joints at _ratio, optionally counting stats.
std::vector<ozz::math::SoaTransform> SparseSample(
    const Animation* _animation, SparseSamplingJob::Context* _context,
    const ozz::span<const int>& _joints, float _ratio,
    SparseSamplingJob::Stats* _stats = nullptr) {
  std::vector<ozz::math::SoaTransform> output(
      static_cast<size_t>(_animation->num_soa_tracks()));
  SparseSamplingJob job;
  job.animation = _animation;
  job.context = _context;
  job.joint_indices = _joints;
  job.ratio = _ratio;
  job.output = ozz::make_span(output);
  job.stats = _stats;
  SPARSE_CHECK(job.Run());
  return output;
}

// A half-float friendly, normalized quaternion rotating around a fixed axis.
ozz::math::Quaternion TestQuaternion(int _i, int _k) {
  const float angle = static_cast<float>(_k + 1) * (0.03125f + _i * 0.004f);
  const float s = std::sin(angle);
  return ozz::math::Quaternion(0.6f * s, 0.8f * s, 0.f, std::cos(angle));
}

// Builds the main test animation: 5 joints (so 2 SoA groups, the second one
// holding 3 padding lanes), with staggered multi-channel keys. Track 0 gets
// more than 256 translation keys so that 16-bit timepoint indices are used.
// Keys of different tracks share time points (duplicate times across tracks).
ozz::unique_ptr<Animation> BuildMainAnimation() {
  RawAnimation raw;
  raw.duration = 1.f;
  raw.tracks.resize(5);

  // Track 0: dense translations (> 256 time points).
  constexpr int kDenseKeys = 300;
  for (int k = 0; k <= kDenseKeys; ++k) {
    const float time = static_cast<float>(k) /
                       static_cast<float>(kDenseKeys);
    const float v = static_cast<float>(k % 64) * 0.5f;
    raw.tracks[0].translations.push_back(
        {time, ozz::math::Float3(v, 0.25f * (k % 8), 0.f)});
  }

  for (int joint = 0; joint < 5; ++joint) {
    RawAnimation::JointTrack& track = raw.tracks[joint];

    // Staggered translation keys (track 0 already has its dense set).
    if (joint != 0) {
      const int count = 3 + joint * 2;
      for (int k = 0; k < count; ++k) {
        const float time = static_cast<float>(k) /
                           static_cast<float>(count - 1);
        track.translations.push_back(
            {time, ozz::math::Float3(static_cast<float>(joint) + 0.5f * k,
                                     0.5f * joint, -0.25f * k)});
      }
    }

    // Rotations, offset per track so channels are staggered. Track 2 only
    // provides interior keys, forcing AnimationBuilder to synthesize the
    // first (t = 0) and last (t = 1) keys itself.
    const int rcount = 4 + joint;
    for (int k = 0; k < rcount; ++k) {
      float time =
          static_cast<float>(k) / static_cast<float>(rcount - 1);
      if (joint == 2) {
        time = 0.2f + 0.6f * time;  // Interior range [0.2, 0.8].
      }
      track.rotations.push_back({time, TestQuaternion(joint, k)});
    }

    // Scales, another key layout, including 1.f identity-ish values.
    const int scount = 2 + joint;
    for (int k = 0; k < scount; ++k) {
      const float time =
          static_cast<float>(k) / static_cast<float>(scount - 1);
      track.scales.push_back(
          {time, ozz::math::Float3(1.f + 0.1f * k, 1.f - 0.05f * joint,
                                   0.5f + 0.25f * (k % 3))});
    }
  }

  auto animation = AnimationBuilder()(raw);
  SPARSE_CHECK(animation);
  return animation;
}

// Builds a chain skeleton of _num_joints joints, identity local transforms.
ozz::unique_ptr<ozz::animation::Skeleton> BuildChainSkeleton(
    int _num_joints) {
  RawSkeleton raw;
  raw.roots.resize(1);
  RawSkeleton::Joint* joint = &raw.roots[0];
  for (int i = 0; i < _num_joints; ++i) {
    joint->name = "joint";
    joint->transform = ozz::math::Transform::identity();
    if (i < _num_joints - 1) {
      joint->children.resize(1);
      joint = &joint->children[0];
    }
  }
  auto skeleton = SkeletonBuilder()(raw);
  SPARSE_CHECK(skeleton);
  return skeleton;
}

// Fills a posture with recognizable bit patterns (qNaN and negative zero).
void Poison(ozz::span<ozz::math::SoaTransform> _output) {
  const uint32_t patterns[2] = {0x7fc00000u, 0x80000000u};  // qNaN, -0
  uint32_t* raw = reinterpret_cast<uint32_t*>(_output.data());
  const size_t count = _output.size_bytes() / sizeof(uint32_t);
  for (size_t i = 0; i < count; ++i) {
    raw[i] = patterns[i & 1];
  }
}

// Allocator counting live allocations and total allocation calls, used to
// prove that Run() performs no heap allocation/deallocation.
class CountingAllocator : public ozz::memory::Allocator {
 public:
  explicit CountingAllocator(ozz::memory::Allocator* _inner)
      : inner_(_inner) {}
  void* Allocate(size_t _size, size_t _alignment) override {
    ++allocations;
    ++live;
    return inner_->Allocate(_size, _alignment);
  }
  void Deallocate(void* _block) override {
    if (_block) {
      --live;
      ++deallocations;
    }
    inner_->Deallocate(_block);
  }
  int allocations = 0;
  int deallocations = 0;
  int live = 0;

 private:
  ozz::memory::Allocator* inner_;
};

}  // namespace

// 1. Numeric parity with SamplingJob over half-float translations/scales and
// compressed quaternions, including 0/1 bounds and ratio clamping.
SPARSE_CASE(sparse_values) {
  auto animation = BuildMainAnimation();
  SparseSamplingJob::Context context;
  SPARSE_CHECK(context.Prepare(animation.get()));
  SPARSE_CHECK(context.animation() == animation.get());
  SPARSE_CHECK(context.num_soa_tracks() == animation->num_soa_tracks());

  const int joints_all[] = {0, 1, 2, 3, 4};
  const float ratios[] = {0.f,   1.f,    .25f, .5f,  .75f,
                          -1.f,  2.f,    .125f, .875f, .333333f};

  for (float ratio : ratios) {
    const auto reference = ReferenceSample(animation.get(), ratio);
    const auto sparse = SparseSample(
        animation.get(), &context, ozz::make_span(joints_all), ratio);

    for (int group = 0; group < animation->num_soa_tracks(); ++group) {
      for (int lane = 0; lane < 4; ++lane) {
        const int joint = group * 4 + lane;
        if (joint >= animation->num_tracks()) {
          continue;  // Padding lanes are not part of the animation.
        }
        ExpectLaneEq(ReadLane(reference[group], lane),
                     ReadLane(sparse[group], lane));
      }
    }
  }

  // Empty joint set is legal and writes nothing.
  std::vector<ozz::math::SoaTransform> untouched(
      static_cast<size_t>(animation->num_soa_tracks()));
  Poison(ozz::make_span(untouched));
  const size_t float_count =
      untouched.size() * sizeof(ozz::math::SoaTransform) / sizeof(float);
  const std::vector<uint32_t> before(
      reinterpret_cast<uint32_t*>(untouched.data()),
      reinterpret_cast<uint32_t*>(untouched.data()) + float_count);
  {
    SparseSamplingJob::Stats stats;
    SparseSamplingJob job;
    job.animation = animation.get();
    job.context = &context;
    job.joint_indices = {};
    job.ratio = .5f;
    job.output = ozz::make_span(untouched);
    job.stats = &stats;
    SPARSE_CHECK(job.Run());
    SPARSE_CHECK(stats.time_comparisons == 0);
    SPARSE_CHECK(stats.float3_decompress_blocks == 0);
    SPARSE_CHECK(stats.quaternion_decompress_blocks == 0);
    SPARSE_CHECK(stats.interpolated_groups == 0);
  }
  const uint32_t* after = reinterpret_cast<uint32_t*>(untouched.data());
  for (size_t i = 0; i < before.size(); ++i) {
    SPARSE_CHECK(after[i] == before[i]);
  }
}

// 2. Backward seeks, far jumps, mask changes, unselected lane preservation
// and a LocalToModelJob round trip on a composited posture.
SPARSE_CASE(seek_and_masks) {
  auto animation = BuildMainAnimation();
  auto skeleton = BuildChainSkeleton(5);
  SparseSamplingJob::Context context;
  SPARSE_CHECK(context.Prepare(animation.get()));

  const float seek_ratios[] = {.75f, .25f, .5f, 1.f, 0.f, .9f, .1f, .5f};
  const std::vector<std::vector<int>> masks = {
      {0}, {2, 4}, {1, 3}, {0, 1, 2, 3, 4}, {3}, {}};

  for (float ratio : seek_ratios) {
    const auto reference = ReferenceSample(animation.get(), ratio);

    for (const auto& mask : masks) {
      // Start from the full reference posture, then poison it: sparse
      // sampling must restore the selected joints exactly while leaving all
      // other lanes at their poison bit pattern.
      std::vector<ozz::math::SoaTransform> posture(
          static_cast<size_t>(animation->num_soa_tracks()));
      Poison(ozz::make_span(posture));

      SparseSamplingJob job;
      job.animation = animation.get();
      job.context = &context;
      job.joint_indices = ozz::make_span(mask);
      job.ratio = ratio;
      job.output = ozz::make_span(posture);
      SPARSE_CHECK(job.Run());

      std::vector<bool> selected(5, false);
      for (int j : mask) {
        selected[j] = true;
      }
      for (int joint = 0; joint < 5; ++joint) {
        const int group = joint / 4;
        const int lane = joint % 4;
        if (selected[joint]) {
          ExpectLaneEq(ReadLane(reference[group], lane),
                       ReadLane(posture[group], lane));
        } else {
          // Unselected lane: all 10 floats keep their poison bit pattern.
          // Poison alternates across the 40 floats of a SoaTransform, so an
          // even lane holds qNaN everywhere while an odd lane holds -0.
          const LaneTransform kept = ReadLane(posture[group], lane);
          const float fields[10] = {kept.tx, kept.ty, kept.tz, kept.rx,
                                    kept.ry, kept.rz, kept.rw, kept.sx,
                                    kept.sy, kept.sz};
          const uint32_t expected =
              (lane & 1) ? 0x80000000u : 0x7fc00000u;
          for (int f = 0; f < 10; ++f) {
            SPARSE_CHECK(Bits(fields[f]) == expected);
          }
        }
      }
    }
  }

  // Composite sparse results into a full posture and check world endpoints
  // against a full LocalToModelJob.
  const auto full_posture = ReferenceSample(animation.get(), .375f);
  std::vector<ozz::math::SoaTransform> composited(
      static_cast<size_t>(animation->num_soa_tracks()));
  for (auto& soa : composited) {
    soa = ozz::math::SoaTransform::identity();
  }
  {
    const int first_half[] = {0, 1};
    SparseSamplingJob job;
    job.animation = animation.get();
    job.context = &context;
    job.joint_indices = ozz::make_span(first_half);
    job.ratio = .375f;
    job.output = ozz::make_span(composited);
    SPARSE_CHECK(job.Run());
  }
  {
    const int second_half[] = {2, 3, 4};
    SparseSamplingJob job;
    job.animation = animation.get();
    job.context = &context;
    job.joint_indices = ozz::make_span(second_half);
    job.ratio = .375f;
    job.output = ozz::make_span(composited);
    SPARSE_CHECK(job.Run());
  }
  for (int group = 0; group < animation->num_soa_tracks(); ++group) {
    for (int lane = 0; lane < 4; ++lane) {
      const int joint = group * 4 + lane;
      if (joint < 5) {
        ExpectLaneEq(ReadLane(full_posture[group], lane),
                     ReadLane(composited[group], lane));
      }
    }
  }

  ozz::math::Float4x4 reference_models[5];
  ozz::math::Float4x4 sparse_models[5];
  ozz::animation::LocalToModelJob ltm;
  ltm.skeleton = skeleton.get();
  ltm.input = ozz::make_span(full_posture);
  ltm.output = ozz::make_span(reference_models);
  SPARSE_CHECK(ltm.Run());
  ltm.input = ozz::make_span(composited);
  ltm.output = ozz::make_span(sparse_models);
  SPARSE_CHECK(ltm.Run());
  float ref_endpoint[4];
  float sparse_endpoint[4];
  ozz::math::StorePtrU(reference_models[4].cols[3], ref_endpoint);
  ozz::math::StorePtrU(sparse_models[4].cols[3], sparse_endpoint);
  for (int i = 0; i < 3; ++i) {
    SPARSE_CHECK(std::fabs(ref_endpoint[i] - sparse_endpoint[i]) < kEps);
  }

  // Output larger than the animation: the extra SoA entries must not be
  // written at all.
  std::vector<ozz::math::SoaTransform> oversized(
      static_cast<size_t>(animation->num_soa_tracks()) + 2);
  Poison(ozz::make_span(oversized));
  {
    const int joints[] = {0, 4};
    SparseSamplingJob job;
    job.animation = animation.get();
    job.context = &context;
    job.joint_indices = ozz::make_span(joints);
    job.ratio = .5f;
    job.output = ozz::make_span(oversized);
    SPARSE_CHECK(job.Run());
  }
  for (size_t group = animation->num_soa_tracks();
       group < oversized.size(); ++group) {
    for (int lane = 0; lane < 4; ++lane) {
      const uint32_t expected = (lane & 1) ? 0x80000000u : 0x7fc00000u;
      const LaneTransform kept = ReadLane(oversized[group], lane);
      const float fields[10] = {kept.tx, kept.ty, kept.tz, kept.rx,
                                kept.ry, kept.rz, kept.rw, kept.sx,
                                kept.sy, kept.sz};
      for (float f : fields) {
        SPARSE_CHECK(Bits(f) == expected);
      }
    }
  }

  // Last group padding lanes (the animation has 5 joints, group 1 has 3
  // padding lanes) are never part of any selection and stay untouched.
  {
    std::vector<ozz::math::SoaTransform> padded(
        static_cast<size_t>(animation->num_soa_tracks()));
    Poison(ozz::make_span(padded));
    const int joints[] = {4};
    SparseSamplingJob job;
    job.animation = animation.get();
    job.context = &context;
    job.joint_indices = ozz::make_span(joints);
    job.ratio = .5f;
    job.output = ozz::make_span(padded);
    SPARSE_CHECK(job.Run());
    for (int lane = 1; lane < 4; ++lane) {
      const uint32_t expected = (lane & 1) ? 0x80000000u : 0x7fc00000u;
      const LaneTransform kept = ReadLane(padded[1], lane);
      const float fields[10] = {kept.tx, kept.ty, kept.tz, kept.rx,
                                kept.ry, kept.rz, kept.rw, kept.sx,
                                kept.sy, kept.sz};
      for (float f : fields) {
        SPARSE_CHECK(Bits(f) == expected);
      }
    }
  }
}

// 3. The >256 timepoints animation exercises 16-bit packed timepoint
// indices; 0/1 boundaries and duplicate times across tracks are covered.
SPARSE_CASE(packed_timepoints) {
  auto animation = BuildMainAnimation();
  SPARSE_CHECK(animation->timepoints().size() > 256);
  SparseSamplingJob::Context context;
  SPARSE_CHECK(context.Prepare(animation.get()));

  const int joints[] = {0, 1, 4};
  const float ratios[] = {0.f, 1.f, .0033f, .5f, .9967f, .137f};
  for (float ratio : ratios) {
    const auto reference = ReferenceSample(animation.get(), ratio);
    const auto sparse = SparseSample(
        animation.get(), &context, ozz::make_span(joints), ratio);
    for (int joint : joints) {
      ExpectLaneEq(ReadLane(reference[joint / 4], joint % 4),
                   ReadLane(sparse[joint / 4], joint % 4));
    }
  }

  // Re-Prepare is supported and keeps producing correct indexes.
  SPARSE_CHECK(context.Prepare(animation.get()));
  {
    const auto reference = ReferenceSample(animation.get(), .42f);
    const auto sparse = SparseSample(
        animation.get(), &context, ozz::make_span(joints), .42f);
    for (int joint : joints) {
      ExpectLaneEq(ReadLane(reference[joint / 4], joint % 4),
                   ReadLane(sparse[joint / 4], joint % 4));
    }
  }

  // Time comparison bound with a high-K track: track 0 holds 301 translation
  // keys and the selection covers 2 groups, so the bound is
  // 12G(ceil(log2(K+1))+2) with K = 301, G = 2.
  {
    SparseSamplingJob::Stats stats;
    SparseSample(animation.get(), &context, ozz::make_span(joints), .61f,
                 &stats);
    constexpr uint32_t k = 301;
    uint32_t log2 = 0;
    while ((1u << log2) < k + 1u) {
      ++log2;
    }
    SPARSE_CHECK(stats.time_comparisons <= 12u * 2u * (log2 + 2u));
    SPARSE_CHECK(stats.float3_decompress_blocks <= 4u * 2u);
    SPARSE_CHECK(stats.quaternion_decompress_blocks <= 2u * 2u);
    SPARSE_CHECK(stats.interpolated_groups == 2u);
  }
}

// 4. Real workload counters on a 1024-joint animation, mask switching and
// zero allocation during Run().
SPARSE_CASE(work_and_allocations) {
  RawAnimation raw;
  raw.duration = 1.f;
  raw.tracks.resize(1024);
  for (int joint = 0; joint < 1024; ++joint) {
    raw.tracks[joint].translations.push_back(
        {0.f, ozz::math::Float3(0.f, 0.f, 0.f)});
    raw.tracks[joint].translations.push_back(
        {1.f, ozz::math::Float3(static_cast<float>(joint), 1.f, -1.f)});
    raw.tracks[joint].rotations.push_back(
        {0.f, TestQuaternion(joint % 7, 0)});
    raw.tracks[joint].rotations.push_back(
        {1.f, TestQuaternion(joint % 7, 3)});
    raw.tracks[joint].scales.push_back(
        {0.f, ozz::math::Float3(1.f, 1.f, 1.f)});
    raw.tracks[joint].scales.push_back(
        {1.f, ozz::math::Float3(2.f, .5f, 1.f)});
  }
  auto animation = AnimationBuilder()(raw);
  SPARSE_CHECK(animation);
  SPARSE_CHECK(animation->num_soa_tracks() == 256);

  CountingAllocator counter(ozz::memory::default_allocator());
  ozz::memory::Allocator* previous =
      ozz::memory::SetDefaulAllocator(&counter);

  SparseSamplingJob::Context context;
  SPARSE_CHECK(context.Prepare(animation.get()));
  SPARSE_CHECK(counter.allocations >= 1);  // Preparation does allocate.

  // One selected SoA group: at most (and here exactly, without caching)
  // 4 float3 blocks, 2 quaternion blocks and 1 group interpolation.
  {
    const int one_group[] = {0};
    SparseSamplingJob::Stats stats;
    const int before_alloc = counter.allocations;
    const int before_dealloc = counter.deallocations;
    const auto sparse = SparseSample(
        animation.get(), &context, ozz::make_span(one_group), .6f, &stats);
    SPARSE_CHECK(counter.allocations == before_alloc);
    SPARSE_CHECK(counter.deallocations == before_dealloc);
    SPARSE_CHECK(stats.float3_decompress_blocks == 4);
    SPARSE_CHECK(stats.quaternion_decompress_blocks == 2);
    SPARSE_CHECK(stats.interpolated_groups == 1);
    SPARSE_CHECK(stats.time_comparisons > 0);

    const auto reference = ReferenceSample(animation.get(), .6f);
    ExpectLaneEq(ReadLane(reference[0], 0), ReadLane(sparse[0], 0));

    // K = 2 keys per track/channel: bound is 12G(ceil(log2(K+1))+2).
    SPARSE_CHECK(stats.time_comparisons <= 12u * 1u * 4u);
  }

  // All 256 groups selected: the counters expose the real, bounded work.
  {
    std::vector<int> all_joints;
    all_joints.reserve(1024);
    for (int j = 0; j < 1024; ++j) {
      all_joints.push_back(j);
    }
    SparseSamplingJob::Stats stats;
    const int before_alloc = counter.allocations;
    const int before_dealloc = counter.deallocations;
    const auto sparse =
        SparseSample(animation.get(), &context, ozz::make_span(all_joints),
                     .3f, &stats);
    SPARSE_CHECK(counter.allocations == before_alloc);
    SPARSE_CHECK(counter.deallocations == before_dealloc);
    SPARSE_CHECK(stats.float3_decompress_blocks == 256u * 4u);
    SPARSE_CHECK(stats.quaternion_decompress_blocks == 256u * 2u);
    SPARSE_CHECK(stats.interpolated_groups == 256u);
    SPARSE_CHECK(stats.time_comparisons <= 12u * 256u * 4u);
    SPARSE_CHECK(stats.time_comparisons > 0);

    const auto reference = ReferenceSample(animation.get(), .3f);
    for (int joint : {0, 1, 511, 512, 1023}) {
      ExpectLaneEq(ReadLane(reference[joint / 4], joint % 4),
                   ReadLane(sparse[joint / 4], joint % 4));
    }
  }

  // Switching masks, including from and to an empty set, must not allocate,
  // and must preserve unselected bit patterns.
  std::vector<ozz::math::SoaTransform> output(
      static_cast<size_t>(animation->num_soa_tracks()));
  Poison(ozz::make_span(output));

  const std::vector<std::vector<int>> masks = {
      {7}, {}, {1023}, {0}, {255}};
  for (const auto& mask : masks) {
    const int before_alloc = counter.allocations;
    const int before_dealloc = counter.deallocations;

    // Snapshot the posture before the run: unselected lanes must come out
    // bit-identical to what they were going in, regardless of what previous
    // masks wrote there.
    const std::vector<ozz::math::SoaTransform> previous = output;

    SparseSamplingJob job;
    job.animation = animation.get();
    job.context = &context;
    job.joint_indices = ozz::make_span(mask);
    job.ratio = .5f;
    job.output = ozz::make_span(output);
    SPARSE_CHECK(job.Run());
    SPARSE_CHECK(counter.allocations == before_alloc);
    SPARSE_CHECK(counter.deallocations == before_dealloc);

    std::vector<bool> selected(1024, false);
    for (int j : mask) {
      selected[j] = true;
    }
    for (int joint = 0; joint < 1024; ++joint) {
      if (selected[joint]) {
        continue;
      }
      // Unselected lane: every float keeps its pre-run bit pattern.
      const int group = joint / 4;
      const int lane = joint % 4;
      const LaneTransform kept = ReadLane(output[group], lane);
      const LaneTransform was = ReadLane(previous[group], lane);
      const float kept_f[10] = {kept.tx, kept.ty, kept.tz, kept.rx,
                                kept.ry, kept.rz, kept.rw, kept.sx,
                                kept.sy, kept.sz};
      const float was_f[10] = {was.tx, was.ty, was.tz, was.rx,
                               was.ry, was.rz, was.rw, was.sx,
                               was.sy, was.sz};
      for (int f = 0; f < 10; ++f) {
        SPARSE_CHECK(Bits(kept_f[f]) == Bits(was_f[f]));
      }
    }
  }

  // After every sparse Run() above, the counters never moved (checked
  // per-run). At teardown time, the only live ozz allocation is the context's
  // own one: Run() neither allocated nor freed, and the reference SamplingJob
  // contexts already released theirs.
  SPARSE_CHECK(counter.live == 1);

  ozz::memory::SetDefaulAllocator(previous);
}

// 5. Archive round trip, context isolation, invalidation and all validation
// failures; failures leave output, context state and stats untouched.
SPARSE_CASE(archive_and_errors) {
  auto animation = BuildMainAnimation();

  // In-memory archive round trip of the untouched compressed animation.
  ozz::io::MemoryStream stream;
  {
    ozz::io::OArchive archive(&stream);
    archive << *animation;
  }
  stream.Seek(0, ozz::io::Stream::kSet);
  Animation restored;
  {
    ozz::io::IArchive archive(&stream);
    archive >> restored;
  }
  SPARSE_CHECK(restored.num_tracks() == animation->num_tracks());

  SparseSamplingJob::Context context;
  SPARSE_CHECK(context.Prepare(animation.get()));

  // Re-Prepare on the deserialized animation, then sample from it.
  SPARSE_CHECK(context.Prepare(&restored));
  SPARSE_CHECK(context.animation() == &restored);
  {
    const int joints[] = {0, 2, 4};
    const auto reference = ReferenceSample(&restored, .55f);
    const auto sparse = SparseSample(
        &restored, &context, ozz::make_span(joints), .55f);
    for (int joint : joints) {
      ExpectLaneEq(ReadLane(reference[joint / 4], joint % 4),
                   ReadLane(sparse[joint / 4], joint % 4));
    }
  }

  // Two contexts on two animations do not pollute each other.
  auto other_animation = BuildMainAnimation();
  SparseSamplingJob::Context context_a;
  SparseSamplingJob::Context context_b;
  SPARSE_CHECK(context_a.Prepare(animation.get()));
  SPARSE_CHECK(context_b.Prepare(other_animation.get()));
  const int shared_joints[] = {1, 3};
  for (float ratio : {.2f, .8f, .4f}) {
    const auto ref_a = ReferenceSample(animation.get(), ratio);
    const auto ref_b = ReferenceSample(other_animation.get(), ratio);
    const auto out_a = SparseSample(
        animation.get(), &context_a, ozz::make_span(shared_joints), ratio);
    const auto out_b =
        SparseSample(other_animation.get(), &context_b,
                     ozz::make_span(shared_joints), ratio);
    for (int joint : shared_joints) {
      ExpectLaneEq(ReadLane(ref_a[joint / 4], joint % 4),
                   ReadLane(out_a[joint / 4], joint % 4));
      ExpectLaneEq(ReadLane(ref_b[joint / 4], joint % 4),
                   ReadLane(out_b[joint / 4], joint % 4));
    }
  }

  // Invalidation detaches the context; Prepare(nullptr) keeps state intact.
  SPARSE_CHECK(!context_a.Prepare(nullptr));
  SPARSE_CHECK(context_a.animation() == animation.get());
  context_a.Invalidate();
  SPARSE_CHECK(context_a.animation() == nullptr);
  SPARSE_CHECK(context_a.Prepare(animation.get()));

  // Move construction transfers the prepared indexes without dangling
  // pointers: the moved-from context is unprepared, the moved-to one samples
  // correctly.
  SparseSamplingJob::Context moved(std::move(context_a));
  SPARSE_CHECK(moved.animation() == animation.get());
  SPARSE_CHECK(context_a.animation() == nullptr);
  {
    const int joints[] = {0, 3};
    const auto reference = ReferenceSample(animation.get(), .7f);
    const auto sparse = SparseSample(
        animation.get(), &moved, ozz::make_span(joints), .7f);
    for (int joint : joints) {
      ExpectLaneEq(ReadLane(reference[joint / 4], joint % 4),
                   ReadLane(sparse[joint / 4], joint % 4));
    }
  }

  // Move assignment works too, including over an already prepared context.
  SparseSamplingJob::Context assigned;
  SPARSE_CHECK(assigned.Prepare(other_animation.get()));
  assigned = std::move(moved);
  SPARSE_CHECK(assigned.animation() == animation.get());
  SPARSE_CHECK(moved.animation() == nullptr);
  {
    const int joints[] = {2};
    const auto reference = ReferenceSample(animation.get(), .15f);
    const auto sparse = SparseSample(
        animation.get(), &assigned, ozz::make_span(joints), .15f);
    ExpectLaneEq(ReadLane(reference[0], 2), ReadLane(sparse[0], 2));
  }

  // --- Validation failures, all must return false without side effects. ---
  // context_a was moved from above, so prepare it again for the checks.
  SPARSE_CHECK(context_a.Prepare(animation.get()));
  std::vector<ozz::math::SoaTransform> output(
      static_cast<size_t>(animation->num_soa_tracks()));
  Poison(ozz::make_span(output));
  const std::vector<uint32_t> output_snapshot(
      reinterpret_cast<uint32_t*>(output.data()),
      reinterpret_cast<uint32_t*>(output.data()) +
          output.size() * sizeof(ozz::math::SoaTransform) / sizeof(uint32_t));
  SparseSamplingJob::Stats sentinel;
  sentinel.time_comparisons = 12345;
  sentinel.float3_decompress_blocks = 678;
  sentinel.quaternion_decompress_blocks = 910;
  sentinel.interpolated_groups = 111;
  const int good_joints[] = {0, 4};

  auto expect_failure = [&](const SparseSamplingJob& _job) {
    SPARSE_CHECK(!_job.Validate());
    SPARSE_CHECK(!_job.Run());
    SPARSE_CHECK(context_a.animation() == animation.get());
    SPARSE_CHECK(sentinel.time_comparisons == 12345);
    const uint32_t* raw = reinterpret_cast<uint32_t*>(output.data());
    for (size_t i = 0; i < output_snapshot.size(); ++i) {
      SPARSE_CHECK(raw[i] == output_snapshot[i]);
    }
  };

  SparseSamplingJob job;
  job.animation = animation.get();
  job.context = &context_a;
  job.joint_indices = ozz::make_span(good_joints);
  job.ratio = .5f;
  job.output = ozz::make_span(output);
  job.stats = &sentinel;

  // Null animation.
  {
    SparseSamplingJob invalid = job;
    invalid.animation = nullptr;
    expect_failure(invalid);
  }
  // Null context.
  {
    SparseSamplingJob invalid = job;
    invalid.context = nullptr;
    expect_failure(invalid);
  }
  // Animation/context mismatch.
  {
    SparseSamplingJob invalid = job;
    invalid.animation = other_animation.get();
    expect_failure(invalid);
  }
  // Unprepared context.
  {
    SparseSamplingJob::Context unprepared;
    SparseSamplingJob invalid = job;
    invalid.context = &unprepared;
    expect_failure(invalid);
  }
  // Output too small and empty output.
  {
    SparseSamplingJob invalid = job;
    invalid.output = ozz::span<ozz::math::SoaTransform>(output.data(), 1);
    SPARSE_CHECK(animation->num_soa_tracks() > 1);
    expect_failure(invalid);
    invalid.output = {};
    expect_failure(invalid);
  }
  // Joint out of range, negative, unsorted, duplicated.
  {
    const int out_of_range[] = {0, 5};
    SparseSamplingJob invalid = job;
    invalid.joint_indices = ozz::make_span(out_of_range);
    expect_failure(invalid);
  }
  {
    const int negative[] = {-1, 2};
    SparseSamplingJob invalid = job;
    invalid.joint_indices = ozz::make_span(negative);
    expect_failure(invalid);
  }
  {
    const int unsorted[] = {3, 1};
    SparseSamplingJob invalid = job;
    invalid.joint_indices = ozz::make_span(unsorted);
    expect_failure(invalid);
  }
  {
    const int duplicated[] = {1, 1};
    SparseSamplingJob invalid = job;
    invalid.joint_indices = ozz::make_span(duplicated);
    expect_failure(invalid);
  }
  // Non finite ratios.
  {
    SparseSamplingJob invalid = job;
    invalid.ratio = std::numeric_limits<float>::infinity();
    expect_failure(invalid);
    invalid.ratio = std::numeric_limits<float>::quiet_NaN();
    expect_failure(invalid);
  }

  // The context is still usable after every failed attempt.
  SparseSamplingJob::Stats stats;
  const auto still_good = SparseSample(
      animation.get(), &context_a, ozz::make_span(good_joints), .5f, &stats);
  const auto reference = ReferenceSample(animation.get(), .5f);
  for (int joint : good_joints) {
    ExpectLaneEq(ReadLane(reference[joint / 4], joint % 4),
                 ReadLane(still_good[joint / 4], joint % 4));
  }
  SPARSE_CHECK(stats.interpolated_groups >= 1);
}
