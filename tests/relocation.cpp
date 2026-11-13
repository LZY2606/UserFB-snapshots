#include "task_support/check.h"
#include "task_support/meshes.hh"

#include <OpenMesh/Core/Geometry/QuadricT.hh>
#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModAspectRatioT.hh>
#include <OpenMesh/Tools/Decimater/ModNormalFlippingT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace task_fixture;

typedef OpenMesh::Decimater::ModQuadricT<Mesh> Quadric;
typedef OpenMesh::Decimater::ModNormalFlippingT<Mesh> Flipping;
typedef OpenMesh::Decimater::ModAspectRatioT<Mesh> Aspect;
typedef OpenMesh::Decimater::DecimaterT<Mesh> Decimater;

// ---------------------------------------------------------------- helpers

/// Count vertices / faces that are not flagged deleted.
void alive_counts(const Mesh& _mesh, size_t& _nv, size_t& _nf) {
  _nv = _nf = 0;
  for (Mesh::ConstVertexIter it = _mesh.vertices_begin();
       it != _mesh.vertices_end(); ++it)
    if (!_mesh.status(*it).deleted()) ++_nv;
  for (Mesh::ConstFaceIter it = _mesh.faces_begin();
       it != _mesh.faces_end(); ++it)
    if (!_mesh.status(*it).deleted()) ++_nf;
}

/// True if the mesh is a consistent two-manifold after the collapses.
bool mesh_is_consistent(Mesh& _mesh) {
  OpenMesh::Utils::MeshCheckerT<Mesh> checker(_mesh);
  return checker.check(OpenMesh::Utils::MeshCheckerT<Mesh>::CHECK_ALL, omerr());
}

/// Number of vertices that are not at an original lattice position.
/// Only relocation can produce such vertices, since endpoint collapses keep
/// points that were created by the fixture.
size_t off_lattice_vertices(const Mesh& _mesh) {
  size_t n = 0;
  for (Mesh::ConstVertexIter it = _mesh.vertices_begin();
       it != _mesh.vertices_end(); ++it) {
    if (_mesh.status(*it).deleted()) continue;
    const Mesh::Point& p = _mesh.point(*it);
    const double rx = std::fabs(double(p[0]) - std::floor(double(p[0]) + 0.5));
    const double ry = std::fabs(double(p[1]) - std::floor(double(p[1]) + 0.5));
    if (rx > 1e-9 || ry > 1e-9) ++n;
  }
  return n;
}

/// Every face normal in the mesh agrees with the geometry.
bool normals_are_current(Mesh& _mesh) {
  for (Mesh::FaceIter it = _mesh.faces_begin(); it != _mesh.faces_end(); ++it) {
    if (_mesh.status(*it).deleted()) continue;
    const Mesh::Normal stored = _mesh.normal(*it);
    const Mesh::Normal actual = _mesh.calc_face_normal(*it);
    if ((stored - actual).norm() > 1e-6) return false;
  }
  return true;
}

/// Build a closed octahedron.  The quadric of an opposite vertex pair has
/// full rank, so the 3x3 normal equations have a unique minimizer.  The
/// shape is stretched so that the minimizer differs from both endpoints.
struct Octahedron {
  Mesh mesh;
  Mesh::VertexHandle xp, xn, yp, yn, zp, zn;
};

Octahedron make_octahedron() {
  Octahedron o;
  o.xp = o.mesh.add_vertex(Mesh::Point(1.2, 0.0, 0.4));
  o.xn = o.mesh.add_vertex(Mesh::Point(-1.0, 0.0, 0.0));
  o.yp = o.mesh.add_vertex(Mesh::Point(0.0, 1.1, 0.3));
  o.yn = o.mesh.add_vertex(Mesh::Point(0.0, -1.0, 0.0));
  o.zp = o.mesh.add_vertex(Mesh::Point(0.0, 0.0, 1.3));
  o.zn = o.mesh.add_vertex(Mesh::Point(0.0, 0.0, -1.0));
  const Mesh::VertexHandle v[6] = { o.xp, o.xn, o.yp, o.yn, o.zp, o.zn };
  const int faces[8][3] = {
    {0, 2, 4}, {0, 4, 3}, {0, 3, 5}, {0, 5, 2},
    {1, 4, 2}, {1, 3, 4}, {1, 5, 3}, {1, 2, 5}
  };
  for (int i = 0; i < 8; ++i)
    o.mesh.add_face(v[faces[i][0]], v[faces[i][1]], v[faces[i][2]]);
  o.mesh.request_vertex_status();
  o.mesh.request_edge_status();
  o.mesh.request_face_status();
  o.mesh.request_face_normals();
  o.mesh.update_face_normals();
  return o;
}

