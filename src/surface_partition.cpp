#include "manifold/surface_partition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "manifold/polygon.h"

namespace manifold {

namespace {

constexpr double kBaryEps = 1e-8;

using Gid = uint64_t;

struct EdgeEventKey {
  int edgeOp;
  Gid a, b;
  int faceOp;
  uint64_t tri;

  bool operator<(const EdgeEventKey& o) const {
    return std::tie(edgeOp, a, b, faceOp, tri) <
           std::tie(o.edgeOp, o.a, o.b, o.faceOp, o.tri);
  }
};

struct TripleKey {
  std::array<int, 3> op{{-1, -1, -1}};
  std::array<uint64_t, 3> tri{{0, 0, 0}};

  bool operator<(const TripleKey& o) const {
    return std::tie(op, tri) < std::tie(o.op, o.tri);
  }
};

struct TriPairKey {
  int opA, opB;
  uint64_t triA, triB;

  bool operator<(const TriPairKey& o) const {
    return std::tie(opA, opB, triA, triB) <
           std::tie(o.opA, o.opB, o.triA, o.triB);
  }
};

struct PairSeg {
  int opA, opB;
  uint64_t triA, triB;
  Gid va, vb;
  std::vector<Gid> cuts;
};

struct ProtoFace {
  std::array<Gid, 3> g{};
  uint32_t op = 0;
  uint64_t tri = 0;
  std::array<vec3, 3> bary{};
};

struct OpData {
  Manifold handle;
  MeshGL64 mesh;
  bool empty = true;
  std::vector<std::array<vec3, 3>> triPos;
  std::vector<std::array<uint64_t, 3>> triProp;
  std::vector<int> prop2geom;
  std::vector<Gid> geomGlobal;
  std::vector<int> runOfTri;
  std::vector<std::vector<int>> faceSegs;
};

double Dot2(const vec2& a, const vec2& b) { return a.x * b.x + a.y * b.y; }
double Cross2(const vec2& a, const vec2& b) { return a.x * b.y - a.y * b.x; }
vec2 Sub2(const vec2& a, const vec2& b) { return {a.x - b.x, a.y - b.y}; }

bool Finite3(const vec3& p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

double Norm3(const vec3& p) { return std::sqrt(dot(p, p)); }

vec3 NormalizedOr(const vec3& p, bool& ok) {
  const double n = Norm3(p);
  if (!std::isfinite(n) || n < 1e-300) {
    ok = false;
    return {0.0, 0.0, 1.0};
  }
  return p / n;
}

// Barycentric weights in the order (a,b,c).  Returns false for a
// geometrically degenerate source triangle.
bool Barycentric(const vec3& p, const std::array<vec3, 3>& t, vec3& w) {
  const vec3 v0 = t[1] - t[0];
  const vec3 v1 = t[2] - t[0];
  const vec3 v2 = p - t[0];
  const double d00 = dot(v0, v0);
  const double d01 = dot(v0, v1);
  const double d11 = dot(v1, v1);
  const double d20 = dot(v2, v0);
  const double d21 = dot(v2, v1);
  const double den = d00 * d11 - d01 * d01;
  if (!std::isfinite(den) || std::abs(den) < 1e-300) return false;
  const double v = (d11 * d20 - d01 * d21) / den;
  const double z = (d00 * d21 - d01 * d20) / den;
  const double y = 1.0 - v - z;
  w = {y, v, z};
  return std::isfinite(w.x) && std::isfinite(w.y) && std::isfinite(w.z);
}

bool InsideBary(const vec3& w, double eps = 2e-8) {
  return w.x >= -eps && w.y >= -eps && w.z >= -eps;
}

struct EdgeHit {
  Gid gid;
  vec3 p;
};

}  // namespace

struct SurfacePartition::Impl {
  Status status = Status::Ok;
  uint32_t opCount = 0;
  uint32_t extraProps = 0;

  std::vector<vec3> vertices;
  std::vector<Face> faces;
  std::vector<MeshGL64> sources;

  std::vector<OpData> ops;
  std::vector<PairSeg> segs;
  std::vector<ProtoFace> proto;

  std::map<EdgeEventKey, Gid> edgeEvents;
  std::map<TripleKey, Gid> tripleEvents;
  std::map<TriPairKey, int> pairMap;

  Gid AddVertex(const vec3& p) {
    vertices.push_back(p);
    return static_cast<Gid>(vertices.size() - 1);
  }

