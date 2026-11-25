#include "manifold/surface_partition.h"

#include "manifold/polygon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "manifold/vec_view.h"

namespace manifold {
namespace {

constexpr double kMergeRadius = 2.0e-8;
constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kMergeRadius2 = kMergeRadius * kMergeRadius;
constexpr double kPointEps = 1.0e-8;
constexpr double kClipEps = 1.0e-11;

using std::array;
using std::vector;

array<double, 3> Barycentric(const vec3& p, const array<vec3, 3>& t) {
  const vec3 v0 = t[1] - t[0];
  const vec3 v1 = t[2] - t[0];
  const vec3 v2 = p - t[0];
  const double d00 = la::dot(v0, v0);
  const double d01 = la::dot(v0, v1);
  const double d11 = la::dot(v1, v1);
  const double d20 = la::dot(v2, v0);
  const double d21 = la::dot(v2, v1);
  const double denom = d00 * d11 - d01 * d01;
  if (std::abs(denom) < 1e-30) return {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
  const double v = (d11 * d20 - d01 * d21) / denom;
  const double w = (d00 * d21 - d01 * d20) / denom;
  return {1.0 - v - w, v, w};
}

vec2 BaryUV(const array<double, 3>& b) { return vec2(b[1], b[2]); }

vec3 PointFromUV(const array<vec3, 3>& t, const vec2& uv) {
  return t[0] * (1.0 - uv.x - uv.y) + t[1] * uv.x + t[2] * uv.y;
}

bool Finite(double x) { return std::isfinite(x); }

struct VertexPool {
  vector<vec3> v;
  std::unordered_map<int64_t, vector<uint64_t>> grid;
  double cell = 1.0e-7;

  static int64_t CoordKey(double x, double cell_) {
    return static_cast<int64_t>(std::floor(x / cell_));
  }

  uint64_t AppendRaw(const vec3& p) {
    const auto id = static_cast<uint64_t>(v.size());
    v.push_back(p);
    return id;
  }

  uint64_t Add(const vec3& p) {
    const int64_t kx = CoordKey(p.x, cell);
    const int64_t ky = CoordKey(p.y, cell);
    const int64_t kz = CoordKey(p.z, cell);
    for (int dx = -1; dx <= 1; ++dx)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz) {
          const auto it = grid.find((kx + dx) * 73856093LL ^
                                    (ky + dy) * 19349663LL ^
                                    (kz + dz) * 83492791LL);
          if (it == grid.end()) continue;
          for (uint64_t id : it->second) {
            if (la::dot(v[id] - p, v[id] - p) <= kMergeRadius2) return id;
          }
        }
    const uint64_t id = AppendRaw(p);
    grid[kx * 73856093LL ^ ky * 19349663LL ^ kz * 83492791LL].push_back(id);
    return id;
  }
};

struct Plane {
  vec3 n;
  double d = 0.0;
};

Plane MakePlane(const array<vec3, 3>& t) {
  vec3 n = la::cross(t[1] - t[0], t[2] - t[0]);
  const double len = la::length(n);
  if (len > 0.0) n = n / len;
  return {n, la::dot(n, t[0])};
}

double Side(const Plane& p, const vec3& x) { return la::dot(p.n, x) - p.d; }

bool PointInTriangle(const vec3& p, const array<vec3, 3>& t) {
  const auto b = Barycentric(p, t);
  constexpr double eps = 2.0e-7;
  return b[0] >= -eps && b[1] >= -eps && b[2] >= -eps;
}

void AddCandidate(vector<vec3>& out, const vec3& p) {
  for (const vec3& q : out)
    if (la::dot(p - q, p - q) < 1.0e-18) return;
  out.push_back(p);
}

vec3 EdgePlaneCross(const vec3& a, const vec3& b, const Plane& p, double da,
                    double db) {
  double t = da / (da - db);
  if (!Finite(t)) t = 0.5;
  t = std::max(0.0, std::min(1.0, t));
  return a + (b - a) * t;
}

bool AABBOverlap(const array<vec3, 3>& a, const array<vec3, 3>& b) {
  vec3 amin = a[0], amax = a[0], bmin = b[0], bmax = b[0];
  for (int i = 1; i < 3; ++i) {
    amin = la::min(amin, a[i]);
    amax = la::max(amax, a[i]);
    bmin = la::min(bmin, b[i]);
    bmax = la::max(bmax, b[i]);
  }
  constexpr double eps = 1.0e-8;
  for (int k = 0; k < 3; ++k)
    if (amax[k] + eps < bmin[k] || bmax[k] + eps < amin[k]) return false;
  return true;
}

bool CoplanarOverlap(const array<vec3, 3>& a, const array<vec3, 3>& b,
                     const Plane& pa, const Plane& pb) {
  if (std::abs(la::dot(pa.n, pb.n)) < 1.0 - 1.0e-10) return false;
  if (std::abs(la::dot(pa.n, b[0] - a[0])) > 1.0e-7) return false;
  for (const vec3& p : a)
    if (PointInTriangle(p, b)) return true;
  for (const vec3& p : b)
    if (PointInTriangle(p, a)) return true;
  return false;
}

// 0 = disjoint, 1 = segment, -1 = explicitly unsupported degeneracy.
int TriangleSegment(const array<vec3, 3>& a, const array<vec3, 3>& b,
                    array<vec3, 2>& seg, std::string& why) {
  const Plane pa = MakePlane(a);
  const Plane pb = MakePlane(b);
  if (la::length(la::cross(pa.n, pb.n)) < 1.0e-12) {
    if (CoplanarOverlap(a, b, pa, pb)) {
      why = "coplanar overlapping faces are unsupported";
      return -1;
    }
    return 0;
  }

  vector<vec3> candidates;
  const double sa[3] = {Side(pb, a[0]), Side(pb, a[1]), Side(pb, a[2])};
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3;
    if ((sa[i] > kPointEps && sa[j] < -kPointEps) ||
        (sa[i] < -kPointEps && sa[j] > kPointEps) ||
        std::abs(sa[i]) <= kPointEps || std::abs(sa[j]) <= kPointEps) {
      if (std::abs(sa[i]) <= 1.0e-12) AddCandidate(candidates, a[i]);
      if (std::abs(sa[j]) <= 1.0e-12) AddCandidate(candidates, a[j]);
      if (sa[i] * sa[j] < 0.0) {
        const vec3 p = EdgePlaneCross(a[i], a[j], pb, sa[i], sa[j]);
        if (PointInTriangle(p, b)) AddCandidate(candidates, p);
      }
    }
  }
  const double sb[3] = {Side(pa, b[0]), Side(pa, b[1]), Side(pa, b[2])};
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3;
    if (sb[i] * sb[j] < 0.0) {
      const vec3 p = EdgePlaneCross(b[i], b[j], pa, sb[i], sb[j]);
      if (PointInTriangle(p, a)) AddCandidate(candidates, p);
    } else {
      if (std::abs(sb[i]) <= 1.0e-12 && PointInTriangle(b[i], a))
        AddCandidate(candidates, b[i]);
      if (std::abs(sb[j]) <= 1.0e-12 && PointInTriangle(b[j], a))
        AddCandidate(candidates, b[j]);
    }
  }

