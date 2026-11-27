// Relocation support for the OpenMesh decimater: tests.
//
// The test harness supplies main(); every case below returns 0 on success
// and uses TASK_CHECK for content assertions.

#include "task_support/check.h"
#include "task_support/meshes.hh"

#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModAspectRatioT.hh>
#include <OpenMesh/Tools/Decimater/ModBaseT.hh>
#include <OpenMesh/Tools/Decimater/ModNormalFlippingT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>
#include <OpenMesh/Core/Geometry/QuadricT.hh>

#include <cmath>
#include <cstddef>
#include <vector>

namespace {

typedef task_fixture::Mesh Mesh;
typedef Mesh::Point Point;
typedef Mesh::VertexHandle VertexHandle;
typedef Mesh::FaceHandle FaceHandle;
typedef Mesh::EdgeHandle EdgeHandle;
typedef OpenMesh::Decimater::DecimaterT<Mesh> Decimater;
typedef OpenMesh::Decimater::ModQuadricT<Mesh> ModQuadric;
typedef OpenMesh::Decimater::ModNormalFlippingT<Mesh> ModNormalFlipping;
typedef OpenMesh::Decimater::ModAspectRatioT<Mesh> ModAspectRatio;

// ---------------------------------------------------------------------------
// helpers for independent verification

// A quadric stored as its raw 4x4 symmetric upper triangle, built here from
// scratch so the tests do not depend on the module's own construction.
struct RawQuadric {
  double a, b, c, d, e, f, g, h, i, j;
  RawQuadric() : a(0), b(0), c(0), d(0), e(0), f(0), g(0), h(0), i(0), j(0) {}
  void addPlaneAt(double nx, double ny, double nz, double dd, double w) {
    a += w * nx * nx; b += w * nx * ny; c += w * nx * nz; d += w * nx * dd;
    e += w * ny * ny; f += w * ny * nz; g += w * ny * dd;
    h += w * nz * nz; i += w * nz * dd;
    j += w * dd * dd;
  }
  void plus(const RawQuadric& o) {
    a += o.a; b += o.b; c += o.c; d += o.d; e += o.e;
    f += o.f; g += o.g; h += o.h; i += o.i; j += o.j;
  }
  double eval(const Point& p) const {
    const double x = p[0], y = p[1], z = p[2];
    return a*x*x + 2*b*x*y + 2*c*x*z + 2*d*x
                 + e*y*y + 2*f*y*z + 2*g*y
                          + h*z*z + 2*i*z
                                   + j;
  }
  OpenMesh::Geometry::QuadricT<double> toQuadric() const {
    return OpenMesh::Geometry::QuadricT<double>(a, b, c, d, e, f, g, h, i, j);
  }
};

// Build per-vertex quadrics exactly as a face-plane accumulation would.
std::vector<RawQuadric> buildRawQuadrics(Mesh& m) {
  std::vector<RawQuadric> q(m.n_vertices());
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f) {
    Mesh::FaceVertexIter fv = m.fv_iter(*f);
    Point v0 = m.point(*fv); ++fv;
    Point v1 = m.point(*fv); ++fv;
    Point v2 = m.point(*fv);
    Point n = (v1 - v0) % (v2 - v0);
    double area = n.norm();
    if (area > 1e-37) {
      n /= area;
      area *= 0.5;
    } else {
      area = 0.0;
    }
    double dd = -(v0 | n);
    Mesh::FaceVertexIter fv2 = m.fv_iter(*f);
    for (; fv2.is_valid(); ++fv2) {
      q[(*fv2).idx()].addPlaneAt(n[0], n[1], n[2], dd, area);
    }
  }
  return q;
}

// Independent solve of the merged quadric.  Mirrors the documented
// contract: singular or non-finite -> lowest error of p1, p0, midpoint;
// ties prefer p1, then p0, then the midpoint.
Point independentTarget(const OpenMesh::Geometry::QuadricT<double>& qq,
                        const Point& p0, const Point& p1, bool& full_rank) {
  const double A[3][3] = {
    { qq.a(), qq.b(), qq.c() },
    { qq.b(), qq.e(), qq.f() },
    { qq.c(), qq.f(), qq.h() }
  };
  const double rhs[3] = { -qq.d(), -qq.g(), -qq.i() };

  double scale = 0.0;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      if (std::fabs(A[r][c]) > scale) scale = std::fabs(A[r][c]);
  const double threshold = 1e-12 * scale;

  double m[3][4];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) m[r][c] = A[r][c];
    m[r][3] = rhs[r];
  }

  bool singular = (scale == 0.0);
  for (int c = 0; c < 3 && !singular; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 3; ++r)
      if (std::fabs(m[r][c]) > std::fabs(m[pivot][c])) pivot = r;
    if (std::fabs(m[pivot][c]) <= threshold) { singular = true; break; }
    if (pivot != c)
      for (int k = c; k < 4; ++k) {
        double t = m[c][k]; m[c][k] = m[pivot][k]; m[pivot][k] = t;
      }
    for (int r = c + 1; r < 3; ++r) {
      double fac = m[r][c] / m[c][c];
      for (int k = c; k < 4; ++k) m[r][k] -= fac * m[c][k];
    }
  }

  bool finite = !singular;
  Point sol;
  if (finite) {
    double x[3];
    for (int r = 2; r >= 0; --r) {
      double v = m[r][3];
      for (int k = r + 1; k < 3; ++k) v -= m[r][k] * x[k];
      x[r] = v / m[r][r];
      finite = finite && std::isfinite(x[r]);
    }
    sol = Point(x[0], x[1], x[2]);
  }

  full_rank = finite;
  if (finite) return sol;

  const Point mid = (p0 + p1) * 0.5;
  Point best = p1;
  double best_err = qq(p1);
  double err = qq(p0);
  if (err < best_err) { best_err = err; best = p0; }
  err = qq(mid);
  if (err < best_err) best = mid;
  return best;
}

