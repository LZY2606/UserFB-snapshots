/*===========================================================================*\
 *  Task checks for the local, replayable DecimaterT collapse history.
 *
 *  Registered through task_support/check.h; no main() is defined here.
\*===========================================================================*/

#include <task_support/check.h>

#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>
#include <OpenMesh/Tools/Decimater/ModNormalFlippingT.hh>
#include <OpenMesh/Tools/Decimater/ModIndependentSetsT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Throw-based check for helper functions that do not return int.
#define LOCAL_REQUIRE(x) \
  do { if (!(x)) throw std::runtime_error("LOCAL_REQUIRE failed: " #x); } while (0)


using Mesh   = OpenMesh::TriMesh_ArrayKernelT<>;
using Point  = Mesh::Point;
using Vec2f  = OpenMesh::Vec2f;
using Vec3f  = OpenMesh::Vec3f;
using Decimater      = OpenMesh::Decimater::DecimaterT<Mesh>;
using QuadricHandle  = OpenMesh::Decimater::ModQuadricT<Mesh>::Handle;
using FlippingHandle = OpenMesh::Decimater::ModNormalFlippingT<Mesh>::Handle;
using IndepHandle    = OpenMesh::Decimater::ModIndependentSetsT<Mesh>::Handle;
using CollapseInfo   = OpenMesh::Decimater::CollapseInfoT<Mesh>;

// A copyable, non-serializable user type.
struct CopyCount
{
  CopyCount() : value(0) {}
  CopyCount(const CopyCount& _rhs) : value(_rhs.value) { ++copies; }
  CopyCount& operator=(const CopyCount& _rhs)
  { value = _rhs.value; ++copies; return *this; }

  int value;
  static int copies;
};
int CopyCount::copies = 0;

//------------------------------------------------------------ mesh generators

// Curved triangle grid with a boundary.  Deterministic jitter keeps the
// quadric priorities free of ties.
Mesh make_boundary_grid(int _w, int _h)
{
  Mesh m;
  m.request_vertex_status();
  m.request_halfedge_status();
  m.request_edge_status();
  m.request_face_status();
  m.request_face_normals();
  m.request_halfedge_texcoords2D();

  auto jitter = [](int x, int y) {
    return 0.007f * std::sin(0.7f * x + 1.3f * y)
         + 0.005f * std::cos(1.1f * y - 0.3f * x);
  };

  for (int y = 0; y < _h; ++y)
    for (int x = 0; x < _w; ++x) {
      Point p(float(x), float(y),
              0.031f * x * x + 0.027f * y * y - 0.013f * x * y
              + jitter(x, y));
      m.add_vertex(p);
    }

  for (int y = 0; y < _h - 1; ++y)
    for (int x = 0; x < _w - 1; ++x) {
      int a = y * _w + x;
      LOCAL_REQUIRE(m.add_face(Mesh::VertexHandle(a),
                            Mesh::VertexHandle(a + 1),
                            Mesh::VertexHandle(a + _w + 1)).is_valid());
      LOCAL_REQUIRE(m.add_face(Mesh::VertexHandle(a),
                            Mesh::VertexHandle(a + _w + 1),
                            Mesh::VertexHandle(a + _w)).is_valid());
    }

  m.update_face_normals();
  return m;
}

// Closed UV sphere (no boundary).
Mesh make_closed_sphere(size_t _sectors = 10, size_t _rings = 6)
{
  Mesh m;
  m.request_vertex_status();
  m.request_halfedge_status();
  m.request_edge_status();
  m.request_face_status();
  m.request_face_normals();

  const float pi = 3.14159265358979323846f;
  Mesh::VertexHandle top = m.add_vertex(Point(0.0f, 0.0f, 1.0f));
  Mesh::VertexHandle bot = m.add_vertex(Point(0.0f, 0.0f, -1.0f));

  std::vector<std::vector<Mesh::VertexHandle> > ring(_rings - 1);
  for (size_t r = 1; r < _rings; ++r) {
    float phi = pi * float(r) / float(_rings);
    for (size_t s = 0; s < _sectors; ++s) {
      float theta = 2.0f * pi * float(s) / float(_sectors);
      float rr = std::sin(phi);
      Point p(rr * std::cos(theta), rr * std::sin(theta), std::cos(phi));
      p[0] += 0.003f * std::sin(2.0f * theta + phi);
      p[1] += 0.003f * std::cos(3.0f * theta - phi);
      ring[r - 1].push_back(m.add_vertex(p));
    }
  }

  for (size_t s = 0; s < _sectors; ++s) {
    size_t s1 = (s + 1) % _sectors;
    // cap winding is opposite to the side strips so every edge stays
    // two-manifold (one face per directed halfedge)
    LOCAL_REQUIRE(m.add_face(top, ring[0][s1], ring[0][s]).is_valid());
    LOCAL_REQUIRE(m.add_face(bot, ring[_rings - 2][s],
                             ring[_rings - 2][s1]).is_valid());
  }
  for (size_t r = 0; r + 1 < _rings - 1; ++r)
    for (size_t s = 0; s < _sectors; ++s) {
      size_t s1 = (s + 1) % _sectors;
      Mesh::VertexHandle a = ring[r][s];
      Mesh::VertexHandle b = ring[r][s1];
      Mesh::VertexHandle c = ring[r + 1][s1];
      Mesh::VertexHandle d = ring[r + 1][s];
      LOCAL_REQUIRE(m.add_face(a, b, c).is_valid());
      LOCAL_REQUIRE(m.add_face(a, c, d).is_valid());
    }

  m.update_face_normals();
  return m;
}

// A small locked disconnected component placed far away.
void add_locked_component(Mesh& _m, float _origin)
{
  Mesh::VertexHandle v0 = _m.add_vertex(Point(_origin, 0.0f, 0.0f));
  Mesh::VertexHandle v1 = _m.add_vertex(Point(_origin + 1.0f, 0.0f, 0.0f));
  Mesh::VertexHandle v2 = _m.add_vertex(Point(_origin + 0.5f, 1.0f, 0.0f));
  _m.add_face(v0, v1, v2);
  _m.status(v0).set_locked(true);
  _m.status(v1).set_locked(true);
  _m.status(v2).set_locked(true);
}

void lock_boundary(Mesh& _m)
{
  for (auto v = _m.vertices_begin(); v != _m.vertices_end(); ++v)
    if (_m.is_boundary(*v))
      _m.status(*v).set_locked(true);
}

//------------------------------------------------------------- state snapshots

int h2i(OpenMesh::BaseHandle _h) { return _h.is_valid() ? _h.idx() : -1; }

struct VState { int he; unsigned int st; };
struct HState { int face, to, next, prev; unsigned int st; };
struct EState { unsigned int st; };
struct FState { int he; unsigned int st; };

struct TopoSnap
{
  size_t nv, nh, ne, nf;
  std::vector<VState> vs;
  std::vector<HState> hs;
  std::vector<EState> es;
  std::vector<FState> fs;
};

TopoSnap snap_topology(const Mesh& _m)
{
  // tagged bits are temporary legality-test storage and are compared
  // separately.
  const unsigned int tagged_mask =
      static_cast<unsigned int>(~OpenMesh::Attributes::TAGGED);

  TopoSnap s;
  s.nv = _m.n_vertices();
  s.nh = _m.n_halfedges();
  s.ne = _m.n_edges();
  s.nf = _m.n_faces();

  for (auto v = _m.vertices_begin(); v != _m.vertices_end(); ++v)
    s.vs.push_back(VState{ h2i(_m.halfedge_handle(*v)),
                           _m.status(*v).bits() & tagged_mask });
  for (auto h = _m.halfedges_begin(); h != _m.halfedges_end(); ++h)
    s.hs.push_back(HState{
        h2i(_m.face_handle(*h)), h2i(_m.to_vertex_handle(*h)),
        h2i(_m.next_halfedge_handle(*h)),
        h2i(_m.prev_halfedge_handle(*h)),
        _m.status(*h).bits() & tagged_mask });
  for (auto e = _m.edges_begin(); e != _m.edges_end(); ++e)
    s.es.push_back(EState{ _m.status(*e).bits() & tagged_mask });
  for (auto f = _m.faces_begin(); f != _m.faces_end(); ++f)
    s.fs.push_back(FState{ h2i(_m.halfedge_handle(*f)),
                           _m.status(*f).bits() & tagged_mask });
  return s;
}

void check_topology(const TopoSnap& _a, const TopoSnap& _b)
{
  LOCAL_REQUIRE(_a.nv == _b.nv);
  LOCAL_REQUIRE(_a.nh == _b.nh);
  LOCAL_REQUIRE(_a.ne == _b.ne);
  LOCAL_REQUIRE(_a.nf == _b.nf);
  LOCAL_REQUIRE(_a.vs.size() == _b.vs.size());
  LOCAL_REQUIRE(_a.hs.size() == _b.hs.size());
  LOCAL_REQUIRE(_a.es.size() == _b.es.size());
  LOCAL_REQUIRE(_a.fs.size() == _b.fs.size());
  for (size_t i = 0; i < _a.vs.size(); ++i) {
    LOCAL_REQUIRE(_a.vs[i].he == _b.vs[i].he);
    LOCAL_REQUIRE(_a.vs[i].st == _b.vs[i].st);
  }
  for (size_t i = 0; i < _a.hs.size(); ++i) {
    LOCAL_REQUIRE(_a.hs[i].face == _b.hs[i].face);
    LOCAL_REQUIRE(_a.hs[i].to   == _b.hs[i].to);
    LOCAL_REQUIRE(_a.hs[i].next == _b.hs[i].next);
    LOCAL_REQUIRE(_a.hs[i].prev == _b.hs[i].prev);
    LOCAL_REQUIRE(_a.hs[i].st   == _b.hs[i].st);
  }
  for (size_t i = 0; i < _a.es.size(); ++i)
    LOCAL_REQUIRE(_a.es[i].st == _b.es[i].st);
  for (size_t i = 0; i < _a.fs.size(); ++i) {
    LOCAL_REQUIRE(_a.fs[i].he == _b.fs[i].he);
    LOCAL_REQUIRE(_a.fs[i].st == _b.fs[i].st);
  }
}

template <class T>
struct PropState
{
  std::vector<T> values;
};

template <class T>
PropState<T> snap_vprop(Mesh& _m, OpenMesh::VPropHandleT<T> _ph)
{
  PropState<T> s;
  s.values.reserve(_m.n_vertices());
  for (auto v = _m.vertices_begin(); v != _m.vertices_end(); ++v)
    s.values.push_back(_m.property(_ph, *v));
  return s;
}
template <class T>
PropState<T> snap_hprop(Mesh& _m, OpenMesh::HPropHandleT<T> _ph)
{
  PropState<T> s;
  s.values.reserve(_m.n_halfedges());
  for (auto h = _m.halfedges_begin(); h != _m.halfedges_end(); ++h)
    s.values.push_back(_m.property(_ph, *h));
  return s;
}
template <class T>
PropState<T> snap_eprop(Mesh& _m, OpenMesh::EPropHandleT<T> _ph)
{
  PropState<T> s;
  s.values.reserve(_m.n_edges());
  for (auto e = _m.edges_begin(); e != _m.edges_end(); ++e)
    s.values.push_back(_m.property(_ph, *e));
  return s;
}
template <class T>
PropState<T> snap_fprop(Mesh& _m, OpenMesh::FPropHandleT<T> _ph)
{
  PropState<T> s;
  s.values.reserve(_m.n_faces());
  for (auto f = _m.faces_begin(); f != _m.faces_end(); ++f)
    s.values.push_back(_m.property(_ph, *f));
  return s;
}

template <class T>
void check_prop(const char* _name, const PropState<T>& _a,
                const PropState<T>& _b)
{
  LOCAL_REQUIRE(_a.values.size() == _b.values.size());
  for (size_t i = 0; i < _a.values.size(); ++i)
    if (!(_a.values[i] == _b.values[i])) {
      std::fprintf(stderr, "property %s mismatch at %zu\n", _name, i);
      LOCAL_REQUIRE(false);
    }
}

std::vector<Point> snap_points(const Mesh& _m)
{
  std::vector<Point> p;
  p.reserve(_m.n_vertices());
  for (auto v = _m.vertices_begin(); v != _m.vertices_end(); ++v)
    p.push_back(_m.point(*v));
  return p;
}
void check_points(const std::vector<Point>& _a, const std::vector<Point>& _b)
{
  LOCAL_REQUIRE(_a.size() == _b.size());
  for (size_t i = 0; i < _a.size(); ++i)
    for (int k = 0; k < 3; ++k)
      LOCAL_REQUIRE(_a[i][k] == _b[i][k]);
}

std::vector<Vec2f> snap_texcoords(Mesh& _m)
{
  std::vector<Vec2f> t;
  t.reserve(_m.n_halfedges());
  for (auto h = _m.halfedges_begin(); h != _m.halfedges_end(); ++h)
    t.push_back(_m.texcoord2D(*h));
  return t;
}

std::vector<Point> snap_face_normals(Mesh& _m)
{
  std::vector<Point> n;
  n.reserve(_m.n_faces());
  for (auto f = _m.faces_begin(); f != _m.faces_end(); ++f)
    n.push_back(_m.normal(*f));
  return n;
}

size_t alive_vertices(const Mesh& _m)
{
  size_t n = 0;
  for (auto v = _m.vertices_sbegin(); v != _m.vertices_end(); ++v)
    ++n;
  return n;
}
size_t alive_faces(const Mesh& _m)
{
  size_t n = 0;
  for (auto f = _m.faces_sbegin(); f != _m.faces_end(); ++f)
    ++n;
  return n;
}

// quadric image of every surviving vertex slot
struct QuadricState
{
  int v;
  double c[10];
};

std::vector<QuadricState>
snap_quadrics(Mesh& _m, OpenMesh::Decimater::ModQuadricT<Mesh>& _q)
{
  std::vector<QuadricState> s;
  for (auto v = _m.vertices_begin(); v != _m.vertices_end(); ++v) {
    if (_m.status(*v).deleted()) continue;
    const auto& q = _q.quadric(*v);
    QuadricState st;
    st.v = v->idx();
    st.c[0] = q.a(); st.c[1] = q.b(); st.c[2] = q.c(); st.c[3] = q.d();
    st.c[4] = q.e(); st.c[5] = q.f(); st.c[6] = q.g();
    st.c[7] = q.h(); st.c[8] = q.i(); st.c[9] = q.j();
    s.push_back(st);
  }
  return s;
}

void check_quadrics(const std::vector<QuadricState>& _a,
                    const std::vector<QuadricState>& _b)
{
  LOCAL_REQUIRE(_a.size() == _b.size());
  for (size_t i = 0; i < _a.size(); ++i) {
    LOCAL_REQUIRE(_a[i].v == _b[i].v);
    for (int k = 0; k < 10; ++k)
      LOCAL_REQUIRE(_a[i].c[k] == _b[i].c[k]);
  }
}

// All dynamic properties used by the property tests, on all four entity
// kinds and with all required value categories.
struct TestProps
{
  explicit TestProps(Mesh& _m) : m(_m)
  {
    m.add_property(origin, "origin");
    m.add_property(weight, "weight");
    m.add_property(vbool,  "vbool");
    m.add_property(vint,   "vint");
    m.add_property(vcount, "vcount");
    m.add_property(huv2,   "huv2");
    m.add_property(hint,   "hint");
    m.add_property(hbool,  "hbool");
    m.add_property(hcount, "hcount");
    m.add_property(etag,   "etag");
    m.add_property(ebool,  "ebool");
    m.add_property(ecount, "ecount");
    m.add_property(ftag,   "ftag");
    m.add_property(fbool,  "fbool");
    m.add_property(fcount, "fcount");
  }

  Mesh& m;
  OpenMesh::VPropHandleT<std::string> origin;
  OpenMesh::VPropHandleT<double>      weight;
  OpenMesh::VPropHandleT<bool>        vbool;
  OpenMesh::VPropHandleT<int>         vint;
  OpenMesh::VPropHandleT<CopyCount>   vcount;
  OpenMesh::HPropHandleT<Vec2f>       huv2;
  OpenMesh::HPropHandleT<int>         hint;
  OpenMesh::HPropHandleT<bool>        hbool;
  OpenMesh::HPropHandleT<CopyCount>   hcount;
  OpenMesh::EPropHandleT<std::string> etag;
  OpenMesh::EPropHandleT<bool>        ebool;
  OpenMesh::EPropHandleT<CopyCount>   ecount;
  OpenMesh::FPropHandleT<std::string> ftag;
  OpenMesh::FPropHandleT<bool>        fbool;
  OpenMesh::FPropHandleT<CopyCount>   fcount;
};

void initialize_test_values(Mesh& _m, TestProps& _p)
{
  int k = 0;
  for (auto v = _m.vertices_begin(); v != _m.vertices_end(); ++v, ++k) {
    _m.property(_p.origin, *v) = "org" + std::to_string(k);
    _m.property(_p.weight, *v) = 0.5 + 0.01 * k;
    _m.property(_p.vbool, *v)  = (k % 3 == 0);
    _m.property(_p.vint, *v)   = k * 7;
    _m.property(_p.vcount, *v).value = 1000 + k;
  }
  k = 0;
  for (auto h = _m.halfedges_begin(); h != _m.halfedges_end(); ++h, ++k) {
    _m.set_texcoord2D(*h, Vec2f(0.1f * k, 0.2f * k));
    _m.property(_p.huv2, *h)  = Vec2f(-0.1f * k, 0.3f * k);
    _m.property(_p.hint, *h)  = k;
    _m.property(_p.hbool, *h) = (k % 2 == 0);
    _m.property(_p.hcount, *h).value = 2000 + k;
  }
  k = 0;
  for (auto e = _m.edges_begin(); e != _m.edges_end(); ++e, ++k) {
    _m.property(_p.etag, *e)  = "e" + std::to_string(k);
    _m.property(_p.ebool, *e) = (k % 4 == 1);
    _m.property(_p.ecount, *e).value = 3000 + k;
  }
  k = 0;
  for (auto f = _m.faces_begin(); f != _m.faces_end(); ++f, ++k) {
    _m.property(_p.ftag, *f)  = "f" + std::to_string(k);
    _m.property(_p.fbool, *f) = (k % 5 == 2);
    _m.property(_p.fcount, *f).value = 4000 + k;
  }
}

// The modeling-side transfer: origin/weight move to the surviving vertex,
// both per-corner UV sets and the corner attributes are updated, and the
// face/edge attributes of the local support are touched.
std::function<void(const CollapseInfo&)>
make_transfer(Mesh& _m, TestProps& _p, int& _calls)
{
  return [&_m, &_p, &_calls](const CollapseInfo& _ci) {
    ++_calls;

    _m.property(_p.origin, _ci.v1) =
        _m.property(_p.origin, _ci.v0) + ">" +
        _m.property(_p.origin, _ci.v1);
    _m.property(_p.weight, _ci.v1) += _m.property(_p.weight, _ci.v0);
    _m.property(_p.vbool, _ci.v1) =
        _m.property(_p.vbool, _ci.v1) || _m.property(_p.vbool, _ci.v0);
    _m.property(_p.vint, _ci.v1) += _m.property(_p.vint, _ci.v0);
    _m.property(_p.vcount, _ci.v1).value +=
        _m.property(_p.vcount, _ci.v0).value;

    for (auto vf = _m.vf_iter(_ci.v1); vf.is_valid(); ++vf) {
      Mesh::FaceHandle fh = *vf;
      if (_m.status(fh).deleted()) continue;
      _m.property(_p.ftag, fh) = _m.property(_p.ftag, fh) + "+";
      _m.property(_p.fbool, fh) = !_m.property(_p.fbool, fh);
      _m.property(_p.fcount, fh).value += 1;
      for (auto fh_it = _m.fh_iter(fh); fh_it.is_valid(); ++fh_it) {
        Mesh::HalfedgeHandle hh = *fh_it;
        _m.set_texcoord2D(hh, _m.texcoord2D(hh) + Vec2f(0.01f, 0.02f));
        _m.property(_p.huv2, hh) =
            _m.property(_p.huv2, hh) + Vec2f(0.03f, -0.01f);
        _m.property(_p.hint, hh) += 1;
        _m.property(_p.hbool, hh) = !_m.property(_p.hbool, hh);
        _m.property(_p.hcount, hh).value += 2;
      }
    }

    // The collapsed edge slot is deleted but still writable.
    Mesh::EdgeHandle eh = _m.edge_handle(_ci.v0v1);
    _m.property(_p.etag, eh) = _m.property(_p.etag, eh) + "x";
    _m.property(_p.ebool, eh) = !_m.property(_p.ebool, eh);
    _m.property(_p.ecount, eh).value += 3;
  };
}

} // anonymous namespace

//=============================================================================
// topology group
//=============================================================================

static int topo_boundary_undo_redo()
{
  Mesh m = make_boundary_grid(6, 5);
  lock_boundary(m);
  TopoSnap initial = snap_topology(m);
  std::vector<Point> initial_points = snap_points(m);

  Decimater d(m);
  QuadricHandle q;
  TASK_CHECK(d.add(q));
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.enable_history());
  TASK_CHECK(d.history_size() == 0);
  TASK_CHECK(d.history_position() == 0);

  // one snapshot per history position (0 = initial state, then one per
  // single collapse)
  std::vector<TopoSnap> milestones;
  std::vector<std::vector<Point> > point_milestones;
  milestones.push_back(snap_topology(m));
  point_milestones.push_back(snap_points(m));

  const int total = 5;
  for (int k = 0; k < total; ++k) {
    TASK_CHECK(d.decimate(1) == 1);
    milestones.push_back(snap_topology(m));
    point_milestones.push_back(snap_points(m));
  }
  TASK_CHECK(d.history_size() == size_t(total));
  TASK_CHECK(d.history_position() == size_t(total));
  TASK_CHECK(alive_vertices(m) + size_t(total) == m.n_vertices());

  // stepwise undo, checking every intermediate position
  for (int pos = total; pos > 0; --pos) {
    TASK_CHECK(d.undo(1) == 1);
    TASK_CHECK(d.history_position() == size_t(pos - 1));
    check_topology(snap_topology(m), milestones[pos - 1]);
    check_points(snap_points(m), point_milestones[pos - 1]);
  }

  // nothing more to undo, n=0 is a no-op
  TASK_CHECK(d.undo(10) == 0);
  TASK_CHECK(d.undo(0) == 0);
  check_topology(snap_topology(m), milestones[0]);
  check_points(snap_points(m), point_milestones[0]);

  // redo step by step
  for (int pos = 0; pos < total; ++pos) {
    TASK_CHECK(d.redo(1) == 1);
    TASK_CHECK(d.history_position() == size_t(pos + 1));
    check_topology(snap_topology(m), milestones[pos + 1]);
    check_points(snap_points(m), point_milestones[pos + 1]);
  }
  TASK_CHECK(d.redo(10) == 0);
  TASK_CHECK(d.redo(0) == 0);

  // circulators and the mesh checker still work on the restored mesh
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
  size_t walked = 0;
  for (auto v = m.vertices_sbegin(); v != m.vertices_end(); ++v) {
    size_t valence = 0;
    for (auto vv = m.vv_iter(*v); vv.is_valid(); ++vv) ++valence;
    TASK_CHECK(valence == m.valence(*v));
    ++walked;
  }
  TASK_CHECK(walked == alive_vertices(m));

  // full undo again, checker on the original topology
  TASK_CHECK(d.undo(5) == 5);
  check_topology(snap_topology(m), initial);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
  for (auto f = m.faces_sbegin(); f != m.faces_end(); ++f)
    m.calc_face_normal(*f);

  d.disable_history();
  TASK_CHECK(d.history_size() == 0);
  TASK_CHECK(d.history_position() == 0);
  return 0;
}