  Status Build(const std::vector<Manifold>& input) {
    if (input.size() > 4) return Status::TooManyOperands;
    opCount = static_cast<uint32_t>(input.size());
    ops.resize(input.size());
    sources.resize(input.size());

    int commonExtra = -1;
    for (size_t oi = 0; oi < input.size(); ++oi) {
      const Manifold::Error st = input[oi].Status();
      if (st != Manifold::Error::NoError) return Status::OperandError;

      OpData& op = ops[oi];
      op.handle = input[oi];
      op.mesh = input[oi].GetMeshGL64();
      sources[oi] = op.mesh;

      if (op.mesh.numProp < 3) return Status::InvalidPropertyCount;
      const int extra = static_cast<int>(op.mesh.numProp) - 3;
      if (commonExtra < 0) commonExtra = extra;
      if (extra != commonExtra) return Status::InvalidPropertyCount;
      if (extra < 0) return Status::InvalidPropertyCount;
      extraProps = static_cast<uint32_t>(extra);

      for (double x : op.mesh.vertProperties) {
        if (!std::isfinite(x)) return Status::NonFiniteInput;
      }
      for (double x : op.mesh.runTransform) {
        if (!std::isfinite(x)) return Status::NonFiniteInput;
      }

      const size_t ntri = op.mesh.NumTri();
      op.empty = ntri == 0 || input[oi].IsEmpty();
      op.triPos.resize(ntri);
      op.triProp.resize(ntri);
      op.faceSegs.assign(ntri, {});
      op.runOfTri.assign(ntri, -1);

      // Map every property vertex to a geometric vertex using the explicit
      // MeshGL merge relation.  Do not use positional tolerance here.
      const size_t npv = op.mesh.NumVert();
      std::vector<int> parent(npv);
      std::iota(parent.begin(), parent.end(), 0);
      std::function<int(int)> find = [&](int x) {
        if (parent[x] == x) return x;
        return parent[x] = find(parent[x]);
      };
      auto unite = [&](uint64_t a, uint64_t b) {
        if (a >= npv || b >= npv) return;
        a = static_cast<uint64_t>(find(static_cast<int>(a)));
        b = static_cast<uint64_t>(find(static_cast<int>(b)));
        if (a != b) parent[static_cast<size_t>(b)] = static_cast<int>(a);
      };
      for (size_t k = 0; k < op.mesh.mergeFromVert.size(); ++k) {
        unite(op.mesh.mergeFromVert[k], op.mesh.mergeToVert[k]);
      }

      std::unordered_map<int, int> root2geom;
      op.prop2geom.resize(npv);
      for (size_t pv = 0; pv < npv; ++pv) {
        const int root = find(static_cast<int>(pv));
        auto it = root2geom.find(root);
        int g;
        if (it == root2geom.end()) {
          g = static_cast<int>(root2geom.size());
          root2geom.emplace(root, g);
        } else {
          g = it->second;
        }
        op.prop2geom[pv] = g;
      }
      op.geomGlobal.resize(root2geom.size());
      for (auto& gid : op.geomGlobal) {
        const int g = static_cast<int>(&gid - op.geomGlobal.data());
        int root = -1;
        for (const auto& kv : root2geom) {
          if (kv.second == g) {
            root = kv.first;
            break;
          }
        }
        const vec3 p = op.mesh.GetVertPos(static_cast<size_t>(root));
        if (!Finite3(p)) return Status::NonFiniteInput;
        if (std::abs(p.x) > 100.0 || std::abs(p.y) > 100.0 ||
            std::abs(p.z) > 100.0) {
          return Status::UnsupportedGeometry;
        }
        gid = AddVertex(p);
      }

      // Run lookup for provenance.
      for (size_t r = 0; r < op.mesh.runOriginalID.size(); ++r) {
        const size_t begin = op.mesh.runIndex[r] / 3;
        const size_t end = op.mesh.runIndex[r + 1] / 3;
        for (size_t t = begin; t < end && t < ntri; ++t)
          op.runOfTri[t] = static_cast<int>(r);
      }
      if (!op.mesh.runIndex.empty() && op.mesh.runIndex.size() >= 2) {
        for (size_t r = 0; r + 1 < op.mesh.runIndex.size(); ++r) {
          const size_t begin = op.mesh.runIndex[r] / 3;
          const size_t end = op.mesh.runIndex[r + 1] / 3;
          for (size_t t = begin; t < end && t < ntri; ++t)
            op.runOfTri[t] = static_cast<int>(
                std::min(r, op.mesh.runOriginalID.size() - 1));
        }
      }

      for (size_t t = 0; t < ntri; ++t) {
        for (int k = 0; k < 3; ++k) {
          const uint64_t pv = op.mesh.triVerts[3 * t + k];
          if (pv >= npv) return Status::OperandError;
          op.triProp[t][k] = pv;
          op.triPos[t][k] = op.mesh.GetVertPos(pv);
          if (!Finite3(op.triPos[t][k])) return Status::NonFiniteInput;
        }
        const double e0 = Norm3(op.triPos[t][1] - op.triPos[t][0]);
        const double e1 = Norm3(op.triPos[t][2] - op.triPos[t][1]);
        const double e2 = Norm3(op.triPos[t][0] - op.triPos[t][2]);
        const double longest = std::max({e0, e1, e2});
        const double shortest = std::min({e0, e1, e2});
        const vec3 cr = cross(op.triPos[t][1] - op.triPos[t][0],
                              op.triPos[t][2] - op.triPos[t][0]);
        const double area2 = Norm3(cr);
        if (shortest < 1e-3) return Status::UnsupportedGeometry;
        if (area2 <= 0.0 || longest * longest / area2 > 1000.0) {
          // longest / altitude = longest / (2Area/longest) = longest^2/(2A).
          // cr norm is 2A, so longest^2/|cross| is exactly the ratio.
          return Status::UnsupportedGeometry;
        }
      }
    }

    if (input.empty()) return Status::Ok;

    Status st = BuildPairIntersections();
    if (st != Status::Ok) return st;
    st = BuildTripleEvents();
    if (st != Status::Ok) return st;
    st = BuildArrangement();
    if (st != Status::Ok) return st;
    st = ClassifyFaces();
    if (st != Status::Ok) return st;

    // Kernel handles are no longer needed: all geometry, properties, masks and
    // provenance are owned by this Impl.
    for (OpData& op : ops) op.handle = Manifold();
    return Status::Ok;
  }

  Gid EdgeEvent(int edgeOp, Gid ga, Gid gb, int faceOp, uint64_t faceTri,
                const vec3& p) {
    if (ga > gb) std::swap(ga, gb);
    EdgeEventKey key{edgeOp, ga, gb, faceOp, faceTri};
    auto it = edgeEvents.find(key);
    if (it != edgeEvents.end()) return it->second;
    const Gid gid = AddVertex(p);
    edgeEvents.emplace(key, gid);
    return gid;
  }