// Count live vertices / faces, ignoring deleted ones.
int liveVertices(Mesh& m) {
  int n = 0;
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    if (!m.status(*v).deleted()) ++n;
  return n;
}
int liveFaces(Mesh& m) {
  int n = 0;
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f)
    if (!m.status(*f).deleted()) ++n;
  return n;
}

// Every live face normal must agree with its geometry.
bool normalsConsistent(Mesh& m) {
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f) {
    if (m.status(*f).deleted()) continue;
    if ((m.normal(*f) - m.calc_face_normal(*f)).norm() > 1e-9) return false;
  }
  return true;
}

// All live faces must be non-degenerate with a normalized normal.
bool facesValid(Mesh& m) {
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f) {
    if (m.status(*f).deleted()) continue;
    Point n = m.calc_face_normal(*f);
    if (!(n.norm() > 0.0)) return false;
  }
  return true;
}

// Every live face must reference only live vertices, and the retained
// positions must still form valid triangles with non-zero area.
bool connectivityValid(Mesh& m) {
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f) {
    if (m.status(*f).deleted()) continue;
    int live = 0;
    for (Mesh::FaceVertexIter fv = m.fv_iter(*f); fv.is_valid(); ++fv) {
      if (m.status(*fv).deleted()) return false;
      ++live;
    }
    if (live != 3) return false;
    if (!(m.calc_face_normal(*f).norm() > 0.0)) return false;
  }
  return true;
}

// Every live vertex must still have an outgoing halfedge connected to a live
// face, and no live vertex may be isolated.
bool vertexConnectivityValid(Mesh& m) {
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v) {
    if (m.status(*v).deleted()) continue;
    Mesh::HalfedgeHandle h = m.halfedge_handle(*v);
    if (!h.is_valid()) return false;
    bool incident = false;
    for (Mesh::VertexFaceIter vf = m.vf_iter(*v); vf.is_valid(); ++vf)
      if (!m.status(*vf).deleted()) incident = true;
    if (!incident) return false;
  }
  return true;
}

// Small deterministic grids used to exercise exact geometry.
Mesh makeGrid(int n, double step, double amp) {
  Mesh m;
  std::vector<VertexHandle> v(n * n);
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      double z = (x == 0 || y == 0 || x == n - 1 || y == n - 1)
                     ? 0.0
                     : amp * ((x + y) % 2 ? 1.0 : -1.0);
      v[y * n + x] = m.add_vertex(Point(x * step, y * step, z));
    }
  for (int y = 0; y + 1 < n; ++y)
    for (int x = 0; x + 1 < n; ++x) {
      VertexHandle a = v[y * n + x], b = v[y * n + x + 1];
      VertexHandle c = v[(y + 1) * n + x + 1], d = v[(y + 1) * n + x];
      m.add_face(a, b, c);
      m.add_face(a, c, d);
    }
  m.request_vertex_status();
  m.request_edge_status();
  m.request_face_status();
  m.request_face_normals();
  m.update_face_normals();
  return m;
}

// ---------------------------------------------------------------------------
// generic module used to exercise compatibility rules

// Accepts absolutely nothing; useful as an unsupported binary module.
class ModRejectAll : public OpenMesh::Decimater::ModBaseT<Mesh> {
public:
  typedef OpenMesh::Decimater::ModBaseT<Mesh> Base;
  typedef OpenMesh::Decimater::CollapseInfoT<Mesh> CollapseInfo;
  typedef ModRejectAll Self;
  typedef OpenMesh::Decimater::ModHandleT<Self> Handle;
  ModRejectAll(Mesh& _mesh) : Base(_mesh, true) {}
  float collapse_priority(const CollapseInfo&) { return float(Base::ILLEGAL_COLLAPSE); }
  bool relocation_compatible() const { return true; }
  virtual const std::string& name() const { static std::string n("RejectAll"); return n; }
};

// A binary module that does not understand relocated candidates.
class ModOpaqueBinary : public OpenMesh::Decimater::ModBaseT<Mesh> {
public:
  typedef OpenMesh::Decimater::ModBaseT<Mesh> Base;
  typedef OpenMesh::Decimater::CollapseInfoT<Mesh> CollapseInfo;
  typedef ModOpaqueBinary Self;
  typedef OpenMesh::Decimater::ModHandleT<Self> Handle;
  ModOpaqueBinary(Mesh& _mesh) : Base(_mesh, true) {}
  float collapse_priority(const CollapseInfo&) { return float(Base::LEGAL_COLLAPSE); }
  virtual const std::string& name() const { static std::string n("OpaqueBinary"); return n; }
};

// A non-binary priority module that does not know how to relocate.
class ModPlainPriority : public OpenMesh::Decimater::ModBaseT<Mesh> {
public:
  typedef OpenMesh::Decimater::ModBaseT<Mesh> Base;
  typedef OpenMesh::Decimater::CollapseInfoT<Mesh> CollapseInfo;
  typedef ModPlainPriority Self;
  typedef OpenMesh::Decimater::ModHandleT<Self> Handle;
  ModPlainPriority(Mesh& _mesh) : Base(_mesh, false) {}
  float collapse_priority(const CollapseInfo&) { return 0.0f; }
  virtual const std::string& name() const { static std::string n("PlainPriority"); return n; }
};

} // namespace

// ---------------------------------------------------------------------------
// group "placement": where a candidate's target position comes from

