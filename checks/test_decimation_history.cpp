#include "task_support/check.h"

#include <OpenMesh/Core/Mesh/TriMesh_ArrayKernelT.hh>
#include <OpenMesh/Tools/Decimater/DecimaterT.hh>
#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>
#include <OpenMesh/Tools/Decimater/ModNormalFlippingT.hh>
#include <OpenMesh/Tools/Utils/MeshCheckerT.hh>

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using Mesh = OpenMesh::TriMesh_ArrayKernelT<>;
using Decimater = OpenMesh::Decimater::DecimaterT<Mesh>;
using QuadricModule = OpenMesh::Decimater::ModQuadricT<Mesh>;
using NormalModule = OpenMesh::Decimater::ModNormalFlippingT<Mesh>;
using Collapse = Decimater::CollapseInfo;

struct Counted
{
  int value;
  static int copies;

  explicit Counted(int _value = 0) : value(_value) {}
  Counted(const Counted& _other) : value(_other.value) { ++copies; }
  Counted& operator=(const Counted& _other)
  {
    value = _other.value;
    ++copies;
    return *this;
  }
  bool operator==(const Counted& _other) const { return value == _other.value; }
};

int Counted::copies = 0;

namespace {

struct TopologySnapshot
{
  size_t n_vertices, n_halfedges, n_edges, n_faces;
  std::vector<int> vertex_halfedge;
  std::vector<int> halfedge_vertex, halfedge_face;
  std::vector<int> halfedge_next, halfedge_prev;
  std::vector<int> face_halfedge;
  std::vector<unsigned int> vertex_status, halfedge_status;
  std::vector<unsigned int> edge_status, face_status;
  std::vector<Mesh::Point> points;
  std::vector<Mesh::Normal> vertex_normals, face_normals;

  void capture(Mesh& m)
  {
    n_vertices = m.n_vertices();
    n_halfedges = m.n_halfedges();
    n_edges = m.n_edges();
    n_faces = m.n_faces();

    vertex_halfedge.resize(n_vertices);
    vertex_status.resize(n_vertices);
    points.resize(n_vertices);
    vertex_normals.resize(n_vertices);
    for (size_t i = 0; i < n_vertices; ++i) {
      Mesh::VertexHandle h(static_cast<int>(i));
      vertex_halfedge[i] = m.halfedge_handle(h).idx();
      vertex_status[i] = m.status(h).bits();
      points[i] = m.point(h);
      vertex_normals[i] = m.normal(h);
    }

    halfedge_vertex.resize(n_halfedges);
    halfedge_face.resize(n_halfedges);
    halfedge_next.resize(n_halfedges);
    halfedge_prev.resize(n_halfedges);
    halfedge_status.resize(n_halfedges);
    for (size_t i = 0; i < n_halfedges; ++i) {
      Mesh::HalfedgeHandle h(static_cast<int>(i));
      halfedge_vertex[i] = m.to_vertex_handle(h).idx();
      halfedge_face[i] = m.face_handle(h).idx();
      halfedge_next[i] = m.next_halfedge_handle(h).idx();
      halfedge_prev[i] = m.prev_halfedge_handle(h).idx();
      halfedge_status[i] = m.status(h).bits();
    }

    face_halfedge.resize(n_faces);
    face_status.resize(n_faces);
    face_normals.resize(n_faces);
    for (size_t i = 0; i < n_faces; ++i) {
      Mesh::FaceHandle h(static_cast<int>(i));
      face_halfedge[i] = m.halfedge_handle(h).idx();
      face_status[i] = m.status(h).bits();
      face_normals[i] = m.normal(h);
    }

    edge_status.resize(n_edges);
    for (size_t i = 0; i < n_edges; ++i)
      edge_status[i] = m.status(Mesh::EdgeHandle(static_cast<int>(i))).bits();
  }

  bool same(const TopologySnapshot& o) const
  {
    return n_vertices == o.n_vertices &&
           n_halfedges == o.n_halfedges &&
           n_edges == o.n_edges &&
           n_faces == o.n_faces &&
           vertex_halfedge == o.vertex_halfedge &&
           halfedge_vertex == o.halfedge_vertex &&
           halfedge_face == o.halfedge_face &&
           halfedge_next == o.halfedge_next &&
           halfedge_prev == o.halfedge_prev &&
           face_halfedge == o.face_halfedge &&
           vertex_status == o.vertex_status &&
           halfedge_status == o.halfedge_status &&
           edge_status == o.edge_status &&
           face_status == o.face_status &&
           points == o.points &&
           vertex_normals == o.vertex_normals &&
           face_normals == o.face_normals;
  }