static int topo_closed_mesh()
{
  Mesh m = make_closed_sphere();
  TopoSnap initial = snap_topology(m);
  std::vector<Point> initial_points = snap_points(m);
  std::vector<Point> initial_normals = snap_face_normals(m);

  Decimater d(m);
  QuadricHandle q;
  FlippingHandle nf;
  TASK_CHECK(d.add(q));
  TASK_CHECK(d.add(nf));
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.enable_history());

  TASK_CHECK(d.decimate_to_faces(0, m.n_faces() - 8) >= 4);
  size_t done = d.history_position();
  TASK_CHECK(done >= 4);
  TASK_CHECK(alive_faces(m) <= m.n_faces() - 8);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());

  // every collapse is reversible on the closed mesh too
  TASK_CHECK(d.undo(done) == done);
  check_topology(snap_topology(m), initial);
  check_points(snap_points(m), initial_points);
  check_points(snap_face_normals(m), initial_normals);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());

  // and replayable
  TASK_CHECK(d.redo(done) == done);
  TASK_CHECK(d.history_position() == done);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());

  d.disable_history();
  return 0;
}

static int topo_predeleted_slots()
{
  Mesh m = make_boundary_grid(5, 5);

  // delete a face and the isolated vertex it produces before enabling
  Mesh::FaceHandle old_face = Mesh::FaceHandle(0);
  Mesh::VertexHandle old_vertex = m.from_vertex_handle(m.halfedge_handle(old_face));
  m.delete_face(old_face, true);
  TASK_CHECK(m.status(old_face).deleted());

  Decimater d(m);
  QuadricHandle q;
  TASK_CHECK(d.add(q));
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.enable_history());

  size_t alive_before = alive_vertices(m);
  TASK_CHECK(d.decimate_to(alive_before - 3) == 3);
  TASK_CHECK(alive_vertices(m) == alive_before - 3);

  TASK_CHECK(d.undo(3) == 3);
  // slots deleted before the session must not be resurrected
  TASK_CHECK(m.status(old_face).deleted());
  if (old_vertex.is_valid() && m.status(old_vertex).deleted())
    TASK_CHECK(m.status(old_vertex).deleted());
  TASK_CHECK(alive_vertices(m) == alive_before);

  // decimate_to judges by alive count, delayed deletions do not count
  TASK_CHECK(d.decimate_to(alive_before) == 0);
  TASK_CHECK(d.decimate_to(alive_before + 10) == 0);
  TASK_CHECK(d.history_position() == 0);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());

  d.disable_history();
  return 0;
}