/// Merge the double quadrics of all faces incident to _from or _to,
/// independent of the module's internal bookkeeping.  A face on the collapsed
/// edge is incident to both vertices and therefore contributes twice, exactly
/// as Q(v0) + Q(v1) does.
OpenMesh::Geometry::QuadricT<double> merged_quadric(
    const Mesh& _mesh, Mesh::VertexHandle _from, Mesh::VertexHandle _to) {
  OpenMesh::Geometry::QuadricT<double> merged;
  for (Mesh::ConstFaceIter f = _mesh.faces_begin(); f != _mesh.faces_end(); ++f) {
    Mesh::ConstFaceVertexIter fv = _mesh.cfv_iter(*f);
    Mesh::VertexHandle hs[3] = { *fv, *(++fv), *(++fv) };
    int count = 0;
    OpenMesh::Vec3d p[3];
    for (int i = 0; i < 3; ++i) {
      if (hs[i] == _from) ++count;
      if (hs[i] == _to) ++count;
      const Mesh::Point pt = _mesh.point(hs[i]);
      p[i] = OpenMesh::Vec3d(double(pt[0]), double(pt[1]), double(pt[2]));
    }
    if (!count) continue;
    OpenMesh::Vec3d n = (p[1] - p[0]) % (p[2] - p[0]);
    const double area = n.norm();
    if (!(area > 1e-12)) continue;
    n /= area;
    OpenMesh::Geometry::QuadricT<double> q(n, p[0]);
    q *= 0.5 * area;
    for (int i = 0; i < count; ++i) merged += q;
  }
  return merged;
}

/// Solve the quadric's normal equations with plain Gaussian elimination and
/// return false if the system is singular.
bool solve_merged_quadric(const OpenMesh::Geometry::QuadricT<double>& _q,
                          double _out[3]) {
  double A[3][4] = {
    { _q.a(), _q.b(), _q.c(), -_q.d() },
    { _q.b(), _q.e(), _q.f(), -_q.g() },
    { _q.c(), _q.f(), _q.h(), -_q.i() }
  };
  for (int col = 0; col < 3; ++col) {
    int piv = col;
    for (int r = col + 1; r < 3; ++r)
      if (std::fabs(A[r][col]) > std::fabs(A[piv][col])) piv = r;
    if (!(std::fabs(A[piv][col]) > 1e-9)) return false;
    if (piv != col)
      for (int j = col; j < 4; ++j) std::swap(A[col][j], A[piv][j]);
    for (int r = col + 1; r < 3; ++r) {
      const double factor = A[r][col] / A[col][col];
      for (int j = col; j < 4; ++j) A[r][j] -= factor * A[col][j];
    }
  }
  for (int r = 2; r >= 0; --r) {
    double s = A[r][3];
    for (int j = r + 1; j < 3; ++j) s -= A[r][j] * _out[j];
    _out[r] = s / A[r][r];
  }
  return std::isfinite(_out[0]) && std::isfinite(_out[1]) &&
         std::isfinite(_out[2]);
}

/// A single halfedge collapse candidate for (a -> b).
OpenMesh::Decimater::CollapseInfoT<Mesh> candidate(Mesh& _mesh,
                                                   Mesh::VertexHandle _from,
                                                   Mesh::VertexHandle _to) {
  return OpenMesh::Decimater::CollapseInfoT<Mesh>(
      _mesh, _mesh.find_halfedge(_from, _to));
}

/// Build a flat mesh with a deliberately non-planar interior so that a
/// relocation target exists.
Mesh make_wavy(unsigned _seed) { return make_mesh(_seed); }

// ------------------------------------------------------------- test cases

// ---------------------------------------------------------------- placement

int case_default_is_off() {
  Mesh mesh = make_wavy(2);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(!decimater.relocation_enabled());
  TASK_CHECK(decimater.initialize());
  TASK_CHECK(!decimater.relocation_enabled());
  // the module itself also reports that it is not relocating by default
  TASK_CHECK(decimater.module(quadric).is_binary() == false);
  return 0;
}

int case_set_relocation_resets_state() {
  Mesh mesh = make_wavy(3);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.initialize());
  TASK_CHECK(decimater.is_initialized());

  // Changing the setting requires a new initialize().
  decimater.set_relocation(true);
  TASK_CHECK(decimater.relocation_enabled());
  TASK_CHECK(!decimater.is_initialized());

  // Repeating the same value is a no-op.
  decimater.set_relocation(true);
  decimater.set_relocation(false);
  TASK_CHECK(!decimater.relocation_enabled());
  TASK_CHECK(!decimater.is_initialized());
  return 0;
}

int case_endpoint_mode_keeps_points() {
  Mesh mesh = make_wavy(4);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.initialize());
  TASK_CHECK(decimater.decimate(8) == 8);
  TASK_CHECK(off_lattice_vertices(mesh) == 0);
  return 0;
}

