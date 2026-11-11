// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2026 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
//
// Sanity tests for igl::decimate_progressive / igl::refine_progressive.
// No dependencies beyond Eigen and libigl headers.
#include <igl/decimate.h>
#include <igl/decimate_progressive.h>
#include <igl/refine_progressive.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <vector>

static int g_checks = 0;
static const char * g_scenario = "?";
static const char * g_phase = "";
#define CHECK(cond, msg)                                                       \
  do                                                                           \
  {                                                                            \
    ++g_checks;                                                                \
    if(!(cond))                                                                \
    {                                                                          \
      std::cerr << "FAIL [" << g_scenario << g_phase << "]: " << (msg) <<  \
        " at " << __FILE__ << ":" << __LINE__ << "\n";                      \
      std::exit(1);                                                            \
    }                                                                          \
  } while(0)

namespace
{
  using MatrixXd = Eigen::MatrixXd;
  using MatrixXi = Eigen::MatrixXi;
  using RowVectorXd = Eigen::RowVectorXd;
  using VectorXi = Eigen::VectorXi;

  struct Mesh
  {
    MatrixXd V, P;
    MatrixXi F;
    VectorXi L;
  };

  // Deterministic pseudo-random jitter.
  double jitter(int i, int salt)
  {
    unsigned int x = (unsigned int)(i * 73856093u) ^
                     (unsigned int)(salt * 19349663u) ^ 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return ((double)(x & 0xffff) / 32768.0 - 1.0) * 0.18;
  }

  // A 1-to-4 subdivided, asymmetrically jittered octahedron: closed, oriented,
  // edge-manifold, genus 0.
  Mesh make_blob(
    int rounds, int nattr, const RowVectorXd & center, double scale, int salt)
  {
    MatrixXd V(6,3);
    V << 1,0,0, -1,0,0, 0,1,0, 0,-1,0, 0,0,1, 0,0,-1;
    MatrixXi F(8,3);
    F << 0,2,4, 2,1,4, 1,3,4, 3,0,4,
         2,0,5, 1,2,5, 3,1,5, 0,3,5;
    for(int r = 0; r < rounds; r++)
    {
      std::map<std::pair<int,int>,int> mid;
      const auto midpoint = [&](int a, int b)
      {
        const int lo = std::min(a,b), hi = std::max(a,b);
        auto key = std::make_pair(lo,hi);
        auto it = mid.find(key);
        if(it != mid.end()) return it->second;
        const int id = (int)V.rows();
        V.conservativeResize(id+1, 3);
        V.row(id) = 0.5*(V.row(a)+V.row(b));
        mid[key] = id;
        return id;
      };
      MatrixXi NF(4*F.rows(),3);
      for(int f = 0; f < F.rows(); f++)
      {
        const int a = F(f,0), b = F(f,1), c = F(f,2);
        const int mab = midpoint(a,b), mbc = midpoint(b,c), mac = midpoint(c,a);
        NF.row(4*f+0) << a, mab, mac;
        NF.row(4*f+1) << mab, b, mbc;
        NF.row(4*f+2) << mac, mbc, c;
        NF.row(4*f+3) << mab, mbc, mac;
      }
      F = NF;
    }
    for(int v = 0; v < V.rows(); v++)
    {
      for(int k = 0; k < 3; k++)
        V(v,k) = V(v,k) + jitter(v, salt+k);
      V.row(v) *= scale;
      V.row(v) += center;
    }
    Mesh m;
    m.V = V;
    m.F = F;
    m.P.resize(V.rows(), nattr);
    for(int v = 0; v < V.rows(); v++)
      for(int k = 0; k < nattr; k++)
        m.P(v,k) = (k+1)*0.25*v + V(v,k%3)*0.7 + 0.01*k;
    m.L.resize(F.rows());
    for(int f = 0; f < F.rows(); f++) m.L(f) = f+1;
    return m;
  }

  // Flip faces until every shared edge is used in opposite directions. The
  // complexes built here are topologically orientable, so BFS reorientation
  // always succeeds.
  // Flip faces until every shared undirected edge is used in opposite
  // directions by its two flaps. Works from undirected edge adjacency, so it
  // does not assume the input already has consistent winding. The complexes
  // built here are topologically orientable closed shells, so this BFS always
  // succeeds.
  void reorient(MatrixXi & F)
  {
    // Undirected edge -> incident (face, directed corner c where edge is
    // (F(f,c), F(f,(c+1)%3))).
    std::map<std::pair<int,int>,
             std::vector<std::pair<int,int> > > emap;
    for(int f = 0; f < F.rows(); f++)
      for(int c = 0; c < 3; c++)
      {
        const int a = F(f,c), b = F(f,(c+1)%3);
        emap[{std::min(a,b), std::max(a,b)}].push_back({f,c});
      }
    for(auto & kv : emap)
      CHECK(kv.second.size() == 2, "reorient expects a closed manifold");

    std::vector<int> sign(F.rows(), 0);
    std::vector<int> stack;
    sign[0] = 1;
    stack.push_back(0);
    while(!stack.empty())
    {
      const int f = stack.back();
      stack.pop_back();
      for(int c = 0; c < 3; c++)
      {
        const int a = F(f,c), b = F(f,(c+1)%3);
        const auto key = std::make_pair(std::min(a,b), std::max(a,b));
        for(auto pr : emap[key])
        {
          const int g = pr.first;
          if(g == f) continue;
          const int gc = pr.second;
          const int ga = F(g,gc), gb = F(g,(gc+1)%3);
          // Neighbor agrees with f across this edge iff it traverses the
          // shared edge in the opposite direction. Agreement maps to equal
          // signs; disagreement means g needs the opposite sign.
          const bool opposite_now = (ga == b && gb == a);
          const int want = opposite_now ? sign[f] : -sign[f];
          if(sign[g] == 0)
          {
            sign[g] = want;
            stack.push_back(g);
          } else
          {
            CHECK(sign[g] == want, "non-orientable complex");
          }
        }
      }
    }
    for(int f = 0; f < F.rows(); f++)
      if(sign[f] == -1) std::swap(F(f,1), F(f,2));
  }