  std::optional<EdgeHit> EdgeHitsTriangle(int edgeOp, uint64_t edgeGa,
                                          uint64_t edgeGbG, const vec3& e0,
                                          const vec3& e1, int faceOp,
                                          uint64_t tri) {
    const auto& tt = ops[faceOp].triPos[tri];
    bool ok = true;
    const vec3 n = NormalizedOr(cross(tt[1] - tt[0], tt[2] - tt[0]), ok);
    if (!ok) return std::nullopt;
    const double d = dot(n, tt[0]);
    const double s0 = dot(n, e0) - d;
    const double s1 = dot(n, e1) - d;
    // Generic position guarantees no source vertex lies on the other plane.
    if (s0 * s1 >= 0.0) return std::nullopt;
    const double t = s0 / (s0 - s1);
    if (!std::isfinite(t) || t < 0.0 || t > 1.0) return std::nullopt;
    const vec3 p = e0 + t * (e1 - e0);
    vec3 w;
    if (!Barycentric(p, tt, w) || !InsideBary(w)) return std::nullopt;
    const Gid gid = EdgeEvent(edgeOp, edgeGa, edgeGbG, faceOp, tri, p);
    return EdgeHit{gid, p};
  }

  Status BuildPairIntersections() {
    for (int a = 0; a < static_cast<int>(opCount); ++a) {
      for (int b = a + 1; b < static_cast<int>(opCount); ++b) {
        if (ops[a].empty || ops[b].empty) continue;
        for (uint64_t ta = 0; ta < ops[a].triPos.size(); ++ta) {
          for (uint64_t tb = 0; tb < ops[b].triPos.size(); ++tb) {
            std::vector<EdgeHit> hits;
            const auto& pa = ops[a].triPos[ta];
            const auto& pb = ops[b].triPos[tb];
            for (int e = 0; e < 3; ++e) {
              const int k = (e + 1) % 3;
              const Gid ga = static_cast<Gid>(
                  ops[a].prop2geom[ops[a].triProp[ta][e]]);
              const Gid gb = static_cast<Gid>(
                  ops[a].prop2geom[ops[a].triProp[ta][k]]);
              auto h = EdgeHitsTriangle(a, ga, gb, pa[e], pa[k], b, tb);
              if (h) hits.push_back(*h);
            }
            for (int e = 0; e < 3; ++e) {
              const int k = (e + 1) % 3;
              const Gid ga = static_cast<Gid>(
                  ops[b].prop2geom[ops[b].triProp[tb][e]]);
              const Gid gb = static_cast<Gid>(
                  ops[b].prop2geom[ops[b].triProp[tb][k]]);
              auto h = EdgeHitsTriangle(b, ga, gb, pb[e], pb[k], a, ta);
              if (h) hits.push_back(*h);
            }

            std::vector<Gid> uniq;
            for (const auto& h : hits) {
              if (std::find(uniq.begin(), uniq.end(), h.gid) == uniq.end())
                uniq.push_back(h.gid);
            }
            if (uniq.size() < 2) continue;

            // Generic triangle/triangle crossings have exactly two edge/face
            // endpoints.  If numerical duplication produced more, retain the
            // farthest pair.
            Gid u = uniq[0], v = uniq[1];
            if (uniq.size() > 2) {
              double best = -1.0;
              for (size_t i = 0; i < uniq.size(); ++i) {
                for (size_t j = i + 1; j < uniq.size(); ++j) {
                  const double d =
                      Norm3(vertices[uniq[i]] - vertices[uniq[j]]);
                  if (d > best) {
                    best = d;
                    u = uniq[i];
                    v = uniq[j];
                  }
                }
              }
            }
            if (Norm3(vertices[u] - vertices[v]) < 1e-12) continue;

            PairSeg seg{a, b, ta, tb, u, v, {}};
            const int id = static_cast<int>(segs.size());
            segs.push_back(std::move(seg));
            ops[a].faceSegs[ta].push_back(id);
            ops[b].faceSegs[tb].push_back(id);
            TriPairKey key{a, b, ta, tb};
            pairMap.emplace(key, id);
          }
        }
      }
    }
    return Status::Ok;
  }

