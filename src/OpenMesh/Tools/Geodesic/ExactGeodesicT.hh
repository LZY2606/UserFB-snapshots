/*===========================================================================*\
 *                                                                           *
 *                               OpenMesh                                    *
 *      Copyright (C) 2001-2013 by Computer Graphics Group, RWTH Aachen      *
 *                           www.openmesh.org                                *
 *                                                                           *
 *---------------------------------------------------------------------------*
 *  This file is part of OpenMesh.                                           *
 *                                                                           *
 *  OpenMesh is free software: you can redistribute it and/or modify         *
 *  it under the terms of the GNU Lesser General Public License as           *
 *  published by the Free Software Foundation, either version 3 of           *
 *  the License, or (at your option) any later version with the              *
 *  following exceptions:                                                    *
 *                                                                           *
 *  If other files instantiate templates or use macros                       *
 *  or inline functions from this file, or you compile this file and         *
 *  link it with other files to produce an executable, this file does        *
 *  not by itself cause the resulting executable to be covered by the        *
 *  GNU Lesser General Public License. This exception does not however       *
 *  invalidate any other reasons why the executable file might be            *
 *  covered by the GNU Lesser General Public License.                        *
 *                                                                           *
\*===========================================================================*/

//=============================================================================
//
//  Exact polyhedral geodesic distances and shortest paths
//
//  Mitchell-Mount-Papadimitriou (MMP) window propagation.  Distance
//  functions travel along mesh edges as continuous intervals ("windows")
//
//      d(t) = d_ps + sqrt((t - td)^2 + sigma^2),   t in [b0, b1],
//
//  i.e. distances to a pseudosource unfolded into the edge plane plus the
//  already known source-to-pseudosource length.  Competing windows are
//  reduced to their pointwise lower envelope before being propagated
//  through the adjacent triangle.  Saddle vertices and boundary vertices
//  generate pseudosource (vertex) events; convex interior vertices never
//  do, so no illegal shortcuts are introduced.
//
//=============================================================================

#ifndef OPENMESH_GEODESIC_EXACT_GEODESIC_T_HH
#define OPENMESH_GEODESIC_EXACT_GEODESIC_T_HH

#include <OpenMesh/Core/System/config.h>
#include <OpenMesh/Core/Mesh/Handles.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

namespace OpenMesh {
namespace Geodesic {

/** \brief Exact (continuous) geodesic distances and shortest paths on a
 *         triangular polyhedral surface.
 *
 *  The solver takes an immutable snapshot of the mesh geometry and
 *  connectivity at construction time.  All computations run on that
 *  snapshot, so the original mesh can be modified or destroyed afterwards
 *  without affecting an already constructed solver.
 *
 *  Supported inputs: oriented 2-manifold triangle meshes with boundaries,
 *  holes and non-convex folds; self-intersecting embeddings are allowed.
 *  Isolated vertices, zero-area faces, non-finite coordinates and
 *  non-manifold vertices are rejected.
 */
template <class Mesh>
class ExactGeodesicT
{
public:

  //----- public nested types -----------------------------------------------

  /// A point on the surface, given by a face and barycentric coordinates.
  struct SurfacePoint
  {
    typename Mesh::FaceHandle      face;
    std::array<double, 3>          bary;
  };

  //----- construction ------------------------------------------------------

  /** \brief Take a snapshot of the mesh and validate it.
   *  \throw std::invalid_argument if the mesh contains unsupported input.
   */
  explicit ExactGeodesicT(const Mesh& _mesh);

  ExactGeodesicT(const ExactGeodesicT&)            = delete;
  ExactGeodesicT& operator=(const ExactGeodesicT&) = delete;

  //----- single source queries ---------------------------------------------

  /** \brief Compute the geodesic distance field for one source vertex.
   *
   *  A failed call leaves any previously computed solution untouched.
   *  \return false if the source handle is not a live vertex of the mesh.
   */
  bool solve(typename Mesh::VertexHandle _source);

  /** \brief Global shortest geodesic distance of a vertex.
   *  \return positive infinity for vertices in disconnected components.
   *  \throw std::logic_error      if solve() has not been called successfully.
   *  \throw std::invalid_argument if the handle is not a live vertex.
   */
  double distance(typename Mesh::VertexHandle _v) const;

  /** \brief Trace a shortest path from a surface point back to the source.
   *
   *  \param _f     face containing the target point.
   *  \param _bary  barycentric coordinates, ordered like the face's
   *                cfv_iter at construction time.
   *  \param _out   receives target-to-source surface points; points
   *                crossing an edge are recorded once on each side.
   *  \param _length receives the accumulated in-face path length.
   *  \return false if the target lies in an unreachable component; in that
   *          case neither \p _out nor \p _length are modified.
   *  \throw std::logic_error      if solve() has not been called successfully.
   *  \throw std::invalid_argument if the handle or coordinates are invalid.
   */
  bool trace(typename Mesh::FaceHandle _f,
             const std::array<double, 3>& _bary,
             std::vector<SurfacePoint>& _out,
             double& _length) const;

  //----- snapshot introspection (handles of the original mesh) -------------

  std::size_t n_snapshot_vertices() const { return vertices_.size(); }
  std::size_t n_snapshot_faces()    const { return faces_.size(); }

private:

  //================ tiny linear algebra =================

  struct V3
  {
    double x, y, z;
    V3() : x(0.0), y(0.0), z(0.0) {}
    V3(double _x, double _y, double _z) : x(_x), y(_y), z(_z) {}
    V3 operator+(const V3& o) const { return V3(x+o.x, y+o.y, z+o.z); }
    V3 operator-(const V3& o) const { return V3(x-o.x, y-o.y, z-o.z); }
    V3 operator*(double s) const    { return V3(x*s, y*s, z*s); }
  };

  struct V2
  {
    double x, y;
    V2() : x(0.0), y(0.0) {}
    V2(double _x, double _y) : x(_x), y(_y) {}
    V2 operator+(const V2& o) const { return V2(x+o.x, y+o.y); }
    V2 operator-(const V2& o) const { return V2(x-o.x, y-o.y); }
    V2 operator*(double s) const    { return V2(x*s, y*s); }
  };

  static double dot3(const V3& a, const V3& b)
  { return a.x*b.x + a.y*b.y + a.z*b.z; }
  static double norm3(const V3& a) { return std::sqrt(dot3(a, a)); }
  static double dot2(const V2& a, const V2& b)
  { return a.x*b.x + a.y*b.y; }
  static double cross2(const V2& a, const V2& b)
  { return a.x*b.y - a.y*b.x; }
  static double norm2(const V2& a) { return std::sqrt(dot2(a, a)); }
  static double dist2(const V2& a, const V2& b) { return norm2(a - b); }
  static double kPi() { return std::acos(-1.0); }
  static double rel_eps() { return 1e-12; }

  //================ geometric snapshot =================

  struct SnapVertex
  {
    V3                 p;
    int                orig;        // original vertex index
    std::vector<int>   faces;       // incident snapshot faces
    std::vector<int>   edges;       // incident snapshot edges
    double             angle_sum;   // sum of incident corner angles
    bool               boundary;
    int                component;   // connected component id
  };