  struct ManifoldStats
  {
    int components = 0;
    std::vector<int> vertex_component;
    std::vector<int> face_component;
  };

  // Full structural validation: in-range indices, no dangling vertices, no
  // duplicate faces, every edge has exactly two oppositely oriented flaps,
  // per-component Euler characteristic (expect 2 for the genus-0 shells used
  // here).
  ManifoldStats check_manifold(
    const MatrixXd & V, const MatrixXi & F, int expected_components)
  {
    const int n = (int)V.rows();
    ManifoldStats st;
    std::vector<char> used(n, 0);
    std::set<std::pair<int,int> > oriented_seen;
    std::map<std::pair<int,int>, int> edge_count;
    for(int f = 0; f < F.rows(); f++)
    {
      for(int c = 0; c < 3; c++)
      {
        const int a = F(f,c), b = F(f,(c+1)%3);
        CHECK(a >= 0 && a < n && b >= 0 && b < n, "face index out of range");
        CHECK(a != b, "degenerate face");
        used[a] = 1; used[b] = 1;
        const auto key = std::make_pair(std::min(a,b), std::max(a,b));
        edge_count[key]++;
        CHECK(oriented_seen.insert({a,b}).second, "duplicate oriented edge");
      }
    }
    for(int v = 0; v < n; v++) CHECK(used[v], "dangling vertex");
    for(auto & kv : edge_count)
      CHECK(kv.second == 2, "edge does not have exactly two flaps");

    // Connected components over vertex adjacency.
    st.vertex_component.assign(n, -1);
    std::vector<std::vector<int> > adj(n);
    for(int f = 0; f < F.rows(); f++)
      for(int c = 0; c < 3; c++)
      {
        const int a = F(f,c), b = F(f,(c+1)%3);
        adj[a].push_back(b);
        adj[b].push_back(a);
      }
    int comps = 0;
    std::vector<int> stack;
    for(int s = 0; s < n; s++)
    {
      if(st.vertex_component[s] != -1) continue;
      st.vertex_component[s] = comps;
      stack.push_back(s);
      while(!stack.empty())
      {
        const int v = stack.back();
        stack.pop_back();
        for(int u : adj[v])
          if(st.vertex_component[u] == -1)
          {
            st.vertex_component[u] = comps;
            stack.push_back(u);
          }
      }
      comps++;
    }
    CHECK(comps == expected_components, "unexpected component count");
    st.components = comps;

    std::vector<int> Vc(comps,0), Fc(comps,0), Ec(comps,0);
    for(int v = 0; v < n; v++) Vc[st.vertex_component[v]]++;
    st.face_component.resize(F.rows());
    for(int f = 0; f < F.rows(); f++)
    {
      const int c = st.vertex_component[F(f,0)];
      CHECK(st.vertex_component[F(f,1)] == c &&
            st.vertex_component[F(f,2)] == c, "face spans components");
      st.face_component[f] = c;
      Fc[c]++;
    }
    for(auto & kv : edge_count)
      Ec[st.vertex_component[kv.first.first]]++;
    for(int c = 0; c < comps; c++)
    {
      const int chi = Vc[c] - Ec[c] + Fc[c];
      CHECK(chi == 2, "Euler characteristic is not 2 (genus 0 expected)");
      CHECK(3*Fc[c] == 2*Ec[c], "edge/face counting mismatch");
    }
    return st;
  }