static int case_default_target_is_p1() {
  Mesh m = makeGrid(4, 1.0, 0.5);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.initialize());

  // With relocation off the priority module must leave every candidate at
  // the retained endpoint.
  OpenMesh::Decimater::CollapseInfoT<Mesh> ci(m, m.halfedge_handle(Mesh::EdgeHandle(5), 0));
  TASK_CHECK(ci.p_target == ci.p1);
  TASK_CHECK(!ci.relocating);
  TASK_CHECK(!d.relocation());
  return 0;
}

static int case_full_rank_matches_independent_solve() {
  Mesh m = makeGrid(5, 1.0, 0.75);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.initialize());
  ModQuadric& qm = d.module(qh);

  std::vector<RawQuadric> raw = buildRawQuadrics(m);
  int checked = 0;
  for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e) {
    for (int dir = 0; dir < 2; ++dir) {
      OpenMesh::Decimater::CollapseInfoT<Mesh> ci(m, m.halfedge_handle(*e, dir));
      if (m.is_boundary(ci.v0) || m.is_boundary(ci.v1)) continue;
      if (m.is_boundary(ci.v0v1) || m.is_boundary(ci.v1v0)) continue;

      RawQuadric merged = raw[ci.v0.idx()];
      merged.plus(raw[ci.v1.idx()]);
      bool full_rank = false;
      Point expected = independentTarget(merged.toQuadric(), ci.p0, ci.p1, full_rank);
      Point actual = qm.relocate(ci);
      TASK_CHECK(full_rank);
      TASK_CHECK((actual - expected).norm() < 1e-9);
      ++checked;
    }
  }
  TASK_CHECK(checked > 0);
  return 0;
}

static int case_singular_zero_matrix_falls_back() {
  // A plain zero quadric is singular and must not produce NaN; the fallback
  // chooses the lowest error of p1, p0 and the midpoint.
  OpenMesh::Geometry::QuadricT<double> zero;
  zero.clear();
  Point p0(0, 0, 0), p1(2, 0, 0);
  Point got = ModQuadric::optimal_position(zero, p0, p1);
  // All three candidates have error 0, so the tie order picks p1.
  TASK_CHECK(got[0] == p1[0] && got[1] == p1[1] && got[2] == p1[2]);
  return 0;
}

static int case_singular_degenerate_quadric_uses_fallback() {
  // A rank-deficient quadric (single plane) has a whole line of minimizers.
  // The solver must detect the near-singular system and fall back rather
  // than emit an unbounded point.
  RawQuadric r;
  r.addPlaneAt(0.0, 0.0, 1.0, 0.0, 1.0); // z-plane only, x/y unconstrained
  OpenMesh::Geometry::QuadricT<double> q = r.toQuadric();
  Point p0(0, 0, 0), p1(1, 1, 3);
  bool full = true;
  Point expected = independentTarget(q, p0, p1, full);
  TASK_CHECK(!full);
  Point got = ModQuadric::optimal_position(q, p0, p1);
  TASK_CHECK((got - expected).norm() < 1e-12);
  // p1 has the largest error, so the fallback must move off it.
  TASK_CHECK((got - p1).norm() > 0.1);
  return 0;
}

static int case_nonfinite_rhs_is_rejected() {
  // A matrix that is finite but produces a non-finite solution must also
  // use the fallback.
  OpenMesh::Geometry::QuadricT<double> q(1e308, 0, 0, 0, 1e308, 0, 0, 1e308, 0, 0);
  Point p0(0, 0, 0), p1(1, 0, 0);
  Point got = ModQuadric::optimal_position(q, p0, p1);
  for (int k = 0; k < 3; ++k) TASK_CHECK(std::isfinite(got[k]));
  return 0;
}

static int case_boundary_and_feature_stay_at_p1() {
  Mesh m = makeGrid(4, 1.0, 0.5);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  d.set_relocation(true);
  TASK_CHECK(d.initialize());
  ModQuadric& qm = d.module(qh);

  int interior = 0, boundary = 0, feature = 0, locked = 0;
  for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e) {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci(m, m.halfedge_handle(*e, 0));
    bool touchesBoundary = m.is_boundary(ci.v0) || m.is_boundary(ci.v1) ||
                           m.is_boundary(ci.v0v1) || m.is_boundary(ci.v1v0);
    if (touchesBoundary) {
      TASK_CHECK((qm.relocate(ci) - ci.p1).norm() == 0.0);
      ++boundary;
    } else {
      ++interior;
    }
  }
  TASK_CHECK(boundary > 0 && interior > 0);

  Mesh fm = makeGrid(4, 1.0, 0.5);
  Decimater fd(fm);
  ModQuadric::Handle fh;
  TASK_CHECK(fd.add(fh));
  fd.set_relocation(true);
  TASK_CHECK(fd.initialize());
  ModQuadric& fqm = fd.module(fh);
  for (Mesh::EdgeIter e = fm.edges_begin(); e != fm.edges_end(); ++e) {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci(fm, fm.halfedge_handle(*e, 0));
    if (fm.is_boundary(ci.v0) || fm.is_boundary(ci.v1)) continue;
    fm.status(ci.v0).set_feature(true);
    fm.status(ci.v1).set_feature(true);
  }
  // Set a feature vertex then confirm its incident candidates do not move.
  VertexHandle center = fm.vertex_handle(0);
  for (Mesh::VertexIter v = fm.vertices_begin(); v != fm.vertices_end(); ++v) {
    if (!fm.is_boundary(*v)) { center = *v; break; }
  }
  fm.status(center).set_feature(true);
  for (Mesh::VertexOHalfedgeIter oh = fm.voh_iter(center); oh.is_valid(); ++oh) {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci(fm, *oh);
    if (!fm.is_collapse_ok(ci.v0v1)) continue;
    if (fm.is_boundary(ci.v0) || fm.is_boundary(ci.v1)) continue;
    TASK_CHECK((fqm.relocate(ci) - ci.p1).norm() == 0.0);
    ++feature;
  }
  TASK_CHECK(feature > 0);

  // Locked candidates are also pinned to the endpoint.
  Mesh lm = makeGrid(4, 1.0, 0.5);
  Decimater ld(lm);
  ModQuadric::Handle lh;
  TASK_CHECK(ld.add(lh));
  ld.set_relocation(true);
  TASK_CHECK(ld.initialize());
  ModQuadric& lqm = ld.module(lh);
  VertexHandle lv = lm.vertex_handle(0);
  for (Mesh::VertexIter v = lm.vertices_begin(); v != lm.vertices_end(); ++v)
    if (!lm.is_boundary(*v)) { lv = *v; break; }
  lm.status(lv).set_locked(true);
  for (Mesh::VertexOHalfedgeIter oh = lm.voh_iter(lv); oh.is_valid(); ++oh) {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci(lm, *oh);
    if (!lm.is_collapse_ok(ci.v0v1)) continue;
    if (lm.is_boundary(ci.v0) || lm.is_boundary(ci.v1)) continue;
    TASK_CHECK((lqm.relocate(ci) - ci.p1).norm() == 0.0);
    ++locked;
  }
  TASK_CHECK(locked > 0);
  return 0;
}

