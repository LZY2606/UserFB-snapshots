// Tests for ozz::animation::offline::ClipSkinBoundsBuilder, registered through
// task_support/check.h (task_cases / task_case_count).
//
// The tests build their own branching skeleton and small mesh, and cover:
//  - bounds: conservative boxes on a forked, multi-root skeleton with
//    per-channel key times, a child bone translated under a rotating parent,
//    two-bone weights with non-identity inverse binds, non-uniform and
//    negative scales, an interior rotation extremum that endpoints and all
//    original keys miss, a degenerate (single time) interval, preservation of
//    the caller's result on invalid inputs, and insufficient budget behavior.
//  - tightness: independent analytic special cases (static pose, linear
//    translation, single rotating bone) checking the slack of all six faces.
//  - integration: cross-checks of containment and witnesses against the
//    existing ozz pipeline (offline SampleAnimation, LocalToModelJob and
//    geometry SkinningJob) at many times, never against the builder's own
//    internal sampling.

#include "task_support/check.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ozz/animation/offline/clip_skin_bounds_builder.h"
#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/offline/raw_animation_utils.h"
#include "ozz/animation/offline/raw_skeleton.h"
#include "ozz/animation/offline/skeleton_builder.h"
#include "ozz/animation/runtime/local_to_model_job.h"
#include "ozz/animation/runtime/skeleton.h"
#include "ozz/base/maths/quaternion.h"
#include "ozz/base/maths/simd_math.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/maths/transform.h"
#include "ozz/base/maths/vec_float.h"
#include "ozz/base/memory/unique_ptr.h"
#include "ozz/base/span.h"
#include "ozz/geometry/runtime/skinning_job.h"

