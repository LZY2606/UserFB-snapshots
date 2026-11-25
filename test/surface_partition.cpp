#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

#include "manifold/manifold.h"
#include "manifold/mesh.h"
#include "manifold/surface_partition.h"

using namespace manifold;

namespace {

using Gid = uint64_t;
using CoordKey = std::array<uint64_t, 3>;
using TriKey = std::array<CoordKey, 3>;

constexpr double kGeomTol = 1e-8;
constexpr double kVolumeTol = 2e-7;

uint64_t DoubleBits(double x) {
  if (x == 0.0) x = 0.0;  // Make +0 and -0 compare equal.
  uint64_t u;
  std::memcpy(&u, &x, sizeof(u));
  return u;
}

CoordKey KeyOf(const vec3& p) {
  return {DoubleBits(p.x), DoubleBits(p.y), DoubleBits(p.z)};
}

TriKey KeyOfTri(const std::array<CoordKey, 3>& c) {
  TriKey k{c[0], c[1], c[2]};
  std::sort(k.begin(), k.end());
  return k;
}

struct MeshCoordIndex {
  explicit MeshCoordIndex(const MeshGL64& g) {
    for (uint64_t t = 0; t < g.NumTri(); ++t) {
      std::array<CoordKey, 3> oriented;
      for (int k = 0; k < 3; ++k) {
        const uint64_t vi = g.triVerts[3 * t + k];
        oriented[k] = KeyOf(g.GetVertPos(vi));
      }
      const TriKey key = KeyOfTri(oriented);
      orientations[key].push_back(oriented);
    }
  }

  bool HasOriented(const std::array<CoordKey, 3>& o) const {
    auto it = orientations.find(KeyOfTri(o));
    if (it == orientations.end()) return false;
    return std::find(it->second.begin(), it->second.end(), o) !=
           it->second.end();
  }

  std::map<TriKey, std::vector<std::array<CoordKey, 3>>> orientations;
};

struct DSU {
  std::vector<int> p;
  explicit DSU(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0); }
  int find(int x) {
    if (p[x] == x) return x;
    return p[x] = find(p[x]);
  }
  void unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) p[b] = a;
  }
};

double Distance(const vec3& a, const vec3& b) {
  return std::sqrt(dot(a - b, a - b));
}

bool CheckClosedMesh(const Manifold& m, double* signedVolume = nullptr) {
  if (m.Status() != Manifold::Error::NoError) return false;
  const MeshGL64 g = m.GetMeshGL64();
  if (g.NumTri() < 4) return false;

  const size_t nv = g.NumVert();
  DSU dsu(nv);
  for (size_t i = 0; i < g.mergeFromVert.size(); ++i) {
    if (g.mergeFromVert[i] >= nv || g.mergeToVert[i] >= nv) return false;
    dsu.unite(static_cast<int>(g.mergeFromVert[i]),
              static_cast<int>(g.mergeToVert[i]));
  }

  std::map<std::pair<int, int>, int> directed;
  std::map<int, std::map<int, std::map<int, int>>> linkAdj;
  double volume6 = 0.0;

  for (uint64_t t = 0; t < g.NumTri(); ++t) {
    int r[3];
    vec3 p[3];
    for (int k = 0; k < 3; ++k) {
      const uint64_t vi = g.triVerts[3 * t + k];
      if (vi >= nv) return false;
      r[k] = dsu.find(static_cast<int>(vi));
      p[k] = g.GetVertPos(vi);
    }
    if (r[0] == r[1] || r[1] == r[2] || r[2] == r[0]) return false;

    for (int k = 0; k < 3; ++k) {
      const int a = r[k];
      const int b = r[(k + 1) % 3];
      ++directed[{a, b}];
    }

    volume6 += dot(p[0], cross(p[1], p[2]));

    for (int k = 0; k < 3; ++k) {
      const int v = r[k];
      const int a = r[(k + 1) % 3];
      const int b = r[(k + 2) % 3];
      ++linkAdj[v][a][b];
      ++linkAdj[v][b][a];
    }
  }

  // Every directed halfedge has its opposite.  In a clean extracted region
  // each orientation occurs once.
  for (const auto& [e, count] : directed) {
    auto rev = directed.find({e.second, e.first});
    if (rev == directed.end() || rev->second != count) return false;
  }

  // One complete vertex fan per geometric vertex.  Around a mesh vertex,
  // the other two corners of each incident triangle form an edge of its
  // one-dimensional link; a closed manifold fan is one cycle in which every
  // link vertex has degree two.
  for (const auto& [v, adj] : linkAdj) {
    for (const auto& [n, neighbors] : adj) {
      int degree = 0;
      for (const auto& [w, multiplicity] : neighbors) degree += multiplicity;
      if (degree != 2) return false;
    }
    std::set<int> seen;
    std::vector<int> stack{adj.begin()->first};
    while (!stack.empty()) {
      int x = stack.back();
      stack.pop_back();
      if (!seen.insert(x).second) continue;
      auto it = adj.find(x);
      if (it == adj.end()) return false;
      for (const auto& [y, multiplicity] : it->second) {
        (void)multiplicity;
        if (!seen.count(y)) stack.push_back(y);
      }
    }
    if (seen.size() != adj.size()) return false;
  }

  const double sv = volume6 / 6.0;
  if (signedVolume) *signedVolume = sv;
  if (!(sv > 0.0)) return false;
  if (std::abs(sv - m.Volume()) > kVolumeTol) return false;
  return true;
}