  // Two fine blobs joined by a long, coarse 3-sided tube: one closed shell.
  Mesh make_dumbbell(int nattr)
  {
    const double D = 6.0;
    Mesh A = make_blob(2, nattr,
      (RowVectorXd(3) << -D,0,0).finished(), 1.0, 11);
    Mesh B = make_blob(2, nattr,
      (RowVectorXd(3) <<  D,0,0).finished(), 1.0, 23);

    // Remove the most +x face of A and the most -x face of B.
    int fA = 0, fB = 0;
    for(int f = 0; f < A.F.rows(); f++)
    {
      const double cx = A.V(A.F(f,0),0)+A.V(A.F(f,1),0)+A.V(A.F(f,2),0);
      const double bx = A.V(A.F(fA,0),0)+A.V(A.F(fA,1),0)+A.V(A.F(fA,2),0);
      if(cx > bx) fA = f;
    }
    for(int f = 0; f < B.F.rows(); f++)
    {
      const double cx = B.V(B.F(f,0),0)+B.V(B.F(f,1),0)+B.V(B.F(f,2),0);
      const double bx = B.V(B.F(fB,0),0)+B.V(B.F(fB,1),0)+B.V(B.F(fB,2),0);
      if(cx < bx) fB = f;
    }
    VectorXi loopA = A.F.row(fA), loopB = B.F.row(fB);

    auto erase_face = [](MatrixXi & F, VectorXi & L, int dead)
    {
      MatrixXi NF(F.rows()-1,3);
      VectorXi NL(L.size()-1);
      int w = 0;
      for(int f = 0; f < F.rows(); f++)
      {
        if(f == dead) continue;
        NF.row(w) = F.row(f);
        NL(w) = L(f);
        w++;
      }
      F = NF; L = NL;
    };
    erase_face(A.F, A.L, fA);
    erase_face(B.F, B.L, fB);

    // Offset B's vertices into A's index space.
    const int off = A.V.rows();
    MatrixXd V(A.V.rows()+B.V.rows(), 3);
    V << A.V, B.V;
    MatrixXd P(A.P.rows()+B.P.rows(), A.P.cols());
    P << A.P, B.P;
    MatrixXi F(A.F.rows()+B.F.rows(), 3);
    F << A.F, (B.F.array()+off);
    VectorXi L(A.L.size()+B.L.size());
    L << A.L, B.L;

    // Tube rings. End rings are the two boundary loops; middle rings are new,
    // coarsely spaced vertices (long coarse band).
    const int rings = 8;
    std::vector<std::array<int,3> > R(rings);
    R[0] = {(int)loopA(0),(int)loopA(1),(int)loopA(2)};
    R[rings-1] = {
      (int)loopB(0)+off,(int)loopB(1)+off,(int)loopB(2)+off};
    for(int r = 1; r < rings-1; r++)
    {
      const double t = (double)r/(rings-1);
      for(int i = 0; i < 3; i++)
      {
        const int id = (int)V.rows();
        V.conservativeResize(id+1,3);
        P.conservativeResize(id+1, nattr);
        V.row(id) = (1.0-t)*V.row(R[0][i]) + t*V.row(R[rings-1][i]);
        for(int k = 0; k < nattr; k++)
          P(id,k) = 7.0 + 0.13*r + 0.37*i + k;
        R[r][i] = id;
      }
    }
    std::vector<MatrixXi::Scalar> tubeF;
    std::vector<VectorXi::Scalar> tubeL;
    for(int r = 0; r+1 < rings; r++)
    {
      for(int i = 0; i < 3; i++)
      {
        const int j = (i+1)%3;
        tubeF.insert(tubeF.end(), {
          R[r][i], R[r][j], R[r+1][j],
          R[r][i], R[r+1][j], R[r+1][i]});
        tubeL.push_back(900000 + 9*r + 2*i);
        tubeL.push_back(900000 + 9*r + 2*i + 1);
      }
    }
    const int old_faces = F.rows();
    F.conservativeResize(old_faces + (int)tubeF.size()/3, 3);
    L.conservativeResize(old_faces + (int)tubeF.size()/3);
    for(int q = 0; q < (int)tubeF.size()/3; q++)
    {
      F.row(old_faces+q) << tubeF[3*q], tubeF[3*q+1], tubeF[3*q+2];
      L(old_faces+q) = tubeL[q];
    }
    reorient(F);

    Mesh out;
    out.V = V; out.P = P; out.F = F; out.L = L;
    return out;
  }

  // Permanent-id aligned lookup helper. `size` is the full permanent id
  // range (number of original elements), not the number of live ids, so
  // lookups for absent ids safely return -1.
  std::vector<int> id_to_row(const VectorXi & ids, int size)
  {
    std::vector<int> row(size, -1);
    for(int i = 0; i < ids.size(); i++) row[ids(i)] = i;
    return row;
  }

  std::set<int> face_id_set(const VectorXi & CJ)
  {
    std::set<int> s;
    for(int i = 0; i < CJ.size(); i++) s.insert(CJ(i));
    return s;
  }

  // Record touched permanent faces (killed faces + ring faces).
  std::set<int> touched_faces(const igl::ProgressiveSplitRecord & rec)
  {
    std::set<int> s;
    s.insert(rec.dead_face[0]);
    s.insert(rec.dead_face[1]);
    for(int k = 0; k < rec.ring.rows(); k++) s.insert(rec.ring(k,0));
    return s;
  }

  // Exact, permanent-id aligned comparison of a refined mesh against the
  // original fine mesh.
  void check_exact_restore(const Mesh & fine, const igl::ProgressiveMesh & pm,
    const MatrixXd & RV, const MatrixXi & RF, const MatrixXd & RP,
    const VectorXi & RJ, const VectorXi & RI, const VectorXi & RL)
  {
    CHECK(RV.rows() == fine.V.rows() && RF.rows() == fine.F.rows(),
      "full restore has wrong element counts");
    const auto vrow = id_to_row(RI, fine.V.rows());
    const auto frow = id_to_row(RJ, fine.F.rows());
    for(int v = 0; v < fine.V.rows(); v++)
    {
      CHECK(vrow[v] >= 0, "missing permanent vertex in full restore");
      for(int k = 0; k < fine.V.cols(); k++)
        CHECK(RV(vrow[v],k) == fine.V(v,k), "position not exactly restored");
      for(int k = 0; k < fine.P.cols(); k++)
        CHECK(RP(vrow[v],k) == fine.P(v,k), "attribute not exactly restored");
    }
    for(int f = 0; f < fine.F.rows(); f++)
    {
      CHECK(frow[f] >= 0, "missing permanent face in full restore");
      // Compare corner winding in permanent ids.
      std::array<int,3> got = {
        RI(RF(frow[f],0)), RI(RF(frow[f],1)), RI(RF(frow[f],2))};
      std::array<int,3> want = {fine.F(f,0), fine.F(f,1), fine.F(f,2)};
      // Winding must match up to cyclic rotation, never reversal.
      bool cyclic = false;
      for(int rot = 0; rot < 3; rot++)
        if(got[0]==want[rot] && got[1]==want[(rot+1)%3] &&
           got[2]==want[(rot+2)%3])
          cyclic = true;
      CHECK(cyclic, "face winding not exactly restored");
      CHECK(RL(frow[f]) == fine.L(f), "face label not restored by identity");
    }
    CHECK(pm.reached, "full decimation should reach its budget in this test");
  }