namespace {

using ozz::animation::Skeleton;
using ozz::animation::offline::ClipSkinBoundsBuilder;
using ozz::animation::offline::RawAnimation;
using ozz::math::Float3;
using ozz::math::Float4x4;
using ozz::math::Quaternion;

const double kPi = 3.14159265358979323846;
const double kSqrt3_2 = 0.86602540378443865;  // cos(30 degrees)
const double kCrossTol = 1e-4;  // Allowed float/double cross-check tolerance.

Quaternion RotAxis(double _deg, double _x, double _y, double _z) {
  const double half = _deg * kPi / 360.0;
  return Quaternion(static_cast<float>(_x * std::sin(half)),
                    static_cast<float>(_y * std::sin(half)),
                    static_cast<float>(_z * std::sin(half)),
                    static_cast<float>(std::cos(half)));
}
Quaternion RotZ(double _deg) { return RotAxis(_deg, 0.0, 0.0, 1.0); }
Quaternion RotX(double _deg) { return RotAxis(_deg, 1.0, 0.0, 0.0); }
Quaternion RotY(double _deg) { return RotAxis(_deg, 0.0, 1.0, 0.0); }

ClipSkinBoundsBuilder::Vertex MakeVertex(float _x, float _y, float _z,
                                         const uint16_t* _joints,
                                         const float* _weights, int _count) {
  ClipSkinBoundsBuilder::Vertex v;
  v.position = Float3(_x, _y, _z);
  v.influence_count = _count;
  for (int k = 0; k < ClipSkinBoundsBuilder::kMaxInfluences; ++k) {
    v.joints[k] = k < _count ? _joints[k] : 0;
    v.weights[k] = k < _count ? _weights[k] : 0.0f;
  }
  return v;
}

// ---------------------------------------------------------------------------
// Forked, multi-root skeleton: A (root) -> {B, C}, plus a second root D.
// Joint indices after SkeletonBuilder depth-first ordering: A=0, B=1, C=2,
// D=3.
// ---------------------------------------------------------------------------
ozz::unique_ptr<Skeleton> BuildForkedSkeleton() {
  ozz::animation::offline::RawSkeleton raw;
  raw.roots.resize(2);
  raw.roots[0].name = "A";
  raw.roots[0].children.resize(2);
  raw.roots[0].children[0].name = "B";
  raw.roots[0].children[0].transform.translation = Float3(0.0f, 1.0f, 0.0f);
  raw.roots[0].children[1].name = "C";
  raw.roots[0].children[1].transform.translation = Float3(0.0f, 0.0f, 2.0f);
  raw.roots[1].name = "D";
  raw.roots[1].transform.translation = Float3(5.0f, 0.0f, 0.0f);
  ozz::animation::offline::SkeletonBuilder builder;
  return builder(raw);
}

// Animation with different key times per channel, a child translating under
// its rotating parent, non-uniform and negative scales, and a second root.
RawAnimation MakeForkedAnimation() {
  RawAnimation raw;
  raw.duration = 1.0f;
  raw.tracks.resize(4);
  // A: rotation keys at [0, 1], translation keys at [0.25, 0.75].
  raw.tracks[0].rotations.push_back({0.0f, Quaternion::identity()});
  raw.tracks[0].rotations.push_back({1.0f, RotZ(90.0)});
  raw.tracks[0].translations.push_back({0.25f, Float3(0.0f, 0.0f, 0.0f)});
  raw.tracks[0].translations.push_back({0.75f, Float3(1.0f, 0.0f, 0.0f)});
  // B (child of A): translates while its parent rotates.
  raw.tracks[1].translations.push_back({0.0f, Float3(0.0f, 1.0f, 0.0f)});
  raw.tracks[1].translations.push_back({0.5f, Float3(0.0f, 2.0f, 0.0f)});
  raw.tracks[1].translations.push_back({1.0f, Float3(0.0f, 1.0f, 0.0f)});
  raw.tracks[1].rotations.push_back({0.2f, Quaternion::identity()});
  raw.tracks[1].rotations.push_back({0.8f, RotX(60.0)});
  // C: non-uniform and negative scale, translation at yet other times.
  raw.tracks[2].scales.push_back({0.0f, Float3(1.0f, 1.0f, 1.0f)});
  raw.tracks[2].scales.push_back({1.0f, Float3(2.0f, -1.0f, 0.5f)});
  raw.tracks[2].translations.push_back({0.1f, Float3(0.0f, 0.0f, 2.0f)});
  raw.tracks[2].translations.push_back({0.9f, Float3(0.0f, 1.0f, 2.0f)});
  // D: second root, translates and rotates.
  raw.tracks[3].translations.push_back({0.0f, Float3(5.0f, 0.0f, 0.0f)});
  raw.tracks[3].translations.push_back({1.0f, Float3(5.0f, 3.0f, 0.0f)});
  raw.tracks[3].rotations.push_back({0.0f, Quaternion::identity()});
  raw.tracks[3].rotations.push_back({1.0f, RotY(-40.0)});
  return raw;
}

// Non-identity inverse-bind matrices for some joints.
std::vector<Float4x4> MakeForkedInverseBinds() {
  std::vector<Float4x4> binds(4);
  binds[0] = Float4x4::identity();
  binds[1] = Float4x4::FromAffine(Float3(-0.5f, 0.25f, 1.0f), RotZ(30.0),
                                  Float3(0.5f, 2.0f, 1.0f));
  binds[2] = Float4x4::FromAffine(Float3(1.0f, 1.0f, 1.0f),
                                  Quaternion::identity(),
                                  Float3(2.0f, 2.0f, 2.0f));
  binds[3] = Float4x4::identity();
  return binds;
}

// Small mesh: single, two, three and repeated-joint influences.
std::vector<ClipSkinBoundsBuilder::Vertex> MakeForkedVertices() {
  std::vector<ClipSkinBoundsBuilder::Vertex> vertices;
  {
    const uint16_t j[] = {1};
    const float w[] = {1.0f};
    vertices.push_back(MakeVertex(0.5f, 0.0f, 0.0f, j, w, 1));
  }
  {
    const uint16_t j[] = {0, 1};
    const float w[] = {0.25f, 0.75f};
    vertices.push_back(MakeVertex(0.0f, 0.5f, 0.5f, j, w, 2));
  }
  {
    const uint16_t j[] = {2};
    const float w[] = {1.0f};
    vertices.push_back(MakeVertex(-1.0f, 0.0f, 0.25f, j, w, 1));
  }
  {
    const uint16_t j[] = {3};
    const float w[] = {1.0f};
    vertices.push_back(MakeVertex(0.25f, 0.25f, 0.0f, j, w, 1));
  }
  {
    const uint16_t j[] = {0, 1, 2};
    const float w[] = {0.5f, 0.25f, 0.25f};
    vertices.push_back(MakeVertex(0.1f, -0.2f, 0.3f, j, w, 3));
  }
  {
    // Repeated joint is allowed.
    const uint16_t j[] = {3, 3};
    const float w[] = {0.5f, 0.5f};
    vertices.push_back(MakeVertex(0.0f, 0.0f, 0.1f, j, w, 2));
  }
  return vertices;
}

// ---------------------------------------------------------------------------
// Single bone rotating from 0 to 120 degrees about z, driving the off-axis
// point (cos30, sin30, 0). The point reaches y = 1 in the middle of the
// rotation (at t = 0.5), while both endpoints (and only keys) give y = 0.5.
// ---------------------------------------------------------------------------
ozz::unique_ptr<Skeleton> BuildSingleJointSkeleton() {
  ozz::animation::offline::RawSkeleton raw;
  raw.roots.resize(1);
  raw.roots[0].name = "root";
  ozz::animation::offline::SkeletonBuilder builder;
  return builder(raw);
}

RawAnimation MakeRotationAnimation() {
  RawAnimation raw;
  raw.duration = 1.0f;
  raw.tracks.resize(1);
  raw.tracks[0].rotations.push_back({0.0f, Quaternion::identity()});
  raw.tracks[0].rotations.push_back({1.0f, RotZ(120.0)});
  return raw;
}

std::vector<ClipSkinBoundsBuilder::Vertex> MakeArcVertices() {
  const uint16_t j[] = {0};
  const float w[] = {1.0f};
  std::vector<ClipSkinBoundsBuilder::Vertex> vertices;
  vertices.push_back(MakeVertex(static_cast<float>(kSqrt3_2), 0.5f, 0.0f, j, w,
                                1));
  return vertices;
}

// Analytic description of the swept arc, computed from the exact double value
// of the float vertex position (cos30 is not exactly representable as a
// float, so the radius is not exactly 1) and of the float rotation key.
struct ArcAnalytic {
  double px, py;   // Vertex position in bind space (double of the floats).
  double radius;   // Distance from the rotation axis.
  double phi0;     // Initial angle of the vertex, about 30 degrees.
  double phi1;     // Final swept angle, phi0 + (angle of the key quaternion).
  double min_x, max_x, min_y, max_y;  // Extremes over the swept arc.
};

ArcAnalytic ComputeArcAnalytic(const RawAnimation& _animation) {
  ArcAnalytic a;
  a.px = static_cast<double>(static_cast<float>(kSqrt3_2));
  a.py = 0.5;
  a.radius = std::sqrt(a.px * a.px + a.py * a.py);
  a.phi0 = std::atan2(a.py, a.px);
  // Swept angle of the actual animation quaternion: its components are float
  // (sin(60) is not exactly representable), so the total rotation is not
  // exactly 120 degrees.
  const double kz = _animation.tracks[0].rotations[1].value.z;
  const double kw = _animation.tracks[0].rotations[1].value.w;
  a.phi1 = a.phi0 + 2.0 * std::atan2(kz, kw);
  // The pose at t = 1 uses the raw (not exactly unit) key value as-is, with
  // matrix entries 1-2z^2 and 2zw.
  const double ka = 1.0 - 2.0 * kz * kz;
  const double kb = 2.0 * kz * kw;
  const double x_end = ka * a.px - kb * a.py;
  const double y_end = kb * a.px + ka * a.py;
  // Cosine decreases over [phi0, phi1] (within [0, 180 degrees]).
  a.max_x = a.px;
  a.min_x = std::min(a.radius * std::cos(a.phi1), x_end);
  // Sine reaches 1 at 90 degrees, inside the swept range.
  a.max_y = a.radius;
  a.min_y = std::min(std::min(a.py, a.radius * std::sin(a.phi1)), y_end);
  return a;
}

// Analytic position of the arc vertex at time t, following the RawAnimation
// rules in double precision: clamped to key values outside [0, 1] (the last
// key is used as-is, even though it is not exactly unit), hemisphere-fixed
// normalized lerp inside.
void AnalyticArcPosition(const RawAnimation& _animation, double _t,
                         double _out[3]) {
  const ArcAnalytic a = ComputeArcAnalytic(_animation);
  const double kz = _animation.tracks[0].rotations[1].value.z;
  const double kw = _animation.tracks[0].rotations[1].value.w;
  double qz, qw;
  if (_t <= 0.0) {
    qz = 0.0;
    qw = 1.0;
  } else if (_t >= 1.0) {
    qz = kz;
    qw = kw;
  } else {
    qz = _t * kz;
    qw = (1.0 - _t) + _t * kw;
    const double n = std::sqrt(qz * qz + qw * qw);
    qz /= n;
    qw /= n;
  }
  // Matrix entries of the (possibly non-unit) quaternion, as computed by
  // ozz::math::Float4x4::FromAffine.
  const double m00 = 1.0 - 2.0 * qz * qz;
  const double m01 = -2.0 * qz * qw;
  const double m10 = 2.0 * qz * qw;
  const double m11 = m00;
  _out[0] = m00 * a.px + m01 * a.py;
  _out[1] = m10 * a.px + m11 * a.py;
  _out[2] = 0.0;
}

// ---------------------------------------------------------------------------
// Independent double precision 3x4 affine matrix helpers, used by the
// analytic tightness cases (never by the builder itself).
// ---------------------------------------------------------------------------
struct M34 {
  double m[3][4];
};

M34 FromFloat4x4(const Float4x4& _m) {
  M34 r;
  const float rows[3][4] = {
      {ozz::math::GetX(_m.cols[0]), ozz::math::GetX(_m.cols[1]),
       ozz::math::GetX(_m.cols[2]), ozz::math::GetX(_m.cols[3])},
      {ozz::math::GetY(_m.cols[0]), ozz::math::GetY(_m.cols[1]),
       ozz::math::GetY(_m.cols[2]), ozz::math::GetY(_m.cols[3])},
      {ozz::math::GetZ(_m.cols[0]), ozz::math::GetZ(_m.cols[1]),
       ozz::math::GetZ(_m.cols[2]), ozz::math::GetZ(_m.cols[3])}};
  for (int i = 0; i < 3; ++i) {
    for (int c = 0; c < 4; ++c) {
      r.m[i][c] = rows[i][c];
    }
  }
  return r;
}

void ApplyPoint(const M34& _m, const double _v[3], double _out[3]) {
  for (int r = 0; r < 3; ++r) {
    _out[r] = _m.m[r][0] * _v[0] + _m.m[r][1] * _v[1] + _m.m[r][2] * _v[2] +
              _m.m[r][3];
  }
}

// Analytically skins a vertex with identity model matrices (static pose).
void AnalyticStaticSkin(const std::vector<Float4x4>& _binds,
                        const ClipSkinBoundsBuilder::Vertex& _v,
                        double _out[3]) {
  _out[0] = _out[1] = _out[2] = 0.0;
  const double vp[3] = {_v.position.x, _v.position.y, _v.position.z};
  for (int k = 0; k < _v.influence_count; ++k) {
    const M34 bind = FromFloat4x4(_binds[_v.joints[k]]);
    double p[3];
    ApplyPoint(bind, vp, p);
    for (int c = 0; c < 3; ++c) {
      _out[c] += _v.weights[k] * p[c];
    }
  }
}

// ---------------------------------------------------------------------------
// Runs the pre-existing ozz pipeline (offline SampleAnimation ->
// LocalToModelJob -> geometry SkinningJob) at one time, and returns the
// skinned position of every vertex. This is the independent reference used to
// cross-check the builder; the builder's own sampling is never used here.
// ---------------------------------------------------------------------------
bool SkinWithOzzPipeline(
    const Skeleton& _skeleton, const RawAnimation& _animation,
    const std::vector<Float4x4>& _binds,
    const std::vector<ClipSkinBoundsBuilder::Vertex>& _vertices, float _time,
    std::vector<float>* _out) {
  const int joint_count = _skeleton.num_joints();
  std::vector<ozz::math::Transform> locals(joint_count);
  if (!ozz::animation::offline::SampleAnimation(_animation, _time,
                                                ozz::make_span(locals))) {
    return false;
  }
  // Packs local transforms into SoA form for LocalToModelJob.
  const int soa_count = _skeleton.num_soa_joints();
  std::vector<ozz::math::SoaTransform> soa(soa_count);
  for (int s = 0; s < soa_count; ++s) {
    float tx[4] = {0, 0, 0, 0}, ty[4] = {0, 0, 0, 0}, tz[4] = {0, 0, 0, 0};
    float rx[4] = {0, 0, 0, 0}, ry[4] = {0, 0, 0, 0}, rz[4] = {0, 0, 0, 0};
    float rw[4] = {1, 1, 1, 1};
    float sx[4] = {1, 1, 1, 1}, sy[4] = {1, 1, 1, 1}, sz[4] = {1, 1, 1, 1};
    for (int lane = 0; lane < 4; ++lane) {
      const int j = s * 4 + lane;
      if (j >= joint_count) {
        break;
      }
      tx[lane] = locals[j].translation.x;
      ty[lane] = locals[j].translation.y;
      tz[lane] = locals[j].translation.z;
      rx[lane] = locals[j].rotation.x;
      ry[lane] = locals[j].rotation.y;
      rz[lane] = locals[j].rotation.z;
      rw[lane] = locals[j].rotation.w;
      sx[lane] = locals[j].scale.x;
      sy[lane] = locals[j].scale.y;
      sz[lane] = locals[j].scale.z;
    }
    soa[s].translation.x = ozz::math::simd_float4::Load(tx[0], tx[1], tx[2], tx[3]);
    soa[s].translation.y = ozz::math::simd_float4::Load(ty[0], ty[1], ty[2], ty[3]);
    soa[s].translation.z = ozz::math::simd_float4::Load(tz[0], tz[1], tz[2], tz[3]);
    soa[s].rotation.x = ozz::math::simd_float4::Load(rx[0], rx[1], rx[2], rx[3]);
    soa[s].rotation.y = ozz::math::simd_float4::Load(ry[0], ry[1], ry[2], ry[3]);
    soa[s].rotation.z = ozz::math::simd_float4::Load(rz[0], rz[1], rz[2], rz[3]);
    soa[s].rotation.w = ozz::math::simd_float4::Load(rw[0], rw[1], rw[2], rw[3]);
    soa[s].scale.x = ozz::math::simd_float4::Load(sx[0], sx[1], sx[2], sx[3]);
    soa[s].scale.y = ozz::math::simd_float4::Load(sy[0], sy[1], sy[2], sy[3]);
    soa[s].scale.z = ozz::math::simd_float4::Load(sz[0], sz[1], sz[2], sz[3]);
  }
  std::vector<Float4x4> models(joint_count);
  ozz::animation::LocalToModelJob ltm;
  ltm.skeleton = &_skeleton;
  ltm.input = ozz::make_span(soa);
  ltm.output = ozz::make_span(models);
  if (!ltm.Run()) {
    return false;
  }
  std::vector<Float4x4> palette(joint_count);
  for (int j = 0; j < joint_count; ++j) {
    palette[j] = models[j] * _binds[j];
  }
  _out->resize(_vertices.size() * 3);
  for (size_t i = 0; i < _vertices.size(); ++i) {
    const ClipSkinBoundsBuilder::Vertex& v = _vertices[i];
    const float in_position[3] = {v.position.x, v.position.y, v.position.z};
    float out_position[3] = {0.0f, 0.0f, 0.0f};
    ozz::geometry::SkinningJob job;
    job.vertex_count = 1;
    job.influences_count = v.influence_count;
    job.joint_matrices = ozz::make_span(palette);
    job.joint_indices =
        ozz::span<const uint16_t>(v.joints, v.influence_count);
    job.joint_indices_stride = 0;
    job.joint_weights =
        ozz::span<const float>(v.weights, v.influence_count - 1);
    job.joint_weights_stride = 0;
    job.in_positions = ozz::span<const float>(in_position, 3);
    job.in_positions_stride = 0;
    job.out_positions = ozz::span<float>(out_position, 3);
    job.out_positions_stride = 0;
    if (!job.Run()) {
      return false;
    }
    for (int c = 0; c < 3; ++c) {
      (*_out)[i * 3 + c] = out_position[c];
    }
  }
  return true;
}

bool CheckResultWellFormed(const ClipSkinBoundsBuilder::Result& _result,
                           size_t _vertex_count, double _start, double _end,
                           double _epsilon) {
  bool ok = true;
  for (int c = 0; c < 3; ++c) {
    ok &= std::isfinite(_result.min[c]) && std::isfinite(_result.max[c]);
    ok &= _result.min[c] <= _result.max[c];
  }
  ok &= _result.max_face_gap >= 0.0 && _result.max_face_gap <= _epsilon;
  for (int f = 0; f < ClipSkinBoundsBuilder::kFaceCount; ++f) {
    const ClipSkinBoundsBuilder::Witness& w = _result.witnesses[f];
    ok &= w.vertex_index < _vertex_count;
    ok &= w.time >= _start && w.time <= _end;
    const int axis = f >> 1;
    const double gap = (f & 1) == 0 ? w.position[axis] - _result.min[axis]
                                    : _result.max[axis] - w.position[axis];
    ok &= gap >= 0.0 && gap <= _epsilon;
    for (int c = 0; c < 3; ++c) {
      ok &= std::isfinite(w.position[c]);
      ok &= w.position[c] >= _result.min[c] - 1e-9 &&
            w.position[c] <= _result.max[c] + 1e-9;
    }
  }
  return ok;
}

// ---------------------------------------------------------------------------
// bounds group
// ---------------------------------------------------------------------------

int CaseForkedSkeletonBounds() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildForkedSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  TASK_CHECK(skeleton->num_joints() == 4);
  const RawAnimation raw = MakeForkedAnimation();
  TASK_CHECK(raw.Validate());
  const std::vector<Float4x4> binds = MakeForkedInverseBinds();
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices =
      MakeForkedVertices();

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 1e-3;
  options.max_subdivisions = 4096;
  ClipSkinBoundsBuilder::Result result;
  const ClipSkinBoundsBuilder builder;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result));
  TASK_CHECK(result.status == ClipSkinBoundsBuilder::Status::kComplete);
  TASK_CHECK(CheckResultWellFormed(result, vertices.size(), 0.0, 1.0, 1e-3));

  // The swept volume must be non-trivial: the child B rises to y = 2 in
  // parent space while A rotates, so the box must span more than the rest
  // pose. The inverse bind of B maps bind space far away, so only check that
  // the box is not degenerate.
  double extent = 0.0;
  for (int c = 0; c < 3; ++c) {
    extent += result.max[c] - result.min[c];
  }
  TASK_CHECK(extent > 1.0);
  return 0;
}

