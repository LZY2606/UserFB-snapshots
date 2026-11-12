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

#include "ozz/animation/runtime/sparse_sampling_job.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include "ozz/animation/runtime/animation.h"
#include "ozz/base/maths/math_constant.h"
#include "ozz/base/maths/math_ex.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/memory/allocator.h"

// Internal include file
#define OZZ_INCLUDE_PRIVATE_HEADER  // Allows to include private headers.
#include "animation/runtime/animation_keyframe.h"

namespace ozz {
namespace animation {

namespace internal {
struct SparseInterpSoaFloat3 {
  math::SimdFloat4 ratio[2];
  math::SoaFloat3 value[2];
};
struct SparseInterpSoaQuaternion {
  math::SimdFloat4 ratio[2];
  math::SoaQuaternion value[2];
};
}  // namespace internal

bool SparseSamplingJob::Validate() const {
  // Null pointers.
  if (!animation || !context) {
    return false;
  }

  // The context must have been prepared for this very animation.
  if (context->animation_ != animation) {
    return false;
  }

  // Ratio must be finite.
  if (!std::isfinite(ratio)) {
    return false;
  }

  const int num_tracks = animation->num_tracks();
  const int num_soa_tracks = animation->num_soa_tracks();

  // Output must be able to store the whole animation posture, even though only
  // the selected groups will be written.
  if (output.empty() ||
      static_cast<int>(output.size()) < num_soa_tracks) {
    return false;
  }

  // Joint indices must be in range, strictly ascending and not duplicated.
  int previous = -1;
  for (int joint : joint_indices) {
    if (joint <= previous || joint < 0 || joint >= num_tracks) {
      return false;
    }
    previous = joint;
  }

  return true;
}

// Internal helpers. A uniquely named namespace nested in an anonymous one is
// required so that this translation unit can be fused together with
// sampling_job.cc, which defines same-named helpers in its own anonymous
// namespace.
namespace {
namespace sparse_job_internal {

// Reads the timepoint index stored for key _at in a component ratios array.
inline uint32_t KeyTimePointIndex(
    const ozz::span<const float>& _timepoints,
    const ozz::span<const byte>& _ratios, size_t _at) {
  if (_timepoints.size() <= std::numeric_limits<uint8_t>::max()) {
    return reinterpret_span<const uint8_t>(_ratios)[_at];
  }
  return reinterpret_span<const uint16_t>(_ratios)[_at];
}

inline float KeyTime(const ozz::span<const float>& _timepoints,
                     const ozz::span<const byte>& _ratios, size_t _at) {
  return _timepoints[KeyTimePointIndex(_timepoints, _ratios, _at)];
}

inline ozz::math::SimdFloat4 KeysTime(
    const ozz::span<const float>& _timepoints,
    const ozz::span<const byte>& _ratios,
    const uint32_t _ats[4]) {
  if (_timepoints.size() <= std::numeric_limits<uint8_t>::max()) {
    const auto& ratios = reinterpret_span<const uint8_t>(_ratios);
    return ozz::math::simd_float4::Load(
        _timepoints[ratios[_ats[0]]], _timepoints[ratios[_ats[1]]],
        _timepoints[ratios[_ats[2]]], _timepoints[ratios[_ats[3]]]);
  }
  const auto& ratios = reinterpret_span<const uint16_t>(_ratios);
  return ozz::math::simd_float4::Load(
      _timepoints[ratios[_ats[0]]], _timepoints[ratios[_ats[1]]],
      _timepoints[ratios[_ats[2]]], _timepoints[ratios[_ats[3]]]);
}

// Locates, for one track, the global key index of the key immediately at or
// after _ratio (the "right" interpolation key). The track's keys, in time
// ascending order, are _key_index[_start .. _end): the compressed stream is
// interleaved across tracks, so the per-track sequence is reconstructed from
// the previouses chain during Prepare(). Every key time probe is accounted on
// _time_comparisons, at the point of the actual comparison.
inline uint32_t FindRightKey(
    const ozz::span<const float>& _timepoints,
    const Animation::KeyframesCtrlConst& _ctrl,
    const span<const uint32_t>& _key_index, uint32_t _start, uint32_t _end,
    float _ratio, uint32_t* _time_comparisons) {
  size_t lo = 0;
  size_t hi = static_cast<size_t>(_end - _start);
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    ++*_time_comparisons;  // Counted where the comparison is performed.
    if (KeyTime(_timepoints, _ctrl.ratios, _key_index[_start + mid]) <=
        _ratio) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }

  // lo keys have a time lower than or equal to _ratio. The right key is the
  // last of them, or the first/last key when _ratio falls outside the track
  // range (cannot happen with a clamped ratio, but keeps the function total).
  size_t right_offset = lo;
  const size_t count = static_cast<size_t>(_end - _start);
  if (right_offset == 0) {
    right_offset = 1;
  } else if (right_offset == count) {
    right_offset = count - 1;
  }
  return _key_index[_start + static_cast<uint32_t>(right_offset)];
}

inline void DecompressFloat3(const internal::Float3Key& _k0,
                             const internal::Float3Key& _k1,
                             const internal::Float3Key& _k2,
                             const internal::Float3Key& _k3,
                             math::SoaFloat3* _soa_float3) {
  _soa_float3->x = math::HalfToFloat(math::simd_int4::Load(
      _k0.values[0], _k1.values[0], _k2.values[0], _k3.values[0]));
  _soa_float3->y = math::HalfToFloat(math::simd_int4::Load(
      _k0.values[1], _k1.values[1], _k2.values[1], _k3.values[1]));
  _soa_float3->z = math::HalfToFloat(math::simd_int4::Load(
      _k0.values[2], _k1.values[2], _k2.values[2], _k3.values[2]));
}

// Defines a mapping table that defines components assignation in the output
// quaternion.
static constexpr uint8_t kCpntMapping[4][4] = {
    {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 1, 0, 2}, {0, 1, 2, 0}};

inline void DecompressQuaternion(const internal::QuaternionKey& _k0,
                                 const internal::QuaternionKey& _k1,
                                 const internal::QuaternionKey& _k2,
                                 const internal::QuaternionKey& _k3,
                                 math::SoaQuaternion* _quaternion) {
  int largests[4], signs[4], values[4][3];
  internal::unpack(_k0, largests[0], signs[0], values[0]);
  internal::unpack(_k1, largests[1], signs[1], values[1]);
  internal::unpack(_k2, largests[2], signs[2], values[2]);
  internal::unpack(_k3, largests[3], signs[3], values[3]);

  const uint8_t* m0 = kCpntMapping[largests[0]];
  const uint8_t* m1 = kCpntMapping[largests[1]];
  const uint8_t* m2 = kCpntMapping[largests[2]];
  const uint8_t* m3 = kCpntMapping[largests[3]];

  alignas(16) int cmp_keys[4][4] = {
      {values[0][m0[0]], values[1][m1[0]], values[2][m2[0]], values[3][m3[0]]},
      {values[0][m0[1]], values[1][m1[1]], values[2][m2[1]], values[3][m3[1]]},
      {values[0][m0[2]], values[1][m1[2]], values[2][m2[2]], values[3][m3[2]]},
      {values[0][m0[3]], values[1][m1[3]], values[2][m2[3]], values[3][m3[3]]},
  };

  const math::SimdFloat4 kScale =
      math::simd_float4::Load1(math::kSqrt2 / internal::QuaternionKey::kfScale);
  const math::SimdFloat4 kOffset = math::simd_float4::Load1(-math::kSqrt2_2);
  math::SimdFloat4 cpnt[4] = {
      kScale * math::simd_float4::FromInt(
                   math::simd_int4::LoadPtr(cmp_keys[0])) +
          kOffset,
      kScale * math::simd_float4::FromInt(
                   math::simd_int4::LoadPtr(cmp_keys[1])) +
          kOffset,
      kScale * math::simd_float4::FromInt(
                   math::simd_int4::LoadPtr(cmp_keys[2])) +
          kOffset,
      kScale * math::simd_float4::FromInt(
                   math::simd_int4::LoadPtr(cmp_keys[3])) +
          kOffset};

  const math::SimdInt4 mask_f000 = math::simd_int4::mask_f000();
  const math::SimdInt4 mask_0f00 = math::simd_int4::mask_0f00();
  const math::SimdInt4 mask_00f0 = math::simd_int4::mask_00f0();
  const math::SimdInt4 mask_000f = math::simd_int4::mask_000f();
  cpnt[largests[0]] = math::AndNot(cpnt[largests[0]], mask_f000);
  cpnt[largests[1]] = math::AndNot(cpnt[largests[1]], mask_0f00);
  cpnt[largests[2]] = math::AndNot(cpnt[largests[2]], mask_00f0);
  cpnt[largests[3]] = math::AndNot(cpnt[largests[3]], mask_000f);

  const math::SimdFloat4 dot = cpnt[0] * cpnt[0] + cpnt[1] * cpnt[1] +
                               cpnt[2] * cpnt[2] + cpnt[3] * cpnt[3];
  const math::SimdFloat4 ww0 = math::simd_float4::one() - dot;
  const math::SimdFloat4 w0 = ww0 * math::RSqrtEst(ww0);

  const math::SimdInt4 sign = math::ShiftL(
      math::simd_int4::Load(signs[0], signs[1], signs[2], signs[3]), 31);
  const math::SimdFloat4 restored = math::Or(w0, sign);

  cpnt[largests[0]] =
      math::Or(cpnt[largests[0]], math::And(restored, mask_f000));
  cpnt[largests[1]] =
      math::Or(cpnt[largests[1]], math::And(restored, mask_0f00));
  cpnt[largests[2]] =
      math::Or(cpnt[largests[2]], math::And(restored, mask_00f0));
  cpnt[largests[3]] =
      math::Or(cpnt[largests[3]], math::And(restored, mask_000f));

  _quaternion->x = cpnt[0];
  _quaternion->y = cpnt[1];
  _quaternion->z = cpnt[2];
  _quaternion->w = cpnt[3];
}

// Decompresses the two interpolation keys (left and right) of one SoA group
// of a Float3 component (translations or scales). One 4-lane block per side.
template <typename _Interp>
void DecompressFloat3Group(
    const ozz::span<const float>& _timepoints,
    const Animation::KeyframesCtrlConst& _ctrl,
    const ozz::span<const internal::Float3Key>& _compressed,
    const uint32_t _rights[4], _Interp* _interp,
    SparseSamplingJob::Stats* _stats) {
  const uint32_t lefts[4] = {
      _rights[0] - _ctrl.previouses[_rights[0]],
      _rights[1] - _ctrl.previouses[_rights[1]],
      _rights[2] - _ctrl.previouses[_rights[2]],
      _rights[3] - _ctrl.previouses[_rights[3]]};

  _interp->ratio[0] = KeysTime(_timepoints, _ctrl.ratios, lefts);
  DecompressFloat3(_compressed[lefts[0]], _compressed[lefts[1]],
                   _compressed[lefts[2]], _compressed[lefts[3]],
                   &_interp->value[0]);
  ++_stats->float3_decompress_blocks;

  _interp->ratio[1] = KeysTime(_timepoints, _ctrl.ratios, _rights);
  DecompressFloat3(_compressed[_rights[0]], _compressed[_rights[1]],
                   _compressed[_rights[2]], _compressed[_rights[3]],
                   &_interp->value[1]);
  ++_stats->float3_decompress_blocks;
}

// Decompresses the two interpolation keys (left and right) of one SoA group
// of the rotation component. One 4-lane block per side.
void DecompressQuaternionGroup(
    const ozz::span<const float>& _timepoints,
    const Animation::KeyframesCtrlConst& _ctrl,
    const ozz::span<const internal::QuaternionKey>& _compressed,
    const uint32_t _rights[4], internal::SparseInterpSoaQuaternion* _interp,
    SparseSamplingJob::Stats* _stats) {
  const uint32_t lefts[4] = {
      _rights[0] - _ctrl.previouses[_rights[0]],
      _rights[1] - _ctrl.previouses[_rights[1]],
      _rights[2] - _ctrl.previouses[_rights[2]],
      _rights[3] - _ctrl.previouses[_rights[3]]};

  _interp->ratio[0] = KeysTime(_timepoints, _ctrl.ratios, lefts);
  DecompressQuaternion(_compressed[lefts[0]], _compressed[lefts[1]],
                       _compressed[lefts[2]], _compressed[lefts[3]],
                       &_interp->value[0]);
  ++_stats->quaternion_decompress_blocks;

  _interp->ratio[1] = KeysTime(_timepoints, _ctrl.ratios, _rights);
  DecompressQuaternion(_compressed[_rights[0]], _compressed[_rights[1]],
                       _compressed[_rights[2]], _compressed[_rights[3]],
                       &_interp->value[1]);
  ++_stats->quaternion_decompress_blocks;
}

// Writes lane _lane of a 4-float SIMD register without touching the other
// lanes. This preserves unselected output lanes bit for bit, including NaN
// sentinels and negative zeros.
inline void StoreLane(math::SimdFloat4& _dst, math::SimdFloat4 _src,
                      int _lane) {
  alignas(16) float dst[4];
  alignas(16) float src[4];
  math::StorePtrU(_dst, dst);
  math::StorePtrU(_src, src);
  dst[_lane] = src[_lane];
  _dst = math::simd_float4::LoadPtrU(dst);
}

// Builds, for one component, two indexes from its untouched compressed data:
// - _key_index lists, for every track and in time ascending order, the global
//   indices of the track's keys. The compressed stream is interleaved across
//   tracks and sorted by (time, track), so a track's keys are not contiguous;
//   track membership is recovered through the previouses offsets.
// - _track_starts[t] is the offset of track t's first entry in _key_index,
//   with an extra sentinel entry holding the total number of keys.
// The whole component key stream is scanned a constant number of times,
// linearly. _track_of and _cursor are scratch spans owned by the context.
void BuildComponentIndex(const Animation::KeyframesCtrlConst& _ctrl,
                         int _num_soa_tracks, span<uint32_t> _key_index,
                         span<uint32_t> _track_starts,
                         span<uint32_t> _track_of,
                         span<uint32_t> _cursor) {
  const uint32_t num_keys = static_cast<uint32_t>(_ctrl.previouses.size());
  const uint32_t num_tracks = static_cast<uint32_t>(_num_soa_tracks) * 4;

  // First pass: recover the track of every key. A null previous offset marks
  // a track's first key (the builder lays the first key of every track at
  // the beginning of the stream, in track order); any other key belongs to
  // the same track as its predecessor, whose index is always smaller and
  // already resolved.
  uint32_t track_count = 0;
  for (uint32_t key = 0; key < num_keys; ++key) {
    const uint16_t previous = _ctrl.previouses[key];
    if (previous == 0) {
      assert(track_count < num_tracks);
      _track_of[key] = track_count++;
    } else {
      _track_of[key] = _track_of[key - previous];
    }
  }
  assert(track_count == num_tracks && "Every SoA lane must have keyframes.");

  // Second pass: count keys per track, then turn counts into start offsets.
  for (uint32_t track = 0; track <= num_tracks; ++track) {
    _track_starts[track] = 0;
  }
  for (uint32_t key = 0; key < num_keys; ++key) {
    ++_track_starts[_track_of[key]];
  }
  uint32_t sum = 0;
  for (uint32_t track = 0; track < num_tracks; ++track) {
    const uint32_t count = _track_starts[track];
    _track_starts[track] = sum;
    sum += count;
  }
  _track_starts[num_tracks] = sum;
  assert(sum == num_keys);

  // Third pass: fill each track's ordered key list. The global stream is
  // time-sorted, so appending in stream order yields ascending times.
  for (uint32_t track = 0; track < num_tracks; ++track) {
    _cursor[track] = _track_starts[track];
  }
  for (uint32_t key = 0; key < num_keys; ++key) {
    const uint32_t track = _track_of[key];
    _key_index[_cursor[track]++] = key;
  }
}

}  // namespace sparse_job_internal
}  // namespace

bool SparseSamplingJob::Run() const {
  using namespace sparse_job_internal;

  if (!Validate()) {
    return false;
  }

  // Validation succeeded: from here on the output, context state and stats are
  // only touched through the documented sampling operations. A local sink is
  // used when the caller didn't provide counters.
  Stats local_stats;
  Stats* const st = stats ? stats : &local_stats;
  *st = Stats{};

  // Early out for an empty joint set: legal, but nothing is decompressed nor
  // written.
  if (joint_indices.empty()) {
    return true;
  }

  const int num_soa_tracks = animation->num_soa_tracks();

  // Clamps ratio in range [0,1], as SamplingJob does.
  const float clamped_ratio = math::Clamp(0.f, ratio, 1.f);

  const span<const float> timepoints = animation->timepoints();
  const Animation::KeyframesCtrlConst& translations_ctrl =
      animation->translations_ctrl();
  const Animation::KeyframesCtrlConst& rotations_ctrl =
      animation->rotations_ctrl();
  const Animation::KeyframesCtrlConst& scales_ctrl = animation->scales_ctrl();

  const math::SimdFloat4 anim_ratio =
      math::simd_float4::Load1(clamped_ratio);

  // Iterates selected SoA groups. joint_indices is strictly ascending, so
  // groups are visited in ascending order without duplicates.
  size_t selection = 0;
  int previous_group = -1;
  while (selection < joint_indices.size()) {
    const int first_joint = joint_indices[selection];
    const int group = first_joint / 4;
    assert(group != previous_group);
    previous_group = group;
    assert(group < num_soa_tracks);

    // Lanes of this group that are actually selected. Other lanes still have
    // to be decoded (the decompression works on 4-lane blocks) but their
    // result must not be written to the output.
    bool selected_lane[4] = {false, false, false, false};
    size_t next = selection;
    while (next < joint_indices.size() &&
           joint_indices[next] / 4 == group) {
      selected_lane[joint_indices[next] % 4] = true;
      ++next;
    }
    selection = next;

    // Locates the right interpolation key of each of the 4 lanes of this
    // group. Unselected lanes of the group are read too, which is required by
    // the 4-lane decompression; groups with no selected joint are never
    // reached.
    uint32_t t_rights[4], r_rights[4], s_rights[4];
    for (int lane = 0; lane < 4; ++lane) {
      const int track = group * 4 + lane;
      t_rights[lane] = FindRightKey(
          timepoints, translations_ctrl, context->translations_.key_index,
          context->translations_.track_starts[track],
          context->translations_.track_starts[track + 1], clamped_ratio,
          &st->time_comparisons);
      r_rights[lane] = FindRightKey(
          timepoints, rotations_ctrl, context->rotations_.key_index,
          context->rotations_.track_starts[track],
          context->rotations_.track_starts[track + 1], clamped_ratio,
          &st->time_comparisons);
      s_rights[lane] = FindRightKey(
          timepoints, scales_ctrl, context->scales_.key_index,
          context->scales_.track_starts[track],
          context->scales_.track_starts[track + 1], clamped_ratio,
          &st->time_comparisons);
    }

    // Decompresses only this group's keys.
    internal::SparseInterpSoaFloat3 translations;
    internal::SparseInterpSoaQuaternion rotations;
    internal::SparseInterpSoaFloat3 scales;

    DecompressFloat3Group(timepoints, translations_ctrl,
                          animation->translations_values(), t_rights,
                          &translations, st);
    DecompressQuaternionGroup(timepoints, rotations_ctrl,
                              animation->rotations_values(), r_rights,
                              &rotations, st);
    DecompressFloat3Group(timepoints, scales_ctrl,
                          animation->scales_values(), s_rights, &scales, st);

    // Interpolation coefficients, identical to SamplingJob's formulas.
    const math::SimdFloat4 t_ratio =
        (anim_ratio - translations.ratio[0]) *
        math::RcpEst(translations.ratio[1] - translations.ratio[0]);
    const math::SimdFloat4 r_ratio =
        (anim_ratio - rotations.ratio[0]) *
        math::RcpEst(rotations.ratio[1] - rotations.ratio[0]);
    const math::SimdFloat4 s_ratio =
        (anim_ratio - scales.ratio[0]) *
        math::RcpEst(scales.ratio[1] - scales.ratio[0]);

    // Computes the whole 4-lane group, but only selected lanes are written:
    // unselected lanes keep their exact output bit pattern (NaN sentinels
    // and negative zeros included).
    const math::SoaFloat3 out_translation =
        Lerp(translations.value[0], translations.value[1], t_ratio);
    const math::SoaQuaternion out_rotation =
        NLerpEst(rotations.value[0], rotations.value[1], r_ratio);
    const math::SoaFloat3 out_scale =
        Lerp(scales.value[0], scales.value[1], s_ratio);

    const math::SimdFloat4 t_xyz[3] = {out_translation.x, out_translation.y,
                                       out_translation.z};
    const math::SimdFloat4 r_xyzw[4] = {out_rotation.x, out_rotation.y,
                                        out_rotation.z, out_rotation.w};
    const math::SimdFloat4 s_xyz[3] = {out_scale.x, out_scale.y, out_scale.z};

    for (int lane = 0; lane < 4; ++lane) {
      if (!selected_lane[lane]) {
        continue;
      }
      StoreLane(output[group].translation.x, t_xyz[0], lane);
      StoreLane(output[group].translation.y, t_xyz[1], lane);
      StoreLane(output[group].translation.z, t_xyz[2], lane);
      StoreLane(output[group].rotation.x, r_xyzw[0], lane);
      StoreLane(output[group].rotation.y, r_xyzw[1], lane);
      StoreLane(output[group].rotation.z, r_xyzw[2], lane);
      StoreLane(output[group].rotation.w, r_xyzw[3], lane);
      StoreLane(output[group].scale.x, s_xyz[0], lane);
      StoreLane(output[group].scale.y, s_xyz[1], lane);
      StoreLane(output[group].scale.z, s_xyz[2], lane);
    }

    ++st->interpolated_groups;
  }

  return true;
}

SparseSamplingJob::Context::Context() = default;

SparseSamplingJob::Context::Context(Context&& _other) noexcept
    : animation_(_other.animation_),
      num_soa_tracks_(_other.num_soa_tracks_),
      allocation_(_other.allocation_),
      allocation_size_(_other.allocation_size_),
      translations_(_other.translations_),
      rotations_(_other.rotations_),
      scales_(_other.scales_) {
  _other.animation_ = nullptr;
  _other.num_soa_tracks_ = 0;
  _other.allocation_ = nullptr;
  _other.allocation_size_ = 0;
  _other.translations_ = Component{};
  _other.rotations_ = Component{};
  _other.scales_ = Component{};
}

SparseSamplingJob::Context& SparseSamplingJob::Context::operator=(
    Context&& _other) noexcept {
  if (this != &_other) {
    Deallocate();
    animation_ = _other.animation_;
    num_soa_tracks_ = _other.num_soa_tracks_;
    allocation_ = _other.allocation_;
    allocation_size_ = _other.allocation_size_;
    translations_ = _other.translations_;
    rotations_ = _other.rotations_;
    scales_ = _other.scales_;

    _other.animation_ = nullptr;
    _other.num_soa_tracks_ = 0;
    _other.allocation_ = nullptr;
    _other.allocation_size_ = 0;
    _other.translations_ = Component{};
    _other.rotations_ = Component{};
    _other.scales_ = Component{};
  }
  return *this;
}

SparseSamplingJob::Context::~Context() { Deallocate(); }

void SparseSamplingJob::Context::Deallocate() {
  memory::default_allocator()->Deallocate(allocation_);
  allocation_ = nullptr;
  allocation_size_ = 0;
  animation_ = nullptr;
  num_soa_tracks_ = 0;
  translations_ = Component{};
  rotations_ = Component{};
  scales_ = Component{};
}

bool SparseSamplingJob::Context::Prepare(const Animation* _animation) {
  using namespace sparse_job_internal;

  if (!_animation) {
    return false;  // Keeps any existing prepared state untouched.
  }

  const int num_soa_tracks = _animation->num_soa_tracks();
  const size_t num_tracks = static_cast<size_t>(num_soa_tracks) * 4;

  // Per component: one ordered global key index per compressed key, plus one
  // start offset per track and a trailing sentinel. The track-of and cursor
  // arrays are preparation scratch, also backed by the same single
  // allocation.
  const size_t t_keys = _animation->translations_ctrl().previouses.size();
  const size_t r_keys = _animation->rotations_ctrl().previouses.size();
  const size_t s_keys = _animation->scales_ctrl().previouses.size();
  const size_t index_count = t_keys + r_keys + s_keys;
  const size_t starts_count = (num_tracks + 1) * 3;
  const size_t track_of_count = index_count;
  const size_t cursor_count = num_tracks * 3;
  const size_t size =
      (index_count + starts_count + track_of_count + cursor_count) *
      sizeof(uint32_t);
  void* allocation =
      memory::default_allocator()->Allocate(size, alignof(uint32_t));

  span<byte> buffer = {static_cast<byte*>(allocation), size};
  Component translations;
  Component rotations;
  Component scales;
  translations.key_index = fill_span<uint32_t>(buffer, t_keys);
  rotations.key_index = fill_span<uint32_t>(buffer, r_keys);
  scales.key_index = fill_span<uint32_t>(buffer, s_keys);
  translations.track_starts = fill_span<uint32_t>(buffer, num_tracks + 1);
  rotations.track_starts = fill_span<uint32_t>(buffer, num_tracks + 1);
  scales.track_starts = fill_span<uint32_t>(buffer, num_tracks + 1);
  const span<uint32_t> t_track_of = fill_span<uint32_t>(buffer, t_keys);
  const span<uint32_t> r_track_of = fill_span<uint32_t>(buffer, r_keys);
  const span<uint32_t> s_track_of = fill_span<uint32_t>(buffer, s_keys);
  const span<uint32_t> t_cursor = fill_span<uint32_t>(buffer, num_tracks);
  const span<uint32_t> r_cursor = fill_span<uint32_t>(buffer, num_tracks);
  const span<uint32_t> s_cursor = fill_span<uint32_t>(buffer, num_tracks);
  assert(buffer.empty());

  // Scans every compressed key of every component. This is the preparation
  // cost: it is linear in the total number of keys and never happens again
  // until the next Prepare().
  BuildComponentIndex(_animation->translations_ctrl(), num_soa_tracks,
                      translations.key_index, translations.track_starts,
                      t_track_of, t_cursor);
  BuildComponentIndex(_animation->rotations_ctrl(), num_soa_tracks,
                      rotations.key_index, rotations.track_starts,
                      r_track_of, r_cursor);
  BuildComponentIndex(_animation->scales_ctrl(), num_soa_tracks,
                      scales.key_index, scales.track_starts, s_track_of,
                      s_cursor);

  // Replace the previous state only once the new indexes are fully built.
  Deallocate();
  animation_ = _animation;
  num_soa_tracks_ = num_soa_tracks;
  allocation_ = allocation;
  allocation_size_ = size;
  translations_ = translations;
  rotations_ = rotations;
  scales_ = scales;

  return true;
}

void SparseSamplingJob::Context::Invalidate() {
  // Keeps the allocations, only detaches from the compressed animation data.
  animation_ = nullptr;
}
}  // namespace animation
}  // namespace ozz