  // Records are stored in collapse order, so every legal prefix
  // {0,...,k-1} is a legal (dependency-closed) request. Its refinement is a
  // valid manifold view, though generally not a forward intermediate: the
  // forward level reached after t collapses corresponds to refining the
  // suffix {t,...,R-1}. Both families are exercised here.
  void check_all_prefixes(const Mesh & fine, const igl::ProgressiveMesh & pm)
  {
    const int R = (int)pm.records.size();
    const int m = fine.F.rows();

    // Every legal prefix: manifold output, identity labels, exact face
    // accounting (each applied split adds the two faces it killed).
    for(int k = 0; k <= R; k++)
    {
      std::vector<int> req(k);
      for(int i = 0; i < k; i++) req[i] = i;
      MatrixXd RV, RP; MatrixXi RF; VectorXi RI, RJ, RL;
      CHECK(igl::refine_progressive(pm, req, RV, RF, RP, RJ, RI, RL),
        "prefix refinement failed");
      g_phase = " prefix";
      check_manifold(RV, RF, pm.num_components);
      g_phase = "";
      const auto frow = id_to_row(RJ, fine.F.rows());
      for(int f = 0; f < m; f++)
        if(frow[f] >= 0) CHECK(RL(frow[f]) == fine.L(f),
          "label drifted away from face identity in prefix");

      if(k == 0)
      {
        CHECK(RF.rows() == pm.CF.rows() && RV.rows() == pm.CV.rows(),
          "empty request must return the coarse mesh");
      }
      // Dependency completion may add records, so the result has at least the
      // k requested splits; every applied split adds exactly two faces.
      CHECK(RF.rows() >= pm.CF.rows() + 2*k &&
        ((RF.rows() - pm.CF.rows()) % 2 == 0),
        "prefix refinement face accounting wrong");
    }

    // Every forward intermediate level: refine the suffix {t,...,R-1} and
    // cross-check against an independent decimation stopped at the same face
    // count.
    for(int t = 0; t <= R; t++)
    {
      std::vector<int> req;
      req.reserve(R-t);
      for(int i = t; i < R; i++) req.push_back(i);
      MatrixXd RV, RP; MatrixXi RF; VectorXi RI, RJ, RL;
      CHECK(igl::refine_progressive(pm, req, RV, RF, RP, RJ, RI, RL),
        "suffix refinement failed");
      check_manifold(RV, RF, pm.num_components);

      igl::ProgressiveMesh pm2;
      const bool ok2 = igl::decimate_progressive(
        fine.V, fine.F, fine.P, fine.L, RF.rows(), pm2);
      CHECK(ok2 && (int)pm2.records.size() == t,
        "independent level decimation did not stop at the same record count");
      CHECK(face_id_set(pm2.CJ) == face_id_set(RJ),
        "level face set mismatch vs independent decimation");
      const auto c2 = id_to_row(pm2.CI, fine.V.rows());
      const auto r1 = id_to_row(RI, fine.V.rows());
      std::set<int> lv, lv2;
      for(int i = 0; i < RI.size(); i++) lv.insert(RI(i));
      for(int i = 0; i < pm2.CI.size(); i++) lv2.insert(pm2.CI(i));
      CHECK(lv == lv2, "level vertex set mismatch vs independent run");
      for(int v = 0; v < fine.V.rows(); v++)
      {
        if(c2[v] < 0) continue;
        for(int q = 0; q < fine.V.cols(); q++)
          CHECK(RV(r1[v],q) == pm2.CV(c2[v],q),
            "level position mismatch vs independent decimation");
        for(int q = 0; q < fine.P.cols(); q++)
          CHECK(RP(r1[v],q) == pm2.CP(c2[v],q),
            "level attribute mismatch vs independent decimation");
      }
    }
  }
} // namespace