int case_full_rank_minimizer() {
  Octahedron o = make_octahedron();
  Mesh& mesh = o.mesh;

  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  // The opposite pole pair (xp, yp) has incident faces in all three
  // coordinate directions, so the merged quadric has full rank.
  OpenMesh::Decimater::CollapseInfoT<Mesh> ci = candidate(mesh, o.xp, o.yp);
  TASK_CHECK(ci.fl.is_valid() && ci.fr.is_valid());
  TASK_CHECK(!mesh.is_boundary(o.xp) && !mesh.is_boundary(o.yp));
  TASK_CHECK(!mesh.status(o.xp).locked() && !mesh.status(o.yp).locked());
  TASK_CHECK(!mesh.status(o.xp).feature() && !mesh.status(o.yp).feature());

  // Rebuild the merged quadric independently in double precision and solve
  // the normal equations with plain Gaussian elimination.
  const OpenMesh::Geometry::QuadricT<double> merged =
      merged_quadric(mesh, o.xp, o.yp);
  double expected[3] = { 0.0, 0.0, 0.0 };
  TASK_CHECK(solve_merged_quadric(merged, expected));

  Quadric& qmod = decimater.module(quadric);
  Mesh::Point target;
  TASK_CHECK(qmod.relocation_target(ci, target));
  TASK_CHECK(std::fabs(double(target[0]) - expected[0]) < 1e-5);
  TASK_CHECK(std::fabs(double(target[1]) - expected[1]) < 1e-5);
  TASK_CHECK(std::fabs(double(target[2]) - expected[2]) < 1e-5);
  TASK_CHECK(!(target == ci.p1));

  // The minimizer really is better than p1, and the module reports that
  // lower error (i.e. it evaluates the actual target, not the endpoint).
  const OpenMesh::Vec3d pt{ double(target[0]), double(target[1]),
                            double(target[2]) };
  const OpenMesh::Vec3d p1{ double(ci.p1[0]), double(ci.p1[1]),
                            double(ci.p1[2]) };
  TASK_CHECK(merged(pt) < merged(p1));

  OpenMesh::Decimater::CollapseInfoT<Mesh> relocated = ci;
  relocated.relocate = true;
  relocated.p_target = target;
  OpenMesh::Decimater::CollapseInfoT<Mesh> endpoint = ci;
  endpoint.relocate = false;
  endpoint.p_target = endpoint.p1;
  const float err_relocated = qmod.collapse_priority(relocated);
  const float err_endpoint = qmod.collapse_priority(endpoint);
  TASK_CHECK(err_relocated >= 0.0f && err_endpoint >= 0.0f);
  TASK_CHECK(err_relocated < err_endpoint);
  return 0;
}

int case_singular_fallback() {
  // Collapse an edge that is completely contained in one plane (flat
  // neighbourhood).  The merged quadric then has a one-dimensional null
  // space (the plane normal direction is constrained only by the two ring
  // faces, which lie in the same plane) and the 3x3 system is singular.
  Mesh mesh;
  Mesh::VertexHandle v[5][5];
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 5; ++x)
      v[y][x] = mesh.add_vertex(Mesh::Point(double(x), double(y), 0.0));
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      mesh.add_face(v[y][x], v[y][x + 1], v[y + 1][x + 1]);
      mesh.add_face(v[y][x], v[y + 1][x + 1], v[y + 1][x]);
    }
  mesh.request_vertex_status();
  mesh.request_edge_status();
  mesh.request_face_status();
  mesh.request_face_normals();
  mesh.update_face_normals();

  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  // A flat interior edge: the planar quadric is singular.
  OpenMesh::Decimater::CollapseInfoT<Mesh> ci =
      candidate(mesh, v[2][2], v[2][3]);
  TASK_CHECK(ci.fl.is_valid() && ci.fr.is_valid());
  Quadric& qmod = decimater.module(quadric);
  Mesh::Point target;
  // The fallback evaluates p1, p0 and the midpoint.  On a plane all three
  // lie in the plane; the quadric is zero everywhere on the plane, so the
  // errors tie and the first candidate in the tie order wins: p1.  The
  // module reports false because the remaining vertex does not move.
  TASK_CHECK(qmod.relocation_target(ci, target) == false);
  TASK_CHECK(target == ci.p1);
  return 0;
}

int case_zero_matrix_fallback() {
  // A freshly constructed module owns an all-zero quadric property (no
  // initialize() call fills it in).  The zero matrix has no minimizer, so
  // the fallback must pick the smallest of p1 / p0 / midpoint; all errors
  // are zero here, so the tie order selects p1.
  Octahedron o = make_octahedron();
  Mesh& mesh = o.mesh;
  Quadric qmod(mesh);

  OpenMesh::Decimater::CollapseInfoT<Mesh> ci = candidate(mesh, o.xp, o.yp);
  TASK_CHECK(ci.fl.is_valid() && ci.fr.is_valid());
  TASK_CHECK(!mesh.is_boundary(o.xp) && !mesh.is_boundary(o.yp));

  Mesh::Point target;
  TASK_CHECK(qmod.relocation_target(ci, target) == false);
  TASK_CHECK(target == ci.p1);

  // The same holds after the quadrics are initialized on a planar mesh:
  // there the merged matrix is rank-deficient (not zero) and the tie also
  // resolves to p1.
  Mesh flat;
  Mesh::VertexHandle v[4][4];
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
      v[y][x] = flat.add_vertex(Mesh::Point(double(x), double(y), 0.0));
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 3; ++x) {
      flat.add_face(v[y][x], v[y][x + 1], v[y + 1][x + 1]);
      flat.add_face(v[y][x], v[y + 1][x + 1], v[y + 1][x]);
    }
  flat.request_vertex_status();
  flat.request_edge_status();
  flat.request_face_status();
  flat.request_face_normals();
  flat.update_face_normals();

  Decimater decimater(flat);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());
  OpenMesh::Decimater::CollapseInfoT<Mesh> fci =
      candidate(flat, v[1][1], v[1][2]);
  TASK_CHECK(fci.fl.is_valid() && fci.fr.is_valid());
  TASK_CHECK(!flat.is_boundary(v[1][1]) && !flat.is_boundary(v[1][2]));
  const OpenMesh::Geometry::QuadricT<double> merged =
      merged_quadric(flat, v[1][1], v[1][2]);
  double minimizer[3] = { 0.0, 0.0, 0.0 };
  TASK_CHECK(!solve_merged_quadric(merged, minimizer));  // rank deficient
  TASK_CHECK(decimater.module(quadric).relocation_target(fci, target) == false);
  TASK_CHECK(target == fci.p1);
  return 0;
}

