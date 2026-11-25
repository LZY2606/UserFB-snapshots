#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include "task_support/check.h"
#include "manifold/surface_partition.h"

using namespace manifold;

namespace {

constexpr double kTestPi = 3.14159265358979323846;
using u64 = uint64_t;
using Edge = std::pair<u64, u64>;

Edge SortedEdge(u64 a, u64 b) {
  return {std::min(a, b), std::max(a, b)};
}

double Dot(vec3 a, vec3 b) { return la::dot(a, b); }
vec3 Cross(vec3 a, vec3 b) { return la::cross(a, b); }
double Len(vec3 a) { return la::length(a); }

bool TopologyOK(const SurfacePartition& p, uint32_t mask) {
  std::map<std::pair<u64, u64>, int> directed;
  std::set<u64> used;
  for (const SurfacePartition::Triangle& f : p.Triangles()) {
    if (f.minusMask != mask && f.plusMask != mask) continue;
    std::array<u64, 3> id = f.vertices;
    if (f.plusMask == mask) std::swap(id[1], id[2]);
    for (u64 v : id) used.insert(v);
    for (int k = 0; k < 3; ++k) {
      const u64 a = id[k];
      const u64 b = id[(k + 1) % 3];
      if (a == b) return false;
      ++directed[{a, b}];
    }
  }
  if (used.empty()) return true;

  for (const auto& [e, count] : directed) {
    if (count != 1) return false;
    auto it = directed.find({e.second, e.first});
    if (it == directed.end() || it->second != 1) return false;
  }

  std::map<u64, std::map<u64, std::set<u64>>> links;
  for (const SurfacePartition::Triangle& f : p.Triangles()) {
    if (f.minusMask != mask && f.plusMask != mask) continue;
    std::array<u64, 3> id = f.vertices;
    if (f.plusMask == mask) std::swap(id[1], id[2]);
    for (int k = 0; k < 3; ++k) {
      const u64 v = id[k];
      const u64 a = id[(k + 1) % 3];
      const u64 b = id[(k + 2) % 3];
      links[v][a].insert(b);
      links[v][b].insert(a);
    }
  }

  for (u64 v : used) {
    auto lit = links.find(v);
    if (lit == links.end() || lit->second.size() < 3) return false;
    std::set<u64> linkVerts;
    std::map<u64, std::set<u64>> g;
    for (const auto& [a, bs] : lit->second) {
      linkVerts.insert(a);
      for (u64 b : bs) {
        linkVerts.insert(b);
        g[a].insert(b);
        g[b].insert(a);
      }
    }
    for (u64 a : linkVerts) {
      auto git = g.find(a);
      if (git == g.end() || git->second.size() != 2) return false;
    }
    std::set<u64> seen;
    std::vector<u64> stack{*linkVerts.begin()};
    while (!stack.empty()) {
      u64 x = stack.back();
      stack.pop_back();
      if (!seen.insert(x).second) continue;
      for (u64 y : g[x])
        if (!seen.count(y)) stack.push_back(y);
    }
    if (seen.size() != linkVerts.size()) return false;
  }
  return true;
}

bool InterfaceInvariants(const SurfacePartition& p, size_t n) {
  const uint32_t valid = n == 0 ? 0u : ((1u << n) - 1u);
  for (const auto& f : p.Triangles()) {
    auto bad=[&](const char* why){std::fprintf(stderr,"INV %s op%u t%llu mm%u pm%u\n",why,f.sourceOperand,(unsigned long long)f.sourceTriangle,f.minusMask,f.plusMask);return false;};
    if (f.sourceOperand >= n) return bad("op");
    if ((f.minusMask ^ f.plusMask) != (1u << f.sourceOperand)) return bad("xor");
    if ((f.minusMask & (1u << f.sourceOperand)) == 0) return bad("minusbit");
    if ((f.plusMask & (1u << f.sourceOperand)) != 0) return bad("plusbit");
    if (f.minusMask > valid || f.plusMask > valid) return bad("range");
    if (f.sourceTriangle >= p.SourceMesh(f.sourceOperand).NumTri())
      return bad("tri");
    for (int c = 0; c < 3; ++c) {
      const vec3& b = f.sourceBarycentric[c];
      if (!std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z))
        return bad("finite");
      if (b.x < -1e-7 || b.y < -1e-7 || b.z < -1e-7) return bad("baryneg");
      const double sum = b.x + b.y + b.z;
      if (std::abs(sum - 1.0) > 1e-7) return bad("barysum");
    }
  }
  return true;
}

