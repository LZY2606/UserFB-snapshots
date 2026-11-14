// =============================================================================
// task_tests/geodesic_network.cpp
//
// Standalone regression tests for geometry-central's FlipEdgeNetwork
// (flip-based geodesic shortening on intrinsic triangulations), focused on
//
//   chart   -- shortening on a non-planar developable surface (a flat 2D
//              triangle atlas hinged into 3D along mesh edges, preserving all
//              intrinsic edge lengths), verified against an independent 2D
//              unfolding: the exact geodesic is a straight line in the atlas.
//   network -- a planar triangulated region with a hole: two initial paths in
//              different homotopy classes must keep their class and stay out
//              of the hole; class lengths come from a hand-written 2D
//              segment-visibility + polygon-corner shortest path solver.
//              Also: piecewise paths with intermediate marked points must not
//              be jumped over by the shortening of a neighboring segment.
//   rewind  -- shorten / rewind() / reinitializePath() / shorten again on one
//              network with supportRewinding=true: initial connectivity, edge
//              lengths, original-edge flags and path occupancy must be
//              restored, and a reused network must produce the same surface
//              path as a brand-new network on the same input.
//
// All expected values are derived independently of the library: closed-form
// Euclidean expressions in the 2D atlas, a hand-written line/triangulation
// walk, and a hand-written visibility-graph solver. The library's length(),
// its tracing routines, and the current intrinsic triangulation are never
// used to *generate* expectations -- they only appear on the checked side of
// an assertion.
//
// Mesh scale: all coordinates are O(1)..O(6), edge lengths in [0.25, sqrt(2)].
// Tolerances below are set accordingly (TOL = 1e-6 absolute); the diagnostic
// defects in task_tests/defects/ shift results by ~1e-2 or more, far outside
// these tolerances, so each defect is caught by content assertions (not by
// crashes or compile errors).
//
// Cases are registered through task_support/check.h (task_cases /
// task_case_count); a case returns 0 on success and 1 on the first failed
// TASK_CHECK.
// =============================================================================

// NOTE: task_support/driver.c is compiled as C++ (see task_support/run.sh),
// so task_check/task_cases/task_case_count use plain C++ linkage here.
#include "task_support/check.h"

#include "geometrycentral/surface/flip_geodesics.h"
#include "geometrycentral/surface/mesh_graph_algorithms.h"
#include "geometrycentral/surface/surface_mesh_factories.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace geometrycentral;
using namespace geometrycentral::surface;