static void scenario_full_roundtrip()
{
  // Asymmetric closed mesh, two attribute columns.
  Mesh fine = make_blob(3, 2,
    (RowVectorXd(3) << 0.3, -0.7, 0.2).finished(), 1.7, 5);
  CHECK(fine.V.allFinite(), "input coordinates must be finite");

  const int target = fine.F.rows()/2;
  igl::ProgressiveMesh pm;
  const bool reached =
    igl::decimate_progressive(fine.V, fine.F, fine.P, fine.L, target, pm);
  CHECK(reached, "budget should be reachable on a subdivided blob");
  CHECK(pm.CF.rows() <= target, "coarse mesh exceeds face budget");
  CHECK((int)pm.records.size() ==
        (fine.F.rows()-(int)pm.CF.rows())/2,
    "one record per successful collapse, two faces each");

  // Coarse mesh must agree with the default igl::decimate output.
  MatrixXd U; MatrixXi G; VectorXi J, I;
  CHECK(igl::decimate(fine.V, fine.F, target, false, U, G, J, I),
    "reference decimate failed");
  CHECK(face_id_set(pm.CJ) == face_id_set(J),
    "progressive coarse faces differ from default decimate");
  {
    const auto prow = id_to_row(pm.CI, fine.V.rows());
    const auto urow = id_to_row(I, fine.V.rows());
    for(int v = 0; v < fine.V.rows(); v++)
    {
      if(urow[v] < 0) continue;
      CHECK(prow[v] >= 0, "progressive coarse vertex set differs");
      for(int q = 0; q < 3; q++)
        CHECK(pm.CV(prow[v],q) == U(urow[v],q),
          "progressive coarse positions differ from default decimate");
    }
  }
  g_phase = " coarse";
  check_manifold(pm.CV, pm.CF, 1);

  // Full roundtrip: exact restoration.
  std::vector<int> all(pm.records.size());
  for(int i = 0; i < (int)all.size(); i++) all[i] = i;
  MatrixXd RV, RP; MatrixXi RF; VectorXi RI, RJ, RL;
  CHECK(igl::refine_progressive(pm, all, RV, RF, RP, RJ, RI, RL),
    "full refine failed");
  g_phase = " full";
  check_manifold(RV, RF, 1);
  g_phase = "";
  check_exact_restore(fine, pm, RV, RF, RP, RJ, RI, RL);

  // Every legal prefix, manifold and identity checks, independent cross-check.
  check_all_prefixes(fine, pm);

  // Shuffled, duplicated requests must give the identical result.
  std::vector<int> messy;
  std::mt19937 rng(42);
  for(int i = (int)all.size()-1; i >= 0; i--) messy.push_back(i);
  std::shuffle(messy.begin(), messy.end(), rng);
  messy.insert(messy.end(), all.begin(), all.begin()+all.size()/2);
  messy.push_back(messy.front());
  MatrixXd RV2, RP2; MatrixXi RF2; VectorXi RI2, RJ2, RL2;
  CHECK(igl::refine_progressive(pm, messy, RV2, RF2, RP2, RJ2, RI2, RL2),
    "messy refine failed");
  CHECK(face_id_set(RJ2) == face_id_set(RJ),
    "duplicate/out-of-order requests changed the face set");
  {
    const auto a = id_to_row(RI, fine.V.rows()),
               b = id_to_row(RI2, fine.V.rows());
    for(int v = 0; v < fine.V.rows(); v++)
      if(a[v] >= 0)
      {
        CHECK(b[v] >= 0, "messy request changed vertex set");
        for(int q = 0; q < fine.V.cols(); q++)
          CHECK(RV(a[v],q) == RV2(b[v],q),
            "messy request changed positions");
        for(int q = 0; q < fine.P.cols(); q++)
          CHECK(RP(a[v],q) == RP2(b[v],q),
            "messy request changed attributes");
      }
  }

  // Uncollapsible tetrahedron: no record, target not reached, mesh intact.
  {
    MatrixXd TV(4,3);
    TV << 0,0,0, 1,0,0, 0,1,0, 0,0,1;
    MatrixXi TF(4,3);
    TF << 0,2,1, 0,1,3, 0,3,2, 1,2,3;
    MatrixXd TP(4,1); TP << 0,1,2,3;
    VectorXi TL(4); TL << 41,42,43,44;
    igl::ProgressiveMesh tpm;
    const bool tok =
      igl::decimate_progressive(TV,TF,TP,TL,2,tpm);
    CHECK(!tok, "tetrahedron must not reach a smaller face budget");
    CHECK(tpm.records.empty(), "failed candidates must not take record ids");
    CHECK(tpm.CF.rows() == 4 && tpm.CV.rows() == 4,
      "tetrahedron coarse mesh must stay intact");
    MatrixXd tRV,tRP; MatrixXi tRF; VectorXi tRI,tRJ,tRL;
    CHECK(igl::refine_progressive(tpm,{},tRV,tRF,tRP,tRJ,tRI,tRL),
      "tetrahedron refine failed");
    check_manifold(tRV,tRF,1);
    CHECK(tRF.rows()==4 && tRJ.minCoeff()>=0 && tRJ.maxCoeff()<=3,
      "tetrahedron refine changed the mesh");
  }

  // No-op target: zero records, coarse mesh is the input, permutation ids.
  {
    igl::ProgressiveMesh npm;
    const bool nok = igl::decimate_progressive(
      fine.V, fine.F, fine.P, fine.L, fine.F.rows(), npm);
    CHECK(nok && npm.records.empty(), "no-op target must produce no records");
    CHECK(npm.CF.rows() == fine.F.rows() &&
          npm.CV.rows() == fine.V.rows(),
      "no-op target must keep the full mesh");
    for(int v = 0; v < fine.V.rows(); v++)
      CHECK(npm.CI(v) == v, "no-op permanent vertex ids must be identity");
    for(int f = 0; f < fine.F.rows(); f++)
      CHECK(npm.CJ(f) == f, "no-op permanent face ids must be identity");
  }

  // Illegal record indices are rejected and outputs stay untouched.
  {
    MatrixXd sRV = MatrixXd::Zero(2,3);
    MatrixXi sRF = MatrixXi::Ones(2,3);
    MatrixXd sRP = MatrixXd::Zero(2,2);
    VectorXi sRJ = VectorXi::Ones(2), sRI = VectorXi::Ones(2),
              sRL = VectorXi::Ones(2);
    const MatrixXd snapV = sRV; const MatrixXi snapF = sRF;
    const MatrixXd snapP = sRP; const VectorXi snapJ = sRJ, snapI = sRI,
                                  snapL = sRL;
    CHECK(!igl::refine_progressive(pm,{(int)pm.records.size()},
      sRV,sRF,sRP,sRJ,sRI,sRL), "out-of-range record must be rejected");
    CHECK(!igl::refine_progressive(pm,{-1},
      sRV,sRF,sRP,sRJ,sRI,sRL), "negative record must be rejected");
    CHECK((sRV-snapV).norm()==0 && (sRF-snapF).norm()==0 &&
          (sRP-snapP).norm()==0 && (sRJ-snapJ).norm()==0 &&
          (sRI-snapI).norm()==0 && (sRL-snapL).norm()==0,
      "rejected request modified the outputs");
  }
  std::cout << "PASS full-roundtrip\n";
}