double MeshWinding(const vec3& q, const MeshGL64& m) {
  double angle = 0.0;
  for (u64 t = 0; t < m.NumTri(); ++t) {
    vec3 v[3];
    for (int k = 0; k < 3; ++k) {
      u64 id = m.triVerts[3 * t + k];
      size_t off = static_cast<size_t>(id) * m.numProp;
      v[k] = vec3(m.vertProperties[off], m.vertProperties[off + 1],
                  m.vertProperties[off + 2]);
    }
    vec3 a = v[0] - q, b = v[1] - q, c = v[2] - q;
    double al = Len(a), bl = Len(b), cl = Len(c);
    if (al < 1e-12 || bl < 1e-12 || cl < 1e-12) continue;
    double det = Dot(a, Cross(b, c));
    double denom = al * bl * cl + Dot(a, b) * cl + Dot(b, c) * al +
                   Dot(c, a) * bl;
    angle += 2.0 * std::atan2(det, denom);
  }
  return angle / (4.0 * kTestPi);
}

using CoordKey = std::array<double, 9>;
CoordKey SortedCoordKey(std::array<vec3, 3> q) {
  std::array<std::array<double, 3>, 3> p = {
      std::array<double, 3>{q[0].x, q[0].y, q[0].z},
      {q[1].x, q[1].y, q[1].z},
      {q[2].x, q[2].y, q[2].z}};
  std::sort(p.begin(), p.end());
  return {p[0][0], p[0][1], p[0][2], p[1][0], p[1][1], p[1][2],
          p[2][0], p[2][1], p[2][2]};
}

CoordKey MeshTriangleKey(const MeshGL64& m, u64 t) {
  std::array<vec3, 3> q;
  for (int k = 0; k < 3; ++k) {
    u64 id = m.triVerts[3 * t + k];
    size_t off = static_cast<size_t>(id) * m.numProp;
    q[k] = vec3(m.vertProperties[off], m.vertProperties[off + 1],
                m.vertProperties[off + 2]);
  }
  return SortedCoordKey(q);
}

double DirectedVolume(const MeshGL64& m) {  double vol = 0.0;
  for (u64 t = 0; t < m.NumTri(); ++t) {
    vec3 v[3];
    for (int k = 0; k < 3; ++k) {
      u64 id = m.triVerts[3 * t + k];
      size_t off = static_cast<size_t>(id) * m.numProp;
      v[k] = vec3(m.vertProperties[off], m.vertProperties[off + 1],
                  m.vertProperties[off + 2]);
    }
    vol += Dot(v[0], Cross(v[1], v[2])) / 6.0;
  }
  return vol;
}

SurfacePartition MakeOblique() {
  Manifold a = Manifold::Cube({5.0, 4.5, 4.0}, true)
                   .Rotate(5, 0, 0)
                   .Translate({-0.8, 0.0, 0.0});
  Manifold b = Manifold::Cube({4.5, 5.0, 4.2}, true)
                   .Translate({0.0, -0.8, 0.0});
  Manifold c = Manifold::Cube({4.1, 4.7, 5.0}, true)
                   .Rotate(0, 0, 5)
                   .Translate({0.0, 0.0, -0.8});
  return SurfacePartition({a, b, c});
}