namespace {

// Absolute tolerance for positions/lengths at the generated mesh scale.
const double TOL = 1e-6;
// rewind() restores recorded values bit-exactly, so the post-rewind metric
// comparison can be much tighter than the geometric one.
const double TOL_EXACT = 1e-9;

// ---------------------------------------------------------------------------
// Minimal 2D vector type for the independent geometric derivations.
// ---------------------------------------------------------------------------
struct P2 {
  double x = 0., y = 0.;
};
P2 operator+(P2 a, P2 b) { return {a.x + b.x, a.y + b.y}; }
P2 operator-(P2 a, P2 b) { return {a.x - b.x, a.y - b.y}; }
P2 operator*(P2 a, double s) { return {a.x * s, a.y * s}; }
double dot2(P2 a, P2 b) { return a.x * b.x + a.y * b.y; }
double cross2(P2 a, P2 b) { return a.x * b.y - a.y * b.x; }
double norm2(P2 a) { return std::hypot(a.x, a.y); }
double dist2(P2 a, P2 b) { return norm2(a - b); }

// ---------------------------------------------------------------------------
// Independent interpolation of a retraced SurfacePoint from raw vertex data.
// Vertex, edge and face points are each interpreted according to their own
// meaning (never as "the same index on some new mesh"):
//   Vertex -> position of that input vertex
//   Edge   -> lerp along the input edge, t measured from the tail of
//             edge.halfedge() (geometry-central's SurfacePoint convention)
//   Face   -> barycentric weights ordered along the face halfedge cycle
// ---------------------------------------------------------------------------
template <typename V>
V interpSP(const SurfacePoint& sp, const std::vector<V>& pos) {
  switch (sp.type) {
  case SurfacePointType::Vertex:
    return pos[sp.vertex.getIndex()];
  case SurfacePointType::Edge: {
    Halfedge he = sp.edge.halfedge();
    const V& a = pos[he.tailVertex().getIndex()];
    const V& b = pos[he.tipVertex().getIndex()];
    return a * (1. - sp.tEdge) + b * sp.tEdge;
  }
  case SurfacePointType::Face: {
    Halfedge he = sp.face.halfedge();
    const V& a = pos[he.vertex().getIndex()];
    he = he.next();
    const V& b = pos[he.vertex().getIndex()];
    he = he.next();
    const V& c = pos[he.vertex().getIndex()];
    return a * sp.faceCoords.x + b * sp.faceCoords.y + c * sp.faceCoords.z;
  }
  }
  return V();
}

// ---------------------------------------------------------------------------
// Generated mesh data: positions in 3D, positions in the 2D atlas (the
// unfolding), the face list, and the unique undirected edges of the atlas.
// ---------------------------------------------------------------------------
struct GeneratedMesh {
  std::vector<Vector3> pos3;
  std::vector<P2> pos2;
  std::vector<std::vector<size_t>> faces;
  std::vector<std::array<size_t, 2>> edges;
};

std::vector<std::array<size_t, 2>> uniqueEdges(const std::vector<std::vector<size_t>>& faces) {
  std::vector<std::array<size_t, 2>> edges;
  for (const auto& f : faces) {
    for (int k = 0; k < 3; k++) {
      size_t a = f[k], b = f[(k + 1) % 3];
      if (a > b) std::swap(a, b);
      edges.push_back({a, b});
    }
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
  return edges;
}

// --- the folded strip -------------------------------------------------------
// A 6 x 2 rectangle of unit squares, triangulated with "/" diagonals, hinged
// into 3D along the vertical grid line x = 3 by a 60-degree fold. Every face
// keeps its intrinsic edge lengths exactly (each side of the fold is rigidly
// rotated), so the 2D atlas IS the intrinsic unfolding of the surface.
const size_t STRIP_NX = 7, STRIP_NY = 3; // vertices: x in 0..6, y in 0..2
const double STRIP_FOLD_X = 3.0;

Vector3 stripFold3D(P2 p) {
  if (p.x <= STRIP_FOLD_X) return {p.x, p.y, 0.};
  double d = p.x - STRIP_FOLD_X;
  return {STRIP_FOLD_X + 0.5 * d, p.y, std::sqrt(3.) * 0.5 * d};
}

GeneratedMesh buildFoldedStrip() {
  GeneratedMesh g;
  for (size_t y = 0; y < STRIP_NY; y++) {
    for (size_t x = 0; x < STRIP_NX; x++) {
      P2 p{double(x), double(y)};
      g.pos2.push_back(p);
      g.pos3.push_back(stripFold3D(p));
    }
  }
  for (size_t y = 0; y + 1 < STRIP_NY; y++) {
    for (size_t x = 0; x + 1 < STRIP_NX; x++) {
      size_t a = y * STRIP_NX + x;
      g.faces.push_back({a, a + 1, a + STRIP_NX + 1});
      g.faces.push_back({a, a + STRIP_NX + 1, a + STRIP_NX});
    }
  }
  g.edges = uniqueEdges(g.faces);
  return g;
}

size_t stripVid(size_t x, size_t y) { return y * STRIP_NX + x; }

// --- the planar domain with a hole ------------------------------------------
// [0,4]^2 sampled on a 0.25 grid, with the axis-aligned square hole
// (1.5,2.5) x (1.25,2.25) removed; hole boundary follows mesh edges.
struct HoleDomain {
  GeneratedMesh g;
  size_t idxS = 0, idxE = 0;                       // shared endpoints (0,2) and (4,2)
  std::vector<size_t> pathAbove, pathBelow;        // initial edge paths, vertex indices
  std::vector<P2> hole;                            // hole corners, CCW
  P2 holeCenter;
};

HoleDomain buildHoleDomain() {
  const size_t n = 17; // vertices per side
  const double h = 0.25;
  auto gvid = [&](size_t i, size_t j) { return j * n + i; };
  // hole cells: x in [1.5,2.5], y in [1.25,2.25]
  auto inHole = [&](size_t i, size_t j) { return i >= 6 && i <= 9 && j >= 5 && j <= 8; };

  HoleDomain D;
  std::vector<std::vector<size_t>> facesG;
  std::vector<bool> used(n * n, false);
  for (size_t j = 0; j + 1 < n; j++) {
    for (size_t i = 0; i + 1 < n; i++) {
      if (inHole(i, j)) continue;
      size_t a = gvid(i, j);
      facesG.push_back({a, a + 1, a + n + 1});
      facesG.push_back({a, a + n + 1, a + n});
      used[a] = used[a + 1] = used[a + n + 1] = used[a + n] = true;
    }
  }
  std::vector<int> remap(n * n, -1);
  for (size_t v = 0; v < n * n; v++) {
    if (used[v]) {
      remap[v] = (int)D.g.pos3.size();
      P2 p{double(v % n) * h, double(v / n) * h};
      D.g.pos2.push_back(p);
      D.g.pos3.push_back({p.x, p.y, 0.});
    }
  }
  for (auto& f : facesG) D.g.faces.push_back({(size_t)remap[f[0]], (size_t)remap[f[1]], (size_t)remap[f[2]]});
  D.g.edges = uniqueEdges(D.g.faces);
  auto vid = [&](size_t i, size_t j) { return (size_t)remap[gvid(i, j)]; };

  D.idxS = vid(0, 8);  // (0,2)
  D.idxE = vid(16, 8); // (4,2)
  // path above the hole: (0,2) -> (0,3.5) -> (4,3.5) -> (4,2)
  for (size_t j = 8; j <= 14; j++) D.pathAbove.push_back(vid(0, j));
  for (size_t i = 1; i <= 16; i++) D.pathAbove.push_back(vid(i, 14));
  for (size_t j = 13; j + 1 > 8; j--) D.pathAbove.push_back(vid(16, j)); // j down to 8
  // path below the hole: (0,2) -> (0,0.5) -> (4,0.5) -> (4,2)
  for (size_t j = 8; j + 1 > 2; j--) D.pathBelow.push_back(vid(0, j)); // j down to 2
  for (size_t i = 1; i <= 16; i++) D.pathBelow.push_back(vid(i, 2));
  for (size_t j = 3; j <= 8; j++) D.pathBelow.push_back(vid(16, j));

  D.hole = {{1.5, 1.25}, {2.5, 1.25}, {2.5, 2.25}, {1.5, 2.25}}; // CCW
  D.holeCenter = {2.0, 1.75};
  return D;
}

// --- the planar grid for the marked-point piecewise test --------------------
struct MarkGrid {
  GeneratedMesh g;
  size_t idxP0 = 0, idxM = 0, idxP2 = 0;
  P2 p0, m, p2;
};

MarkGrid buildMarkGrid() {
  // [-0.5,2.5] x [-0.5,1.5] sampled at 0.25
  const size_t nx = 13, ny = 9;
  const double h = 0.25;
  MarkGrid D;
  for (size_t j = 0; j < ny; j++) {
    for (size_t i = 0; i < nx; i++) {
      P2 p{-0.5 + i * h, -0.5 + j * h};
      D.g.pos2.push_back(p);
      D.g.pos3.push_back({p.x, p.y, 0.});
    }
  }
  auto vid = [&](size_t i, size_t j) { return j * nx + i; };
  for (size_t j = 0; j + 1 < ny; j++) {
    for (size_t i = 0; i + 1 < nx; i++) {
      size_t a = vid(i, j);
      D.g.faces.push_back({a, a + 1, a + nx + 1});
      D.g.faces.push_back({a, a + nx + 1, a + nx});
    }
  }
  D.g.edges = uniqueEdges(D.g.faces);
  D.idxP0 = vid(2, 3);   // (0.0, 0.25)
  D.idxM = vid(6, 6);    // (1.0, 1.0)
  D.idxP2 = vid(10, 3);  // (2.0, 0.25)
  D.p0 = D.g.pos2[D.idxP0];
  D.m = D.g.pos2[D.idxM];
  D.p2 = D.g.pos2[D.idxP2];
  return D;
}

// ---------------------------------------------------------------------------
// Build a halfedge path from consecutive vertex indices (construction helper
// only; no geometric content is taken from the library).
// ---------------------------------------------------------------------------
std::vector<Halfedge> edgePathFromIndices(ManifoldSurfaceMesh& mesh, const std::vector<size_t>& idx) {
  std::vector<Halfedge> hes;
  for (size_t k = 1; k < idx.size(); k++) {
    bool ok = false;
    for (Halfedge h : mesh.vertex(idx[k - 1]).outgoingHalfedges()) {
      if (h.tipVertex() == mesh.vertex(idx[k])) {
        hes.push_back(h);
        ok = true;
        break;
      }
    }
    if (!ok) return {};
  }
  return hes;
}

// ---------------------------------------------------------------------------
// Independent 2D line/triangulation walk: proper crossings of the segment
// p0->p1 with the atlas edges, sorted along the line. Endpoints of the line
// and exact vertex hits are excluded (our generated lines hit no vertices).
// ---------------------------------------------------------------------------
struct Crossing {
  std::array<size_t, 2> edge;
  double tEdge; // along edge[0] -> edge[1]
  double s;     // along p0 -> p1
  P2 pos;
};

std::vector<Crossing> lineCrossings(const GeneratedMesh& g, P2 p0, P2 p1) {
  std::vector<Crossing> out;
  P2 d = p1 - p0;
  for (const auto& e : g.edges) {
    P2 a = g.pos2[e[0]], b = g.pos2[e[1]];
    P2 u = b - a;
    double denom = cross2(d, u);
    if (std::fabs(denom) < 1e-12) continue; // parallel: no proper crossing
    P2 w = a - p0;
    double s = cross2(w, u) / denom;
    double t = cross2(w, d) / denom;
    if (s <= 1e-9 || s >= 1. - 1e-9 || t <= 1e-9 || t >= 1. - 1e-9) continue;
    out.push_back({e, t, s, p0 + d * s});
  }
  std::sort(out.begin(), out.end(), [](const Crossing& a, const Crossing& b) { return a.s < b.s; });
  return out;
}

// ---------------------------------------------------------------------------
// Winding number of a closed loop (implicitly closed last->first) around c.
// ---------------------------------------------------------------------------
double windingNumber(const std::vector<P2>& loop, P2 c) {
  double sum = 0.;
  for (size_t i = 0; i < loop.size(); i++) {
    P2 a = loop[i] - c;
    P2 b = loop[(i + 1) % loop.size()] - c;
    sum += std::atan2(cross2(a, b), dot2(a, b));
  }
  return sum / (2. * M_PI);
}

// ---------------------------------------------------------------------------
// Rectangle interior tests (the hole). Boundary contact is allowed; only
// proper interior crossings count.
// ---------------------------------------------------------------------------
struct Rect {
  double x0, x1, y0, y1;
};
bool strictlyInsideRect(P2 p, Rect r) {
  const double eps = 1e-9;
  return p.x > r.x0 + eps && p.x < r.x1 - eps && p.y > r.y0 + eps && p.y < r.y1 - eps;
}
double orient2(P2 a, P2 b, P2 c) { return cross2(b - a, c - a); }
bool properSegIntersect(P2 a, P2 b, P2 c, P2 d) {
  return orient2(a, b, c) * orient2(a, b, d) < 0. && orient2(c, d, a) * orient2(c, d, b) < 0.;
}
std::array<P2, 4> rectCorners(Rect r) { return {P2{r.x0, r.y0}, P2{r.x1, r.y0}, P2{r.x1, r.y1}, P2{r.x0, r.y1}}; }
bool segmentCrossesRectInterior(P2 p, P2 q, Rect r) {
  if (strictlyInsideRect(p, r) || strictlyInsideRect(q, r)) return true;
  if (strictlyInsideRect((p + q) * 0.5, r)) return true;
  auto c = rectCorners(r);
  for (int k = 0; k < 4; k++) {
    if (properSegIntersect(p, q, c[k], c[(k + 1) % 4])) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Hand-written shortest path in the plane minus a convex polygonal hole,
// constrained to a homotopy class. In a polygonal domain the taut path is a
// polygonal chain whose interior vertices are hole vertices consecutive in
// cyclic order; we enumerate all such chains (both orientations), keep those
// whose links are visible (segment-visibility test against the hole), and bin
// them by the winding class of the loop closed with the fixed reference
// segment e -> s. This is the independent derivation of the class lengths.
// ---------------------------------------------------------------------------
double holeChainShortest(const std::vector<P2>& hole, P2 center, P2 s, P2 e, double targetWinding) {
  const size_t n = hole.size();
  Rect r{hole[0].x, hole[1].x, hole[0].y, hole[2].y}; // axis-aligned hole
  auto visible = [&](P2 a, P2 b) { return !segmentCrossesRectInterior(a, b, r); };

  double best = std::numeric_limits<double>::infinity();
  for (int dir = -1; dir <= 1; dir += 2) {
    for (size_t start = 0; start < n; start++) {
      for (size_t k = 0; k <= n; k++) {
        std::vector<P2> pts{s};
        for (size_t q = 0; q < k; q++) pts.push_back(hole[(start + n + dir * (ptrdiff_t)q) % n]);
        pts.push_back(e);
        bool ok = true;
        double len = 0.;
        for (size_t q = 1; q < pts.size() && ok; q++) {
          ok = ok && visible(pts[q - 1], pts[q]);
          len += dist2(pts[q - 1], pts[q]);
        }
        if (!ok) continue;
        double w = windingNumber(pts, center);
        if (std::llround(w) == std::llround(targetWinding)) best = std::fmin(best, len);
      }
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// Map an intrinsic vertex of a network back to the input vertex index.
// (No refinement is used anywhere in these tests, so locations are vertices.)
// ---------------------------------------------------------------------------
size_t inputIndexOf(FlipEdgeNetwork& net, Vertex v) {
  SurfacePoint loc = net.tri->vertexLocations[v];
  if (loc.type != SurfacePointType::Vertex) return SIZE_MAX;
  return loc.vertex.getIndex();
}

size_t countMarked(FlipEdgeNetwork& net) {
  size_t n = 0;
  for (Vertex v : net.mesh.vertices())
    if (net.isMarkedVertex[v]) n++;
  return n;
}

// =============================================================================
// chart group: non-planar developable strip vs. its 2D unfolding
// =============================================================================
int caseChartFoldedStrip() {
  GeneratedMesh D = buildFoldedStrip();
  auto mpGp = makeManifoldSurfaceMeshAndGeometry(D.faces, D.pos3);
  ManifoldSurfaceMesh& mesh = *std::get<0>(mpGp);
  VertexPositionGeometry& geom = *std::get<1>(mpGp);

  // -- self-checks on the generated data: the fold is an exact isometry of
  //    every atlas edge, and the surface is genuinely non-planar.
  for (const auto& e : D.edges) {
    double l3 = (D.pos3[e[0]] - D.pos3[e[1]]).norm();
    double l2 = dist2(D.pos2[e[0]], D.pos2[e[1]]);
    TASK_CHECK(std::fabs(l3 - l2) < TOL);
  }
  double maxAbsZ = 0.;
  for (const auto& p : D.pos3) maxAbsZ = std::fmax(maxAbsZ, std::fabs(p.z));
  TASK_CHECK(maxAbsZ > 0.5);

  // -- initial edge path (0,0) -> (6,1) with genuine corners; vertex indices
  //    (0,0)=0 ... staircase ... (6,1)=13.
  const std::vector<size_t> pathIdx = {0, 1, 8, 9, 16, 17, 18, 11, 12, 13};
  std::vector<Halfedge> hes = edgePathFromIndices(mesh, pathIdx);
  TASK_CHECK(hes.size() + 1 == pathIdx.size());

  FlipEdgeNetwork net(mesh, geom, {hes});
  net.posGeom = &geom;

  // -- independent expectations, derived only from the 2D atlas.
  const P2 A{0., 0.}, B{6., 1.};
  const double expectLen = dist2(A, B); // sqrt(37) = 6.0827625...
  double initLen2D = 0.;
  for (size_t k = 1; k < pathIdx.size(); k++) initLen2D += dist2(D.pos2[pathIdx[k - 1]], D.pos2[pathIdx[k]]);
  const double chord3D = (D.pos3[pathIdx.front()] - D.pos3[pathIdx.back()]).norm();
  TASK_CHECK(std::fabs(initLen2D - 9.0) < TOL);          // 9 unit atlas edges
  TASK_CHECK(std::fabs(expectLen - chord3D) > 0.1);      // surface length != 3D chord
  TASK_CHECK(std::fabs(net.length() - initLen2D) < TOL); // library agrees before shortening

  // -- the initial path must have a genuinely shortenable corner
  TASK_CHECK(net.minAngle() < M_PI - 0.5);

  // -- stepwise shortening: length must be monotone non-increasing
  std::vector<double> stepLens{net.length()};
  size_t steps = 0;
  while (!net.wedgeAngleQueue.empty() && steps < 100000) {
    net.iterativeShorten(1);
    stepLens.push_back(net.length());
    steps++;
  }
  TASK_CHECK(steps > 0);
  for (size_t k = 1; k < stepLens.size(); k++) TASK_CHECK(stepLens[k] <= stepLens[k - 1] + 1e-10);

  // -- run to convergence
  net.iterativeShorten();
  TASK_CHECK(net.wedgeAngleQueue.empty());
  TASK_CHECK(net.nFlips > 0);        // the shortening really flipped edges
  TASK_CHECK(net.nShortenIters > 0); // and really straightened wedges
  TASK_CHECK(net.minAngle() >= M_PI - 1e-6);

  // -- final length: surface length in the atlas, not the 3D chord
  const double finalLen = net.length();
  TASK_CHECK(finalLen < stepLens.front() - 0.5);
  TASK_CHECK(std::fabs(finalLen - expectLen) < TOL);
  TASK_CHECK(std::fabs(finalLen - chord3D) > 0.1);

  // -- endpoints preserved
  std::vector<std::vector<SurfacePoint>> poly = net.getPathPolyline();
  TASK_CHECK(poly.size() == 1);
  TASK_CHECK(poly[0].front().type == SurfacePointType::Vertex && poly[0].front().vertex.getIndex() == pathIdx.front());
  TASK_CHECK(poly[0].back().type == SurfacePointType::Vertex && poly[0].back().vertex.getIndex() == pathIdx.back());
  TASK_CHECK(countMarked(net) == 2);
  TASK_CHECK(net.isMarkedVertex[net.mesh.vertex(pathIdx.front())]);
  TASK_CHECK(net.isMarkedVertex[net.mesh.vertex(pathIdx.back())]);

  // -- retraced intersections in the original triangle atlas: the geodesic is
  //    the straight atlas line A->B; independently walk the atlas edges.
  //    (Hand-computed: 5 vertical + 4 diagonal proper crossings.)
  std::vector<Crossing> crossings = lineCrossings(D, A, B);
  TASK_CHECK(crossings.size() == 9);
  TASK_CHECK(poly[0].size() == crossings.size() + 2);
  if (poly[0].size() != crossings.size() + 2) return 1;

  double prevS = 0.;
  for (size_t i = 1; i + 1 < poly[0].size(); i++) {
    const SurfacePoint& sp = poly[0][i];
    const Crossing& cx = crossings[i - 1];
    TASK_CHECK(sp.type == SurfacePointType::Edge);
    if (sp.type != SurfacePointType::Edge) return 1;
    // same atlas edge (unordered endpoint pair)
    Halfedge he = sp.edge.halfedge();
    std::array<size_t, 2> got{he.tailVertex().getIndex(), he.tipVertex().getIndex()};
    if (got[0] > got[1]) std::swap(got[0], got[1]);
    TASK_CHECK(got == cx.edge);
    // same parameter along the edge, in the edge's own orientation
    double tExpect = (he.tailVertex().getIndex() == cx.edge[0]) ? cx.tEdge : 1. - cx.tEdge;
    TASK_CHECK(std::fabs(sp.tEdge - tExpect) < TOL);
    // actual position in the atlas, and order along the path
    P2 p = interpSP(sp, D.pos2);
    TASK_CHECK(dist2(p, cx.pos) < TOL);
    double s = dot2(p - A, B - A) / dot2(B - A, B - A);
    TASK_CHECK(s > prevS && s < 1.);
    prevS = s;
  }

  // -- 3D positions: independent interpolation of the SurfacePoints with the
  //    original vertex positions must match getPathPolyline3D pointwise.
  std::vector<std::vector<Vector3>> poly3 = net.getPathPolyline3D();
  TASK_CHECK(poly3.size() == 1 && poly3[0].size() == poly[0].size());
  if (poly3.size() != 1 || poly3[0].size() != poly[0].size()) return 1;
  for (size_t i = 0; i < poly[0].size(); i++) {
    Vector3 expect3 = interpSP(poly[0][i], D.pos3);
    TASK_CHECK((poly3[0][i] - expect3).norm() < TOL);
  }
  // -- and they must equal the folded images of the atlas crossings
  TASK_CHECK((poly3[0].front() - stripFold3D(A)).norm() < TOL);
  for (size_t i = 0; i < crossings.size(); i++) {
    TASK_CHECK((poly3[0][i + 1] - stripFold3D(crossings[i].pos)).norm() < TOL);
  }
  TASK_CHECK((poly3[0].back() - stripFold3D(B)).norm() < TOL);

  // -- the 3D polyline is genuinely kinked at the fold: it is not the chord
  Vector3 chordDir = D.pos3[pathIdx.back()] - D.pos3[pathIdx.front()];
  double maxDev = 0.;
  for (const Vector3& q : poly3[0]) {
    Vector3 w = q - D.pos3[pathIdx.front()];
    double dev = (w - chordDir * (dot(w, chordDir) / dot(chordDir, chordDir))).norm();
    maxDev = std::fmax(maxDev, dev);
  }
  TASK_CHECK(maxDev > 0.1);

  return 0;
}

// =============================================================================
// network group: planar domain with a hole, homotopy classes
// =============================================================================
int caseNetworkHoleClasses() {
  HoleDomain D = buildHoleDomain();
  auto mpGp = makeManifoldSurfaceMeshAndGeometry(D.g.faces, D.g.pos3);
  ManifoldSurfaceMesh& mesh = *std::get<0>(mpGp);
  VertexPositionGeometry& geom = *std::get<1>(mpGp);

  // -- self-checks on the generated data
  TASK_CHECK(mesh.nBoundaryLoops() == 2); // outer boundary + hole
  for (const auto& p : D.g.pos3) TASK_CHECK(p.z == 0.);

  // -- homotopy classes of the two initial paths (loop closed with the fixed
  //    reference segment E -> S; the difference of windings is class)
  std::vector<P2> initAbove, initBelow;
  for (size_t v : D.pathAbove) initAbove.push_back(D.g.pos2[v]);
  for (size_t v : D.pathBelow) initBelow.push_back(D.g.pos2[v]);
  const double wAbove = windingNumber(initAbove, D.holeCenter);
  const double wBelow = windingNumber(initBelow, D.holeCenter);
  TASK_CHECK(std::llround(wAbove) != std::llround(wBelow));

  // -- independent class lengths: hand-written visibility + corner chains
  const double lenAbove = holeChainShortest(D.hole, D.holeCenter, D.g.pos2[D.idxS], D.g.pos2[D.idxE], wAbove);
  const double lenBelow = holeChainShortest(D.hole, D.holeCenter, D.g.pos2[D.idxS], D.g.pos2[D.idxE], wBelow);
  // closed-form cross-check of the solver itself:
  //   above: (0,2)->(1.5,2.25)->(2.5,2.25)->(4,2) = 2*sqrt(2.3125) + 1
  //   below: (0,2)->(1.5,1.25)->(2.5,1.25)->(4,2) = 2*sqrt(2.8125) + 1
  TASK_CHECK(std::fabs(lenAbove - (2. * std::sqrt(2.3125) + 1.)) < TOL);
  TASK_CHECK(std::fabs(lenBelow - (2. * std::sqrt(2.8125) + 1.)) < TOL);

  // -- one network holding both initial paths
  std::vector<Halfedge> hesA = edgePathFromIndices(mesh, D.pathAbove);
  std::vector<Halfedge> hesB = edgePathFromIndices(mesh, D.pathBelow);
  TASK_CHECK(hesA.size() + 1 == D.pathAbove.size());
  TASK_CHECK(hesB.size() + 1 == D.pathBelow.size());
  FlipEdgeNetwork net(mesh, geom, {hesA, hesB});
  net.posGeom = &geom;
  TASK_CHECK(net.paths.size() == 2);
  const double initLen = net.length();
  TASK_CHECK(std::fabs(initLen - 14.0) < TOL); // 2 x 28 grid edges of 0.25

  net.iterativeShorten();
  TASK_CHECK(net.nFlips > 0);
  TASK_CHECK(net.paths.size() == 2);
  TASK_CHECK(net.length() < initLen - 0.5);
  TASK_CHECK(std::fabs(net.length() - (lenAbove + lenBelow)) < TOL);

  std::vector<std::vector<SurfacePoint>> poly = net.getPathPolyline();
  TASK_CHECK(poly.size() == 2);
  if (poly.size() != 2) return 1;

  const Rect holeRect{1.5, 2.5, 1.25, 2.25};
  const double expectLen[2] = {lenAbove, lenBelow};
  const double expectW[2] = {wAbove, wBelow};
  for (int k = 0; k < 2; k++) {
    const std::vector<SurfacePoint>& line = poly[k];
    // endpoints preserved
    TASK_CHECK(line.front().type == SurfacePointType::Vertex && line.front().vertex.getIndex() == D.idxS);
    TASK_CHECK(line.back().type == SurfacePointType::Vertex && line.back().vertex.getIndex() == D.idxE);
    // retraced positions in the plane
    std::vector<P2> pts;
    for (const SurfacePoint& sp : line) pts.push_back(interpSP(sp, D.g.pos2));
    // homotopy class preserved
    double w = windingNumber(pts, D.holeCenter);
    TASK_CHECK(std::llround(w) == std::llround(expectW[k]));
    // independent surface length of the retraced polyline
    double len = 0.;
    for (size_t i = 1; i < pts.size(); i++) len += dist2(pts[i - 1], pts[i]);
    TASK_CHECK(std::fabs(len - expectLen[k]) < TOL);
    // stays in the domain and never crosses the hole interior
    for (const P2& p : pts) {
      TASK_CHECK(p.x > -TOL && p.x < 4. + TOL && p.y > -TOL && p.y < 4. + TOL);
      TASK_CHECK(!strictlyInsideRect(p, holeRect));
    }
    for (size_t i = 1; i < pts.size(); i++) TASK_CHECK(!segmentCrossesRectInterior(pts[i - 1], pts[i], holeRect));
  }

  return 0;
}

// =============================================================================
// network group: piecewise paths with intermediate marked points
// =============================================================================
int caseNetworkMarkedPiecewise() {
  MarkGrid D = buildMarkGrid();
  auto mpGp = makeManifoldSurfaceMeshAndGeometry(D.g.faces, D.g.pos3);
  ManifoldSurfaceMesh& mesh = *std::get<0>(mpGp);
  VertexPositionGeometry& geom = *std::get<1>(mpGp);

  Vertex vP0 = mesh.vertex(D.idxP0);
  Vertex vM = mesh.vertex(D.idxM);
  Vertex vP2 = mesh.vertex(D.idxP2);

  // Independent expectations: |P0 M| = |(1, .75)| = 1.25, |M P2| = 1.25, so
  // the pinned optimum is 2.5 while the unconstrained |P0 P2| = 2.0.
  const double expectPinned = 2.5;
  const double unconstrained = dist2(D.p0, D.p2);
  TASK_CHECK(std::fabs(unconstrained - 2.0) < TOL);

  // -- Part A: two separate paths joined at the marked vertex M
  {
    std::vector<Halfedge> he1 = shortestEdgePath(geom, vP0, vM);
    std::vector<Halfedge> he2 = shortestEdgePath(geom, vM, vP2);
    TASK_CHECK(!he1.empty() && !he2.empty());
    FlipEdgeNetwork net(mesh, geom, {he1, he2});
    net.posGeom = &geom;
    TASK_CHECK(net.paths.size() == 2);
    const double initLen = net.length();
    TASK_CHECK(initLen > expectPinned + 0.2);

    net.iterativeShorten();

    // the container still holds exactly the two segments
    TASK_CHECK(net.paths.size() == 2);
    // total length is the pinned optimum, not the unconstrained straight line
    TASK_CHECK(std::fabs(net.length() - expectPinned) < TOL);
    TASK_CHECK(std::fabs(net.length() - unconstrained) > 0.2);

    // the actual container contents: segment endpoints and the junction at M
    std::vector<Halfedge> list0 = net.paths[0]->getHalfedgeList();
    std::vector<Halfedge> list1 = net.paths[1]->getHalfedgeList();
    TASK_CHECK(!list0.empty() && !list1.empty());
    TASK_CHECK(inputIndexOf(net, list0.front().vertex()) == D.idxP0);
    TASK_CHECK(inputIndexOf(net, list0.back().twin().vertex()) == D.idxM);
    TASK_CHECK(inputIndexOf(net, list1.front().vertex()) == D.idxM);
    TASK_CHECK(inputIndexOf(net, list1.back().twin().vertex()) == D.idxP2);
    // the junction is one and the same intrinsic vertex
    TASK_CHECK(list0.back().twin().vertex() == list1.front().vertex());

    // the junction survives in the retraced polylines as well
    std::vector<std::vector<SurfacePoint>> poly = net.getPathPolyline();
    TASK_CHECK(poly.size() == 2);
    if (poly.size() == 2) {
      TASK_CHECK(poly[0].back().type == SurfacePointType::Vertex && poly[0].back().vertex.getIndex() == D.idxM);
      TASK_CHECK(poly[1].front().type == SurfacePointType::Vertex && poly[1].front().vertex.getIndex() == D.idxM);
      // each segment is straight on its own side (no crossing over M)
      for (int k = 0; k < 2; k++) {
        P2 a = (k == 0) ? D.p0 : D.m, b = (k == 0) ? D.m : D.p2;
        for (const SurfacePoint& sp : poly[k]) {
          P2 p = interpSP(sp, D.g.pos2);
          double t = dot2(p - a, b - a) / dot2(b - a, b - a);
          P2 proj = a + (b - a) * t;
          TASK_CHECK(dist2(p, proj) < TOL);
        }
      }
    }

    // endpoints and the junction are the marked vertices, and nothing else
    TASK_CHECK(countMarked(net) == 3);
    TASK_CHECK(net.isMarkedVertex[net.mesh.vertex(D.idxP0)]);
    TASK_CHECK(net.isMarkedVertex[net.mesh.vertex(D.idxM)]);
    TASK_CHECK(net.isMarkedVertex[net.mesh.vertex(D.idxP2)]);

    // validate() must agree too (but the checks above are the real content)
    bool valid = true;
    try {
      net.validate();
    } catch (...) {
      valid = false;
    }
    TASK_CHECK(valid);
  }

  // -- Part B: one piecewise path with a marked interior point; with
  //    straightenAroundMarkedVertices=false the marked point is an obstacle
  //    that the shortening may not cross.
  {
    std::unique_ptr<FlipEdgeNetwork> net =
        FlipEdgeNetwork::constructFromPiecewiseDijkstraPath(mesh, geom, {vP0, vM, vP2}, false, true);
    TASK_CHECK(net != nullptr);
    if (!net) return 1;
    net->posGeom = &geom;
    net->straightenAroundMarkedVertices = false;
    TASK_CHECK(net->paths.size() == 1);
    TASK_CHECK(net->isMarkedVertex[net->mesh.vertex(D.idxM)]);

    net->iterativeShorten();

    TASK_CHECK(net->paths.size() == 1);
    TASK_CHECK(std::fabs(net->length() - expectPinned) < TOL);
    TASK_CHECK(std::fabs(net->length() - unconstrained) > 0.2);

    // the path still passes through the marked point
    std::vector<std::vector<SurfacePoint>> poly = net->getPathPolyline();
    TASK_CHECK(poly.size() == 1);
    if (poly.size() == 1) {
      bool hitsM = false;
      for (const SurfacePoint& sp : poly[0]) {
        if (sp.type == SurfacePointType::Vertex && sp.vertex.getIndex() == D.idxM) hitsM = true;
      }
      TASK_CHECK(hitsM);
    }
  }

  return 0;
}

// =============================================================================
// rewind group: shorten / rewind / reinitializePath / shorten, then align
// with a fresh network on the same input
// =============================================================================
int caseRewindReinitialize() {
  GeneratedMesh D = buildFoldedStrip();
  auto mpGp = makeManifoldSurfaceMeshAndGeometry(D.faces, D.pos3);
  ManifoldSurfaceMesh& mesh = *std::get<0>(mpGp);
  VertexPositionGeometry& geom = *std::get<1>(mpGp);

  // path A: (0,0) -> (6,1) staircase; expected geodesic length sqrt(37)
  const std::vector<size_t> pathA = {0, 1, 8, 9, 16, 17, 18, 11, 12, 13};
  // path B: (0,2) -> (5,0) via the top row; expected geodesic length sqrt(29)
  const std::vector<size_t> pathB = {14, 15, 16, 17, 18, 19, 12, 5};
  const double expectA = dist2(P2{0., 0.}, P2{6., 1.});
  const double expectB = dist2(P2{0., 2.}, P2{5., 0.});

  std::vector<Halfedge> hesA = edgePathFromIndices(mesh, pathA);
  std::vector<Halfedge> hesB = edgePathFromIndices(mesh, pathB);
  TASK_CHECK(hesA.size() + 1 == pathA.size());
  TASK_CHECK(hesB.size() + 1 == pathB.size());

  FlipEdgeNetwork netA(mesh, geom, {hesA});
  netA.posGeom = &geom;
  netA.supportRewinding = true;

  // -- 1) shorten the first path
  netA.iterativeShorten();
  TASK_CHECK(netA.nFlips > 0);
  TASK_CHECK(!netA.intrinsicTriIsOriginal()); // flips really changed the triangulation
  TASK_CHECK(std::fabs(netA.length() - expectA) < TOL);

  // -- 2) rewind: the path is cleared by design; connectivity, edge lengths,
  //    original-edge flags and path occupancy must be back to the initial state
  netA.rewind();
  TASK_CHECK(netA.paths.empty());
  TASK_CHECK(netA.getPathPolyline().empty());
  TASK_CHECK(netA.wedgeAngleQueue.empty());
  size_t nOccupied = 0;
  for (Edge e : netA.mesh.edges())
    if (netA.edgeInPath(e)) nOccupied++;
  TASK_CHECK(nOccupied == 0);

  TASK_CHECK(netA.intrinsicTriIsOriginal()); // original-edge flags restored
  TASK_CHECK(netA.mesh.nVertices() == mesh.nVertices());
  TASK_CHECK(netA.mesh.nEdges() == mesh.nEdges());
  TASK_CHECK(netA.mesh.nFaces() == mesh.nFaces());
  TASK_CHECK(netA.mesh.nHalfedges() == mesh.nHalfedges());
  // initial connectivity, halfedge by halfedge
  size_t nBadHalfedge = 0;
  for (size_t i = 0; i < mesh.nHalfedges(); i++) {
    Halfedge hin = mesh.halfedge(i);
    Halfedge hintr = netA.mesh.halfedge(i);
    if (hin.tailVertex().getIndex() != hintr.tailVertex().getIndex() ||
        hin.tipVertex().getIndex() != hintr.tipVertex().getIndex())
      nBadHalfedge++;
  }
  TASK_CHECK(nBadHalfedge == 0);
  // initial metric, independently recomputed from the generated 3D positions
  for (Edge e : netA.mesh.edges()) {
    Halfedge he = e.halfedge();
    size_t a = he.tailVertex().getIndex(), b = he.tipVertex().getIndex();
    double expect = (D.pos3[a] - D.pos3[b]).norm();
    TASK_CHECK(std::fabs(netA.tri->edgeLengths[e] - expect) < TOL_EXACT);
  }
  // current interface convention: rewind() does not restore isMarkedVertex;
  // the marks of the first path's endpoints are still set
  TASK_CHECK(countMarked(netA) == 2);
  TASK_CHECK(netA.isMarkedVertex[netA.mesh.vertex(pathA.front())]);
  TASK_CHECK(netA.isMarkedVertex[netA.mesh.vertex(pathA.back())]);

  // -- 3) reinitialize with a different path
  netA.reinitializePath({hesB});
  TASK_CHECK(netA.paths.size() == 1);
  // path occupancy is exactly the new path's edges -- nothing inherited
  nOccupied = 0;
  for (Edge e : netA.mesh.edges())
    if (netA.edgeInPath(e)) nOccupied++;
  TASK_CHECK(nOccupied == pathB.size() - 1);
  for (size_t k = 1; k < pathB.size(); k++) {
    bool found = false;
    for (Halfedge h : netA.mesh.vertex(pathB[k - 1]).outgoingHalfedges()) {
      if (h.tipVertex() == netA.mesh.vertex(pathB[k])) {
        found = netA.edgeInPath(h.edge());
        break;
      }
    }
    TASK_CHECK(found);
  }
  // Per the current interface, reinitializePath keeps whatever marks were
  // left over; explicitly set the same marked set a fresh network would have
  // before comparing.
  netA.isMarkedVertex.fill(false);
  netA.isMarkedVertex[netA.mesh.vertex(pathB.front())] = true;
  netA.isMarkedVertex[netA.mesh.vertex(pathB.back())] = true;

  // -- 4) shorten again on the reused network
  netA.iterativeShorten();
  TASK_CHECK(std::fabs(netA.length() - expectB) < TOL);

  // -- interleave an independent network on a separate instance, so that any
  //    leaked queue / edge-stack / metric state would have to survive an
  //    unrelated computation to go unnoticed
  {
    GeneratedMesh D2 = buildFoldedStrip();
    auto mpGp2 = makeManifoldSurfaceMeshAndGeometry(D2.faces, D2.pos3);
    std::vector<Halfedge> hesX = edgePathFromIndices(*std::get<0>(mpGp2), pathA);
    TASK_CHECK(hesX.size() + 1 == pathA.size());
    FlipEdgeNetwork netX(*std::get<0>(mpGp2), *std::get<1>(mpGp2), {hesX});
    netX.iterativeShorten();
    TASK_CHECK(std::fabs(netX.length() - expectA) < TOL);
  }

  // -- a brand-new network on a fresh instance of the same input
  GeneratedMesh D3 = buildFoldedStrip();
  auto mpGp3 = makeManifoldSurfaceMeshAndGeometry(D3.faces, D3.pos3);
  std::vector<Halfedge> hesF = edgePathFromIndices(*std::get<0>(mpGp3), pathB);
  TASK_CHECK(hesF.size() + 1 == pathB.size());
  FlipEdgeNetwork netF(*std::get<0>(mpGp3), *std::get<1>(mpGp3), {hesF});
  netF.iterativeShorten();
  TASK_CHECK(std::fabs(netF.length() - expectB) < TOL);

  // -- align the surface paths: same point count, same point kinds, same
  //    positions (geometric comparison -- internal handle numbering and flip
  //    order are deliberately NOT fixed as the unique correct answer)
  std::vector<std::vector<SurfacePoint>> polyA = netA.getPathPolyline();
  std::vector<std::vector<SurfacePoint>> polyF = netF.getPathPolyline();
  TASK_CHECK(polyA.size() == 1 && polyF.size() == 1);
  if (polyA.size() != 1 || polyF.size() != 1) return 1;
  TASK_CHECK(polyA[0].size() == polyF[0].size());
  if (polyA[0].size() != polyF[0].size()) return 1;
  for (size_t i = 0; i < polyA[0].size(); i++) {
    TASK_CHECK(polyA[0][i].type == polyF[0][i].type);
    Vector3 a = interpSP(polyA[0][i], D.pos3);
    Vector3 f = interpSP(polyF[0][i], D3.pos3);
    TASK_CHECK((a - f).norm() < TOL);
  }
  // the reused network's path is also the correct geodesic in the atlas
  TASK_CHECK(polyA[0].front().type == SurfacePointType::Vertex &&
             polyA[0].front().vertex.getIndex() == pathB.front());
  TASK_CHECK(polyA[0].back().type == SurfacePointType::Vertex &&
             polyA[0].back().vertex.getIndex() == pathB.back());

  // -- marked sets agree with the fresh network (explicitly equalized above)
  TASK_CHECK(netA.mesh.nVertices() == netF.mesh.nVertices());
  for (size_t i = 0; i < netA.mesh.nVertices(); i++) {
    TASK_CHECK(netA.isMarkedVertex[netA.mesh.vertex(i)] == netF.isMarkedVertex[netF.mesh.vertex(i)]);
  }
  TASK_CHECK(countMarked(netF) == 2);

  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Case registration for task_support/driver.c
// ---------------------------------------------------------------------------
const struct task_case task_cases[] = {
    {"chart", "folded_strip_unfolding", &caseChartFoldedStrip},
    {"network", "hole_homotopy_classes", &caseNetworkHoleClasses},
    {"network", "marked_point_piecewise", &caseNetworkMarkedPiecewise},
    {"rewind", "rewind_reinitialize_alignment", &caseRewindReinitialize},
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