static void scenario_independent_components()
{
  Mesh A = make_blob(2, 2,
    (RowVectorXd(3) << -8,0,0).finished(), 1.0, 31);
  Mesh B = make_blob(2, 2,
    (RowVectorXd(3) <<  8,0,0).finished(), 1.0, 37);
  Mesh fine;
  fine.V.resize(A.V.rows()+B.V.rows(),3);
  fine.P.resize(A.P.rows()+B.P.rows(),A.P.cols());
  fine.F.resize(A.F.rows()+B.F.rows(),3);
  fine.L.resize(A.L.size()+B.L.size());
  fine.V << A.V, B.V;
  fine.P << A.P, B.P;
  fine.F << A.F, (B.F.array()+A.V.rows());
  fine.L << A.L, (B.L.array()+5000);

  igl::ProgressiveMesh pm;
  const bool reached = igl::decimate_progressive(
    fine.V, fine.F, fine.P, fine.L, fine.F.rows()/2, pm);
  CHECK(reached, "component budget unreachable");
  CHECK(pm.num_components == 2, "expected two components");
  check_manifold(pm.CV, pm.CF, 2);

  // Records on different components must never depend on each other.
  int empty_dep_records = 0;
  for(const auto & rec : pm.records)
  {
    if(rec.deps.empty()) empty_dep_records++;
    for(int dep : rec.deps)
      CHECK(pm.records[dep].component == rec.component,
        "dependency crosses connected components");
  }
  CHECK(empty_dep_records >= 2,
    "records collapsed into a single global chain");

  // Refine only component 0's records.
  std::vector<int> req0;
  std::set<int> records1;
  for(int i = 0; i < (int)pm.records.size(); i++)
  {
    if(pm.records[i].component == 0) req0.push_back(i);
    else records1.insert(i);
  }
  CHECK(!req0.empty() && !records1.empty(),
    "expected collapses on both components");
  MatrixXd RV, RP; MatrixXi RF; VectorXi RI, RJ, RL;
  CHECK(igl::refine_progressive(pm, req0, RV, RF, RP, RJ, RI, RL),
    "component-selective refine failed");
  const ManifoldStats st = check_manifold(RV, RF, 2);
  CHECK(RL.size() == RF.rows(), "label size mismatch");

  // Original face component membership, derived from the input.
  std::vector<int> face_comp(fine.F.rows());
  for(int f = 0; f < fine.F.rows(); f++)
    face_comp[f] = fine.F(f,0) < (int)A.V.rows() ? 0 : 1;

  const std::set<int> coarse_faces = face_id_set(pm.CJ);
  const std::set<int> out_faces = face_id_set(RJ);
  int refined0 = 0;
  for(int f = 0; f < fine.F.rows(); f++)
  {
    if(face_comp[f] == 1)
    {
      // Component 1 must remain exactly in its coarse state.
      CHECK(coarse_faces.count(f) == out_faces.count(f),
        "non-requested component changed face state");
    } else if(out_faces.count(f) && !coarse_faces.count(f))
    {
      refined0++;
    }
  }
  CHECK(refined0 > 0, "requested component was not refined");

  // Every component-1 record is still unapplied: detached vertices and killed
  // faces remain inactive.
  const auto vrow = id_to_row(RI, fine.V.rows());
  const auto frow = id_to_row(RJ, fine.F.rows());
  for(int i : records1)
  {
    const auto & rec = pm.records[i];
    CHECK(vrow[rec.d] < 0, "detached vertex of a skipped record is alive");
    CHECK(frow[rec.dead_face[0]] < 0 && frow[rec.dead_face[1]] < 0,
      "killed face of a skipped record was revived");
  }

  // Labels still follow permanent face identity.
  for(int k = 0; k < RJ.size(); k++)
    CHECK(RL(k) == fine.L(RJ(k)), "label provenance broken across components");

  // Refining the other component alone yields a disjoint face set change.
  std::vector<int> req1(records1.begin(), records1.end());
  MatrixXd RVb, RPb; MatrixXi RFb; VectorXi RIb, RJb, RLb;
  CHECK(igl::refine_progressive(pm, req1, RVb, RFb, RPb, RJb, RIb, RLb),
    "second component refine failed");
  check_manifold(RVb, RFb, 2);
  std::set<int> diffA = out_faces, diffB = face_id_set(RJb);
  for(int f : coarse_faces) { diffA.erase(f); diffB.erase(f); }
  std::set<int> inter;
  std::set_intersection(diffA.begin(), diffA.end(),
    diffB.begin(), diffB.end(),
    std::inserter(inter, inter.end()));
  CHECK(inter.empty(),
    "component refinements share restored faces");
  std::cout << "PASS independent-components\n";
}