int CaseInteriorExtremumMissedByKeys() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildSingleJointSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  const RawAnimation raw = MakeRotationAnimation();
  TASK_CHECK(raw.Validate());
  const std::vector<Float4x4> binds(1, Float4x4::identity());
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices = MakeArcVertices();

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 1e-3;
  options.max_subdivisions = 4096;
  ClipSkinBoundsBuilder::Result result;
  const ClipSkinBoundsBuilder builder;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result));
  TASK_CHECK(result.status == ClipSkinBoundsBuilder::Status::kComplete);

  // The interior maximum y = radius (reached mid-rotation) is contained and
  // tight.
  const ArcAnalytic arc = ComputeArcAnalytic(raw);
  TASK_CHECK(result.max[1] >= arc.max_y - 1e-9);
  TASK_CHECK(result.max[1] <= arc.max_y + options.epsilon + 1e-9);
  const ClipSkinBoundsBuilder::Witness& top =
      result.witnesses[ClipSkinBoundsBuilder::kMaxY];
  TASK_CHECK(top.position[1] >= arc.max_y - options.epsilon - 1e-9);
  TASK_CHECK(top.position[1] <= arc.max_y + 1e-9);
  // The witness is reached at an interior time, not at a key.
  TASK_CHECK(top.time > 0.2 && top.time < 0.8);

  // Endpoints (which are also all the original keys) only reach y = 0.5:
  // merging keyframe positions would miss the interior maximum by ~0.5.
  double p[3];
  AnalyticArcPosition(raw, 0.0, p);
  TASK_CHECK(p[1] <= 0.5 + 1e-9);
  AnalyticArcPosition(raw, 1.0, p);
  TASK_CHECK(p[1] <= 0.5 + 1e-9);
  TASK_CHECK(result.max[1] >= 0.5 + 0.4);

  // Other faces match the analytic swept arc.
  TASK_CHECK(result.min[0] <= arc.min_x + 1e-9);
  TASK_CHECK(result.min[0] >= arc.min_x - options.epsilon - 1e-9);
  TASK_CHECK(result.max[0] >= arc.max_x - 1e-9);
  TASK_CHECK(result.max[0] <= arc.max_x + options.epsilon + 1e-9);
  TASK_CHECK(result.min[1] <= arc.min_y + 1e-9);
  TASK_CHECK(result.min[1] >= arc.min_y - options.epsilon - 1e-9);
  TASK_CHECK(result.min[2] <= 1e-9);
  TASK_CHECK(result.min[2] >= -options.epsilon - 1e-9);
  TASK_CHECK(result.max[2] >= -1e-9);
  TASK_CHECK(result.max[2] <= options.epsilon + 1e-9);
  return 0;
}

