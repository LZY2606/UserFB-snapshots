// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "qslim.h"

#include "connect_boundary_to_infinity.h"
#include "decimate.h"
#include "edge_flaps.h"
#include "find.h"
#include "is_edge_manifold.h"
#include "max_faces_stopping_condition.h"
#include "per_vertex_point_to_plane_quadrics.h"
#include "qslim_optimal_collapse_edge_callbacks.h"
#include "placeholders.h"
#include "quadric_binary_plus_operator.h"
#include "remove_unreferenced.h"
#include "block_self_intersections.h"
#include "PlainMatrix.h"
#include "circulation.h"
#include "edge_collapse_is_valid.h"
#include "is_vertex_manifold.h"
#include <Eigen/Cholesky>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <map>
#include <numeric>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

IGL_INLINE bool igl::qslim(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const int max_m,
  const bool block_intersections,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I)
{
  std::vector<decimate_pre_post_collapse_callbacks_decorator> decorators;
  if(block_intersections)
  {
    decorators.push_back(igl::block_self_intersections());
  }
  return qslim(V,F,max_m,decorators,U,G,J,I);
}

IGL_INLINE bool igl::qslim(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const int max_m,
  const std::vector<decimate_pre_post_collapse_callbacks_decorator> & decorators,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I)
{
  using namespace igl;
  // Original number of faces
  const int orig_m = F.rows();
  // Tracking number of faces
  int m = F.rows();
  typedef Eigen::MatrixXd DerivedV;
  typedef Eigen::MatrixXi DerivedF;
  PlainMatrix<DerivedV,Eigen::Dynamic> VO;
  PlainMatrix<DerivedF,Eigen::Dynamic> FO;
  igl::connect_boundary_to_infinity(V,F,VO,FO);
  // decimate will not work correctly on non-edge-manifold meshes. By extension
  // this includes meshes with non-manifold vertices on the boundary since these
  // will create a non-manifold edge when connected to infinity.
  if(!is_edge_manifold(FO))
  {
    return false;
  }
  // These will unfortunately be immediately recomputed in decimate.
  Eigen::VectorXi EMAP;
  Eigen::MatrixXi E,EF,EI;
  edge_flaps(FO,E,EMAP,EF,EI);
  // Quadrics per vertex
  typedef std::tuple<Eigen::MatrixXd,Eigen::RowVectorXd,double> Quadric;
  std::vector<Quadric> quadrics;
  per_vertex_point_to_plane_quadrics(VO,FO,EMAP,EF,EI,quadrics);
  // State variables keeping track of edge we just collapsed
  int v1 = -1;
  int v2 = -1;
  // Callbacks for computing and updating metric
  decimate_cost_and_placement_callback cost_and_placement;
  decimate_pre_collapse_callback       pre_collapse;
  decimate_post_collapse_callback      post_collapse;
  qslim_optimal_collapse_edge_callbacks(
    E,quadrics,v1,v2, cost_and_placement, pre_collapse,post_collapse);
  // Cascade the decorators: each wraps the result of the previous.
  for(const auto & decorator : decorators)
  {
    decorator(VO,FO,orig_m,pre_collapse,post_collapse);
  }
  // Call to greedy decimator
  bool ret = decimate(
    VO, FO,
    cost_and_placement,
    max_faces_stopping_condition(m,orig_m,max_m),
    pre_collapse,
    post_collapse,
    U, G, J, I);
  // Remove phony boundary faces and clean up
  const Eigen::Array<bool,Eigen::Dynamic,1> keep = (J.array()<orig_m);
  const auto keep_i = igl::find(keep);
  G = G(keep_i,igl::placeholders::all).eval();
  J = J(keep_i).eval();
  Eigen::VectorXi _1,I2;
  igl::remove_unreferenced(Eigen::MatrixXd(U),Eigen::MatrixXi(G),U,G,_1,I2);
  I = I(I2).eval();

  return ret;
}

namespace
{
  struct QslimUvQuad5
  {
    Eigen::Matrix<double, 5, 5> A;
    Eigen::Matrix<double, 1, 5> b;
    double c;
    QslimUvQuad5()
    {
      A.setZero();
      b.setZero();
      c = 0.0;
    }
  };

  struct QslimUvGroup
  {
    std::vector<int> roots;
    Eigen::Vector2d uv = Eigen::Vector2d::Zero();
  };

  struct QslimUvCandidate
  {
    bool ok = false;
    double cost = std::numeric_limits<double>::infinity();
    Eigen::Vector3d x = Eigen::Vector3d::Zero();
    std::vector<QslimUvGroup> groups;
    std::vector<int> star;
    std::array<int, 2> f = {{-1, -1}};
    bool seam = false;
    bool seam_vertex_a = false;
    bool seam_vertex_b = false;
  };