static void scenario_selective_connected()
{
  Mesh fine = make_dumbbell(2);
  check_manifold(fine.V, fine.F, 1);
  const int n = fine.V.rows();

  igl::ProgressiveMesh pm;
  // Moderate budget: the fine blobs collapse, the long coarse bridge does not.
  const bool reached = igl::decimate_progressive(
    fine.V, fine.F, fine.P, fine.L, fine.F.rows()-40, pm);
  CHECK(reached, "dumbbell budget unreachable");
  CHECK((int)pm.records.size() == 20, "expected twenty collapses");

  const double xA = -3.0, xB = 3.0;
  std::vector<int> groupA, groupB;
  for(int i = 0; i < (int)pm.records.size(); i++)
  {
    const auto & rec = pm.records[i];
    const double xs = fine.V(rec.s,0), xd = fine.V(rec.d,0);
    if(xs < xA && xd < xA) groupA.push_back(i);
    if(xs > xB && xd > xB) groupB.push_back(i);
  }
  CHECK(!groupA.empty() && !groupB.empty(),
    "could not find far-apart record groups in the real record stream");

  // The two groups must touch disjoint face stars.
  std::set<int> touchA, touchB;
  for(int i : groupA)
  {
    auto s = touched_faces(pm.records[i]);
    touchA.insert(s.begin(), s.end());
  }
  for(int i : groupB)
  {
    auto s = touched_faces(pm.records[i]);
    touchB.insert(s.begin(), s.end());
  }
  std::set<int> inter;
  std::set_intersection(touchA.begin(), touchA.end(),
    touchB.begin(), touchB.end(),
    std::inserter(inter, inter.end()));
  CHECK(inter.empty(),
    "far-apart operations share a collapse face star");

  // No dependency edge may link the two groups.
  std::set<int> setA(groupA.begin(), groupA.end()),
                setB(groupB.begin(), groupB.end());
  for(int i : groupA)
    for(int d : pm.records[i].deps)
      CHECK(!setB.count(d), "conservative dependency links far groups A->B");
  for(int i : groupB)
    for(int d : pm.records[i].deps)
      CHECK(!setA.count(d), "conservative dependency links far groups B->A");

  // Request only group A. Dependencies are completed by refine_progressive.
  MatrixXd RV, RP; MatrixXi RF; VectorXi RI, RJ, RL;
  CHECK(igl::refine_progressive(pm, groupA, RV, RF, RP, RJ, RI, RL),
    "connected selective refine failed");
  check_manifold(RV, RF, 1);

  const std::set<int> coarse_faces = face_id_set(pm.CJ);
  const std::set<int> out_faces = face_id_set(RJ);
  const auto vrow = id_to_row(RI, fine.V.rows());
  const auto frow = id_to_row(RJ, fine.F.rows());

  // Region of a face from its original corner coordinates.
  const auto region = [&](int f) -> int
  {
    double mx = -1e9, mn = 1e9;
    for(int c = 0; c < 3; c++)
    {
      mx = std::max(mx, (double)fine.V(fine.F(f,c),0));
      mn = std::min(mn, (double)fine.V(fine.F(f,c),0));
    }
    if(mx < xA) return 0;
    if(mn > xB) return 1;
    return 2; // bridge band
  };

  int a_grew = 0;
  for(int f = 0; f < fine.F.rows(); f++)
  {
    const int reg = region(f);
    const bool in_coarse = coarse_faces.count(f) > 0;
    const bool in_out = out_faces.count(f) > 0;
    if(reg == 1 || reg == 2)
    {
      // Far blob and the bridge band stay exactly coarse.
      CHECK(in_coarse == in_out,
        "non-requested region changed face state");
    } else if(in_out && !in_coarse)
    {
      a_grew++;
    }
  }
  CHECK(a_grew > 0, "requested region was not refined");

  // Group B records remain fully unapplied, record by record.
  for(int i : groupB)
  {
    const auto & rec = pm.records[i];
    CHECK(vrow[rec.d] < 0, "far group vertex wrongly reactivated");
    CHECK(frow[rec.dead_face[0]] < 0 && frow[rec.dead_face[1]] < 0,
      "far group killed face wrongly restored");
  }

  // Requesting B alone is symmetric and independent of A.
  MatrixXd RV2, RP2; MatrixXi RF2; VectorXi RI2, RJ2, RL2;
  CHECK(igl::refine_progressive(pm, groupB, RV2, RF2, RP2, RJ2, RI2, RL2),
    "far group B refine failed");
  check_manifold(RV2, RF2, 1);
  const std::set<int> out2 = face_id_set(RJ2);
  for(int f = 0; f < fine.F.rows(); f++)
  {
    if(region(f) == 0)
      CHECK(coarse_faces.count(f) == out2.count(f),
        "group B request disturbed group A region");
  }

  // Requesting both restores strictly more than either alone.
  std::vector<int> both = groupA;
  both.insert(both.end(), groupB.begin(), groupB.end());
  MatrixXd RV3, RP3; MatrixXi RF3; VectorXi RI3, RJ3, RL3;
  CHECK(igl::refine_progressive(pm, both, RV3, RF3, RP3, RJ3, RI3, RL3),
    "combined refine failed");
  check_manifold(RV3, RF3, 1);
  CHECK((int)RJ3.size() > (int)RJ.size() &&
        (int)RJ3.size() > (int)RJ2.size(),
    "combined refinement is not strictly finer");
  for(int k = 0; k < RJ3.size(); k++)
    CHECK(RL3(k) == fine.L(RJ3(k)), "label provenance broken on shell");
  std::cout << "PASS selective-connected\n";
}