  void diff(const TopologySnapshot& o) const
  {
    std::fprintf(stderr, "topo sizes %zu/%zu %zu/%zu %zu/%zu %zu/%zu\n",
                 n_vertices,o.n_vertices,n_halfedges,o.n_halfedges,
                 n_edges,o.n_edges,n_faces,o.n_faces);
    auto report_ints = [](const char* n, const std::vector<int>& a,
                          const std::vector<int>& b) {
      size_t lim = std::min(a.size(), b.size());
      for (size_t i=0;i<lim;++i) if(a[i]!=b[i]) {
        std::fprintf(stderr,"DIFF %s[%zu] %d != %d\n",n,i,a[i],b[i]); return;
      }
      if(a.size()!=b.size()) std::fprintf(stderr,"DIFF %s size\n",n);
    };
    auto report_bits = [](const char* n, const std::vector<unsigned>& a,
                          const std::vector<unsigned>& b) {
      size_t lim = std::min(a.size(), b.size());
      for (size_t i=0;i<lim;++i) if(a[i]!=b[i]) {
        std::fprintf(stderr,"DIFF %s[%zu] %u != %u\n",n,i,a[i],b[i]); return;
      }
    };
    report_ints("v-halfedge",vertex_halfedge,o.vertex_halfedge);
    report_ints("h-vertex",halfedge_vertex,o.halfedge_vertex);
    report_ints("h-face",halfedge_face,o.halfedge_face);
    report_ints("h-next",halfedge_next,o.halfedge_next);
    report_ints("h-prev",halfedge_prev,o.halfedge_prev);
    report_ints("f-halfedge",face_halfedge,o.face_halfedge);
    report_bits("v-status",vertex_status,o.vertex_status);
    report_bits("h-status",halfedge_status,o.halfedge_status);
    report_bits("e-status",edge_status,o.edge_status);
    report_bits("f-status",face_status,o.face_status);
    for(size_t i=0;i<std::min(points.size(),o.points.size());++i)
      if(points[i]!=o.points[i]) { std::fprintf(stderr,"DIFF point %zu\n",i); break; }
    for(size_t i=0;i<std::min(face_normals.size(),o.face_normals.size());++i)
      if(face_normals[i]!=o.face_normals[i]) { std::fprintf(stderr,"DIFF fnormal %zu\n",i); break; }
  }
};

struct AbstractValues
{
  virtual ~AbstractValues() {}
  virtual bool equals(const AbstractValues&) const = 0;
};

template <class T>
struct Values : AbstractValues
{
  std::vector<T> values;
  bool equals(const AbstractValues& o) const
  {
    const Values<T>* other = dynamic_cast<const Values<T>*>(&o);
    return other != 0 && values == other->values;
  }
};

struct AbstractProperty
{
  std::string name;
  explicit AbstractProperty(const std::string& _name = "") : name(_name) {}
  virtual ~AbstractProperty() {}
  virtual AbstractValues* capture() const = 0;
};

static size_t property_count(Mesh& m, OpenMesh::VertexHandle) { return m.n_vertices(); }
static size_t property_count(Mesh& m, OpenMesh::HalfedgeHandle) { return m.n_halfedges(); }
static size_t property_count(Mesh& m, OpenMesh::EdgeHandle) { return m.n_edges(); }
static size_t property_count(Mesh& m, OpenMesh::FaceHandle) { return m.n_faces(); }

template <class PropertyHandleT, class ElementHandleT, class T>
struct PropertyRecorder : AbstractProperty
{
  Mesh& mesh;
  PropertyHandleT handle;

  PropertyRecorder(Mesh& _mesh, PropertyHandleT _handle,
                   const std::string& _name)
    : AbstractProperty(_name), mesh(_mesh), handle(_handle) {}

  AbstractValues* capture() const
  {
    Values<T>* result = new Values<T>;
    const size_t n = property_count(mesh, ElementHandleT());
    result->values.resize(n);
    for (size_t i = 0; i < n; ++i) {
      result->values[i] =
          mesh.property(handle, ElementHandleT(static_cast<int>(i)));
    }
    return result;
  }
};

struct PropertyBank
{
  Mesh& mesh;
  std::vector<std::unique_ptr<AbstractProperty> > properties;

  OpenMesh::HPropHandleT<OpenMesh::Vec2f> uv_a, uv_b;
  OpenMesh::VPropHandleT<int> v_int;
  OpenMesh::HPropHandleT<double> h_double;
  OpenMesh::EPropHandleT<long long> e_int;
  OpenMesh::FPropHandleT<short> f_int;

  OpenMesh::VPropHandleT<bool> v_bool;
  OpenMesh::HPropHandleT<bool> h_bool;
  OpenMesh::EPropHandleT<bool> e_bool;
  OpenMesh::FPropHandleT<bool> f_bool;

  OpenMesh::VPropHandleT<std::string> v_string;
  OpenMesh::HPropHandleT<std::string> h_string;
  OpenMesh::EPropHandleT<std::string> e_string;
  OpenMesh::FPropHandleT<std::string> f_string;

  OpenMesh::VPropHandleT<Counted> v_counted;
  OpenMesh::HPropHandleT<Counted> h_counted;
  OpenMesh::EPropHandleT<Counted> e_counted;
  OpenMesh::FPropHandleT<Counted> f_counted;

  struct Snapshot
  {
    TopologySnapshot topology;
    std::vector<std::unique_ptr<AbstractValues> > values;

    bool same(const Snapshot& other) const
    {
      if (!topology.same(other.topology) ||
          values.size() != other.values.size())
        return false;
      for (size_t i = 0; i < values.size(); ++i) {
        if (!values[i]->equals(*other.values[i]))
          return false;
      }
      return true;
    }
  };

  explicit PropertyBank(Mesh& _mesh) : mesh(_mesh)
  {
    mesh.request_vertex_status();
    mesh.request_halfedge_status();
    mesh.request_edge_status();
    mesh.request_face_status();
    mesh.request_vertex_normals();
    mesh.request_face_normals();

    add<Mesh::HalfedgeHandle>(uv_a, "hist.uv_a");
    add<Mesh::HalfedgeHandle>(uv_b, "hist.uv_b");
    add<Mesh::VertexHandle>(v_int, "hist.v_int");
    add<Mesh::HalfedgeHandle>(h_double, "hist.h_double");
    add<Mesh::EdgeHandle>(e_int, "hist.e_int");
    add<Mesh::FaceHandle>(f_int, "hist.f_int");
    add<Mesh::VertexHandle>(v_bool, "hist.v_bool");
    add<Mesh::HalfedgeHandle>(h_bool, "hist.h_bool");
    add<Mesh::EdgeHandle>(e_bool, "hist.e_bool");
    add<Mesh::FaceHandle>(f_bool, "hist.f_bool");
    add<Mesh::VertexHandle>(v_string, "hist.v_string");
    add<Mesh::HalfedgeHandle>(h_string, "hist.h_string");
    add<Mesh::EdgeHandle>(e_string, "hist.e_string");
    add<Mesh::FaceHandle>(f_string, "hist.f_string");
    add<Mesh::VertexHandle>(v_counted, "hist.v_counted");
    add<Mesh::HalfedgeHandle>(h_counted, "hist.h_counted");
    add<Mesh::EdgeHandle>(e_counted, "hist.e_counted");
    add<Mesh::FaceHandle>(f_counted, "hist.f_counted");
  }

