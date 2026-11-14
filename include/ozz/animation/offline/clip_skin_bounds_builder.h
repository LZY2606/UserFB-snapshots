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

#ifndef OZZ_OZZ_ANIMATION_OFFLINE_CLIP_SKIN_BOUNDS_BUILDER_H_
#define OZZ_OZZ_ANIMATION_OFFLINE_CLIP_SKIN_BOUNDS_BUILDER_H_

#include <stddef.h>
#include <stdint.h>

#include "ozz/animation/offline/export.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/span.h"

namespace ozz {
namespace animation {
class Skeleton;
namespace offline {
struct RawAnimation;

// Computes a conservative axis-aligned bounding box of every skinned vertex
// position of a mesh, over a continuous time interval of a RawAnimation.
//
// Unlike keyframe-only bounding (which misses extrema reached between keys,
// e.g. a vertex swung by a rotating joint) or fixed-rate sampling (which gives
// no guarantee between samples), this builder evaluates the animation as the
// continuous curves defined by the RawAnimation per-channel interpolation
// rules, and proves containment with interval arithmetic:
//
//  - The time interval [start, end] is partitioned into segments. Each segment
//    is evaluated with interval arithmetic in double precision. Every
//    elementary operation (add, multiply, division, square root) rounds its
//    result outward with std::nextafter, so the computed intervals provably
//    contain the exact real-valued results.
//  - Translation and scale channels are piecewise linear in time, so their
//    exact min/max over a segment are found at the segment ends and at the key
//    times inside it.
//  - Rotation channels follow normalized linear interpolation (nlerp) of two
//    keys qa, qb (qb first flipped into qa hemisphere). The un-normalized
//    value u(a) = lerp(qa, qb', a) is linear in a, hence exactly bounded. The
//    squared norm |u(a)|^2 is a quadratic in a whose exact min/max over the
//    segment are computed in closed form. Adjacent keys are required to have
//    an absolute dot product of at least 1e-3, which keeps the normalization
//    denominator |u| >= sqrt((1 + dot) / 2) > 0.7, away from the half-turn
//    sign ambiguity. Each quaternion component is then bounded by interval
//    division u_i / |u|, which stays inclusive because interval division
//    contains every quotient of contained values.
//  - Local TRS matrices are composed into interval affine matrices, and model
//    matrices are accumulated along the real parent chain with interval matrix
//    products. Interval matrix multiplication is inclusion monotone: if every
//    true matrix entry lies inside its interval, so does every entry of the
//    product.
//  - Skinning uses the same rule as ozz::geometry::SkinningJob: a vertex
//    position is sum_k w_k * (model(joint_k) * inverse_bind(joint_k)) * v,
//    with exact (non-interval) weights, inverse-bind matrices and bind
//    positions. Non-negative weights keep the weighted sum inclusive.
//  - The union of all segment boxes therefore contains every vertex position
//    at every time in [start, end]. Segment boundary times are also evaluated
//    exactly (point sampling in double precision) and used as witnesses; the
//    reported box is the union of the interval boxes and those samples.
//
// Tightness certificate: for each of the 6 box faces the builder returns a
// witness (a vertex index, a time inside [start, end], and the position of
// that vertex at that time). The axial distance from a face to its witness is
// non-negative (the box contains every true position, hence every witness).
// When that distance is at most Options::epsilon for all 6 faces, the result
// is Status::kComplete: the box is proven to contain the whole swept volume,
// and proven (by actual positions) to overestimate each face by no more than
// epsilon. No fixed-step sampling or empirical padding is involved in the
// guarantee.
//
// Budget behavior: Options::max_subdivisions bounds how many times a time
// segment may be bisected (each bisection replaces one segment by two and adds
// one exact sample). The initial whole-interval evaluation is always
// performed, so even a budget of 0 yields a valid finite outer box. If the
// budget runs out before every face meets epsilon, the result is
// Status::kIncomplete and still reports the valid outer box, the current
// witnesses, and Result::max_face_gap (the largest face-to-witness distance),
// so the caller knows how loose the bound remains. kIncomplete is never
// reported as kComplete.
//
// Invalid inputs (wrong track or matrix counts, non-finite or non-affine
// inverse-bind matrices, empty vertex array, non-finite positions, negative or
// non-normalized weights, joint indices out of range, rotation keys closer
// than the 1e-3 dot-product limit, invalid time interval, negative or
// non-finite epsilon, or numerical overflow during evaluation) make Build()
// return false and leave the output Result untouched. Inputs are never
// modified. Morph targets, dual quaternion skinning and loop extrapolation are
// not handled.
//
// Usage example:
//  ozz::animation::offline::ClipSkinBoundsBuilder builder;
//  ozz::animation::offline::ClipSkinBoundsBuilder::Options options;
//  options.start = 0.0;
//  options.end = raw_animation.duration;
//  options.epsilon = 1e-3;
//  options.max_subdivisions = 1024;
//  ozz::animation::offline::ClipSkinBoundsBuilder::Result result;
//  if (builder.Build(skeleton, raw_animation, inverse_bind_matrices, vertices,
//                    options, &result)) {
//    if (result.status ==
//        ozz::animation::offline::ClipSkinBoundsBuilder::Status::kComplete) {
//      // result.min / result.max bound every vertex at every time, and each
//      // face is within options.epsilon of an actual vertex position
//      // (result.witnesses).
//    } else {
//      // Box is still a valid outer bound, but up to result.max_face_gap
//      // away from the best known position on some face.
//    }
//  }
class OZZ_ANIMOFFLINE_DLL ClipSkinBoundsBuilder {
 public:
  // Maximum number of joint influences per vertex.
  enum { kMaxInfluences = 4 };

