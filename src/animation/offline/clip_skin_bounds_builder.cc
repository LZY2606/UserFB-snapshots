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

#include "ozz/animation/offline/clip_skin_bounds_builder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "ozz/animation/offline/raw_animation.h"
#include "ozz/animation/runtime/skeleton.h"

namespace ozz {
namespace animation {
namespace offline {
namespace {

// Minimum absolute dot product accepted between two consecutive rotation
// keys. Keeps the nlerp normalization denominator away from 0 (the half-turn
// sign ambiguity), as documented on ClipSkinBoundsBuilder.
const double kMinRotationDot = 1e-3;

// ---------------------------------------------------------------------------
// Interval arithmetic with systematic outward rounding.
// Every elementary operation rounds its result outward with std::nextafter,
// so an Interval provably contains the exact real-valued result of the
// operation, for every contained input values. Chaining such operations
// (inclusion monotonicity) yields a conservative bound of the whole
// evaluation, without any fixed-step sampling or empirical padding.
// ---------------------------------------------------------------------------
struct Interval {
  double lo, hi;
};

double RoundDown(double _v) {
  return std::nextafter(_v, -std::numeric_limits<double>::infinity());
}
double RoundUp(double _v) {
  return std::nextafter(_v, std::numeric_limits<double>::infinity());
}

// Point interval around an exactly known value.
Interval IPoint(double _v) { return Interval{RoundDown(_v), RoundUp(_v)}; }

// Widens an interval by _n ulp on each side, to cover the accumulated
// rounding of the expression that produced it.
Interval IWiden(Interval _v, int _n) {
  for (int i = 0; i < _n; ++i) {
    _v.lo = RoundDown(_v.lo);
    _v.hi = RoundUp(_v.hi);
  }
  return _v;
}

Interval IAdd(Interval _a, Interval _b) {
  return Interval{RoundDown(_a.lo + _b.lo), RoundUp(_a.hi + _b.hi)};
}

Interval ISub(Interval _a, Interval _b) {
  return Interval{RoundDown(_a.lo - _b.hi), RoundUp(_a.hi - _b.lo)};
}

double Min4(double _a, double _b, double _c, double _d) {
  // NaN propagating, so that a numerical accident cannot be silently dropped.
  if (std::isnan(_a) || std::isnan(_b) || std::isnan(_c) || std::isnan(_d)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::min(std::min(_a, _b), std::min(_c, _d));
}
double Max4(double _a, double _b, double _c, double _d) {
  if (std::isnan(_a) || std::isnan(_b) || std::isnan(_c) || std::isnan(_d)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::max(std::max(_a, _b), std::max(_c, _d));
}

Interval IMul(Interval _a, Interval _b) {
  const double p0 = _a.lo * _b.lo, p1 = _a.lo * _b.hi;
  const double p2 = _a.hi * _b.lo, p3 = _a.hi * _b.hi;
  return Interval{RoundDown(Min4(p0, p1, p2, p3)),
                  RoundUp(Max4(p0, p1, p2, p3))};
}

// Product with an exact scalar.
Interval IMul(Interval _a, double _s) {
  const double p0 = _a.lo * _s, p1 = _a.hi * _s;
  return _s >= 0.0 ? Interval{RoundDown(p0), RoundUp(p1)}
                   : Interval{RoundDown(p1), RoundUp(p0)};
}

Interval ISquare(Interval _a) {
  if (_a.lo >= 0.0) {
    return Interval{RoundDown(_a.lo * _a.lo), RoundUp(_a.hi * _a.hi)};
  }
  if (_a.hi <= 0.0) {
    return Interval{RoundDown(_a.hi * _a.hi), RoundUp(_a.lo * _a.lo)};
  }
  return Interval{0.0, RoundUp(std::max(_a.lo * _a.lo, _a.hi * _a.hi))};
}

// Division by a strictly positive interval.
Interval IDiv(Interval _a, Interval _b) {
  const double q0 = _a.lo / _b.lo, q1 = _a.lo / _b.hi;
  const double q2 = _a.hi / _b.lo, q3 = _a.hi / _b.hi;
  return Interval{RoundDown(Min4(q0, q1, q2, q3)),
                  RoundUp(Max4(q0, q1, q2, q3))};
}

void IUnion(Interval* _acc, Interval _v) {
  _acc->lo = std::min(_acc->lo, _v.lo);
  _acc->hi = std::max(_acc->hi, _v.hi);
}

// ---------------------------------------------------------------------------
// Double precision affine matrix (3x4, last row implicitly 0 0 0 1), and its
// interval counterpart. Layout matches ozz::math::Float4x4 conventions:
// m[r][c] is row r, column c; column 3 is the translation.
// ---------------------------------------------------------------------------
struct DMat {
  double m[3][4];
};
struct IMat {
  Interval m[3][4];
};

// C = A * B for affine matrices.
DMat DMul(const DMat& _a, const DMat& _b) {
  DMat c;
  for (int r = 0; r < 3; ++r) {
    for (int col = 0; col < 4; ++col) {
      double v = (col == 3) ? _a.m[r][3] : 0.0;
      for (int k = 0; k < 3; ++k) {
        v += _a.m[r][k] * _b.m[k][col];
      }
      c.m[r][col] = v;
    }
  }
  return c;
}

// Interval matrix product. Interval arithmetic is inclusion monotone: if the
// entries of the true A and B lie inside _a and _b, then every entry of the
// true A * B lies inside the result.
IMat IMul(const IMat& _a, const IMat& _b) {
  IMat c;
  for (int r = 0; r < 3; ++r) {
    for (int col = 0; col < 4; ++col) {
      Interval v = (col == 3) ? _a.m[r][3] : IPoint(0.0);
      for (int k = 0; k < 3; ++k) {
        v = IAdd(v, IMul(_a.m[r][k], _b.m[k][col]));
      }
      c.m[r][col] = v;
    }
  }
  return c;
}

// Product of an interval matrix with an exact matrix.
IMat IMul(const IMat& _a, const DMat& _b) {
  IMat c;
  for (int r = 0; r < 3; ++r) {
    for (int col = 0; col < 4; ++col) {
      Interval v = (col == 3) ? _a.m[r][3] : IPoint(0.0);
      for (int k = 0; k < 3; ++k) {
        v = IAdd(v, IMul(_a.m[r][k], _b.m[k][col]));
      }
      c.m[r][col] = v;
    }
  }
  return c;
}

// Builds the local affine matrix from translation, rotation (quaternion) and
// scale, following the exact same formulas as ozz::math::Float4x4::FromAffine
// (M = T * R * S, so m[r][c] = scale_c * R[r][c] for c < 3).
DMat DFromTRS(const double _t[3], const double _q[4], const double _s[3]) {
  const double x = _q[0], y = _q[1], z = _q[2], w = _q[3];
  const double xx = x * x, xy = x * y, xz = x * z, xw = x * w;
  const double yy = y * y, yz = y * z, yw = y * w;
  const double zz = z * z, zw = z * w;
  DMat m;
  m.m[0][0] = 1.0 - 2.0 * (yy + zz);
  m.m[0][1] = 2.0 * (xy - zw);
  m.m[0][2] = 2.0 * (xz + yw);
  m.m[1][0] = 2.0 * (xy + zw);
  m.m[1][1] = 1.0 - 2.0 * (xx + zz);
  m.m[1][2] = 2.0 * (yz - xw);
  m.m[2][0] = 2.0 * (xz - yw);
  m.m[2][1] = 2.0 * (yz + xw);
  m.m[2][2] = 1.0 - 2.0 * (xx + yy);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      m.m[r][c] *= _s[c];
    }
    m.m[r][3] = _t[r];
  }
  return m;
}

// Interval counterpart of DFromTRS. Quaternion component intervals feed the
// same polynomial formulas; every true quaternion lies inside the component
// intervals, so every true matrix entry lies inside the computed interval.
IMat IFromTRS(const Interval _t[3], const Interval _q[4], const Interval _s[3]) {
  const Interval one = IPoint(1.0);
  const Interval xx = ISquare(_q[0]), yy = ISquare(_q[1]), zz = ISquare(_q[2]);
  const Interval xy = IMul(_q[0], _q[1]), xz = IMul(_q[0], _q[2]);
  const Interval xw = IMul(_q[0], _q[3]), yz = IMul(_q[1], _q[2]);
  const Interval yw = IMul(_q[1], _q[3]), zw = IMul(_q[2], _q[3]);
  Interval r[3][3];
  r[0][0] = ISub(one, IMul(IAdd(yy, zz), 2.0));
  r[0][1] = IMul(ISub(xy, zw), 2.0);
  r[0][2] = IMul(IAdd(xz, yw), 2.0);
  r[1][0] = IMul(IAdd(xy, zw), 2.0);
  r[1][1] = ISub(one, IMul(IAdd(xx, zz), 2.0));
  r[1][2] = IMul(ISub(yz, xw), 2.0);
  r[2][0] = IMul(ISub(xz, yw), 2.0);
  r[2][1] = IMul(IAdd(yz, xw), 2.0);
  r[2][2] = ISub(one, IMul(IAdd(xx, yy), 2.0));
  IMat m;
  for (int i = 0; i < 3; ++i) {
    for (int c = 0; c < 3; ++c) {
      m.m[i][c] = IMul(r[i][c], _s[c]);
    }
    m.m[i][3] = _t[i];
  }
  return m;
}

// ---------------------------------------------------------------------------
// RawAnimation channel evaluation, following the RawAnimation per-channel
// rules: empty channel -> identity, clamped outside the first/last keys,
// linear interpolation for translation and scale, hemisphere-fixed normalized
// linear interpolation for rotation. Each channel has its own key times.
// ---------------------------------------------------------------------------

// Finds the index of the first key with time > _t. Keys are validated sorted.
template <typename _Key>
size_t UpperKey(const ozz::vector<_Key>& _keys, double _t) {
  size_t lo = 0, hi = _keys.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (_t < static_cast<double>(_keys[mid].time)) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return lo;
}

// Point value of a translation or scale channel at time _t.
template <typename _Key>
void Vec3ChannelAt(const ozz::vector<_Key>& _keys, double _identity, double _t,
                   double _out[3]) {
  if (_keys.empty()) {
    _out[0] = _out[1] = _out[2] = _identity;
    return;
  }
  const size_t right = UpperKey(_keys, _t);
  if (right == 0) {
    _out[0] = _keys.front().value.x;
    _out[1] = _keys.front().value.y;
    _out[2] = _keys.front().value.z;
    return;
  }
  if (right == _keys.size()) {
    _out[0] = _keys.back().value.x;
    _out[1] = _keys.back().value.y;
    _out[2] = _keys.back().value.z;
    return;
  }
  const _Key& a = _keys[right - 1];
  const _Key& b = _keys[right];
  const double alpha =
      (_t - static_cast<double>(a.time)) / (static_cast<double>(b.time) - a.time);
  _out[0] = a.value.x + alpha * (b.value.x - a.value.x);
  _out[1] = a.value.y + alpha * (b.value.y - a.value.y);
  _out[2] = a.value.z + alpha * (b.value.z - a.value.z);
}

// Exact range of a translation or scale channel over [_t0, _t1]. The channel
// is piecewise linear in time, so its extrema are reached at the interval
// ends or at key times inside the interval.
template <typename _Key>
void Vec3ChannelRange(const ozz::vector<_Key>& _keys, double _identity,
                      double _t0, double _t1, Interval _out[3]) {
  if (_keys.empty()) {
    for (int c = 0; c < 3; ++c) {
      _out[c] = IPoint(_identity);
    }
    return;
  }
  double lo[3] = {std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity()};
  double hi[3] = {-lo[0], -lo[1], -lo[2]};
  double v[3];
  Vec3ChannelAt(_keys, _identity, _t0, v);
  for (int c = 0; c < 3; ++c) {
    lo[c] = std::min(lo[c], v[c]);
    hi[c] = std::max(hi[c], v[c]);
  }
  Vec3ChannelAt(_keys, _identity, _t1, v);
  for (int c = 0; c < 3; ++c) {
    lo[c] = std::min(lo[c], v[c]);
    hi[c] = std::max(hi[c], v[c]);
  }
  // Keys strictly inside the interval hold the local extrema.
  for (size_t k = UpperKey(_keys, _t0);
       k < _keys.size() && static_cast<double>(_keys[k].time) < _t1; ++k) {
    lo[0] = std::min(lo[0], static_cast<double>(_keys[k].value.x));
    lo[1] = std::min(lo[1], static_cast<double>(_keys[k].value.y));
    lo[2] = std::min(lo[2], static_cast<double>(_keys[k].value.z));
    hi[0] = std::max(hi[0], static_cast<double>(_keys[k].value.x));
    hi[1] = std::max(hi[1], static_cast<double>(_keys[k].value.y));
    hi[2] = std::max(hi[2], static_cast<double>(_keys[k].value.z));
  }
  for (int c = 0; c < 3; ++c) {
    // Two ulps cover the rounding of the lerp evaluations above.
    _out[c] = IWiden(Interval{lo[c], hi[c]}, 2);
  }
}

struct DQuat {
  double v[4];  // x, y, z, w
};

DQuat QuatFromKey(const RawAnimation::RotationKey& _key) {
  DQuat q = {{_key.value.x, _key.value.y, _key.value.z, _key.value.w}};
  return q;
}

double Dot(const DQuat& _a, const DQuat& _b) {
  return _a.v[0] * _b.v[0] + _a.v[1] * _b.v[1] + _a.v[2] * _b.v[2] +
         _a.v[3] * _b.v[3];
}

// Point value of a rotation channel at time _t: hemisphere-fixed normalized
// lerp, matching ozz::animation::offline::LerpRotation.
bool RotationChannelAt(const ozz::vector<RawAnimation::RotationKey>& _keys,
                       double _t, DQuat* _out) {
  if (_keys.empty()) {
    _out->v[0] = _out->v[1] = _out->v[2] = 0.0;
    _out->v[3] = 1.0;
    return true;
  }
  const size_t right = UpperKey(_keys, _t);
  if (right == 0) {
    *_out = QuatFromKey(_keys.front());
    return true;
  }
  if (right == _keys.size()) {
    *_out = QuatFromKey(_keys.back());
    return true;
  }
  const DQuat qa = QuatFromKey(_keys[right - 1]);
  DQuat qb = QuatFromKey(_keys[right]);
  if (Dot(qa, qb) < 0.0) {
    for (int c = 0; c < 4; ++c) {
      qb.v[c] = -qb.v[c];
    }
  }
  const double ta = _keys[right - 1].time, tb = _keys[right].time;
  const double alpha = (_t - ta) / (tb - ta);
  double n2 = 0.0;
  for (int c = 0; c < 4; ++c) {
    _out->v[c] = qa.v[c] + alpha * (qb.v[c] - qa.v[c]);
    n2 += _out->v[c] * _out->v[c];
  }
  if (!(n2 > 0.0)) {
    return false;
  }
  const double inv_n = 1.0 / std::sqrt(n2);
  for (int c = 0; c < 4; ++c) {
    _out->v[c] *= inv_n;
  }
  return true;
}

// Conservative range of a rotation channel over [_t0, _t1].
//
// On each key-to-key piece, the un-normalized quaternion
// u(a) = qa + a * (qb' - qa) is linear in the interpolation parameter a (qb'
// is qb flipped into qa hemisphere), so each component is exactly bounded by
// the piece ends. |u(a)|^2 is a quadratic in a, minimized/maximized in closed
// form. Consecutive keys are validated to have |dot| >= 1e-3, hence
// |u|^2 >= (1 + dot) / 2 > 0.5: the normalization denominator never
// approaches 0. Each normalized component u_i / |u| is then bounded by
// interval division, which contains every quotient of contained values.
// Returns false if the denominator cannot be bounded away from 0 (invalid
// input).
bool RotationChannelRange(const ozz::vector<RawAnimation::RotationKey>& _keys,
                          double _t0, double _t1, Interval _out[4]) {
  const Interval kEmpty = {std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()};
  for (int c = 0; c < 4; ++c) {
    _out[c] = kEmpty;
  }
  if (_keys.empty()) {
    _out[0] = _out[1] = _out[2] = IPoint(0.0);
    _out[3] = IPoint(1.0);
    return true;
  }
  const double t_first = _keys.front().time;
  const double t_last = _keys.back().time;
  if (_t1 <= t_first) {
    const DQuat q = QuatFromKey(_keys.front());
    for (int c = 0; c < 4; ++c) {
      _out[c] = IPoint(q.v[c]);
    }
    return true;
  }
  if (_t0 >= t_last) {
    const DQuat q = QuatFromKey(_keys.back());
    for (int c = 0; c < 4; ++c) {
      _out[c] = IPoint(q.v[c]);
    }
    return true;
  }
  if (_t0 < t_first) {
    const DQuat q = QuatFromKey(_keys.front());
    for (int c = 0; c < 4; ++c) {
      IUnion(&_out[c], IPoint(q.v[c]));
    }
  }
  if (_t1 > t_last) {
    const DQuat q = QuatFromKey(_keys.back());
    for (int c = 0; c < 4; ++c) {
      IUnion(&_out[c], IPoint(q.v[c]));
    }
  }
  for (size_t k = 0; k + 1 < _keys.size(); ++k) {
    const double ta = _keys[k].time, tb = _keys[k + 1].time;
    const double lo = std::max(_t0, ta), hi = std::min(_t1, tb);
    if (lo > hi) {
      continue;
    }
    const DQuat qa = QuatFromKey(_keys[k]);
    DQuat qb = QuatFromKey(_keys[k + 1]);
    if (Dot(qa, qb) < 0.0) {
      for (int c = 0; c < 4; ++c) {
        qb.v[c] = -qb.v[c];
      }
    }
    const double alpha0 = (lo - ta) / (tb - ta);
    const double alpha1 = (hi - ta) / (tb - ta);
    double diff[4];
    for (int c = 0; c < 4; ++c) {
      diff[c] = qb.v[c] - qa.v[c];
    }
    // |u(a)|^2 = A a^2 + B a + C, a convex parabola (A >= 0).
    const double A = diff[0] * diff[0] + diff[1] * diff[1] +
                     diff[2] * diff[2] + diff[3] * diff[3];
    const double B = 2.0 * (qa.v[0] * diff[0] + qa.v[1] * diff[1] +
                            qa.v[2] * diff[2] + qa.v[3] * diff[3]);
    const double C = Dot(qa, qa);
    const auto norm2 = [&](double a) { return (A * a + B) * a + C; };
    double n2_min = std::min(norm2(alpha0), norm2(alpha1));
    double n2_max = std::max(norm2(alpha0), norm2(alpha1));
    if (A > 0.0) {
      const double a_star = -B / (2.0 * A);
      if (a_star > alpha0 && a_star < alpha1) {
        n2_min = std::min(n2_min, norm2(a_star));
      }
    }
    if (!(n2_min > 1e-12)) {
      return false;  // Normalization denominator not bounded away from 0.
    }
    // Widened to cover the rounding of the norm2 evaluations.
    const Interval len = IWiden(
        Interval{std::sqrt(n2_min), std::sqrt(n2_max)}, 4);
    for (int c = 0; c < 4; ++c) {
      const double u0 = qa.v[c] + alpha0 * diff[c];
      const double u1 = qa.v[c] + alpha1 * diff[c];
      const Interval u = IWiden(
          Interval{std::min(u0, u1), std::max(u0, u1)}, 2);
      IUnion(&_out[c], IDiv(u, len));
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Decoded, double precision copy of the builder inputs.
// ---------------------------------------------------------------------------
struct DecodedVertex {
  double position[3];
  int joints[ClipSkinBoundsBuilder::kMaxInfluences];
  double weights[ClipSkinBoundsBuilder::kMaxInfluences];
  int influence_count;
};

struct Evaluator {
  const RawAnimation& animation;
  std::vector<int> parents;
  std::vector<DMat> inverse_bind;  // Per joint, exact.
  std::vector<DecodedVertex> vertices;

  explicit Evaluator(const RawAnimation& _animation) : animation(_animation) {}

  // Evaluates the conservative box of all skinned vertices over [_t0, _t1].
  // Returns false on numerical overflow or degenerate rotation denominator.
  bool SegmentBox(double _t0, double _t1, double _lo[3], double _hi[3]) const {
    const int joint_count = static_cast<int>(parents.size());
    std::vector<IMat> models(joint_count);
    for (int j = 0; j < joint_count; ++j) {
      const RawAnimation::JointTrack& track = animation.tracks[j];
      Interval t[3], q[4], s[3];
      Vec3ChannelRange(track.translations, 0.0, _t0, _t1, t);
      if (!RotationChannelRange(track.rotations, _t0, _t1, q)) {
        return false;
      }
      Vec3ChannelRange(track.scales, 1.0, _t0, _t1, s);
      const IMat local = IFromTRS(t, q, s);
      // Parents are guaranteed to be evaluated before their children.
      models[j] =
          parents[j] < 0 ? local : IMul(models[parents[j]], local);
    }
    for (int c = 0; c < 3; ++c) {
      _lo[c] = std::numeric_limits<double>::infinity();
      _hi[c] = -std::numeric_limits<double>::infinity();
    }
    for (const DecodedVertex& vertex : vertices) {
      // Skin matrix: sum of w_k * (model(j_k) * inverse_bind(j_k)). Weights
      // are non-negative, so each weighted interval stays inclusive.
      IMat skin;
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          skin.m[r][c] = IPoint(0.0);
        }
      }
      for (int k = 0; k < vertex.influence_count; ++k) {
        const IMat palette = IMul(models[vertex.joints[k]],
                                  inverse_bind[vertex.joints[k]]);
        for (int r = 0; r < 3; ++r) {
          for (int c = 0; c < 4; ++c) {
            skin.m[r][c] =
                IAdd(skin.m[r][c], IMul(palette.m[r][c], vertex.weights[k]));
          }
        }
      }
      // Applies the skin matrix to the exact bind-space position.
      for (int r = 0; r < 3; ++r) {
        Interval p = skin.m[r][3];
        for (int c = 0; c < 3; ++c) {
          p = IAdd(p, IMul(skin.m[r][c], vertex.position[c]));
        }
        _lo[r] = std::min(_lo[r], p.lo);
        _hi[r] = std::max(_hi[r], p.hi);
      }
    }
    // Detects numerical overflow: a valid outer box must be finite.
    for (int c = 0; c < 3; ++c) {
      if (!std::isfinite(_lo[c]) || !std::isfinite(_hi[c])) {
        return false;
      }
    }
    return true;
  }

  // Evaluates the exact (double precision) skinned position of every vertex
  // at time _t. Used for witnesses only, never for the containment proof.
  bool Sample(double _t, std::vector<std::array<double, 3>>* _out) const {
    const int joint_count = static_cast<int>(parents.size());
    std::vector<DMat> models(joint_count);
    for (int j = 0; j < joint_count; ++j) {
      const RawAnimation::JointTrack& track = animation.tracks[j];
      double t[3], s[3];
      DQuat q;
      Vec3ChannelAt(track.translations, 0.0, _t, t);
      if (!RotationChannelAt(track.rotations, _t, &q)) {
        return false;
      }
      Vec3ChannelAt(track.scales, 1.0, _t, s);
      const DMat local = DFromTRS(t, q.v, s);
      models[j] = parents[j] < 0 ? local : DMul(models[parents[j]], local);
    }
    _out->resize(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
      const DecodedVertex& vertex = vertices[i];
      DMat skin = {{{0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0},
                    {0.0, 0.0, 0.0, 0.0}}};
      for (int k = 0; k < vertex.influence_count; ++k) {
        const DMat palette =
            DMul(models[vertex.joints[k]], inverse_bind[vertex.joints[k]]);
        for (int r = 0; r < 3; ++r) {
          for (int c = 0; c < 4; ++c) {
            skin.m[r][c] += vertex.weights[k] * palette.m[r][c];
          }
        }
      }
      std::array<double, 3>& p = (*_out)[i];
      for (int r = 0; r < 3; ++r) {
        p[r] = skin.m[r][3];
        for (int c = 0; c < 3; ++c) {
          p[r] += skin.m[r][c] * vertex.position[c];
        }
      }
      if (!std::isfinite(p[0]) || !std::isfinite(p[1]) ||
          !std::isfinite(p[2])) {
        return false;
      }
    }
    return true;
  }
};

// A time segment with its conservative box.
struct Segment {
  double t0, t1;
  double lo[3], hi[3];
};

}  // namespace

bool ClipSkinBoundsBuilder::Build(
    const Skeleton& _skeleton, const RawAnimation& _animation,
    const span<const math::Float4x4>& _inverse_bind_matrices,
    const span<const Vertex>& _vertices, const Options& _options,
    Result* _result) const {
  if (_result == nullptr) {
    return false;
  }

  // ------------------------------------------------------------------
  // Input validation. Any failure here leaves *_result untouched.
  // ------------------------------------------------------------------
  const int joint_count = _skeleton.num_joints();
  if (!_animation.Validate() || _animation.num_tracks() != joint_count ||
      joint_count <= 0) {
    return false;
  }
  if (static_cast<int>(_inverse_bind_matrices.size()) != joint_count) {
    return false;
  }
  if (_vertices.empty()) {
    return false;
  }
  if (!std::isfinite(_options.start) || !std::isfinite(_options.end) ||
      !std::isfinite(_options.epsilon) || _options.epsilon < 0.0) {
    return false;
  }
  const double duration = _animation.duration;
  if (!(_options.start >= 0.0) || !(_options.start <= _options.end) ||
      !(_options.end <= duration)) {
    return false;
  }

  Evaluator eval(_animation);
  eval.parents.resize(joint_count);
  const span<const int16_t> parents = _skeleton.joint_parents();
  for (int j = 0; j < joint_count; ++j) {
    const int parent = parents[j];
    // ozz skeletons store joints so that parents always precede children.
    if (parent != Skeleton::kNoParent && (parent < 0 || parent >= j)) {
      return false;
    }
    eval.parents[j] = parent;
  }

  // Inverse-bind matrices must be finite and affine (last row 0 0 0 1).
  eval.inverse_bind.resize(joint_count);
  for (int j = 0; j < joint_count; ++j) {
    const math::Float4x4& m = _inverse_bind_matrices[j];
    const float wf = math::GetW(m.cols[3]);
    if (math::GetW(m.cols[0]) != 0.0f || math::GetW(m.cols[1]) != 0.0f ||
        math::GetW(m.cols[2]) != 0.0f || wf != 1.0f) {
      return false;
    }
    const float rows[3][4] = {
        {math::GetX(m.cols[0]), math::GetX(m.cols[1]), math::GetX(m.cols[2]),
         math::GetX(m.cols[3])},
        {math::GetY(m.cols[0]), math::GetY(m.cols[1]), math::GetY(m.cols[2]),
         math::GetY(m.cols[3])},
        {math::GetZ(m.cols[0]), math::GetZ(m.cols[1]), math::GetZ(m.cols[2]),
         math::GetZ(m.cols[3])}};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) {
        if (!std::isfinite(rows[r][c])) {
          return false;
        }
        eval.inverse_bind[j].m[r][c] = rows[r][c];
      }
    }
  }