  if (candidates.size() < 2) return 0;
  size_t bi = 0, bj = 1;
  double best = -1.0;
  for (size_t i = 0; i < candidates.size(); ++i)
    for (size_t j = i + 1; j < candidates.size(); ++j) {
      const double d2 = la::dot(candidates[i] - candidates[j],
                                candidates[i] - candidates[j]);
      if (d2 > best) {
        best = d2;
        bi = i;
        bj = j;
      }
    }
  if (best < 1.0e-16) return 0;
  const double normalCross = la::length(la::cross(pa.n, pb.n));
  if (normalCross < 1.0e-4) {
    why = "intersecting surfaces are too close to parallel";
    return -1;
  }
  seg = {candidates[bi], candidates[bj]};
  return 1;
}

int WindingNumber(const vec3& p, const vector<array<vec3, 3>>& tris) {
  double angle = 0.0;
  for (const auto& t : tris) {
    vec3 a = t[0] - p;
    vec3 b = t[1] - p;
    vec3 c = t[2] - p;
    const double al = la::length(a), bl = la::length(b), cl = la::length(c);
    if (al < 1.0e-12 || bl < 1.0e-12 || cl < 1.0e-12) continue;
    const double det = la::dot(a, la::cross(b, c));
    const double denom = al * bl * cl + la::dot(a, b) * cl +
                         la::dot(b, c) * al + la::dot(c, a) * bl;
    angle += 2.0 * std::atan2(det, denom);
  }
  return static_cast<int>(std::lround(angle / (4.0 * kPi)));
}

bool IsInside(const vec3& p, const vector<array<vec3, 3>>& tris) {
  return WindingNumber(p, tris) != 0;
}

struct RawSeg {
  uint32_t opA, triA, opB, triB;
  uint64_t v0, v1;
};

struct PNode {
  uint64_t gid;
  vec2 uv;
};

struct Line {
  vec2 p;
  vec2 d;
};

double LineSide(const Line& l, const vec2& q) {
  return l.d.x * (q.y - l.p.y) - l.d.y * (q.x - l.p.x);
}

vec2 LineIntersectionUV(const PNode& a, const PNode& b, double sa, double sb) {
  const double t = sa / (sa - sb);
  return a.uv + (b.uv - a.uv) * t;
}

using Poly = vector<PNode>;

void CleanPoly(Poly& poly) {
  Poly out;
  for (const PNode& n : poly) {
    if (!out.empty() && out.back().gid == n.gid) continue;
    out.push_back(n);
  }
  while (out.size() > 1 && out.front().gid == out.back().gid) out.pop_back();
  poly.swap(out);
}

double PolyArea(const Poly& p) {
  double a = 0.0;
  for (size_t i = 0; i < p.size(); ++i) {
    const vec2& u = p[i].uv;
    const vec2& v = p[(i + 1) % p.size()].uv;
    a += u.x * v.y - u.y * v.x;
  }
  return 0.5 * a;
}

vec2 UVOf(const VertexPool& pool, uint64_t id, const array<vec3, 3>& src) {
  return BaryUV(Barycentric(pool.v[id], src));
}

double Cross2(const vec2& a, const vec2& b) {
  return a.x * b.y - a.y * b.x;
}