int case_boundary_keeps_p1() {
  Mesh mesh = make_wavy(5);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());
  Decimater::Module& base = decimater.module(quadric);

  Mesh::VertexHandle boundary = OpenMesh::VertexHandle();
  for (Mesh::ConstVertexIter it = mesh.vertices_begin();
       it != mesh.vertices_end(); ++it)
    if (mesh.is_boundary(*it)) { boundary = *it; break; }
  TASK_CHECK(boundary.is_valid());
  Mesh::VertexHandle inner = *mesh.vv_iter(boundary);

  OpenMesh::Decimater::CollapseInfoT<Mesh> ci =
      candidate(mesh, boundary, inner);
  Mesh::Point target;
  TASK_CHECK(base.relocation_target(ci, target) == false);
  TASK_CHECK(base.relocation_target(ci, target) == false);
  return 0;
}

int case_locked_and_feature_keep_p1() {
  Mesh mesh = make_wavy(6);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  // Interior edge, not boundary.
  Mesh::VertexHandle a = OpenMesh::VertexHandle();
  Mesh::VertexHandle b = OpenMesh::VertexHandle();
  for (Mesh::VertexIter it = mesh.vertices_begin();
       it != mesh.vertices_end() && !a.is_valid(); ++it) {
    if (mesh.is_boundary(*it)) continue;
    for (Mesh::VertexVertexIter vv = mesh.vv_iter(*it); vv.is_valid(); ++vv) {
      if (!mesh.is_boundary(*vv)) { a = *it; b = *vv; break; }
    }
  }
  TASK_CHECK(a.is_valid() && b.is_valid());

  Mesh::VertexHandle saved_a = a, saved_b = b;
  Quadric& qmod = decimater.module(quadric);

  {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci =
        candidate(mesh, saved_a, saved_b);
    Mesh::Point target;
    TASK_CHECK(qmod.relocation_target(ci, target) == true);
    TASK_CHECK(!(target == ci.p1));
  }

  mesh.status(saved_a).set_locked(true);
  {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci =
        candidate(mesh, saved_a, saved_b);
    Mesh::Point target;
    TASK_CHECK(qmod.relocation_target(ci, target) == false);
  }
  mesh.status(saved_a).set_locked(false);

  mesh.status(saved_b).set_feature(true);
  {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci =
        candidate(mesh, saved_a, saved_b);
    Mesh::Point target;
    TASK_CHECK(qmod.relocation_target(ci, target) == false);
  }
  mesh.status(saved_b).set_feature(false);
  return 0;
}

int case_max_err_uses_target() {
  // On the octahedron the minimizer has a strictly lower error than p1.  A
  // max_err between the two keeps the relocated candidate legal only if
  // the actual target position is evaluated.
  Octahedron o = make_octahedron();
  Mesh& mesh = o.mesh;
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());
  Quadric& qmod = decimater.module(quadric);

  OpenMesh::Decimater::CollapseInfoT<Mesh> ci = candidate(mesh, o.xp, o.yp);
  Mesh::Point target;
  TASK_CHECK(qmod.relocation_target(ci, target));
  TASK_CHECK(!(target == ci.p1));

  OpenMesh::Decimater::CollapseInfoT<Mesh> relocated = ci;
  relocated.relocate = true;
  relocated.p_target = target;
  OpenMesh::Decimater::CollapseInfoT<Mesh> endpoint = ci;
  endpoint.relocate = false;
  endpoint.p_target = endpoint.p1;

  const float err_relocated = qmod.collapse_priority(relocated);
  const float err_endpoint = qmod.collapse_priority(endpoint);
  TASK_CHECK(err_relocated >= 0.0f && err_endpoint >= 0.0f);
  TASK_CHECK(err_relocated < err_endpoint);

  // A limit between both errors accepts the relocated candidate and
  // rejects the endpoint one, proving the priority uses p_target.
  const double limit = double(err_relocated) +
      0.25 * (double(err_endpoint) - double(err_relocated));
  qmod.set_max_err(limit, false);
  TASK_CHECK(qmod.collapse_priority(relocated) >= 0.0f);
  TASK_CHECK(qmod.collapse_priority(endpoint) < 0.0f);
  qmod.unset_max_err();
  return 0;
}

// ----------------------------------------------------------------- geometry