  // Key values must be finite; consecutive rotation keys must keep the
  // documented minimum absolute dot product.
  std::vector<double> key_times;
  for (int j = 0; j < joint_count; ++j) {
    const RawAnimation::JointTrack& track = _animation.tracks[j];
    for (const RawAnimation::TranslationKey& key : track.translations) {
      if (!std::isfinite(key.value.x) || !std::isfinite(key.value.y) ||
          !std::isfinite(key.value.z)) {
        return false;
      }
      key_times.push_back(key.time);
    }
    for (const RawAnimation::ScaleKey& key : track.scales) {
      if (!std::isfinite(key.value.x) || !std::isfinite(key.value.y) ||
          !std::isfinite(key.value.z)) {
        return false;
      }
      key_times.push_back(key.time);
    }
    for (const RawAnimation::RotationKey& key : track.rotations) {
      if (!std::isfinite(key.value.x) || !std::isfinite(key.value.y) ||
          !std::isfinite(key.value.z) || !std::isfinite(key.value.w)) {
        return false;
      }
      key_times.push_back(key.time);
    }
    for (size_t k = 0; k + 1 < track.rotations.size(); ++k) {
      const DQuat qa = QuatFromKey(track.rotations[k]);
      const DQuat qb = QuatFromKey(track.rotations[k + 1]);
      if (std::fabs(Dot(qa, qb)) < kMinRotationDot - 1e-9) {
        return false;
      }
    }
  }
  std::sort(key_times.begin(), key_times.end());
  key_times.erase(std::unique(key_times.begin(), key_times.end()),
                  key_times.end());

