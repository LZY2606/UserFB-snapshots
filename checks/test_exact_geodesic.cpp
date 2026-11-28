//=============================================================================
//
//  Tests for OpenMesh::Geodesic::ExactGeodesicT (exact polyhedral geodesics)
//
//  Cases are registered in the three groups "distance", "windows" and
//  "trace" consumed by task_support/driver.c.  Every case returns 0 on
//  success and uses TASK_CHECK for its assertions.
//
//=============================================================================

#include <OpenMesh/Tools/Geodesic/ExactGeodesicT.hh>
#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>

#include "task_support/check.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

struct DoubleTraits : OpenMesh::DefaultTraits
{
  typedef OpenMesh::Vec3d Point;
  typedef OpenMesh::Vec3d Normal;
};

typedef OpenMesh::TriMesh_ArrayKernelT<DoubleTraits> Mesh;
typedef OpenMesh::Geodesic::ExactGeodesicT<Mesh>     Solver;
typedef Mesh::Point                                   P3;
typedef Solver::SurfacePoint                          SurfacePoint;

const double INF = std::numeric_limits<double>::infinity();
const double PI  = std::acos(-1.0);

bool near_eq(double a, double b, double tol = 1e-7)
{ return std::fabs(a - b) <= tol; }

//--- mesh builders -----------------------------------------------------------

// axis-aligned quad grid; diag flips the triangulation orientation
Mesh make_grid(int nx, int ny, int diag)
{
  Mesh m;
  std::vector<std::vector<Mesh::VertexHandle> > v(nx + 1);
  for (auto& row : v) row.resize(ny + 1);
  for (int i = 0; i <= nx; ++i)
    for (int j = 0; j <= ny; ++j)
      v[i][j] = m.add_vertex(P3(i, j, 0.0));
  for (int i = 0; i < nx; ++i)
    for (int j = 0; j < ny; ++j)
    {
      auto a = v[i][j], b = v[i+1][j], c = v[i+1][j+1], d = v[i][j+1];
      if ((i + j + diag) % 2 == 0)
      { m.add_face(a, b, c); m.add_face(a, c, d); }
      else
      { m.add_face(a, b, d); m.add_face(b, c, d); }
    }
  return m;
}

// grid with a rectangular run of cells removed as a hole
Mesh make_holed_grid(int nx, int ny, int hx0, int hx1, int hy0, int hy1)
{
  Mesh m;
  std::vector<std::vector<Mesh::VertexHandle> > v(nx + 1);
  for (auto& row : v) row.resize(ny + 1);
  for (int i = 0; i <= nx; ++i)
    for (int j = 0; j <= ny; ++j)
      v[i][j] = m.add_vertex(P3(i, j, 0.0));
  for (int i = 0; i < nx; ++i)
    for (int j = 0; j < ny; ++j)
    {
      if (i >= hx0 && i < hx1 && j >= hy0 && j < hy1) continue;
      auto a = v[i][j], b = v[i+1][j], c = v[i+1][j+1], d = v[i][j+1];
      if ((i + j) % 2 == 0)
      { m.add_face(a, b, c); m.add_face(a, c, d); }
      else
      { m.add_face(a, b, d); m.add_face(b, c, d); }
    }
  return m;
}

P3 world_point(const Mesh& m, const SurfacePoint& sp)
{
  P3 p(0.0, 0.0, 0.0);
  auto fv = m.cfv_iter(sp.face);
  for (int k = 0; k < 3; ++k, ++fv)
    p = p + m.point(*fv) * sp.bary[k];
  return p;
}

double path_length(const Mesh& m, const std::vector<SurfacePoint>& path)
{
  double L = 0.0;
  for (std::size_t i = 1; i < path.size(); ++i)
    L += (world_point(m, path[i]) - world_point(m, path[i-1])).norm();
  return L;
}

// largest world gap across an edge crossing (records on different faces)
double max_cross_gap(const Mesh& m, const std::vector<SurfacePoint>& path)
{
  double g = 0.0;
  for (std::size_t i = 1; i < path.size(); ++i)
    if (path[i].face != path[i-1].face)
      g = std::max(g,
        (world_point(m, path[i]) - world_point(m, path[i-1])).norm());
  return g;
}

bool valid_bary(const std::array<double, 3>& b)
{
  double s = 0.0;
  for (double q : b)
  {
    if (!std::isfinite(q) || q < -1e-9 || q > 1.0 + 1e-9) return false;
    s += q;
  }
  return std::fabs(s - 1.0) <= 1e-9;
}