SurfacePartition MakeAxisThree() {
  Manifold a = Manifold::Cube({6, 5, 5}, true).Translate({-1, 0, 0});
  Manifold b = Manifold::Cube({5, 6, 5.5}, true).Translate({0, -1, 0});
  Manifold c = Manifold::Cube({4.5, 5.5, 6}, true).Translate({0, 0, -1});
  return SurfacePartition({a, b, c});
}

MeshGL64 AppendCube(MeshGL64 out, vec3 size, vec3 center, bool reverse) {
  MeshGL64 g = Manifold::Cube(size, true).Translate(center).GetMeshGL64();
  const u64 offset = out.NumVert();
  out.numProp = 3;
  out.vertProperties.insert(out.vertProperties.end(),
                            g.vertProperties.begin(),
                            g.vertProperties.end());
  for (u64 t = 0; t < g.NumTri(); ++t) {
    u64 a = g.triVerts[3 * t], b = g.triVerts[3 * t + 1],
        c = g.triVerts[3 * t + 2];
    out.triVerts.push_back(offset + a);
    if (reverse) {
      out.triVerts.push_back(offset + c);
      out.triVerts.push_back(offset + b);
    } else {
      out.triVerts.push_back(offset + b);
      out.triVerts.push_back(offset + c);
    }
  }
  return out;
}

bool InBox(const vec3& p, vec3 lo, vec3 hi) {
  return p.x > lo.x && p.x < hi.x && p.y > lo.y && p.y < hi.y &&
         p.z > lo.z && p.z < hi.z;
}

uint32_t AxisMask(const vec3& p) {
  uint32_t m = 0;
  if (InBox(p, {-4, -2.5, -2.5}, {2, 2.5, 2.5})) m |= 1;
  if (InBox(p, {-2.5, -4, -2.75}, {2.5, 2, 2.75})) m |= 2;
  if (InBox(p, {-2.25, -2.75, -4}, {2.25, 2.75, 2})) m |= 4;
  return m;
}

}  // namespace

int junctions_triple_shared_index() {
  SurfacePartition p = MakeOblique();
  TASK_CHECK(p.ok());
  TASK_CHECK(InterfaceInvariants(p, 3));

  std::map<u64, std::set<uint32_t>> vertexOps;
  std::map<Edge, std::set<uint32_t>> edgeOps;
  for (const auto& f : p.Triangles()) {
    for (u64 v : f.vertices) vertexOps[v].insert(f.sourceOperand);
    for (int k = 0; k < 3; ++k) {
      Edge e = SortedEdge(f.vertices[k], f.vertices[(k + 1) % 3]);
      edgeOps[e].insert(f.sourceOperand);
    }
  }

  u64 tripleVertex = std::numeric_limits<u64>::max();
  int tripleCount = 0;
  for (const auto& [v, ops] : vertexOps) {
    if (ops.size() == 3) {
      ++tripleCount;
      if (tripleVertex == std::numeric_limits<u64>::max()) tripleVertex = v;
    }
  }
  TASK_CHECK(tripleCount > 0);

  for (uint32_t i = 0; i < 3; ++i) {
    for (uint32_t j = i + 1; j < 3; ++j) {
      bool commonLineAtVertex = false;
      for (const auto& [e, ops] : edgeOps) {
        if (e.first != tripleVertex && e.second != tripleVertex) continue;
        if (ops.count(i) && ops.count(j)) commonLineAtVertex = true;
      }
      TASK_CHECK(commonLineAtVertex);
    }
  }

  for (uint32_t m = 1; m < 8; ++m) TASK_CHECK(TopologyOK(p, m));
  return 0;
}

int junctions_axis_exact_point_and_no_t_junction() {
  SurfacePartition p = MakeAxisThree();
  TASK_CHECK(p.ok());
  u64 exact = std::numeric_limits<u64>::max();
  double best = 1e100;
  for (size_t i = 0; i < p.Vertices().size(); ++i) {
    vec3 d = p.Vertices()[i] - vec3(2.0, 2.0, 2.0);
    double dist = Len(d);
    if (dist < best) {
      best = dist;
      exact = i;
    }
  }
  TASK_CHECK(best < 1e-7);
  std::set<uint32_t> ops;
  for (const auto& f : p.Triangles())
    for (u64 v : f.vertices)
      if (v == exact) ops.insert(f.sourceOperand);
  TASK_CHECK(ops.size() == 3);
  for (uint32_t m = 1; m < 8; ++m) TASK_CHECK(TopologyOK(p, m));
  return 0;
}