int CaseDegenerateInterval() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildSingleJointSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  const RawAnimation raw = MakeRotationAnimation();
  const std::vector<Float4x4> binds(1, Float4x4::identity());
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices = MakeArcVertices();

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.25;
  options.end = 0.25;  // Single time.
  options.epsilon = 0.0;
  options.max_subdivisions = 16;
  ClipSkinBoundsBuilder::Result result;
  const ClipSkinBoundsBuilder builder;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result));
  TASK_CHECK(result.status == ClipSkinBoundsBuilder::Status::kComplete);
  TASK_CHECK(result.max_face_gap == 0.0);
  // The box is a single point: the pose at t = 0.25.
  for (int c = 0; c < 3; ++c) {
    TASK_CHECK(result.max[c] - result.min[c] <= 1e-12);
  }
  double expected[3];
  AnalyticArcPosition(raw, 0.25, expected);
  for (int c = 0; c < 3; ++c) {
    TASK_CHECK(result.min[c] <= expected[c] + 1e-9);
    TASK_CHECK(result.max[c] >= expected[c] - 1e-9);
  }
  // All six witnesses are the same pose.
  for (int f = 0; f < ClipSkinBoundsBuilder::kFaceCount; ++f) {
    TASK_CHECK(result.witnesses[f].time == 0.25);
    for (int c = 0; c < 3; ++c) {
      TASK_CHECK(std::fabs(result.witnesses[f].position[c] - expected[c]) <=
                 1e-9);
    }
  }
  return 0;
}