  struct SnapFace
  {
    std::array<int, 3>    verts;     // cfv order at construction time
    std::array<int, 3>    edges;     // edge i joins verts i and i+1
    int                   orig;      // original face index
    std::array<V2, 3>     local;     // intrinsic 2D embedding
    std::array<double, 3> angle;     // corner angles
    int                   component;
  };

  struct SnapEdge
  {
    int    v0, v1;                  // canonical orientation v0 -> v1
    int    f_pos;                   // face on the positive (left) side
    int    f_neg;                   // face on the negative side, -1 if none
    double length;
  };

  //================ solver state =================

  struct Window
  {
    int      edge;                  // snapshot edge
    double   b0, b1;                // interval along the canonical edge frame
    double   d0, d1;                // total source distances at b0, b1
    double   td, sigma;             // pseudosource projection / altitude
    double   ps_dist;               // source-to-pseudosource length
    int      ps_sign;               // +1 / -1: pseudosource canonical side
    int      enter_face;            // face on the pseudosource side
    int      parent;                // parent window id, -1 at a chain root
    int      root_vertex;           // pseudosource vertex for roots, else -1
    int      root_parent;           // window behind a vertex event, else -1
    bool     dead;
    bool     processed;             // already propagated through a face
  };

  struct QueueItem
  {
    double key;
    bool   is_vertex;
    int    id;
    bool operator<(const QueueItem& o) const { return key > o.key; }
  };

  struct SolveState
  {
    std::vector<Window> windows;
    std::vector<std::vector<int> > edge_windows;  // live window ids per edge
    std::vector<double> vdist;      // shortest vertex distances
    std::vector<int>    v_origin;   // window id establishing vdist, -1 source
    std::vector<char>   v_emitted;  // vertex event already processed
    int                 source;
    bool                done;
    SolveState() : source(-1), done(false) {}
  };

  //================ members =================

  std::vector<SnapVertex> vertices_;
  std::vector<SnapFace>   faces_;
  std::vector<SnapEdge>   edges_;
  std::vector<int>        v_local_;  // original idx -> snapshot idx, -1 dead
  std::vector<int>        f_local_;  // original idx -> snapshot idx, -1 dead
  std::vector<std::vector<int> > vfan_;  // ordered face fan per vertex
  SolveState              state_;

  //================ snapshot construction =================

  void build_snapshot(const Mesh& _mesh);
  void build_face_layout(SnapFace& _f) const;
  void build_fans();
  void compute_components();

  //================ frame helpers =================

  int  face_third_vertex(int _e, int _f) const;
  int  other_face(int _e, int _f) const;
  V2   edge_point_in_face(int _e, int _f, double _t, double _h) const;
  void canonical_coords(int _e, int _f, const V2& _p,
                        double& _t, double& _h) const;
  V2   face_bary_to_local(int _f, const std::array<double, 3>& _b) const;
  std::array<double, 3> local_to_bary(int _f, const V2& _p) const;
  V3   bary_to_world(int _f, const std::array<double, 3>& _b) const;
  int  vertex_slot(int _f, int _v) const;

  //================ window propagation =================

  int  new_window(SolveState& _s, int _edge, double _b0, double _b1,
                  double _td, double _sigma, double _ps_dist, int _sign,
                  int _enter_face, int _parent, int _root_v,
                  int _root_parent);
  void update_vertex_distance(SolveState& _s,
                              std::priority_queue<QueueItem>& _pq,
                              int _v, double _d, int _origin);
  void emit_vertex_windows(SolveState& _s,
                           std::priority_queue<QueueItem>& _pq,
                           int _v, int _origin);
  void insert_windows(SolveState& _s,
                      std::priority_queue<QueueItem>& _pq,
                      int _edge, std::vector<int>& _fresh);
  void lower_envelope(SolveState& _s, int _edge,
                      const std::vector<int>& _fresh_ids);
  void propagate(SolveState& _s, std::priority_queue<QueueItem>& _pq,
                 int _wid);
  static double window_value(const Window& _w, double _t)
  { return _w.ps_dist + std::hypot(_t - _w.td, _w.sigma); }

  //================ tracing =================