static int case_retained_vertex_actually_moves() {
  // Run a real collapse on a mesh whose interior minimizers are clearly
  // distinct from every original vertex.  After the collapse at least one
  // live vertex must sit at a brand-new position, proving relocation
  // actually completes collapses rather than just changing a score.
  Mesh m = task_fixture::make_mesh(1);
  std::vector<Point> snapshot(m.n_vertices());
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    snapshot[(*v).idx()] = m.point(*v);

  // Lock the boundary so the heap picks interior collapses, whose
  // minimizers are genuinely different from both endpoints.
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    if (m.is_boundary(*v)) m.status(*v).set_locked(true);

  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  d.set_relocation(true);
  TASK_CHECK(d.initialize());

  size_t removed = d.decimate(2);
  TASK_CHECK(removed == 2);

  int moved = 0;
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v) {
    if (m.status(*v).deleted()) continue;
    bool original = false;
    for (size_t k = 0; k < snapshot.size(); ++k)
      if ((m.point(*v) - snapshot[k]).norm() < 1e-9) original = true;
    if (!original) ++moved;
  }
  TASK_CHECK(moved >= 1);
  TASK_CHECK(normalsConsistent(m));
  TASK_CHECK(facesValid(m));
  return 0;
}

static int case_placement_is_deterministic() {
  // The target must be a pure function of the current geometry and quadrics.
  Mesh m = task_fixture::make_mesh(7);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  d.set_relocation(true);
  TASK_CHECK(d.initialize());
  ModQuadric& qm = d.module(qh);
  OpenMesh::Decimater::CollapseInfoT<Mesh> ci(m, m.halfedge_handle(Mesh::EdgeHandle(20), 0));
  Point a = qm.relocate(ci);
  Point b = qm.relocate(ci);
  TASK_CHECK((a - b).norm() == 0.0);
  return 0;
}

// ---------------------------------------------------------------------------
// group "geometry": normals, aspect ratio and validity around the target

static int case_normal_check_covers_retained_side() {
  // There are candidates where the two-endpoint (classic) normal test passes
  // but the retained vertex's own faces would flip when it moves to the
  // minimizer.  Checking only the removed vertex misses these.
  Mesh m = task_fixture::make_mesh(2);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  d.set_relocation(true);
  TASK_CHECK(d.initialize());
  ModQuadric& qm = d.module(qh);
  ModNormalFlipping nf(m);

  bool found = false;
  for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end() && !found; ++e) {
    for (int dir = 0; dir < 2 && !found; ++dir) {
      OpenMesh::Decimater::CollapseInfoT<Mesh> ci(m, m.halfedge_handle(*e, dir));
      if (!m.is_collapse_ok(ci.v0v1)) continue;
    if (m.is_boundary(ci.v0) || m.is_boundary(ci.v1)) continue;
      if (m.is_boundary(ci.v0) || m.is_boundary(ci.v1)) continue;
      Point t = qm.relocate(ci);
      if ((t - ci.p1).norm() < 0.1) continue;

      // classic test on the removed vertex only
      OpenMesh::Decimater::CollapseInfoT<Mesh> classic(m, m.halfedge_handle(*e, dir));
      classic.p_target = classic.p1;
      classic.relocating = false;
      if (nf.collapse_priority(classic) < 0.0) continue;

      // full test including the retained vertex
      OpenMesh::Decimater::CollapseInfoT<Mesh> reloc(m, m.halfedge_handle(*e, dir));
      reloc.p_target = t;
      reloc.relocating = true;
      bool reloc_legal = nf.collapse_priority(reloc) >= 0.0;

      // independently confirm the retained side actually flips at t
      Point save0 = m.point(ci.v0), save1 = m.point(ci.v1);
      m.set_point(ci.v0, t);
      m.set_point(ci.v1, t);
      bool retained_flips = false;
      for (Mesh::ConstVertexFaceIter vf = m.cvf_iter(ci.v1); vf.is_valid(); ++vf) {
        FaceHandle fh = *vf;
        if (fh == ci.fl || fh == ci.fr) continue;
        if (dot(m.normal(fh), m.calc_face_normal(fh)) < 0.0) {
          retained_flips = true;
          break;
        }
      }
      m.set_point(ci.v0, save0);
      m.set_point(ci.v1, save1);

      if (retained_flips && !reloc_legal) found = true;
    }
  }
  TASK_CHECK(found);
  return 0;
}