static void scenario_attributes()
{
  // Three attribute columns; hand-check the first midpoint merge.
  Mesh fine = make_blob(1, 3,
    (RowVectorXd(3) << 0,0,0).finished(), 1.0, 71);
  igl::ProgressiveMesh pm;
  const bool reached = igl::decimate_progressive(
    fine.V, fine.F, fine.P, fine.L, fine.F.rows()-2, pm);
  CHECK(reached && pm.records.size() == 1, "expected exactly one collapse");
  const auto & rec = pm.records[0];
  const auto crow = id_to_row(pm.CI, fine.V.rows());
  CHECK(crow[rec.s] >= 0, "merge survivor missing from coarse mesh");
  for(int k = 0; k < fine.P.cols(); k++)
  {
    const double want = 0.5*(fine.P(rec.s,k)+fine.P(rec.d,k));
    CHECK(pm.CP(crow[rec.s],k) == want,
      "continuous attributes must merge by endpoint mean");
  }
  // The midpoint position uses the same rule.
  for(int k = 0; k < 3; k++)
  {
    const double want = 0.5*(fine.V(rec.s,k)+fine.V(rec.d,k));
    CHECK(pm.CV(crow[rec.s],k) == want,
      "coarse position must be the edge midpoint");
  }
  // Labels are untouched on every surviving face identity.
  for(int i = 0; i < pm.CJ.size(); i++)
    CHECK(pm.CL(i) == fine.L(pm.CJ(i)),
      "labels must follow permanent face identity, never interpolate");

  // Zero-attribute inputs still work through the whole pipeline.
  {
    MatrixXd emptyP(fine.V.rows(), 0);
    igl::ProgressiveMesh epm;
    CHECK(igl::decimate_progressive(
      fine.V, fine.F, emptyP, fine.L, fine.F.rows()/2, epm),
      "zero-attribute decimation failed");
    CHECK(epm.CP.cols() == 0, "zero-attribute coarse matrix must have 0 cols");
    std::vector<int> all(epm.records.size());
    for(int i = 0; i < (int)all.size(); i++) all[i] = i;
    MatrixXd RV, RP; MatrixXi RF; VectorXi RI, RJ, RL;
    CHECK(igl::refine_progressive(epm, all, RV, RF, RP, RJ, RI, RL),
      "zero-attribute refine failed");
    check_manifold(RV, RF, 1);
    CHECK(RP.cols() == 0 && RV.rows() == fine.V.rows(),
      "zero-attribute roundtrip counts wrong");
  }

  // Empty labels default to zero and round-trip.
  {
    VectorXi emptyL;
    igl::ProgressiveMesh lpm;
    CHECK(igl::decimate_progressive(
      fine.V, fine.F, fine.P, emptyL, fine.F.rows()/2, lpm),
      "empty-label decimation failed");
    CHECK(lpm.CL.minCoeff() == 0 && lpm.CL.maxCoeff() == 0,
      "empty labels should default to zero");
  }

  // Full pipeline restores every attribute exactly, including multi-merge
  // chains on a more decimated mesh.
  igl::ProgressiveMesh pm2;
  CHECK(igl::decimate_progressive(
    fine.V, fine.F, fine.P, fine.L, fine.F.rows()/3, pm2),
    "deep decimation failed");
  std::vector<int> all(pm2.records.size());
  for(int i = 0; i < (int)all.size(); i++) all[i] = i;
  MatrixXd RV, RP; MatrixXi RF; VectorXi RI, RJ, RL;
  CHECK(igl::refine_progressive(pm2, all, RV, RF, RP, RJ, RI, RL),
    "deep refine failed");
  const auto vrow = id_to_row(RI, fine.V.rows());
  for(int v = 0; v < fine.V.rows(); v++)
  {
    CHECK(vrow[v] >= 0, "missing vertex in attribute roundtrip");
    for(int k = 0; k < fine.P.cols(); k++)
      CHECK(RP(vrow[v],k) == fine.P(v,k),
        "attribute not exactly restored after merge chain");
  }

  // An unreachable budget still returns a valid mesh and usable records;
  // refining them all still restores the original.
  {
    MatrixXd TV(4,3);
    TV << 0,0,0, 1,0,0, 0,1,0, 0,0,1;
    MatrixXi TF(4,3);
    TF << 0,2,1, 0,1,3, 0,3,2, 1,2,3;
    MatrixXd TP(4,2); TP << 1,2,3,4,5,6,7,8;
    VectorXi TL(4); TL << 7,8,9,10;
    igl::ProgressiveMesh tpm;
    const bool ok = igl::decimate_progressive(TV,TF,TP,TL,2,tpm);
    CHECK(!ok, "unreachable budget must report failure");
    CHECK(tpm.CF.rows() == 4 && !tpm.reached,
      "failed run must still expose the valid coarse mesh");
    MatrixXd tRV,tRP; MatrixXi tRF; VectorXi tRI,tRJ,tRL;
    CHECK(igl::refine_progressive(tpm,{},tRV,tRF,tRP,tRJ,tRI,tRL),
      "refine on failed decimation crashed");
    check_manifold(tRV,tRF,1);
  }
  std::cout << "PASS attributes\n";
}

int main()
{
  g_scenario = "full-roundtrip";
  scenario_full_roundtrip();
  g_scenario = "independent-components";
  scenario_independent_components();
  g_scenario = "selective-connected";
  scenario_selective_connected();
  g_scenario = "attributes";
  scenario_attributes();
  CHECK(g_checks > 0, "no checks executed");
  std::cout << "LIBIGL_PROGRESSIVE_OK\n";
  return 0;
}