  void append_point(std::vector<SurfacePoint>& _out, int _face,
                    const std::array<double, 3>& _bary) const;
  std::vector<int> fan_arc(int _v, int _from_face, int _to_face) const;
  bool trace_walk(int _face, const std::array<double, 3>& _bary,
                  std::vector<SurfacePoint>& _out, double& _length) const;
  void finish(std::vector<SurfacePoint>& _out, double& _length) const;
};

//=============================================================================
//=============================================================================
//============ IMPLEMENTATION ================================================
//=============================================================================
//=============================================================================

template <class Mesh>
ExactGeodesicT<Mesh>::ExactGeodesicT(const Mesh& _mesh)
{
  build_snapshot(_mesh);
}

//-----------------------------------------------------------------------------

template <class Mesh>
void ExactGeodesicT<Mesh>::build_snapshot(const Mesh& _mesh)
{
  v_local_.assign(_mesh.n_vertices(), -1);
  f_local_.assign(_mesh.n_faces(),    -1);

  //---- vertices -----------------------------------------------------------
  for (typename Mesh::ConstVertexIter vi = _mesh.vertices_begin();
       vi != _mesh.vertices_end(); ++vi)
  {
    typename Mesh::VertexHandle vh = *vi;
    const typename Mesh::Point& p = _mesh.point(vh);
    if (!std::isfinite(static_cast<double>(p[0])) ||
        !std::isfinite(static_cast<double>(p[1])) ||
        !std::isfinite(static_cast<double>(p[2])))
      throw std::invalid_argument(
        "ExactGeodesicT: vertex with non-finite coordinates is not supported");

    SnapVertex sv;
    sv.p         = V3(static_cast<double>(p[0]),
                      static_cast<double>(p[1]),
                      static_cast<double>(p[2]));
    sv.orig      = vh.idx();
    sv.angle_sum = 0.0;
    sv.boundary  = false;
    sv.component = -1;
    v_local_[vh.idx()] = static_cast<int>(vertices_.size());
    vertices_.push_back(sv);
  }

  if (vertices_.empty())
    throw std::invalid_argument("ExactGeodesicT: mesh has no vertices");

  //---- faces --------------------------------------------------------------
  for (typename Mesh::ConstFaceIter fi = _mesh.faces_begin();
       fi != _mesh.faces_end(); ++fi)
  {
    typename Mesh::FaceHandle fh = *fi;
    SnapFace sf;
    sf.orig      = fh.idx();
    sf.component = -1;

    int k = 0;
    for (typename Mesh::ConstFaceVertexIter fv = _mesh.cfv_iter(fh);
         fv.is_valid(); ++fv, ++k)
    {
      if (k >= 3)
        throw std::invalid_argument(
          "ExactGeodesicT: only triangle faces are supported");
      typename Mesh::VertexHandle vh = *fv;
      if (!vh.is_valid() || !_mesh.is_valid_handle(vh) ||
          vh.idx() < 0 ||
          vh.idx() >= static_cast<int>(v_local_.size()) ||
          v_local_[vh.idx()] < 0)
        throw std::invalid_argument(
          "ExactGeodesicT: face references a dead vertex");
      sf.verts[k] = v_local_[vh.idx()];
    }
    if (k != 3)
      throw std::invalid_argument(
        "ExactGeodesicT: only triangle faces are supported");

    if (sf.verts[0] == sf.verts[1] || sf.verts[1] == sf.verts[2] ||
        sf.verts[2] == sf.verts[0])
      throw std::invalid_argument(
        "ExactGeodesicT: zero-area (degenerate) face is not supported");

    {
      const V3& a = vertices_[sf.verts[0]].p;
      const V3& b = vertices_[sf.verts[1]].p;
      const V3& c = vertices_[sf.verts[2]].p;
      V3 ab = b - a, ac = c - a;
      V3 cr(ab.y*ac.z - ab.z*ac.y,
            ab.z*ac.x - ab.x*ac.z,
            ab.x*ac.y - ab.y*ac.x);
      double scale2 = std::max(dot3(ab, ab), dot3(ac, ac));
      if (dot3(cr, cr) <= 1e-20 * std::max(1.0, scale2))
        throw std::invalid_argument(
          "ExactGeodesicT: zero-area (degenerate) face is not supported");
    }

    f_local_[fh.idx()] = static_cast<int>(faces_.size());
    faces_.push_back(sf);
  }

  if (faces_.empty())
    throw std::invalid_argument(
      "ExactGeodesicT: mesh without faces (isolated vertices) is not supported");

  //---- edges --------------------------------------------------------------
  std::vector<std::vector<std::pair<int, int> > > v_edges(vertices_.size());
  auto find_edge = [&](int a, int b) -> int
  {
    for (const std::pair<int,int>& pr : v_edges[a])
      if (pr.first == b) return pr.second;
    return -1;
  };

  for (int fi = 0; fi < static_cast<int>(faces_.size()); ++fi)
  {
    SnapFace& f = faces_[fi];
    for (int i = 0; i < 3; ++i)
    {
      int a = f.verts[i];
      int b = f.verts[(i + 1) % 3];
      int eid = find_edge(a, b);
      if (eid < 0) eid = find_edge(b, a);
      if (eid < 0)
      {
        SnapEdge e;
        e.v0 = a; e.v1 = b;
        e.f_pos = fi; e.f_neg = -1;
        e.length = norm3(vertices_[b].p - vertices_[a].p);
        if (!(e.length > 0.0))
          throw std::invalid_argument(
            "ExactGeodesicT: zero-length edge between distinct vertices");
        eid = static_cast<int>(edges_.size());
        edges_.push_back(e);
        v_edges[a].push_back(std::make_pair(b, eid));
        v_edges[b].push_back(std::make_pair(a, eid));
        vertices_[a].edges.push_back(eid);
        vertices_[b].edges.push_back(eid);
      }
      else
      {
        SnapEdge& e = edges_[eid];
        if (e.f_pos == fi || e.f_neg == fi)
          throw std::invalid_argument(
            "ExactGeodesicT: duplicate face orientation detected");
        if (e.f_neg >= 0)
          throw std::invalid_argument(
            "ExactGeodesicT: non-manifold edge (more than two incident faces)");
        e.f_neg = fi;
      }
      f.edges[i] = eid;
    }
  }

  //---- intrinsic face layouts and corner angles ---------------------------
  for (int fi = 0; fi < static_cast<int>(faces_.size()); ++fi)
  {
    SnapFace& f = faces_[fi];
    build_face_layout(f);
    for (int i = 0; i < 3; ++i)
      vertices_[f.verts[i]].angle_sum += f.angle[i];
  }

  //---- vertex incidence, manifold and isolated checks ---------------------
  for (int vi = 0; vi < static_cast<int>(vertices_.size()); ++vi)
  {
    SnapVertex& v = vertices_[vi];
    for (int eid : v.edges)
    {
      const SnapEdge& e = edges_[eid];
      if (e.f_pos >= 0) v.faces.push_back(e.f_pos);
      if (e.f_neg >= 0) v.faces.push_back(e.f_neg);
    }
    std::sort(v.faces.begin(), v.faces.end());
    v.faces.erase(std::unique(v.faces.begin(), v.faces.end()), v.faces.end());

    if (v.faces.empty())
      throw std::invalid_argument(
        "ExactGeodesicT: isolated vertex is not supported");

    if (!_mesh.is_manifold(typename Mesh::VertexHandle(v.orig)))
      throw std::invalid_argument(
        "ExactGeodesicT: non-manifold vertex is not supported");

    v.boundary = false;
    for (int eid : v.edges)
      if (edges_[eid].f_neg < 0) { v.boundary = true; break; }

    std::size_t nf = v.faces.size();
    if (v.boundary)
    {
      if (v.edges.size() != nf + 1)
        throw std::invalid_argument(
          "ExactGeodesicT: non-manifold vertex fan is not supported");
    }
    else if (v.edges.size() != nf)
      throw std::invalid_argument(
        "ExactGeodesicT: non-manifold vertex fan is not supported");
  }

  // orient canonical edge frames so that f_pos is the left-hand face of
  // the directed edge v0 -> v1 in that face's intrinsic layout.
  for (int ei = 0; ei < static_cast<int>(edges_.size()); ++ei)
  {
    SnapEdge& e = edges_[ei];
    int sf = e.f_pos >= 0 ? e.f_pos : e.f_neg;
    const SnapFace& f = faces_[sf];
    int c = face_third_vertex(ei, sf);
    V2 pa, pb, pc;
    for (int i = 0; i < 3; ++i)
    {
      if (f.verts[i] == e.v0) pa = f.local[i];
      if (f.verts[i] == e.v1) pb = f.local[i];
      if (f.verts[i] == c)    pc = f.local[i];
    }
    if (cross2(pb - pa, pc - pa) < 0.0)
      std::swap(e.f_pos, e.f_neg);
  }

  build_fans();
  compute_components();
}

//-----------------------------------------------------------------------------

template <class Mesh>
void ExactGeodesicT<Mesh>::build_face_layout(SnapFace& _f) const
{
  const V3& p0 = vertices_[_f.verts[0]].p;
  const V3& p1 = vertices_[_f.verts[1]].p;
  const V3& p2 = vertices_[_f.verts[2]].p;
  double l01 = norm3(p1 - p0);
  double l02 = norm3(p2 - p0);
  double l12 = norm3(p2 - p1);

  _f.local[0] = V2(0.0, 0.0);
  _f.local[1] = V2(l01, 0.0);
  double x  = (l01*l01 + l02*l02 - l12*l12) / (2.0 * l01);
  double y2 = l02*l02 - x*x;
  if (y2 < 0.0) y2 = 0.0;
  _f.local[2] = V2(x, std::sqrt(y2));

  auto angle_at = [&](double la, double lb, double lc)
  {
    double cs = (la*la + lb*lb - lc*lc) / (2.0 * la * lb);
    cs = std::max(-1.0, std::min(1.0, cs));
    return std::acos(cs);
  };
  _f.angle[0] = angle_at(l01, l02, l12);
  _f.angle[1] = angle_at(l01, l12, l02);
  _f.angle[2] = angle_at(l02, l12, l01);
}

//-----------------------------------------------------------------------------

template <class Mesh>
void ExactGeodesicT<Mesh>::build_fans()
{
  vfan_.resize(vertices_.size());
  for (int vi = 0; vi < static_cast<int>(vertices_.size()); ++vi)
  {
    const SnapVertex& v = vertices_[vi];
    int start = v.faces.front();
    if (v.boundary)
    {
      // begin at a face touching a boundary edge incident to vi
      for (int fi : v.faces)
      {
        const SnapFace& f = faces_[fi];
        int slot = vertex_slot(fi, vi);
        int ea = f.edges[slot];                 // edge vi -> verts[slot+1]
        int eb = f.edges[(slot + 2) % 3];       // edge verts[slot-1] -> vi
        if (edges_[ea].f_neg < 0 || edges_[eb].f_neg < 0) { start = fi; break; }
      }
    }

    std::vector<int>& fan = vfan_[vi];
    std::vector<char> visited(faces_.size(), 0);
    int cur = start;
    while (cur >= 0 && !visited[cur])
    {
      visited[cur] = 1;
      fan.push_back(cur);
      const SnapFace& f = faces_[cur];
      int slot = vertex_slot(cur, vi);
      int ea = f.edges[slot];
      int eb = f.edges[(slot + 2) % 3];
      int na = other_face(ea, cur);
      int nb = other_face(eb, cur);
      int next = -1;
      if (na >= 0 && !visited[na]) next = na;
      else if (nb >= 0 && !visited[nb]) next = nb;
      cur = next;
    }
  }
}

//-----------------------------------------------------------------------------

template <class Mesh>
void ExactGeodesicT<Mesh>::compute_components()
{
  std::vector<int> fcomp(faces_.size(), -1);
  int comp = 0;
  for (int seed = 0; seed < static_cast<int>(faces_.size()); ++seed)
  {
    if (fcomp[seed] >= 0) continue;
    std::vector<int> stack(1, seed);
    fcomp[seed] = comp;
    while (!stack.empty())
    {
      int fi = stack.back(); stack.pop_back();
      for (int eid : faces_[fi].edges)
      {
        int of = other_face(eid, fi);
        if (of >= 0 && fcomp[of] < 0)
        {
          fcomp[of] = comp;
          stack.push_back(of);
        }
      }
    }
    ++comp;
  }
  for (std::size_t fi = 0; fi < faces_.size(); ++fi)
  {
    faces_[fi].component = fcomp[fi];
    for (int v : faces_[fi].verts)
      vertices_[v].component = fcomp[fi];
  }
}

//-----------------------------------------------------------------------------
// frame helpers
//-----------------------------------------------------------------------------

template <class Mesh>
int ExactGeodesicT<Mesh>::face_third_vertex(int _e, int _f) const
{
  const SnapEdge& e = edges_[_e];
  for (int v : faces_[_f].verts)
    if (v != e.v0 && v != e.v1) return v;
  return -1;
}

template <class Mesh>
int ExactGeodesicT<Mesh>::other_face(int _e, int _f) const
{
  const SnapEdge& e = edges_[_e];
  if (e.f_pos == _f) return e.f_neg;
  if (e.f_neg == _f) return e.f_pos;
  return -1;
}

template <class Mesh>
int ExactGeodesicT<Mesh>::vertex_slot(int _f, int _v) const
{
  const SnapFace& f = faces_[_f];
  for (int i = 0; i < 3; ++i)
    if (f.verts[i] == _v) return i;
  return -1;
}

template <class Mesh>
typename ExactGeodesicT<Mesh>::V2
ExactGeodesicT<Mesh>::edge_point_in_face(
  int _e, int _f, double _t, double _h) const
{
  const SnapEdge& e = edges_[_e];
  const SnapFace& f = faces_[_f];
  V2 pa, pb;
  for (int i = 0; i < 3; ++i)
  {
    if (f.verts[i] == e.v0) pa = f.local[i];
    if (f.verts[i] == e.v1) pb = f.local[i];
  }
  V2 u = (pb - pa) * (1.0 / e.length);
  V2 nL(-u.y, u.x);                   // left normal of v0 -> v1 in this layout
  // In each incident face's own intrinsic layout, the left normal of
  // v0 -> v1 points toward the canonical f_pos side of the edge, so the
  // same canonical height works in both faces.
  return pa + u * _t + nL * _h;
}

template <class Mesh>
void ExactGeodesicT<Mesh>::canonical_coords(
  int _e, int _f, const V2& _p, double& _t, double& _h) const
{
  const SnapEdge& e = edges_[_e];
  const SnapFace& f = faces_[_f];
  V2 pa, pb;
  for (int i = 0; i < 3; ++i)
  {
    if (f.verts[i] == e.v0) pa = f.local[i];
    if (f.verts[i] == e.v1) pb = f.local[i];
  }
  V2 u = (pb - pa) * (1.0 / e.length);
  V2 d = _p - pa;
  _t = dot2(d, u);
  _h = cross2(u, d);                  // > 0 on the canonical f_pos side
}

template <class Mesh>
typename ExactGeodesicT<Mesh>::V2
ExactGeodesicT<Mesh>::face_bary_to_local(
  int _f, const std::array<double, 3>& _b) const
{
  const SnapFace& f = faces_[_f];
  V2 p(0.0, 0.0);
  for (int i = 0; i < 3; ++i)
    p = p + f.local[i] * _b[i];
  return p;
}

template <class Mesh>
std::array<double, 3>
ExactGeodesicT<Mesh>::local_to_bary(int _f, const V2& _p) const
{
  const SnapFace& f = faces_[_f];
  V2 a = f.local[0], b = f.local[1], c = f.local[2];
  double den = cross2(b - a, c - a);
  std::array<double, 3> bary;
  bary[1] = cross2(_p - a, c - a) / den;
  bary[2] = cross2(b - a, _p - a) / den;
  bary[0] = 1.0 - bary[1] - bary[2];
  for (double& q : bary)
  {
    if (std::fabs(q) < 1e-12) q = 0.0;
    else if (std::fabs(q - 1.0) < 1e-12) q = 1.0;
  }
  return bary;
}

template <class Mesh>
typename ExactGeodesicT<Mesh>::V3
ExactGeodesicT<Mesh>::bary_to_world(
  int _f, const std::array<double, 3>& _b) const
{
  const SnapFace& f = faces_[_f];
  V3 p(0.0, 0.0, 0.0);
  for (int i = 0; i < 3; ++i)
    p = p + vertices_[f.verts[i]].p * _b[i];
  return p;
}

//-----------------------------------------------------------------------------
// windows
//-----------------------------------------------------------------------------

template <class Mesh>
int ExactGeodesicT<Mesh>::new_window(
  SolveState& _s, int _edge, double _b0, double _b1,
  double _td, double _sigma, double _ps_dist, int _sign,
  int _enter_face, int _parent, int _root_v, int _root_parent)
{
  const SnapEdge& e = edges_[_edge];
  double eps_t = rel_eps() * std::max(1.0, e.length);
  if (_b0 < 0.0 && _b0 > -eps_t) _b0 = 0.0;
  if (_b1 > e.length && _b1 < e.length + eps_t) _b1 = e.length;
  if (!(_b1 > _b0)) return -1;
  if (_b1 - _b0 <= eps_t) return -1;

  Window w;
  w.edge       = _edge;
  w.b0 = _b0; w.b1 = _b1;
  w.td = _td; w.sigma = _sigma; w.ps_dist = _ps_dist;
  w.ps_sign    = _sign;
  w.enter_face = _enter_face;
  w.parent     = _parent;
  w.root_vertex = _root_v;
  w.root_parent = _root_parent;
  w.dead       = false;
  w.processed  = false;
  w.d0 = window_value(w, _b0);
  w.d1 = window_value(w, _b1);
  _s.windows.push_back(w);
  return static_cast<int>(_s.windows.size()) - 1;
}

template <class Mesh>
void ExactGeodesicT<Mesh>::update_vertex_distance(
  SolveState& _s, std::priority_queue<QueueItem>& _pq,
  int _v, double _d, int _origin)
{
  if (!std::isfinite(_d)) return;
  if (_d < _s.vdist[_v])
  {
    _s.vdist[_v]   = _d;
    _s.v_origin[_v] = _origin;
    _pq.push(QueueItem{_d, true, _v});
  }
}

template <class Mesh>
void ExactGeodesicT<Mesh>::emit_vertex_windows(
  SolveState& _s, std::priority_queue<QueueItem>& _pq,
  int _v, int _origin)
{
  const SnapVertex& sv = vertices_[_v];
  double dv = _s.vdist[_v];

  for (int fi : sv.faces)
  {
    const SnapFace& f = faces_[fi];
    int slot = vertex_slot(fi, _v);
    V2 pv = f.local[slot];

    for (int k = 0; k < 3; ++k)
    {
      int eid = f.edges[k];
      const SnapEdge& e = edges_[eid];
      double t, h;
      canonical_coords(eid, fi, pv, t, h);
      int sign = h >= 0.0 ? 1 : -1;
      std::vector<int> fresh;
      int wid = new_window(_s, eid, 0.0, e.length, t,
                           std::fabs(h), dv, sign, fi,
                           -1, _v, _origin);
      if (wid >= 0)
      {
        fresh.push_back(wid);
        insert_windows(_s, _pq, eid, fresh);
      }
    }
  }
}

//-----------------------------------------------------------------------------
// pointwise lower envelope of the distance functions living on one edge
//-----------------------------------------------------------------------------

template <class Mesh>
void ExactGeodesicT<Mesh>::lower_envelope(
  SolveState& _s, int _edge, const std::vector<int>& _fresh_ids)
{
  const SnapEdge& e = edges_[_edge];
  const double L = e.length;
  const double eps_t = rel_eps() * std::max(1.0, L);

  std::vector<int> all = _s.edge_windows[_edge];
  for (int id : _fresh_ids)
    if (!_s.windows[id].dead) all.push_back(id);
  if (all.empty()) return;

  // breakpoints: interval ends and pairwise intersections of squared
  // distance functions (two pseudodistance functions cross at most once)
  std::vector<double> cuts;
  cuts.push_back(0.0);
  cuts.push_back(L);
  for (int id : all)
  {
    const Window& w = _s.windows[id];
    cuts.push_back(w.b0);
    cuts.push_back(w.b1);
  }
  for (std::size_t i = 0; i < all.size(); ++i)
  {
    const Window& wi = _s.windows[all[i]];
    for (std::size_t j = i + 1; j < all.size(); ++j)
    {
      const Window& wj = _s.windows[all[j]];
      double lo = std::max(wi.b0, wj.b0);
      double hi = std::min(wi.b1, wj.b1);
      if (hi - lo <= eps_t) continue;
      // the difference of two offset pseudodistance functions is smooth
      // and has at most two zeros; bracket them by dense sampling and
      // refine with bisection.
      const int samples = 64;
      double prev_x = lo;
      double prev_g = window_value(wi, lo) - window_value(wj, lo);
      for (int s = 1; s <= samples; ++s)
      {
        double x = lo + (hi - lo) * (double)s / samples;
        double g = window_value(wi, x) - window_value(wj, x);
        if (prev_g * g < 0.0)
        {
          double a = prev_x, b = x, fa = prev_g;
          for (int it = 0; it < 80; ++it)
          {
            double m = 0.5 * (a + b);
            double fm = window_value(wi, m) - window_value(wj, m);
            if (fa * fm <= 0.0) { b = m; }
            else                 { a = m; fa = fm; }
          }
          double root = 0.5 * (a + b);
          if (root > lo + eps_t && root < hi - eps_t) cuts.push_back(root);
        }
        prev_x = x; prev_g = g;
      }
    }
  }
  std::sort(cuts.begin(), cuts.end());
  std::vector<double> bp;
  for (double c : cuts)
  {
    c = std::max(0.0, std::min(L, c));
    if (bp.empty() || c - bp.back() > eps_t) bp.push_back(c);
  }
  if (bp.size() < 2) return;

  // winner on each elementary interval, sampled at the midpoint
  std::vector<int> winner(bp.size() - 1, -1);
  for (std::size_t k = 0; k + 1 < bp.size(); ++k)
  {
    double mid = 0.5 * (bp[k] + bp[k + 1]);
    double best = std::numeric_limits<double>::infinity();
    for (int id : all)
    {
      const Window& w = _s.windows[id];
      if (mid < w.b0 - eps_t || mid > w.b1 + eps_t) continue;
      double d = window_value(w, mid);
      if (d < best) { best = d; winner[k] = id; }
    }
  }

  // old windows die; surviving pieces are recreated with clipped intervals.
  // dead windows keep every field, so parent chains remain traceable.
  for (int id : _s.edge_windows[_edge])
    _s.windows[id].dead = true;

  std::vector<int> new_live;
  std::size_t k = 0;
  while (k < winner.size())
  {
    int wid = winner[k];
    if (wid < 0) { ++k; continue; }
    std::size_t k2 = k;
    while (k2 + 1 < winner.size() && winner[k2 + 1] == wid) ++k2;
    double lo = bp[k], hi = bp[k2 + 1];
    Window w = _s.windows[wid];
    w.dead = false;
    w.b0 = lo; w.b1 = hi;
    w.d0 = window_value(w, lo);
    w.d1 = window_value(w, hi);
    // a clipped piece of an already propagated window needs no second
    // propagation: the original pass already covered this sub-interval.
    _s.windows.push_back(w);
    new_live.push_back(static_cast<int>(_s.windows.size()) - 1);
    k = k2 + 1;
  }
  _s.edge_windows[_edge].swap(new_live);
}

//-----------------------------------------------------------------------------

template <class Mesh>
void ExactGeodesicT<Mesh>::insert_windows(
  SolveState& _s, std::priority_queue<QueueItem>& _pq,
  int _edge, std::vector<int>& _fresh)
{
  if (_fresh.empty()) return;
  lower_envelope(_s, _edge, _fresh);

  const SnapEdge& e = edges_[_edge];
  double eps_t = rel_eps() * std::max(1.0, e.length);
  for (int id : _s.edge_windows[_edge])
  {
    const Window& w = _s.windows[id];
    double t = std::max(w.b0, std::min(w.b1, w.td));
    _pq.push(QueueItem{window_value(w, t), false, id});

    if (w.b0 <= eps_t)
      update_vertex_distance(_s, _pq, e.v0, w.d0, id);
    if (w.b1 >= e.length - eps_t)
      update_vertex_distance(_s, _pq, e.v1, w.d1, id);
  }
}

//-----------------------------------------------------------------------------
// propagate one window through the triangle opposite its enter face
//-----------------------------------------------------------------------------

template <class Mesh>
void ExactGeodesicT<Mesh>::propagate(
  SolveState& _s, std::priority_queue<QueueItem>& _pq, int _wid)
{
  const Window& w = _s.windows[_wid];
  if (w.dead) return;

  const int eid = w.edge;
  const SnapEdge& e = edges_[eid];
  const int f2 = other_face(eid, w.enter_face);
  if (f2 < 0) return;                                 // boundary: wave stops
  const SnapFace& face = faces_[f2];
  const int c = face_third_vertex(eid, f2);

  // Unfold the pseudosource into f2's intrinsic plane.  The canonical edge
  // frame is face-independent: the pseudosource stays on the source side of
  // the entry edge and keeps its canonical sign; the intrinsic layout maps
  // that side automatically, including in the f_neg face.
  const V2 P  = edge_point_in_face(eid, f2, w.td,
                                   w.ps_sign * w.sigma);
  V2 A, B, C;
  int ia = -1, ib = -1;
  for (int i = 0; i < 3; ++i)
  {
    if (face.verts[i] == e.v0) { A = face.local[i]; ia = i; }
    if (face.verts[i] == e.v1) { B = face.local[i]; ib = i; }
    if (face.verts[i] == c)    { C = face.local[i]; }
  }
  // edge opposite slot i joins verts[i+1] and verts[i+2] = edges[(i+1)%3]
  const int e_ac = face.edges[(ib + 1) % 3];  // edge A-C, opposite B
  const int e_bc = face.edges[(ia + 1) % 3];  // edge B-C, opposite A

  // ray P -> X extended past X; intersect with segment U-V
  auto ray_hit = [&](const V2& X, const V2& U, const V2& V,
                     V2& hit) -> bool
  {
    V2 rx = X - P, sx = V - U;
    double den = cross2(rx, sx);
    if (std::fabs(den) < 1e-18) return false;
    V2 q = U - P;
    double lr = cross2(q, sx) / den;     // parameter on P + lr*rx
    double ls = cross2(q, rx) / den;     // parameter on U + ls*sx
    if (lr < 1.0 - 1e-10) return false;
    if (ls < -1e-10 || ls > 1.0 + 1e-10) return false;
    hit = P + rx * lr;
    return true;
  };

  // canonical edge parameter where the separatrix ray P -> C crosses AB
  double t_cross = std::numeric_limits<double>::quiet_NaN();
  {
    V2 ab = B - A, pc = C - P;
    double den = cross2(pc, ab);
    if (std::fabs(den) >= 1e-18)
    {
      double lr = cross2(A - P, ab) / den;   // parameter on P + lr*pc
      double ls = cross2(A - P, pc) / den;   // parameter on A + ls*ab
      if (lr > 1e-10 && lr < 1.0 - 1e-10 &&
          ls >= -1e-10 && ls <= 1.0 + 1e-10)
      {
        V2 hit = P + pc * lr;
        double hh;
        canonical_coords(eid, f2, hit, t_cross, hh);
      }
    }
  }

  struct Sub { double lo, hi; int dest; V2 hlo, hhi; };
  std::vector<Sub> subs;
  const double eps_t = rel_eps() * std::max(1.0, e.length);

  auto edge_of = [&](double u) -> int
  {
    V2 X = edge_point_in_face(eid, f2, u, 0.0);
    V2 dummy;
    if (ray_hit(X, A, C, dummy)) return e_ac;
    return e_bc;
  };

  std::vector<double> splits;
  splits.push_back(w.b0);
  if (std::isfinite(t_cross) &&
      t_cross > w.b0 + eps_t && t_cross < w.b1 - eps_t)
    splits.push_back(t_cross);
  splits.push_back(w.b1);

  for (std::size_t si = 0; si + 1 < splits.size(); ++si)
  {
    double u0 = splits[si], u1 = splits[si + 1];
    int dest = edge_of(0.5 * (u0 + u1));
    V2 X0 = edge_point_in_face(eid, f2, u0, 0.0);
    V2 X1 = edge_point_in_face(eid, f2, u1, 0.0);
    V2 h0, h1;
    bool ok0 = ray_hit(X0, A, C, h0);
    bool ok1 = ray_hit(X1, A, C, h1);
    if (dest == e_bc)
    {
      ok0 = ray_hit(X0, B, C, h0);
      ok1 = ray_hit(X1, B, C, h1);
    }
    if (!ok0 || !ok1) continue;
    subs.push_back(Sub{u0, u1, dest, h0, h1});
  }

  std::vector<int> fresh_ac, fresh_bc;
  for (const Sub& sub : subs)
  {
    const SnapEdge& de = edges_[sub.dest];
    double t0, hh0, t1, hh1;
    canonical_coords(sub.dest, f2, sub.hlo, t0, hh0);
    canonical_coords(sub.dest, f2, sub.hhi, t1, hh1);
    double lo = std::max(0.0, std::min(de.length, std::min(t0, t1)));
    double hi = std::min(de.length, std::max(t0, t1));
    double tp, hp;
    canonical_coords(sub.dest, f2, P, tp, hp);
    int sign = hp >= 0.0 ? 1 : -1;
    int nid = new_window(_s, sub.dest, lo, hi, tp, std::fabs(hp),
                         w.ps_dist, sign, f2, _wid,
                         w.root_vertex, w.root_parent);
    if (nid >= 0)
    {
      if (sub.dest == e_ac) fresh_ac.push_back(nid);
      else                  fresh_bc.push_back(nid);
    }
  }

  // the separatrix through C reaches the third vertex straight
  if (std::isfinite(t_cross) &&
      t_cross >= w.b0 - eps_t && t_cross <= w.b1 + eps_t)
    update_vertex_distance(_s, _pq, c,
                           w.ps_dist + dist2(P, C), _wid);

  if (!fresh_ac.empty()) insert_windows(_s, _pq, e_ac, fresh_ac);
  if (!fresh_bc.empty()) insert_windows(_s, _pq, e_bc, fresh_bc);
}

//-----------------------------------------------------------------------------
// solve
//-----------------------------------------------------------------------------

template <class Mesh>
bool ExactGeodesicT<Mesh>::solve(typename Mesh::VertexHandle _source)
{
  if (!_source.is_valid() ||
      _source.idx() < 0 ||
      _source.idx() >= static_cast<int>(v_local_.size()) ||
      v_local_[_source.idx()] < 0)
    return false;

  const int src = v_local_[_source.idx()];

  // build the new solution locally: a failure must never destroy an
  // existing successful solution.
  SolveState ns;
  ns.windows.reserve(edges_.size() * 8);
  ns.edge_windows.resize(edges_.size());
  ns.vdist.assign(vertices_.size(),
                  std::numeric_limits<double>::infinity());
  ns.v_origin.assign(vertices_.size(), -1);
  ns.v_emitted.assign(vertices_.size(), 0);
  ns.source = src;

  std::priority_queue<QueueItem> pq;
  ns.vdist[src] = 0.0;

  // root windows: the source acts as pseudosource inside each incident face
  for (int fi : vertices_[src].faces)
  {
    const SnapFace& f = faces_[fi];
    int slot = vertex_slot(fi, src);
    V2 ps = f.local[slot];
    for (int k = 0; k < 3; ++k)
    {
      int eid = f.edges[k];
      const SnapEdge& e = edges_[eid];
      double t, h;
      canonical_coords(eid, fi, ps, t, h);
      std::vector<int> fresh;
      int wid = new_window(ns, eid, 0.0, e.length, t, std::fabs(h),
                           0.0, h >= 0.0 ? 1 : -1, fi,
                           -1, src, -1);
      if (wid >= 0) fresh.push_back(wid);
      insert_windows(ns, pq, eid, fresh);
    }
  }
  ns.v_emitted[src] = 1;

  while (!pq.empty())
  {
    QueueItem item = pq.top(); pq.pop();

    if (item.is_vertex)
    {
      int v = item.id;
      if (ns.v_emitted[v]) continue;
      if (item.key > ns.vdist[v] + 1e-10) continue;

      // Pseudosource events are allowed only where the surface cannot be
      // flattened without overlap: saddle interior vertices (total angle
      // > 2 pi) and concave boundary vertices (total angle > pi).  Convex
      // interior vertices and convex/flat boundary vertices must never act
      // as pseudosources, or they would introduce illegal shortcuts.
      const double angle = vertices_[v].angle_sum;
      bool emit = vertices_[v].boundary
                  ? angle > kPi() + 1e-10
                  : angle > 2.0 * kPi() + 1e-10;
      ns.v_emitted[v] = 1;
      if (emit)
        emit_vertex_windows(ns, pq, v, ns.v_origin[v]);
      continue;
    }

    Window& w = ns.windows[item.id];
    if (w.dead || w.processed) continue;
    double t = std::max(w.b0, std::min(w.b1, w.td));
    double cur = window_value(w, t);
    if (item.key < cur - 1e-10)
    {
      pq.push(QueueItem{cur, false, item.id});
      continue;
    }
    w.processed = true;
    propagate(ns, pq, item.id);
  }

  ns.done = true;
  state_ = std::move(ns);
  return true;
}

//-----------------------------------------------------------------------------
// queries
//-----------------------------------------------------------------------------

template <class Mesh>
double ExactGeodesicT<Mesh>::distance(
  typename Mesh::VertexHandle _v) const
{
  if (!state_.done)
    throw std::logic_error(
      "ExactGeodesicT::distance: solve() has not been called");
  if (!_v.is_valid() || _v.idx() < 0 ||
      _v.idx() >= static_cast<int>(v_local_.size()) ||
      v_local_[_v.idx()] < 0)
    throw std::invalid_argument(
      "ExactGeodesicT::distance: invalid vertex handle");
  return state_.vdist[v_local_[_v.idx()]];
}

//-----------------------------------------------------------------------------
// tracing
//-----------------------------------------------------------------------------

template <class Mesh>
void ExactGeodesicT<Mesh>::append_point(
  std::vector<SurfacePoint>& _out, int _face,
  const std::array<double, 3>& _bary) const
{
  SurfacePoint sp;
  sp.face = typename Mesh::FaceHandle(faces_[_face].orig);
  sp.bary = _bary;
  _out.push_back(sp);
}

template <class Mesh>
std::vector<int> ExactGeodesicT<Mesh>::fan_arc(
  int _v, int _from_face, int _to_face) const
{
  // faces strictly after _from_face up to and including _to_face.
  // Through a vertex either rotational direction is geometrically valid;
  // take the shorter contiguous arc of the stored fan.
  const std::vector<int>& fan = vfan_[_v];
  std::size_t p0 = fan.size(), p1 = fan.size();
  for (std::size_t i = 0; i < fan.size(); ++i)
  {
    if (fan[i] == _from_face) p0 = i;
    if (fan[i] == _to_face)   p1 = i;
  }
  std::vector<int> arc;
  if (p0 == fan.size() || p1 == fan.size() || p0 == p1) return arc;
  if (p1 > p0)
    for (std::size_t i = p0 + 1; i <= p1; ++i) arc.push_back(fan[i]);
  else
    for (std::size_t i = p0; i > p1; --i) arc.push_back(fan[i - 1]);
  return arc;
}

template <class Mesh>
bool ExactGeodesicT<Mesh>::trace(
  typename Mesh::FaceHandle _f, const std::array<double, 3>& _bary,
  std::vector<SurfacePoint>& _out, double& _length) const
{
  if (!state_.done)
    throw std::logic_error(
      "ExactGeodesicT::trace: solve() has not been called");
  if (!_f.is_valid() || _f.idx() < 0 ||
      _f.idx() >= static_cast<int>(f_local_.size()) ||
      f_local_[_f.idx()] < 0)
    throw std::invalid_argument(
      "ExactGeodesicT::trace: invalid face handle");
  for (double q : _bary)
  {
    if (!std::isfinite(q) || q < 0.0 || q > 1.0 + 1e-12)
      throw std::invalid_argument(
        "ExactGeodesicT::trace: barycentric coordinates must be in [0,1]");
  }
  if (std::fabs(_bary[0] + _bary[1] + _bary[2] - 1.0) > 1e-9)
    throw std::invalid_argument(
      "ExactGeodesicT::trace: barycentric coordinates must sum to 1");

  const int fi = f_local_[_f.idx()];
  if (vertices_[faces_[fi].verts[0]].component !=
      vertices_[state_.source].component)
    return false;

  std::vector<SurfacePoint> path;
  double length = 0.0;
  if (!trace_walk(fi, _bary, path, length))
    return false;

  _out = std::move(path);
  _length = length;
  return true;
}

//-----------------------------------------------------------------------------

template <class Mesh>
bool ExactGeodesicT<Mesh>::trace_walk(
  int _face, const std::array<double, 3>& _bary,
  std::vector<SurfacePoint>& _out, double& _length) const
{
  const SolveState& s = state_;

  struct Cand
  {
    double cost;
    bool   via_window;
    int    wid;
    V2     cross;
    int    vert;
  };

  int F = _face;
  V2 q = face_bary_to_local(F, _bary);
  append_point(_out, F, _bary);

  auto corner_bary = [&](int face, int v)
  {
    std::array<double, 3> b{{0.0, 0.0, 0.0}};
    b[vertex_slot(face, v)] = 1.0;
    return b;
  };

  // evaluate every way to leave the current face back toward the source
  auto best_candidate = [&](int face, const V2& pt) -> Cand
  {
    Cand best{std::numeric_limits<double>::infinity(),
              false, -1, V2(), -1};
    const SnapFace& f = faces_[face];

    for (int k = 0; k < 3; ++k)
    {
      int eid = f.edges[k];
      const SnapEdge& e = edges_[eid];
      for (int id : s.edge_windows[eid])
      {
        const Window& w = s.windows[id];
        if (w.enter_face == face) continue;       // points the wrong way
        V2 P = edge_point_in_face(eid, face, w.td, w.ps_sign * w.sigma);
        V2 A = edge_point_in_face(eid, face, 0.0, 0.0);
        V2 B = edge_point_in_face(eid, face, e.length, 0.0);
        V2 ab = B - A, pq = pt - P;
        double den = cross2(pq, ab);
        double cost;
        V2 r;
        double eps_t = rel_eps() * std::max(1.0, e.length);
        if (std::fabs(den) < 1e-18)
        {
          double tc = std::max(w.b0, std::min(w.b1,
                                               std::max(0.0,
                                                 std::min(e.length, w.td))));
          r = edge_point_in_face(eid, face, tc, 0.0);
          cost = w.ps_dist + dist2(P, r) + dist2(r, pt);
        }
        else
        {
          double lp = cross2(A - P, ab) / den;
          double te = cross2(A - P, pq) / den;
          r = P + pq * lp;
          if (lp >= -1e-10 && lp <= 1.0 + 1e-10 &&
              te >= w.b0 - eps_t && te <= w.b1 + eps_t)
            cost = w.ps_dist + dist2(P, pt);      // straight in unfolding
          else
          {
            double tc = std::max(w.b0, std::min(w.b1,
                            std::max(0.0, std::min(e.length, te))));
            r = edge_point_in_face(eid, face, tc, 0.0);
            cost = w.ps_dist + dist2(P, r) + dist2(r, pt);
          }
        }
        if (cost < best.cost)
          best = Cand{cost, true, id, r, -1};
      }
    }

    for (int i = 0; i < 3; ++i)
    {
      int v = f.verts[i];
      double d = s.vdist[v] + dist2(f.local[i], pt);
      if (d < best.cost)
        best = Cand{d, false, -1, f.local[i], v};
    }
    return best;
  };

  Cand best = best_candidate(F, q);
  if (!std::isfinite(best.cost)) return false;

  int wid = -1;

  if (!best.via_window)
  {
    // leave through a corner
    int v = best.vert;
    append_point(_out, F, corner_bary(F, v));
    if (v == s.source) { finish(_out, _length); return true; }
    wid = s.v_origin[v];
    if (wid < 0) return false;
    int G = s.windows[wid].enter_face;
    if (G != F)
    {
      // walk around the vertex fan; each step crosses an edge at the vertex
      for (int nf : fan_arc(v, F, G))
        append_point(_out, nf, corner_bary(nf, v));
      F = G;
    }
    q = face_bary_to_local(F, corner_bary(F, v));
  }
  else
  {
    wid = best.wid;
    const Window& w0 = s.windows[wid];
    int eid = w0.edge;
    int G = w0.enter_face;
    std::array<double, 3> rb = local_to_bary(F, best.cross);
    append_point(_out, F, rb);                    // crossing, current face
    double tpar, hh;
    canonical_coords(eid, F, best.cross, tpar, hh);
    V2 r2 = edge_point_in_face(eid, G, tpar, 0.0);
    append_point(_out, G, local_to_bary(G, r2)); // crossing, enter face
    F = G;
    q = r2;
  }

  // follow the window chain back to the pseudosource roots
  for (int guard = 0; guard < 1000000; ++guard)
  {
    const Window& w = s.windows[wid];
    int eid = w.edge;
    const SnapEdge& e = edges_[eid];

    if (w.parent < 0)
    {
      // chain root: pseudosource is root_vertex, a corner of enter face
      int rv = w.root_vertex;
      int G = w.enter_face;
      if (G != F) return false;
      append_point(_out, F, corner_bary(F, rv));
      if (rv == s.source) break;

      int pw = w.root_parent;
      if (pw < 0) return false;
      int G2 = s.windows[pw].enter_face;
      if (G2 != F)
      {
        for (int nf : fan_arc(rv, F, G2))
          append_point(_out, nf, corner_bary(nf, rv));
        F = G2;
      }
      q = face_bary_to_local(F, corner_bary(F, rv));
      wid = pw;
      continue;
    }

    // segment from q toward the parent pseudosource crosses the parent edge
    const Window& wp = s.windows[w.parent];
    int pe = wp.edge;
    int G = wp.enter_face;
    V2 P = edge_point_in_face(pe, F, wp.td, wp.ps_sign * wp.sigma);
    V2 A = edge_point_in_face(pe, F, 0.0, 0.0);
    V2 B = edge_point_in_face(pe, F, edges_[pe].length, 0.0);
    V2 ab = B - A, pq = P - q;
    double den = cross2(pq, ab);
    V2 r;
    if (std::fabs(den) < 1e-18)
      r = q;
    else
    {
      double lp = cross2(A - q, ab) / den;
      r = q + pq * lp;
    }
    append_point(_out, F, local_to_bary(F, r));
    double tpar, hh;
    canonical_coords(pe, F, r, tpar, hh);
    V2 r2 = edge_point_in_face(pe, G, tpar, 0.0);
    append_point(_out, G, local_to_bary(G, r2));
    F = G;
    q = r2;
    wid = w.parent;
    (void)eid; (void)e;
  }

  finish(_out, _length);
  return true;
}

//-----------------------------------------------------------------------------

template <class Mesh>
void ExactGeodesicT<Mesh>::finish(
  std::vector<SurfacePoint>& _out, double& _length) const
{
  _length = 0.0;
  for (std::size_t i = 1; i < _out.size(); ++i)
  {
    const SurfacePoint& a = _out[i - 1];
    const SurfacePoint& b = _out[i];
    int fa = f_local_[a.face.idx()];
    int fb = f_local_[b.face.idx()];
    V3 pa = bary_to_world(fa, a.bary);
    V3 pb = bary_to_world(fb, b.bary);
    _length += norm3(pb - pa);
  }
}

} // namespace Geodesic
} // namespace OpenMesh

#endif // OPENMESH_GEODESIC_EXACT_GEODESIC_T_HH