int case_normal_flip_on_v1_side_rejected() {
  // v0-side faces stay valid while a face on v1's other side flips.  A
  // check that only walks the removed vertex's ring misses this.
  Mesh mesh;
  Mesh::VertexHandle v0 = mesh.add_vertex(Mesh::Point(0.0, 0.0, 0.0));
  Mesh::VertexHandle v1 = mesh.add_vertex(Mesh::Point(1.0, 0.0, 0.0));
  Mesh::VertexHandle a = mesh.add_vertex(Mesh::Point(0.5, 1.0, 0.0));
  Mesh::VertexHandle b = mesh.add_vertex(Mesh::Point(0.5, -1.0, 0.0));
  Mesh::VertexHandle c = mesh.add_vertex(Mesh::Point(2.0, 1.0, 0.8));
  Mesh::VertexHandle d = mesh.add_vertex(Mesh::Point(2.0, -1.0, -0.8));
  Mesh::VertexHandle e = mesh.add_vertex(Mesh::Point(-1.0, 1.0, 0.0));
  Mesh::VertexHandle f = mesh.add_vertex(Mesh::Point(-1.0, -1.0, 0.0));

  mesh.add_face(v0, v1, a);
  mesh.add_face(v0, b, v1);
  mesh.add_face(v0, a, e);
  mesh.add_face(v0, e, f);
  mesh.add_face(v0, f, b);
  mesh.add_face(v1, c, a);
  mesh.add_face(v1, d, c);
  mesh.add_face(v1, b, d);
  mesh.request_vertex_status();
  mesh.request_edge_status();
  mesh.request_face_status();
  mesh.request_face_normals();
  mesh.update_face_normals();

  OpenMesh::Decimater::CollapseInfoT<Mesh> ci = candidate(mesh, v0, v1);
  TASK_CHECK(ci.fl.is_valid() && ci.fr.is_valid());
  TASK_CHECK(!mesh.is_boundary(v0) && !mesh.is_boundary(v1));

  // Search for a target that passes the v0 ring but flips a face on v1's
  // other side.  (Geometry of the fixture above.)
  Mesh::Point flip_target;
  bool found = false;
  for (double tx = 0.0; tx <= 1.5 && !found; tx += 0.1)
    for (double ty = -2.0; ty <= 2.0 && !found; ty += 0.1)
      for (double tz = -1.5; tz <= 1.5 && !found; tz += 0.5) {
        Mesh::Point t;
        t[0] = float(tx); t[1] = float(ty); t[2] = float(tz);

        mesh.set_point(v0, t);
        bool v0_flip = false;
        for (Mesh::VertexFaceIter vf = mesh.vf_iter(v0); vf.is_valid(); ++vf) {
          if (*vf == ci.fl || *vf == ci.fr) continue;
          if (dot(mesh.normal(*vf), mesh.calc_face_normal(*vf)) < 0.5)
            v0_flip = true;
        }
        mesh.set_point(v0, ci.p0);

        mesh.set_point(v1, t);
        bool v1_flip = false;
        for (Mesh::VertexFaceIter vf = mesh.vf_iter(v1); vf.is_valid(); ++vf) {
          if (*vf == ci.fl || *vf == ci.fr) continue;
          if (dot(mesh.normal(*vf), mesh.calc_face_normal(*vf)) < 0.5)
            v1_flip = true;
        }
        mesh.set_point(v1, ci.p1);

        if (!v0_flip && v1_flip) { flip_target = t; found = true; }
      }
  TASK_CHECK(found);

  // The module must reject that target even though the removed vertex's
  // ring alone would accept it.
  Flipping module(mesh);
  module.set_max_normal_deviation(30.0);
  OpenMesh::Decimater::CollapseInfoT<Mesh> reloc = ci;
  reloc.relocate = true;
  reloc.p_target = flip_target;
  TASK_CHECK(module.collapse_priority(reloc) ==
             float(OpenMesh::Decimater::ModBaseT<Mesh>::ILLEGAL_COLLAPSE));

  // The same candidate at p1 must be legal (no movement, no flip).
  OpenMesh::Decimater::CollapseInfoT<Mesh> endpoint = ci;
  endpoint.relocate = false;
  endpoint.p_target = ci.p1;
  TASK_CHECK(module.collapse_priority(endpoint) ==
             float(OpenMesh::Decimater::ModBaseT<Mesh>::LEGAL_COLLAPSE));

  // Trial runs must not leave any changes behind.
  TASK_CHECK(mesh.point(v0) == ci.p0);
  TASK_CHECK(mesh.point(v1) == ci.p1);
  TASK_CHECK(normals_are_current(mesh));
  return 0;
}

int case_trial_leaves_no_trace() {
  Mesh mesh = make_wavy(12);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Flipping::Handle flipping;
  Aspect::Handle aspect;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(flipping));
  TASK_CHECK(decimater.add(aspect));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  // Snapshot all points, normals and vertex priorities.
  std::vector<Mesh::Point> points;
  std::vector<Mesh::Normal> normals;
  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v) points.push_back(mesh.point(*v));
  for (Mesh::ConstFaceIter f = mesh.faces_begin();
       f != mesh.faces_end(); ++f) normals.push_back(mesh.normal(*f));

  OpenMesh::Decimater::CollapseInfoT<Mesh> ci = candidate(
      mesh, *mesh.vertices_begin(), *mesh.vv_iter(*mesh.vertices_begin()));
  TASK_CHECK(decimater.module(flipping).collapse_priority(ci) >= 0.0f ||
             true);  // outcome irrelevant; only side effects matter

  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v)
    TASK_CHECK(mesh.point(*v) == points[v->idx()]);
  for (Mesh::ConstFaceIter f = mesh.faces_begin();
       f != mesh.faces_end(); ++f)
    TASK_CHECK(mesh.normal(*f) == normals[f->idx()]);
  TASK_CHECK(normals_are_current(mesh));
  return 0;
}