// corner barycentric coordinates of vertex _vh inside face _fh
std::array<double, 3> corner_bary(const Mesh& m, Mesh::FaceHandle fh,
                                  Mesh::VertexHandle vh)
{
  std::array<double, 3> b{{0.0, 0.0, 0.0}};
  int slot = 0;
  for (auto fv = m.cfv_iter(fh); fv.is_valid(); ++fv, ++slot)
    if (*fv == vh) b[slot] = 1.0;
  return b;
}

bool path_visits(const Mesh& m, const std::vector<SurfacePoint>& path,
                 const P3& p, double tol = 1e-9)
{
  for (const SurfacePoint& sp : path)
    if ((world_point(m, sp) - p).norm() <= tol) return true;
  return false;
}

Mesh::FaceHandle face_with(const Mesh& m, Mesh::VertexHandle vh)
{
  for (auto f = m.faces_begin(); f != m.faces_end(); ++f)
    for (auto fv = m.cfv_iter(*f); fv.is_valid(); ++fv)
      if (*fv == vh) return *f;
  return Mesh::FaceHandle();
}

//=========================== distance cases ==================================

int case_planar_grid_analytic()
{
  for (int diag = 0; diag < 2; ++diag)
  {
    Mesh g = make_grid(4, 3, diag);
    Solver e(g);
    TASK_CHECK(e.solve(g.vertex_handle(0)));
    double max_err = 0.0;
    for (int i = 0; i <= 4; ++i)
      for (int j = 0; j <= 3; ++j)
      {
        int idx = i * 4 + j;
        double d = e.distance(g.vertex_handle(idx));
        max_err = std::max(max_err,
                           std::fabs(d - std::hypot((double)i, (double)j)));
      }
    TASK_CHECK(max_err <= 1e-7);
    // continuous straight line, not the edge-graph Manhattan length 7
    TASK_CHECK(near_eq(e.distance(g.vertex_handle(19)), 5.0));
  }
  return 0;
}

int case_folded_band_analytic()
{
  Mesh m;
  const double theta = PI / 3.0;
  auto A = m.add_vertex(P3(0.0, 0.0, 0.0));
  auto B = m.add_vertex(P3(1.0, 0.0, 0.0));
  auto C = m.add_vertex(P3(0.5, 1.0, 0.0));
  auto D = m.add_vertex(P3(0.5, -std::cos(theta), std::sin(theta)));
  m.add_face(A, B, C);
  m.add_face(B, A, D);
  Solver e(m);
  TASK_CHECK(e.solve(C));
  // the two unit isosceles triangles unfold into a straight 2-unit path
  TASK_CHECK(near_eq(e.distance(D), 2.0));
  // the spatial chord is strictly shorter, proving this is a surface length
  TASK_CHECK((m.point(C) - m.point(D)).norm() < 2.0 - 1e-6);
  return 0;
}

int case_hole_detour_analytic()
{
  // 5x5 grid with the single cell [2,3]x[2,3] removed
  Mesh m = make_holed_grid(5, 5, 2, 3, 2, 3);
  Solver e(m);
  auto src = m.vertex_handle(0);                 // (0,0)
  auto dst = m.vertex_handle(5 * 6 + 5);         // (5,5)
  TASK_CHECK(e.solve(src));
  double expect = 2.0 * std::sqrt(13.0);         // via corner (2,3)/(3,2)
  TASK_CHECK(near_eq(e.distance(dst), expect, 1e-6));
  TASK_CHECK(e.distance(dst) > 5.0 * std::sqrt(2.0) + 1e-6);
  return 0;
}

int case_cone_analytic()
{
  // convex cone: 8 congruent triangles around an interior apex
  const int n = 8;
  const double R = 1.0, H = 2.0;
  Mesh m;
  auto O = m.add_vertex(P3(0.0, 0.0, H));
  Mesh::VertexHandle ring[8];
  for (int i = 0; i < n; ++i)
  {
    double phi = 2.0 * PI * i / n;
    ring[i] = m.add_vertex(P3(R * std::cos(phi), R * std::sin(phi), 0.0));
  }
  for (int i = 0; i < n; ++i)
    m.add_face(O, ring[i], ring[(i + 1) % n]);

  Solver e(m);
  TASK_CHECK(e.solve(ring[0]));

  double slant = std::sqrt(R * R + H * H);
  double base = 2.0 * R * std::sin(PI / n);
  double beta = 2.0 * std::asin(base / (2.0 * slant));
  for (int k = 1; k <= 4; ++k)
  {
    double sector = k * beta;
    double expect = (sector <= PI)
                    ? 2.0 * slant * std::sin(sector / 2.0)
                    : 2.0 * slant;
    TASK_CHECK(near_eq(e.distance(ring[k]), expect, 1e-7));
  }
  TASK_CHECK(near_eq(e.distance(O), slant, 1e-7));
  return 0;
}