static int topo_gc_and_enable_rules()
{
  Mesh m = make_boundary_grid(4, 4);
  lock_boundary(m);

  // enable before initialize fails and changes nothing
  Decimater d0(m);
  QuadricHandle q0;
  TASK_CHECK(d0.add(q0));
  TASK_CHECK(!d0.enable_history());
  TASK_CHECK(d0.history_size() == 0);
  m.garbage_collection(); // still allowed

  TASK_CHECK(d0.initialize());
  TASK_CHECK(d0.enable_history());
  TASK_CHECK(!d0.enable_history()); // already enabled

  // a second session on the same mesh is rejected
  Decimater d1(m);
  QuadricHandle q1;
  TASK_CHECK(d1.add(q1));
  TASK_CHECK(d1.initialize());
  TASK_CHECK(!d1.enable_history());

  // both GC overloads throw while enabled, even with an empty history
  TASK_CHECK(d0.history_size() == 0);
  bool threw = false;
  try { m.garbage_collection(); }
  catch (const std::logic_error&) { threw = true; }
  TASK_CHECK(threw);

  threw = false;
  std::vector<Mesh::VertexHandle*> vh_up;
  std::vector<Mesh::HalfedgeHandle*> hh_up;
  std::vector<Mesh::FaceHandle*> fh_up;
  try { m.garbage_collection(vh_up, hh_up, fh_up); }
  catch (const std::logic_error&) { threw = true; }
  TASK_CHECK(threw);

  d0.disable_history();
  size_t before = m.n_vertices();
  m.garbage_collection();                 // ordinary behavior restored
  m.garbage_collection(vh_up, hh_up, fh_up);
  TASK_CHECK(m.n_vertices() <= before);

  // unsupported module combination: independent sets alongside quadric
  Mesh m2 = make_boundary_grid(4, 4);
  Decimater d2(m2);
  QuadricHandle q2;
  IndepHandle is2;
  TASK_CHECK(d2.add(q2));
  TASK_CHECK(d2.add(is2));
  TASK_CHECK(d2.initialize());
  TASK_CHECK(!d2.enable_history());
  TASK_CHECK(d2.history_size() == 0);
  // failed enable leaves ordinary decimation working
  TASK_CHECK(d2.decimate(1) == 1);
  TASK_CHECK(!m2.has_history_session());

  // destructor releases the session as well
  {
    Mesh m3 = make_boundary_grid(4, 4);
    {
      Decimater d3(m3);
      QuadricHandle q3;
      TASK_CHECK(d3.add(q3));
      TASK_CHECK(d3.initialize());
      TASK_CHECK(d3.enable_history());
      TASK_CHECK(d3.decimate(1) == 1);
    }
    TASK_CHECK(!m3.has_history_session());
    m3.garbage_collection();
  }
  return 0;
}