bool CheckPartitionEdgesNoTJunctions(const SurfacePartition& p) {
  const auto& verts = p.Vertices();
  const auto& faces = p.Faces();
  std::map<std::pair<Gid, Gid>, int> edgeFaces;
  std::vector<std::pair<Gid, Gid>> edges;
  for (const auto& f : faces) {
    for (int k = 0; k < 3; ++k) {
      Gid a = f.vertices[k];
      Gid b = f.vertices[(k + 1) % 3];
      if (a == b) return false;
      Gid lo = std::min(a, b), hi = std::max(a, b);
      ++edgeFaces[{lo, hi}];
    }
  }
  for (const auto& [e, n] : edgeFaces) {
    if (n != 2 && n != 4) return false;
    edges.push_back(e);
  }

  // No arrangement vertex may sit in the interior of another edge.  The
  // input contract separates unrelated events/edges by 1e-6, so 1e-7 is a
  // safe independent probe.
  for (const auto& [a, b] : edges) {
    const vec3 A = verts[a];
    const vec3 B = verts[b];
    const vec3 d = B - A;
    const double l2 = dot(d, d);
    if (!(l2 > 1e-12)) return false;
    for (Gid g = 0; g < verts.size(); ++g) {
      if (g == a || g == b) continue;
      const vec3 q = verts[g] - A;
      const double t = dot(q, d) / l2;
      if (!(t > 1e-7 && t < 1.0 - 1e-7)) continue;
      const vec3 closest = A + t * d;
      if (Distance(verts[g], closest) < 1e-7) return false;
    }
  }
  return true;
}

bool CheckBarycentricBacksubstitution(const SurfacePartition& p,
                                      double tol = kGeomTol) {
  const auto& sm = p.SourceMeshes();
  for (const auto& f : p.Faces()) {
    if (f.sourceOperand >= sm.size()) return false;
    const MeshGL64& m = sm[f.sourceOperand];
    if (f.sourceTriangle >= m.NumTri()) return false;
    std::array<vec3, 3> sp{};
    for (int j = 0; j < 3; ++j) {
      const uint64_t vi = m.triVerts[3 * f.sourceTriangle + j];
      sp[j] = m.GetVertPos(vi);
    }
    for (int k = 0; k < 3; ++k) {
      const vec3& w = f.sourceBarycentric[k];
      if (std::abs(w.x + w.y + w.z - 1.0) > 1e-9) return false;
      if (w.x < -1e-8 || w.y < -1e-8 || w.z < -1e-8) return false;
      const vec3 back = w.x * sp[0] + w.y * sp[1] + w.z * sp[2];
      if (Distance(back, p.Vertices()[f.vertices[k]]) > tol) return false;
    }
  }
  return true;
}