static int case_normal_check_classic_mode_unchanged() {
  // With relocation off the classic one-ring test must still be the one
  // applied to the removed vertex.
  Mesh m = makeGrid(4, 1.0, 0.5);
  Decimater d(m);
  ModQuadric::Handle qh;
  ModNormalFlipping::Handle nh;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.add(nh));
  d.set_relocation(false);
  TASK_CHECK(d.initialize());
  ModNormalFlipping& nf = d.module(nh);
  (void)qh;

  int checked = 0;
  for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e) {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci(m, m.halfedge_handle(*e, 0));
    if (!m.is_collapse_ok(ci.v0v1)) continue;
    if (m.is_boundary(ci.v0) || m.is_boundary(ci.v1)) continue;
    float got = nf.collapse_priority(ci);
    TASK_CHECK(got >= 0.0f || got == float(OpenMesh::Decimater::ModBaseT<Mesh>::ILLEGAL_COLLAPSE));
    ++checked;
  }
  TASK_CHECK(checked > 0);
  return 0;
}

static int case_aspect_ratio_uses_both_endpoint_faces() {
  // For some relocated candidates the classic two-endpoint aspect test
  // accepts while the union of both endpoint face rings rejects.
  Mesh m;
  bool found = false;
  for (unsigned seed = 1; seed <= 40 && !found; ++seed) {
    m = task_fixture::make_mesh(seed);
    Decimater d(m);
    ModQuadric::Handle qh;
    TASK_CHECK(d.add(qh));
    d.set_relocation(true);
    TASK_CHECK(d.initialize());
    ModQuadric& qm = d.module(qh);
    ModAspectRatio ar(m);
    ar.set_aspect_ratio(8.0f);
    ar.initialize();

    for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end() && !found; ++e) {
      for (int dir = 0; dir < 2 && !found; ++dir) {
        OpenMesh::Decimater::CollapseInfoT<Mesh> ci(m, m.halfedge_handle(*e, dir));
        if (!m.is_collapse_ok(ci.v0v1)) continue;
    if (m.is_boundary(ci.v0) || m.is_boundary(ci.v1)) continue;
        if (m.is_boundary(ci.v0) || m.is_boundary(ci.v1)) continue;
        Point t = qm.relocate(ci);
        if ((t - ci.p1).norm() < 0.1) continue;

        OpenMesh::Decimater::CollapseInfoT<Mesh> classic(m, m.halfedge_handle(*e, dir));
        classic.p_target = classic.p1;
        classic.relocating = false;
        OpenMesh::Decimater::CollapseInfoT<Mesh> reloc(m, m.halfedge_handle(*e, dir));
        reloc.p_target = t;
        reloc.relocating = true;

        if (ar.collapse_priority(classic) >= 0.0f &&
            ar.collapse_priority(reloc) < 0.0f)
          found = true;
      }
    }
  }
  TASK_CHECK(found);
  return 0;
}

static int case_strict_aspect_ratio_rejects_and_continues() {
  Mesh m = task_fixture::make_mesh(1);
  Decimater d(m);
  ModQuadric::Handle qh;
  ModAspectRatio::Handle ah;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.add(ah));
  d.module(ah).set_aspect_ratio(50.0f);
  d.set_relocation(true);
  TASK_CHECK(d.initialize());
  size_t removed = d.decimate(5);
  TASK_CHECK(removed > 0 && removed <= 5);
  TASK_CHECK(facesValid(m));
  TASK_CHECK(normalsConsistent(m));
  return 0;
}

static int case_trial_does_not_leave_state() {
  // Evaluating a candidate, including rejected binary modules, must not
  // move coordinates, normals or cached aspect values.
  Mesh m = task_fixture::make_mesh(4);
  Decimater d(m);
  ModQuadric::Handle qh;
  ModAspectRatio::Handle ah;
  ModNormalFlipping::Handle nh;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.add(ah));
  TASK_CHECK(d.add(nh));
  d.set_relocation(true);
  TASK_CHECK(d.initialize());
  ModQuadric& qm = d.module(qh);
  ModNormalFlipping& nf = d.module(nh);
  ModAspectRatio& ar = d.module(ah);

  std::vector<Point> before(m.n_vertices());
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    before[(*v).idx()] = m.point(*v);
  std::vector<Point> normals(m.n_faces());
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f)
    normals[(*f).idx()] = m.normal(*f);

  int exercised = 0;
  for (Mesh::EdgeIter e = m.edges_begin(); e != m.edges_end(); ++e) {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci(m, m.halfedge_handle(*e, 0));
    if (!m.is_collapse_ok(ci.v0v1)) continue;
    if (m.is_boundary(ci.v0) || m.is_boundary(ci.v1)) continue;
    Point t = qm.relocate(ci);
    OpenMesh::Decimater::CollapseInfoT<Mesh> reloc(m, m.halfedge_handle(*e, 0));
    reloc.p_target = t;
    reloc.relocating = true;
    nf.collapse_priority(reloc);
    ar.collapse_priority(reloc);
    ++exercised;
  }
  TASK_CHECK(exercised > 0);

  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    TASK_CHECK((m.point(*v) - before[(*v).idx()]).norm() == 0.0);
  for (Mesh::FaceIter f = m.faces_begin(); f != m.faces_end(); ++f)
    TASK_CHECK((m.normal(*f) - normals[(*f).idx()]).norm() == 0.0);

  // Probing must not poison any quality cache either.  Two identical meshes
  // decimated with the same settings must agree; one of them first probes
  // every candidate through all modules.
  Mesh ref = task_fixture::make_mesh(4);
  Decimater rd(ref);
  ModQuadric::Handle rq;
  ModAspectRatio::Handle ra;
  ModNormalFlipping::Handle rn;
  TASK_CHECK(rd.add(rq));
  TASK_CHECK(rd.add(ra));
  TASK_CHECK(rd.add(rn));
  rd.set_relocation(true);
  TASK_CHECK(rd.initialize());
  size_t ref_removed = rd.decimate(4);
  TASK_CHECK(ref_removed == 4);

  size_t probe_removed = d.decimate(4);
  TASK_CHECK(probe_removed == ref_removed);
  int compared = 0;
  for (Mesh::VertexIter v = m.vertices_begin(), rv = ref.vertices_begin();
       v != m.vertices_end() && rv != ref.vertices_end(); ++v, ++rv) {
    if (m.status(*v).deleted() || ref.status(*rv).deleted()) continue;
    TASK_CHECK((m.point(*v) - ref.point(*rv)).norm() < 1e-12);
    ++compared;
  }
  TASK_CHECK(compared > 0);
  return 0;
}