  struct QslimUvState
  {
    double uv_weight = 1.0;
    Eigen::MatrixXi FT;
    std::vector<int> parent;
    std::vector<QslimUvQuad5> quadrics;
    std::vector<Eigen::Vector2d> uv;
    std::vector<int> uv_component_parent;
    Eigen::VectorXi face_component;
    std::vector<int> component_faces;
    Eigen::VectorXd uv_orientation;
    std::vector<QslimUvCandidate> candidates;
    int pending_edge = -1;
    QslimUvCandidate pending;

    int root_of(const int x) const
    {
      int r = x;
      while(parent[r] != r) r = parent[r];
      return r;
    }
    int find(const int x)
    {
      int r = root_of(x);
      int y = x;
      while(parent[y] != y)
      {
        const int next = parent[y];
        parent[y] = r;
        y = next;
      }
      return r;
    }
    int component_root_of(const int x) const
    {
      int r = x;
      while(uv_component_parent[r] != r) r = uv_component_parent[r];
      return r;
    }
    int component_find(const int x)
    {
      int r = component_root_of(x);
      int y = x;
      while(uv_component_parent[y] != y)
      {
        const int next = uv_component_parent[y];
        uv_component_parent[y] = r;
        y = next;
      }
      return r;
    }
  };

  static int qslim_uv_corner_for_vertex(
    const Eigen::MatrixXi & F,
    const int f,
    const int v)
  {
    for(int c = 0; c < 3; ++c)
    {
      if(F(f, c) == v) return c;
    }
    return -1;
  }

  static QslimUvQuad5 qslim_uv_plane_quadric(
    const Eigen::Matrix<double,1,5> & p,
    const Eigen::Matrix<double,1,5> & q,
    const Eigen::Matrix<double,1,5> & r)
  {
    Eigen::Matrix<double,1,5> pq = q - p;
    Eigen::Matrix<double,1,5> pr = r - p;
    const double gram =
      pq.squaredNorm() * pr.squaredNorm() - std::pow(pr.dot(pq), 2.0);
    const double area = std::sqrt(std::max(0.0, gram));
    Eigen::Matrix<double,1,5> e1 = pq.normalized();
    Eigen::Matrix<double,1,5> e2 = pr - e1.dot(pr) * e1;
    e2.normalize();

    Eigen::Matrix<double, 5, 5> I5 = Eigen::Matrix<double, 5, 5>::Identity();
    QslimUvQuad5 out;
    out.A = I5;
    out.b = -p;
    out.c = p.dot(p);
    for(const Eigen::Matrix<double,1,5> & e : {e1, e2})
    {
      out.A -= e.transpose() * e;
      out.b += p.dot(e) * e;
      out.c -= std::pow(p.dot(e), 2.0);
    }
    out.A *= area;
    out.b *= area;
    out.c *= area;
    return out;
  }

  static QslimUvQuad5 qslim_uv_point_quadric(
    const Eigen::Matrix<double,1,5> & p,
    const double weight)
  {
    QslimUvQuad5 out;
    out.A = weight * Eigen::Matrix<double, 5, 5>::Identity();
    out.b = -weight * p;
    out.c = weight * p.dot(p);
    return out;
  }

  static QslimUvQuad5 qslim_uv_add_quadric(
    const QslimUvQuad5 & a,
    const QslimUvQuad5 & b)
  {
    QslimUvQuad5 out;
    out.A = a.A + b.A;
    out.b = a.b + b.b;
    out.c = a.c + b.c;
    return out;
  }

  static bool qslim_uv_is_seam_edge(
    QslimUvState & state,
    const int ei,
    const Eigen::MatrixXi & F,
    const Eigen::MatrixXi & E,
    const Eigen::MatrixXi & EF)
  {
    if(ei < 0 || ei >= E.rows()) return false;
    const int f0 = EF(ei, 0);
    const int f1 = EF(ei, 1);
    if(f0 < 0 || f1 < 0 || f0 >= F.rows() || f1 >= F.rows()) return false;
    for(int endpoint = 0; endpoint < 2; ++endpoint)
    {
      const int v = E(ei, endpoint);
      const int c0 = qslim_uv_corner_for_vertex(F, f0, v);
      const int c1 = qslim_uv_corner_for_vertex(F, f1, v);
      if(c0 < 0 || c1 < 0) return true;
      const int w0 = state.root_of(state.FT(f0, c0));
      const int w1 = state.root_of(state.FT(f1, c1));
      if(w0 != w1) return true;
    }
    return false;
  }