std::vector<Manifold> JunctionOperands() {
  return {
      Manifold::Cube({2.0, 2.0, 2.0}, true),
      Manifold::Cube({1.55, 1.85, 1.65}, true)
          .Rotate(13.0, 29.0, 37.0)
          .Translate({0.16, 0.06, 0.11}),
      Manifold::Cube({1.75, 1.45, 1.95}, true)
          .Rotate(-17.0, 23.0, -11.0)
          .Translate({-0.11, 0.13, 0.07}),
  };
}

bool BoxContains(const vec3& p, const vec3& c, const vec3& h) {
  return std::abs(p.x - c.x) <= h.x && std::abs(p.y - c.y) <= h.y &&
         std::abs(p.z - c.z) <= h.z;
}

struct SeamData {
  Manifold manifold;
  uint32_t originalID = 0;
};

SeamData MakeSeamCube() {
  const MeshGL64 base = Manifold::Cube({1.7, 1.3, 1.1}, true).GetMeshGL64();
  MeshGL64 g;
  g.numProp = 5;
  const size_t nt = base.NumTri();
  g.triVerts.resize(3 * nt);
  g.vertProperties.reserve(3 * nt * 5);
  std::vector<int> canonical(base.NumVert(), -1);

  for (size_t t = 0; t < nt; ++t) {
    for (int k = 0; k < 3; ++k) {
      const uint64_t oldVi = base.triVerts[3 * t + k];
      const vec3 p = base.GetVertPos(oldVi);
      const uint64_t newVi = g.NumVert();
      g.triVerts[3 * t + k] = newVi;
      g.vertProperties.insert(
          g.vertProperties.end(),
          {p.x, p.y, p.z, 10.0 * static_cast<double>(t + 1) + k,
           3.0 * p.x + 2.0 * p.y - p.z + 0.25 * k});
      if (canonical[oldVi] < 0) {
        canonical[oldVi] = static_cast<int>(newVi);
      } else {
        g.mergeFromVert.push_back(newVi);
        g.mergeToVert.push_back(
            static_cast<uint64_t>(canonical[oldVi]));
      }
    }
    g.faceID.push_back(static_cast<uint64_t>(9000 + t));
  }
  g.runIndex = {0, static_cast<uint64_t>(3 * nt)};
  const uint32_t oid = Manifold::ReserveIDs(1);
  g.runOriginalID = {oid};
  Manifold m(g);
  return {m, oid};
}

}  // namespace

