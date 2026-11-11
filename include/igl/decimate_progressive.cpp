// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2026 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "decimate_progressive.h"
#include "COLLAPSE_EDGE_NULL.h"
#include "circulation.h"
#include "collapse_edge.h"
#include "edge_flaps.h"
#include "is_edge_manifold.h"
#include "max_faces_stopping_condition.h"
#include "min_heap.h"
#include "remove_unreferenced.h"
#include "shortest_edge_and_midpoint.h"
#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>
#include <unordered_set>

IGL_INLINE bool igl::decimate_progressive(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXd & P,
  const Eigen::VectorXi & L,
  const int max_m,
  ProgressiveMesh & pm)
{
  using namespace igl;
  const int n = (int)V.rows();
  const int m = (int)F.rows();
  const int dim = (int)V.cols();
  const int nattr = (int)P.cols();
  assert((P.size() == 0 || P.rows() == n) &&
    "P must have one attribute row per vertex");
  assert((L.size() == 0 || L.size() == m) &&
    "L must have one label per face");

  // ---- working mesh state: slots are permanent ids and are never compacted
  Eigen::MatrixXd WV = V;
  Eigen::MatrixXi WF = F;
  // Working per-vertex continuous attributes.
  Eigen::MatrixXd WA(n, nattr);
  if(nattr > 0) WA = P;
  // Working per-face integer labels, indexed by permanent face slot.
  Eigen::VectorXi WL(m);
  if(L.size() == m) WL = L; else WL.setZero();

  Eigen::VectorXi EMAP;
  Eigen::MatrixXi E, EF, EI;
  edge_flaps(WF, E, EMAP, EF, EI);
  {
    Eigen::Array<bool,Eigen::Dynamic,Eigen::Dynamic> BF;
    Eigen::Array<bool,Eigen::Dynamic,1> BE;
    if(!is_edge_manifold(WF,E.rows(),EMAP,BF,BE))
    {
      return false;
    }
  }

  // ---- connected components of the input mesh (local adjacency, one pass)
  std::vector<std::vector<int> > VV(n);
  for(int f = 0; f < m; f++)
  {
    for(int c = 0; c < 3; c++)
    {
      const int a = WF(f,c), b = WF(f,(c+1)%3);
      VV[a].push_back(b);
      VV[b].push_back(a);
    }
  }
  Eigen::VectorXi comp(n);
  comp.setConstant(-1);
  int num_components = 0;
  {
    std::vector<int> stack;
    for(int seed = 0; seed < n; seed++)
    {
      if(comp(seed) != -1) continue;
      comp(seed) = num_components;
      stack.push_back(seed);
      while(!stack.empty())
      {
        const int v = stack.back();
        stack.pop_back();
        for(int u : VV[v])
        {
          if(comp(u) == -1)
          {
            comp(u) = num_components;
            stack.push_back(u);
          }
        }
      }
      num_components++;
    }
  }

  // ---- the exact priority-queue machinery used by igl::decimate
  min_heap<std::tuple<double,int,int> > Q;
  Eigen::VectorXi EQ = Eigen::VectorXi::Zero(E.rows());
  Eigen::MatrixXd C(E.rows(), dim);
  for(int e = 0; e < E.rows(); e++)
  {
    double cost = e;
    Eigen::RowVectorXd p(1, dim);
    shortest_edge_and_midpoint(
      e, WV, WF, E, EMAP, EF, EI, cost, p);
    C.row(e) = p;
    Q.emplace(cost, e, 0);
  }

  int face_count = m;
  const auto stopping_condition =
    max_faces_stopping_condition(face_count, m, max_m);

  // A budget at or above the current face count is a genuine no-op: do not
  // collapse anything. (The generic igl::decimate loop only tests the
  // stopping condition after the first collapse; the progressive API keeps
  // that function's behavior untouched and handles the no-op explicitly.)
  bool clean_finish = (m <= max_m);
  int prev_e = -1;
  pm.records.clear();
  // Last successful record that rewrote each (permanent) face slot.
  std::unordered_map<int,int> face_last;
  face_last.reserve(2 * m);
  while(!clean_finish)
  {
    // Pop the freshest least-cost edge, mirroring collapse_least_cost_edge.
    int e = -1;
    {
      std::tuple<double,int,int> top;
      while(true)
      {
        if(Q.empty()) { e = -1; break; }
        top = Q.top();
        if(std::get<0>(top) == std::numeric_limits<double>::infinity())
        {
          e = -1;
          break;
        }
        Q.pop();
        const int cand = std::get<1>(top);
        if(std::get<2>(top) == EQ(cand)) { e = cand; break; }
      }
    }
    if(e == -1) break;

    // Local neighborhood only: no whole-mesh scan.
    std::vector<int> Nsv, Nsf, Ndv, Ndf;
    circulation(e, true,  WF, EMAP, EF, EI, Nsv, Nsf);
    circulation(e, false, WF, EMAP, EF, EI, Ndv, Ndf);

    const int eflip = E(e,0) > E(e,1);
    const int s = eflip ? E(e,1) : E(e,0);
    const int d = eflip ? E(e,0) : E(e,1);
    // Faces incident on d before the collapse (see collapse_edge.cpp).
    const std::vector<int> & d_faces = (!eflip ? Nsf : Ndf);

    // Snapshot everything the collapse is about to destroy.
    const int f1 = EF(e,0), f2 = EF(e,1);
    Eigen::MatrixXi dead_corners(2,3);
    dead_corners.row(0) = WF.row(f1);
    dead_corners.row(1) = WF.row(f2);
    Eigen::VectorXi dead_labels(2);
    dead_labels(0) = WL(f1);
    dead_labels(1) = WL(f2);
    const Eigen::RowVectorXd vd = WV.row(d);
    const Eigen::RowVectorXd vs = WV.row(s);
    // Guarded: taking .row() of a zero-column matrix trips UBSan on Eigen's
    // null base pointer even though no element is ever read.
    const Eigen::RowVectorXd pd = nattr > 0 ? WA.row(d).eval()
                                            : Eigen::RowVectorXd();
    const Eigen::RowVectorXd ps = nattr > 0 ? WA.row(s).eval()
                                            : Eigen::RowVectorXd();
    const int d_component = comp(d);

    int e1, e2, cf1, cf2;
    const bool collapsed = collapse_edge(
      e, C.row(e), Nsv, Nsf, Ndv, Ndf,
      WV, WF, E, EMAP, EF, EI, e1, e2, cf1, cf2);

    if(!collapsed)
    {
      // Failed candidate: no record, no record index. Reinsert as infinite,
      // exactly like collapse_least_cost_edge.
      EQ(e)++;
      Q.emplace(std::numeric_limits<double>::infinity(), e, EQ(e));
      if(prev_e == e)
      {
        assert(false && "Edge collapse no progress... bad stopping condition?");
        break;
      }
      prev_e = e;
      continue;
    }

    // ---- successful collapse: build one local vertex-split record
    ProgressiveSplitRecord rec;
    rec.s = s;
    rec.d = d;
    rec.vs = vs;
    rec.ps = ps;
    rec.vd = vd;
    rec.pd = pd;
    rec.component = d_component;
    rec.dead_face[0] = f1;
    rec.dead_face[1] = f2;
    rec.dead_corners = dead_corners;
    rec.dead_labels = dead_labels;

    std::unordered_set<int> d_face_set;
    d_face_set.reserve(2 * d_faces.size());
    for(int f : d_faces) d_face_set.insert(f);

    // Surviving faces of the post-collapse star of s.
    std::vector<int> star_faces;
    star_faces.reserve(Nsf.size() + Ndf.size());
    for(int f : Nsf) star_faces.push_back(f);
    for(int f : Ndf) star_faces.push_back(f);
    std::sort(star_faces.begin(), star_faces.end());
    star_faces.erase(
      std::unique(star_faces.begin(), star_faces.end()), star_faces.end());

    std::vector<std::array<int,4> > ring_data; // (face id, corner c, a, b)
    std::vector<int> ring_side;
    // Faces this collapse rewrote: the two killed faces plus every reconnected
    // ring face. Dependency bookkeeping touches only this local set.
    std::vector<int> touched;
    touched.push_back(f1);
    touched.push_back(f2);
    for(int f : star_faces)
    {
      if(WF(f,0) == IGL_COLLAPSE_EDGE_NULL &&
         WF(f,1) == IGL_COLLAPSE_EDGE_NULL &&
         WF(f,2) == IGL_COLLAPSE_EDGE_NULL)
      {
        continue;
      }
      touched.push_back(f);
      int c = 0;
      while(c < 3 && WF(f,c) != s) c++;
      assert(c < 3 && "surviving star face must be incident on s");
      ring_data.push_back({f, c, WF(f,(c+1)%3), WF(f,(c+2)%3)});
      ring_side.push_back(d_face_set.count(f) ? 1 : 0);
    }
    rec.ring.resize(ring_data.size(), 4);
    rec.ring_side.resize(ring_data.size());
    for(size_t k = 0; k < ring_data.size(); k++)
    {
      rec.ring((int)k,0) = ring_data[k][0];
      rec.ring((int)k,1) = ring_data[k][1];
      rec.ring((int)k,2) = ring_data[k][2];
      rec.ring((int)k,3) = ring_data[k][3];
      rec.ring_side((int)k) = ring_side[k];
    }

    // Continuous attributes merge by endpoint averaging.
    if(nattr > 0)
    {
      WA.row(s) = 0.5 * (WA.row(s) + WA.row(d));
    }

    // Conservative dependencies: any earlier record that rewrote a face that
    // this collapse also rewrites. Different components can never meet here.
    std::unordered_set<int> dep_set;
    for(int f : touched)
    {
      auto it = face_last.find(f);
      if(it != face_last.end()) dep_set.insert(it->second);
    }
    rec.deps.assign(dep_set.begin(), dep_set.end());
    std::sort(rec.deps.begin(), rec.deps.end());
    const int rec_id = (int)pm.records.size();
    for(int f : touched) face_last[f] = rec_id;
    pm.records.push_back(std::move(rec));

    // Update queue costs in the local two-ring, as decimate does.
    EQ(e) = -1;
    EQ(e1) = -1;
    EQ(e2) = -1;
    std::vector<int> Nf;
    Nf.reserve(Nsf.size() + Ndf.size());
    Nf.insert(Nf.end(), Nsf.begin(), Nsf.end());
    Nf.insert(Nf.end(), Ndf.begin(), Ndf.end());
    std::sort(Nf.begin(), Nf.end());
    Nf.erase(std::unique(Nf.begin(), Nf.end()), Nf.end());
    std::vector<int> Ne;
    Ne.reserve(3 * Nf.size());
    for(int nf : Nf)
    {
      if(WF(nf,0) == IGL_COLLAPSE_EDGE_NULL &&
         WF(nf,1) == IGL_COLLAPSE_EDGE_NULL &&
         WF(nf,2) == IGL_COLLAPSE_EDGE_NULL)
      {
        continue;
      }
      for(int v = 0; v < 3; v++)
      {
        Ne.push_back(EMAP(v * WF.rows() + nf));
      }
    }
    std::sort(Ne.begin(), Ne.end());
    Ne.erase(std::unique(Ne.begin(), Ne.end()), Ne.end());
    for(int ei : Ne)
    {
      double cost;
      Eigen::RowVectorXd place(1, dim);
      shortest_edge_and_midpoint(
        ei, WV, WF, E, EMAP, EF, EI, cost, place);
      EQ(ei)++;
      Q.emplace(cost, ei, EQ(ei));
      C.row(ei) = place;
    }

    if(stopping_condition(
      WV, WF, E, EMAP, EF, EI, Q, EQ, C, e, e1, e2, cf1, cf2))
    {
      clean_finish = true;
      break;
    }
    prev_e = e;
  }

  // ---- compact to the coarse mesh; permanent ids survive via J/I
  Eigen::MatrixXi F2(WF.rows(), 3);
  Eigen::VectorXi J(WF.rows());
  int cm = 0;
  for(int f = 0; f < WF.rows(); f++)
  {
    if(WF(f,0) != IGL_COLLAPSE_EDGE_NULL ||
       WF(f,1) != IGL_COLLAPSE_EDGE_NULL ||
       WF(f,2) != IGL_COLLAPSE_EDGE_NULL)
    {
      F2.row(cm) = WF.row(f);
      J(cm) = f;
      cm++;
    }
  }
  F2.conservativeResize(cm, 3);
  J.conservativeResize(cm);

  Eigen::VectorXi old_to_new, new_to_old;
  Eigen::MatrixXd CV;
  Eigen::MatrixXi CF;
  remove_unreferenced(WV, F2, CV, CF, old_to_new, new_to_old);

  pm.CV = CV;
  pm.CF = CF;
  pm.CI = new_to_old;
  pm.CJ = J;
  pm.CL.resize(cm);
  for(int k = 0; k < cm; k++) pm.CL(k) = WL(J(k));
  pm.CP.resize(CV.rows(), nattr);
  if(nattr > 0)
  {
    for(int k = 0; k < CV.rows(); k++)
    {
      pm.CP.row(k) = WA.row(new_to_old(k));
    }
  }
  pm.num_vertices = n;
  pm.num_faces = m;
  pm.num_components = num_components;
  pm.reached = clean_finish;
  return clean_finish;
}