  static QslimUvCandidate qslim_uv_make_candidate(
    QslimUvState & state,
    const int e,
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const Eigen::MatrixXi & E,
    const Eigen::VectorXi & EMAP,
    const Eigen::MatrixXi & EF,
    const Eigen::MatrixXi & EI)
  {
    QslimUvCandidate cand;
    if(e < 0 || e >= E.rows()) return cand;
    const int a = E(e, 0);
    const int b = E(e, 1);
    if(a < 0 || b < 0 || a >= V.rows() || b >= V.rows()) return cand;

    std::vector<int> Nsv, Nsf, Ndv, Ndf;
    igl::circulation(e, true, F, EMAP, EF, EI, Nsv, Nsf);
    igl::circulation(e, false, F, EMAP, EF, EI, Ndv, Ndf);
    cand.star = Nsf;
    cand.star.insert(cand.star.end(), Ndf.begin(), Ndf.end());
    std::sort(cand.star.begin(), cand.star.end());
    cand.star.erase(std::unique(cand.star.begin(), cand.star.end()), cand.star.end());

    std::set<int> roots_a, roots_b, roots_all;
    for(const int f : cand.star)
    {
      if(f < 0 || f >= F.rows()) continue;
      for(int c = 0; c < 3; ++c)
      {
        const int v = F(f, c);
        const int r = state.root_of(state.FT(f, c));
        if(v == a) roots_a.insert(r);
        if(v == b) roots_b.insert(r);
        if(v == a || v == b) roots_all.insert(r);
      }
    }
    cand.seam_vertex_a = roots_a.size() > 1;
    cand.seam_vertex_b = roots_b.size() > 1;

    std::map<int, int> root_to_group;
    for(const int r : roots_all)
    {
      root_to_group[r] = (int)cand.groups.size();
      QslimUvGroup g;
      g.roots.push_back(r);
      cand.groups.push_back(std::move(g));
    }
    if(cand.groups.empty()) return cand;

    const int f0 = EF(e, 0);
    const int f1 = EF(e, 1);
    cand.f = {{f0, f1}};
    const int ca0 = qslim_uv_corner_for_vertex(F, f0, a);
    const int cb0 = qslim_uv_corner_for_vertex(F, f0, b);
    const int ca1 = qslim_uv_corner_for_vertex(F, f1, a);
    const int cb1 = qslim_uv_corner_for_vertex(F, f1, b);
    if(ca0 < 0 || cb0 < 0 || ca1 < 0 || cb1 < 0) return cand;

    const int ra0 = state.root_of(state.FT(f0, ca0));
    const int rb0 = state.root_of(state.FT(f0, cb0));
    const int ra1 = state.root_of(state.FT(f1, ca1));
    const int rb1 = state.root_of(state.FT(f1, cb1));
    cand.seam = (ra0 != ra1 || rb0 != rb1);

    const auto unite_roots = [&](int r1, int r2)
    {
      if(r1 == r2) return;
      int g1 = root_to_group[r1];
      int g2 = root_to_group[r2];
      if(g1 == g2) return;
      const int keep = g1;
      const int kill = g2;
      cand.groups[keep].roots.insert(
        cand.groups[keep].roots.end(),
        cand.groups[kill].roots.begin(),
        cand.groups[kill].roots.end());
      for(const int r : cand.groups[kill].roots) root_to_group[r] = keep;
      cand.groups[kill].roots.clear();
    };
    unite_roots(ra0, rb0);
    unite_roots(ra1, rb1);

    std::vector<QslimUvGroup> active;
    for(auto & g : cand.groups)
    {
      if(!g.roots.empty()) active.push_back(std::move(g));
    }
    cand.groups = std::move(active);
    const int G = (int)cand.groups.size();
    const int n = 3 + 2 * G;
    Eigen::MatrixXd H = Eigen::MatrixXd::Zero(n, n);
    Eigen::VectorXd d = Eigen::VectorXd::Zero(n);
    double c = 0.0;
    const double wuv = state.uv_weight;

    for(int g = 0; g < G; ++g)
    {
      QslimUvQuad5 q;
      for(const int r : cand.groups[g].roots)
      {
        q = qslim_uv_add_quadric(q, state.quadrics[r]);
      }
      c += q.c;
      H.block<3, 3>(0, 0) += q.A.block<3, 3>(0, 0);
      const Eigen::Matrix<double, 3, 2> Axu = q.A.block<3, 2>(0, 3);
      H.block<3, 2>(0, 3 + 2 * g) += wuv * Axu;
      H.block<2, 3>(3 + 2 * g, 0) += wuv * q.A.block<2, 3>(3, 0);
      H.block<2, 2>(3 + 2 * g, 3 + 2 * g) +=
        wuv * wuv * q.A.block<2, 2>(3, 3);
      d.head<3>() += q.b.head<3>().transpose();
      d.segment<2>(3 + 2 * g) += wuv * q.b.segment<2>(3).transpose();
    }
    H = 0.5 * (H + H.transpose()).eval();
    Eigen::LDLT<Eigen::MatrixXd> ldlt(H);
    if(ldlt.info() != Eigen::Success) return cand;
    Eigen::VectorXd z = ldlt.solve(-d);
    if(ldlt.info() != Eigen::Success || !z.allFinite()) return cand;

    cand.x = z.head<3>();
    for(int g = 0; g < G; ++g)
    {
      cand.groups[g].uv = z.segment<2>(3 + 2 * g);
    }
    cand.cost = z.dot(H * z) + 2.0 * d.dot(z) + c;
    if(!std::isfinite(cand.cost))
    {
      cand.cost = std::numeric_limits<double>::infinity();
      return cand;
    }
    cand.ok = cand.x.allFinite();
    for(const auto & g : cand.groups) cand.ok = cand.ok && g.uv.allFinite();
    return cand;
  }