static int case_failed_candidate_does_not_alter_mesh() {
  // A binary module that rejects every candidate must leave the mesh and
  // its coordinates untouched.
  Mesh m = task_fixture::make_mesh(3);
  Decimater d(m);
  ModQuadric::Handle qh;
  ModRejectAll::Handle rh;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.add(rh));
  d.set_relocation(true);
  TASK_CHECK(d.initialize());
  std::vector<Point> before(m.n_vertices());
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    before[(*v).idx()] = m.point(*v);
  TASK_CHECK(d.decimate(1) == 0);
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    TASK_CHECK((m.point(*v) - before[(*v).idx()]).norm() == 0.0);
  TASK_CHECK(liveVertices(m) == (int)m.n_vertices());
  return 0;
}

// ---------------------------------------------------------------------------
// group "sequence": several collapses through both entry points

static int case_multiple_collapses_through_decimate() {
  Mesh m = task_fixture::make_mesh(1);
  Decimater d(m);
  ModQuadric::Handle qh;
  ModNormalFlipping::Handle nh;
  ModAspectRatio::Handle ah;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.add(nh));
  TASK_CHECK(d.add(ah));
  d.set_relocation(true);
  TASK_CHECK(d.initialize());

  int v0 = liveVertices(m);
  size_t removed = d.decimate(6);
  TASK_CHECK(removed > 0 && removed <= 6);
  TASK_CHECK(liveVertices(m) == v0 - (int)removed);
  TASK_CHECK(liveFaces(m) >= 1);
  TASK_CHECK(normalsConsistent(m));
  TASK_CHECK(facesValid(m));
  TASK_CHECK(connectivityValid(m));
  TASK_CHECK(vertexConnectivityValid(m));

  // Retained vertices must not coincide with each other.
  std::vector<Point> pts;
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v) {
    if (m.status(*v).deleted()) continue;
    for (size_t k = 0; k < pts.size(); ++k)
      TASK_CHECK((m.point(*v) - pts[k]).norm() > 1e-6);
    pts.push_back(m.point(*v));
  }
  return 0;
}

static int case_multiple_collapses_through_decimate_to_faces() {
  Mesh m = task_fixture::make_mesh(2);
  Decimater d(m);
  ModQuadric::Handle qh;
  ModNormalFlipping::Handle nh;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.add(nh));
  d.set_relocation(true);
  TASK_CHECK(d.initialize());

  int v0 = liveVertices(m), f0 = liveFaces(m);
  // The loop stops as soon as either target is reached, so request a
  // modest reduction and assert it made real progress.
  size_t removed = d.decimate_to_faces((size_t)(v0 - 4), (size_t)(f0 - 8));
  TASK_CHECK(removed > 0);
  TASK_CHECK(liveVertices(m) <= v0);
  TASK_CHECK(liveFaces(m) <= f0);
  TASK_CHECK(liveVertices(m) == v0 - (int)removed);
  TASK_CHECK(normalsConsistent(m));
  TASK_CHECK(facesValid(m));
  TASK_CHECK(connectivityValid(m));
  TASK_CHECK(vertexConnectivityValid(m));
  return 0;
}