bool SegmentIntersection2D(const vec2& p1, const vec2& p2, const vec2& p3,
                          const vec2& p4, double& t, double& u) {
  const vec2 r = p2 - p1;
  const vec2 s = p4 - p3;
  const double den = Cross2(r, s);
  if (std::abs(den) < 1.0e-20) return false;
  const vec2 qp = p3 - p1;
  t = Cross2(qp, s) / den;
  u = Cross2(qp, r) / den;
  return t >= -1.0e-9 && t <= 1.0 + 1.0e-9 && u >= -1.0e-9 &&
         u <= 1.0 + 1.0e-9;
}

bool SegmentTriangleInterval(const vec2& p, const vec2& q,
                             array<vec2, 3> t) {  double area = Cross2(t[1] - t[0], t[2] - t[0]);
  if (std::abs(area) < 1.0e-22) return false;
  if (area < 0.0) std::swap(t[1], t[2]);
  double lo = 0.0, hi = 1.0;
  const vec2 d = q - p;
  for (int e = 0; e < 3; ++e) {
    const vec2 a = t[e];
    const vec2 b = t[(e + 1) % 3];
    const vec2 edge = b - a;
    const double f0 = Cross2(edge, p - a);
    const double f1 = f0 + Cross2(edge, d);
    const double coeff = f1 - f0;
    constexpr double eps = 1.0e-10;
    if (std::abs(coeff) < 1.0e-20) {
      if (f0 < -eps) return false;
      continue;
    }
    const double root = -f0 / coeff;
    if (coeff > 0.0)
      lo = std::max(lo, root);
    else
      hi = std::min(hi, root);
    if (lo > hi + 1.0e-9) return false;
  }
  lo = std::max(0.0, lo);
  hi = std::min(1.0, hi);
  return hi - lo > 1.0e-11;
}

vector<array<uint64_t, 3>> EarTriangulateChain(
    Poly poly, uint64_t keepA, uint64_t keepB,
    const std::set<uint64_t>& preserve) {
  vector<array<uint64_t, 3>> out;
  if (poly.size() < 3) return out;
  if (PolyArea(poly) < 0.0) std::reverse(poly.begin(), poly.end());

  auto insideEar = [](const vec2& p, const vec2& a, const vec2& b,
                      const vec2& c) {
    const double s1 = Cross2(b - a, p - a);
    const double s2 = Cross2(c - b, p - b);
    const double s3 = Cross2(a - c, p - c);
    constexpr double eps = 1.0e-10;
    return s1 >= -eps && s2 >= -eps && s3 >= -eps;
  };

  size_t guard = 0;
  while (poly.size() > 3 && guard++ < 100000) {
    int ear = -1;
    int redundant = -1;
    const int n = static_cast<int>(poly.size());
    for (int i = 0; i < n; ++i) {
      const PNode& p = poly[(i + n - 1) % n];
      const PNode& c = poly[i];
      const PNode& q = poly[(i + 1) % n];
      const double ar = Cross2(c.uv - p.uv, q.uv - p.uv);
      if (std::abs(ar) < 1.0e-13) {
        if (c.gid != keepA && c.gid != keepB && !preserve.count(c.gid))
          redundant = i;
        continue;
      }
      if (ar < 0.0) continue;
      bool blocked = false;
      for (int j = 0; j < n; ++j) {
        if (j == i || j == (i + n - 1) % n || j == (i + 1) % n) continue;
        if (insideEar(poly[j].uv, p.uv, c.uv, q.uv)) {
          blocked = true;
          break;
        }
      }
      if (!blocked && c.gid != keepA && c.gid != keepB &&
          !preserve.count(c.gid)) {
        ear = i;
        break;
      }
    }
    if (ear < 0) {
      for (int i = 0; i < n; ++i) {
        const PNode& p = poly[(i + n - 1) % n];
        const PNode& c = poly[i];
        const PNode& q = poly[(i + 1) % n];
        const double ar = Cross2(c.uv - p.uv, q.uv - p.uv);
        if (ar > 1.0e-13) {
          ear = i;
          break;
        }
      }
    }
    if (ear >= 0) {
      const int n2 = static_cast<int>(poly.size());
      out.push_back({poly[(ear + n2 - 1) % n2].gid, poly[ear].gid,
                     poly[(ear + 1) % n2].gid});
      poly.erase(poly.begin() + ear);
    } else if (redundant >= 0) {
      poly.erase(poly.begin() + redundant);
    } else {
      break;
    }
  }
  if (poly.size() == 3 && std::abs(PolyArea(poly)) > 1.0e-14)
    out.push_back({poly[0].gid, poly[1].gid, poly[2].gid});
  return out;
}

}  // namespace

const char* ToString(SurfacePartition::Error error) {
  switch (error) {
    case SurfacePartition::Error::NoError:
      return "NoError";
    case SurfacePartition::Error::TooManyOperands:
      return "SurfacePartition supports at most four operands";
    case SurfacePartition::Error::InvalidOperand:
      return "an input Manifold has an error status";
    case SurfacePartition::Error::NonFiniteInput:
      return "input contains a non-finite value";
    case SurfacePartition::Error::PropertyCountMismatch:
      return "inputs have different extra scalar-property counts";
    case SurfacePartition::Error::InvalidGeometry:
      return "input is outside the supported closed-triangle range";
    case SurfacePartition::Error::UnsupportedArrangement:
      return "unsupported intersection/degeneracy in surface arrangement";
    case SurfacePartition::Error::EmptyMaskIsUnbounded:
      return "Extract(0) is the unbounded exterior and is not a closed solid";
    case SurfacePartition::Error::NoSuchMask:
      return "mask contains bits for operands that were not supplied";
  }
  return "Unknown SurfacePartition error";
}