  template <class ElementHandleT, class PropertyHandleT>
  void add(PropertyHandleT& _handle, const std::string& _name)
  {
    typedef typename PropertyHandleT::Value T;
    mesh.add_property(_handle, _name);
    properties.push_back(std::unique_ptr<AbstractProperty>(
        new PropertyRecorder<PropertyHandleT, ElementHandleT, T>(
            mesh, _handle, _name)));
  }

  void initialize_values()
  {
    for (size_t i = 0; i < mesh.n_vertices(); ++i) {
      Mesh::VertexHandle h(static_cast<int>(i));
      mesh.property(v_int, h) = static_cast<int>(i * 17 + 3);
      mesh.property(v_bool, h) = ((i * 7) & 1U) != 0;
      mesh.property(v_string, h) = "V" + std::to_string(i) + "_locked?";
      mesh.property(v_counted, h).value = static_cast<int>(i * 3 + 1);
    }
    for (size_t i = 0; i < mesh.n_halfedges(); ++i) {
      Mesh::HalfedgeHandle h(static_cast<int>(i));
      mesh.property(uv_a, h) = OpenMesh::Vec2f(0.01f * i, 0.02f * i + 1.0f);
      mesh.property(uv_b, h) = OpenMesh::Vec2f(0.03f * i + 2.0f, 0.04f * i);
      mesh.property(h_double, h) = 0.125 * i + 0.5;
      mesh.property(h_bool, h) = ((i * 11) & 1U) == 0;
      mesh.property(h_string, h) = "H" + std::to_string(i);
      mesh.property(h_counted, h).value = static_cast<int>(i * 5 + 2);
    }
    for (size_t i = 0; i < mesh.n_edges(); ++i) {
      Mesh::EdgeHandle h(static_cast<int>(i));
      mesh.property(e_int, h) = static_cast<long long>(i) * 1000003LL + 7LL;
      mesh.property(e_bool, h) = ((i * 13) & 1U) != 0;
      mesh.property(e_string, h) = "E" + std::to_string(i);
      mesh.property(e_counted, h).value = static_cast<int>(i * 7 + 4);
    }
    for (size_t i = 0; i < mesh.n_faces(); ++i) {
      Mesh::FaceHandle h(static_cast<int>(i));
      mesh.property(f_int, h) = static_cast<short>(i * 19 + 11);
      mesh.property(f_bool, h) = ((i * 17) & 1U) == 0;
      mesh.property(f_string, h) = "F" + std::to_string(i);
      mesh.property(f_counted, h).value = static_cast<int>(i * 11 + 5);
    }
    mesh.update_face_normals();
    mesh.update_vertex_normals();
  }

  Snapshot snapshot() const
  {
    Snapshot result;
    result.topology.capture(mesh);
    for (const auto& property : properties)
      result.values.push_back(
          std::unique_ptr<AbstractValues>(property->capture()));
    return result;
  }
};

void make_grid(Mesh& m, int w, bool lock_boundary)
{
  m.request_vertex_status();
  m.request_edge_status();
  m.request_face_status();
  m.request_vertex_normals();
  m.request_face_normals();
  for (int y = 0; y < w; ++y) {
    for (int x = 0; x < w; ++x) {
      const double xx = static_cast<double>(x) +
                        0.018 * std::sin(12.9898 * x + 4.1414 * y);
      const double yy = static_cast<double>(y) +
                        0.015 * std::cos(7.123 * x + 11.71 * y);
      const double z = 0.013 * xx * xx + 0.021 * yy * yy +
                       0.007 * xx * yy +
                       0.035 * std::sin(1.7 * xx + 2.3 * yy);
      m.add_vertex(Mesh::Point(xx, yy, z));
    }
  }

  for (int y = 0; y < w - 1; ++y) {
    for (int x = 0; x < w - 1; ++x) {
      const int a = y * w + x;
      m.add_face(Mesh::VertexHandle(a),
                 Mesh::VertexHandle(a + 1),
                 Mesh::VertexHandle(a + w + 1));
      m.add_face(Mesh::VertexHandle(a),
                 Mesh::VertexHandle(a + w + 1),
                 Mesh::VertexHandle(a + w));
    }
  }
  if (lock_boundary) {
    for (Mesh::VertexIter it = m.vertices_begin();
         it != m.vertices_end(); ++it) {
      if (m.is_boundary(*it))
        m.status(*it).set_locked(true);
    }
  }
  m.update_face_normals();
  m.update_vertex_normals();
}

void make_closed_mesh(Mesh& m, int subdivisions)
{
  m.request_vertex_normals();
  m.request_face_normals();

  std::vector<std::array<double, 3> > p = {
    {{ 1, 0, 0 }}, {{ -1, 0, 0 }},
    {{ 0, 1, 0 }}, {{ 0, -1, 0 }},
    {{ 0, 0, 1 }}, {{ 0, 0, -1 }}
  };
  std::vector<std::array<int, 3> > t = {
    {{0, 2, 4}}, {{2, 1, 4}}, {{1, 3, 4}}, {{3, 0, 4}},
    {{2, 0, 5}}, {{1, 2, 5}}, {{3, 1, 5}}, {{0, 3, 5}}
  };

  for (int level = 0; level < subdivisions; ++level) {
    std::map<std::pair<int, int>, int> middle;
    std::vector<std::array<int, 3> > next;

    auto midpoint = [&](int a, int b) -> int
    {
      const int lo = std::min(a, b);
      const int hi = std::max(a, b);
      const std::pair<int, int> key(lo, hi);
      std::map<std::pair<int, int>, int>::iterator found = middle.find(key);
      if (found != middle.end())
        return found->second;

      std::array<double, 3> q = {{
        0.5 * (p[a][0] + p[b][0]),
        0.5 * (p[a][1] + p[b][1]),
        0.5 * (p[a][2] + p[b][2])
      }};
      const double len = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2]);
      for (int k = 0; k < 3; ++k) q[k] /= len;
      const double bump =
          1.0 + 0.07 * std::sin(13.1 * q[0] + 7.7 * q[1] + 5.3 * q[2]);
      for (int k = 0; k < 3; ++k) q[k] *= bump;

      const int id = static_cast<int>(p.size());
      p.push_back(q);
      middle[key] = id;
      return id;
    };