static int case_later_candidates_see_updated_geometry() {
  // After a moved collapse the retained vertex must carry the merged
  // quadric and its actual new coordinate, so later adjacent candidates see
  // the up-to-date geometry rather than a stale endpoint score.
  Mesh m = task_fixture::make_mesh(1);
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    if (m.is_boundary(*v)) m.status(*v).set_locked(true);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  d.set_relocation(true);
  TASK_CHECK(d.initialize());
  ModQuadric& qm = d.module(qh);

  std::vector<OpenMesh::Geometry::QuadricT<double> > original_quadrics(
      m.n_vertices());
  std::vector<Point> original_points(m.n_vertices());
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v) {
    original_quadrics[(*v).idx()] = qm.quadric(*v);
    original_points[(*v).idx()] = m.point(*v);
  }

  size_t removed_first = d.decimate(1);
  TASK_CHECK(removed_first == 1);

  // Locate the single removed and the retained vertex.
  int deleted = -1, retained = -1;
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v) {
    if (m.status(*v).deleted()) deleted = (*v).idx();
    else if ((m.point(*v) - original_points[(*v).idx()]).norm() > 1e-9)
      retained = (*v).idx();
  }
  TASK_CHECK(deleted >= 0);
  TASK_CHECK(retained >= 0);

  // The retained vertex's quadric must be exactly the sum of the two
  // pre-collapse quadrics (the merge is by collapse history).
  OpenMesh::Geometry::QuadricT<double> expected = original_quadrics[retained];
  expected += original_quadrics[deleted];
  const OpenMesh::Geometry::QuadricT<double>& actual =
      qm.quadric(m.vertex_handle(retained));
  TASK_CHECK(actual.a() == expected.a() && actual.b() == expected.b() &&
             actual.c() == expected.c() && actual.d() == expected.d() &&
             actual.e() == expected.e() && actual.f() == expected.f() &&
             actual.g() == expected.g() && actual.h() == expected.h() &&
             actual.i() == expected.i() && actual.j() == expected.j());

  // The retained coordinate is the actual new target, not a stale point.
  Point p = m.point(m.vertex_handle(retained));
  TASK_CHECK((p - original_points[retained]).norm() > 1e-9);
  for (int k = 0; k < 3; ++k) TASK_CHECK(std::isfinite(p[k]));

  // Later candidates around the retained vertex must be evaluated against
  // the updated geometry and merge; they must produce finite targets.
  int evaluated = 0;
  for (Mesh::VertexOHalfedgeIter oh = m.voh_iter(m.vertex_handle(retained));
       oh.is_valid(); ++oh) {
    OpenMesh::Decimater::CollapseInfoT<Mesh> ci(m, *oh);
    if (!m.is_collapse_ok(ci.v0v1)) continue;
    Point t = qm.relocate(ci);
    for (int k = 0; k < 3; ++k) TASK_CHECK(std::isfinite(t[k]));
    ++evaluated;
  }
  TASK_CHECK(evaluated > 0);

  size_t more = d.decimate(4);
  TASK_CHECK(more + removed_first <= 5);
  TASK_CHECK(normalsConsistent(m));
  TASK_CHECK(facesValid(m));
  TASK_CHECK(connectivityValid(m));
  return 0;
}

static int case_sequence_is_stable_across_entry_points() {
  // The same initial mesh decimated with the same settings through either
  // entry point must produce valid, consistent geometry (not necessarily
  // identical vertex counts because the stopping criteria differ).
  Mesh m1 = task_fixture::make_mesh(5);
  Decimater d1(m1);
  ModQuadric::Handle q1;
  TASK_CHECK(d1.add(q1));
  d1.set_relocation(true);
  TASK_CHECK(d1.initialize());
  size_t r1 = d1.decimate(3);
  TASK_CHECK(r1 == 3);
  TASK_CHECK(normalsConsistent(m1));

  Mesh m2 = task_fixture::make_mesh(5);
  Decimater d2(m2);
  ModQuadric::Handle q2;
  TASK_CHECK(d2.add(q2));
  d2.set_relocation(true);
  TASK_CHECK(d2.initialize());
  size_t r2 = d2.decimate_to_faces(0, liveFaces(m2) > 6 ? liveFaces(m2) - 6 : 0);
  TASK_CHECK(r2 > 0);
  TASK_CHECK(normalsConsistent(m2));
  return 0;
}

static int case_rejected_candidates_do_not_stall() {
  // A strict constraint may reject some candidates; decimation must keep
  // going and still make progress where legal candidates exist.
  Mesh m = task_fixture::make_mesh(1);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  d.module(qh).set_max_err(1e-6, false);
  d.set_relocation(true);
  TASK_CHECK(d.initialize());
  size_t removed = d.decimate(2);
  TASK_CHECK(liveVertices(m) == 49 - (int)removed);
  return 0;
}

// ---------------------------------------------------------------------------
// group "compatibility": configuration acceptance/rejection

static int case_relocation_requires_nonbinary_priority() {
  // A binary Quadric cannot serve as the relocating priority module.
  Mesh m = makeGrid(4, 1.0, 0.5);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  d.module(qh).set_max_err(1e9, true);
  d.set_relocation(true);
  TASK_CHECK(!d.initialize());
  TASK_CHECK(!d.is_initialized());
  TASK_CHECK(d.decimate(1) == 0);
  TASK_CHECK(liveVertices(m) == 16);
  return 0;
}

static int case_unsupported_binary_module_rejected() {
  // An arbitrary binary module does not know how to handle a relocated
  // candidate and must make initialize() fail.
  Mesh m = makeGrid(4, 1.0, 0.5);
  Decimater d(m);
  ModQuadric::Handle qh;
  ModOpaqueBinary::Handle rh;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.add(rh));
  d.set_relocation(true);
  TASK_CHECK(!d.initialize());
  TASK_CHECK(!d.is_initialized());
  TASK_CHECK(d.decimate(1) == 0);
  TASK_CHECK(liveVertices(m) == 16);
  return 0;
}

static int case_unsupported_priority_module_rejected() {
  // Only the quadric module advertises relocation among priority modules;
  // a custom non-binary priority module must not silently enable it.
  Mesh m = makeGrid(4, 1.0, 0.5);
  Decimater d(m);
  ModPlainPriority::Handle ph;
  TASK_CHECK(d.add(ph));
  d.set_relocation(true);
  TASK_CHECK(!d.initialize());
  TASK_CHECK(!d.is_initialized());
  return 0;
}