  static bool qslim_uv_candidate_admissible(
    QslimUvState & state,
    const QslimUvCandidate & cand,
    const int e,
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const Eigen::MatrixXi & E,
    const Eigen::VectorXi & EMAP,
    const Eigen::MatrixXi & EF,
    const Eigen::MatrixXi & EI,
    const Eigen::VectorXi & EQ)
  {
    if(!cand.ok || e < 0 || e >= E.rows()) return false;
    if(!igl::edge_collapse_is_valid(e, F, E, EMAP, EF, EI)) return false;

    const int a = E(e, 0);
    const int b = E(e, 1);
    if(!cand.seam && cand.seam_vertex_a && cand.seam_vertex_b) return false;

    if(cand.seam)
    {
      const auto other_seam_neighbor = [&](const int v) -> int
      {
        std::set<int> others;
        for(const int f : cand.star)
        {
          if(f < 0 || f >= F.rows()) continue;
          for(int k = 0; k < 3; ++k)
          {
            const int ei = EMAP(f + F.rows() * k);
            if(ei < 0 || ei >= E.rows() || ei == e) continue;
            if(EQ.size() == E.rows() && EQ(ei) == -1) continue;
            if(!(E(ei, 0) == v || E(ei, 1) == v)) continue;
            if(qslim_uv_is_seam_edge(state, ei, F, E, EF))
            {
              others.insert(E(ei, 0) == v ? E(ei, 1) : E(ei, 0));
            }
          }
        }
        return others.size() == 1 ? *others.begin() : -2;
      };
      const int na = other_seam_neighbor(a);
      const int nb = other_seam_neighbor(b);
      if(na < 0 || nb < 0) return false;
      // A three-edge simple seam loop would become a two-edge loop.
      if(na == nb) return false;
    }

    std::map<int, int> root_to_group;
    for(int g = 0; g < (int)cand.groups.size(); ++g)
    {
      for(const int r : cand.groups[g].roots) root_to_group[r] = g;
    }
    const int sim_base = -1000000;
    const auto sim_id = [&](const int f, const int c) -> int
    {
      const int r = state.root_of(state.FT(f, c));
      auto it = root_to_group.find(r);
      return it == root_to_group.end() ? r : sim_base - it->second;
    };
    const auto uv_for_corner = [&](const int f, const int c) -> Eigen::Vector2d
    {
      const int r = state.root_of(state.FT(f, c));
      auto it = root_to_group.find(r);
      if(it != root_to_group.end()) return cand.groups[it->second].uv;
      return state.uv[r];
    };

    std::map<int, int> removed_components;
    for(const int f : cand.f)
    {
      if(f >= 0 && f < state.face_component.size())
      {
        ++removed_components[state.face_component(f)];
      }
    }
    for(const auto & [component, removed] : removed_components)
    {
      if(component < 0 || component >= (int)state.component_faces.size()) return false;
      if(state.component_faces[component] <= removed) return false;
    }

    std::vector<std::array<int, 3> > sim_tris;
    for(const int f : cand.star)
    {
      if(f < 0 || f >= F.rows()) continue;
      if(f == cand.f[0] || f == cand.f[1]) continue;
      if(F(f, 0) == IGL_COLLAPSE_EDGE_NULL &&
         F(f, 1) == IGL_COLLAPSE_EDGE_NULL &&
         F(f, 2) == IGL_COLLAPSE_EDGE_NULL) continue;

      const Eigen::Vector3d old_p0 = V.row(F(f, 0)).head<3>();
      const Eigen::Vector3d old_p1 = V.row(F(f, 1)).head<3>();
      const Eigen::Vector3d old_p2 = V.row(F(f, 2)).head<3>();
      Eigen::Vector3d old_normal = (old_p1 - old_p0).cross(old_p2 - old_p0);
      Eigen::Vector3d np[3];
      for(int c = 0; c < 3; ++c)
      {
        const int v = F(f, c);
        if(v == a || v == b) np[c] = cand.x;
        else np[c] = V.row(v).head<3>();
      }
      const Eigen::Vector3d new_normal = (np[1] - np[0]).cross(np[2] - np[0]);
      if(old_normal.dot(new_normal) <= 0.0) return false;

      Eigen::Vector2d uv[3];
      int ids[3];
      for(int c = 0; c < 3; ++c)
      {
        uv[c] = uv_for_corner(f, c);
        ids[c] = sim_id(f, c);
      }
      const double uv_area =
        (uv[1] - uv[0]).x() * (uv[2] - uv[0]).y() -
        (uv[1] - uv[0]).y() * (uv[2] - uv[0]).x();
      if(uv_area * state.uv_orientation(f) <= 0.0) return false;
      if(ids[0] == ids[1] || ids[1] == ids[2] || ids[2] == ids[0]) return false;
      sim_tris.push_back({ids[0], ids[1], ids[2]});
    }

    std::map<std::pair<int, int>, int> edge_count;
    for(const auto & t : sim_tris)
    {
      for(int k = 0; k < 3; ++k)
      {
        int x = t[k], y = t[(k + 1) % 3];
        if(x > y) std::swap(x, y);
        ++edge_count[{x, y}];
      }
    }
    for(const auto & [edge, count] : edge_count)
    {
      if((edge.first < 0 || edge.second < 0) && count > 2) return false;
    }

    for(int g = 0; g < (int)cand.groups.size(); ++g)
    {
      const int wedge_id = sim_base - g;
      std::map<std::pair<int, int>, int> link_edges;
      std::map<int, int> degree;
      int containing = 0;
      for(const auto & t : sim_tris)
      {
        int k = -1;
        for(int q = 0; q < 3; ++q) if(t[q] == wedge_id) k = q;
        if(k < 0) continue;
        ++containing;
        const int x = t[(k + 1) % 3];
        const int y = t[(k + 2) % 3];
        if(x == y) return false;
        std::pair<int, int> le = {std::min(x, y), std::max(x, y)};
        if(++link_edges[le] > 1) return false;
        ++degree[x];
        ++degree[y];
      }
      if(containing == 0 || degree.empty()) return false;
      int degree_one = 0;
      std::map<int, std::vector<int> > link_adj;
      for(const auto & [le, count] : link_edges)
      {
        if(count != 1) return false;
        if(degree[le.first] > 2 || degree[le.second] > 2) return false;
        degree_one += (degree[le.first] == 1);
        degree_one += (degree[le.second] == 1);
        link_adj[le.first].push_back(le.second);
        link_adj[le.second].push_back(le.first);
      }
      if(degree_one != 0 && degree_one != 2) return false;
      std::set<int> seen;
      std::vector<int> stack = {link_adj.begin()->first};
      while(!stack.empty())
      {
        const int x = stack.back();
        stack.pop_back();
        if(seen.count(x)) continue;
        seen.insert(x);
        for(const int y : link_adj[x]) if(!seen.count(y)) stack.push_back(y);
      }
      if((int)seen.size() != (int)degree.size()) return false;
    }
    return true;
  }