int regions_axis_volumes_probes_and_fans() {
  Manifold a = Manifold::Cube({6, 5, 5}, true).Translate({-1, 0, 0});
  Manifold b = Manifold::Cube({5, 6, 5.5}, true).Translate({0, -1, 0});
  Manifold c = Manifold::Cube({4.5, 5.5, 6}, true).Translate({0, 0, -1});
  SurfacePartition p = MakeAxisThree();
  TASK_CHECK(p.ok());
  TASK_CHECK(InterfaceInvariants(p, 3));

  const double opVol[3] = {a.Volume(), b.Volume(), c.Volume()};
  for (uint32_t bit = 0; bit < 3; ++bit) {
    double sum = 0.0;
    for (uint32_t m = 1; m < 8; ++m) {
      if (m & (1u << bit)) {
        SurfacePartition::Error e;
        Manifold r = p.Extract(m, &e);
        TASK_CHECK(e == SurfacePartition::Error::NoError);
        TASK_CHECK(r.Status() == Manifold::Error::NoError);
        sum += r.Volume();
      }
    }
    TASK_CHECK(std::abs(sum - opVol[bit]) < 1e-7);
  }

  double regionsVolume = 0.0;
  for (uint32_t m = 1; m < 8; ++m) {
    SurfacePartition::Error e;
    Manifold r = p.Extract(m, &e);
    TASK_CHECK(e == SurfacePartition::Error::NoError);
    MeshGL64 g = r.GetMeshGL64();
    double dv = DirectedVolume(g);
    TASK_CHECK(dv > 0.0);
    TASK_CHECK(std::abs(dv - r.Volume()) < 1e-7);
    regionsVolume += r.Volume();
    TASK_CHECK(TopologyOK(p, m));
  }
  TASK_CHECK(std::abs(regionsVolume - (a + b + c).Volume()) < 1e-7);

  std::map<uint32_t, std::set<CoordKey>> regionTriangles;
  for (uint32_t m = 1; m < 8; ++m) {
    MeshGL64 g = p.Extract(m).GetMeshGL64();
    for (u64 t = 0; t < g.NumTri(); ++t)
      regionTriangles[m].insert(MeshTriangleKey(g, t));
  }
  for (const auto& f : p.Triangles()) {
    std::array<vec3, 3> q = {p.Vertices()[f.vertices[0]],
                             p.Vertices()[f.vertices[1]],
                             p.Vertices()[f.vertices[2]]};
    CoordKey key = SortedCoordKey(q);
    TASK_CHECK(regionTriangles[f.minusMask].count(key) == 1);
    if (f.plusMask != 0)
      TASK_CHECK(regionTriangles[f.plusMask].count(key) == 1);
  }

  for (const auto& f : p.Triangles()) {
    vec3 centroid = (p.Vertices()[f.vertices[0]] +
                    p.Vertices()[f.vertices[1]] +
                    p.Vertices()[f.vertices[2]]) /
                   3.0;
    vec3 n = Cross(p.Vertices()[f.vertices[1]] - p.Vertices()[f.vertices[0]],
                   p.Vertices()[f.vertices[2]] - p.Vertices()[f.vertices[0]]);
    n = la::normalize(n);
    TASK_CHECK(AxisMask(centroid - 1e-5 * n) == f.minusMask);
    TASK_CHECK(AxisMask(centroid + 1e-5 * n) == f.plusMask);
  }

  const vec3 probes[] = {{-3.5, 0, 0},   {0, -3.4, 0.2}, {0.1, 0.2, -3.5},
                         {1.8, 1.8, 0},  {1.8, 0, 1.8},  {0, 1.8, 1.8},
                         {1.8, 1.8, 1.8}, {-1.5, 1.8, 1.8}};
  for (vec3 q : probes) {
    uint32_t m = AxisMask(q);
    if (m == 0) continue;
    SurfacePartition::Error e;
    Manifold r = p.Extract(m, &e);
    TASK_CHECK(e == SurfacePartition::Error::NoError);
    MeshGL64 g = r.GetMeshGL64();
    TASK_CHECK(std::abs(MeshWinding(q, g)) > 0.5);
  }
  return 0;
}