extern const struct task_case task_cases[] = {
    {"junctions", "three_oblique_solids_share_triple_vertex",
     []() -> int {
       auto ops = JunctionOperands();
       SurfacePartition p(ops);
       TASK_CHECK(p.GetStatus() == SurfacePartition::Status::Ok);
       TASK_CHECK(p.Valid());
       TASK_CHECK(p.OperandCount() == 3);
       TASK_CHECK(!p.Faces().empty());
       TASK_CHECK(CheckBarycentricBacksubstitution(p));
       TASK_CHECK(CheckPartitionEdgesNoTJunctions(p));

       std::map<Gid, std::set<uint32_t>> opsAtVertex;
       for (const auto& f : p.Faces()) {
         for (Gid g : f.vertices) opsAtVertex[g].insert(f.sourceOperand);
       }
       int tripleVertices = 0;
       for (const auto& [g, opsHere] : opsAtVertex) {
         if (opsHere.size() >= 3) {
           ++tripleVertices;
           TASK_CHECK(g < p.Vertices().size());
           TASK_CHECK(std::isfinite(p.Vertices()[g].x));
         }
       }
       TASK_CHECK(tripleVertices >= 1);

       // Masks on every face differ by exactly the source bit.
       for (const auto& f : p.Faces()) {
         const uint8_t expected = static_cast<uint8_t>(1u << f.sourceOperand);
         TASK_CHECK((f.minusMask ^ f.plusMask) == expected);
         TASK_CHECK((f.minusMask & expected) != 0);
         TASK_CHECK((f.plusMask & expected) == 0);
         TASK_CHECK(f.minusMask < 8 && f.plusMask < 8);
       }
       return 0;
     }},

    {"regions", "all_masks_are_closed_and_match_analytic_boxes",
     []() -> int {
       const vec3 c0{0, 0, 0}, h0{1, 1, 1};
       const vec3 c1{0.4, 0.35, 0.3}, h1{0.8, 0.8, 0.8};
       const vec3 c2{-0.28, 0.18, 0.12}, h2{0.68, 0.88, 0.58};
       std::vector<Manifold> ops = {
           Manifold::Cube(2.0 * h0, true).Translate(c0),
           Manifold::Cube(2.0 * h1, true).Translate(c1),
           Manifold::Cube(2.0 * h2, true).Translate(c2),
       };
       SurfacePartition p(ops);
       TASK_CHECK(p.Valid());
       TASK_CHECK(CheckBarycentricBacksubstitution(p));
       TASK_CHECK(CheckPartitionEdgesNoTJunctions(p));

       std::vector<Manifold> regions(8);
       std::vector<MeshCoordIndex> indices;
       for (uint32_t mask = 1; mask < 8; ++mask) {
         bool ok = p.TryExtract(mask, regions[mask]);
         TASK_CHECK(ok);
         TASK_CHECK(regions[mask].Status() == Manifold::Error::NoError);
         if (!regions[mask].IsEmpty()) {
           double sv = 0.0;
           TASK_CHECK(CheckClosedMesh(regions[mask], &sv));
           TASK_CHECK(sv > 1e-10);
         }
       }

       // Exact same coordinate triangle for both non-exterior sides.
       for (uint32_t mask = 1; mask < 8; ++mask) {
         indices.emplace_back(regions[mask].GetMeshGL64());
       }
       for (const auto& f : p.Faces()) {
         std::array<CoordKey, 3> stored = {
             KeyOf(p.Vertices()[f.vertices[0]]),
             KeyOf(p.Vertices()[f.vertices[1]]),
             KeyOf(p.Vertices()[f.vertices[2]])};
         std::array<CoordKey, 3> reversed = {stored[0], stored[2],
                                             stored[1]};
         TASK_CHECK(f.minusMask >= 1 && f.minusMask < 8);
         TASK_CHECK(indices[f.minusMask - 1].HasOriented(stored));
         if (f.plusMask != 0) {
           TASK_CHECK(indices[f.plusMask - 1].HasOriented(reversed));
         }
       }

       // Explicit analytic occupancy probes.
       const std::vector<vec3> probes = {
           {-1.37, -0.91, -0.43}, {-0.83, -0.21, 0.37},
           {0.68, 0.72, 0.51},    {0.91, -0.63, 0.27},
           {-0.55, 0.83, 0.41},   {0.25, 0.15, -0.42},
           {1.27, 1.13, 0.97},    {-0.15, 0.45, 0.25}};
       std::vector<uint32_t> expected(probes.size(), 0);
       for (size_t i = 0; i < probes.size(); ++i) {
         if (BoxContains(probes[i], c0, h0)) expected[i] |= 1u;
         if (BoxContains(probes[i], c1, h1)) expected[i] |= 2u;
         if (BoxContains(probes[i], c2, h2)) expected[i] |= 4u;
       }
       for (uint32_t mask = 1; mask < 8; ++mask) {
         if (regions[mask].IsEmpty()) continue;
         std::vector<int> wn = regions[mask].WindingNumber(probes);
         TASK_CHECK(wn.size() == probes.size());
         for (size_t i = 0; i < probes.size(); ++i) {
           const bool inside = wn[i] != 0;
           TASK_CHECK(inside == (expected[i] == mask));
         }
       }

       // Volume conservation by operand occupancy and by total union.
       double sumAll = 0.0;
       std::array<double, 3> sumByBit{0.0, 0.0, 0.0};
       for (uint32_t mask = 1; mask < 8; ++mask) {
         const double v = regions[mask].Volume();
         sumAll += v;
         for (int b = 0; b < 3; ++b)
           if (mask & (1u << b)) sumByBit[b] += v;
       }
       for (int b = 0; b < 3; ++b) {
         TASK_CHECK(std::abs(sumByBit[b] - ops[b].Volume()) < kVolumeTol);
       }
       Manifold unionAll = Manifold::BatchBoolean(ops, OpType::Add);
       TASK_CHECK(unionAll.Status() == Manifold::Error::NoError);
       TASK_CHECK(std::abs(sumAll - unionAll.Volume()) < kVolumeTol);
       return 0;
     }},

    {"regions", "nonconvex_nested_cavity_and_separate_component",
     []() -> int {
       Manifold outer = Manifold::Cube({3.0, 3.0, 3.0}, true);
       Manifold cavity =
           Manifold::Cube({1.0, 1.0, 1.0}, true).Translate({0.25, 0.25, 0.25});
       Manifold wing =
           Manifold::Cube({0.8, 0.8, 0.8}, true).Translate({2.2, 0.0, 0.0});
       Manifold body = (outer - cavity) + wing;
       Manifold embedded = Manifold::Cube({0.35, 0.5, 0.4}, true)
                               .Translate({1.05, 0.1, -0.2});
       TASK_CHECK(body.Status() == Manifold::Error::NoError);
       SurfacePartition p({body, embedded});
       TASK_CHECK(p.Valid());
       TASK_CHECK(CheckBarycentricBacksubstitution(p));

       size_t embeddedFaces = 0;
       for (const auto& f : p.Faces()) {
         if (f.sourceOperand == 1) {
           ++embeddedFaces;
           TASK_CHECK(f.minusMask == 3);
           TASK_CHECK(f.plusMask == 1);
         }
       }
       TASK_CHECK(embeddedFaces == 12);  // Entire contained cube shell.

       Manifold shellRegion = p.Extract(1);
       Manifold innerRegion = p.Extract(3);
       TASK_CHECK(CheckClosedMesh(shellRegion));
       TASK_CHECK(CheckClosedMesh(innerRegion));

       const std::vector<vec3> probes = {
           {0.25, 0.25, 0.25},  // cavity -> outside everything
           {1.05, 0.1, -0.2},   // embedded operand
           {1.3, 0.0, 0.0},     // body material
           {2.2, 0.0, 0.0},     // separate wing component
           {4.0, 4.0, 4.0}};
       std::vector<int> w1 = shellRegion.WindingNumber(probes);
       std::vector<int> w3 = innerRegion.WindingNumber(probes);
       TASK_CHECK(w1.size() == probes.size() && w3.size() == probes.size());
       TASK_CHECK(w1[0] == 0 && w3[0] == 0);
       TASK_CHECK(w1[1] == 0 && w3[1] != 0);
       TASK_CHECK(w1[2] != 0 && w3[2] == 0);
       TASK_CHECK(w1[3] != 0 && w3[3] == 0);
       TASK_CHECK(w1[4] == 0 && w3[4] == 0);

       double bit0 = 0.0, all = 0.0;
       for (uint32_t mask = 1; mask < 4; ++mask) {
         Manifold r = p.Extract(mask);
         TASK_CHECK(r.Status() == Manifold::Error::NoError);
         all += r.Volume();
         if (mask & 1u) bit0 += r.Volume();
       }
       TASK_CHECK(std::abs(bit0 - body.Volume()) < kVolumeTol);
       TASK_CHECK(p.Extract(2).IsEmpty());
       TASK_CHECK(std::abs(p.Extract(3).Volume() - embedded.Volume()) <
                  kVolumeTol);
       TASK_CHECK(std::abs(all - body.Volume()) < kVolumeTol);
       return 0;
     }},

    {"junctions", "four_operand_masks_and_shared_arrangement",
     []() -> int {
       std::vector<Manifold> ops = JunctionOperands();
       ops.push_back(Manifold::Cube({1.25, 1.55, 1.35}, true)
                         .Rotate(21.0, -13.0, 41.0)
                         .Translate({0.34, -0.28, 0.22}));
       SurfacePartition p(ops);
       TASK_CHECK(p.Valid());
       TASK_CHECK(p.OperandCount() == 4);
       TASK_CHECK(CheckBarycentricBacksubstitution(p));
       TASK_CHECK(CheckPartitionEdgesNoTJunctions(p));

       for (const auto& f : p.Faces()) {
         TASK_CHECK(f.minusMask < 16 && f.plusMask < 16);
         TASK_CHECK((f.minusMask ^ f.plusMask) ==
                    (1u << f.sourceOperand));
       }
       for (uint32_t mask = 1; mask < 16; ++mask) {
         Manifold r;
         TASK_CHECK(p.TryExtract(mask, r));
         TASK_CHECK(r.Status() == Manifold::Error::NoError);
         if (!r.IsEmpty()) TASK_CHECK(CheckClosedMesh(r));
       }

       std::array<double, 4> byBit{};
       double all = 0.0;
       for (uint32_t mask = 1; mask < 16; ++mask) {
         const double v = p.Extract(mask).Volume();
         all += v;
         for (int b = 0; b < 4; ++b)
           if (mask & (1u << b)) byBit[b] += v;
       }
       for (int b = 0; b < 4; ++b)
         TASK_CHECK(std::abs(byBit[b] - ops[b].Volume()) < kVolumeTol);
       Manifold u = Manifold::BatchBoolean(ops, OpType::Add);
       TASK_CHECK(u.Status() == Manifold::Error::NoError);
       TASK_CHECK(std::abs(all - u.Volume()) < kVolumeTol);
       return 0;
     }},

    {"properties", "face_corner_seams_barycentric_and_native_runs",
     []() -> int {
       SeamData seam = MakeSeamCube();
       TASK_CHECK(seam.manifold.Status() == Manifold::Error::NoError);
       Manifold other =
           Manifold::Cube({1.5, 1.4, 1.6}, true)
               .SetProperties(
                   2, [](double* out, vec3 p, const double*) {
                     out[0] = 2.0 * p.x - p.y + 0.35 * p.z;
                     out[1] = p.x + 4.0 * p.y - 2.0 * p.z;
                   })
               .Rotate(7.0, 17.0, 23.0)
               .Translate({0.25, 0.1, 0.05});
       SurfacePartition p({seam.manifold, other});
       TASK_CHECK(p.Valid());
       TASK_CHECK(p.ExtraPropertyCount() == 2);
       TASK_CHECK(CheckBarycentricBacksubstitution(p));

       Manifold r = p.Extract(1);
       TASK_CHECK(CheckClosedMesh(r));
       MeshGL64 out = r.GetMeshGL64();
       TASK_CHECK(out.numProp == 5);
       TASK_CHECK(!out.runOriginalID.empty());
       bool foundOid = false;
       for (uint32_t id : out.runOriginalID)
         if (id == seam.originalID) foundOid = true;
       TASK_CHECK(foundOid);
       TASK_CHECK(out.runTransform.size() ==
                  12 * static_cast<size_t>(out.NumRun()));

       const MeshGL64& src = p.SourceMeshes()[0];
       TASK_CHECK(!src.faceID.empty());
       std::map<TriKey, std::vector<size_t>> faceByKey;
       for (const auto& f : p.Faces()) {
         std::array<CoordKey, 3> c = {
             KeyOf(p.Vertices()[f.vertices[0]]),
             KeyOf(p.Vertices()[f.vertices[1]]),
             KeyOf(p.Vertices()[f.vertices[2]])};
         faceByKey[KeyOfTri(c)].push_back(&f - &p.Faces().front());
       }

       double maxPosErr = 0.0;
       double maxBaryErr = 0.0;
       double maxPropErr = 0.0;
       int checked = 0;
       for (uint64_t t = 0; t < out.NumTri(); ++t) {
         std::array<CoordKey, 3> oc;
         for (int k = 0; k < 3; ++k)
           oc[k] = KeyOf(out.GetVertPos(out.triVerts[3 * t + k]));
         auto it = faceByKey.find(KeyOfTri(oc));
         TASK_CHECK(it != faceByKey.end());
         const SurfacePartition::Face* hit = nullptr;
         int outputSlot[3] = {-1, -1, -1};
         for (size_t fi : it->second) {
           const auto& f = p.Faces()[fi];
           if (f.sourceOperand != 0) continue;
           bool good = true;
           int slots[3] = {-1, -1, -1};
           for (int ok = 0; ok < 3; ++ok) {
             CoordKey ck = KeyOf(p.Vertices()[f.vertices[ok]]);
             for (int ik = 0; ik < 3; ++ik)
               if (oc[ik] == ck) slots[ok] = ik;
             if (slots[ok] < 0) good = false;
           }
           if (good) {
             hit = &f;
             for (int k = 0; k < 3; ++k) outputSlot[k] = slots[k];
             break;
           }
         }
         if (hit == nullptr) continue;
         TASK_CHECK(outputSlot[0] >= 0 && outputSlot[1] >= 0 &&
                    outputSlot[2] >= 0);
         if (hit->sourceOperand != 0) continue;
         TASK_CHECK(out.faceID.size() == out.NumTri());
         TASK_CHECK(out.faceID[t] == src.faceID[hit->sourceTriangle]);

         std::array<vec3, 3> sp{};
         for (int j = 0; j < 3; ++j)
           sp[j] = src.GetVertPos(src.triVerts[3 * hit->sourceTriangle + j]);
         for (int q = 0; q < 3; ++q) {
           const vec3& w = hit->sourceBarycentric[q];
           vec3 back = w.x * sp[0] + w.y * sp[1] + w.z * sp[2];
           maxPosErr =
               std::max(maxPosErr, Distance(back, p.Vertices()[hit->vertices[q]]));
           maxBaryErr = std::max(
               maxBaryErr, std::abs(w.x + w.y + w.z - 1.0));
           const uint64_t outVi = out.triVerts[3 * t + outputSlot[q]];
           for (int c = 0; c < 2; ++c) {
             double expected = 0.0;
             const uint64_t st = hit->sourceTriangle;
             const uint64_t ps[3] = {src.triVerts[3 * st],
                                     src.triVerts[3 * st + 1],
                                     src.triVerts[3 * st + 2]};
             for (int j = 0; j < 3; ++j) {
               const double bw[3] = {w.x, w.y, w.z};
               expected +=
                   bw[j] *
                   src.vertProperties[ps[j] * src.numProp + 3 + c];
             }
             const double actual =
                 out.vertProperties[outVi * out.numProp + 3 + c];
             maxPropErr = std::max(maxPropErr, std::abs(actual - expected));
           }
           ++checked;
         }
       }
       TASK_CHECK(checked > 0);
       TASK_CHECK(maxPosErr <= 1e-8);
       TASK_CHECK(maxBaryErr <= 1e-8);
       TASK_CHECK(maxPropErr <= 1e-8);

       // MeshGL native source IDs survive another construction/output cycle.
       Manifold again(out);
       TASK_CHECK(again.Status() == Manifold::Error::NoError);
       bool survived = false;
       for (uint32_t id : again.GetMeshGL64().runOriginalID)
         if (id == seam.originalID) survived = true;
       TASK_CHECK(survived);
       return 0;
     }},

    {"reuse", "invalid_arguments_and_failed_extract_leave_partition_usable",
     []() -> int {
       Manifold a = Manifold::Cube({1.4, 1.2, 1.3}, true);
       Manifold b = Manifold::Cube({1.2, 1.5, 1.1}, true)
                        .Translate({0.35, 0.1, 0.2});
       SurfacePartition good({a, b});
       SurfacePartition direct(a, b);
       TASK_CHECK(good.Valid());
       TASK_CHECK(direct.Valid());
       TASK_CHECK(direct.OperandCount() == 2);

       bool threwZero = false;
       try {
         (void)good.Extract(0);
       } catch (const std::invalid_argument&) {
         threwZero = true;
       }
       TASK_CHECK(threwZero);
       Manifold afterBad;
       TASK_CHECK(!good.TryExtract(0, afterBad));
       TASK_CHECK(!good.TryExtract(4, afterBad));

       Manifold r1 = good.Extract(1);
       Manifold r2 = good.Extract(1);
       TASK_CHECK(CheckClosedMesh(r1));
       TASK_CHECK(CheckClosedMesh(r2));
       TASK_CHECK(r1.GetMeshGL64().NumTri() == r2.GetMeshGL64().NumTri());
       TASK_CHECK(std::abs(r1.Volume() - r2.Volume()) < 1e-12);

       std::vector<Manifold> five(5, a);
       SurfacePartition tooMany(five);
       TASK_CHECK(!tooMany.Valid());
       TASK_CHECK(tooMany.GetStatus() ==
                  SurfacePartition::Status::TooManyOperands);

       MeshGL64 badGL;
       badGL.numProp = 3;
       badGL.vertProperties = {0, 0, 0, 1, 0, 0, 0, 1, 0};
       badGL.triVerts = {0, 1, 2};
       Manifold badManifold(badGL);
       TASK_CHECK(badManifold.Status() != Manifold::Error::NoError);
       SurfacePartition badPartition({badManifold});
       TASK_CHECK(!badPartition.Valid());

       Manifold nanCube = Manifold::Cube().Warp([](vec3& p) {
         if (p.x > 0.49) p.x = std::numeric_limits<double>::quiet_NaN();
       });
       SurfacePartition nanPartition({nanCube});
       TASK_CHECK(!nanPartition.Valid());

       Manifold p1 = Manifold::Cube().SetProperties(1, [](double* o, vec3,
                                                          const double*) {
         o[0] = 1.0;
       });
       Manifold p2 = Manifold::Cube().SetProperties(2, [](double* o, vec3,
                                                          const double*) {
         o[0] = 1.0;
         o[1] = 2.0;
       });
       SurfacePartition propMismatch({p1, p2});
       TASK_CHECK(propMismatch.GetStatus() ==
                  SurfacePartition::Status::InvalidPropertyCount);

       SurfacePartition emptyList;
       TASK_CHECK(emptyList.Valid());
       TASK_CHECK(emptyList.OperandCount() == 0);
       TASK_CHECK(emptyList.Faces().empty());

       SurfacePartition emptyOperand({Manifold(), a});
       TASK_CHECK(emptyOperand.Valid());
       TASK_CHECK(emptyOperand.OperandCount() == 2);
       Manifold onlyA = emptyOperand.Extract(2);
       TASK_CHECK(CheckClosedMesh(onlyA));
       TASK_CHECK(std::abs(onlyA.Volume() - a.Volume()) < kVolumeTol);
       return 0;
     }},

    {"reuse", "partition_survives_source_destruction_and_old_booleans_work",
     []() -> int {
       SurfacePartition p = [] {
         Manifold a = Manifold::Cube({1.6, 1.4, 1.5}, true)
                          .Rotate(9.0, 17.0, 13.0);
         Manifold b = Manifold::Cube({1.3, 1.7, 1.2}, true)
                          .Rotate(-11.0, 23.0, 29.0)
                          .Translate({0.25, -0.1, 0.15});
         return SurfacePartition({a, b});
       }();
       TASK_CHECK(p.Valid());
       TASK_CHECK(CheckBarycentricBacksubstitution(p));
       Manifold r1 = p.Extract(3);
       Manifold r2 = p.Extract(3);
       TASK_CHECK(CheckClosedMesh(r1));
       TASK_CHECK(CheckClosedMesh(r2));
       TASK_CHECK(r1.GetMeshGL64().NumTri() == r2.GetMeshGL64().NumTri());

       auto a = Manifold::Cube({1, 1, 1});
       auto b = a.Translate({0.5, 0, 0});
       auto halves = a.Split(b);
       Manifold sum = a + b;
       TASK_CHECK(halves.first.Status() == Manifold::Error::NoError);
       TASK_CHECK(halves.second.Status() == Manifold::Error::NoError);
       TASK_CHECK(sum.Status() == Manifold::Error::NoError);
       TASK_CHECK(std::abs(halves.first.Volume() - 0.5) < 1e-8);
       TASK_CHECK(std::abs(halves.second.Volume() - 0.5) < 1e-8);
       TASK_CHECK(std::abs(sum.Volume() - 1.5) < 1e-8);
       return 0;
     }},
};

extern const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