static int case_supported_combinations_initialize() {
  // Quadric + NormalFlipping and Quadric + AspectRatio are supported,
  // both alone and together.
  {
    Mesh m = makeGrid(4, 1.0, 0.5);
    Decimater d(m);
    ModQuadric::Handle qh;
    ModNormalFlipping::Handle nh;
    TASK_CHECK(d.add(qh));
    TASK_CHECK(d.add(nh));
    d.set_relocation(true);
    TASK_CHECK(d.initialize());
    TASK_CHECK(d.is_initialized());
  }
  {
    Mesh m = makeGrid(4, 1.0, 0.5);
    Decimater d(m);
    ModQuadric::Handle qh;
    ModAspectRatio::Handle ah;
    TASK_CHECK(d.add(qh));
    TASK_CHECK(d.add(ah));
    d.set_relocation(true);
    TASK_CHECK(d.initialize());
    TASK_CHECK(d.is_initialized());
  }
  {
    Mesh m = makeGrid(4, 1.0, 0.5);
    Decimater d(m);
    ModQuadric::Handle qh;
    ModNormalFlipping::Handle nh;
    ModAspectRatio::Handle ah;
    TASK_CHECK(d.add(qh));
    TASK_CHECK(d.add(nh));
    TASK_CHECK(d.add(ah));
    d.set_relocation(true);
    TASK_CHECK(d.initialize());
    TASK_CHECK(d.is_initialized());
  }
  return 0;
}

static int case_setting_change_requires_reinitialize() {
  Mesh m = makeGrid(4, 1.0, 0.5);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.is_initialized());
  d.set_relocation(true);
  TASK_CHECK(!d.is_initialized());
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.is_initialized());
  TASK_CHECK(d.decimate(1) == 1);
  // Changing the flag invalidates the initialization; no further collapse is
  // performed until the caller explicitly initializes again.
  d.set_relocation(false);
  TASK_CHECK(!d.is_initialized());
  TASK_CHECK(d.decimate(1) == 0);
  return 0;
}

static int case_default_mode_matches_baseline() {
  // With relocation left at its default (off) a plain Quadric decimation
  // must behave exactly like the original: retained vertices keep their
  // endpoint coordinates.
  Mesh m = makeGrid(5, 1.0, 0.05);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  TASK_CHECK(d.initialize());

  std::vector<Point> original;
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v)
    original.push_back(m.point(*v));

  size_t removed = d.decimate(3);
  TASK_CHECK(removed == 3);
  for (Mesh::VertexIter v = m.vertices_begin(); v != m.vertices_end(); ++v) {
    if (m.status(*v).deleted()) continue;
    Point p = m.point(*v);
    bool found = false;
    for (size_t k = 0; k < original.size(); ++k)
      if ((p - original[k]).norm() < 1e-9) { found = true; break; }
    TASK_CHECK(found);
  }
  return 0;
}

static int case_quadric_binary_without_relocation_still_works() {
  // The classic binary Quadric configuration must keep working when
  // relocation is disabled.
  Mesh m = makeGrid(5, 1.0, 0.05);
  Decimater d(m);
  ModQuadric::Handle qh;
  TASK_CHECK(d.add(qh));
  d.module(qh).set_max_err(1e3, true);
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.decimate(2) == 2);
  return 0;
}

// ---------------------------------------------------------------------------
// registration

const task_case task_cases[] = {
  {"placement", "default_target_is_p1", case_default_target_is_p1},
  {"placement", "full_rank_matches_independent_solve",
   case_full_rank_matches_independent_solve},
  {"placement", "singular_zero_matrix_falls_back",
   case_singular_zero_matrix_falls_back},
  {"placement", "singular_degenerate_quadric_uses_fallback",
   case_singular_degenerate_quadric_uses_fallback},
  {"placement", "nonfinite_rhs_is_rejected", case_nonfinite_rhs_is_rejected},
  {"placement", "boundary_and_feature_stay_at_p1",
   case_boundary_and_feature_stay_at_p1},
  {"placement", "retained_vertex_actually_moves",
   case_retained_vertex_actually_moves},
  {"placement", "placement_is_deterministic", case_placement_is_deterministic},

  {"geometry", "normal_check_covers_retained_side",
   case_normal_check_covers_retained_side},
  {"geometry", "normal_check_classic_mode_unchanged",
   case_normal_check_classic_mode_unchanged},
  {"geometry", "aspect_ratio_uses_both_endpoint_faces",
   case_aspect_ratio_uses_both_endpoint_faces},
  {"geometry", "strict_aspect_ratio_rejects_and_continues",
   case_strict_aspect_ratio_rejects_and_continues},
  {"geometry", "trial_does_not_leave_state", case_trial_does_not_leave_state},
  {"geometry", "failed_candidate_does_not_alter_mesh",
   case_failed_candidate_does_not_alter_mesh},

  {"sequence", "multiple_collapses_through_decimate",
   case_multiple_collapses_through_decimate},
  {"sequence", "multiple_collapses_through_decimate_to_faces",
   case_multiple_collapses_through_decimate_to_faces},
  {"sequence", "later_candidates_see_updated_geometry",
   case_later_candidates_see_updated_geometry},
  {"sequence", "sequence_is_stable_across_entry_points",
   case_sequence_is_stable_across_entry_points},
  {"sequence", "rejected_candidates_do_not_stall",
   case_rejected_candidates_do_not_stall},

  {"compatibility", "relocation_requires_nonbinary_priority",
   case_relocation_requires_nonbinary_priority},
  {"compatibility", "unsupported_binary_module_rejected",
   case_unsupported_binary_module_rejected},
  {"compatibility", "unsupported_priority_module_rejected",
   case_unsupported_priority_module_rejected},
  {"compatibility", "supported_combinations_initialize",
   case_supported_combinations_initialize},
  {"compatibility", "setting_change_requires_reinitialize",
   case_setting_change_requires_reinitialize},
  {"compatibility", "default_mode_matches_baseline",
   case_default_mode_matches_baseline},
  {"compatibility", "quadric_binary_without_relocation_still_works",
   case_quadric_binary_without_relocation_still_works},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