int case_saddle_via_apex()
{
  // four triangles fanning around a saddle apex (total angle > 2 pi)
  double s = 1.0 / std::sqrt(3.0);
  P3 dirs[4] = { P3( s,  s,  s), P3( s, -s, -s),
                 P3(-s,  s, -s), P3(-s, -s,  s) };
  Mesh m;
  auto O = m.add_vertex(P3(0.0, 0.0, 0.0));
  Mesh::VertexHandle rv[4];
  for (int i = 0; i < 4; ++i) rv[i] = m.add_vertex(dirs[i]);
  for (int i = 0; i < 4; ++i)
    m.add_face(O, rv[i], rv[(i + 1) % 4]);

  Solver e(m);
  TASK_CHECK(e.solve(rv[0]));
  // opposite ring vertices: shortest route turns at the saddle apex, 2 units
  TASK_CHECK(near_eq(e.distance(rv[2]), 2.0));
  TASK_CHECK(near_eq(e.distance(O), 1.0));
  return 0;
}

int case_disconnected_infinity()
{
  Mesh m;
  auto a = m.add_vertex(P3(0, 0, 0));
  auto b = m.add_vertex(P3(1, 0, 0));
  auto c = m.add_vertex(P3(0, 1, 0));
  m.add_face(a, b, c);
  auto d = m.add_vertex(P3(5, 0, 0));
  auto e2 = m.add_vertex(P3(6, 0, 0));
  auto f = m.add_vertex(P3(5, 1, 0));
  m.add_face(d, e2, f);

  Solver e(m);
  TASK_CHECK(e.solve(a));
  TASK_CHECK(near_eq(e.distance(b), 1.0));
  TASK_CHECK(std::isinf(e.distance(d)));
  TASK_CHECK(e.distance(d) > 0.0);
  return 0;
}

int case_repeated_solve_isolation()
{
  Mesh m;
  auto a = m.add_vertex(P3(0, 0, 0));
  auto b = m.add_vertex(P3(1, 0, 0));
  auto c = m.add_vertex(P3(0, 1, 0));
  m.add_face(a, b, c);
  auto d = m.add_vertex(P3(5, 0, 0));
  auto e2 = m.add_vertex(P3(6, 0, 0));
  auto f = m.add_vertex(P3(5, 1, 0));
  m.add_face(d, e2, f);

  Solver e(m);
  TASK_CHECK(e.solve(a));
  TASK_CHECK(std::isinf(e.distance(d)));
  TASK_CHECK(e.solve(d));                 // switch source component
  TASK_CHECK(near_eq(e.distance(e2), 1.0));
  TASK_CHECK(std::isinf(e.distance(a)));  // no stale windows from source a
  TASK_CHECK(e.solve(a));                 // and switch back cleanly
  TASK_CHECK(near_eq(e.distance(b), 1.0));
  TASK_CHECK(std::isinf(e.distance(d)));

  // an invalid source must fail without poisoning the current solution
  TASK_CHECK(!e.solve(Mesh::VertexHandle(999)));
  TASK_CHECK(near_eq(e.distance(b), 1.0));
  return 0;
}

int case_snapshot_independence()
{
  Mesh m;
  auto a = m.add_vertex(P3(0, 0, 0));
  auto b = m.add_vertex(P3(1, 0, 0));
  auto c = m.add_vertex(P3(0, 1, 0));
  m.add_face(a, b, c);
  Solver e(m);
  m.set_point(b, P3(99.0, 99.0, 99.0));   // mutate the original mesh
  TASK_CHECK(e.solve(a));
  TASK_CHECK(near_eq(e.distance(b), 1.0)); // solver keeps its own snapshot
  return 0;
}