int CaseInvalidInputsPreserveResult() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildSingleJointSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  const RawAnimation raw = MakeRotationAnimation();
  const std::vector<Float4x4> binds(1, Float4x4::identity());
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices = MakeArcVertices();
  const ClipSkinBoundsBuilder builder;

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 1e-3;
  options.max_subdivisions = 64;

  // Sentinel-filled result; every failed call must leave it bit-identical.
  ClipSkinBoundsBuilder::Result result;
  std::memset(&result, 0xCD, sizeof(result));
  std::vector<unsigned char> snapshot(sizeof(result));
  std::memcpy(snapshot.data(), &result, sizeof(result));

  const auto expect_failure = [&](const RawAnimation& anim,
                                  const std::vector<Float4x4>& b,
                                  const std::vector<ClipSkinBoundsBuilder::Vertex>& v,
                                  const ClipSkinBoundsBuilder::Options& opt) {
    const bool ok = builder.Build(*skeleton, anim, ozz::make_span(b),
                                  ozz::make_span(v), opt, &result);
    TASK_CHECK(!ok);
    TASK_CHECK(std::memcmp(snapshot.data(), &result, sizeof(result)) == 0);
  };

  // Weight sum not equal to 1.
  {
    std::vector<ClipSkinBoundsBuilder::Vertex> bad = vertices;
    bad[0].weights[0] = 0.5f;
    expect_failure(raw, binds, bad, options);
  }
  // Negative weight.
  {
    std::vector<ClipSkinBoundsBuilder::Vertex> bad = vertices;
    bad[0].influence_count = 2;
    bad[0].joints[1] = 0;
    bad[0].weights[0] = 1.5f;
    bad[0].weights[1] = -0.5f;
    expect_failure(raw, binds, bad, options);
  }
  // start > end.
  {
    ClipSkinBoundsBuilder::Options bad = options;
    bad.start = 0.75;
    bad.end = 0.25;
    expect_failure(raw, binds, vertices, bad);
  }
  // end beyond duration.
  {
    ClipSkinBoundsBuilder::Options bad = options;
    bad.end = 1.5;
    expect_failure(raw, binds, vertices, bad);
  }
  // Negative start.
  {
    ClipSkinBoundsBuilder::Options bad = options;
    bad.start = -0.5;
    bad.end = 0.5;
    expect_failure(raw, binds, vertices, bad);
  }
  // Negative epsilon.
  {
    ClipSkinBoundsBuilder::Options bad = options;
    bad.epsilon = -1e-3;
    expect_failure(raw, binds, vertices, bad);
  }
  // NaN epsilon.
  {
    ClipSkinBoundsBuilder::Options bad = options;
    bad.epsilon = std::nan("");
    expect_failure(raw, binds, vertices, bad);
  }
  // Track count != joint count.
  {
    RawAnimation bad = raw;
    bad.tracks.resize(2);
    expect_failure(bad, binds, vertices, options);
  }
  // Joint index out of range.
  {
    std::vector<ClipSkinBoundsBuilder::Vertex> bad = vertices;
    bad[0].joints[0] = 3;
    expect_failure(raw, binds, bad, options);
  }
  // Invalid influence counts.
  {
    std::vector<ClipSkinBoundsBuilder::Vertex> bad = vertices;
    bad[0].influence_count = 0;
    expect_failure(raw, binds, bad, options);
    bad[0].influence_count = 5;
    expect_failure(raw, binds, bad, options);
  }
  // No vertices.
  {
    const std::vector<ClipSkinBoundsBuilder::Vertex> empty;
    expect_failure(raw, binds, empty, options);
  }
  // Inverse-bind count mismatch.
  {
    const std::vector<Float4x4> empty;
    expect_failure(raw, empty, vertices, options);
  }
  // Non-affine inverse-bind matrix.
  {
    std::vector<Float4x4> bad = binds;
    bad[0].cols[3] = ozz::math::simd_float4::Load(0.0f, 0.0f, 0.0f, 2.0f);
    expect_failure(raw, bad, vertices, options);
  }
  // Non-finite vertex position.
  {
    std::vector<ClipSkinBoundsBuilder::Vertex> bad = vertices;
    bad[0].position.x = std::nan("");
    expect_failure(raw, binds, bad, options);
  }
  // Rotation keys closer than the 1e-3 dot limit (half-turn ambiguity).
  {
    RawAnimation bad = raw;
    bad.tracks[0].rotations[1].value = Quaternion(0.0f, 0.0f, 1.0f, 0.0f);
    expect_failure(bad, binds, vertices, options);
  }
  // Invalid animation (key time beyond duration).
  {
    RawAnimation bad = raw;
    bad.tracks[0].rotations.push_back({2.0f, Quaternion::identity()});
    expect_failure(bad, binds, vertices, options);
  }
  return 0;
}