  std::optional<Gid> GetTripleEvent(
      uint64_t t0, uint64_t t1, uint64_t t2, int o0, int o1, int o2,
      const std::array<const std::array<vec3, 3>*, 3>& tt) {
    std::array<std::pair<int, uint64_t>, 3> f = {
        std::make_pair(o0, t0), std::make_pair(o1, t1),
        std::make_pair(o2, t2)};
    std::sort(f.begin(), f.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    TripleKey key;
    for (int i = 0; i < 3; ++i) {
      key.op[i] = f[i].first;
      key.tri[i] = f[i].second;
    }
    auto it = tripleEvents.find(key);
    if (it != tripleEvents.end()) return it->second;

    vec3 n[3];
    double d[3];
    for (int i = 0; i < 3; ++i) {
      const auto& q = *tt[i];
      bool ok = true;
      n[i] = NormalizedOr(cross(q[1] - q[0], q[2] - q[0]), ok);
      if (!ok) return std::nullopt;
      d[i] = dot(n[i], q[0]);
    }
    const double det = dot(n[0], cross(n[1], n[2]));
    if (!std::isfinite(det) || std::abs(det) < 1e-4)
      return std::nullopt;  // supported threshold

    auto det3 = [](const vec3& r0, const vec3& r1, const vec3& r2) {
      return dot(r0, cross(r1, r2));
    };
    const double x = det3({d[0], n[0].y, n[0].z},
                          {d[1], n[1].y, n[1].z},
                          {d[2], n[2].y, n[2].z}) /
                     det;
    const double y = det3({n[0].x, d[0], n[0].z},
                          {n[1].x, d[1], n[1].z},
                          {n[2].x, d[2], n[2].z}) /
                     det;
    const double z = det3({n[0].x, n[0].y, d[0]},
                          {n[1].x, n[1].y, d[1]},
                          {n[2].x, n[2].y, d[2]}) /
                     det;
    const vec3 p{x, y, z};
    if (!Finite3(p)) return std::nullopt;

    // The sorted key changes operand order; tt is in caller order.  Check all
    // caller faces before accepting.
    for (int i = 0; i < 3; ++i) {
      vec3 w;
      if (!Barycentric(p, *tt[i], w) || !InsideBary(w, 5e-7))
        return std::nullopt;
    }
    const Gid gid = AddVertex(p);
    tripleEvents.emplace(key, gid);
    return gid;
  }

  void AddCut(int segId, Gid gid) {
    auto& cuts = segs[segId].cuts;
    if (std::find(cuts.begin(), cuts.end(), gid) == cuts.end())
      cuts.push_back(gid);
  }

  Status BuildTripleEvents() {
    for (int s = 0; s < static_cast<int>(opCount); ++s) {
      for (uint64_t ts = 0; ts < ops[s].triPos.size(); ++ts) {
        const auto& segIds = ops[s].faceSegs[ts];
        const auto& st = ops[s].triPos[ts];
        for (size_t ii = 0; ii < segIds.size(); ++ii) {
          for (size_t jj = ii + 1; jj < segIds.size(); ++jj) {
            const PairSeg& s1 = segs[segIds[ii]];
            const PairSeg& s2 = segs[segIds[jj]];
            int o1, f1, o2, f2;
            if (s1.opA == s && s1.triA == ts) {
              o1 = s1.opB;
              f1 = static_cast<int>(s1.triB);
            } else {
              o1 = s1.opA;
              f1 = static_cast<int>(s1.triA);
            }
            if (s2.opA == s && s2.triA == ts) {
              o2 = s2.opB;
              f2 = static_cast<int>(s2.triB);
            } else {
              o2 = s2.opA;
              f2 = static_cast<int>(s2.triA);
            }
            if (o1 == o2) continue;

            vec3 wa0, wa1, wb0, wb1;
            if (!Barycentric(vertices[s1.va], st, wa0) ||
                !Barycentric(vertices[s1.vb], st, wa1) ||
                !Barycentric(vertices[s2.va], st, wb0) ||
                !Barycentric(vertices[s2.vb], st, wb1))
              continue;
            const vec2 p0{wa0.y, wa0.z};
            const vec2 p1{wa1.y, wa1.z};
            const vec2 q0{wb0.y, wb0.z};
            const vec2 q1{wb1.y, wb1.z};
            const vec2 r = Sub2(p1, p0);
            const vec2 qs = Sub2(q1, q0);
            const double den = Cross2(r, qs);
            if (std::abs(den) < 1e-14) continue;
            const vec2 qmp = Sub2(q0, p0);
            const double t = Cross2(qmp, qs) / den;
            const double u = Cross2(qmp, r) / den;
            if (!std::isfinite(t) || !std::isfinite(u)) continue;
            if (t <= 1e-10 || t >= 1.0 - 1e-10 || u <= 1e-10 ||
                u >= 1.0 - 1e-10)
              continue;

            std::array<const std::array<vec3, 3>*, 3> tt = {
                &ops[s].triPos[ts], &ops[o1].triPos[f1],
                &ops[o2].triPos[f2]};
            auto triple = GetTripleEvent(ts, f1, f2, s, o1, o2, tt);
            if (!triple) {
              // A detected proper crossing that cannot be represented is an
              // unsupported degeneracy, not a reason to silently omit it.
              return Status::UnsupportedGeometry;
            }
            const Gid g = *triple;
            AddCut(segIds[ii], g);
            AddCut(segIds[jj], g);

            const int lo = std::min(o1, o2);
            const int hi = std::max(o1, o2);
            const uint64_t tlo = lo == o1 ? f1 : f2;
            const uint64_t thi = hi == o1 ? f1 : f2;
            auto pit = pairMap.find(TriPairKey{lo, hi, tlo, thi});
            if (pit == pairMap.end()) return Status::InternalError;
            AddCut(pit->second, g);
          }
        }
      }
    }
    return Status::Ok;
  }

  struct LNode {
    Gid gid;
    vec2 p;
    vec3 bary;
  };

  struct Cycle {
    std::vector<int> h;
    std::vector<int> nodes;
    double area = 0.0;
    bool external = false;
    vec2 leftPoint{};
  };

  bool PointInCycle(const vec2& q, const Cycle& c,
                    const std::vector<LNode>& nodes) const {
    bool in = false;
    for (size_t i = 0, j = c.nodes.size() - 1; i < c.nodes.size();
         j = i++) {
      const vec2& a = nodes[c.nodes[j]].p;
      const vec2& b = nodes[c.nodes[i]].p;
      if (((b.y > q.y) != (a.y > q.y)) &&
          (q.x < (a.x - b.x) * (q.y - b.y) / (a.y - b.y) + b.x))
        in = !in;
    }
    return in;
  }

  Status TriangulateCell(const std::vector<Cycle>& cycles,
                         const std::vector<LNode>& nodes,
                         const Cycle& outer,
                         const std::vector<int>& holes, uint32_t op,
                         uint64_t tri) {
    PolygonsIdx polys;
    auto addContour = [&](const Cycle& c) {
      SimplePolygonIdx poly;
      poly.reserve(c.nodes.size());
      for (int li : c.nodes) {
        poly.push_back(PolyVert{nodes[li].p, li});
      }
      polys.push_back(std::move(poly));
    };
    addContour(outer);
    for (int hi : holes) addContour(cycles[hi]);

        std::vector<ivec3> tris;
        try {
          tris = TriangulateIdx(polys, -1.0, false);
        } catch (...) {
          return Status::UnsupportedGeometry;
        }
        if (tris.empty()) return Status::InternalError;

    const auto& st = ops[op].triPos[tri];
    const vec3 sourceN = cross(st[1] - st[0], st[2] - st[0]);
    for (ivec3 it : tris) {
      int ids[3] = {it[0], it[1], it[2]};
      for (int k = 0; k < 3; ++k) {
        if (ids[k] < 0 || static_cast<size_t>(ids[k]) >= nodes.size())
          return Status::InternalError;
      }
      vec3 n = cross(vertices[nodes[ids[1]].gid] -
                         vertices[nodes[ids[0]].gid],
                     vertices[nodes[ids[2]].gid] -
                         vertices[nodes[ids[0]].gid]);
      if (dot(n, sourceN) < 0.0) std::swap(ids[1], ids[2]);

      ProtoFace pf;
      pf.op = op;
      pf.tri = tri;
      for (int k = 0; k < 3; ++k) {
        pf.g[k] = nodes[ids[k]].gid;
        pf.bary[k] = nodes[ids[k]].bary;
        if (!InsideBary(pf.bary[k], 1e-6))
          return Status::InternalError;
      }
      proto.push_back(pf);
    }
    return Status::Ok;
  }

  Status BuildArrangement() {
    for (uint32_t op = 0; op < opCount; ++op) {
      for (uint64_t tri = 0; tri < ops[op].triPos.size(); ++tri) {
        const auto& st = ops[op].triPos[tri];
        std::vector<LNode> nodes;
        std::unordered_map<Gid, int> gid2local;
        auto getNode = [&](Gid gid) -> std::optional<int> {
          auto it = gid2local.find(gid);
          if (it != gid2local.end()) return it->second;
          vec3 w;
          if (!Barycentric(vertices[gid], st, w)) return std::nullopt;
          // Remove only floating-point-sized excursions from the closed
          // source triangle domain.
          for (int k = 0; k < 3; ++k) {
            if (std::abs(w[k]) < 1e-10) w[k] = 0.0;
          }
          const double sum = w.x + w.y + w.z;
          if (std::abs(sum - 1.0) < 1e-9 && sum != 0.0) w = w / sum;
          if (!InsideBary(w, 2e-7)) return std::nullopt;
          const int id = static_cast<int>(nodes.size());
          gid2local.emplace(gid, id);
          nodes.push_back({gid, {w.y, w.z}, w});
          return id;
        };

        const auto& tp = ops[op].triProp[tri];
        for (int k = 0; k < 3; ++k) {
          const Gid gid =
              ops[op].geomGlobal[ops[op].prop2geom[tp[k]]];
          if (!getNode(gid)) return Status::InternalError;
        }

        std::set<std::pair<int, int>> edgeSet;
        auto addEdgeGids = [&](Gid ga, Gid gb) -> Status {
          auto ia = getNode(ga);
          auto ib = getNode(gb);
          if (!ia || !ib) return Status::InternalError;
          int a = *ia, b = *ib;
          if (a == b) return Status::Ok;
          if (a > b) std::swap(a, b);
          edgeSet.emplace(a, b);
          return Status::Ok;
        };

        // Make every intersection-curve vertex known before classifying and
        // splitting the source boundary.  Otherwise an edge/face event on a
        // source edge would leave the unsplit long edge in the graph.
        for (int sid : ops[op].faceSegs[tri]) {
          const PairSeg& seg0 = segs[sid];
          if (!getNode(seg0.va) || !getNode(seg0.vb))
            return Status::InternalError;
          for (Gid g : seg0.cuts)
            if (!getNode(g)) return Status::InternalError;
        }

        // Source boundary, split at every event lying on it.
        std::vector<std::vector<int>> boundary(3);
        for (const auto& [gid, li] : gid2local) {
          const vec3& w = nodes[li].bary;
          for (int k = 0; k < 3; ++k) {
            if (std::abs(w[k]) < 2e-7) boundary[k].push_back(li);
          }
        }
        for (int k = 0; k < 3; ++k) {
          const int a = (k + 1) % 3;
          const int b = (k + 2) % 3;
          std::sort(boundary[k].begin(), boundary[k].end(),
                    [&](int x, int y) {
                      return nodes[x].bary[a] < nodes[y].bary[a];
                    });
          boundary[k].erase(
              std::unique(boundary[k].begin(), boundary[k].end()),
              boundary[k].end());
          // Ensure corners are present.
          auto cornerA = getNode(
              ops[op].geomGlobal[ops[op].prop2geom[tp[a]]]);
          auto cornerB = getNode(
              ops[op].geomGlobal[ops[op].prop2geom[tp[b]]]);
          if (!cornerA || !cornerB) return Status::InternalError;
          if (std::find(boundary[k].begin(), boundary[k].end(), *cornerA) ==
              boundary[k].end())
            boundary[k].push_back(*cornerA);
          if (std::find(boundary[k].begin(), boundary[k].end(), *cornerB) ==
              boundary[k].end())
            boundary[k].push_back(*cornerB);
          std::sort(boundary[k].begin(), boundary[k].end(),
                    [&](int x, int y) {
                      return nodes[x].bary[a] < nodes[y].bary[a];
                    });
          for (size_t i = 1; i < boundary[k].size(); ++i) {
            int x = boundary[k][i - 1], y = boundary[k][i];
            if (x != y) {
              if (x > y) std::swap(x, y);
              edgeSet.emplace(x, y);
            }
          }
        }

        // Intersection curves, ordered and split at triple events.
        for (int sid : ops[op].faceSegs[tri]) {
          PairSeg& seg = segs[sid];
          const vec3 p0 = vertices[seg.va];
          const vec3 delta = vertices[seg.vb] - p0;
          const double len2 = dot(delta, delta);
          std::vector<std::pair<double, Gid>> ordered;
          ordered.push_back({0.0, seg.va});
          ordered.push_back({1.0, seg.vb});
          for (Gid g : seg.cuts) {
            const double t =
                len2 > 0 ? dot(vertices[g] - p0, delta) / len2 : 0.0;
            ordered.push_back({std::max(0.0, std::min(1.0, t)), g});
          }
          std::sort(ordered.begin(), ordered.end());
          Gid prev = 0;
          bool havePrev = false;
          for (auto [t, g] : ordered) {
            (void)t;
            if (havePrev) {
              Status es = addEdgeGids(prev, g);
              if (es != Status::Ok) return es;
            }
            prev = g;
            havePrev = true;
          }
        }

        std::vector<std::pair<int, int>> edges(edgeSet.begin(),
                                               edgeSet.end());
        const int E = static_cast<int>(edges.size());
        if (E < 3) return Status::InternalError;

        // Half-edge construction and angular next-edge lookup.
        std::vector<int> from(2 * E), to(2 * E), nxt(2 * E, -1);
        std::vector<std::vector<int>> out(nodes.size());
        for (int e = 0; e < E; ++e) {
          const int h = 2 * e;
          from[h] = edges[e].first;
          to[h] = edges[e].second;
          from[h ^ 1] = edges[e].second;
          to[h ^ 1] = edges[e].first;
          out[from[h]].push_back(h);
          out[from[h ^ 1]].push_back(h ^ 1);
        }
        auto halfPlane = [](const vec2& v) {
          return (v.y > 0.0 || (v.y == 0.0 && v.x >= 0.0)) ? 0 : 1;
        };
        for (size_t v = 0; v < nodes.size(); ++v) {
          std::sort(out[v].begin(), out[v].end(), [&](int h1, int h2) {
            vec2 a = Sub2(nodes[to[h1]].p, nodes[from[h1]].p);
            vec2 b = Sub2(nodes[to[h2]].p, nodes[from[h2]].p);
            int ha = halfPlane(a), hb = halfPlane(b);
            if (ha != hb) return ha < hb;
            double cr = Cross2(a, b);
            if (cr != 0.0) return cr > 0.0;
            return h1 < h2;
          });
        }
        // All angular orders must be finalized before successors are chosen.
        for (size_t v = 0; v < nodes.size(); ++v) {
          for (int h : out[v]) {
            const int twin = h ^ 1;
            auto& ov = out[to[h]];
            auto it = std::find(ov.begin(), ov.end(), twin);
            if (it == ov.end()) return Status::InternalError;
            size_t pos = static_cast<size_t>(it - ov.begin());
            nxt[h] = ov[(pos + ov.size() - 1) % ov.size()];
          }
        }

        std::vector<char> used(2 * E, false);
        std::vector<Cycle> cycles;
        for (int h = 0; h < 2 * E; ++h) {
          if (used[h]) continue;
          Cycle c;
          int cur = h;
          do {
            if (used[cur]) return Status::InternalError;
            used[cur] = true;
            c.h.push_back(cur);
            c.nodes.push_back(from[cur]);
            cur = nxt[cur];
            if (cur < 0) return Status::InternalError;
          } while (cur != h);
          double area2 = 0.0;
          for (size_t i = 0; i < c.nodes.size(); ++i) {
            const vec2& a = nodes[c.nodes[i]].p;
            const vec2& b = nodes[c.nodes[(i + 1) % c.nodes.size()]].p;
            area2 += a.x * b.y - a.y * b.x;
          }
          c.area = area2 * 0.5;
          cycles.push_back(std::move(c));
        }

        std::vector<int> outers, holes;
        for (int ci = 0; ci < static_cast<int>(cycles.size()); ++ci) {
          Cycle& c = cycles[ci];
          if (c.nodes.size() < 3) continue;
          if (c.area > 1e-14) {
            outers.push_back(ci);
          } else if (c.area < -1e-14) {
            // Distinguish a real hole (left side remains in the source
            // triangle) from the reverse outer boundary (left side is the
            // unbounded outside).
            const vec2& u = nodes[c.nodes[0]].p;
            const vec2& v = nodes[c.nodes[1]].p;
            vec2 d = Sub2(v, u);
            const double len = std::sqrt(Dot2(d, d));
            if (len > 0.0) d = {d.x / len, d.y / len};
            const vec2 mid{(u.x + v.x) * 0.5, (u.y + v.y) * 0.5};
            const vec2 left{mid.x - d.y * 1e-8, mid.y + d.x * 1e-8};
            c.leftPoint = left;
            const bool inSource = left.x >= -1e-9 && left.y >= -1e-9 &&
                                   left.x + left.y <= 1.0 + 1e-9;
            if (inSource)
              holes.push_back(ci);
            else
              c.external = true;
          }
        }

        for (int oi : outers) {
          const Cycle& outer = cycles[oi];
          std::vector<int> assignedHoles;
          for (int hi : holes) {
            const Cycle& hc = cycles[hi];
            // The left side of a clockwise contour is the face material
            // outside the hole.  That point identifies the outer contour of
            // the cell that owns this hole.
            const vec2 inside = hc.leftPoint;
            int parent = -1;
            double parentArea = std::numeric_limits<double>::infinity();
            for (int oj : outers) {
              if (PointInCycle(inside, cycles[oj], nodes) &&
                  std::abs(cycles[oj].area) < parentArea) {
                parent = oj;
                parentArea = std::abs(cycles[oj].area);
              }
            }
            if (parent == oi) assignedHoles.push_back(hi);
          }
          Status ts =
              TriangulateCell(cycles, nodes, outer, assignedHoles, op, tri);
          if (ts != Status::Ok) return ts;
        }
      }
    }
    return Status::Ok;
  }

  Status ClassifyFaces() {
    if (proto.empty() && opCount > 0) {
      // Every valid non-empty closed operand has source triangles.
      for (const OpData& op : ops) {
        if (!op.empty) return Status::InternalError;
      }
    }
    std::vector<vec3> probes;
    probes.reserve(proto.size());
    for (const ProtoFace& pf : proto) {
      vec3 c{0, 0, 0};
      for (Gid g : pf.g) c = c + vertices[g];
      probes.push_back(c / 3.0);
    }

    std::vector<std::vector<int>> winding(opCount);
    for (uint32_t op = 0; op < opCount; ++op) {
      if (ops[op].empty || probes.empty()) {
        winding[op].assign(probes.size(), 0);
      } else {
        winding[op] = ops[op].handle.WindingNumber(probes);
        if (winding[op].size() != probes.size())
          return Status::InternalError;
      }
    }

    faces.reserve(proto.size());
    for (size_t i = 0; i < proto.size(); ++i) {
      const ProtoFace& pf = proto[i];
      uint8_t common = 0;
      for (uint32_t op = 0; op < opCount; ++op) {
        if (op == pf.op) continue;
        if (winding[op][i] != 0) common = common | (1u << op);
      }
      Face f;
      f.vertices = {pf.g[0], pf.g[1], pf.g[2]};
      f.sourceOperand = pf.op;
      f.sourceTriangle = pf.tri;
      f.sourceBarycentric = pf.bary;
      f.minusMask = static_cast<uint8_t>(common | (1u << pf.op));
      f.plusMask = static_cast<uint8_t>(common);
      if (f.minusMask == f.plusMask) return Status::InternalError;
      faces.push_back(f);
    }
    return Status::Ok;
  }

  struct OutRec {
    const Face* f;
    bool reverse;
    uint32_t op;
    uint64_t run;
  };

  bool TryExtract(uint32_t mask, Manifold& out) const {
    out = Manifold();
    if (status != Status::Ok) return false;
    if (mask == 0 || mask >= (1u << opCount)) return false;

    std::vector<OutRec> recs;
    for (const Face& f : faces) {
      if (mask == f.minusMask) {
        recs.push_back({&f, false, f.sourceOperand, 0});
      } else if (mask == f.plusMask) {
        recs.push_back({&f, true, f.sourceOperand, 0});
      }
    }
    if (recs.empty()) {
      out = Manifold();
      return true;
    }

    for (OutRec& r : recs) {
      const MeshGL64& sm = sources[r.op];
      const uint64_t t = r.f->sourceTriangle;
      int run = t < sm.NumTri() && t < ops[r.op].runOfTri.size()
                    ? ops[r.op].runOfTri[t]
                    : -1;
      if (run < 0 && !sm.runOriginalID.empty()) run = 0;
      r.run = static_cast<uint64_t>(std::max(run, 0));
    }
    std::stable_sort(recs.begin(), recs.end(), [](const OutRec& a,
                                                 const OutRec& b) {
      if (a.op != b.op) return a.op < b.op;
      return a.run < b.run;
    });

    MeshGL64 m;
    m.numProp = 3 + static_cast<int>(extraProps);
    std::vector<int64_t> canonical(vertices.size(), -1);
    auto appendCorner = [&](Gid gid, uint32_t op, uint64_t tri,
                            const vec3& bary) -> uint64_t {
      const MeshGL64& sm = sources[op];
      const size_t stride = static_cast<size_t>(sm.numProp);
      const uint64_t p0 = sm.triVerts[3 * tri + 0];
      const uint64_t p1 = sm.triVerts[3 * tri + 1];
      const uint64_t p2 = sm.triVerts[3 * tri + 2];
      const uint64_t ps[3] = {p0, p1, p2};
      const double bw[3] = {bary.x, bary.y, bary.z};

      const uint64_t newIdx = m.NumVert();
      const vec3 pos = vertices[gid];
      m.vertProperties.push_back(pos.x);
      m.vertProperties.push_back(pos.y);
      m.vertProperties.push_back(pos.z);
      for (uint32_t c = 0; c < extraProps; ++c) {
        double value = 0.0;
        for (int k = 0; k < 3; ++k) {
          value += bw[k] *
                   sm.vertProperties[static_cast<size_t>(ps[k]) * stride + 3 +
                                     c];
        }
        m.vertProperties.push_back(value);
      }
      if (canonical[gid] < 0) {
        canonical[gid] = static_cast<int64_t>(newIdx);
      } else {
        m.mergeFromVert.push_back(newIdx);
        m.mergeToVert.push_back(static_cast<uint64_t>(canonical[gid]));
      }
      return newIdx;
    };

    bool anyTransform = false;
    for (const OutRec& r : recs) {
      if (12 * (r.run + 1) <= sources[r.op].runTransform.size())
        anyTransform = true;
    }

    size_t recBegin = 0;
    while (recBegin < recs.size()) {
      size_t recEnd = recBegin + 1;
      while (recEnd < recs.size() && recs[recEnd].op == recs[recBegin].op &&
             recs[recEnd].run == recs[recBegin].run)
        ++recEnd;

      const uint64_t runIndex = static_cast<uint64_t>(m.triVerts.size());
      m.runIndex.push_back(runIndex);
      const OutRec& first = recs[recBegin];
      const MeshGL64& sm = sources[first.op];
      if (!sm.runOriginalID.empty() &&
          first.run < sm.runOriginalID.size()) {
        m.runOriginalID.push_back(sm.runOriginalID[first.run]);
      } else {
        m.runOriginalID.push_back(0);
      }
      if (!sm.runFlags.empty() && first.run < sm.runFlags.size())
        m.runFlags.push_back(sm.runFlags[first.run]);
      else if (!m.runFlags.empty())
        m.runFlags.push_back(0);

      // Always provide a complete transform array when any source had one.
      if (anyTransform) {
        if (12 * (first.run + 1) <= sm.runTransform.size()) {
          for (int k = 0; k < 12; ++k)
            m.runTransform.push_back(sm.runTransform[12 * first.run + k]);
        } else {
          const double identity[12] = {1, 0, 0, 0, 1, 0,
                                       0, 0, 1, 0, 0, 0};
          for (double x : identity) m.runTransform.push_back(x);
        }
      }

      const bool sourceHasFace = !sm.faceID.empty();
      for (size_t ri = recBegin; ri < recEnd; ++ri) {
        const OutRec& r = recs[ri];
        const Face& f = *r.f;
        int order[3] = {0, 1, 2};
        if (r.reverse) std::swap(order[1], order[2]);
        uint64_t ids[3];
        for (int k = 0; k < 3; ++k) {
          const int q = order[k];
          ids[k] = appendCorner(f.vertices[q], r.op, f.sourceTriangle,
                                f.sourceBarycentric[q]);
        }
        for (uint64_t id : ids) m.triVerts.push_back(id);
        if (sourceHasFace && f.sourceTriangle < sm.faceID.size())
          m.faceID.push_back(sm.faceID[f.sourceTriangle]);
        else
          m.faceID.push_back(0);
      }
      recBegin = recEnd;
    }
    m.runIndex.push_back(static_cast<uint64_t>(m.triVerts.size()));

    // If no source carried face IDs, do not invent a global face-ID channel.
    bool anyFace = false;
    for (uint32_t op = 0; op < opCount; ++op)
      anyFace = anyFace || !sources[op].faceID.empty();
    if (!anyFace) m.faceID.clear();

    out = Manifold(m);
    return out.Status() == Manifold::Error::NoError;
  }
};

SurfacePartition::SurfacePartition() : impl_(std::make_shared<Impl>()) {}

SurfacePartition::SurfacePartition(const std::vector<Manifold>& operands)
    : impl_(std::make_shared<Impl>()) {
  impl_->status = impl_->Build(operands);
}

SurfacePartition::SurfacePartition(std::initializer_list<Manifold> operands)
    : SurfacePartition(std::vector<Manifold>(operands)) {}

SurfacePartition::SurfacePartition(const Manifold& a)
    : SurfacePartition(std::vector<Manifold>{a}) {}

SurfacePartition::SurfacePartition(const Manifold& a, const Manifold& b)
    : SurfacePartition(std::vector<Manifold>{a, b}) {}

SurfacePartition::SurfacePartition(const Manifold& a, const Manifold& b,
                                   const Manifold& c)
    : SurfacePartition(std::vector<Manifold>{a, b, c}) {}

SurfacePartition::SurfacePartition(const Manifold& a, const Manifold& b,
                                   const Manifold& c, const Manifold& d)
    : SurfacePartition(std::vector<Manifold>{a, b, c, d}) {}

SurfacePartition::~SurfacePartition() = default;

SurfacePartition::Status SurfacePartition::GetStatus() const noexcept {
  return impl_ ? impl_->status : Status::InternalError;
}

uint32_t SurfacePartition::OperandCount() const noexcept {
  return impl_ ? impl_->opCount : 0;
}

uint32_t SurfacePartition::ExtraPropertyCount() const noexcept {
  return impl_ ? impl_->extraProps : 0;
}

const std::vector<vec3>& SurfacePartition::Vertices() const noexcept {
  static const std::vector<vec3> empty;
  return impl_ ? impl_->vertices : empty;
}

const std::vector<SurfacePartition::Face>& SurfacePartition::Faces()
    const noexcept {
  static const std::vector<Face> empty;
  return impl_ ? impl_->faces : empty;
}

const std::vector<MeshGL64>& SurfacePartition::SourceMeshes() const noexcept {
  static const std::vector<MeshGL64> empty;
  return impl_ ? impl_->sources : empty;
}

Manifold SurfacePartition::Extract(uint32_t mask) const {
  Manifold result;
  if (!TryExtract(mask, result)) {
    throw std::invalid_argument(
        "SurfacePartition::Extract: invalid mask or failed partition");
  }
  return result;
}

bool SurfacePartition::TryExtract(uint32_t mask, Manifold& out) const {
  if (!impl_) {
    out = Manifold();
    return false;
  }
  return impl_->TryExtract(mask, out);
}

}  // namespace manifold