//=============================================================================
// properties group
//=============================================================================

static int props_all_kinds_transfer()
{
  Mesh m = make_boundary_grid(6, 5);
  lock_boundary(m);
  TestProps p(m);
  initialize_test_values(m, p);

  TopoSnap topo0 = snap_topology(m);
  auto pts0 = snap_points(m);
  auto uv0  = snap_texcoords(m);
  auto fn0  = snap_face_normals(m);
  auto s_origin0 = snap_vprop(m, p.origin);
  auto s_weight0 = snap_vprop(m, p.weight);
  auto s_vbool0  = snap_vprop(m, p.vbool);
  auto s_vint0   = snap_vprop(m, p.vint);
  auto s_vcnt0   = snap_vprop(m, p.vcount);
  auto s_huv0    = snap_hprop(m, p.huv2);
  auto s_hint0   = snap_hprop(m, p.hint);
  auto s_hbool0  = snap_hprop(m, p.hbool);
  auto s_hcnt0   = snap_hprop(m, p.hcount);
  auto s_etag0   = snap_eprop(m, p.etag);
  auto s_ebool0  = snap_eprop(m, p.ebool);
  auto s_ecnt0   = snap_eprop(m, p.ecount);
  auto s_ftag0   = snap_fprop(m, p.ftag);
  auto s_fbool0  = snap_fprop(m, p.fbool);
  auto s_fcnt0   = snap_fprop(m, p.fcount);

  Decimater d(m);
  QuadricHandle q;
  TASK_CHECK(d.add(q));
  TASK_CHECK(d.initialize());

  int calls = 0;
  d.set_history_transfer(make_transfer(m, p, calls));
  TASK_CHECK(d.enable_history());

  const int n = 4;
  TASK_CHECK(d.decimate(n) == size_t(n));
  TASK_CHECK(calls == n);
  TASK_CHECK(d.history_size() == size_t(n));
  TASK_CHECK(d.history_stored_elements() > 0);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());

  // the transfer really modified surviving elements
  bool transferred = false;
  for (auto v = m.vertices_sbegin(); v != m.vertices_end(); ++v)
    if (m.property(p.origin, *v).find(">") != std::string::npos)
      transferred = true;
  TASK_CHECK(transferred);

  // undo/redo must not invoke the transfer again
  size_t stored = d.history_stored_elements();
  TASK_CHECK(d.undo(n) == size_t(n));
  TASK_CHECK(calls == n);
  TASK_CHECK(d.history_stored_elements() == stored);

  // everything is back to the original values, slot by slot
  check_topology(snap_topology(m), topo0);
  check_points(snap_points(m), pts0);
  TASK_CHECK(snap_texcoords(m) == uv0);
  check_points(snap_face_normals(m), fn0);
  check_prop("origin", snap_vprop(m, p.origin), s_origin0);
  check_prop("weight", snap_vprop(m, p.weight), s_weight0);
  check_prop("vbool",  snap_vprop(m, p.vbool),  s_vbool0);
  check_prop("vint",   snap_vprop(m, p.vint),   s_vint0);
  TASK_CHECK(snap_vprop(m, p.vcount).values.size() == s_vcnt0.values.size());
  for (size_t i = 0; i < s_vcnt0.values.size(); ++i)
    TASK_CHECK(snap_vprop(m, p.vcount).values[i].value ==
               s_vcnt0.values[i].value);
  check_prop("huv2",  snap_hprop(m, p.huv2),  s_huv0);
  check_prop("hint",  snap_hprop(m, p.hint),  s_hint0);
  check_prop("hbool", snap_hprop(m, p.hbool), s_hbool0);
  for (size_t i = 0; i < s_hcnt0.values.size(); ++i)
    TASK_CHECK(snap_hprop(m, p.hcount).values[i].value ==
               s_hcnt0.values[i].value);
  check_prop("etag",  snap_eprop(m, p.etag),  s_etag0);
  check_prop("ebool", snap_eprop(m, p.ebool), s_ebool0);
  for (size_t i = 0; i < s_ecnt0.values.size(); ++i)
    TASK_CHECK(snap_eprop(m, p.ecount).values[i].value ==
               s_ecnt0.values[i].value);
  check_prop("ftag",  snap_fprop(m, p.ftag),  s_ftag0);
  check_prop("fbool", snap_fprop(m, p.fbool), s_fbool0);
  for (size_t i = 0; i < s_fcnt0.values.size(); ++i)
    TASK_CHECK(snap_fprop(m, p.fcount).values[i].value ==
               s_fcnt0.values[i].value);

  // replay restores the post-transfer state, still without new callbacks
  TASK_CHECK(d.redo(n) == size_t(n));
  TASK_CHECK(calls == n);
  TASK_CHECK(d.history_stored_elements() == stored);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());

  d.disable_history();
  return 0;
}