int case_exceptions_and_validation()
{
  Mesh m;
  auto a = m.add_vertex(P3(0, 0, 0));
  auto b = m.add_vertex(P3(1, 0, 0));
  auto c = m.add_vertex(P3(0, 1, 0));
  m.add_face(a, b, c);
  Solver e(m);

  bool threw = false;
  try { (void)e.distance(a); }
  catch (const std::logic_error&) { threw = true; }
  TASK_CHECK(threw);

  threw = false;
  try
  {
    std::vector<SurfacePoint> out; double len;
    e.trace(m.face_handle(0), {{1.0/3.0, 1.0/3.0, 1.0/3.0}}, out, len);
  }
  catch (const std::logic_error&) { threw = true; }
  TASK_CHECK(threw);

  TASK_CHECK(e.solve(a));

  threw = false;
  try { (void)e.distance(Mesh::VertexHandle(7)); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  threw = false;
  try
  {
    std::vector<SurfacePoint> out; double len;
    e.trace(Mesh::FaceHandle(7), {{1.0, 0.0, 0.0}}, out, len);
  }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  threw = false;
  try
  {
    std::vector<SurfacePoint> out; double len;
    e.trace(m.face_handle(0), {{0.5, 0.6, 0.0}}, out, len);
  }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  // unsupported construction inputs
  Mesh iso; iso.add_vertex(P3(0, 0, 0));
  threw = false;
  try { Solver bad(iso); } catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  Mesh nf;
  auto p = nf.add_vertex(P3(0, 0, 0));
  auto q = nf.add_vertex(P3(1, 0, 0));
  auto r = nf.add_vertex(P3(0, 1, 0));
  nf.add_face(p, q, r);
  nf.set_point(q, P3(std::nan(""), 0.0, 0.0));
  threw = false;
  try { Solver bad(nf); } catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);

  Mesh za;
  auto z0 = za.add_vertex(P3(0, 0, 0));
  auto z1 = za.add_vertex(P3(1, 0, 0));
  auto z2 = za.add_vertex(P3(2, 0, 0));
  threw = false;
  try { za.add_face(z0, z1, z2); Solver bad(za); }
  catch (const std::invalid_argument&) { threw = true; }
  TASK_CHECK(threw);
  return 0;
}

//============================ windows cases =================================

int case_envelope_diagonal_invariance()
{
  // identical planar patch, the two possible diagonal triangulations must
  // produce the same continuous answer (never the edge-graph distance)
  Mesh g0 = make_grid(6, 5, 0);
  Mesh g1 = make_grid(6, 5, 1);
  Solver e0(g0), e1(g1);
  TASK_CHECK(e0.solve(g0.vertex_handle(0)));
  TASK_CHECK(e1.solve(g1.vertex_handle(0)));
  double max_diff = 0.0;
  for (int i = 0; i <= 6; ++i)
    for (int j = 0; j <= 5; ++j)
    {
      int idx = i * 6 + j;
      double d0 = e0.distance(g0.vertex_handle(idx));
      double d1 = e1.distance(g1.vertex_handle(idx));
      max_diff = std::max(max_diff, std::fabs(d0 - d1));
      TASK_CHECK(near_eq(d0, std::hypot((double)i, (double)j)));
    }
  TASK_CHECK(max_diff <= 1e-9);
  // edge-graph Manhattan distance to the far corner is 11, exact is sqrt(61)
  TASK_CHECK(std::fabs(e0.distance(g0.vertex_handle(6 * 6 + 5)) -
                       std::sqrt(61.0)) <= 1e-7);
  return 0;
}

int case_wavefront_competition()
{
  // two-cell horizontal hole blocking the straight line; two competing
  // wavefronts wrap around the two hole corners, the shorter one wins
  Mesh m = make_holed_grid(6, 6, 2, 4, 2, 3);  // hole spans x[2,4] y[2,3]
  Solver e(m);
  auto src = m.vertex_handle(0);               // (0,0)
  auto dst = m.vertex_handle(6 * 7 + 6);       // (6,6)
  TASK_CHECK(e.solve(src));

  P3 corner_left(2.0, 3.0, 0.0);               // shorter route
  P3 corner_right(4.0, 2.0, 0.0);
  double via_left  = corner_left.norm() +
                     (P3(6.0, 6.0, 0.0) - corner_left).norm();
  double via_right = corner_right.norm() +
                     (P3(6.0, 6.0, 0.0) - corner_right).norm();
  TASK_CHECK(via_left < via_right - 1e-9);
  TASK_CHECK(near_eq(e.distance(dst), via_left, 1e-7));

  std::vector<SurfacePoint> path; double len;
  TASK_CHECK(e.trace(face_with(m, dst), corner_bary(m, face_with(m, dst), dst),
                     path, len));
  TASK_CHECK(near_eq(len, via_left, 1e-7));
  TASK_CHECK(path_visits(m, path, corner_left, 1e-8));
  return 0;
}