int CaseInsufficientBudget() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildSingleJointSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  const RawAnimation raw = MakeRotationAnimation();
  const std::vector<Float4x4> binds(1, Float4x4::identity());
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices = MakeArcVertices();
  const ClipSkinBoundsBuilder builder;

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 1e-6;
  options.max_subdivisions = 0;  // Only the initial whole-interval evaluation.

  ClipSkinBoundsBuilder::Result result0;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result0));
  // Budget too small: must not claim completeness.
  TASK_CHECK(result0.status == ClipSkinBoundsBuilder::Status::kIncomplete);
  TASK_CHECK(result0.max_face_gap > options.epsilon);
  // The box is still a valid finite outer bound of the swept arc.
  const ArcAnalytic arc = ComputeArcAnalytic(raw);
  for (int c = 0; c < 3; ++c) {
    TASK_CHECK(std::isfinite(result0.min[c]) && std::isfinite(result0.max[c]));
  }
  TASK_CHECK(result0.min[0] <= arc.min_x + 1e-9);
  TASK_CHECK(result0.max[0] >= arc.max_x - 1e-9);
  TASK_CHECK(result0.min[1] <= arc.min_y + 1e-9);
  TASK_CHECK(result0.max[1] >= arc.max_y - 1e-9);
  TASK_CHECK(result0.min[2] <= 1e-9);
  TASK_CHECK(result0.max[2] >= -1e-9);
  // Witnesses are actual positions on the arc, at times in range. Note the
  // last rotation key is not exactly unit (float rounding of sin(60)), and
  // the RawAnimation clamping rule uses key values as-is. The pose at t = 1
  // therefore uses the matrix entries 1-2z^2 and 2zw of the raw key, whose
  // squared scale is a^2 + b^2 (not exactly 1).
  const double kz = raw.tracks[0].rotations[1].value.z;
  const double kw = raw.tracks[0].rotations[1].value.w;
  const double ka = 1.0 - 2.0 * kz * kz;
  const double kb = 2.0 * kz * kw;
  const double key_m2 = ka * ka + kb * kb;
  for (int f = 0; f < ClipSkinBoundsBuilder::kFaceCount; ++f) {
    const ClipSkinBoundsBuilder::Witness& w = result0.witnesses[f];
    TASK_CHECK(w.vertex_index == 0);
    TASK_CHECK(w.time >= 0.0 && w.time <= 1.0);
    const double scale2 = (w.time == 1.0) ? key_m2 : 1.0;
    const double r2 = w.position[0] * w.position[0] +
                      w.position[1] * w.position[1];
    TASK_CHECK(std::fabs(r2 - arc.radius * arc.radius * scale2) <= 1e-9);
    TASK_CHECK(std::fabs(w.position[2]) <= 1e-9);
  }

  // A larger (still insufficient) budget must not make the gap worse.
  options.max_subdivisions = 8;
  ClipSkinBoundsBuilder::Result result8;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result8));
  TASK_CHECK(result8.status == ClipSkinBoundsBuilder::Status::kIncomplete);
  TASK_CHECK(result8.max_face_gap > options.epsilon);
  TASK_CHECK(result8.max_face_gap <= result0.max_face_gap + 1e-12);
  // And the incomplete box still contains the complete one computed with a
  // sufficient budget.
  options.epsilon = 1e-3;
  options.max_subdivisions = 4096;
  ClipSkinBoundsBuilder::Result full;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &full));
  TASK_CHECK(full.status == ClipSkinBoundsBuilder::Status::kComplete);
  for (int c = 0; c < 3; ++c) {
    TASK_CHECK(result8.min[c] <= full.min[c] + 1e-9);
    TASK_CHECK(result8.max[c] >= full.max[c] - 1e-9);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// tightness group
// ---------------------------------------------------------------------------

int CaseStaticPoseExact() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildForkedSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  // All channels empty: every joint local transform is the identity, so every
  // model matrix is the identity and skinning reduces to the inverse binds.
  RawAnimation raw;
  raw.duration = 1.0f;
  raw.tracks.resize(4);
  TASK_CHECK(raw.Validate());
  const std::vector<Float4x4> binds = MakeForkedInverseBinds();
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices =
      MakeForkedVertices();

  // Independent analytic expectation.
  double expected_lo[3] = {1e30, 1e30, 1e30};
  double expected_hi[3] = {-1e30, -1e30, -1e30};
  std::vector<std::vector<double>> expected(vertices.size(),
                                            std::vector<double>(3));
  for (size_t i = 0; i < vertices.size(); ++i) {
    AnalyticStaticSkin(binds, vertices[i], expected[i].data());
    for (int c = 0; c < 3; ++c) {
      expected_lo[c] = std::min(expected_lo[c], expected[i][c]);
      expected_hi[c] = std::max(expected_hi[c], expected[i][c]);
    }
  }

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 1e-9;
  options.max_subdivisions = 64;
  ClipSkinBoundsBuilder::Result result;
  const ClipSkinBoundsBuilder builder;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result));
  TASK_CHECK(result.status == ClipSkinBoundsBuilder::Status::kComplete);
  // All six faces are (nearly) exactly tight on a static pose.
  TASK_CHECK(result.max_face_gap <= 1e-9);
  for (int c = 0; c < 3; ++c) {
    TASK_CHECK(std::fabs(result.min[c] - expected_lo[c]) <= 1e-9);
    TASK_CHECK(std::fabs(result.max[c] - expected_hi[c]) <= 1e-9);
  }
  // Every witness position matches the analytic position of its vertex.
  for (int f = 0; f < ClipSkinBoundsBuilder::kFaceCount; ++f) {
    const ClipSkinBoundsBuilder::Witness& w = result.witnesses[f];
    TASK_CHECK(w.vertex_index < vertices.size());
    for (int c = 0; c < 3; ++c) {
      TASK_CHECK(std::fabs(w.position[c] - expected[w.vertex_index][c]) <=
                 1e-9);
    }
  }
  return 0;
}

