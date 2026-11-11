// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2026 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "refine_progressive.h"
#include <algorithm>
#include <cassert>
#include <vector>

IGL_INLINE bool igl::refine_progressive(
  const ProgressiveMesh & pm,
  const std::vector<int> & requested,
  Eigen::MatrixXd & RV,
  Eigen::MatrixXi & RF,
  Eigen::MatrixXd & RP,
  Eigen::VectorXi & RJ,
  Eigen::VectorXi & RI,
  Eigen::VectorXi & RL)
{
  const int R = (int)pm.records.size();
  const int n = pm.num_vertices;
  const int m = pm.num_faces;
  const int dim = (int)pm.CV.cols();
  const int nattr = (int)pm.CP.cols();

  // Reject illegal indices before touching any output.
  for(int q : requested)
  {
    if(q < 0 || q >= R) return false;
  }

  // ---- collapse forest: parent[v] is the survivor that absorbed v. Built
  // purely from the local s/d links of the records, no mesh scan.
  std::vector<int> parent(n, -1), detach_record(n, -1);
  for(int r = 0; r < R; r++)
  {
    const int s = pm.records[r].s, d = pm.records[r].d;
    detach_record[d] = r;
    parent[d] = s;
  }

  // ---- dependency closure. Splits are applied in descending record order,
  // so the conservative backward face-star edges (ProgressiveSplitRecord::
  // deps) are honored automatically: a finer dependent split runs after the
  // coarser one and wins every contested corner/value. The edges that must be
  // force-completed are _legality_ edges: a face can only be restored at its
  // original resolution once every later collapse that absorbed one of its
  // corners is undone. Otherwise two originally distinct edges could have
  // identified and the face would be reborn degenerate or with a third flap.
  // The walk only follows the record's own stored corner ids and the local
  // collapse forest, so it never leaves the operation's vertex star and can
  // never cross a connected component (forest roots are component-local).
  std::vector<char> selected(R, 0);
  // anc_closed[v]: every record on v's path to its forest root has already
  // been selected. Each forest edge is therefore traversed at most once, so
  // the whole closure is linear in the number of records plus vertices.
  std::vector<char> anc_closed(n, 0);
  std::vector<int> stack;
  stack.reserve(requested.size());
  const auto select = [&](int q)
  {
    if(q >= 0 && !selected[q])
    {
      selected[q] = 1;
      stack.push_back(q);
    }
  };
  for(int q : requested) select(q);
  while(!stack.empty())
  {
    const int r = stack.back();
    stack.pop_back();
    const ProgressiveSplitRecord & rec = pm.records[r];
    const auto close_corners = [&](int x, int y, int z)
    {
      const int tri[3] = {x,y,z};
      for(int v0 : tri)
      {
        int v = v0;
        while(v != -1 && !anc_closed[v])
        {
          anc_closed[v] = 1;
          if(parent[v] != -1) select(detach_record[v]); // later than r
          v = parent[v];
        }
      }
    };
    close_corners(rec.dead_corners(0,0), rec.dead_corners(0,1),
      rec.dead_corners(0,2));
    close_corners(rec.dead_corners(1,0), rec.dead_corners(1,1),
      rec.dead_corners(1,2));
    for(int k = 0; k < rec.ring.rows(); k++)
    {
      close_corners(
        rec.ring_side(k) ? rec.d : rec.s,
        rec.ring(k,2), rec.ring(k,3));
    }
  }

  // ---- working state in permanent-id slots. The coarse mesh is copied
  // exactly once.
  Eigen::MatrixXd V(n, dim);
  Eigen::MatrixXd A(n, nattr);
  Eigen::MatrixXi F(m, 3);
  F.setConstant(-1);
  Eigen::VectorXi labels(m);
  std::vector<char> alive_v(n, 0), alive_f(m, 0);

  for(int k = 0; k < (int)pm.CI.size(); k++)
  {
    const int v = pm.CI(k);
    alive_v[v] = 1;
    V.row(v) = pm.CV.row(k);
    if(nattr > 0) A.row(v) = pm.CP.row(k);
  }
  for(int k = 0; k < (int)pm.CJ.size(); k++)
  {
    const int f = pm.CJ(k);
    alive_f[f] = 1;
    // CF indexes compact coarse rows; translate to permanent vertex slots.
    for(int c = 0; c < 3; c++) F(f,c) = pm.CI(pm.CF(k,c));
    labels(f) = pm.CL(k);
  }

  // Current representative of a permanent vertex under the collapses that
  // remain in effect: walk parent links to the nearest live ancestor.
  const auto resolve = [&](int v)
  {
    int x = v;
    while(!alive_v[x])
    {
      x = parent[x];
      assert(x != -1 && "live face references a vertex with no live root");
    }
    return x;
  };

  // Apply splits in descending record (coarsest first) order. Every forward
  // legality dependency points to a later record, so when a record is reached
  // all merges that could make its restored faces degenerate have already
  // been undone. Finer splits apply last and win every contested corner or
  // value (last-touch wins), which is exactly the pre-collapse state.
  for(int r = R-1; r >= 0; r--)
  {
    if(!selected[r]) continue;
    const ProgressiveSplitRecord & rec = pm.records[r];
    const int s = rec.s;
    const int d = rec.d;
    assert(alive_v[s] && "dependency closure failed: survivor inactive");
    assert(!alive_v[d] && "detached vertex reactivated before its own split");
    alive_v[d] = 1;
    V.row(d) = rec.vd;
    if(nattr > 0) A.row(d) = rec.pd;
    // Finer splits overwrite coarser survivor values.
    V.row(s) = rec.vs;
    if(nattr > 0) A.row(s) = rec.ps;

    // Ring faces: a side-1 face gets its corner back from the merged
    // representative onto d. A face killed by a still-collapsed later record
    // is inactive and simply absent; if that later split was selected it has
    // already run (descending order) and the face is live again.
    for(int k = 0; k < rec.ring.rows(); k++)
    {
      if(rec.ring_side(k) != 1) continue;
      const int f = rec.ring(k,0);
      const int c = rec.ring(k,1);
      if(alive_f[f]) F(f,c) = d;
    }

    // Restore the two faces killed by the collapse, with original winding.
    for(int k = 0; k < 2; k++)
    {
      const int f = rec.dead_face[k];
      assert(!alive_f[f] && "face reactivated twice");
      alive_f[f] = 1;
      const Eigen::RowVectorXi corners = rec.dead_corners.row(k);
      int prev = -2;
      for(int c = 0; c < 3; c++)
      {
        const int rv = resolve(corners(c));
        assert(rv != prev && "restored face would be degenerate");
        prev = rv;
        F(f,c) = rv;
      }
      labels(f) = rec.dead_labels(k);
    }
  }

  // ---- compact to a drawable mesh, keeping permanent ids aligned.
  std::vector<int> old_to_new(n, -1);
  std::vector<int> verts;
  verts.reserve(n);
  for(int v = 0; v < n; v++)
  {
    if(alive_v[v])
    {
      old_to_new[v] = (int)verts.size();
      verts.push_back(v);
    }
  }
  std::vector<int> faces;
  faces.reserve(m);
  for(int f = 0; f < m; f++)
  {
    if(alive_f[f]) faces.push_back(f);
  }

  RV.resize(verts.size(), dim);
  RP.resize(verts.size(), nattr);
  RI.resize(verts.size());
  for(int k = 0; k < (int)verts.size(); k++)
  {
    const int v = verts[k];
    RV.row(k) = V.row(v);
    if(nattr > 0) RP.row(k) = A.row(v);
    RI(k) = v;
  }
  RF.resize(faces.size(), 3);
  RJ.resize(faces.size());
  RL.resize(faces.size());
  for(int k = 0; k < (int)faces.size(); k++)
  {
    const int f = faces[k];
    for(int c = 0; c < 3; c++)
    {
      const int nv = old_to_new[F(f,c)];
      assert(nv >= 0 && "live face references an inactive vertex");
      RF(k,c) = nv;
    }
    RJ(k) = f;
    RL(k) = labels(f);
  }
  return true;
}