    for (const auto& tri : t) {
      const int a = tri[0], b = tri[1], c = tri[2];
      const int ab = midpoint(a, b);
      const int bc = midpoint(b, c);
      const int ca = midpoint(c, a);
      next.push_back({{a, ab, ca}});
      next.push_back({{b, bc, ab}});
      next.push_back({{c, ca, bc}});
      next.push_back({{ab, bc, ca}});
    }
    t.swap(next);
  }

  for (const auto& q : p)
    m.add_vertex(Mesh::Point(q[0], q[1], q[2]));
  for (const auto& tri : t) {
    OpenMesh::FaceHandle fh = m.add_face(
        Mesh::VertexHandle(tri[0]),
        Mesh::VertexHandle(tri[1]),
        Mesh::VertexHandle(tri[2]));
    (void)fh;
  }
  m.update_face_normals();
  m.update_vertex_normals();
}

void add_locked_triangles(Mesh& m, int count)
{
  for (int k = 0; k < count; ++k) {
    const int base = static_cast<int>(m.n_vertices());
    const double x = 50.0 + 5.0 * k;
    m.add_vertex(Mesh::Point(x, 100.0, 0.0));
    m.add_vertex(Mesh::Point(x + 1.0, 100.0, 0.2));
    m.add_vertex(Mesh::Point(x + 0.5, 101.0, 0.1));
    Mesh::FaceHandle f = m.add_face(Mesh::VertexHandle(base),
                                    Mesh::VertexHandle(base + 1),
                                    Mesh::VertexHandle(base + 2));
    if (f.is_valid()) {
      for (int i = 0; i < 3; ++i) {
        Mesh::VertexHandle vh(base + i);
        m.status(vh).set_locked(true);
      }
      for (Mesh::FaceEdgeIter eit(m, f); eit.is_valid(); ++eit)
        m.status(*eit).set_locked(true);
    }
  }
}

void add_predeleted_component(Mesh& m)
{
  const int base = static_cast<int>(m.n_vertices());
  m.add_vertex(Mesh::Point(-50, -50, 0));
  m.add_vertex(Mesh::Point(-49, -50, 0));
  m.add_vertex(Mesh::Point(-49.5, -49, 0));
  Mesh::FaceHandle f = m.add_face(Mesh::VertexHandle(base),
                                  Mesh::VertexHandle(base + 1),
                                  Mesh::VertexHandle(base + 2));
  if (f.is_valid())
    m.delete_face(f, true);
}

size_t live_vertices(const Mesh& m)
{
  size_t n = 0;
  for (Mesh::ConstVertexIter it = m.vertices_begin();
       it != m.vertices_end(); ++it) {
    if (!m.status(*it).deleted()) ++n;
  }
  return n;
}

size_t live_faces(const Mesh& m)
{
  size_t n = 0;
  for (Mesh::ConstFaceIter it = m.faces_begin();
       it != m.faces_end(); ++it) {
    if (!m.status(*it).deleted()) ++n;
  }
  return n;
}

bool checker_ok(Mesh& m)
{
  return OpenMesh::Utils::MeshCheckerT<Mesh>(m).check();
}

bool bool_gc_throws(Mesh& m)
{
  try { m.garbage_collection(); }
  catch (const std::logic_error&) { return true; }
  catch (...) { return false; }
  return false;
}

bool tracked_gc_throws(Mesh& m)
{
  std::vector<Mesh::VertexHandle*> vh;
  std::vector<Mesh::HalfedgeHandle*> hh;
  std::vector<Mesh::FaceHandle*> fh;
  try { m.garbage_collection(vh, hh, fh); }
  catch (const std::logic_error&) { return true; }
  catch (...) { return false; }
  return false;
}

template <class MeshT>
class BogusModuleT : public OpenMesh::Decimater::ModBaseT<MeshT>
{
public:
  DECIMATING_MODULE(BogusModuleT, MeshT, Bogus)
  explicit BogusModuleT(MeshT& _mesh) : Base(_mesh, true) {}
};

} // namespace

//=============================================================================
// topology
//=============================================================================

static int topology_enable_and_empty_history(void)
{
  Mesh mesh;
  make_grid(mesh, 5, true);
  PropertyBank bank(mesh);
  bank.initialize_values();

  Decimater dec(mesh);
  QuadricModule::Handle quadric;
  TASK_CHECK(!dec.enable_history()); // not initialized yet
  TASK_CHECK(dec.add(quadric));
  TASK_CHECK(dec.initialize());
  TASK_CHECK(dec.enable_history());
  TASK_CHECK(!dec.enable_history());
  TASK_CHECK(dec.history_size() == 0);
  TASK_CHECK(dec.history_position() == 0);
  TASK_CHECK(dec.history_stored_elements() == 0);
  TASK_CHECK(dec.undo(3) == 0);
  TASK_CHECK(dec.redo(3) == 0);
  TASK_CHECK(dec.undo(0) == 0);
  TASK_CHECK(dec.redo(0) == 0);
  TASK_CHECK(bool_gc_throws(mesh));
  TASK_CHECK(tracked_gc_throws(mesh));

  dec.disable_history();
  TASK_CHECK(dec.history_size() == 0);
  mesh.garbage_collection();
  TASK_CHECK(checker_ok(mesh));
  return 0;
}