int case_convex_vertex_not_shortcut()
{
  // on a convex cone the apex must never act as a pseudosource: a straight
  // unfolded chord between ring vertices is shorter than going over the apex
  const int n = 8;
  const double R = 1.0, H = 2.0;
  Mesh m;
  auto O = m.add_vertex(P3(0.0, 0.0, H));
  Mesh::VertexHandle ring[8];
  for (int i = 0; i < n; ++i)
  {
    double phi = 2.0 * PI * i / n;
    ring[i] = m.add_vertex(P3(R * std::cos(phi), R * std::sin(phi), 0.0));
  }
  for (int i = 0; i < n; ++i)
    m.add_face(O, ring[i], ring[(i + 1) % n]);

  Solver e(m);
  TASK_CHECK(e.solve(ring[0]));
  double slant = std::sqrt(R * R + H * H);

  std::vector<SurfacePoint> path; double len;
  Mesh::FaceHandle fh = face_with(m, ring[3]);
  TASK_CHECK(e.trace(fh, corner_bary(m, fh, ring[3]), path, len));
  TASK_CHECK(len < 2.0 * slant - 1e-6);       // not the over-apex route
  TASK_CHECK(!path_visits(m, path, m.point(O), 1e-9));
  TASK_CHECK(near_eq(len, e.distance(ring[3]), 1e-9));
  return 0;
}

//============================= trace cases ==================================

int case_planar_trace_continuity()
{
  Mesh g = make_grid(4, 3, 0);
  Solver e(g);
  TASK_CHECK(e.solve(g.vertex_handle(0)));

  Mesh::FaceHandle fh = g.face_handle(g.n_faces() - 1);
  std::array<double, 3> bary{{0.2, 0.3, 0.5}};
  std::vector<SurfacePoint> path; double len = 0.0;
  TASK_CHECK(e.trace(fh, bary, path, len));

  // target-to-source ordering
  TASK_CHECK(near_eq((world_point(g, path.front()) -
                      world_point(g, path.front())).norm(), 0.0));
  P3 target(0.0, 0.0, 0.0);
  {
    auto fv = g.cfv_iter(fh);
    for (int k = 0; k < 3; ++k, ++fv)
      target = target + g.point(*fv) * bary[k];
  }
  TASK_CHECK((world_point(g, path.front()) - target).norm() <= 1e-10);
  TASK_CHECK((world_point(g, path.back()) - g.point(g.vertex_handle(0))).norm()
             <= 1e-10);

  // every record carries valid barycentric coordinates
  for (const SurfacePoint& sp : path)
  {
    TASK_CHECK(sp.face.is_valid());
    TASK_CHECK(valid_bary(sp.bary));
  }
  // consecutive records share a face, or are the same point across an edge
  TASK_CHECK(max_cross_gap(g, path) <= 1e-8);
  // the in-face segment sum equals the analytic straight-line length
  TASK_CHECK(near_eq(path_length(g, path), target.norm(), 1e-9));
  TASK_CHECK(near_eq(len, target.norm(), 1e-9));
  return 0;
}