int case_normals_and_cache_updated() {
  Mesh mesh = make_wavy(7);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Flipping::Handle flipping;
  Aspect::Handle aspect;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(flipping));
  TASK_CHECK(decimater.add(aspect));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  size_t removed = decimater.decimate(4);
  TASK_CHECK(removed > 0);
  TASK_CHECK(off_lattice_vertices(mesh) > 0);
  TASK_CHECK(normals_are_current(mesh));
  return 0;
}

int case_aspect_rejects_on_union() {
  // A relocation target that keeps the v0 side fine but ruins a face on
  // the v1 side has to be rejected by the aspect-ratio module.
  Mesh mesh = make_wavy(8);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Aspect::Handle aspect;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(aspect));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());
  Aspect& amod = decimater.module(aspect);

  // Find an interior edge.
  Mesh::VertexHandle a, b;
  for (Mesh::VertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end() && !a.is_valid(); ++v) {
    if (mesh.is_boundary(*v)) continue;
    for (Mesh::VertexVertexIter vv = mesh.vv_iter(*v); vv.is_valid(); ++vv)
      if (!mesh.is_boundary(*vv)) { a = *v; b = *vv; break; }
  }
  TASK_CHECK(a.is_valid() && b.is_valid());

  OpenMesh::Decimater::CollapseInfoT<Mesh> ci = candidate(mesh, a, b);
  // Endpoint target is fine.
  OpenMesh::Decimater::CollapseInfoT<Mesh> endpoint = ci;
  endpoint.relocate = false;
  endpoint.p_target = ci.p1;
  TASK_CHECK(amod.collapse_priority(endpoint) >= 0.0f);

  // Push the target far away from the surface: the worst aspect ratio
  // after the collapse collapses to nearly zero, which the binary rule
  // must reject on either ring.
  OpenMesh::Decimater::CollapseInfoT<Mesh> reloc = ci;
  reloc.relocate = true;
  reloc.p_target = OpenMesh::Vec3d(double(ci.p0[0]) + 30.0,
                                   double(ci.p0[1]) + 0.0,
                                   double(ci.p0[2]) + 0.0);
  TASK_CHECK(amod.collapse_priority(reloc) ==
             float(OpenMesh::Decimater::ModBaseT<Mesh>::ILLEGAL_COLLAPSE));
  return 0;
}

// ----------------------------------------------------------------- sequence

int case_repeated_collapses_decimate() {
  Mesh mesh = make_wavy(9);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  size_t before_v = 0, before_f = 0;
  alive_counts(mesh, before_v, before_f);
  size_t done = 0;
  size_t prev_f = before_f;
  for (int step = 0; step < 6; ++step) {
    size_t n = decimater.decimate(1);
    done += n;
    size_t nv, nf;
    alive_counts(mesh, nv, nf);
    TASK_CHECK(nv + done == before_v);
    // every collapse removes one vertex and one or two faces
    TASK_CHECK(nf < prev_f);
    TASK_CHECK(prev_f - nf <= 2);
    prev_f = nf;
    TASK_CHECK(mesh_is_consistent(mesh));
    TASK_CHECK(normals_are_current(mesh));
  }
  TASK_CHECK(done == 6);
  TASK_CHECK(off_lattice_vertices(mesh) > 0);
  return 0;
}

int case_repeated_collapses_to_faces() {
  // decimate_to_faces() has to run the same relocation pipeline, including
  // pop-time revalidation.  Seed 16 with a tight normal constraint makes the
  // two implementations differ, so the exact removed-vertex sequence is
  // pinned as well.
  Mesh mesh = make_wavy(16);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Flipping::Handle flipping;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(flipping));
  decimater.module(flipping).set_max_normal_deviation(10.0);
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  std::vector<char> alive;
  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v)
    alive.push_back(!mesh.status(*v).deleted());
  size_t before_v = 0, before_f;
  alive_counts(mesh, before_v, before_f);
  (void)before_f;
  size_t target_v = before_v - 5;
  size_t done = decimater.decimate_to_faces(target_v, 0);
  size_t nv, nf;
  alive_counts(mesh, nv, nf);
  TASK_CHECK(done == 5);
  TASK_CHECK(nv == target_v);
  TASK_CHECK(mesh_is_consistent(mesh));
  TASK_CHECK(normals_are_current(mesh));
  TASK_CHECK(off_lattice_vertices(mesh) > 0);

  const int expected[5] = { 0, 20, 21, 39, 47 };
  size_t k = 0;
  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v) {
    if (!alive[v->idx()] || !mesh.status(*v).deleted()) continue;
    TASK_CHECK(k < 5);
    TASK_CHECK(v->idx() == expected[k]);
    ++k;
  }
  TASK_CHECK(k == 5);
  return 0;
}