  // Vertices.
  eval.vertices.resize(_vertices.size());
  for (size_t i = 0; i < _vertices.size(); ++i) {
    const Vertex& src = _vertices[i];
    DecodedVertex& dst = eval.vertices[i];
    if (!std::isfinite(src.position.x) || !std::isfinite(src.position.y) ||
        !std::isfinite(src.position.z)) {
      return false;
    }
    dst.position[0] = src.position.x;
    dst.position[1] = src.position.y;
    dst.position[2] = src.position.z;
    if (src.influence_count < 1 ||
        src.influence_count > kMaxInfluences) {
      return false;
    }
    dst.influence_count = src.influence_count;
    double weight_sum = 0.0;
    for (int k = 0; k < src.influence_count; ++k) {
      if (src.joints[k] >= joint_count) {
        return false;
      }
      const float w = src.weights[k];
      if (!std::isfinite(w) || w < 0.0f) {
        return false;
      }
      dst.joints[k] = src.joints[k];
      dst.weights[k] = w;
      weight_sum += w;
    }
    if (std::fabs(weight_sum - 1.0) > 1e-3) {
      return false;
    }
  }

  // ------------------------------------------------------------------
  // Computation, into local storage. *_result is only written on success.
  // ------------------------------------------------------------------
  Result result;