  static void qslim_uv_validate_input(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const Eigen::MatrixXd & T,
    const Eigen::MatrixXi & FT,
    const int max_m,
    const double uv_weight)
  {
    const std::string bad = "qslim_with_uv: invalid input";
    if(F.rows() < 1 || F.cols() != 3) throw std::invalid_argument(bad);
    if(V.cols() != 3 || V.rows() < F.maxCoeff() + 1) throw std::invalid_argument(bad);
    if(T.cols() != 2 || FT.rows() != F.rows() || FT.cols() != 3)
      throw std::invalid_argument(bad);
    if(max_m < 0 || !std::isfinite(uv_weight) || uv_weight <= 0.0)
      throw std::invalid_argument(bad);
    if(!V.allFinite() || !T.allFinite()) throw std::invalid_argument(bad);

    for(int f = 0; f < F.rows(); ++f)
    {
      for(int c = 0; c < 3; ++c)
      {
        if(F(f, c) < 0 || F(f, c) >= V.rows()) throw std::invalid_argument(bad);
        if(FT(f, c) < 0 || FT(f, c) >= T.rows()) throw std::invalid_argument(bad);
      }
      if(F(f, 0) == F(f, 1) || F(f, 1) == F(f, 2) || F(f, 2) == F(f, 0))
        throw std::invalid_argument(bad);
      const Eigen::Vector3d qv0 = V.row(F(f, 0)).head<3>();
      const Eigen::Vector3d qv1 = V.row(F(f, 1)).head<3>();
      const Eigen::Vector3d qv2 = V.row(F(f, 2)).head<3>();
      const Eigen::Vector3d n = (qv1 - qv0).cross(qv2 - qv0);
      if(n.squaredNorm() <= 0.0) throw std::invalid_argument(bad);
      const double ua =
        (T.row(FT(f, 1)) - T.row(FT(f, 0))).x() *
          (T.row(FT(f, 2)) - T.row(FT(f, 0))).y() -
        (T.row(FT(f, 1)) - T.row(FT(f, 0))).y() *
          (T.row(FT(f, 2)) - T.row(FT(f, 0))).x();
      if(ua == 0.0 || !std::isfinite(ua)) throw std::invalid_argument(bad);
    }

    struct Occ { int f; int a; int b; };
    std::map<std::pair<int, int>, std::vector<Occ> > edge_map;
    for(int f = 0; f < F.rows(); ++f)
    {
      for(int c = 0; c < 3; ++c)
      {
        const int a = F(f, c);
        const int b = F(f, (c + 1) % 3);
        edge_map[{std::min(a, b), std::max(a, b)}].push_back({f, a, b});
      }
    }
    struct SeamEdge { int a, b; };
    std::vector<SeamEdge> seam_edges;
    std::vector<std::vector<std::pair<int, int> > > seam_adj(V.rows());
    for(auto & entry : edge_map)
    {
      auto & occ = entry.second;
      if(occ.size() != 2) throw std::invalid_argument(bad);
      // Consistently oriented closed faces emit opposite directed edges.
      const bool opposite =
        (occ[0].a == occ[1].b && occ[0].b == occ[1].a);
      if(!opposite) throw std::invalid_argument(bad);

      const int a = entry.first.first;
      const int b = entry.first.second;
      const int f0 = occ[0].f;
      const int f1 = occ[1].f;
      bool seam = false;
      for(const int v : {a, b})
      {
        const int c0 = qslim_uv_corner_for_vertex(F, f0, v);
        const int c1 = qslim_uv_corner_for_vertex(F, f1, v);
        if(c0 < 0 || c1 < 0) throw std::invalid_argument(bad);
        if(FT(f0, c0) != FT(f1, c1)) seam = true;
      }
      if(seam)
      {
        const int id = (int)seam_edges.size();
        seam_edges.push_back({a, b});
        seam_adj[a].push_back({b, id});
        seam_adj[b].push_back({a, id});
      }
    }

    Eigen::MatrixXd Vc;
    Eigen::MatrixXi Fc;
    Eigen::VectorXi old_to_new, birth_v;
    igl::remove_unreferenced(V, F, Vc, Fc, old_to_new, birth_v);
    if(!igl::is_vertex_manifold(Fc)) throw std::invalid_argument(bad);

    Eigen::VectorXi uv_geom = Eigen::VectorXi::Constant(T.rows(), -1);
    std::vector<std::vector<int> > uv_faces(T.rows());
    std::vector<char> uv_referenced(T.rows(), false);
    for(int f = 0; f < F.rows(); ++f)
    {
      for(int c = 0; c < 3; ++c)
      {
        const int t = FT(f, c);
        const int v = F(f, c);
        uv_referenced[t] = true;
        if(uv_geom(t) == -1) uv_geom(t) = v;
        if(uv_geom(t) != v) throw std::invalid_argument(bad);
        uv_faces[t].push_back(f);
      }
    }
    for(int t = 0; t < T.rows(); ++t)
    {
      if(!uv_referenced[t]) continue;
      std::sort(uv_faces[t].begin(), uv_faces[t].end());
      uv_faces[t].erase(std::unique(uv_faces[t].begin(), uv_faces[t].end()),
                       uv_faces[t].end());
      std::set<int> face_set(uv_faces[t].begin(), uv_faces[t].end());
      std::set<int> seen;
      std::vector<int> stack = {uv_faces[t][0]};
      while(!stack.empty())
      {
        const int f = stack.back();
        stack.pop_back();
        if(seen.count(f)) continue;
        seen.insert(f);
        for(int c = 0; c < 3; ++c)
        {
          const int a = F(f, c);
          const int b = F(f, (c + 1) % 3);
          auto it = edge_map.find({std::min(a, b), std::max(a, b)});
          if(it == edge_map.end()) continue;
          for(const Occ & o : it->second)
          {
            if(face_set.count(o.f) && !seen.count(o.f)) stack.push_back(o.f);
          }
        }
      }
      if(seen.size() != face_set.size()) throw std::invalid_argument(bad);
    }

    for(int v = 0; v < V.rows(); ++v)
    {
      if(seam_adj[v].empty()) continue;
      std::sort(seam_adj[v].begin(), seam_adj[v].end());
      seam_adj[v].erase(std::unique(seam_adj[v].begin(), seam_adj[v].end()),
                       seam_adj[v].end());
      if(seam_adj[v].size() != 2) throw std::invalid_argument(bad);
    }
    std::vector<char> seam_seen(seam_edges.size(), false);
    for(int start_id = 0; start_id < (int)seam_edges.size(); ++start_id)
    {
      if(seam_seen[start_id]) continue;
      const int start_v = seam_edges[start_id].a;
      int current_v = start_v;
      int edge_id = start_id;
      int edge_count = 0;
      std::set<int> vertices_seen;
      vertices_seen.insert(start_v);
      while(true)
      {
        if(edge_id < 0 || edge_id >= (int)seam_edges.size() || seam_seen[edge_id])
          throw std::invalid_argument(bad);
        seam_seen[edge_id] = true;
        ++edge_count;
        const SeamEdge & se = seam_edges[edge_id];
        const int next_v = se.a == current_v ? se.b : se.a;
        if(next_v == current_v) throw std::invalid_argument(bad);
        if(next_v == start_v) break;
        if(vertices_seen.count(next_v)) throw std::invalid_argument(bad);
        vertices_seen.insert(next_v);
        if(seam_adj[next_v].size() != 2) throw std::invalid_argument(bad);
        int next_edge = -1;
        for(const auto & [neighbor, id] : seam_adj[next_v])
        {
          if(id != edge_id)
          {
            next_edge = id;
            break;
          }
        }
        if(next_edge < 0) throw std::invalid_argument(bad);
        current_v = next_v;
        edge_id = next_edge;
        if(edge_count > (int)seam_edges.size() + 1)
          throw std::invalid_argument(bad);
      }
      if(edge_count < 3) throw std::invalid_argument(bad);
    }
  }
}