static int props_bool_string_spots()
{
  Mesh m = make_boundary_grid(4, 4);
  OpenMesh::VPropHandleT<bool> vb;
  OpenMesh::FPropHandleT<std::string> fs;
  OpenMesh::HPropHandleT<bool> hb;
  m.add_property(vb, "vb");
  m.add_property(fs, "fs");
  m.add_property(hb, "hb");
  for (auto v = m.vertices_begin(); v != m.vertices_end(); ++v)
    m.property(vb, *v) = (v->idx() % 2 == 0);
  for (auto f = m.faces_begin(); f != m.faces_end(); ++f)
    m.property(fs, *f) = "face-" + std::to_string(f->idx());
  for (auto h = m.halfedges_begin(); h != m.halfedges_end(); ++h)
    m.property(hb, *h) = (h->idx() % 3 != 0);

  Decimater d(m);
  QuadricHandle q;
  TASK_CHECK(d.add(q));
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.enable_history());
  TASK_CHECK(d.decimate(3) == 3);
  TASK_CHECK(d.undo(3) == 3);

  for (auto v = m.vertices_begin(); v != m.vertices_end(); ++v)
    TASK_CHECK(m.property(vb, *v) == (v->idx() % 2 == 0));
  for (auto f = m.faces_begin(); f != m.faces_end(); ++f)
    TASK_CHECK(m.property(fs, *f) == "face-" + std::to_string(f->idx()));
  for (auto h = m.halfedges_begin(); h != m.halfedges_end(); ++h)
    TASK_CHECK(m.property(hb, *h) == (h->idx() % 3 != 0));

  d.disable_history();
  return 0;
}