  // Best witness per face (faces 2a and 2a+1 are the min and max faces of
  // axis a), updated incrementally with every exact sample: witnesses only
  // ever improve, so they can be tracked running.
  Witness best[kFaceCount];
  double best_value[kFaceCount];
  for (int f = 0; f < kFaceCount; ++f) {
    best[f].vertex_index = 0;
    best[f].time = _options.start;
    best[f].position[0] = best[f].position[1] = best[f].position[2] = 0.0;
    best_value[f] = (f & 1) == 0 ? std::numeric_limits<double>::infinity()
                                 : -std::numeric_limits<double>::infinity();
  }

  // Box of all exact samples (only grows).
  double sample_lo[3] = {std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::infinity()};
  double sample_hi[3] = {-sample_lo[0], -sample_lo[1], -sample_lo[2]};

  std::vector<std::array<double, 3>> positions;
  const auto add_samples = [&](double _t) -> bool {
    if (!eval.Sample(_t, &positions)) {
      return false;
    }
    for (size_t i = 0; i < positions.size(); ++i) {
      const std::array<double, 3>& p = positions[i];
      for (int c = 0; c < 3; ++c) {
        sample_lo[c] = std::min(sample_lo[c], p[c]);
        sample_hi[c] = std::max(sample_hi[c], p[c]);
      }
      for (int f = 0; f < kFaceCount; ++f) {
        const int axis = f >> 1;
        const double v = p[axis];
        if ((f & 1) == 0 ? v < best_value[f] : v > best_value[f]) {
          best_value[f] = v;
          best[f].vertex_index = i;
          best[f].time = _t;
          best[f].position[0] = p[0];
          best[f].position[1] = p[1];
          best[f].position[2] = p[2];
        }
      }
    }
    return true;
  };