int case_edge_target_duplicated_record()
{
  // target placed on a shared interior edge: it must be recorded once on
  // each side, the two records coinciding in space
  Mesh g = make_grid(4, 4, 0);
  Solver e(g);
  TASK_CHECK(e.solve(g.vertex_handle(0)));

  // choose an interior edge far from the source, so the path must cross
  // other edges before reaching it; neither incident face contains source 0
  auto src_vh = g.vertex_handle(0);
  Mesh::EdgeHandle eh;
  bool found = false;
  for (auto it = g.edges_begin(); it != g.edges_end() && !found; ++it)
  {
    if (g.is_boundary(*it)) continue;
    auto he = g.halfedge_handle(*it, 0);
    Mesh::FaceHandle fa = g.face_handle(he);
    Mesh::FaceHandle fb = g.opposite_face_handle(he);
    if (!fa.is_valid() || !fb.is_valid()) continue;
    bool touches_source = false;
    for (auto fv = g.cfv_iter(fa); fv.is_valid(); ++fv)
      if (*fv == src_vh) touches_source = true;
    for (auto fv = g.cfv_iter(fb); fv.is_valid(); ++fv)
      if (*fv == src_vh) touches_source = true;
    if (!touches_source) { eh = *it; found = true; }
  }
  TASK_CHECK(found);
  auto he = g.halfedge_handle(eh, 0);
  Mesh::FaceHandle fh = g.face_handle(he);
  auto va = g.to_vertex_handle(he);
  auto vb = g.from_vertex_handle(he);
  // midpoint of that edge, expressed in this face's cfv order
  std::array<double, 3> bary{{0.0, 0.0, 0.0}};
  int slot = 0;
  for (auto fv = g.cfv_iter(fh); fv.is_valid(); ++fv, ++slot)
  {
    if (*fv == va) bary[slot] += 0.5;
    if (*fv == vb) bary[slot] += 0.5;
  }

  std::vector<SurfacePoint> path; double len = 0.0;
  TASK_CHECK(e.trace(fh, bary, path, len));
  TASK_CHECK(path.size() >= 2);

  // the first record is exactly the requested edge midpoint
  P3 mid = (g.point(va) + g.point(vb)) * 0.5;
  TASK_CHECK(path[0].face == fh);
  TASK_CHECK((world_point(g, path[0]) - mid).norm() <= 1e-10);

  // every edge crossing is recorded twice (one record per incident face)
  // and the two records coincide in space within the required tolerance
  int crossings = 0;
  for (std::size_t i = 1; i < path.size(); ++i)
    if (path[i].face != path[i-1].face)
    {
      ++crossings;
      TASK_CHECK((world_point(g, path[i]) -
                  world_point(g, path[i-1])).norm() <= 1e-8);
    }
  TASK_CHECK(crossings >= 1);
  TASK_CHECK(max_cross_gap(g, path) <= 1e-8);
  TASK_CHECK(near_eq(path_length(g, path), mid.norm(), 1e-9));
  TASK_CHECK(near_eq(len, mid.norm(), 1e-9));
  return 0;
}

int case_folded_trace_to_source()
{
  Mesh m;
  const double theta = PI / 3.0;
  auto A = m.add_vertex(P3(0.0, 0.0, 0.0));
  auto B = m.add_vertex(P3(1.0, 0.0, 0.0));
  auto C = m.add_vertex(P3(0.5, 1.0, 0.0));
  auto D = m.add_vertex(P3(0.5, -std::cos(theta), std::sin(theta)));
  auto f0 = m.add_face(A, B, C);
  auto f1 = m.add_face(B, A, D);
  Solver e(m);
  TASK_CHECK(e.solve(C));

  std::vector<SurfacePoint> path; double len = 0.0;
  TASK_CHECK(e.trace(f1, corner_bary(m, f1, D), path, len));
  TASK_CHECK(near_eq(len, 2.0, 1e-9));
  TASK_CHECK(near_eq(path_length(m, path), 2.0, 1e-9));
  TASK_CHECK(max_cross_gap(m, path) <= 1e-8);
  TASK_CHECK((world_point(m, path.back()) - m.point(C)).norm() <= 1e-10);
  TASK_CHECK((world_point(m, path.front()) - m.point(D)).norm() <= 1e-10);
  (void)f0;
  return 0;
}