static int topology_stepwise_undo_redo(void)
{
  Mesh mesh;
  make_grid(mesh, 7, true);
  PropertyBank bank(mesh);
  bank.initialize_values();

  Decimater dec(mesh);
  QuadricModule::Handle q;
  TASK_CHECK(dec.add(q));
  TASK_CHECK(dec.initialize());
  TASK_CHECK(dec.enable_history());

  std::vector<PropertyBank::Snapshot> states;
  states.push_back(bank.snapshot());

  const int steps = 10;
  for (int i = 0; i < steps; ++i) {
    TASK_CHECK(dec.decimate(1) == 1);
    TASK_CHECK(dec.history_size() == static_cast<size_t>(i + 1));
    TASK_CHECK(dec.history_position() == static_cast<size_t>(i + 1));
    states.push_back(bank.snapshot());
  }
  TASK_CHECK(dec.history_stored_elements() > 0);
  TASK_CHECK(checker_ok(mesh));

  for (int i = steps; i > 0; --i) {
    TASK_CHECK(dec.undo(1) == 1);
    TASK_CHECK(dec.history_position() == static_cast<size_t>(i - 1));
    PropertyBank::Snapshot now = bank.snapshot();
    const bool same_state = now.same(states[i - 1]);
    if (!same_state) {
      now.topology.diff(states[i - 1].topology);
      for (size_t p = 0; p < bank.properties.size(); ++p) {
        if (!now.values[p]->equals(*states[i - 1].values[p]))
          std::fprintf(stderr, "DIFF property %s\n",
                       bank.properties[p]->name.c_str());
      }
    }
    TASK_CHECK(same_state);
    TASK_CHECK(checker_ok(mesh));
  }

  TASK_CHECK(dec.undo(1) == 0);
  TASK_CHECK(dec.undo(10) == 0);
  TASK_CHECK(dec.history_position() == 0);

  for (int i = 1; i <= steps; ++i) {
    TASK_CHECK(dec.redo(1) == 1);
    PropertyBank::Snapshot now = bank.snapshot();
    TASK_CHECK(now.same(states[i]));
  }
  TASK_CHECK(dec.redo(1) == 0);

  TASK_CHECK(dec.undo(2) == 2);
  TASK_CHECK(dec.history_position() == static_cast<size_t>(steps - 2));
  TASK_CHECK(dec.undo(100) == static_cast<size_t>(steps - 2));
  TASK_CHECK(dec.history_position() == 0);
  TASK_CHECK(dec.redo(3) == 3);
  TASK_CHECK(dec.history_position() == 3);
  TASK_CHECK(dec.redo(100) == static_cast<size_t>(steps - 3));
  TASK_CHECK(dec.history_position() == static_cast<size_t>(steps));

  TASK_CHECK(dec.undo(0) == 0);
  TASK_CHECK(dec.redo(0) == 0);
  return 0;
}

static int topology_boundary_closed_and_old_slots(void)
{
  // Boundary grid, with an already deleted disconnected component.
  Mesh boundary;
  make_grid(boundary, 7, false);
  add_predeleted_component(boundary);
  PropertyBank boundary_bank(boundary);
  boundary_bank.initialize_values();
  PropertyBank::Snapshot original = boundary_bank.snapshot();

  Decimater bd(boundary);
  QuadricModule::Handle bq;
  TASK_CHECK(bd.add(bq));
  TASK_CHECK(bd.initialize());
  TASK_CHECK(bd.enable_history());
  TASK_CHECK(bd.decimate(5) == 5);
  TASK_CHECK(checker_ok(boundary));
  TASK_CHECK(bd.undo(5) == 5);
  TASK_CHECK(boundary_bank.snapshot().same(original));
  TASK_CHECK(checker_ok(boundary));

  // Closed mesh with quadric plus normal-flipping module.
  Mesh closed;
  make_closed_mesh(closed, 2);
  PropertyBank closed_bank(closed);
  closed_bank.initialize_values();

  Decimater cd(closed);
  QuadricModule::Handle cq;
  NormalModule::Handle cn;
  TASK_CHECK(cd.add(cq));
  TASK_CHECK(cd.add(cn));
  TASK_CHECK(cd.initialize());
  TASK_CHECK(cd.enable_history());

  PropertyBank::Snapshot c0 = closed_bank.snapshot();
  TASK_CHECK(cd.decimate(4) == 4);
  PropertyBank::Snapshot c4 = closed_bank.snapshot();
  TASK_CHECK(checker_ok(closed));
  TASK_CHECK(cd.undo(4) == 4);
  TASK_CHECK(closed_bank.snapshot().same(c0));
  TASK_CHECK(checker_ok(closed));
  TASK_CHECK(cd.redo(4) == 4);
  TASK_CHECK(closed_bank.snapshot().same(c4));
  TASK_CHECK(checker_ok(closed));
  return 0;
}

static int topology_gc_after_disable_and_destructor(void)
{
  Mesh mesh;
  make_grid(mesh, 6, true);
  add_predeleted_component(mesh);
  PropertyBank bank(mesh);

  {
    Decimater dec(mesh);
    QuadricModule::Handle q;
    TASK_CHECK(dec.add(q));
    TASK_CHECK(dec.initialize());
    TASK_CHECK(dec.enable_history());
    TASK_CHECK(bool_gc_throws(mesh));
    TASK_CHECK(tracked_gc_throws(mesh));
    TASK_CHECK(dec.decimate(2) == 2);
    TASK_CHECK(bool_gc_throws(mesh));
    TASK_CHECK(tracked_gc_throws(mesh));
    dec.disable_history();
  }

  // Destructor and explicit disable both release the mesh session.
  TASK_CHECK(!mesh.decimation_history_active());
  const size_t before = mesh.n_vertices();
  mesh.garbage_collection();
  TASK_CHECK(mesh.n_vertices() < before);
  TASK_CHECK(checker_ok(mesh));

  std::vector<Mesh::VertexHandle*> vh;
  std::vector<Mesh::HalfedgeHandle*> hh;
  std::vector<Mesh::FaceHandle*> fh;
  mesh.garbage_collection(vh, hh, fh);
  return 0;
}