int regions_nonconvex_cavity_and_four_operands() {
  MeshGL64 hollowMesh;
  hollowMesh.numProp = 3;
  hollowMesh = AppendCube(hollowMesh, {3, 3, 3}, {0, 0, 0}, false);
  hollowMesh = AppendCube(hollowMesh, {2.2, 2.2, 2.2}, {0, 0, 0}, true);
  hollowMesh = AppendCube(hollowMesh, {0.8, 0.8, 0.8}, {4, 0, 0}, false);
  Manifold hollow(hollowMesh);
  TASK_CHECK(hollow.Status() == Manifold::Error::NoError);

  SimplePolygon lpoly;
  for (vec2 q : {vec2(-2, -1), vec2(2, -1), vec2(2, 0), vec2(0, 0),
                 vec2(0, 1), vec2(-2, 1)})
    lpoly.push_back(q);
  Manifold lshape = Manifold::Extrude({lpoly}, 2).Translate({0, 0, -1});
  Manifold axisCube = Manifold::Cube({2.5, 2.5, 2.5}, true).Translate({.4, 0, 0});
  SurfacePartition lp({lshape, axisCube});
  TASK_CHECK(lp.ok());
  for (uint32_t m = 1; m < 4; ++m) {
    TASK_CHECK(TopologyOK(lp, m));
    SurfacePartition::Error e;
    Manifold r = lp.Extract(m, &e);
    TASK_CHECK(e == SurfacePartition::Error::NoError);
    TASK_CHECK(r.Status() == Manifold::Error::NoError);
  }

  Manifold bar = Manifold::Cube({4.5, 1.1, 1.1}, true)
                     .Rotate(0, 12, 0)
                     .Translate({0, 0, .2});
  SurfacePartition hp({hollow, bar});
  TASK_CHECK(hp.ok());
  TASK_CHECK(InterfaceInvariants(hp, 2));
  for (uint32_t m = 1; m < 4; ++m) {
    TASK_CHECK(TopologyOK(hp, m));
    SurfacePartition::Error e;
    Manifold r = hp.Extract(m, &e);
    TASK_CHECK(e == SurfacePartition::Error::NoError);
    TASK_CHECK(r.Status() == Manifold::Error::NoError);
  }
  TASK_CHECK(std::abs(hp.Extract(1).Volume() + hp.Extract(3).Volume() -
                      hollow.Volume()) < 1e-6);

  Manifold outer = Manifold::Cube({4, 4, 4}, true);
  Manifold contained = Manifold::Cube({0.9, 0.9, 0.9}, true);
  SurfacePartition containedPartition({outer, contained});
  TASK_CHECK(containedPartition.ok());
  TASK_CHECK(InterfaceInvariants(containedPartition, 2));
  for (uint32_t m = 1; m < 4; ++m) TASK_CHECK(TopologyOK(containedPartition, m));
  TASK_CHECK(std::abs(containedPartition.Extract(1).Volume() -
                      (64.0 - 0.729)) < 1e-8);
  TASK_CHECK(std::abs(containedPartition.Extract(3).Volume() - 0.729) < 1e-8);

  std::vector<Manifold> four = {
      Manifold::Cube({6.0, 5.0, 5.2}, true).Translate({-1, 0, 0}),
      Manifold::Cube({5.1, 6.2, 4.7}, true).Translate({0, -1, 0}),
      Manifold::Cube({4.6, 5.3, 6.1}, true).Translate({0, 0, -1}),
      Manifold::Cube({1.0, 1.0, 1.0}, true).Translate({5, 0, 0})};
  SurfacePartition fp(four);
  TASK_CHECK(fp.ok());
  TASK_CHECK(InterfaceInvariants(fp, 4));
  for (uint32_t m = 1; m < 16; ++m) {
    TASK_CHECK(TopologyOK(fp, m));
    SurfacePartition::Error e;
    Manifold r = fp.Extract(m, &e);
    TASK_CHECK(e == SurfacePartition::Error::NoError);
    TASK_CHECK(r.Status() == Manifold::Error::NoError);
  }
  TASK_CHECK(fp.Extract(8).NumTri() == 12);
  return 0;
}