const MeshGL64& SurfacePartition::SourceMesh(uint32_t operand) const {
  static const MeshGL64 empty;
  return operand < sourceMeshes_.size() ? sourceMeshes_[operand] : empty;
}

Manifold SurfacePartition::ExtractChecked(uint32_t mask) const {
  Error e = Error::NoError;
  Manifold m = Extract(mask, &e);
  if (e != Error::NoError) throw std::invalid_argument(ToString(e));
  return m;
}

Manifold SurfacePartition::Extract(uint32_t mask, Error* error) const {
  auto fail = [&](Error e) -> Manifold {
    if (error) {
      *error = e;
      return Manifold();
    }
    throw std::invalid_argument(ToString(e));
  };

  if (error) *error = Error::NoError;
  if (error_ != Error::NoError) return fail(error_);
  if (mask == 0) return fail(Error::EmptyMaskIsUnbounded);
  if ((mask & ~validMask_) != 0) return fail(Error::NoSuchMask);

  struct Selected {
    size_t face;
    bool reverse;
    uint32_t op;
    uint64_t tri;
    size_t run;
  };
  vector<Selected> selected;
  for (size_t i = 0; i < triangles_.size(); ++i) {
    const Triangle& f = triangles_[i];
    if (f.minusMask == mask)
      selected.push_back({i, false, f.sourceOperand, f.sourceTriangle, 0});
    else if (f.plusMask == mask)
      selected.push_back({i, true, f.sourceOperand, f.sourceTriangle, 0});
  }
  if (selected.empty()) return Manifold();

  for (Selected& s : selected) {
    const MeshGL64& sm = sourceMeshes_[s.op];
    s.run = 0;
    if (sm.runIndex.size() >= 2) {
      const auto offset = static_cast<uint64_t>(3) * s.tri;
      for (size_t r = 0; r + 1 < sm.runIndex.size(); ++r) {
        if (offset >= sm.runIndex[r] && offset < sm.runIndex[r + 1]) {
          s.run = r;
          break;
        }
      }
    }
  }
  std::sort(selected.begin(), selected.end(), [](const Selected& a,
                                                 const Selected& b) {
    if (a.op != b.op) return a.op < b.op;
    return a.run < b.run;
  });

  MeshGL64 out;
  out.numProp = 3 + extraProps_;
  vector<uint64_t> canonicalProp(vertices_.size(),
                                 std::numeric_limits<uint64_t>::max());
  vector<uint64_t> faceIDs;

  size_t pos = 0;
  while (pos < selected.size()) {
    size_t end = pos + 1;
    while (end < selected.size() && selected[end].op == selected[pos].op &&
           selected[end].run == selected[pos].run)
      ++end;

    const uint32_t op = selected[pos].op;
    const size_t run = selected[pos].run;
    const MeshGL64& sm = sourceMeshes_[op];
    out.runIndex.push_back(static_cast<uint64_t>(out.triVerts.size()));
    if (run < sm.runOriginalID.size())
      out.runOriginalID.push_back(sm.runOriginalID[run]);
    else
      out.runOriginalID.push_back(0);
    if (12 * (run + 1) <= sm.runTransform.size()) {
      for (int k = 0; k < 12; ++k)
        out.runTransform.push_back(sm.runTransform[12 * run + k]);
    }
    if (run < sm.runFlags.size()) out.runFlags.push_back(sm.runFlags[run]);

    for (size_t qi = pos; qi < end; ++qi) {
      const Selected& sel = selected[qi];
      const Triangle& f = triangles_[sel.face];
      const MeshGL64& src = sourceMeshes_[f.sourceOperand];
      const uint64_t st = f.sourceTriangle;
      const array<int, 3> map =
          sel.reverse ? array<int, 3>{0, 2, 1} : array<int, 3>{0, 1, 2};

      if (!src.faceID.empty() && st < src.faceID.size()) {
        faceIDs.push_back(src.faceID[st]);
      } else {
        faceIDs.push_back(st);
      }

      for (int outCorner = 0; outCorner < 3; ++outCorner) {
        const int c = map[outCorner];
        const uint64_t gid = f.vertices[c];
        const uint64_t propIndex =
            out.vertProperties.size() / static_cast<size_t>(out.numProp);

        if (canonicalProp[gid] == std::numeric_limits<uint64_t>::max()) {
          canonicalProp[gid] = propIndex;
        } else {
          out.mergeFromVert.push_back(propIndex);
          out.mergeToVert.push_back(canonicalProp[gid]);
        }

        const vec3 p = vertices_[gid];
        out.vertProperties.push_back(p.x);
        out.vertProperties.push_back(p.y);
        out.vertProperties.push_back(p.z);

        for (int ch = 0; ch < extraProps_; ++ch) {
          double value = 0.0;
          if (extraProps_ > 0 && src.numProp >= 3 + extraProps_) {
            for (int k = 0; k < 3; ++k) {
              const uint64_t pv = src.triVerts[3 * st + k];
              const size_t off =
                  static_cast<size_t>(pv) * src.numProp + 3 + ch;
              const double w = f.sourceBarycentric[c][k];
              if (off < src.vertProperties.size())
                value += w * src.vertProperties[off];
            }
          }
          out.vertProperties.push_back(value);
        }
        out.triVerts.push_back(propIndex);
      }
    }
    pos = end;
  }
  out.runIndex.push_back(static_cast<uint64_t>(out.triVerts.size()));
  out.faceID = std::move(faceIDs);

  Manifold result(out);
  if (result.Status() != Manifold::Error::NoError) {
    // The arrangement invariants make this unreachable for supported inputs.
    return fail(Error::UnsupportedArrangement);
  }
  if (error) *error = Error::NoError;
  return result;
}

