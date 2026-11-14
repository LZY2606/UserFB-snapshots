/*
 * Regression and target tests for Loop subdivision + limit projection.
 *
 * The production code in minipbrt.cpp is checked against an independent
 * scalar-typed reference template (instantiated in double precision) that
 * implements the same Loop rules and limit formulas separately.
 */

#include "task_support/check.h"
#include "minipbrt.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace minipbrt;

namespace {

static const float kTestPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------
// Small float helpers for building control cages and inspecting meshes.
// ---------------------------------------------------------------------------

struct Cage {
  std::vector<float> P;
  std::vector<int> indices;
  int levels = 0;

  unsigned numPoints() const { return unsigned(P.size() / 3); }
};

static void addPoint(Cage& cage, float x, float y, float z) {
  cage.P.push_back(x);
  cage.P.push_back(y);
  cage.P.push_back(z);
}

static void addFace(Cage& cage, int a, int b, int c) {
  cage.indices.push_back(a);
  cage.indices.push_back(b);
  cage.indices.push_back(c);
}

static LoopSubdiv* makeShape(const Cage& cage) {
  LoopSubdiv* shape = new LoopSubdiv();
  shape->levels = cage.levels;
  shape->num_points = cage.numPoints();
  shape->num_indices = unsigned(cage.indices.size());
  shape->P = new float[cage.P.size()];
  shape->indices = new int[cage.indices.size()];
  std::memcpy(shape->P, cage.P.data(), cage.P.size() * sizeof(float));
  std::memcpy(shape->indices, cage.indices.data(), cage.indices.size() * sizeof(int));
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      shape->shapeToWorld.start[i][j] = (i == j) ? 1.0f : 0.0f;
      shape->shapeToWorld.end[i][j] = (i == j) ? 1.0f : 0.0f;
    }
  }
  return shape;
}

// ---------------------------------------------------------------------------
// Independent scalar-typed reference implementation, run in double precision.
// ---------------------------------------------------------------------------

template <typename Real>
struct RVec {
  Real x = 0;
  Real y = 0;
  Real z = 0;

  RVec operator+(const RVec& o) const { return {x + o.x, y + o.y, z + o.z}; }
  RVec operator-(const RVec& o) const { return {x - o.x, y - o.y, z - o.z}; }
  RVec operator*(Real s) const { return {x * s, y * s, z * s}; }
  Real dot(const RVec& o) const { return x * o.x + y * o.y + z * o.z; }
  RVec cross(const RVec& o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
  }
  Real length2() const { return dot(*this); }
};

struct REdgeKey {
  int a;
  int b;
  bool operator==(const REdgeKey& o) const { return a == o.a && b == o.b; }
};

struct REdgeKeyHash {
  size_t operator()(const REdgeKey& k) const {
    uint64_t x = (uint64_t(uint32_t(k.a)) << 32) | uint32_t(k.b);
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return size_t(x);
  }
};

static inline REdgeKey redgeKey(int u, int v) {
  return (u < v) ? REdgeKey{u, v} : REdgeKey{v, u};
}

template <typename Real>
struct RefMesh {
  struct EdgeInfo {
    int face[2] = {-1, -1};
    int opposite[2] = {-1, -1};
    int count = 0;
  };

  std::vector<RVec<Real> > P;
  std::vector<std::array<int, 3> > faces;
  std::unordered_map<REdgeKey, EdgeInfo, REdgeKeyHash> edges;
  std::vector<std::vector<int> > ring;
  std::vector<char> boundary;

  unsigned numVerts() const { return unsigned(P.size()); }

  void derive() {
    unsigned nv = numVerts();
    edges.clear();
    std::vector<std::vector<std::pair<int, int> > > directed(nv);
    for (int f = 0; f < int(faces.size()); f++) {
      const std::array<int, 3>& fc = faces[f];
      for (int i = 0; i < 3; i++) {
        int u = fc[i];
        int v = fc[(i + 1) % 3];
        int opp = fc[(i + 2) % 3];
        directed[u].push_back({v, f});
        REdgeKey key = redgeKey(u, v);
        EdgeInfo& info = edges[key];
        info.face[info.count] = f;
        info.opposite[info.count] = opp;
        info.count++;
      }
    }

    std::vector<int> boundaryEdges(nv, 0);
    std::vector<unsigned> degree(nv, 0);
    for (const auto& kv : edges) {
      degree[kv.first.a]++;
      degree[kv.first.b]++;
      if (kv.second.count == 1) {
        boundaryEdges[kv.first.a]++;
        boundaryEdges[kv.first.b]++;
      }
    }

    ring.assign(nv, std::vector<int>());
    boundary.assign(nv, false);
    for (unsigned v = 0; v < nv; v++) {
      boundary[v] = (boundaryEdges[v] == 2);
      // Valence is the undirected edge degree; the directed list misses
      // neighbors reached only via an incoming boundary edge.
      unsigned valence = degree[v];

      if (!boundary[v]) {
        int cur = directed[v][0].first;
        int curFace = directed[v][0].second;
        int start = cur;
        do {
          ring[v].push_back(cur);
          const EdgeInfo& info = edges.find(redgeKey(int(v), cur))->second;
          int nextFace = (info.face[0] == curFace) ? info.face[1] : info.face[0];
          const std::array<int, 3>& fc = faces[nextFace];
          int next = fc[0];
          for (int k = 0; k < 3; k++) {
            if (fc[k] != int(v) && fc[k] != cur) {
              next = fc[k];
              break;
            }
          }
          curFace = nextFace;
          cur = next;
        } while (cur != start);
      }
      else {
        int start = -1;
        int startFace = -1;
        for (const auto& dn : directed[v]) {
          if (edges.find(redgeKey(int(v), dn.first))->second.count == 1) {
            start = dn.first;
            startFace = dn.second;
            break;
          }
        }
        int cur = start;
        int curFace = startFace;
        ring[v].push_back(cur);
        for (unsigned steps = 0; steps < valence; steps++) {
          const std::array<int, 3>& fc = faces[curFace];
          int next = fc[0];
          for (int k = 0; k < 3; k++) {
            if (fc[k] != int(v) && fc[k] != cur) {
              next = fc[k];
              break;
            }
          }
          ring[v].push_back(next);
          const EdgeInfo& info = edges.find(redgeKey(int(v), next))->second;
          if (info.count == 1) {
            break;
          }
          curFace = (info.face[0] == curFace) ? info.face[1] : info.face[0];
          cur = next;
        }
      }
    }
  }

  void init(const Cage& cage) {
    unsigned np = cage.numPoints();
    P.resize(np);
    for (unsigned i = 0; i < np; i++) {
      P[i] = {Real(cage.P[3 * i]), Real(cage.P[3 * i + 1]), Real(cage.P[3 * i + 2])};
    }
    faces.resize(cage.indices.size() / 3);
    for (unsigned f = 0; f < faces.size(); f++) {
      faces[f] = {cage.indices[3 * f], cage.indices[3 * f + 1], cage.indices[3 * f + 2]};
    }
    derive();
  }