int properties_bary_native_ids_and_destroyed_sources() {
  const uint32_t idA = Manifold::ReserveIDs(1);
  const uint32_t idB = Manifold::ReserveIDs(1);
  SurfacePartition p;
  {
    auto makeCube = [](uint32_t oid, vec3 size, vec3 center) {
      MeshGL64 base = Manifold::Cube(size, true).GetMeshGL64();
      MeshGL64 m;
      m.numProp = 5;
      m.runOriginalID = {oid};
      m.runIndex = {0, base.NumTri() * 3};
      m.runTransform = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
      for (u64 t = 0; t < base.NumTri(); ++t) {
        m.faceID.push_back(7000 + t);
        for (int k = 0; k < 3; ++k) {
          u64 oldId = base.triVerts[3 * t + k];
          size_t poff = static_cast<size_t>(oldId) * base.numProp;
          vec3 q(base.vertProperties[poff] + center.x,
                 base.vertProperties[poff + 1] + center.y,
                 base.vertProperties[poff + 2] + center.z);
          u64 pv = m.NumVert();
          m.vertProperties.insert(m.vertProperties.end(),
                                  {q.x, q.y, q.z,
                                   0.37 * double(t) + 1.7 * k + 0.11 * q.x,
                                   100.0 * double(t + 1) + 10.0 * k + q.y});
          m.triVerts.push_back(pv);
        }
      }
      std::map<u64, u64> canonical;
      for (u64 t = 0; t < m.NumTri(); ++t) {
        for (int k = 0; k < 3; ++k) {
          u64 pv = m.triVerts[3 * t + k];
          // Recover the shared cube corner by matching its position.
          vec3 q = m.GetVertPos(pv);
          u64 key = static_cast<u64>(
              (std::llround((q.x + 100.0) * 1e5) * 73856093LL) ^
              (std::llround((q.y + 100.0) * 1e5) * 19349663LL) ^
              (std::llround((q.z + 100.0) * 1e5) * 83492791LL));
          auto it = canonical.find(key);
          if (it == canonical.end()) canonical[key] = pv;
          else {
            m.mergeFromVert.push_back(pv);
            m.mergeToVert.push_back(it->second);
          }
        }
      }
      return Manifold(m);
    };

    Manifold a = makeCube(idA, {2.6, 2.4, 2.2}, {-0.4, 0, 0});
    Manifold b = makeCube(idB, {2.2, 2.6, 2.4}, {0.5, 0.3, 0.2});
    TASK_CHECK(a.Status() == Manifold::Error::NoError);
    TASK_CHECK(b.Status() == Manifold::Error::NoError);
    SurfacePartition::Error buildError = p.Build({a, b});
    if (buildError != SurfacePartition::Error::NoError)
      std::fprintf(stderr, "PROPBUILD %d %s\n", (int)buildError, p.errorMessage().c_str());
    TASK_CHECK(buildError == SurfacePartition::Error::NoError);
  }

  TASK_CHECK(p.ok());
  TASK_CHECK(p.ExtraPropertyCount() == 2);
  TASK_CHECK(p.SourceMesh(0).runOriginalID.size() == 1);
  TASK_CHECK(p.SourceMesh(0).runOriginalID[0] == idA);
  TASK_CHECK(p.SourceMesh(1).runOriginalID[0] == idB);

  for (const auto& f : p.Triangles()) {
    const MeshGL64& sm = p.SourceMesh(f.sourceOperand);
    for (int c = 0; c < 3; ++c) {
      vec3 reconstructed(0, 0, 0);
      for (int k = 0; k < 3; ++k) {
        u64 pv = sm.triVerts[3 * f.sourceTriangle + k];
        size_t off = static_cast<size_t>(pv) * sm.numProp;
        vec3 q(sm.vertProperties[off], sm.vertProperties[off + 1],
               sm.vertProperties[off + 2]);
        reconstructed += q * f.sourceBarycentric[c][k];
      }
      vec3 d = reconstructed - p.Vertices()[f.vertices[c]];
      TASK_CHECK(Len(d) < 1e-8);
    }
  }

  Manifold r1 = p.ExtractChecked(3);
  Manifold r2 = p.ExtractChecked(3);
  TASK_CHECK(r1.Status() == Manifold::Error::NoError);
  TASK_CHECK(r2.Status() == Manifold::Error::NoError);
  TASK_CHECK(std::abs(r1.Volume() - r2.Volume()) < 1e-10);
  MeshGL64 out = r1.GetMeshGL64();
  TASK_CHECK(out.numProp == 5);
  bool foundA = false, foundB = false, foundFace = false;
  for (uint32_t id : out.runOriginalID) {
    if (id == idA) foundA = true;
    if (id == idB) foundB = true;
  }
  for (u64 f : out.faceID)
    if (f >= 7000 && f < 8000) foundFace = true;
  TASK_CHECK(foundA);
  TASK_CHECK(foundB);
  TASK_CHECK(foundFace);

  int checkedProperties = 0;
  for (u64 ot = 0; ot < out.NumTri() && checkedProperties < 80; ++ot) {
    if (out.faceID.empty()) break;
    u64 sourceTri = out.faceID[ot] - 7000;
    if (sourceTri >= p.SourceMesh(0).NumTri() &&
        sourceTri >= p.SourceMesh(1).NumTri())
      continue;
    for (int j = 0; j < 3 && checkedProperties < 80; ++j) {
      u64 opv = out.triVerts[3 * ot + j];
      size_t ooff = static_cast<size_t>(opv) * out.numProp;
      vec3 op(out.vertProperties[ooff], out.vertProperties[ooff + 1],
              out.vertProperties[ooff + 2]);
      for (const auto& f : p.Triangles()) {
        if (f.sourceTriangle != sourceTri) continue;
        const MeshGL64& sm = p.SourceMesh(f.sourceOperand);
        for (int c = 0; c < 3; ++c) {
          if (Len(p.Vertices()[f.vertices[c]] - op) > 1e-7) continue;
          for (int ch = 0; ch < 2; ++ch) {
            double expected = 0.0;
            for (int k = 0; k < 3; ++k) {
              u64 spv = sm.triVerts[3 * sourceTri + k];
              size_t soff = static_cast<size_t>(spv) * sm.numProp;
              expected += f.sourceBarycentric[c][k] *
                          sm.vertProperties[soff + 3 + ch];
            }
            TASK_CHECK(std::abs(out.vertProperties[ooff + 3 + ch] -
                                expected) < 1e-8);
          }
          ++checkedProperties;
        }
      }
    }
  }
  TASK_CHECK(checkedProperties > 0);
  return 0;
}