int CaseLinearTranslationExact() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildSingleJointSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  RawAnimation raw;
  raw.duration = 1.0f;
  raw.tracks.resize(1);
  raw.tracks[0].translations.push_back({0.0f, Float3(0.0f, 0.0f, 0.0f)});
  raw.tracks[0].translations.push_back({1.0f, Float3(2.0f, -4.0f, 6.0f)});
  TASK_CHECK(raw.Validate());
  const std::vector<Float4x4> binds(1, Float4x4::identity());
  const uint16_t j[] = {0};
  const float w[] = {1.0f};
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices = {
      MakeVertex(1.0f, 2.0f, 3.0f, j, w, 1)};

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 1e-9;
  options.max_subdivisions = 64;
  ClipSkinBoundsBuilder::Result result;
  const ClipSkinBoundsBuilder builder;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result));
  TASK_CHECK(result.status == ClipSkinBoundsBuilder::Status::kComplete);
  // The swept segment goes from (1,2,3) to (3,-2,9): the box is exact.
  TASK_CHECK(result.max_face_gap <= 1e-9);
  const double expected_min[3] = {1.0, -2.0, 3.0};
  const double expected_max[3] = {3.0, 2.0, 9.0};
  for (int c = 0; c < 3; ++c) {
    TASK_CHECK(std::fabs(result.min[c] - expected_min[c]) <= 1e-9);
    TASK_CHECK(std::fabs(result.max[c] - expected_max[c]) <= 1e-9);
  }
  // Min faces are witnessed at t = 0, max faces at t = 1.
  TASK_CHECK(result.witnesses[ClipSkinBoundsBuilder::kMinX].time == 0.0);
  TASK_CHECK(result.witnesses[ClipSkinBoundsBuilder::kMaxX].time == 1.0);
  TASK_CHECK(result.witnesses[ClipSkinBoundsBuilder::kMinY].time == 1.0);
  TASK_CHECK(result.witnesses[ClipSkinBoundsBuilder::kMaxY].time == 0.0);
  return 0;
}

int CaseRotationFaceSlack() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildSingleJointSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  const RawAnimation raw = MakeRotationAnimation();
  const std::vector<Float4x4> binds(1, Float4x4::identity());
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices = MakeArcVertices();

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 5e-4;
  options.max_subdivisions = 4096;
  ClipSkinBoundsBuilder::Result result;
  const ClipSkinBoundsBuilder builder;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result));
  TASK_CHECK(result.status == ClipSkinBoundsBuilder::Status::kComplete);
  TASK_CHECK(result.max_face_gap <= options.epsilon);

  // Analytic extremes of the swept arc.
  const ArcAnalytic arc = ComputeArcAnalytic(raw);
  const double analytic_min[3] = {arc.min_x, arc.min_y, 0.0};
  const double analytic_max[3] = {arc.max_x, arc.max_y, 0.0};
  for (int f = 0; f < ClipSkinBoundsBuilder::kFaceCount; ++f) {
    const int axis = f >> 1;
    const ClipSkinBoundsBuilder::Witness& w = result.witnesses[f];
    // Each face-to-witness distance is non-negative and within epsilon.
    const double gap = (f & 1) == 0 ? w.position[axis] - result.min[axis]
                                    : result.max[axis] - w.position[axis];
    TASK_CHECK(gap >= 0.0);
    TASK_CHECK(gap <= options.epsilon);
    // Each face is within epsilon of the analytic extreme.
    if ((f & 1) == 0) {
      TASK_CHECK(result.min[axis] <= analytic_min[axis] + 1e-9);
      TASK_CHECK(result.min[axis] >= analytic_min[axis] - options.epsilon - 1e-9);
      TASK_CHECK(w.position[axis] >= analytic_min[axis] - 1e-9);
      TASK_CHECK(w.position[axis] <= analytic_min[axis] + options.epsilon + 1e-9);
    } else {
      TASK_CHECK(result.max[axis] >= analytic_max[axis] - 1e-9);
      TASK_CHECK(result.max[axis] <= analytic_max[axis] + options.epsilon + 1e-9);
      TASK_CHECK(w.position[axis] <= analytic_max[axis] + 1e-9);
      TASK_CHECK(w.position[axis] >= analytic_max[axis] - options.epsilon - 1e-9);
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// integration group
// ---------------------------------------------------------------------------

int CaseRuntimePipelineContainment() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildForkedSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  const RawAnimation raw = MakeForkedAnimation();
  const std::vector<Float4x4> binds = MakeForkedInverseBinds();
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices =
      MakeForkedVertices();

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 1e-3;
  options.max_subdivisions = 4096;
  ClipSkinBoundsBuilder::Result result;
  const ClipSkinBoundsBuilder builder;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result));
  TASK_CHECK(result.status == ClipSkinBoundsBuilder::Status::kComplete);

  // Sweeps the time interval, including key and non-key times, and checks
  // that the pre-existing pipeline never leaves the computed box.
  const float times[] = {0.0f,  0.03f, 0.13f, 0.25f, 0.37f,
                         0.5f,  0.61f, 0.75f, 0.9f,  1.0f};
  for (float time : times) {
    std::vector<float> positions;
    TASK_CHECK(SkinWithOzzPipeline(*skeleton, raw, binds, vertices, time,
                                   &positions));
    for (size_t i = 0; i < vertices.size(); ++i) {
      for (int c = 0; c < 3; ++c) {
        const float p = positions[i * 3 + c];
        TASK_CHECK(p >= result.min[c] - kCrossTol);
        TASK_CHECK(p <= result.max[c] + kCrossTol);
      }
    }
  }
  return 0;
}