  void subdivide() {
    unsigned oldV = numVerts();
    unsigned oldF = unsigned(faces.size());

    std::unordered_map<REdgeKey, int, REdgeKeyHash> edgeVertex;
    edgeVertex.reserve(edges.size() * 2);
    int next = int(oldV);
    for (const auto& kv : edges) {
      edgeVertex[kv.first] = next++;
    }

    std::vector<RVec<Real> > newP(oldV + edges.size());

    for (const auto& kv : edges) {
      const REdgeKey& key = kv.first;
      const EdgeInfo& info = kv.second;
      int idx = edgeVertex.find(key)->second;
      if (info.count == 1) {
        newP[idx] = (P[key.a] + P[key.b]) * Real(0.5);
      }
      else {
        newP[idx] = (P[key.a] + P[key.b]) * (Real(3) / Real(8)) +
                    (P[info.opposite[0]] + P[info.opposite[1]]) * (Real(1) / Real(8));
      }
    }

    for (unsigned v = 0; v < oldV; v++) {
      unsigned n = unsigned(ring[v].size());
      RVec<Real> sumQ{0, 0, 0};
      for (unsigned k = 0; k < n; k++) {
        sumQ = sumQ + P[ring[v][k]];
      }
      if (boundary[v]) {
        newP[v] = P[v] * (Real(3) / Real(4)) +
                  (P[ring[v].front()] + P[ring[v].back()]) * (Real(1) / Real(8));
      }
      else {
        Real beta = (n == 3) ? Real(3) / Real(16) : Real(3) / (Real(8) * Real(n));
        newP[v] = P[v] * (Real(1) - Real(n) * beta) + sumQ * beta;
      }
    }

    std::vector<std::array<int, 3> > newFaces(oldF * 4);
    for (unsigned f = 0; f < oldF; f++) {
      int a = faces[f][0];
      int b = faces[f][1];
      int c = faces[f][2];
      int eab = edgeVertex.find(redgeKey(a, b))->second;
      int ebc = edgeVertex.find(redgeKey(b, c))->second;
      int eca = edgeVertex.find(redgeKey(c, a))->second;
      newFaces[4 * f + 0] = {a, eab, eca};
      newFaces[4 * f + 1] = {b, ebc, eab};
      newFaces[4 * f + 2] = {c, eca, ebc};
      newFaces[4 * f + 3] = {eab, ebc, eca};
    }

    P.swap(newP);
    faces.swap(newFaces);
    derive();
  }

  void limit(std::vector<RVec<Real> >& outPos, std::vector<RVec<Real> >& outNrm) const {
    const Real pi = std::acos(Real(-1));
    outPos.resize(numVerts());
    outNrm.resize(numVerts());
    for (unsigned v = 0; v < numVerts(); v++) {
      unsigned n = unsigned(ring[v].size());
      RVec<Real> S{0, 0, 0};
      RVec<Real> T{0, 0, 0};
      RVec<Real> pos;
      if (!boundary[v]) {
        Real beta = (n == 3) ? Real(3) / Real(16) : Real(3) / (Real(8) * Real(n));
        Real gamma = Real(1) / (Real(n) + Real(3) / (Real(8) * beta));
        RVec<Real> sumQ{0, 0, 0};
        for (unsigned k = 0; k < n; k++) {
          sumQ = sumQ + P[ring[v][k]];
          Real angle = Real(2) * pi * Real(k) / Real(n);
          S = S + P[ring[v][k]] * std::cos(angle);
          T = T + P[ring[v][k]] * std::sin(angle);
        }
        pos = P[v] * (Real(1) - Real(n) * gamma) + sumQ * gamma;
      }
      else {
        const RVec<Real>& q0 = P[ring[v].front()];
        const RVec<Real>& ql = P[ring[v].back()];
        pos = P[v] * (Real(3) / Real(5)) + (q0 + ql) * (Real(1) / Real(5));
        S = ql - q0;
        if (n == 2) {
          T = q0 + P[ring[v][1]] - P[v] * Real(2);
        }
        else if (n == 3) {
          T = P[ring[v][1]] - P[v];
        }
        else if (n == 4) {
          T = q0 * Real(-1) + P[ring[v][1]] * Real(2) + P[ring[v][2]] * Real(2) +
              P[ring[v][3]] * Real(-1) - P[v] * Real(2);
        }
        else {
          Real theta = pi / Real(n - 1);
          RVec<Real> inner{0, 0, 0};
          for (unsigned k = 1; k + 1 < n; k++) {
            inner = inner + P[ring[v][k]] * std::sin(Real(k) * theta);
          }
          T = (q0 + ql) * (-std::sin(theta)) + inner * (Real(2) - Real(2) * std::cos(theta));
        }
      }

      RVec<Real> nrm = S.cross(T);
      Real len = std::sqrt(nrm.length2());
      nrm = nrm * (Real(1) / len);

      // Fix the sign from the lowest-indexed incident non-degenerate face,
      // matching the production rule (neighbors are appended in face order).
      RVec<Real> ref{0, 0, 0};
      for (int f = 0; f < int(faces.size()); f++) {
        const std::array<int, 3>& fc = faces[f];
        if (fc[0] != int(v) && fc[1] != int(v) && fc[2] != int(v)) {
          continue;
        }
        RVec<Real> fn = (P[fc[1]] - P[fc[0]]).cross(P[fc[2]] - P[fc[0]]);
        if (fn.length2() > Real(0)) {
          ref = fn;
          break;
        }
      }
      if (nrm.dot(ref) < 0) {
        nrm = nrm * Real(-1);
      }
      outPos[v] = pos;
      outNrm[v] = nrm;
    }
  }
};

// Run the reference for `levels` steps and then the limit projection.
template <typename Real>
static void referenceResult(const Cage& cage, int levels,
                            std::vector<RVec<Real> >& refPos,
                            std::vector<RVec<Real> >& refNrm,
                            std::vector<std::array<int, 3> >& refFaces) {
  RefMesh<Real> ref;
  ref.init(cage);
  for (int i = 0; i < levels; i++) {
    ref.subdivide();
  }
  ref.limit(refPos, refNrm);
  refFaces = ref.faces;
}

// ---------------------------------------------------------------------------
// Inspection helpers for the production TriangleMesh.
// ---------------------------------------------------------------------------

// Bounding-box diagonal of the control cage, used as the length scale.
static float cageScale(const Cage& cage) {
  float mn[3] = {cage.P[0], cage.P[1], cage.P[2]};
  float mx[3] = {cage.P[0], cage.P[1], cage.P[2]};
  for (unsigned i = 0; i < cage.numPoints(); i++) {
    for (int k = 0; k < 3; k++) {
      float val = cage.P[3 * i + k];
      mn[k] = std::fmin(mn[k], val);
      mx[k] = std::fmax(mx[k], val);
    }
  }
  float d2 = 0.0f;
  for (int k = 0; k < 3; k++) {
    float d = mx[k] - mn[k];
    d2 += d * d;
  }
  return std::sqrt(d2);
}