SurfacePartition::SurfacePartition(const std::vector<Manifold>& operands) {
  Error e = Build(operands);
  if (e != Error::NoError && error_ == Error::NoError) {
    // Build records constructor failures on a default-constructed object.
  }
  (void)e;
}

SurfacePartition::SurfacePartition(std::initializer_list<Manifold> operands)
    : SurfacePartition(vector<Manifold>(operands)) {}

SurfacePartition::Error SurfacePartition::Build(
    const std::vector<Manifold>& operands) {
  const Error previousError = error_;
  const std::string previousMessage = message_;

  auto fail = [&](Error e, std::string msg) -> Error {
    if (vertices_.empty() && triangles_.empty() && sourceMeshes_.empty()) {
      error_ = e;
      message_ = std::move(msg);
    }
    (void)previousError;
    (void)previousMessage;
    return e;
  };

  if (operands.size() > 4)
    return fail(Error::TooManyOperands, ToString(Error::TooManyOperands));

  SurfacePartition next;
  next.operands_ = operands.size();
  next.validMask_ = operands.empty() ? 0u : (1u << operands.size()) - 1u;
  next.sourceMeshes_.reserve(operands.size());

  vector<vector<array<vec3, 3>>> positions(operands.size());
  int commonExtra = -1;

  for (size_t op = 0; op < operands.size(); ++op) {
    const Manifold::Error st = operands[op].Status();
    if (st != Manifold::Error::NoError)
      return fail(Error::InvalidOperand,
                  "operand " + std::to_string(op) + " has status " +
                      std::to_string(static_cast<int>(st)));

    MeshGL64 mesh = operands[op].GetMeshGL64();
    if (!std::all_of(mesh.vertProperties.begin(), mesh.vertProperties.end(),
                     [](double x) { return Finite(x); }))
      return fail(Error::NonFiniteInput,
                  "operand " + std::to_string(op) + " is non-finite");

    const int extra = static_cast<int>(mesh.numProp) - 3;
    if (mesh.NumTri() > 0) {
      if (extra < 0)
        return fail(Error::InvalidGeometry, "numProp is less than three");
      if (commonExtra < 0) commonExtra = extra;
      if (extra != commonExtra)
        return fail(Error::PropertyCountMismatch,
                    "operand " + std::to_string(op) + " has " +
                        std::to_string(extra) + " extra properties, expected " +
                        std::to_string(commonExtra));
    }

    for (size_t vi = 0; vi < mesh.NumVert(); ++vi) {
      const size_t off = vi * mesh.numProp;
      for (int ch = 0; ch < mesh.numProp; ++ch) {
        const double x = mesh.vertProperties[off + ch];
        if (!Finite(x))
          return fail(Error::NonFiniteInput, "non-finite property");
      }
      for (int xyz = 0; xyz < 3; ++xyz) {
        const double x = mesh.vertProperties[off + xyz];
        if (std::abs(x) > 100.0 + 1.0e-9)
          return fail(Error::InvalidGeometry, "coordinate exceeds +/-100");
      }
    }
    for (double x : mesh.runTransform) {
      if (!Finite(x))
        return fail(Error::NonFiniteInput, "non-finite run transform");
    }

    positions[op].reserve(mesh.NumTri());
    for (uint64_t t = 0; t < mesh.NumTri(); ++t) {
      array<vec3, 3> p;
      for (int k = 0; k < 3; ++k) {
        const uint64_t vi = mesh.triVerts[3 * t + k];
        if (vi >= mesh.NumVert())
          return fail(Error::InvalidGeometry, "triangle vertex out of range");
        const size_t off = static_cast<size_t>(vi) * mesh.numProp;
        p[k] = vec3(mesh.vertProperties[off], mesh.vertProperties[off + 1],
                    mesh.vertProperties[off + 2]);
      }
      const double e01 = la::length(p[1] - p[0]);
      const double e12 = la::length(p[2] - p[1]);
      const double e20 = la::length(p[0] - p[2]);
      const double shortest = std::min({e01, e12, e20});
      const double longest = std::max({e01, e12, e20});
      if (shortest < 1.0e-3 - 1.0e-10)
        return fail(Error::InvalidGeometry, "source edge is shorter than 1e-3");
      const double doubleArea = la::length(la::cross(p[1] - p[0], p[2] - p[0]));
      const double minHeight = doubleArea / longest;
      if (longest / minHeight > 1000.0 + 1.0e-8)
        return fail(Error::InvalidGeometry,
                    "triangle long-edge/min-height ratio exceeds 1000");
      positions[op].push_back(p);
    }
    next.sourceMeshes_.push_back(std::move(mesh));
  }
  next.extraProps_ = std::max(0, commonExtra);

  VertexPool pool;
  vector<vector<vector<size_t>>> incident(operands.size());
  for (size_t op = 0; op < operands.size(); ++op) {
    incident[op].resize(positions[op].size());
    for (const auto& t : positions[op])
      for (const vec3& p : t) pool.Add(p);
  }

  vector<RawSeg> raw;
  for (uint32_t a = 0; a < operands.size(); ++a) {
    for (uint32_t b = a + 1; b < operands.size(); ++b) {
      for (uint32_t ta = 0; ta < positions[a].size(); ++ta) {
        for (uint32_t tb = 0; tb < positions[b].size(); ++tb) {
          const auto& A = positions[a][ta];
          const auto& B = positions[b][tb];
          if (!AABBOverlap(A, B)) continue;
          array<vec3, 2> seg;
          std::string why;
          const int hit = TriangleSegment(A, B, seg, why);
          if (hit < 0)
            return fail(Error::UnsupportedArrangement, std::move(why));
          if (hit == 0) continue;
          const uint64_t v0 = pool.Add(seg[0]);
          const uint64_t v1 = pool.Add(seg[1]);
          if (v0 == v1) continue;
          const size_t id = raw.size();
          raw.push_back({a, ta, b, tb, v0, v1});
          incident[a][ta].push_back(id);
          incident[b][tb].push_back(id);
        }
      }
    }
  }

  for (uint32_t op = 0; op < operands.size(); ++op) {
    for (uint32_t ti = 0; ti < positions[op].size(); ++ti) {
      const array<vec3, 3>& src = positions[op][ti];
      array<uint64_t, 3> corner;
      array<vec2, 3> cuv;
      for (int k = 0; k < 3; ++k) {
        corner[k] = pool.Add(src[k]);
        const auto b = Barycentric(pool.v[corner[k]], src);
        cuv[k] = BaryUV(b);
        if (k == 0) cuv[k] = vec2(0.0, 0.0);
        if (k == 1) cuv[k] = vec2(1.0, 0.0);
        if (k == 2) cuv[k] = vec2(0.0, 1.0);
      }

      vector<uint64_t> hardPoints;
      vector<uint64_t> eventPoints;
      struct LocalSeg {
        uint64_t a, b;
        vec2 ua, ub;
        vector<std::pair<double, uint64_t>> splits;
      };
      vector<LocalSeg> localSegs;
      for (size_t sid : incident[op][ti]) {
        const RawSeg& s = raw[sid];
        const uint64_t a = s.v0, b = s.v1;
        const vec2 ua = UVOf(pool, a, src);
        const vec2 ub = UVOf(pool, b, src);
        if (la::dot(ub - ua, ub - ua) < 1.0e-20) continue;
        LocalSeg ls{a, b, ua, ub, {{0.0, a}, {1.0, b}}};
        localSegs.push_back(std::move(ls));
        if (std::find(hardPoints.begin(), hardPoints.end(), a) ==
            hardPoints.end())
          hardPoints.push_back(a);
        if (std::find(hardPoints.begin(), hardPoints.end(), b) ==
            hardPoints.end())
          hardPoints.push_back(b);
      }

      for (size_t i = 0; i < localSegs.size(); ++i) {
        LocalSeg& li = localSegs[i];
        const vec2 di = li.ub - li.ua;
        const double li2 = la::dot(di, di);
        for (uint64_t hid : hardPoints) {
          const vec2 h = UVOf(pool, hid, src);
          double t = la::dot(h - li.ua, di) / li2;
          t = std::max(0.0, std::min(1.0, t));
          const vec2 q = li.ua + di * t;
          if (la::dot(h - q, h - q) < 1.0e-14 && t > 1.0e-8 &&
              t < 1.0 - 1.0e-8)
            li.splits.push_back({t, hid});
        }
        for (size_t j = i + 1; j < localSegs.size(); ++j) {
          LocalSeg& lj = localSegs[j];
          double ti2 = 0.0, tj = 0.0;
          if (!SegmentIntersection2D(li.ua, li.ub, lj.ua, lj.ub, ti2, tj))
            continue;
          if (ti2 <= 1.0e-7 || ti2 >= 1.0 - 1.0e-7 || tj <= 1.0e-7 ||
              tj >= 1.0 - 1.0e-7)
            continue;
          const vec2 uv = lj.ua + (lj.ub - lj.ua) * tj;
          const uint64_t xid = pool.Add(PointFromUV(src, uv));
          eventPoints.push_back(xid);
          li.splits.push_back({ti2, xid});
          lj.splits.push_back({tj, xid});
        }
      }

      vector<std::pair<uint64_t, uint64_t>> constraints;
      for (auto& ls : localSegs) {
        std::sort(ls.splits.begin(), ls.splits.end(),
                  [](const auto& x, const auto& y) {
                    return x.first < y.first;
                  });
        uint64_t last = ls.a;
        for (const auto& [t, id] : ls.splits) {
          if (id != last && id != ls.a && id != ls.b) {
            constraints.push_back({last, id});
            last = id;
          }
        }
        if (last != ls.b) constraints.push_back({last, ls.b});
      }

      using UTri = array<uint64_t, 3>;
      vector<UTri> mesh = {UTri{corner[0], corner[1], corner[2]}};
      auto meshHasVertex = [&](uint64_t id) {
        for (const auto& t : mesh)
          if (t[0] == id || t[1] == id || t[2] == id) return true;
        return false;
      };
      auto splitEdge = [&](uint64_t ea, uint64_t eb, uint64_t xid) {
        vector<UTri> nextMesh;
        nextMesh.reserve(mesh.size() + 4);
        for (const UTri& t : mesh) {
          int ia = -1, ib = -1;
          for (int k = 0; k < 3; ++k) {
            if (t[k] == ea) ia = k;
            if (t[k] == eb) ib = k;
          }
          if (ia < 0 || ib < 0) {
            nextMesh.push_back(t);
            continue;
          }
          const uint64_t c = t[3 - ia - ib];
          nextMesh.push_back({ea, xid, c});
          nextMesh.push_back({xid, eb, c});
        }
        mesh.swap(nextMesh);
      };

      hardPoints.insert(hardPoints.end(), eventPoints.begin(),
                        eventPoints.end());
      for (uint64_t hid : hardPoints) {
        if (meshHasVertex(hid)) continue;
        const vec2 h = UVOf(pool, hid, src);
        bool handled = false;
        for (const UTri& t : mesh) {
          for (int e = 0; e < 3 && !handled; ++e) {
            const uint64_t ea = t[e];
            const uint64_t eb = t[(e + 1) % 3];
            const vec2 a = UVOf(pool, ea, src);
            const vec2 b = UVOf(pool, eb, src);
            const vec2 ab = b - a;
            const double len2 = la::dot(ab, ab);
            const double tt = la::dot(h - a, ab) / len2;
            const vec2 q = a + ab * std::max(0.0, std::min(1.0, tt));
            if (tt > 1.0e-8 && tt < 1.0 - 1.0e-8 &&
                la::dot(h - q, h - q) < 1.0e-16) {
              splitEdge(ea, eb, hid);
              handled = true;
            }
          }
          if (handled) break;
        }
        if (handled) continue;

        for (size_t ti2 = 0; ti2 < mesh.size(); ++ti2) {
          const UTri& t = mesh[ti2];
          array<vec2, 3> q = {UVOf(pool, t[0], src), UVOf(pool, t[1], src),
                              UVOf(pool, t[2], src)};
          const double ar = Cross2(q[1] - q[0], q[2] - q[0]);
          bool inside = true;
          for (int e = 0; e < 3; ++e) {
            const double s = Cross2(q[(e + 1) % 3] - q[e], h - q[e]);
            if (ar > 0.0 ? s < -1.0e-10 : s > 1.0e-10) inside = false;
          }
          if (!inside) continue;
          vector<UTri> nextMesh;
          nextMesh.reserve(mesh.size() + 2);
          for (size_t k = 0; k < mesh.size(); ++k) {
            if (k == ti2) {
              nextMesh.push_back({t[0], t[1], hid});
              nextMesh.push_back({t[1], t[2], hid});
              nextMesh.push_back({t[2], t[0], hid});
            } else {
              nextMesh.push_back(mesh[k]);
            }
          }
          mesh.swap(nextMesh);
          break;
        }
      }

      auto cleanMesh = [&]() {
        vector<UTri> out;
        std::set<std::array<uint64_t, 3>> seen;
        for (const UTri& t : mesh) {
          if (t[0] == t[1] || t[1] == t[2] || t[2] == t[0]) continue;
          const vec2 a = UVOf(pool, t[0], src);
          const vec2 b = UVOf(pool, t[1], src);
          const vec2 c = UVOf(pool, t[2], src);
          if (std::abs(Cross2(b - a, c - a)) < 1.0e-14) continue;
          UTri key = t;
          std::sort(key.begin(), key.end());
          if (seen.insert(key).second) out.push_back(t);
        }
        mesh.swap(out);
      };

      auto edgeExists = [&](uint64_t a, uint64_t b) {
        for (const UTri& t : mesh)
          for (int e = 0; e < 3; ++e) {
            const uint64_t x = t[e], y = t[(e + 1) % 3];
            if ((x == a && y == b) || (x == b && y == a)) return true;
          }
        return false;
      };

      bool arrangementOK = true;
      for (auto [a, b] : constraints) {
        if (a == b) continue;
        cleanMesh();
        if (edgeExists(a, b)) continue;
        const vec2 ua = UVOf(pool, a, src);
        const vec2 ub = UVOf(pool, b, src);
        vector<unsigned char> marked(mesh.size(), 0);
        size_t markCount = 0;
        for (size_t ti2 = 0; ti2 < mesh.size(); ++ti2) {
          const UTri& t = mesh[ti2];
          array<vec2, 3> q = {UVOf(pool, t[0], src), UVOf(pool, t[1], src),
                              UVOf(pool, t[2], src)};
          if (SegmentTriangleInterval(ua, ub, q)) {
            marked[ti2] = 1;
            ++markCount;
          }
        }
        if (markCount == 0) {
          arrangementOK = false;
          break;
        }

        std::map<std::pair<uint64_t, uint64_t>, int> edgeCount;
        for (size_t ti2 = 0; ti2 < mesh.size(); ++ti2) {
          if (!marked[ti2]) continue;
          const UTri& t = mesh[ti2];
          for (int e = 0; e < 3; ++e) {
            uint64_t x = t[e], y = t[(e + 1) % 3];
            if (x > y) std::swap(x, y);
            edgeCount[{x, y}]++;
          }
        }
        std::map<uint64_t, vector<uint64_t>> adj;
        for (const auto& [e, cnt] : edgeCount) {
          if (cnt != 1) continue;
          adj[e.first].push_back(e.second);
          adj[e.second].push_back(e.first);
        }

        vector<vector<uint64_t>> loops;
        std::set<std::pair<uint64_t, uint64_t>> usedBoundary;
        for (const auto& [e, cnt] : edgeCount) {
          if (cnt != 1) continue;
          auto directed = e;
          if (usedBoundary.count(directed)) continue;
          vector<uint64_t> loop;
          uint64_t start = e.first;
          uint64_t prev = e.first;
          uint64_t cur = e.second;
          loop.push_back(start);
          size_t guard = 0;
          while (cur != start && guard++ < mesh.size() + 10) {
            loop.push_back(cur);
            usedBoundary.insert({std::min(prev, cur), std::max(prev, cur)});
            const auto it = adj.find(cur);
            if (it == adj.end()) {
              arrangementOK = false;
              break;
            }
            uint64_t next = cur;
            for (uint64_t cand : it->second)
              if (cand != prev) {
                next = cand;
                break;
              }
            if (next == cur) {
              arrangementOK = false;
              break;
            }
            prev = cur;
            cur = next;
          }
          if (!arrangementOK) break;
          usedBoundary.insert({std::min(prev, cur), std::max(prev, cur)});
          loops.push_back(std::move(loop));
        }
        if (!arrangementOK) break;

        vector<UTri> replacement;
        for (const auto& loop : loops) {
          auto iaIt = std::find(loop.begin(), loop.end(), a);
          auto ibIt = std::find(loop.begin(), loop.end(), b);
          if (iaIt == loop.end() || ibIt == loop.end()) continue;
          size_t ia = static_cast<size_t>(iaIt - loop.begin());
          size_t ib = static_cast<size_t>(ibIt - loop.begin());
          const size_t L = loop.size();
          vector<uint64_t> c1;
          for (size_t k = ia;; k = (k + 1) % L) {
            c1.push_back(loop[k]);
            if (k == ib) break;
          }
          vector<uint64_t> c2;
          for (size_t k = ib;; k = (k + 1) % L) {
            c2.push_back(loop[k]);
            if (k == ia) break;
          }
          for (const vector<uint64_t>& chain : {c1, c2}) {
            Poly poly;
            for (uint64_t id : chain)
              poly.push_back({id, UVOf(pool, id, src)});
            const std::set<uint64_t> noExtraPreserve;
            vector<array<uint64_t, 3>> tt = EarTriangulateChain(
                std::move(poly), a, b, noExtraPreserve);
            for (const auto& x : tt) replacement.push_back(x);
          }
        }
        if (replacement.empty()) {
          arrangementOK = false;
          break;
        }

        vector<UTri> nextMesh;
        for (size_t ti2 = 0; ti2 < mesh.size(); ++ti2)
          if (!marked[ti2]) nextMesh.push_back(mesh[ti2]);
        for (const UTri& t : replacement) nextMesh.push_back(t);
        mesh.swap(nextMesh);
      }
      if (!arrangementOK) {
        return fail(Error::UnsupportedArrangement,
                    "could not enforce a common intersection constraint");
      }

      const vec3 sourceNormal =
          la::cross(src[1] - src[0], src[2] - src[0]);
      for (const UTri& ids : mesh) {
          if (ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0])
            continue;
          array<array<double, 3>, 3> bary;
          for (int c = 0; c < 3; ++c)
            bary[c] = Barycentric(pool.v[ids[c]], src);

          vec3 n = la::cross(pool.v[ids[1]] - pool.v[ids[0]],
                             pool.v[ids[2]] - pool.v[ids[0]]);
          array<uint64_t, 3> oriented = ids;
          if (la::dot(n, sourceNormal) < 0.0) {
            std::swap(oriented[1], oriented[2]);
            std::swap(bary[1], bary[2]);
          }

          vec3 centroid = (pool.v[oriented[0]] + pool.v[oriented[1]] +
                           pool.v[oriented[2]]) /
                          3.0;
          uint32_t other = 0;
          for (uint32_t j = 0; j < operands.size(); ++j) {
            if (j == op || positions[j].empty()) continue;
            if (IsInside(centroid, positions[j])) other |= 1u << j;
          }
          SurfacePartition::Triangle tri;
          tri.vertices = {oriented[0], oriented[1], oriented[2]};
          tri.minusMask = other | (1u << op);
          tri.plusMask = other & ~(1u << op);
          tri.sourceOperand = op;
          tri.sourceTriangle = ti;
          for (int c = 0; c < 3; ++c)
            tri.sourceBarycentric[c] =
                vec3(bary[c][0], bary[c][1], bary[c][2]);
          next.triangles_.push_back(std::move(tri));
      }
    }
  }

  next.vertices_ = std::move(pool.v);
  next.error_ = Error::NoError;
  next.message_.clear();
  *this = std::move(next);
  (void)previousError;
  (void)previousMessage;
  return Error::NoError;
}

}  // namespace manifold