int CaseWitnessCrossCheck() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildForkedSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  const RawAnimation raw = MakeForkedAnimation();
  const std::vector<Float4x4> binds = MakeForkedInverseBinds();
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices =
      MakeForkedVertices();

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 1e-3;
  options.max_subdivisions = 4096;
  ClipSkinBoundsBuilder::Result result;
  const ClipSkinBoundsBuilder builder;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result));
  TASK_CHECK(result.status == ClipSkinBoundsBuilder::Status::kComplete);

  // Re-evaluates every witness with the pre-existing pipeline: the witness
  // position must match what SampleAnimation + LocalToModelJob + SkinningJob
  // produce for the same vertex at the same time.
  for (int f = 0; f < ClipSkinBoundsBuilder::kFaceCount; ++f) {
    const ClipSkinBoundsBuilder::Witness& w = result.witnesses[f];
    TASK_CHECK(w.time >= options.start && w.time <= options.end);
    TASK_CHECK(w.vertex_index < vertices.size());
    std::vector<float> positions;
    TASK_CHECK(SkinWithOzzPipeline(*skeleton, raw, binds, vertices,
                                   static_cast<float>(w.time), &positions));
    for (int c = 0; c < 3; ++c) {
      TASK_CHECK(std::fabs(positions[w.vertex_index * 3 + c] - w.position[c]) <=
                 kCrossTol);
    }
    // And the witness is inside the box, close to its face.
    const int axis = f >> 1;
    const double gap = (f & 1) == 0 ? w.position[axis] - result.min[axis]
                                    : result.max[axis] - w.position[axis];
    TASK_CHECK(gap >= 0.0 && gap <= options.epsilon);
  }
  return 0;
}

int CaseKeySamplesMissInteriorMaximum() {
  const ozz::unique_ptr<Skeleton> skeleton = BuildSingleJointSkeleton();
  TASK_CHECK(skeleton.get() != nullptr);
  const RawAnimation raw = MakeRotationAnimation();
  const std::vector<Float4x4> binds(1, Float4x4::identity());
  const std::vector<ClipSkinBoundsBuilder::Vertex> vertices = MakeArcVertices();

  ClipSkinBoundsBuilder::Options options;
  options.start = 0.0;
  options.end = 1.0;
  options.epsilon = 1e-3;
  options.max_subdivisions = 4096;
  ClipSkinBoundsBuilder::Result result;
  const ClipSkinBoundsBuilder builder;
  TASK_CHECK(builder.Build(*skeleton, raw, ozz::make_span(binds),
                           ozz::make_span(vertices), options, &result));
  TASK_CHECK(result.status == ClipSkinBoundsBuilder::Status::kComplete);

  // Samples the pre-existing pipeline at every original key time (t = 0 and
  // t = 1, which are also the interval endpoints): y never exceeds 0.5.
  for (size_t k = 0; k < raw.tracks[0].rotations.size(); ++k) {
    const float time = raw.tracks[0].rotations[k].time;
    std::vector<float> positions;
    TASK_CHECK(SkinWithOzzPipeline(*skeleton, raw, binds, vertices, time,
                                   &positions));
    TASK_CHECK(positions[1] <= 0.5f + static_cast<float>(kCrossTol));
  }
  // The continuous bound still reaches the interior maximum y = radius.
  const ArcAnalytic arc = ComputeArcAnalytic(raw);
  TASK_CHECK(result.max[1] >= arc.max_y - 1e-9);
  // Sampling the pipeline at the certified witness time confirms it.
  const ClipSkinBoundsBuilder::Witness& top =
      result.witnesses[ClipSkinBoundsBuilder::kMaxY];
  std::vector<float> positions;
  TASK_CHECK(SkinWithOzzPipeline(*skeleton, raw, binds, vertices,
                                 static_cast<float>(top.time), &positions));
  TASK_CHECK(std::fabs(positions[1] - top.position[1]) <= kCrossTol);
  TASK_CHECK(top.position[1] >= arc.max_y - options.epsilon);
  return 0;
}

}  // namespace

const task_case task_cases[] = {
    {"bounds", "forked_skeleton", &CaseForkedSkeletonBounds},
    {"bounds", "interior_extremum_missed_by_keys",
     &CaseInteriorExtremumMissedByKeys},
    {"bounds", "degenerate_interval", &CaseDegenerateInterval},
    {"bounds", "invalid_inputs_preserve_result",
     &CaseInvalidInputsPreserveResult},
    {"bounds", "insufficient_budget", &CaseInsufficientBudget},
    {"tightness", "static_pose_exact", &CaseStaticPoseExact},
    {"tightness", "linear_translation_exact", &CaseLinearTranslationExact},
    {"tightness", "rotation_face_slack", &CaseRotationFaceSlack},
    {"integration", "runtime_pipeline_containment",
     &CaseRuntimePipelineContainment},
    {"integration", "witness_cross_check", &CaseWitnessCrossCheck},
    {"integration", "key_samples_miss_interior_maximum",
     &CaseKeySamplesMissInteriorMaximum},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