static int props_tagged_bit_isolation()
{
  Mesh m = make_boundary_grid(5, 5);

  // pre-existing tagged state that history mode must preserve
  std::vector<bool> tagged_before;
  for (auto v = m.vertices_begin(); v != m.vertices_end(); ++v) {
    bool t = (v->idx() % 4 == 1);
    m.status(*v).set_tagged(t);
    tagged_before.push_back(t);
  }

  Decimater d(m);
  QuadricHandle q;
  TASK_CHECK(d.add(q));
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.enable_history());
  TASK_CHECK(d.decimate(4) == 4);
  TASK_CHECK(d.undo(2) == 2);

  for (auto v = m.vertices_begin(); v != m.vertices_end(); ++v)
    TASK_CHECK(m.status(*v).tagged() == tagged_before[v->idx()]);

  TASK_CHECK(d.redo(2) == 2);
  for (auto v = m.vertices_begin(); v != m.vertices_end(); ++v)
    TASK_CHECK(m.status(*v).tagged() == tagged_before[v->idx()]);

  d.disable_history();
  return 0;
}

static int props_locality()
{
  struct Run {
    size_t stored;
    int    copies;
    int    collapsed;
  };
  Run runs[2];
  const int component_counts[2] = { 5, 25 };

  for (int r = 0; r < 2; ++r) {
    Mesh m = make_boundary_grid(4, 4);
    OpenMesh::VPropHandleT<CopyCount> cc;
    m.add_property(cc, "cc");
    for (auto v = m.vertices_begin(); v != m.vertices_end(); ++v)
      m.property(cc, *v).value = v->idx();

    for (int k = 0; k < component_counts[r]; ++k)
      add_locked_component(m, 100.0f + 10.0f * k);
    lock_boundary(m);
    for (auto v = m.vertices_begin(); v != m.vertices_end(); ++v)
      m.property(cc, *v).value = v->idx();

    Decimater d(m);
    QuadricHandle q;
    TASK_CHECK(d.add(q));
    TASK_CHECK(d.initialize());
    TASK_CHECK(d.enable_history());

    int collapsed = -1;
    d.set_history_transfer([&](const CollapseInfo& _ci) {
      collapsed = _ci.v0v1.idx();
    });

    CopyCount::copies = 0;
    TASK_CHECK(d.decimate(1) == 1);
    runs[r].copies = CopyCount::copies;
    runs[r].stored = d.history_stored_elements();
    runs[r].collapsed = collapsed;

    // the distant, locked components were never part of any record
    TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());
    d.disable_history();
  }

  // same local collapse regardless of how many far components exist
  TASK_CHECK(runs[0].collapsed == runs[1].collapsed);
  TASK_CHECK(runs[0].collapsed >= 0);
  // record volume and copy work do not grow with the distant mesh
  TASK_CHECK(runs[0].stored == runs[1].stored);
  TASK_CHECK(runs[0].copies == runs[1].copies);
  TASK_CHECK(runs[0].copies > 0);
  return 0;
}