static int topology_enable_rejects_other_modules(void)
{
  Mesh mesh;
  make_grid(mesh, 5, true);
  PropertyBank bank(mesh);

  Decimater dec(mesh);
  QuadricModule::Handle q;
  BogusModuleT<Mesh>::Handle bogus;
  TASK_CHECK(dec.add(q));
  TASK_CHECK(dec.add(bogus));
  TASK_CHECK(dec.initialize());
  TASK_CHECK(!dec.enable_history());
  TASK_CHECK(dec.is_initialized());
  TASK_CHECK(dec.history_size() == 0);
  TASK_CHECK(!mesh.decimation_history_active());

  // A binary quadric is outside the supported non-binary quadric path.
  Mesh mesh2;
  make_grid(mesh2, 5, true);
  Decimater dec2(mesh2);
  QuadricModule::Handle q2;
  TASK_CHECK(dec2.add(q2));
  TASK_CHECK(dec2.initialize());
  dec2.module(q2).set_max_err(0.0, true);
  TASK_CHECK(!dec2.enable_history());
  TASK_CHECK(!mesh2.decimation_history_active());
  return 0;
}

//=============================================================================
// properties
//=============================================================================

static int properties_transfer_and_all_types(void)
{
  Mesh mesh;
  make_grid(mesh, 7, true);
  PropertyBank bank(mesh);
  bank.initialize_values();

  Decimater dec(mesh);
  QuadricModule::Handle q;
  TASK_CHECK(dec.add(q));
  TASK_CHECK(dec.initialize());

  int calls = 0;
  Mesh::VertexHandle last_retained;
  dec.set_history_transfer([&](const Collapse& ci)
  {
    ++calls;
    last_retained = ci.v1;

    // Ordinary V/H/E/F properties only. Topology/status/points are left to
    // the decimater/history machinery.
    mesh.property(bank.v_int, ci.v1) +=
        mesh.property(bank.v_int, ci.v0) + calls;
    mesh.property(bank.v_bool, ci.v1) = true;
    mesh.property(bank.v_string, ci.v1) = "transferred:" +
                                         std::to_string(ci.v1.idx());
    mesh.property(bank.v_counted, ci.v1).value += 100 + calls;

    auto write_face = [&](Mesh::FaceHandle fh)
    {
      if (!fh.is_valid()) return;
      mesh.property(bank.f_int, fh) = static_cast<short>(700 + calls);
      mesh.property(bank.f_bool, fh) = calls % 2 == 0;
      mesh.property(bank.f_string, fh) = "transfer-F-" + std::to_string(calls);
      mesh.property(bank.f_counted, fh).value += calls;

      for (Mesh::FaceEdgeIter eit(mesh, fh); eit.is_valid(); ++eit) {
        Mesh::EdgeHandle eh = *eit;
        mesh.property(bank.e_int, eh) += calls * 3;
        mesh.property(bank.e_bool, eh) = true;
        mesh.property(bank.e_string, eh) = "transfer-E-" + std::to_string(calls);
        mesh.property(bank.e_counted, eh).value += calls;
      }

      for (Mesh::FaceHalfedgeIter hit(mesh, fh); hit.is_valid(); ++hit) {
        Mesh::HalfedgeHandle hh = *hit;
        mesh.property(bank.uv_a, hh) =
            OpenMesh::Vec2f(0.001f * calls, 0.002f * hh.idx());
        mesh.property(bank.uv_b, hh) =
            OpenMesh::Vec2f(0.003f * hh.idx(), 0.004f * calls);
        mesh.property(bank.h_double, hh) += calls + 0.25;
        mesh.property(bank.h_bool, hh) = calls % 3 != 0;
        mesh.property(bank.h_string, hh) =
            "transfer-H-" + std::to_string(hh.idx()) + "-" +
            std::to_string(calls);
        mesh.property(bank.h_counted, hh).value += calls;
      }

      for (Mesh::FaceVertexIter vit(mesh, fh); vit.is_valid(); ++vit)
        mesh.property(bank.v_counted, *vit).value += 1;
    };

    write_face(ci.fl);
    write_face(ci.fr);
    for (Mesh::VertexFaceIter vfit(mesh, ci.v1); vfit.is_valid(); ++vfit)
      write_face(*vfit);
  });

  TASK_CHECK(dec.enable_history());
  std::vector<PropertyBank::Snapshot> states;
  states.push_back(bank.snapshot());

  const int steps = 8;
  for (int i = 0; i < steps; ++i) {
    TASK_CHECK(dec.decimate(1) == 1);
    TASK_CHECK(calls == i + 1);
    states.push_back(bank.snapshot());
  }
  TASK_CHECK(calls == steps);
  TASK_CHECK(last_retained.is_valid());
  TASK_CHECK(mesh.property(bank.v_string, last_retained)
              .find("transferred:") == 0);

  const int calls_after_decimation = calls;
  for (int i = steps; i > 0; --i) {
    TASK_CHECK(dec.undo(1) == 1);
    TASK_CHECK(calls == calls_after_decimation);
    TASK_CHECK(bank.snapshot().same(states[i - 1]));
  }
  TASK_CHECK(dec.undo(steps) == 0);
  TASK_CHECK(calls == calls_after_decimation);

  for (int i = 1; i <= steps; ++i) {
    TASK_CHECK(dec.redo(1) == 1);
    TASK_CHECK(calls == calls_after_decimation);
    TASK_CHECK(bank.snapshot().same(states[i]));
  }
  TASK_CHECK(calls == calls_after_decimation);
  return 0;
}