int case_vertex_event_trace_face_ownership()
{
  // saddle apex path: every bend vertex must carry a real face record
  double s = 1.0 / std::sqrt(3.0);
  P3 dirs[4] = { P3( s,  s,  s), P3( s, -s, -s),
                 P3(-s,  s, -s), P3(-s, -s,  s) };
  Mesh m;
  auto O = m.add_vertex(P3(0.0, 0.0, 0.0));
  Mesh::VertexHandle rv[4];
  for (int i = 0; i < 4; ++i) rv[i] = m.add_vertex(dirs[i]);
  for (int i = 0; i < 4; ++i)
    m.add_face(O, rv[i], rv[(i + 1) % 4]);
  Solver e(m);
  TASK_CHECK(e.solve(rv[0]));

  Mesh::FaceHandle fh = face_with(m, rv[2]);
  std::vector<SurfacePoint> path; double len;
  TASK_CHECK(e.trace(fh, corner_bary(m, fh, rv[2]), path, len));
  TASK_CHECK(near_eq(len, 2.0, 1e-9));
  TASK_CHECK(path_visits(m, path, P3(0.0, 0.0, 0.0), 1e-9));
  // the apex record is a genuine corner of some incident face (no faking)
  for (const SurfacePoint& sp : path)
  {
    TASK_CHECK(sp.face.is_valid());
    TASK_CHECK(valid_bary(sp.bary));
    if ((world_point(m, sp) - m.point(O)).norm() <= 1e-9)
    {
      bool owns = false;
      for (auto fv = m.cfv_iter(sp.face); fv.is_valid(); ++fv)
        if (*fv == O) owns = true;
      TASK_CHECK(owns);
    }
  }

  // concave boundary (reentrant L corner, interior angle 270 degrees)
  Mesh lm;
  Mesh::VertexHandle v[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (!(i == 2 && j == 2))
        v[i][j] = lm.add_vertex(P3(i, j, 0.0));
  auto cell = [&](int i, int j)
  {
    auto a = v[i][j], b = v[i+1][j], c = v[i+1][j+1], d = v[i][j+1];
    lm.add_face(a, b, c); lm.add_face(a, c, d);
  };
  cell(0, 0); cell(1, 0); cell(0, 1);
  Solver le(lm);
  TASK_CHECK(le.solve(v[0][2]));
  Mesh::FaceHandle tf = face_with(lm, v[2][0]);
  std::vector<SurfacePoint> lpath; double llen;
  TASK_CHECK(le.trace(tf, corner_bary(lm, tf, v[2][0]), lpath, llen));
  TASK_CHECK(near_eq(llen, 2.0 * std::sqrt(2.0), 1e-9));
  TASK_CHECK(path_visits(lm, lpath, P3(1.0, 1.0, 0.0), 1e-9));
  TASK_CHECK(max_cross_gap(lm, lpath) <= 1e-8);
  return 0;
}

int case_unreachable_trace_untouched()
{
  Mesh m;
  auto a = m.add_vertex(P3(0, 0, 0));
  auto b = m.add_vertex(P3(1, 0, 0));
  auto c = m.add_vertex(P3(0, 1, 0));
  auto f0 = m.add_face(a, b, c);
  auto d = m.add_vertex(P3(5, 0, 0));
  auto e2 = m.add_vertex(P3(6, 0, 0));
  auto f = m.add_vertex(P3(5, 1, 0));
  auto f1 = m.add_face(d, e2, f);

  Solver e(m);
  TASK_CHECK(e.solve(a));

  std::vector<SurfacePoint> sentinel(1);
  sentinel[0].face = f0;
  sentinel[0].bary = {{1.0, 0.0, 0.0}};
  std::vector<SurfacePoint> out = sentinel;
  double len = -99.0;
  TASK_CHECK(!e.trace(f1, corner_bary(m, f1, d), out, len));
  TASK_CHECK(out.size() == 1);
  TASK_CHECK(out[0].face == sentinel[0].face);
  TASK_CHECK(len == -99.0);
  return 0;
}

//============================ registration ==================================

} // namespace

const struct task_case task_cases[] =
{
  { "distance", "planar_grid_analytic",        case_planar_grid_analytic },
  { "distance", "folded_band_analytic",        case_folded_band_analytic },
  { "distance", "hole_detour_analytic",        case_hole_detour_analytic },
  { "distance", "cone_analytic",               case_cone_analytic },
  { "distance", "saddle_via_apex",             case_saddle_via_apex },
  { "distance", "disconnected_infinity",       case_disconnected_infinity },
  { "distance", "repeated_solve_isolation",    case_repeated_solve_isolation },
  { "distance", "snapshot_independence",       case_snapshot_independence },
  { "distance", "exceptions_and_validation",   case_exceptions_and_validation },

  { "windows",  "envelope_diagonal_invariance",case_envelope_diagonal_invariance },
  { "windows",  "wavefront_competition",       case_wavefront_competition },
  { "windows",  "convex_vertex_not_shortcut",  case_convex_vertex_not_shortcut },

  { "trace",    "planar_trace_continuity",     case_planar_trace_continuity },
  { "trace",    "edge_target_duplicated_record", case_edge_target_duplicated_record },
  { "trace",    "folded_trace_to_source",      case_folded_trace_to_source },
  { "trace",    "vertex_event_face_ownership", case_vertex_event_trace_face_ownership },
  { "trace",    "unreachable_trace_untouched", case_unreachable_trace_untouched },
};

const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