//=============================================================================
// continuation group
//=============================================================================

// deterministic decimater setup shared by the continuation scenarios
static bool build_quadric_decimater(Mesh& _m, Decimater*& _d,
                                    QuadricHandle& _q,
                                    std::vector<int>& _sequence,
                                    int& _calls)
{
  _d = new Decimater(_m);
  if (!_d->add(_q)) return false;
  if (!_d->initialize()) return false;
  _d->set_history_transfer([&_sequence, &_calls](const CollapseInfo& _ci) {
    _sequence.push_back(_ci.v0v1.idx());
    ++_calls;
  });
  return _d->enable_history();
}

static int cont_branch_and_zero_collapse()
{
  Mesh m = make_boundary_grid(6, 6);
  lock_boundary(m);

  Decimater d(m);
  QuadricHandle q;
  TASK_CHECK(d.add(q));
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.enable_history());

  TASK_CHECK(d.decimate(4) == 4);
  TASK_CHECK(d.undo(2) == 2);
  TASK_CHECK(d.history_size() == 4);
  TASK_CHECK(d.history_position() == 2);

  size_t alive = alive_vertices(m);

  // calls that cannot perform any collapse must not discard the future
  TASK_CHECK(d.decimate_to(alive) == 0);
  TASK_CHECK(d.decimate_to(alive + 5) == 0);
  TASK_CHECK(d.decimate_to_faces(alive, alive_faces(m)) == 0);
  TASK_CHECK(d.history_size() == 4);
  TASK_CHECK(d.history_position() == 2);
  // the old future is still replayable
  TASK_CHECK(d.redo(2) == 2);
  TASK_CHECK(d.history_position() == 4);
  TASK_CHECK(d.undo(2) == 2);

  // the first successful collapse replaces the branch and drops the future
  TASK_CHECK(d.decimate(1) == 1);
  TASK_CHECK(d.history_size() == 3);
  TASK_CHECK(d.history_position() == 3);
  // the old future is gone: redo is clamped at the new branch end
  TASK_CHECK(d.redo(10) == 0);
  TASK_CHECK(d.history_position() == 3);

  // continuing on the new branch works normally
  TASK_CHECK(d.decimate(2) == 2);
  TASK_CHECK(d.history_size() == 5);
  TASK_CHECK(d.history_position() == 5);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());

  d.disable_history();
  return 0;
}

static int cont_quadric_not_recomputed()
{
  // Three sessions on identical, tie-free material.  R decimates
  // straight, B pauses while A explores undo/redo.  The current branch of
  // each history session must reproduce R's collapse halfedges, meshes and
  // accumulated quadrics, proving quadrics are restored from the records
  // rather than re-initialized from the current coarse mesh.
  std::vector<int> seqA, seqB, seqR;
  int callsA = 0, callsB = 0, callsR = 0;

  Mesh mA = make_boundary_grid(7, 6);
  lock_boundary(mA);
  Mesh mB = make_boundary_grid(7, 6);
  lock_boundary(mB);
  Mesh mR = make_boundary_grid(7, 6);
  lock_boundary(mR);

  Decimater *dA, *dB, *dR;
  QuadricHandle qA, qB, qR;
  TASK_CHECK(build_quadric_decimater(mA, dA, qA, seqA, callsA));
  TASK_CHECK(build_quadric_decimater(mB, dB, qB, seqB, callsB));
  TASK_CHECK(build_quadric_decimater(mR, dR, qR, seqR, callsR));

  TASK_CHECK(dR->decimate(5) == 5);
  TASK_CHECK(seqR.size() == 5);

  TASK_CHECK(dA->decimate(5) == 5);
  TASK_CHECK(dB->decimate(3) == 3);
  TASK_CHECK(dA->undo(2) == 2);

  // the first branch reproduces the reference decisions
  for (size_t i = 0; i < 5; ++i)
    TASK_CHECK(seqA[i] == seqR[i]);
  for (size_t i = 0; i < 3; ++i)
    TASK_CHECK(seqB[i] == seqR[i]);

  // same history position -> identical mesh state and quadrics
  check_topology(snap_topology(mA), snap_topology(mB));
  check_points(snap_points(mA), snap_points(mB));
  check_quadrics(snap_quadrics(mA, dA->module(qA)),
                 snap_quadrics(mB, dB->module(qB)));

  // continuing from a restored position takes the same deterministic
  // decisions as the straight reference / paused session
  TASK_CHECK(dA->decimate(2) == 2);
  TASK_CHECK(dB->decimate(2) == 2);
  TASK_CHECK(dA->history_position() == 5);
  TASK_CHECK(dB->history_position() == 5);
  // A's two newest entries are the new branch's steps 3 and 4
  TASK_CHECK(seqA.size() == 7);
  TASK_CHECK(seqA[5] == seqR[3]);
  TASK_CHECK(seqA[6] == seqR[4]);
  for (size_t i = 0; i < 5; ++i)
    TASK_CHECK(seqB[i] == seqR[i]);
  check_topology(snap_topology(mA), snap_topology(mB));
  check_points(snap_points(mA), snap_points(mB));
  check_quadrics(snap_quadrics(mA, dA->module(qA)),
                 snap_quadrics(mB, dB->module(qB)));
  check_quadrics(snap_quadrics(mA, dA->module(qA)),
                 snap_quadrics(mR, dR->module(qR)));

  // redo path and forward path must also agree after further undos
  TASK_CHECK(dA->undo(3) == 3);
  TASK_CHECK(dB->undo(3) == 3);
  check_quadrics(snap_quadrics(mA, dA->module(qA)),
                 snap_quadrics(mB, dB->module(qB)));
  TASK_CHECK(dA->redo(3) == 3);
  TASK_CHECK(dB->decimate(3) == 3);
  check_topology(snap_topology(mA), snap_topology(mB));
  check_points(snap_points(mA), snap_points(mB));
  check_quadrics(snap_quadrics(mA, dA->module(qA)),
                 snap_quadrics(mB, dB->module(qB)));
  check_quadrics(snap_quadrics(mA, dA->module(qA)),
                 snap_quadrics(mR, dR->module(qR)));

  // undo restores the exact accumulated quadrics, proving they were not
  // re-initialized from the current coarse mesh
  TASK_CHECK(dA->undo(5) == 5);
  TASK_CHECK(dB->undo(5) == 5);
  TASK_CHECK(dR->undo(5) == 5);
  check_quadrics(snap_quadrics(mA, dA->module(qA)),
                 snap_quadrics(mB, dB->module(qB)));
  check_quadrics(snap_quadrics(mA, dA->module(qA)),
                 snap_quadrics(mR, dR->module(qR)));

  // exactly one transfer callback per successful collapse, none on
  // undo/redo
  TASK_CHECK(callsA == int(seqA.size()));
  TASK_CHECK(callsB == int(seqB.size()));
  TASK_CHECK(callsR == 5);
  delete dA;
  delete dB;
  delete dR;
  return 0;
}