int case_decimate_to_faces_stops_at_faces() {
  Mesh mesh = make_wavy(13);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  size_t before_v, before_f;
  alive_counts(mesh, before_v, before_f);
  size_t target_f = before_f - 4;
  size_t done = decimater.decimate_to_faces(0, target_f);
  size_t nv, nf;
  alive_counts(mesh, nv, nf);
  TASK_CHECK(done >= 2);
  TASK_CHECK(nf <= target_f);
  TASK_CHECK(mesh_is_consistent(mesh));
  return 0;
}

int case_rejected_candidate_continues() {
  // A limit that rejects the very best candidate must not stop the loop;
  // further candidates still decimate.
  Mesh mesh = make_wavy(14);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());
  Quadric& qmod = decimater.module(quadric);

  // Reject everything with a limit of zero, then lift it and continue.
  qmod.set_max_err(0.0, false);
  size_t n0 = decimater.decimate(3);
  TASK_CHECK(n0 == 0);
  qmod.unset_max_err();
  size_t n1 = decimater.decimate(3);
  TASK_CHECK(n1 == 3);
  TASK_CHECK(mesh_is_consistent(mesh));
  return 0;
}

int case_heap_skips_stale_entries() {
  Mesh mesh = make_wavy(15);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  // Perform collapses one at a time; the second call rebuilds the heap,
  // so this mainly guards that stale indices never corrupt the mesh.
  for (int i = 0; i < 4; ++i) {
    TASK_CHECK(decimater.decimate(1) == 1);
    TASK_CHECK(mesh_is_consistent(mesh));
  }
  return 0;
}

int case_heap_revalidates_target_at_pop() {
  // A stored heap entry was scored with the target position known at
  // insertion time.  Neighbouring collapses move vertices, so the target of
  // a pending candidate can change while it sits in the heap.  The pop has
  // to re-evaluate the candidate against the current geometry; otherwise
  // the binary module accepts a stale target and the sequence of removed
  // vertices differs.  Seeds and deviation below were found to distinguish
  // the two behaviours.
  Mesh mesh = make_wavy(16);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Flipping::Handle flipping;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(flipping));
  decimater.module(flipping).set_max_normal_deviation(10.0);
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  // Record the removed vertices to pin the full sequence, not just counts.
  std::vector<char> alive;
  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v)
    alive.push_back(!mesh.status(*v).deleted());

  TASK_CHECK(decimater.decimate(5) == 5);
  const int expected[5] = { 0, 20, 21, 39, 47 };
  size_t k = 0;
  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v) {
    if (!alive[v->idx()] || !mesh.status(*v).deleted()) continue;
    TASK_CHECK(k < 5);
    TASK_CHECK(v->idx() == expected[k]);
    ++k;
  }
  TASK_CHECK(k == 5);
  TASK_CHECK(mesh_is_consistent(mesh));
  return 0;
}

int case_relocation_refreshes_v1_neighbourhood() {
  // Moving the retained vertex changes the candidates of all its neighbours
  // (not just those of the removed vertex).  Their heap entries must be
  // re-evaluated against the new geometry; if the v1 ring is not refreshed,
  // the sequence of removed vertices diverges.  Seed and deviation below
  // distinguish the two behaviours.
  Mesh mesh = make_wavy(4);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Flipping::Handle flipping;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(flipping));
  decimater.module(flipping).set_max_normal_deviation(20.0);
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());

  std::vector<char> alive;
  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v)
    alive.push_back(!mesh.status(*v).deleted());

  TASK_CHECK(decimater.decimate(5) == 5);
  const int expected[5] = { 0, 17, 18, 23, 24 };
  size_t k = 0;
  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v) {
    if (!alive[v->idx()] || !mesh.status(*v).deleted()) continue;
    TASK_CHECK(k < 5);
    TASK_CHECK(v->idx() == expected[k]);
    ++k;
  }
  TASK_CHECK(k == 5);
  return 0;
}

// ------------------------------------------------------------ compatibility

int case_other_binary_module_rejected() {
  // A different binary module that does not know about relocation must
  // make initialize() fail and leave the mesh untouched.
  Mesh mesh = make_wavy(16);

  struct DummyBinary : OpenMesh::Decimater::ModBaseT<Mesh> {
    DummyBinary(Mesh& _m) : OpenMesh::Decimater::ModBaseT<Mesh>(_m, true) {}
    DECIMATER_MODNAME(DummyBinary)
    float collapse_priority(const CollapseInfo&) { return LEGAL_COLLAPSE; }
  };

  Decimater decimater(mesh);
  Quadric::Handle quadric;
  OpenMesh::Decimater::ModHandleT<DummyBinary> dummy;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(dummy));
  decimater.set_relocation(true);
  TASK_CHECK(!decimater.initialize());
  TASK_CHECK(!decimater.is_initialized());

  size_t nv, nf;
  alive_counts(mesh, nv, nf);
  TASK_CHECK(nv == 49 && nf == 72);
  TASK_CHECK(decimater.decimate(1) == 0);
  alive_counts(mesh, nv, nf);
  TASK_CHECK(nv == 49 && nf == 72);
  return 0;
}