static int properties_local_stored_volume_and_copies(void)
{
  Mesh small_mesh;
  make_grid(small_mesh, 5, true);
  PropertyBank small_bank(small_mesh);

  Mesh large_mesh;
  make_grid(large_mesh, 5, true);
  add_locked_triangles(large_mesh, 120);
  PropertyBank large_bank(large_mesh);

  size_t small_stored = 0;
  size_t large_stored = 0;
  int small_copies = 0;
  int large_copies = 0;

  {
    Decimater dec(small_mesh);
    QuadricModule::Handle q;
    TASK_CHECK(dec.add(q));
    TASK_CHECK(dec.initialize());
    TASK_CHECK(dec.enable_history());
    Counted::copies = 0;
    TASK_CHECK(dec.decimate(1) == 1);
    small_copies = Counted::copies;
    small_stored = dec.history_stored_elements();
    TASK_CHECK(small_stored > 0);
  }

  {
    Decimater dec(large_mesh);
    QuadricModule::Handle q;
    TASK_CHECK(dec.add(q));
    TASK_CHECK(dec.initialize());
    TASK_CHECK(dec.enable_history());
    Counted::copies = 0;
    TASK_CHECK(dec.decimate(1) == 1);
    large_copies = Counted::copies;
    large_stored = dec.history_stored_elements();
    TASK_CHECK(large_stored > 0);
  }

  TASK_CHECK(small_stored == large_stored);
  TASK_CHECK(small_copies == large_copies);
  TASK_CHECK(small_copies > 0);
  return 0;
}

static int properties_tagged_bits_are_restored(void)
{
  Mesh mesh;
  make_grid(mesh, 7, true);
  PropertyBank bank(mesh);

  for (Mesh::VertexIter it = mesh.vertices_begin();
       it != mesh.vertices_end(); ++it)
    mesh.status(*it).set_tagged(true);

  Decimater dec(mesh);
  QuadricModule::Handle q;
  TASK_CHECK(dec.add(q));
  TASK_CHECK(dec.initialize());
  TASK_CHECK(dec.enable_history());
  TASK_CHECK(dec.decimate(8) > 0);

  for (Mesh::VertexIter it = mesh.vertices_begin();
       it != mesh.vertices_end(); ++it)
    TASK_CHECK(mesh.status(*it).tagged());

  TASK_CHECK(dec.undo(8) > 0);
  for (Mesh::VertexIter it = mesh.vertices_begin();
       it != mesh.vertices_end(); ++it)
    TASK_CHECK(mesh.status(*it).tagged());
  return 0;
}

//=============================================================================
// continuation
//=============================================================================

static int continuation_new_branch_and_zero_collapse(void)
{
  Mesh mesh;
  make_grid(mesh, 8, true);
  PropertyBank bank(mesh);
  bank.initialize_values();

  Decimater dec(mesh);
  QuadricModule::Handle q;
  TASK_CHECK(dec.add(q));
  TASK_CHECK(dec.initialize());
  TASK_CHECK(dec.enable_history());
  TASK_CHECK(dec.decimate(3) == 3);
  TASK_CHECK(dec.history_size() == 3);

  TASK_CHECK(dec.undo(1) == 1);
  TASK_CHECK(dec.history_position() == 2);
  TASK_CHECK(dec.history_size() == 3);

  // A no-success call must leave the future alone.
  const size_t live = live_vertices(mesh);
  TASK_CHECK(dec.decimate_to(live) == 0);
  TASK_CHECK(dec.history_size() == 3);
  TASK_CHECK(dec.history_position() == 2);
  TASK_CHECK(dec.redo(1) == 1);
  TASK_CHECK(dec.history_position() == 3);

  // Undo again, then a real collapse creates the replacement branch.
  TASK_CHECK(dec.undo(1) == 1);
  TASK_CHECK(dec.decimate(2) == 2);
  TASK_CHECK(dec.history_position() == 4);
  TASK_CHECK(dec.history_size() == 4);
  TASK_CHECK(dec.redo(10) == 0);

  // Undo one and verify another no-op target call retains that future.
  TASK_CHECK(dec.undo(1) == 1);
  const size_t current_live = live_vertices(mesh);
  const size_t current_live_faces = live_faces(mesh);
  TASK_CHECK(dec.decimate_to(current_live) == 0);
  TASK_CHECK(dec.decimate_to_faces(current_live, current_live_faces) == 0);
  TASK_CHECK(dec.history_size() == 4);
  TASK_CHECK(dec.history_position() == 3);
  TASK_CHECK(dec.redo(1) == 1);
  TASK_CHECK(dec.history_position() == 4);
  TASK_CHECK(checker_ok(mesh));
  return 0;
}