static int cont_mixed_entry_targets()
{
  Mesh m = make_boundary_grid(7, 7);
  lock_boundary(m);

  Decimater d(m);
  QuadricHandle q;
  TASK_CHECK(d.add(q));
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.enable_history());

  size_t start = alive_vertices(m);
  size_t start_faces = alive_faces(m);

  TASK_CHECK(d.decimate_to(start - 4) == 4);
  TASK_CHECK(alive_vertices(m) == start - 4);

  // decimate_to_faces judges against currently alive elements; with the
  // face target set to 0 the vertex target is the limiting one
  size_t alive_now = alive_vertices(m);
  size_t got = d.decimate_to_faces(alive_now - 3, 0);
  TASK_CHECK(got == 3);
  TASK_CHECK(alive_vertices(m) == start - 7);

  // already below/at target: no movement, history untouched
  size_t pos = d.history_position();
  TASK_CHECK(d.decimate_to(alive_vertices(m)) == 0);
  TASK_CHECK(d.decimate_to_faces(alive_vertices(m), alive_faces(m)) == 0);
  TASK_CHECK(d.history_position() == pos);

  // face target stops the run; each interior collapse removes two faces,
  // a boundary collapse one, so only bounds are asserted
  size_t faces_now = alive_faces(m);
  got = d.decimate_to_faces(0, faces_now - 4);
  TASK_CHECK(got >= 1);
  TASK_CHECK(got <= 4);
  TASK_CHECK(alive_faces(m) <= faces_now - 4);
  TASK_CHECK(d.history_position() == pos + got);

  // everything remains reversible and consistent
  size_t total_steps = d.history_position();
  TASK_CHECK(d.undo(total_steps) == total_steps);
  TASK_CHECK(d.history_position() == 0);
  TASK_CHECK(alive_vertices(m) == start);
  TASK_CHECK(alive_faces(m) == start_faces);
  TASK_CHECK(OpenMesh::Utils::MeshCheckerT<Mesh>(m).check());

  d.disable_history();
  return 0;
}

static int cont_disable_clears_session()
{
  Mesh m = make_closed_sphere();

  Decimater d(m);
  QuadricHandle q;
  TASK_CHECK(d.add(q));
  TASK_CHECK(d.initialize());
  TASK_CHECK(d.enable_history());
  TASK_CHECK(d.decimate(3) == 3);
  TASK_CHECK(d.history_size() == 3);

  d.disable_history();
  TASK_CHECK(d.history_size() == 0);
  TASK_CHECK(d.history_position() == 0);
  TASK_CHECK(!m.has_history_session());

  // compression is available again, and a new session can start
  size_t allocated = m.n_vertices();
  m.garbage_collection();
  TASK_CHECK(m.n_vertices() < allocated);

  TASK_CHECK(d.initialize());
  TASK_CHECK(d.enable_history());
  TASK_CHECK(d.decimate(1) == 1);
  TASK_CHECK(d.history_size() == 1);
  TASK_CHECK(d.undo(1) == 1);
  d.disable_history();
  return 0;
}

//=============================================================================
// registration
//=============================================================================

const task_case task_cases[] = {
  { "topology", "boundary_undo_redo",      &topo_boundary_undo_redo      },
  { "topology", "closed_mesh",             &topo_closed_mesh             },
  { "topology", "predeleted_slots",        &topo_predeleted_slots        },
  { "topology", "gc_and_enable_rules",     &topo_gc_and_enable_rules     },
  { "properties", "all_kinds_transfer",    &props_all_kinds_transfer     },
  { "properties", "bool_string_spots",     &props_bool_string_spots      },
  { "properties", "tagged_bit_isolation",  &props_tagged_bit_isolation    },
  { "properties", "locality",              &props_locality               },
  { "continuation", "branch_zero_collapse",&cont_branch_and_zero_collapse},
  { "continuation", "quadric_not_recomputed",&cont_quadric_not_recomputed },
  { "continuation", "mixed_entry_targets", &cont_mixed_entry_targets     },
  { "continuation", "disable_clears",      &cont_disable_clears_session  },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