  // Identifies the 6 faces of the output box, used to index
  // Result::witnesses.
  enum Face {
    kMinX = 0,
    kMaxX = 1,
    kMinY = 2,
    kMaxY = 3,
    kMinZ = 4,
    kMaxZ = 5,
    kFaceCount = 6
  };

  // A static vertex, with its position in bind space and 1 to 4 joint
  // influences.
  struct Vertex {
    // Position in bind space. Must be finite.
    math::Float3 position;

    // Indices of the influencing joints. Only the first influence_count
    // entries are read. Repeated joints are allowed. Every read index must be
    // smaller than the skeleton joint count.
    uint16_t joints[kMaxInfluences];

    // Weights of the influencing joints. Only the first influence_count
    // entries are read. Read weights must be finite, non-negative, and sum to
    // 1 (a 1e-3 absolute tolerance is accepted on the sum).
    float weights[kMaxInfluences];

    // Number of influences, in [1, kMaxInfluences].
    int influence_count;
  };

  // Build parameters.
  struct Options {
    // Time interval, in animation seconds. Must satisfy
    // 0 <= start <= end <= animation duration. A degenerate interval
    // (start == end) is supported and evaluates a single pose.
    double start = 0.0;
    double end = 0.0;

    // Tightness tolerance. The result is kComplete only when every face is
    // within epsilon of an actual vertex position. Must be finite and >= 0.
    double epsilon = 1e-3;

    // Maximum number of segment bisections. The initial whole-interval
    // evaluation is always performed and does not count against this budget,
    // so any budget value still produces a valid outer box.
    size_t max_subdivisions = 1024;
  };

  // Completeness status of a Result.
  enum class Status {
    // Every vertex position at every time of the interval is inside the box,
    // and every face is within Options::epsilon of its witness position.
    kComplete,
    // The box is still a valid finite outer bound, but the subdivision budget
    // ran out before every face could be certified within epsilon.
    // Result::max_face_gap reports the remaining worst face-to-witness
    // distance.
    kIncomplete,
  };

  // Proof that a box face is not overestimated: an actual vertex position,
  // reached at an actual time of the interval.
  struct Witness {
    // Index of the vertex (in the input vertex array) that reached Position.
    size_t vertex_index;

    // Time, inside [Options::start, Options::end], at which the position was
    // reached.
    double time;

    // Position of the vertex at that time.
    double position[3];
  };

  // Output of Build(). Only written when Build() returns true.
  struct Result {
    Status status;

    // Conservative axis-aligned bounding box of all skinned vertex positions
    // over the whole time interval. Always finite when Build() returns true.
    double min[3];
    double max[3];

    // One witness per box face, indexed by Face (kMinX .. kMaxZ). For
    // kComplete results, the axial distance from each face to its witness is
    // in [0, Options::epsilon].
    Witness witnesses[kFaceCount];

    // Largest axial distance from a box face to its witness, over the 6
    // faces. Always >= 0. At most Options::epsilon for kComplete results;
    // greater for kIncomplete ones.
    double max_face_gap;
  };

  // Computes the skin bounds. Returns true if the computation ran (the result
  // status must then be checked). Returns false on invalid input or numerical
  // overflow, in which case *_result is left unmodified. No input is modified.
  bool Build(const Skeleton& _skeleton, const RawAnimation& _animation,
             const span<const math::Float4x4>& _inverse_bind_matrices,
             const span<const Vertex>& _vertices, const Options& _options,
             Result* _result) const;
};

}  // namespace offline
}  // namespace animation
}  // namespace ozz
#endif  // OZZ_OZZ_ANIMATION_OFFLINE_CLIP_SKIN_BOUNDS_BUILDER_H_