static int continuation_quadric_history_matches_fresh_session(void)
{
  Mesh ma, mb;
  make_grid(ma, 8, true);
  make_grid(mb, 8, true);
  PropertyBank ba(ma), bb(mb);
  ba.initialize_values();
  bb.initialize_values();

  std::vector<std::pair<int, int> > sa, sb;
  int calls_a = 0, calls_b = 0;

  Decimater da(ma), db(mb);
  QuadricModule::Handle qa, qb;
  TASK_CHECK(da.add(qa));
  TASK_CHECK(db.add(qb));
  TASK_CHECK(da.initialize());
  TASK_CHECK(db.initialize());

  da.set_history_transfer([&](const Collapse& ci)
  {
    ++calls_a;
    sa.push_back({ci.v0.idx(), ci.v1.idx()});
    ma.property(ba.v_int, ci.v1) += ma.property(ba.v_int, ci.v0) + 1;
    ma.property(ba.v_string, ci.v1) =
        "Q" + std::to_string(ci.v1.idx()) + "." +
        std::to_string(ma.property(ba.v_int, ci.v1));
    for (Mesh::VertexFaceIter vfit(ma, ci.v1); vfit.is_valid(); ++vfit) {
      for (Mesh::FaceHalfedgeIter hit(ma, *vfit); hit.is_valid(); ++hit) {
        ma.property(ba.uv_a, *hit) =
            OpenMesh::Vec2f(0.1f * ci.v1.idx(), 0.2f * hit->idx());
      }
    }
  });
  db.set_history_transfer([&](const Collapse& ci)
  {
    ++calls_b;
    sb.push_back({ci.v0.idx(), ci.v1.idx()});
    mb.property(bb.v_int, ci.v1) += mb.property(bb.v_int, ci.v0) + 1;
    mb.property(bb.v_string, ci.v1) =
        "Q" + std::to_string(ci.v1.idx()) + "." +
        std::to_string(mb.property(bb.v_int, ci.v1));
    for (Mesh::VertexFaceIter vfit(mb, ci.v1); vfit.is_valid(); ++vfit) {
      for (Mesh::FaceHalfedgeIter hit(mb, *vfit); hit.is_valid(); ++hit) {
        mb.property(bb.uv_a, *hit) =
            OpenMesh::Vec2f(0.1f * ci.v1.idx(), 0.2f * hit->idx());
      }
    }
  });

  TASK_CHECK(da.enable_history());
  TASK_CHECK(db.enable_history());

  TASK_CHECK(da.decimate(4) == 4);
  TASK_CHECK(db.decimate(2) == 2);
  TASK_CHECK(da.undo(2) == 2);

  TASK_CHECK(sa.size() == 4);
  TASK_CHECK(sb.size() == 2);
  for (size_t i = 0; i < 2; ++i)
    TASK_CHECK(sa[i] == sb[i]);

  const size_t suffix_a = sa.size();
  const size_t suffix_b = sb.size();
  TASK_CHECK(da.decimate(3) == 3);
  TASK_CHECK(db.decimate(3) == 3);
  TASK_CHECK(sa.size() == suffix_a + 3);
  TASK_CHECK(sb.size() == suffix_b + 3);

  for (size_t i = 0; i < 3; ++i) {
    TASK_CHECK(sa[suffix_a + i] == sb[suffix_b + i]);
  }

  PropertyBank::Snapshot snap_a = ba.snapshot();
  PropertyBank::Snapshot snap_b = bb.snapshot();
  TASK_CHECK(snap_a.same(snap_b));
  TASK_CHECK(checker_ok(ma));
  TASK_CHECK(checker_ok(mb));
  return 0;
}

static int continuation_mixed_live_targets(void)
{
  Mesh mesh;
  make_grid(mesh, 8, true);
  PropertyBank bank(mesh);

  Decimater dec(mesh);
  QuadricModule::Handle q;
  TASK_CHECK(dec.add(q));
  TASK_CHECK(dec.initialize());
  TASK_CHECK(dec.enable_history());

  TASK_CHECK(dec.decimate(3) == 3);
  const size_t allocated_v = mesh.n_vertices();
  size_t live_v = live_vertices(mesh);
  size_t live_f = live_faces(mesh);
  TASK_CHECK(allocated_v > live_v);

  // decimate_to must use live slots, not n_vertices()/allocated slots.
  TASK_CHECK(dec.decimate_to(live_v) == 0);
  TASK_CHECK(live_vertices(mesh) == live_v);
  TASK_CHECK(dec.decimate_to_faces(live_v, live_f) == 0);

  const size_t before_to_v = live_v;
  TASK_CHECK(dec.decimate_to(before_to_v - 2) == 2);
  live_v = live_vertices(mesh);
  TASK_CHECK(live_v == before_to_v - 2);

  live_f = live_faces(mesh);
  const size_t target_f = live_f > 4 ? live_f - 4 : 0;
  const size_t before_f = live_f;
  const size_t done = dec.decimate_to_faces(0, target_f);
  TASK_CHECK(done > 0);
  TASK_CHECK(live_faces(mesh) <= target_f);
  TASK_CHECK(live_faces(mesh) < before_f);

  // Mix the vertex entry again and ensure it still counts live elements.
  live_v = live_vertices(mesh);
  if (live_v > 4) {
    const size_t moves = dec.decimate_to(live_v - 2);
    TASK_CHECK(moves == 2);
    TASK_CHECK(live_vertices(mesh) == live_v - 2);
  }

  TASK_CHECK(checker_ok(mesh));
  TASK_CHECK(dec.history_size() == dec.history_position());
  const size_t history_count = dec.history_size();
  TASK_CHECK(dec.undo(history_count) == history_count);
  TASK_CHECK(dec.history_position() == 0);
  TASK_CHECK(checker_ok(mesh));
  return 0;
}

//=============================================================================

const struct task_case task_cases[] = {
  { "topology", "enable_and_empty_history", topology_enable_and_empty_history },
  { "topology", "stepwise_undo_redo", topology_stepwise_undo_redo },
  { "topology", "boundary_closed_and_old_slots", topology_boundary_closed_and_old_slots },
  { "topology", "gc_lifecycle", topology_gc_after_disable_and_destructor },
  { "topology", "module_validation", topology_enable_rejects_other_modules },

  { "properties", "transfer_and_all_types", properties_transfer_and_all_types },
  { "properties", "local_stored_volume", properties_local_stored_volume_and_copies },
  { "properties", "tagged_bits", properties_tagged_bits_are_restored },

  { "continuation", "new_branch", continuation_new_branch_and_zero_collapse },
  { "continuation", "quadric_session_match", continuation_quadric_history_matches_fresh_session },
  { "continuation", "mixed_live_targets", continuation_mixed_live_targets }
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