  // Degenerate interval: a single pose, exactly evaluated.
  if (_options.start == _options.end) {
    if (!add_samples(_options.start)) {
      return false;
    }
    for (int c = 0; c < 3; ++c) {
      result.min[c] = sample_lo[c];
      result.max[c] = sample_hi[c];
    }
    result.status = Status::kComplete;
    result.max_face_gap = 0.0;
    for (int f = 0; f < kFaceCount; ++f) {
      result.witnesses[f] = best[f];
    }
    *_result = result;
    return true;
  }

  // Initial whole-interval evaluation. Always performed, whatever the
  // subdivision budget is, so a valid finite outer box is always produced.
  std::vector<Segment> segments;
  {
    Segment seg;
    seg.t0 = _options.start;
    seg.t1 = _options.end;
    if (!eval.SegmentBox(seg.t0, seg.t1, seg.lo, seg.hi)) {
      return false;
    }
    segments.push_back(seg);
  }
  if (!add_samples(_options.start) || !add_samples(_options.end)) {
    return false;
  }

  size_t subdivisions = 0;
  for (;;) {
    // Conservative box: union of the interval boxes of all segments, and of
    // the exact samples (samples are contained by construction, this union
    // only protects against sub-ulp evaluation discrepancies).
    double box_lo[3], box_hi[3];
    for (int c = 0; c < 3; ++c) {
      box_lo[c] = sample_lo[c];
      box_hi[c] = sample_hi[c];
    }
    for (const Segment& seg : segments) {
      for (int c = 0; c < 3; ++c) {
        box_lo[c] = std::min(box_lo[c], seg.lo[c]);
        box_hi[c] = std::max(box_hi[c], seg.hi[c]);
      }
    }

    // Best witness per face is tracked incrementally by add_samples().
    // Face-to-witness axial distances are non-negative because the box
    // contains every true position, hence every witness.
    double gaps[kFaceCount];
    double max_gap = 0.0;
    for (int f = 0; f < kFaceCount; ++f) {
      const int axis = f >> 1;
      const double gap = (f & 1) == 0
                             ? best_value[f] - box_lo[axis]
                             : box_hi[axis] - best_value[f];
      gaps[f] = std::max(0.0, gap);
      max_gap = std::max(max_gap, gaps[f]);
    }

    if (max_gap <= _options.epsilon) {
      // Every face is certified within epsilon: complete.
      for (int c = 0; c < 3; ++c) {
        result.min[c] = box_lo[c];
        result.max[c] = box_hi[c];
      }
      for (int f = 0; f < kFaceCount; ++f) {
        result.witnesses[f] = best[f];
      }
      result.max_face_gap = max_gap;
      result.status = Status::kComplete;
      *_result = result;
      return true;
    }

    if (subdivisions >= _options.max_subdivisions) {
      // Budget exhausted: report the still-valid outer box, the current
      // witnesses and the remaining worst face-to-witness distance.
      for (int c = 0; c < 3; ++c) {
        result.min[c] = box_lo[c];
        result.max[c] = box_hi[c];
      }
      for (int f = 0; f < kFaceCount; ++f) {
        result.witnesses[f] = best[f];
      }
      result.max_face_gap = max_gap;
      result.status = Status::kIncomplete;
      *_result = result;
      return true;
    }

    // Splits the segment that attains the box extreme of the worst face.
    // Faces are considered by decreasing gap so that a non-splittable
    // segment (floating point limit) cannot block progress.
    int face_order[kFaceCount];
    for (int f = 0; f < kFaceCount; ++f) {
      face_order[f] = f;
    }
    std::sort(face_order, face_order + kFaceCount,
              [&](int a, int b) { return gaps[a] > gaps[b]; });

    bool split_done = false;
    for (int fi = 0; fi < kFaceCount && !split_done; ++fi) {
      const int f = face_order[fi];
      if (gaps[f] <= _options.epsilon) {
        break;
      }
      const int axis = f >> 1;
      const bool is_max = (f & 1) != 0;
      const double extreme = is_max ? box_hi[axis] : box_lo[axis];
      // Finds the widest segment attaining this extreme.
      size_t seg_index = segments.size();
      double seg_width = 0.0;
      for (size_t s = 0; s < segments.size(); ++s) {
        const double bound = is_max ? segments[s].hi[axis] : segments[s].lo[axis];
        if (bound != extreme) {
          continue;
        }
        const double width = segments[s].t1 - segments[s].t0;
        if (seg_index == segments.size() || width > seg_width) {
          seg_index = s;
          seg_width = width;
        }
      }
      if (seg_index == segments.size()) {
        continue;  // Extreme attained by a sample: nothing to subdivide.
      }
      const Segment seg = segments[seg_index];
      // Split at the key time closest to the middle if any lies strictly
      // inside, otherwise at the arithmetic middle. Splitting on key times
      // aligns segments with the internal channel key boundaries.
      double split = 0.0;
      bool found = false;
      const double mid = seg.t0 + (seg.t1 - seg.t0) * 0.5;
      const auto key_it = std::lower_bound(key_times.begin(), key_times.end(),
                                           seg.t0);
      for (auto it = key_it; it != key_times.end() && *it < seg.t1; ++it) {
        if (*it <= seg.t0) {
          continue;
        }
        if (!found || std::fabs(*it - mid) < std::fabs(split - mid)) {
          split = *it;
          found = true;
        }
      }
      if (!found) {
        split = mid;
        if (!(split > seg.t0 && split < seg.t1)) {
          continue;  // Cannot be subdivided further (floating point limit).
        }
      }
      Segment left = seg, right = seg;
      left.t1 = split;
      right.t0 = split;
      if (!eval.SegmentBox(left.t0, left.t1, left.lo, left.hi) ||
          !eval.SegmentBox(right.t0, right.t1, right.lo, right.hi)) {
        return false;
      }
      segments[seg_index] = left;
      segments.push_back(right);
      if (!add_samples(split)) {
        return false;
      }
      ++subdivisions;
      split_done = true;
    }

    if (!split_done) {
      // No segment could be subdivided any further. The box is still a valid
      // outer bound; report it as incomplete.
      for (int c = 0; c < 3; ++c) {
        result.min[c] = box_lo[c];
        result.max[c] = box_hi[c];
      }
      for (int f = 0; f < kFaceCount; ++f) {
        result.witnesses[f] = best[f];
      }
      result.max_face_gap = max_gap;
      result.status = Status::kIncomplete;
      *_result = result;
      return true;
    }
  }
}

}  // namespace offline
}  // namespace animation
}  // namespace ozz