IGL_INLINE bool igl::qslim_with_uv(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXd & T,
  const Eigen::MatrixXi & FT,
  const int max_m,
  const double uv_weight,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::MatrixXd & S,
  Eigen::MatrixXi & FS,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I)
{
  qslim_uv_validate_input(V, F, T, FT, max_m, uv_weight);

  QslimUvState state;
  state.uv_weight = uv_weight;
  state.FT = FT;
  const int nt = T.rows();
  state.parent.resize(nt);
  std::iota(state.parent.begin(), state.parent.end(), 0);
  state.uv_component_parent.resize(nt);
  std::iota(state.uv_component_parent.begin(), state.uv_component_parent.end(), 0);
  state.quadrics.resize(nt);
  state.uv.resize(nt);
  for(int t = 0; t < nt; ++t) state.uv[t] = T.row(t).transpose();

  const double eps = 1e-10;
  std::vector<char> uv_referenced(nt, false);
  for(int f = 0; f < F.rows(); ++f)
  {
    for(int c = 0; c < 3; ++c) uv_referenced[FT(f, c)] = true;
  }
  for(int t = 0; t < nt; ++t)
  {
    if(!uv_referenced[t]) continue;
    const int f0 = [&]()
    {
      for(int f = 0; f < F.rows(); ++f)
        for(int c = 0; c < 3; ++c)
          if(FT(f, c) == t) return f;
      return -1;
    }();
    const int c0 = [&]()
    {
      for(int f = 0; f < F.rows(); ++f)
        for(int c = 0; c < 3; ++c)
          if(FT(f, c) == t) return c;
      return -1;
    }();
    Eigen::Matrix<double,1,5> p;
    p << V.row(F(f0, c0)), uv_weight * T.row(t);
    state.quadrics[t] = qslim_uv_point_quadric(p, eps);
  }

  for(int f = 0; f < F.rows(); ++f)
  {
    Eigen::Matrix<double,1,5> p[3];
    for(int c = 0; c < 3; ++c)
    {
      p[c] << V.row(F(f, c)), uv_weight * T.row(FT(f, c));
    }
    const QslimUvQuad5 qf = qslim_uv_plane_quadric(p[0], p[1], p[2]);
    for(int c = 0; c < 3; ++c)
    {
      const int t = FT(f, c);
      state.quadrics[t] = qslim_uv_add_quadric(state.quadrics[t], qf);
    }
    for(int c = 0; c < 3; ++c)
    {
      const int t0 = FT(f, c);
      const int t1 = FT(f, (c + 1) % 3);
      int r0 = state.component_find(t0);
      int r1 = state.component_find(t1);
      if(r0 != r1) state.uv_component_parent[r1] = r0;
    }
  }

  state.face_component.resize(F.rows());
  state.component_faces.assign(nt, 0);
  state.uv_orientation.resize(F.rows());
  for(int f = 0; f < F.rows(); ++f)
  {
    state.face_component(f) = state.component_find(FT(f, 0));
    ++state.component_faces[state.face_component(f)];
    const double a =
      (T.row(FT(f, 1)) - T.row(FT(f, 0))).x() *
        (T.row(FT(f, 2)) - T.row(FT(f, 0))).y() -
      (T.row(FT(f, 1)) - T.row(FT(f, 0))).y() *
        (T.row(FT(f, 2)) - T.row(FT(f, 0))).x();
    state.uv_orientation(f) = a > 0.0 ? 1.0 : -1.0;
  }

  state.candidates.assign(3 * F.rows() + 10, QslimUvCandidate());
  decimate_cost_and_placement_callback real_cost_cb =
    [&state](
      const int e,
      const Eigen::MatrixXd & curV,
      const Eigen::MatrixXi & curF,
      const Eigen::MatrixXi & E,
      const Eigen::VectorXi & EMAP,
      const Eigen::MatrixXi & EF,
      const Eigen::MatrixXi & EI,
      double & cost,
      Eigen::RowVectorXd & p)
  {
    QslimUvCandidate cand = qslim_uv_make_candidate(
      state, e, curV, curF, E, EMAP, EF, EI);
    if(e >= 0 && e < (int)state.candidates.size()) state.candidates[e] = cand;
    cost = cand.ok ? cand.cost : std::numeric_limits<double>::infinity();
    p.resize(1, 3);
    if(cand.ok) p = cand.x.transpose();
    else p.setZero();
    if(!std::isfinite(cost))
    {
      cost = std::numeric_limits<double>::infinity();
      p.setZero();
    }
  };

  decimate_pre_collapse_callback pre_cb =
    [&state](
      const Eigen::MatrixXd & curV,
      const Eigen::MatrixXi & curF,
      const Eigen::MatrixXi & E,
      const Eigen::VectorXi & EMAP,
      const Eigen::MatrixXi & EF,
      const Eigen::MatrixXi & EI,
      const igl::min_heap<std::tuple<double, int, int> > &,
      const Eigen::VectorXi & EQ,
      const Eigen::MatrixXd & C,
      const int e) -> bool
  {
    state.pending_edge = e;
    state.pending = QslimUvCandidate();
    if(e < 0 || e >= (int)state.candidates.size()) return false;
    QslimUvCandidate cand = state.candidates[e];
    if(!cand.ok || e >= C.rows()) return false;
    if((C.row(e).transpose() - cand.x).cwiseAbs().maxCoeff() != 0.0) return false;
    const bool admissible = qslim_uv_candidate_admissible(
         state, cand, e, curV, curF, E, EMAP, EF, EI, EQ);
    if(!admissible)
    {
      return false;
    }
    state.pending = std::move(cand);
    return true;
  };

  decimate_post_collapse_callback post_cb =
    [&state](
      const Eigen::MatrixXd &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const Eigen::VectorXi &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const igl::min_heap<std::tuple<double, int, int> > &,
      const Eigen::VectorXi &,
      const Eigen::MatrixXd &,
      const int e,
      const int,
      const int,
      const int f1,
      const int f2,
      const bool collapsed)
  {
    if(!collapsed || state.pending_edge != e || !state.pending.ok)
    {
      state.pending_edge = -1;
      state.pending = QslimUvCandidate();
      return;
    }
    for(const QslimUvGroup & group : state.pending.groups)
    {
      if(group.roots.empty()) continue;
      const int base = group.roots.front();
      QslimUvQuad5 q;
      for(const int r : group.roots)
      {
        q = qslim_uv_add_quadric(q, state.quadrics[state.root_of(r)]);
      }
      state.quadrics[base] = q;
      state.uv[base] = group.uv;
      state.parent[base] = base;
      for(size_t i = 1; i < group.roots.size(); ++i)
      {
        const int r = state.root_of(group.roots[i]);
        if(r != base) state.parent[r] = base;
      }
    }
    for(const int f : {f1, f2})
    {
      if(f >= 0 && f < state.face_component.size())
      {
        const int c = state.face_component(f);
        if(c >= 0 && c < (int)state.component_faces.size())
          --state.component_faces[c];
      }
    }
    state.pending_edge = -1;
    state.pending = QslimUvCandidate();
  };

  Eigen::MatrixXd geoU;
  Eigen::MatrixXi geoG;
  Eigen::VectorXi faceJ;
  Eigen::VectorXi vertI;
  bool reached = true;
  if(F.rows() > max_m)
  {
    int m = F.rows();
    reached = igl::decimate(
      V, F,
      real_cost_cb,
      igl::max_faces_stopping_condition(m, F.rows(), max_m),
      pre_cb,
      post_cb,
      geoU, geoG, faceJ, vertI);
  }
  else
  {
    Eigen::VectorXi old_to_new;
    igl::remove_unreferenced(V, F, geoU, geoG, old_to_new, vertI);
    faceJ.resize(F.rows());
    std::iota(faceJ.data(), faceJ.data() + F.rows(), 0);
  }

  FS.resize(geoG.rows(), 3);
  std::vector<int> compact_uv;
  Eigen::VectorXi uv_to_compact = Eigen::VectorXi::Constant(nt, -1);
  for(int k = 0; k < geoG.rows(); ++k)
  {
    const int f = faceJ(k);
    for(int c = 0; c < 3; ++c)
    {
      const int root = state.root_of(FT(f, c));
      if(uv_to_compact(root) == -1)
      {
        uv_to_compact(root) = (int)compact_uv.size();
        compact_uv.push_back(root);
      }
      FS(k, c) = uv_to_compact(root);
    }
  }
  S.resize((int)compact_uv.size(), 2);
  for(int i = 0; i < (int)compact_uv.size(); ++i)
  {
    S.row(i) = state.uv[compact_uv[i]].transpose();
  }
  U = std::move(geoU);
  G = std::move(geoG);
  J = std::move(faceJ);
  I = std::move(vertI);
  return reached;
}