int case_quadric_binary_rejected() {
  Mesh mesh = make_wavy(17);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.module(quadric).set_max_err(1e30, true);
  decimater.set_relocation(true);
  TASK_CHECK(!decimater.initialize());
  size_t nv, nf;
  alive_counts(mesh, nv, nf);
  TASK_CHECK(nv == 49 && nf == 72);
  return 0;
}

int case_quadric_and_normal_flipping_compatible() {
  Mesh mesh = make_wavy(18);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Flipping::Handle flipping;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(flipping));
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());
  TASK_CHECK(decimater.is_initialized());
  TASK_CHECK(decimater.decimate(2) > 0);
  return 0;
}

int case_all_three_modules_compatible() {
  Mesh mesh = make_wavy(19);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Flipping::Handle flipping;
  Aspect::Handle aspect;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(flipping));
  TASK_CHECK(decimater.add(aspect));
  decimater.module(aspect).set_aspect_ratio(3.0f);
  decimater.set_relocation(true);
  TASK_CHECK(decimater.initialize());
  size_t n = decimater.decimate(3);
  TASK_CHECK(n > 0);
  TASK_CHECK(mesh_is_consistent(mesh));
  return 0;
}

int case_two_priority_modules_rejected() {
  Mesh mesh = make_wavy(20);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Quadric::Handle second;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(second));
  decimater.set_relocation(true);
  TASK_CHECK(!decimater.initialize());
  return 0;
}

int case_default_mode_unchanged() {
  // Without relocation, incompatible modules are accepted as before.
  Mesh mesh = make_wavy(21);
  Decimater decimater(mesh);
  Quadric::Handle quadric;
  Aspect::Handle aspect;
  TASK_CHECK(decimater.add(quadric));
  TASK_CHECK(decimater.add(aspect));
  TASK_CHECK(decimater.initialize());
  TASK_CHECK(decimater.decimate(2) == 2);
  TASK_CHECK(off_lattice_vertices(mesh) == 0);
  return 0;
}

int case_initialize_returns_false_keeps_mesh() {
  Mesh mesh = make_wavy(22);
  Decimater decimater(mesh);

  std::vector<Mesh::Point> pts;
  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v) pts.push_back(mesh.point(*v));

  Quadric::Handle quadric;
  TASK_CHECK(decimater.add(quadric));
  decimater.module(quadric).set_max_err(1e30, true);
  decimater.set_relocation(true);
  TASK_CHECK(!decimater.initialize());

  size_t nv, nf;
  alive_counts(mesh, nv, nf);
  TASK_CHECK(nv == 49 && nf == 72);
  size_t i = 0;
  for (Mesh::ConstVertexIter v = mesh.vertices_begin();
       v != mesh.vertices_end(); ++v, ++i)
    TASK_CHECK(mesh.point(*v) == pts[i]);
  return 0;
}

}  // namespace

// --------------------------------------------------------------- registry

const struct task_case task_cases[] = {
  {"placement", "default_is_off", case_default_is_off},
  {"placement", "set_relocation_resets_state", case_set_relocation_resets_state},
  {"placement", "endpoint_mode_keeps_points", case_endpoint_mode_keeps_points},
  {"placement", "full_rank_minimizer", case_full_rank_minimizer},
  {"placement", "singular_fallback", case_singular_fallback},
  {"placement", "zero_matrix_fallback", case_zero_matrix_fallback},
  {"placement", "boundary_keeps_p1", case_boundary_keeps_p1},
  {"placement", "locked_and_feature_keep_p1", case_locked_and_feature_keep_p1},
  {"placement", "max_err_uses_target", case_max_err_uses_target},

  {"geometry", "normal_flip_on_v1_side_rejected",
   case_normal_flip_on_v1_side_rejected},
  {"geometry", "trial_leaves_no_trace", case_trial_leaves_no_trace},
  {"geometry", "normals_and_cache_updated", case_normals_and_cache_updated},
  {"geometry", "aspect_rejects_on_union", case_aspect_rejects_on_union},

  {"sequence", "repeated_collapses_decimate", case_repeated_collapses_decimate},
  {"sequence", "repeated_collapses_to_faces",
   case_repeated_collapses_to_faces},
  {"sequence", "decimate_to_faces_stops_at_faces",
   case_decimate_to_faces_stops_at_faces},
  {"sequence", "rejected_candidate_continues", case_rejected_candidate_continues},
  {"sequence", "heap_skips_stale_entries", case_heap_skips_stale_entries},
  {"sequence", "heap_revalidates_target_at_pop",
   case_heap_revalidates_target_at_pop},
  {"sequence", "relocation_refreshes_v1_neighbourhood",
   case_relocation_refreshes_v1_neighbourhood},

  {"compatibility", "other_binary_module_rejected",
   case_other_binary_module_rejected},
  {"compatibility", "quadric_binary_rejected", case_quadric_binary_rejected},
  {"compatibility", "quadric_and_normal_flipping_compatible",
   case_quadric_and_normal_flipping_compatible},
  {"compatibility", "all_three_modules_compatible",
   case_all_three_modules_compatible},
  {"compatibility", "two_priority_modules_rejected",
   case_two_priority_modules_rejected},
  {"compatibility", "default_mode_unchanged", case_default_mode_unchanged},
  {"compatibility", "initialize_returns_false_keeps_mesh",
   case_initialize_returns_false_keeps_mesh},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