// Compare a production mesh against the double-precision reference as
// unordered vertex clouds (edge-vertex indexing order is implementation
// defined), carrying the limit normal along with each position.
static bool compareToReference(const TriangleMesh* mesh, const Cage& cage,
                               float posTol, float nrmTol,
                               const std::vector<std::array<int, 3> >& refFaces) {
  std::vector<RVec<double> > refPos;
  std::vector<RVec<double> > refNrm;
  std::vector<std::array<int, 3> > faces;
  referenceResult<double>(cage, cage.levels, refPos, refNrm, faces);

  if (mesh->num_vertices != refPos.size()) {
    return false;
  }
  if (mesh->num_indices / 3 != refFaces.size()) {
    return false;
  }

  std::vector<char> used(refPos.size(), 0);
  for (unsigned i = 0; i < mesh->num_vertices; i++) {
    RVec<double> p{mesh->P[3 * i], mesh->P[3 * i + 1], mesh->P[3 * i + 2]};
    RVec<double> n{mesh->N[3 * i], mesh->N[3 * i + 1], mesh->N[3 * i + 2]};
    if (std::fabs(std::sqrt(n.length2()) - 1.0) > nrmTol) {
      return false;
    }
    double best = 1e300;
    unsigned bestJ = 0;
    for (unsigned j = 0; j < refPos.size(); j++) {
      double d = (p - refPos[j]).length2();
      if (d < best) {
        best = d;
        bestJ = j;
      }
    }
    if (used[bestJ] || std::sqrt(best) > posTol) {
      return false;
    }
    if ((n - refNrm[bestJ]).length2() > double(nrmTol) * double(nrmTol)) {
      return false;
    }
    used[bestJ] = 1;
  }
  for (char u : used) {
    if (!u) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Control cage definitions.
// ---------------------------------------------------------------------------

// Irregular tetrahedron: every vertex has interior valence 3.
static Cage tetraCage(float ox, float oy, float oz) {
  Cage cage;
  addPoint(cage, ox + 0.0f, oy + 0.0f, oz + 0.0f);
  addPoint(cage, ox + 2.0f, oy + 0.1f, oz - 0.2f);
  addPoint(cage, ox + 0.1f, oy + 3.0f, oz + 0.3f);
  addPoint(cage, ox - 0.2f, oy + 0.2f, oz + 4.0f);
  addFace(cage, 0, 2, 1);
  addFace(cage, 0, 1, 3);
  addFace(cage, 0, 3, 2);
  addFace(cage, 1, 2, 3);
  return cage;
}

// Jittered icosahedron: every vertex has interior valence 5.
static Cage icosaCage(float ox, float oy, float oz) {
  const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f;
  const float base[12][3] = {
    {-1, phi, 0}, {1, phi, 0}, {-1, -phi, 0}, {1, -phi, 0},
    {0, -1, phi}, {0, 1, phi}, {0, -1, -phi}, {0, 1, -phi},
    {phi, 0, -1}, {-phi, 0, -1}, {phi, 0, 1}, {-phi, 0, 1}
  };
  const int faceData[20][3] = {
    {0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11},
    {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
    {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9},
    {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}
  };
  Cage cage;
  for (int i = 0; i < 12; i++) {
    float scale = 1.0f + 0.11f * std::sin(3.1f * float(i) + 1.3f);
    addPoint(cage, ox + base[i][0] * scale, oy + base[i][1] * scale, oz + base[i][2] * scale);
  }
  for (int f = 0; f < 20; f++) {
    addFace(cage, faceData[f][0], faceData[f][1], faceData[f][2]);
  }
  return cage;
}

// Jittered 7-gonal bipyramid: the two apices have interior valence 7.
static Cage bipyramid7Cage(float ox, float oy, float oz) {
  Cage cage;
  const int n = 7;
  for (int i = 0; i < n; i++) {
    float angle = 2.0f * kTestPi * float(i) / float(n);
    float radius = 1.0f + 0.07f * std::sin(2.0f * float(i) + 1.0f);
    addPoint(cage, ox + radius * std::cos(angle),
             oy + radius * std::sin(angle),
             oz + 0.05f * std::cos(3.0f * float(i)));
  }
  int top = n;
  int bottom = n + 1;
  addPoint(cage, ox + 0.03f, oy - 0.02f, oz + 1.3f);
  addPoint(cage, ox - 0.04f, oy + 0.02f, oz - 1.1f);
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    addFace(cage, top, i, j);
    addFace(cage, bottom, j, i);
  }
  return cage;
}

static Cage closedAsymmetricCage() {
  Cage cage;
  Cage parts[3] = {tetraCage(0.0f, 0.0f, 0.0f),
                   icosaCage(12.0f, 0.0f, 0.0f),
                   bipyramid7Cage(24.0f, 0.0f, 0.0f)};
  for (int p = 0; p < 3; p++) {
    unsigned offset = cage.numPoints();
    cage.P.insert(cage.P.end(), parts[p].P.begin(), parts[p].P.end());
    for (int idx : parts[p].indices) {
      cage.indices.push_back(idx + int(offset));
    }
  }
  return cage;
}

// Open triangle fan: center is a boundary vertex with m-1 boundary neighbors,
// the two rim ends have 2 neighbors, intermediate rim vertices have 3.
// Heights are chosen so the fan is deliberately non-planar.
static Cage fanCage(int m, float ox, float oy, float oz) {
  Cage cage;
  addPoint(cage, ox + 0.0f, oy + 0.0f, oz + 0.35f); // center = vertex 0
  for (int i = 0; i < m; i++) {
    float t = float(i) / float(m - 1);
    float angle = t * 1.25f;
    addPoint(cage, ox + 1.3f * std::cos(angle),
             oy + 1.1f * std::sin(angle),
             oz + 0.2f * std::sin(2.7f * t + 0.4f) + 0.07f * float(i % 2));
  }
  for (int i = 0; i + 1 < m; i++) {
    addFace(cage, 0, 1 + i, 1 + i + 1);
  }
  return cage;
}

static Cage openNonPlanarCage() {
  Cage cage;
  Cage a = fanCage(4, 0.0f, 0.0f, 0.0f);    // center boundary valence 4
  Cage b = fanCage(5, 0.0f, 8.0f, 0.0f);    // center boundary valence 5
  cage.P.insert(cage.P.end(), a.P.begin(), a.P.end());
  for (int idx : a.indices) {
    cage.indices.push_back(idx);
  }
  unsigned offset = cage.numPoints();
  cage.P.insert(cage.P.end(), b.P.begin(), b.P.end());
  for (int idx : b.indices) {
    cage.indices.push_back(idx + int(offset));
  }
  return cage;
}

// Two tetrahedra with one coincident coordinate pair that must stay welded
// apart: topology identity follows indices, not positions.
static Cage coincidentComponentsCage() {
  Cage a = tetraCage(0.0f, 0.0f, 0.0f);
  Cage b = tetraCage(0.0f, 0.0f, 0.0f); // identical coordinates
  Cage cage;
  cage.P = a.P;
  cage.indices = a.indices;
  unsigned offset = cage.numPoints();
  cage.P.insert(cage.P.end(), b.P.begin(), b.P.end());
  for (int idx : b.indices) {
    cage.indices.push_back(idx + int(offset));
  }
  return cage;
}

// Assert the control cage has the valences the task requires, using the
// reference topology code.
static bool checkControlValences(const Cage& cage, bool expectClosed,
                                 const std::vector<unsigned>& interiorValences,
                                 const std::vector<unsigned>& boundaryValences) {
  RefMesh<double> ref;
  ref.init(cage);
  std::vector<unsigned> gotInterior;
  std::vector<unsigned> gotBoundary;
  for (unsigned v = 0; v < ref.numVerts(); v++) {
    if (ref.boundary[v]) {
      gotBoundary.push_back(unsigned(ref.ring[v].size()));
    }
    else {
      gotInterior.push_back(unsigned(ref.ring[v].size()));
    }
  }
  for (unsigned wanted : interiorValences) {
    if (std::find(gotInterior.begin(), gotInterior.end(), wanted) == gotInterior.end()) {
      return false;
    }
  }
  for (unsigned wanted : boundaryValences) {
    if (std::find(gotBoundary.begin(), gotBoundary.end(), wanted) == gotBoundary.end()) {
      return false;
    }
  }
  if (expectClosed && !gotBoundary.empty()) {
    return false;
  }
  if (!expectClosed && gotBoundary.empty()) {
    return false;
  }
  return true;
}

// Full structural check of a production output mesh: manifold, consistently
// oriented faces, no duplicates, and boundary edges forming closed loops.
static bool checkManifoldAndWinding(const TriangleMesh* mesh, bool expectClosed,
                                    unsigned* outBoundaryEdges) {
  struct EdgeRec {
    int count = 0;
    int dirU[2] = {-1, -1};
    int dirV[2] = {-1, -1};
  };
  std::unordered_map<REdgeKey, EdgeRec, REdgeKeyHash> emap;
  unsigned numFaces = mesh->num_indices / 3;
  struct FKey3 {
    int v[3];
    bool operator==(const FKey3& o) const {
      return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2];
    }
  };
  struct FKey3Hash {
    size_t operator()(const FKey3& k) const {
      uint64_t h = uint64_t(k.v[0]) * 0x9e3779b97f4a7c15ULL;
      h ^= uint64_t(k.v[1]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      h ^= uint64_t(k.v[2]) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return size_t(h);
    }
  };
  std::unordered_set<FKey3, FKey3Hash> faceSet;
  faceSet.reserve(numFaces);

  for (unsigned f = 0; f < numFaces; f++) {
    int tri[3];
    for (int k = 0; k < 3; k++) {
      tri[k] = mesh->indices[3 * f + k];
      if (tri[k] < 0 || unsigned(tri[k]) >= mesh->num_vertices) {
        return false;
      }
    }
    if (tri[0] == tri[1] || tri[1] == tri[2] || tri[2] == tri[0]) {
      return false;
    }
    FKey3 fk{{tri[0], tri[1], tri[2]}};
    std::sort(fk.v, fk.v + 3);
    if (!faceSet.insert(fk).second) {
      return false; // duplicate face
    }
    for (int i = 0; i < 3; i++) {
      int u = tri[i];
      int v = tri[(i + 1) % 3];
      REdgeKey key = redgeKey(u, v);
      EdgeRec& rec = emap[key];
      if (rec.count >= 2) {
        return false; // non-manifold edge
      }
      rec.dirU[rec.count] = u;
      rec.dirV[rec.count] = v;
      rec.count++;
    }
  }

  // Every shared edge must be traversed in opposite directions (winding),
  // and boundary edges must form vertex-disjoint closed boundary loops.
  std::vector<int> boundaryDegree(mesh->num_vertices, 0);
  unsigned boundaryEdges = 0;
  for (const auto& kv : emap) {
    const EdgeRec& rec = kv.second;
    if (rec.count == 1) {
      boundaryEdges++;
      boundaryDegree[kv.first.a]++;
      boundaryDegree[kv.first.b]++;
    }
    else {
      if (!(rec.dirU[0] == rec.dirV[1] && rec.dirV[0] == rec.dirU[1])) {
        return false; // same-oriented shared edge
      }
    }
  }
  for (unsigned v = 0; v < mesh->num_vertices; v++) {
    if (boundaryDegree[v] != 0 && boundaryDegree[v] != 2) {
      return false; // boundary does not connect into loops
    }
    if (expectClosed && boundaryDegree[v] != 0) {
      return false;
    }
  }
  if (expectClosed && boundaryEdges != 0) {
    return false;
  }
  if (!expectClosed && boundaryEdges == 0) {
    return false;
  }
  if (outBoundaryEdges != nullptr) {
    *outBoundaryEdges = boundaryEdges;
  }
  return true;
}

// Every output normal must be unit length.
static bool checkNormalsUnit(const TriangleMesh* mesh) {
  for (unsigned v = 0; v < mesh->num_vertices; v++) {
    RVec<float> n{mesh->N[3 * v], mesh->N[3 * v + 1], mesh->N[3 * v + 2]};
    if (std::fabs(std::sqrt(n.length2()) - 1.0f) > 2e-4f) {
      return false;
    }
  }
  return true;
}

// Each vertex normal must agree in sign with the winding of its lowest-indexed
// incident non-degenerate face: the normal sign follows face orientation,
// rather than being averaged from output face normals.
static bool checkNormalsSignConsistent(const TriangleMesh* mesh) {
  unsigned numFaces = mesh->num_indices / 3;
  std::vector<int> lowestFace(mesh->num_vertices, -1);
  for (unsigned f = 0; f < numFaces; f++) {
    for (int k = 0; k < 3; k++) {
      int v = mesh->indices[3 * f + k];
      if (lowestFace[v] < 0) {
        lowestFace[v] = int(f);
      }
    }
  }
  for (unsigned v = 0; v < mesh->num_vertices; v++) {
    int f = lowestFace[v];
    if (f < 0) {
      return false;
    }
    int tri[3] = { mesh->indices[3 * f], mesh->indices[3 * f + 1], mesh->indices[3 * f + 2] };
    RVec<float> p0{mesh->P[3 * tri[0]], mesh->P[3 * tri[0] + 1], mesh->P[3 * tri[0] + 2]};
    RVec<float> p1{mesh->P[3 * tri[1]], mesh->P[3 * tri[1] + 1], mesh->P[3 * tri[1] + 2]};
    RVec<float> p2{mesh->P[3 * tri[2]], mesh->P[3 * tri[2] + 1], mesh->P[3 * tri[2] + 2]};
    RVec<float> fn = (p1 - p0).cross(p2 - p0);
    if (fn.length2() <= 0.0f) {
      return false;
    }
    RVec<float> n{mesh->N[3 * v], mesh->N[3 * v + 1], mesh->N[3 * v + 2]};
    if (n.dot(fn) <= 0.0f) {
      return false;
    }
  }
  return true;
}

// On sufficiently smooth (refined) meshes each vertex normal lies inside the
// cone spanned by all incident face normals. Coarse control cages such as a
// tetrahedron can have incident face normals spanning more than a hemisphere,
// so this stronger property is only asserted after subdivision.
static bool checkNormalsOriented(const TriangleMesh* mesh) {
  if (!checkNormalsUnit(mesh) || !checkNormalsSignConsistent(mesh)) {
    return false;
  }
  unsigned numFaces = mesh->num_indices / 3;
  for (unsigned f = 0; f < numFaces; f++) {
    int tri[3] = { mesh->indices[3 * f], mesh->indices[3 * f + 1], mesh->indices[3 * f + 2] };
    RVec<float> p0{mesh->P[3 * tri[0]], mesh->P[3 * tri[0] + 1], mesh->P[3 * tri[0] + 2]};
    RVec<float> p1{mesh->P[3 * tri[1]], mesh->P[3 * tri[1] + 1], mesh->P[3 * tri[1] + 2]};
    RVec<float> p2{mesh->P[3 * tri[2]], mesh->P[3 * tri[2] + 1], mesh->P[3 * tri[2] + 2]};
    RVec<float> fn = (p1 - p0).cross(p2 - p0);
    if (fn.length2() <= 0.0f) {
      return false;
    }
    for (int k = 0; k < 3; k++) {
      RVec<float> n{mesh->N[3 * tri[k]], mesh->N[3 * tri[k] + 1], mesh->N[3 * tri[k] + 2]};
      if (n.dot(fn) <= 0.0f) {
        return false;
      }
    }
  }
  return true;
}

static unsigned edgeCountOf(const Cage& cage) {
  std::unordered_set<REdgeKey, REdgeKeyHash> edges;
  for (unsigned f = 0; f < cage.indices.size() / 3; f++) {
    for (int i = 0; i < 3; i++) {
      edges.insert(redgeKey(cage.indices[3 * f + i], cage.indices[3 * f + ((i + 1) % 3)]));
    }
  }
  return unsigned(edges.size());
}

// Expected vertex/face counts after `levels` Loop steps: V' = V+E, F' = 4F.
static void expectedCounts(const Cage& cage, int levels,
                           unsigned& outV, unsigned& outF) {
  uint64_t v = cage.numPoints();
  uint64_t f = cage.indices.size() / 3;
  uint64_t e = edgeCountOf(cage);
  for (int i = 0; i < levels; i++) {
    uint64_t nv = v + e;
    e = 2 * e + 3 * f;
    v = nv;
    f *= 4;
  }
  outV = unsigned(v);
  outF = unsigned(f);
}

// ===========================================================================
// Group 1: topology
// ===========================================================================

static int run_topo_closed() {
  Cage cage = closedAsymmetricCage();
  TASK_CHECK(checkControlValences(cage, true, {3, 5, 7}, {}));
  for (int levels = 0; levels <= 2; levels++) {
    cage.levels = levels;
    std::unique_ptr<LoopSubdiv> shape(makeShape(cage));
    std::unique_ptr<TriangleMesh> mesh(shape->triangle_mesh());
    TASK_CHECK(mesh != nullptr);
    TASK_CHECK(mesh->N != nullptr && mesh->S == nullptr && mesh->uv == nullptr);
    unsigned boundaryEdges = 0;
    TASK_CHECK(checkManifoldAndWinding(mesh.get(), true, &boundaryEdges));
    TASK_CHECK(boundaryEdges == 0);
    TASK_CHECK(checkNormalsUnit(mesh.get()));
    // The production normal sign follows the winding of the pre-projection
    // faces; on a coarse cage the limit-position face normal can still tilt
    // past it, so limit-face-based checks start once the mesh is refined.
    // Signed normals themselves are compared against the tangent template in
    // the limit_geometry group at every level.
    if (levels >= 1) {
      TASK_CHECK(checkNormalsSignConsistent(mesh.get()));
    }
    if (levels >= 2) {
      TASK_CHECK(checkNormalsOriented(mesh.get()));
    }
    unsigned expectV = 0, expectF = 0;
    expectedCounts(cage, levels, expectV, expectF);
    TASK_CHECK(mesh->num_vertices == expectV);
    TASK_CHECK(mesh->num_indices == expectF * 3);
  }
  return 0;
}

static int run_topo_open_fan() {
  Cage cage = openNonPlanarCage();
  // Boundary neighbor counts 2, 3, 4 and >=5 must all occur.
  TASK_CHECK(checkControlValences(cage, false, {}, {2, 3, 4, 5}));
  for (int levels = 0; levels <= 2; levels++) {
    cage.levels = levels;
    std::unique_ptr<LoopSubdiv> shape(makeShape(cage));
    std::unique_ptr<TriangleMesh> mesh(shape->triangle_mesh());
    TASK_CHECK(mesh != nullptr);
    unsigned boundaryEdges = 0;
    TASK_CHECK(checkManifoldAndWinding(mesh.get(), false, &boundaryEdges));
    // Two open components, each fan rim is one boundary loop.
    TASK_CHECK(boundaryEdges >= 8);
    TASK_CHECK(checkNormalsUnit(mesh.get()));
    if (levels >= 1) {
      TASK_CHECK(checkNormalsSignConsistent(mesh.get()));
      TASK_CHECK(checkNormalsOriented(mesh.get()));
    }
    unsigned expectV = 0, expectF = 0;
    expectedCounts(cage, levels, expectV, expectF);
    TASK_CHECK(mesh->num_vertices == expectV);
    TASK_CHECK(mesh->num_indices == expectF * 3);
  }
  return 0;
}

static int run_topo_coincident_isolation() {
  Cage single = tetraCage(0.0f, 0.0f, 0.0f);
  single.levels = 1;
  Cage paired = coincidentComponentsCage();
  paired.levels = 1;

  std::unique_ptr<LoopSubdiv> one(makeShape(single));
  std::unique_ptr<LoopSubdiv> two(makeShape(paired));
  std::unique_ptr<TriangleMesh> meshOne(one->triangle_mesh());
  std::unique_ptr<TriangleMesh> meshTwo(two->triangle_mesh());
  TASK_CHECK(meshOne && meshTwo);
  // Coincident coordinates must not be welded: the paired mesh has exactly
  // twice as many vertices and faces.
  TASK_CHECK(meshTwo->num_vertices == 2 * meshOne->num_vertices);
  TASK_CHECK(meshTwo->num_indices == 2 * meshOne->num_indices);

  // Union-find over the output faces: the result must split into exactly two
  // disconnected components, each with as many vertices as a single tetra mesh.
  unsigned nv = meshTwo->num_vertices;
  std::vector<unsigned> parent(nv);
  for (unsigned i = 0; i < nv; i++) {
    parent[i] = i;
  }
  std::function<unsigned(unsigned)> find = [&](unsigned x) -> unsigned {
    return parent[x] == x ? x : (parent[x] = find(parent[x]));
  };
  auto unite = [&](unsigned a, unsigned b) {
    a = find(a);
    b = find(b);
    if (a != b) {
      parent[b] = a;
    }
  };
  unsigned faces = meshTwo->num_indices / 3;
  for (unsigned f = 0; f < faces; f++) {
    int t[3] = { meshTwo->indices[3 * f], meshTwo->indices[3 * f + 1], meshTwo->indices[3 * f + 2] };
    unite(t[0], t[1]);
    unite(t[1], t[2]);
  }
  std::vector<unsigned> componentSize(nv, 0);
  for (unsigned v = 0; v < nv; v++) {
    componentSize[find(v)]++;
  }
  std::vector<unsigned> sizes;
  for (unsigned s : componentSize) {
    if (s > 0) {
      sizes.push_back(s);
    }
  }
  TASK_CHECK(sizes.size() == 2);
  TASK_CHECK(sizes[0] == meshOne->num_vertices);
  TASK_CHECK(sizes[1] == meshOne->num_vertices);
  return 0;
}

static int run_topo_rejects() {
  const float goodP[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
  const int goodIdx[] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};

  auto attempt = [](const float* p, unsigned np, const int* idx, unsigned ni,
                    int levels) -> TriangleMesh* {
    // The shape destructor deletes its arrays, so give it heap-owned copies.
    LoopSubdiv* shape = new LoopSubdiv();
    shape->levels = levels;
    shape->num_points = np;
    shape->num_indices = ni;
    shape->P = (p != nullptr && np > 0) ? new float[np * 3] : nullptr;
    if (shape->P != nullptr) {
      std::memcpy(shape->P, p, np * 3 * sizeof(float));
    }
    shape->indices = (idx != nullptr && ni > 0) ? new int[ni] : nullptr;
    if (shape->indices != nullptr) {
      std::memcpy(shape->indices, idx, ni * sizeof(int));
    }
    TriangleMesh* result = shape->triangle_mesh();
    delete shape;
    return result;
  };

  // Empty cage.
  TASK_CHECK(attempt(goodP, 0, goodIdx, 12, 1) == nullptr);
  TASK_CHECK(attempt(nullptr, 4, goodIdx, 12, 1) == nullptr);
  // Nonzero count with no array.
  TASK_CHECK(attempt(goodP, 4, nullptr, 12, 1) == nullptr);
  TASK_CHECK(attempt(goodP, 4, goodIdx, 0, 1) == nullptr);
  // Index count not a multiple of 3.
  TASK_CHECK(attempt(goodP, 4, goodIdx, 10, 1) == nullptr);
  // Out of range index.
  const int badRange[] = {0, 2, 1, 0, 1, 9};
  TASK_CHECK(attempt(goodP, 4, badRange, 6, 1) == nullptr);
  const int negative[] = {0, 2, -1, 0, 1, 3};
  TASK_CHECK(attempt(goodP, 4, negative, 6, 1) == nullptr);
  // Repeated face corner.
  const int repeated[] = {0, 0, 1, 0, 1, 3};
  TASK_CHECK(attempt(goodP, 4, repeated, 6, 1) == nullptr);
  // Duplicate face.
  const int dupFace[] = {0, 2, 1, 0, 2, 1};
  TASK_CHECK(attempt(goodP, 4, dupFace, 6, 1) == nullptr);
  // Same-oriented shared edge.
  const int sameDir[] = {0, 1, 2, 0, 1, 3};
  TASK_CHECK(attempt(goodP, 4, sameDir, 6, 1) == nullptr);
  // Non-manifold edge: three distinct faces around one edge. The first two
  // traverse it in opposite directions (so winding is fine) and the third
  // makes the edge incident to three faces.
  const float nmP[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0};
  const int nonManifold[] = {0, 1, 2, 1, 0, 3, 0, 1, 4};
  TASK_CHECK(attempt(nmP, 5, nonManifold, 9, 1) == nullptr);
  // Isolated vertex.
  const float isoP[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 5, 5, 5};
  const int isoIdx[] = {0, 1, 2};
  TASK_CHECK(attempt(isoP, 4, isoIdx, 3, 1) == nullptr);
  // Broken 1-ring: two separate triangles sharing only vertex 0.
  const float fanP[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0, 0, 0, -1, 0};
  const int fanIdx[] = {0, 1, 2, 0, 3, 4};
  TASK_CHECK(attempt(fanP, 5, fanIdx, 6, 1) == nullptr);
  // Zero-area control face.
  const float flatP[] = {0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 0, 1};
  const int flatIdx[] = {0, 1, 2, 0, 2, 3};
  TASK_CHECK(attempt(flatP, 4, flatIdx, 6, 1) == nullptr);
  // Non-finite coordinate.
  float infP[12];
  std::memcpy(infP, goodP, sizeof(goodP));
  infP[7] = std::numeric_limits<float>::infinity();
  TASK_CHECK(attempt(infP, 4, goodIdx, 12, 1) == nullptr);
  // Negative levels and levels whose result cannot be represented.
  TASK_CHECK(attempt(goodP, 4, goodIdx, 12, -1) == nullptr);
  TASK_CHECK(attempt(goodP, 4, goodIdx, 12, -100) == nullptr);
  TASK_CHECK(attempt(goodP, 4, goodIdx, 12, 100000) == nullptr);
  return 0;
}

static int run_topo_degenerate_tangent() {
  // Boundary vertex P with n=3: S = Q2-Q0 and T = Q1-P are parallel, so the
  // limit tangent plane degenerates. Both control triangles stay non-degenerate.
  const float p[] = {
    0, 0, 0,   // P  = 0
    0, 1, 0,   // Q0 = 1
    1, 0, 0,   // Q1 = 2
    2, 1, 0};  // Q2 = 3, with Q2-Q0 = 2*(Q1-P)
  const int idx[] = {0, 1, 2, 0, 2, 3};
  std::unique_ptr<LoopSubdiv> shape(new LoopSubdiv());
  shape->levels = 0;
  shape->num_points = 4;
  shape->num_indices = 6;
  shape->P = new float[12];
  shape->indices = new int[6];
  std::memcpy(shape->P, p, sizeof(p));
  std::memcpy(shape->indices, idx, sizeof(idx));
  TASK_CHECK(shape->triangle_mesh() == nullptr);
  return 0;
}

static int run_topo_source_untouched() {
  Cage cage = closedAsymmetricCage();
  cage.levels = 2;
  std::unique_ptr<LoopSubdiv> shape(makeShape(cage));
  std::vector<float> savedP(shape->P, shape->P + shape->num_points * 3);
  std::vector<int> savedIdx(shape->indices, shape->indices + shape->num_indices);
  std::unique_ptr<TriangleMesh> mesh(shape->triangle_mesh());
  TASK_CHECK(mesh != nullptr);
  // The source object is not modified.
  TASK_CHECK(shape->num_points == cage.numPoints());
  TASK_CHECK(shape->num_indices == cage.indices.size());
  TASK_CHECK(std::memcmp(shape->P, savedP.data(), savedP.size() * sizeof(float)) == 0);
  TASK_CHECK(std::memcmp(shape->indices, savedIdx.data(), savedIdx.size() * sizeof(int)) == 0);
  // Returned arrays are independently owned.
  TASK_CHECK(mesh->P != shape->P && mesh->indices != shape->indices);
  mesh->P[0] += 1.0f;
  mesh->indices[0] += 1;
  TASK_CHECK(std::memcmp(shape->P, savedP.data(), savedP.size() * sizeof(float)) == 0);
  TASK_CHECK(std::memcmp(shape->indices, savedIdx.data(), savedIdx.size() * sizeof(int)) == 0);
  return 0;
}

// ===========================================================================
// Group 2: limit geometry
// ===========================================================================

static int run_limit_closed() {
  Cage cage = closedAsymmetricCage();
  TASK_CHECK(checkControlValences(cage, true, {3, 5, 7}, {}));
  const float L = cageScale(cage);
  TASK_CHECK(L > 0.0f);
  for (int levels = 0; levels <= 2; levels++) {
    cage.levels = levels;
    std::unique_ptr<LoopSubdiv> shape(makeShape(cage));
    std::unique_ptr<TriangleMesh> mesh(shape->triangle_mesh());
    TASK_CHECK(mesh != nullptr);
    std::vector<std::array<int, 3> > refFaces;
    std::vector<RVec<double> > refPos;
    std::vector<RVec<double> > refNrm;
    referenceResult<double>(cage, levels, refPos, refNrm, refFaces);
    TASK_CHECK(compareToReference(mesh.get(), cage, 2e-5f * L, 2e-4f, refFaces));
    if (levels >= 1) {
      TASK_CHECK(checkNormalsSignConsistent(mesh.get()));
    }
    // The tetrahedron component's incident face normals only fit inside a
    // hemisphere once the cage has been refined twice.
    if (levels >= 2) {
      TASK_CHECK(checkNormalsOriented(mesh.get()));
    }
  }
  return 0;
}

static int run_limit_open() {
  Cage cage = openNonPlanarCage();
  TASK_CHECK(checkControlValences(cage, false, {}, {2, 3, 4, 5}));
  const float L = cageScale(cage);
  TASK_CHECK(L > 0.0f);
  for (int levels = 0; levels <= 2; levels++) {
    cage.levels = levels;
    std::unique_ptr<LoopSubdiv> shape(makeShape(cage));
    std::unique_ptr<TriangleMesh> mesh(shape->triangle_mesh());
    TASK_CHECK(mesh != nullptr);
    std::vector<std::array<int, 3> > refFaces;
    std::vector<RVec<double> > refPos;
    std::vector<RVec<double> > refNrm;
    referenceResult<double>(cage, levels, refPos, refNrm, refFaces);
    TASK_CHECK(compareToReference(mesh.get(), cage, 2e-5f * L, 2e-4f, refFaces));
    TASK_CHECK(checkNormalsSignConsistent(mesh.get()));
    if (levels >= 1) {
      TASK_CHECK(checkNormalsOriented(mesh.get()));
    }
  }
  return 0;
}

static int run_limit_single_fan_valences() {
  // fanCage(m) gives the center vertex boundary valence m; sweep the boundary
  // neighbor counts 2/3/4/5, including the general n>=5 tangent formula.
  for (int m = 2; m <= 5; m++) {
    Cage cage = fanCage(m, 0.0f, 0.0f, 0.0f);
    cage.levels = 2;
    TASK_CHECK(checkControlValences(cage, false, {}, {2, unsigned(m)}));
    const float L = cageScale(cage);
    std::unique_ptr<LoopSubdiv> shape(makeShape(cage));
    std::unique_ptr<TriangleMesh> mesh(shape->triangle_mesh());
    TASK_CHECK(mesh != nullptr);
    std::vector<std::array<int, 3> > refFaces;
    std::vector<RVec<double> > refPos;
    std::vector<RVec<double> > refNrm;
    referenceResult<double>(cage, 2, refPos, refNrm, refFaces);
    TASK_CHECK(compareToReference(mesh.get(), cage, 2e-5f * L, 2e-4f, refFaces));
  }
  return 0;
}

// ===========================================================================
// Group 3: scene loading
// ===========================================================================

static int run_scene_loaded_cage() {
  Loader loader;
  TASK_CHECK(loader.load("task_support/scenes/loop.pbrt"));
  std::unique_ptr<Scene> scene(loader.take_scene());
  TASK_CHECK(scene != nullptr);
  TASK_CHECK(scene->shapes.size() == 1);
  TASK_CHECK(scene->objects.size() == 1 && scene->instances.size() == 1);
  LoopSubdiv* parsed = static_cast<LoopSubdiv*>(scene->shapes[0]);
  TASK_CHECK(parsed->type() == ShapeType::LoopSubdiv);
  // P's three float components form one vertex: count units match a directly
  // constructed object.
  TASK_CHECK(parsed->num_points == 4);
  TASK_CHECK(parsed->num_indices == 12);
  TASK_CHECK(parsed->P[3] == 2.0f && parsed->P[7] == 3.0f && parsed->P[11] == 4.0f);
  TASK_CHECK(parsed->levels == 2);
  TASK_CHECK(parsed->reverseOrientation == true);
  TASK_CHECK(parsed->object == 0);
  TASK_CHECK(scene->objects[0]->firstShape == 0 && scene->objects[0]->numShapes == 1);
  TASK_CHECK(scene->instances[0]->object == 0);
  TASK_CHECK(parsed->shapeToWorld.start[0][3] == 2.0f);
  TASK_CHECK(parsed->shapeToWorld.start[1][3] == 3.0f);
  TASK_CHECK(parsed->shapeToWorld.start[2][3] == 4.0f);

  // Directly constructed counterpart with the correct vertex count.
  Cage cage;
  const float pts[] = {0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4};
  const int faces[] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
  cage.P.assign(pts, pts + 12);
  cage.indices.assign(faces, faces + 12);
  cage.levels = 2;
  std::unique_ptr<LoopSubdiv> direct(makeShape(cage));
  std::unique_ptr<TriangleMesh> directMesh(direct->triangle_mesh());
  TASK_CHECK(directMesh != nullptr);

  uint32_t material = parsed->material;
  uint32_t object = parsed->object;
  Transform xform = parsed->shapeToWorld;
  bool reversed = parsed->reverseOrientation;

  TASK_CHECK(scene->shapes_to_triangle_mesh(ShapeType::LoopSubdiv) == true);
  TriangleMesh* mesh = static_cast<TriangleMesh*>(scene->shapes[0]);
  TASK_CHECK(scene->shapes[0]->type() == ShapeType::TriangleMesh);
  TASK_CHECK(mesh->num_vertices == directMesh->num_vertices);
  TASK_CHECK(mesh->num_indices == directMesh->num_indices);
  TASK_CHECK(std::memcmp(mesh->P, directMesh->P, mesh->num_vertices * 3 * sizeof(float)) == 0);
  TASK_CHECK(std::memcmp(mesh->indices, directMesh->indices, mesh->num_indices * sizeof(int)) == 0);
  // Common properties and references survive the replacement.
  TASK_CHECK(mesh->material == material);
  TASK_CHECK(mesh->object == object);
  TASK_CHECK(mesh->reverseOrientation == reversed);
  TASK_CHECK(mesh->areaLight == kInvalidIndex);
  TASK_CHECK(mesh->insideMedium == kInvalidIndex);
  TASK_CHECK(mesh->outsideMedium == kInvalidIndex);
  TASK_CHECK(std::memcmp(&mesh->shapeToWorld, &xform, sizeof(Transform)) == 0);
  // Object shape ranges and instance references still line up.
  TASK_CHECK(scene->objects[0]->firstShape == 0 && scene->objects[0]->numShapes == 1);
  TASK_CHECK(scene->instances[0]->object == 0);
  return 0;
}

static bool writeTempFile(const std::string& path, const std::string& content) {
  std::ofstream out(path.c_str(), std::ios::trunc);
  if (!out) {
    return false;
  }
  out << content;
  return bool(out);
}

static int run_scene_bad_loader_and_retry() {
  const char* tmpDir = std::getenv("TMPDIR");
  std::string dir = (tmpDir != nullptr && tmpDir[0] != '\0') ? tmpDir : "/tmp/";
  if (dir.back() != '/') {
    dir.push_back('/');
  }
  std::string badPath = dir + "minipbrt_loop_bad.pbrt";
  std::string goodPath = dir + "minipbrt_loop_good.pbrt";

  // P with a float component count that is not a multiple of 3.
  TASK_CHECK(writeTempFile(badPath,
      "WorldBegin\n"
      "Shape \"loopsubdiv\" \"integer indices\" [0 1 2]\n"
      "  \"point P\" [0 0 0  1 0 0  0 1]\n"
      "WorldEnd\n"));
  {
    Loader loader;
    TASK_CHECK(loader.load(badPath.c_str()) == false);
    TASK_CHECK(loader.error() != nullptr && loader.error()->message() != nullptr);
    TASK_CHECK(loader.take_scene() == nullptr);
  }

  // A loader that failed can be replaced by a fresh successful load.
  TASK_CHECK(writeTempFile(goodPath,
      "WorldBegin\n"
      "Shape \"loopsubdiv\" \"integer levels\" [1]\n"
      "  \"integer indices\" [0 1 2]\n"
      "  \"point P\" [0 0 0  1 0 0  0 1 0]\n"
      "WorldEnd\n"));
  {
    Loader loader;
    TASK_CHECK(loader.load(goodPath.c_str()) == true);
    std::unique_ptr<Scene> scene(loader.take_scene());
    TASK_CHECK(scene && scene->shapes.size() == 1);
    const LoopSubdiv* parsed = static_cast<const LoopSubdiv*>(scene->shapes[0]);
    TASK_CHECK(parsed->num_points == 3);
    TASK_CHECK(scene->shapes_to_triangle_mesh(ShapeType::LoopSubdiv) == true);
    TASK_CHECK(scene->shapes[0]->type() == ShapeType::TriangleMesh);
  }

  // A failed scene conversion keeps the original shape and the source arrays,
  // and a later conversion of the same retained shape still works.
  Scene scene;
  Cage good = tetraCage(0.0f, 0.0f, 0.0f);
  good.levels = 1;
  scene.shapes.push_back(makeShape(good));
  const int brokenIdx[] = {0, 1, 2, 0, 1};
  LoopSubdiv* broken = new LoopSubdiv();
  broken->levels = 1;
  broken->num_points = 4;
  broken->num_indices = 5;
  broken->P = new float[12];
  broken->indices = new int[5];
  std::memcpy(broken->P, good.P.data(), 12 * sizeof(float));
  std::memcpy(broken->indices, brokenIdx, sizeof(brokenIdx));
  scene.shapes.push_back(broken);

  TASK_CHECK(scene.to_triangle_mesh(1) == false);
  TASK_CHECK(scene.shapes[1] == broken); // original shape retained
  TASK_CHECK(scene.shapes[1]->type() == ShapeType::LoopSubdiv);
  TASK_CHECK(broken->num_indices == 5);
  TASK_CHECK(std::memcmp(broken->indices, brokenIdx, sizeof(brokenIdx)) == 0);

  // Repair the shape in place, then convert again.
  int* fixed = new int[12];
  const int fullFaces[] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
  std::memcpy(fixed, fullFaces, sizeof(fullFaces));
  delete[] broken->indices;
  broken->indices = fixed;
  broken->num_indices = 12;
  TASK_CHECK(scene.to_triangle_mesh(1) == true);
  TASK_CHECK(scene.shapes[1]->type() == ShapeType::TriangleMesh);
  // The independently valid shape converts too.
  TASK_CHECK(scene.to_triangle_mesh(0) == true);
  TASK_CHECK(scene.shapes[0]->type() == ShapeType::TriangleMesh);
  return 0;
}

static int run_scene_batch_paths() {
  auto buildScene = [](Scene& scene) {
    Cage goodA = tetraCage(0.0f, 0.0f, 0.0f);
    goodA.levels = 1;
    Cage goodB = icosaCage(9.0f, 0.0f, 0.0f);
    goodB.levels = 1;
    scene.shapes.push_back(makeShape(goodA));
    LoopSubdiv* bad = new LoopSubdiv();
    bad->levels = 1;
    bad->num_points = 4;
    bad->num_indices = 5;
    bad->P = new float[12];
    bad->indices = new int[5];
    const float pts[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    const int brokenIdx[] = {0, 1, 2, 0, 1};
    std::memcpy(bad->P, pts, sizeof(pts));
    std::memcpy(bad->indices, brokenIdx, sizeof(brokenIdx));
    scene.shapes.push_back(bad);
    scene.shapes.push_back(makeShape(goodB));
  };

  // Stop on first error: the shape after the failing one is never visited.
  {
    Scene scene;
    buildScene(scene);
    TASK_CHECK(scene.shapes_to_triangle_mesh(ShapeType::LoopSubdiv, true) == false);
    TASK_CHECK(scene.shapes[0]->type() == ShapeType::TriangleMesh);
    TASK_CHECK(scene.shapes[1]->type() == ShapeType::LoopSubdiv);
    TASK_CHECK(scene.shapes[2]->type() == ShapeType::LoopSubdiv);
  }
  // Continue past errors: every convertible shape is converted.
  {
    Scene scene;
    buildScene(scene);
    TASK_CHECK(scene.shapes_to_triangle_mesh(ShapeType::LoopSubdiv, false) == false);
    TASK_CHECK(scene.shapes[0]->type() == ShapeType::TriangleMesh);
    TASK_CHECK(scene.shapes[1]->type() == ShapeType::LoopSubdiv);
    TASK_CHECK(scene.shapes[2]->type() == ShapeType::TriangleMesh);
  }
  return 0;
}

static int run_scene_other_shapes_untouched() {
  Scene scene;
  Cage cage = tetraCage(0.0f, 0.0f, 0.0f);
  cage.levels = 1;
  scene.shapes.push_back(makeShape(cage));
  TriangleMesh* plain = new TriangleMesh();
  plain->num_vertices = 3;
  plain->num_indices = 3;
  plain->P = new float[9];
  plain->indices = new int[3]{0, 1, 2};
  const float triP[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::memcpy(plain->P, triP, sizeof(triP));
  Shape* plainPtr = plain;
  scene.shapes.push_back(plain);

  TASK_CHECK(scene.shapes_to_triangle_mesh(ShapeType::LoopSubdiv) == true);
  TASK_CHECK(scene.shapes[0]->type() == ShapeType::TriangleMesh);
  // The existing triangle mesh is not a LoopSubdiv and must be left alone.
  TASK_CHECK(scene.shapes[1] == plainPtr);
  TASK_CHECK(scene.shapes[1]->type() == ShapeType::TriangleMesh);
  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Registration (no custom main: task_support/driver.c drives these).
// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
  {"topology", "closed_cages", run_topo_closed},
  {"topology", "open_fan", run_topo_open_fan},
  {"topology", "coincident_components_isolated", run_topo_coincident_isolation},
  {"topology", "invalid_inputs_rejected", run_topo_rejects},
  {"topology", "degenerate_tangent_rejected", run_topo_degenerate_tangent},
  {"topology", "source_object_untouched", run_topo_source_untouched},

  {"limit_geometry", "closed_cage_vs_double_reference", run_limit_closed},
  {"limit_geometry", "open_fan_vs_double_reference", run_limit_open},
  {"limit_geometry", "boundary_valences_2_to_5", run_limit_single_fan_valences},

  {"scene_loading", "loaded_cage_conversion", run_scene_loaded_cage},
  {"scene_loading", "bad_loader_retry_and_failed_conversion", run_scene_bad_loader_and_retry},
  {"scene_loading", "batch_stop_and_continue", run_scene_batch_paths},
  {"scene_loading", "non_loop_shapes_untouched", run_scene_other_shapes_untouched},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