int reuse_errors_empty_and_legacy_boolean() {
  SurfacePartition good = MakeAxisThree();
  TASK_CHECK(good.ok());
  std::vector<Manifold> five(5, Manifold::Cube({1, 1, 1}));
  SurfacePartition tooMany(five);
  TASK_CHECK(tooMany.error() == SurfacePartition::Error::TooManyOperands);
  TASK_CHECK(!tooMany.ok());

  SurfacePartition reuse = tooMany;
  TASK_CHECK(reuse.error() == SurfacePartition::Error::TooManyOperands);
  TASK_CHECK(reuse.Build({Manifold::Cube({1, 1, 1}, true)}) ==
             SurfacePartition::Error::NoError);
  TASK_CHECK(reuse.ok());

  SurfacePartition::Error rebuildError = good.Build(five);
  TASK_CHECK(rebuildError == SurfacePartition::Error::TooManyOperands);
  TASK_CHECK(good.ok());
  TASK_CHECK(good.Extract(7).Status() == Manifold::Error::NoError);

  SurfacePartition emptyList;
  TASK_CHECK(emptyList.ok());
  TASK_CHECK(emptyList.Vertices().empty());
  SurfacePartition::Error zeroError;
  Manifold zero = emptyList.Extract(0, &zeroError);
  TASK_CHECK(zeroError == SurfacePartition::Error::EmptyMaskIsUnbounded);
  TASK_CHECK(zero.Status() == Manifold::Error::NoError);
  bool threw = false;
  try {
    (void)good.Extract(0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);
  threw = false;
  try {
    (void)good.ExtractChecked(0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  TASK_CHECK(threw);

  Manifold emptyOperand;
  SurfacePartition withEmpty({emptyOperand, Manifold::Cube({2, 2, 2}, true)});
  TASK_CHECK(withEmpty.ok());
  TASK_CHECK(withEmpty.Extract(1).IsEmpty());
  Manifold cubeRegion = withEmpty.Extract(2);
  TASK_CHECK(std::abs(cubeRegion.Volume() - 8.0) < 1e-8);
  MeshGL64 cubeOut = cubeRegion.GetMeshGL64();
  TASK_CHECK(cubeOut.faceID.size() == cubeOut.NumTri());

  MeshGL64 bad;
  bad.numProp = 3;
  bad.vertProperties = {0, 0, 0, 1, 0, 0, 0, 1, std::numeric_limits<double>::infinity()};
  bad.triVerts = {0, 1, 2};
  Manifold nonFinite(bad);
  SurfacePartition nf({nonFinite});
  TASK_CHECK(!nf.ok());

  Manifold a = Manifold::Cube({1, 1, 1}).SetProperties(
      2, [](double* out, vec3 p, const double*) {
        out[0] = p.x + 2.0 * p.y;
        out[1] = 1.0 - p.z;
      });
  Manifold b = a.Translate({0.5, 0, 0});
  auto halves = a.Split(b);
  Manifold uni = a + b;
  TASK_CHECK(halves.first.Status() == Manifold::Error::NoError);
  TASK_CHECK(halves.second.Status() == Manifold::Error::NoError);
  TASK_CHECK(uni.Status() == Manifold::Error::NoError);
  TASK_CHECK(std::abs(halves.first.Volume() - 0.5) < 1e-8);
  TASK_CHECK(std::abs(halves.second.Volume() - 0.5) < 1e-8);
  TASK_CHECK(std::abs(uni.Volume() - 1.5) < 1e-8);
  MeshGL64 m = halves.first.GetMeshGL64();
  TASK_CHECK(m.numProp == 5 && m.NumTri() > 0 && !m.runOriginalID.empty());
  return 0;
}

const task_case task_cases[] = {
    {"junctions", "triple_shared_index", junctions_triple_shared_index},
    {"junctions", "axis_exact_point",
     junctions_axis_exact_point_and_no_t_junction},
    {"regions", "axis_volumes_probes_fans",
     regions_axis_volumes_probes_and_fans},
    {"regions", "nonconvex_cavity_four",
     regions_nonconvex_cavity_and_four_operands},
    {"properties", "bary_native_ids_destroyed_sources",
     properties_bary_native_ids_and_destroyed_sources},
    {"reuse", "errors_empty_legacy_boolean",
     reuse_errors_empty_and_legacy_boolean},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
