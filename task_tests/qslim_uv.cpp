#include "task_support/check.h"

#include <igl/decimate.h>
#include <igl/edge_flaps.h>
#include <igl/qslim.h>

#include <Eigen/Cholesky>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <utility>
#include <vector>

using Eigen::MatrixXi;
using Eigen::MatrixXd;
using Eigen::Vector2d;
using Eigen::Vector3d;
using Eigen::VectorXd;
using Eigen::VectorXi;

namespace
{
  struct TestMesh
  {
    MatrixXd V, T;
    MatrixXi F, FT;
    std::vector<int> north_seam;
    std::vector<int> south_seam;
  };

  TestMesh octa_mesh()
  {
    TestMesh m;
    m.V.resize(6, 3);
    m.V << 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1, 0, 0, -1;
    m.F.resize(8, 3);
    m.F << 0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 4,
           2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3, 5;
    m.T.resize(6, 2);
    m.T << 1, 0, -1, 0, 0, 1, 0, -1, 0.2, 0.3, -0.2, -0.3;
    m.FT = m.F;
    return m;
  }

  // A subdivided closed sphere.  Its equator is one simple seam loop; the
  // duplicated south equator UV rows deliberately have exactly the same
  // numerical coordinates as their northern counterparts.
  TestMesh sphere_mesh(const int N = 12, const int L = 8)
  {
    TestMesh m;
    const int rings = L - 1;
    m.V.resize(2 + rings * N, 3);
    const int north = 0;
    m.V.row(north) << 0.0, 1.0, 0.0;
    const auto base = [N](const int j) { return 1 + (j - 1) * N; };
    for(int j = 1; j <= L - 1; ++j)
    {
      const double theta = EIGEN_PI * j / L;
      for(int i = 0; i < N; ++i)
      {
        const double phi = 2.0 * EIGEN_PI * i / N;
        m.V.row(base(j) + i) <<
          std::sin(theta) * std::cos(phi),
          std::cos(theta),
          std::sin(theta) * std::sin(phi);
      }
    }
    const int south = 1 + rings * N;
    m.V.row(south) << 0.0, -1.0, 0.0;

    std::vector<Eigen::RowVector3i> faces;
    for(int i = 0; i < N; ++i)
      faces.push_back({north, base(1) + (i + 1) % N, base(1) + i});
    for(int j = 1; j < L - 1; ++j)
    {
      for(int i = 0; i < N; ++i)
      {
        const int a = base(j) + i;
        const int b = base(j) + (i + 1) % N;
        const int c = base(j + 1) + i;
        const int d = base(j + 1) + (i + 1) % N;
        faces.push_back({a, b, c});
        faces.push_back({b, d, c});
      }
    }
    for(int i = 0; i < N; ++i)
      faces.push_back({south, base(L - 1) + i, base(L - 1) + (i + 1) % N});

    m.F.resize((int)faces.size(), 3);
    for(int f = 0; f < m.F.rows(); ++f) m.F.row(f) = faces[f];

    m.T.resize(m.V.rows() + N, 2);
    m.T.topRows(m.V.rows()) << m.V.col(0), m.V.col(2);
    const int equator_j = L / 2;
    const int equator_base = base(equator_j);
    for(int i = 0; i < N; ++i)
    {
      m.north_seam.push_back(equator_base + i);
      m.south_seam.push_back((int)m.V.rows() + i);
      m.T.row(m.south_seam[i]) = m.T.row(m.north_seam[i]);
    }

    m.FT = m.F;
    for(int f = 0; f < m.F.rows(); ++f)
    {
      const Vector3d center =
        (m.V.row(m.F(f, 0)) + m.V.row(m.F(f, 1)) + m.V.row(m.F(f, 2))) / 3.0;
      if(center.y() >= 0.0) continue;
      for(int c = 0; c < 3; ++c)
      {
        const int v = m.F(f, c);
        for(int i = 0; i < N; ++i)
          if(v == equator_base + i) m.FT(f, c) = m.south_seam[i];
      }
    }
    return m;
  }

  int count_seam_edges(const MatrixXi & G, const MatrixXi & FS)
  {
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> edges;
    for(int f = 0; f < G.rows(); ++f)
    {
      for(int c = 0; c < 3; ++c)
      {
        const int a = G(f, c);
        const int b = G(f, (c + 1) % 3);
        edges[std::minmax(a, b)].push_back({f, c});
      }
    }
    int seams = 0;
    for(const auto & entry : edges)
    {
      const auto & occ = entry.second;
      if(occ.size() != 2) continue;
      const int f0 = occ[0].first;
      const int f1 = occ[1].first;
      const auto corner = [&](const int f, const int v)
      {
        for(int c = 0; c < 3; ++c) if(G(f, c) == v) return c;
        return -1;
      };
      const int x = entry.first.first;
      const int y = entry.first.second;
      const int c0x = corner(f0, x), c1x = corner(f1, x);
      const int c0y = corner(f0, y), c1y = corner(f1, y);
      if(c0x < 0 || c1x < 0 || c0y < 0 || c1y < 0) continue;
      if(FS(f0, c0x) != FS(f1, c1x) || FS(f0, c0y) != FS(f1, c1y)) ++seams;
    }
    return seams;
  }

  int uv_component_count(const MatrixXi & FT, const int t_rows)
  {
    std::vector<int> parent(t_rows);
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&](int x)
    {
      while(parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
      return x;
    };
    for(int f = 0; f < FT.rows(); ++f)
      for(int c = 0; c < 3; ++c)
      {
        int a = find(FT(f, c));
        int b = find(FT(f, (c + 1) % 3));
        if(a != b) parent[b] = a;
      }
    std::set<int> roots;
    for(int f = 0; f < FT.rows(); ++f)
      for(int c = 0; c < 3; ++c) roots.insert(find(FT(f, c)));
    return (int)roots.size();
  }

  bool valid_result(
    const TestMesh & m,
    const MatrixXd & U,
    const MatrixXi & G,
    const MatrixXd & S,
    const MatrixXi & FS,
    const VectorXi & J,
    const VectorXi & I)
  {
    if(G.rows() < 1 || G.cols() != 3 || FS.rows() != G.rows() || FS.cols() != 3)
      return false;
    if(J.size() != G.rows() || U.rows() != I.size() || U.cols() != 3 || S.cols() != 2)
      return false;
    if(!U.allFinite() || !S.allFinite()) return false;

    std::set<int> j_birth;
    std::set<int> i_birth;
    for(int k = 0; k < J.size(); ++k)
    {
      if(J(k) < 0 || J(k) >= m.F.rows() || !j_birth.insert(J(k)).second) return false;
    }
    for(int u = 0; u < I.size(); ++u)
    {
      if(I(u) < 0 || I(u) >= m.V.rows() || !i_birth.insert(I(u)).second) return false;
    }
    for(int k = 0; k < G.rows(); ++k)
    {
      const int f = J(k);
      for(int c = 0; c < 3; ++c)
      {
        const int gv = G(k, c);
        const int uv = FS(k, c);
        if(gv < 0 || gv >= U.rows() || uv < 0 || uv >= S.rows()) return false;
        // J identifies the birth face, but a surviving corner may have been
        // redirected from the collapsed-away endpoint to the survivor.
      }
      const Vector3d p0 = U.row(G(k, 0)).head<3>();
      const Vector3d p1 = U.row(G(k, 1)).head<3>();
      const Vector3d p2 = U.row(G(k, 2)).head<3>();
      if((p1 - p0).cross(p2 - p0).squaredNorm() <= 0.0) return false;

      Vector2d q[3];
      for(int c = 0; c < 3; ++c) q[c] = S.row(FS(k, c));
      const double area =
        (q[1] - q[0]).x() * (q[2] - q[0]).y() -
        (q[1] - q[0]).y() * (q[2] - q[0]).x();
      const double input_area =
        (m.T.row(m.FT(f, 1)) - m.T.row(m.FT(f, 0))).x() *
          (m.T.row(m.FT(f, 2)) - m.T.row(m.FT(f, 0))).y() -
        (m.T.row(m.FT(f, 1)) - m.T.row(m.FT(f, 0))).y() *
          (m.T.row(m.FT(f, 2)) - m.T.row(m.FT(f, 0))).x();
      if(area * input_area <= 0.0) return false;
    }

    std::map<std::pair<int, int>, int> edges;
    for(int f = 0; f < G.rows(); ++f)
      for(int c = 0; c < 3; ++c)
        ++edges[std::minmax(G(f, c), G(f, (c + 1) % 3))];
    for(const auto & e : edges) if(e.second != 2) return false;

    std::vector<char> uv_used(S.rows(), false);
    for(int k = 0; k < FS.size(); ++k) uv_used[FS(k)] = true;
    for(char used : uv_used) if(!used) return false;
    return true;
  }

  int objective_joint_small_matrices()
  {
    TestMesh m = sphere_mesh(8, 4);
    MatrixXi E, EF, EI;
    VectorXi EMAP;
    igl::edge_flaps(m.F, E, EMAP, EF, EI);
    int seam_e = -1;
    for(int e = 0; e < E.rows(); ++e)
    {
      const int f0 = EF(e, 0), f1 = EF(e, 1);
      const auto corner = [&](const int f, const int v)
      {
        for(int c = 0; c < 3; ++c) if(m.F(f, c) == v) return c;
        return -1;
      };
      const int a = E(e, 0), b = E(e, 1);
      if(m.FT(f0, corner(f0, a)) != m.FT(f1, corner(f1, a)) ||
         m.FT(f0, corner(f0, b)) != m.FT(f1, corner(f1, b)))
      {
        seam_e = e;
        break;
      }
    }
    TASK_CHECK(seam_e >= 0);

    QslimUvState state;
    state.uv_weight = 1.7;
    state.FT = m.FT;
    state.parent.resize(m.T.rows());
    state.quadrics.resize(m.T.rows());
    std::iota(state.parent.begin(), state.parent.end(), 0);
    for(int q = 0; q < m.T.rows(); ++q)
    {
      Eigen::Matrix<double, 5, 5> M;
      for(int i = 0; i < 5; ++i)
        for(int j = 0; j < 5; ++j)
          M(i, j) = std::sin(0.31 * (q + 1) + 0.17 * i + 0.11 * j);
      state.quadrics[q].A =
        Eigen::Matrix<double, 5, 5>::Identity() + 0.05 * M.transpose() * M;
      for(int i = 0; i < 5; ++i)
        state.quadrics[q].b(0, i) = 0.1 * std::cos(0.7 * q + 0.3 * i);
      state.quadrics[q].c = 1.0 + 0.125 * q;
    }

    QslimUvCandidate cand = qslim_uv_make_candidate(
      state, seam_e, m.V, m.F, E, EMAP, EF, EI);
    TASK_CHECK(cand.ok);
    TASK_CHECK(cand.groups.size() == 2);

    const int n = 3 + 2 * (int)cand.groups.size();
    MatrixXd H = MatrixXd::Zero(n, n);
    VectorXd d = VectorXd::Zero(n);
    double c0 = 0.0;
    const double w = state.uv_weight;
    for(int g = 0; g < (int)cand.groups.size(); ++g)
    {
      QslimUvQuad5 q;
      for(const int r : cand.groups[g].roots)
        q = qslim_uv_add_quadric(q, state.quadrics[r]);
      c0 += q.c;
      H.block<3, 3>(0, 0) += q.A.block<3, 3>(0, 0);
      H.block<3, 2>(0, 3 + 2 * g) += w * q.A.block<3, 2>(0, 3);
      H.block<2, 3>(3 + 2 * g, 0) += w * q.A.block<2, 3>(3, 0);
      H.block<2, 2>(3 + 2 * g, 3 + 2 * g) += w * w * q.A.block<2, 2>(3, 3);
      d.head<3>() += q.b.head<3>().transpose();
      d.segment<2>(3 + 2 * g) += w * q.b.segment<2>(3).transpose();
    }
    H = 0.5 * (H + H.transpose()).eval();
    VectorXd z = H.ldlt().solve(-d);
    TASK_CHECK(z.allFinite());
    TASK_CHECK((H * z + d).norm() < 1e-8);
    TASK_CHECK((cand.x - z.head<3>()).norm() < 1e-9);
    for(int g = 0; g < (int)cand.groups.size(); ++g)
    {
      TASK_CHECK((cand.groups[g].uv - z.segment<2>(3 + 2 * g)).norm() < 1e-9);
    }
    TASK_CHECK((cand.groups[0].uv - cand.groups[1].uv).norm() > 0.05);
    TASK_CHECK(std::isfinite(cand.cost));
    (void)c0;
    return 0;
  }

  int objective_octa_end_to_end()
  {
    TestMesh m = octa_mesh();
    MatrixXd U, S;
    MatrixXi G, FS;
    VectorXi J, I;
    TASK_CHECK(igl::qslim_with_uv(m.V, m.F, m.T, m.FT, 6, 1.0,
                                 U, G, S, FS, J, I));
    TASK_CHECK(valid_result(m, U, G, S, FS, J, I));
    TASK_CHECK(G.rows() == 6 && I.size() == 5);

    std::set<int> present;
    for(int f : {0, 1, 2, 3, 4, 5, 6, 7}) present.insert(f);
    for(int k = 0; k < J.size(); ++k) present.erase(J(k));
    TASK_CHECK(present.size() == 2);
    auto it = present.begin();
    const int f0 = *it;
    const int f1 = *++it;
    std::set<int> s0 = {m.F(f0, 0), m.F(f0, 1), m.F(f0, 2)};
    std::set<int> shared;
    for(int c = 0; c < 3; ++c) if(s0.count(m.F(f1, c))) shared.insert(m.F(f1, c));
    TASK_CHECK(shared.size() == 2);
    const int a = *shared.begin();
    const int b = *++shared.begin();
    const int survivor = std::min(a, b);
    int output_vertex = -1;
    for(int u = 0; u < I.size(); ++u) if(I(u) == survivor) output_vertex = u;
    TASK_CHECK(output_vertex >= 0);
    TASK_CHECK(U.row(output_vertex).allFinite());
    return 0;
  }

  int seams_real_continuous_decimation()
  {
    TestMesh m = sphere_mesh(12, 8);
    MatrixXd U, S;
    MatrixXi G, FS;
    VectorXi J, I;
    const bool reached = igl::qslim_with_uv(
      m.V, m.F, m.T, m.FT, 40, 1.0, U, G, S, FS, J, I);
    TASK_CHECK(reached);
    TASK_CHECK(valid_result(m, U, G, S, FS, J, I));
    const int initial_seams = count_seam_edges(m.F, m.FT);
    const int final_seams = count_seam_edges(G, FS);
    TASK_CHECK(initial_seams == 12);
    TASK_CHECK(final_seams > 0 && final_seams < initial_seams);
    const int collapses = (m.F.rows() - G.rows()) / 2;
    TASK_CHECK(collapses > final_seams + 4);
    TASK_CHECK(uv_component_count(FS, S.rows()) >= 2);

    VectorXi out_uv_for_input = VectorXi::Constant(m.T.rows(), -1);
    for(int k = 0; k < J.size(); ++k)
    {
      const int f = J(k);
      for(int c = 0; c < 3; ++c)
        out_uv_for_input(m.FT(f, c)) = FS(k, c);
    }
    int distinct_equal_pairs = 0;
    for(int i = 0; i < (int)m.north_seam.size(); ++i)
    {
      const int t1 = m.north_seam[i];
      const int t2 = m.south_seam[i];
      TASK_CHECK((m.T.row(t1) - m.T.row(t2)).norm() == 0.0);
      if(out_uv_for_input(t1) >= 0 && out_uv_for_input(t2) >= 0 &&
         out_uv_for_input(t1) != out_uv_for_input(t2))
      {
        ++distinct_equal_pairs;
      }
    }
    TASK_CHECK(distinct_equal_pairs >= 3);
    return 0;
  }

  int seams_aggressive_keeps_ring_and_maps()
  {
    TestMesh m = sphere_mesh(10, 6);
    MatrixXd U, S;
    MatrixXi G, FS;
    VectorXi J, I;
    const bool reached = igl::qslim_with_uv(
      m.V, m.F, m.T, m.FT, 0, 2.0, U, G, S, FS, J, I);
    TASK_CHECK(!reached);
    TASK_CHECK(valid_result(m, U, G, S, FS, J, I));
    TASK_CHECK(count_seam_edges(G, FS) >= 3);
    TASK_CHECK(uv_component_count(FS, S.rows()) >= 2);
    for(int target : {24, 16, 8})
    {
      MatrixXd U2, S2;
      MatrixXi G2, FS2;
      VectorXi J2, I2;
      bool ok = igl::qslim_with_uv(
        m.V, m.F, m.T, m.FT, target, 0.5, U2, G2, S2, FS2, J2, I2);
      TASK_CHECK(ok == (G2.rows() <= target));
      TASK_CHECK(valid_result(m, U2, G2, S2, FS2, J2, I2));
    }
    return 0;
  }

  int sequence_rejects_invalid_joint_placement()
  {
    TestMesh m = octa_mesh();
    MatrixXi E, EF, EI;
    VectorXi EMAP;
    igl::edge_flaps(m.F, E, EMAP, EF, EI);
    QslimUvState state;
    state.FT = m.FT;
    state.uv_weight = 1.0;
    state.parent.resize(m.T.rows());
    state.quadrics.resize(m.T.rows());
    state.uv.resize(m.T.rows());
    state.uv_orientation = VectorXd::Ones(m.F.rows());
    std::iota(state.parent.begin(), state.parent.end(), 0);
    for(int t = 0; t < m.T.rows(); ++t) state.uv[t] = m.T.row(t).transpose();

    int tested_good = 0;
    for(int e = 0; e < E.rows() && tested_good < 1; ++e)
    {
      QslimUvCandidate cand = qslim_uv_make_candidate(
        state, e, m.V, m.F, E, EMAP, EF, EI);
      if(!cand.ok) continue;
      const bool good = qslim_uv_candidate_admissible(
        state, cand, e, m.V, m.F, E, EMAP, EF, EI, VectorXi());
      if(!good) continue;
      ++tested_good;
      QslimUvCandidate bad = cand;
      bad.x += Vector3d(100.0, 37.0, -51.0);
      const bool bad_geometry = qslim_uv_candidate_admissible(
        state, bad, e, m.V, m.F, E, EMAP, EF, EI, VectorXi());
      TASK_CHECK(!bad_geometry);

      bad = cand;
      for(auto & g : bad.groups) g.uv << 100.0, -100.0;
      bool bad_uv = qslim_uv_candidate_admissible(
        state, bad, e, m.V, m.F, E, EMAP, EF, EI, VectorXi());
      if(bad_uv)
      {
        for(auto & g : bad.groups) g.uv << -100.0, 100.0;
        bad_uv = qslim_uv_candidate_admissible(
          state, bad, e, m.V, m.F, E, EMAP, EF, EI, VectorXi());
      }
      TASK_CHECK(!bad_uv);
    }
    TASK_CHECK(tested_good == 1);

    MatrixXd U, S;
    MatrixXi G, FS;
    VectorXi J, I;
    TASK_CHECK(igl::qslim_with_uv(m.V, m.F, m.T, m.FT, 6, 1.0,
                                 U, G, S, FS, J, I));
    TASK_CHECK(valid_result(m, U, G, S, FS, J, I));
    return 0;
  }

  int compatibility_noop_and_original_qslim()
  {
    TestMesh m = octa_mesh();
    MatrixXd U, S;
    MatrixXi G, FS;
    VectorXi J, I;
    TASK_CHECK(igl::qslim_with_uv(m.V, m.F, m.T, m.FT, 8, 1.0,
                                 U, G, S, FS, J, I));
    TASK_CHECK(G.rows() == 8);
    TASK_CHECK(J.size() == 8);
    for(int f = 0; f < 8; ++f) TASK_CHECK(J(f) == f);
    TASK_CHECK(valid_result(m, U, G, S, FS, J, I));

    MatrixXd qU, dU;
    MatrixXi qG, dG;
    VectorXi qJ, qI, dJ, dI;
    TASK_CHECK(igl::qslim(m.V, m.F, 6, false, qU, qG, qJ, qI));
    TASK_CHECK(qG.rows() == 6);
    TASK_CHECK(igl::decimate(m.V, m.F, 6, false, dU, dG, dJ, dI));
    TASK_CHECK(dG.rows() == 6);
    return 0;
  }

  int compatibility_tetra_cannot_continue()
  {
    TestMesh m;
    m.V.resize(4, 3);
    m.V << 1, 1, 1, -1, -1, 1, -1, 1, -1, 1, -1, -1;
    m.F.resize(4, 3);
    m.F << 0, 1, 2, 0, 3, 1, 0, 2, 3, 1, 3, 2;
    m.T.resize(4, 2);
    m.T << 0, 0, 1, 0, 0, 1, 1, 1;
    m.FT = m.F;
    MatrixXd U, S;
    MatrixXi G, FS;
    VectorXi J, I;
    const bool reached = igl::qslim_with_uv(
      m.V, m.F, m.T, m.FT, 0, 1.0, U, G, S, FS, J, I);
    TASK_CHECK(!reached);
    TASK_CHECK(G.rows() == 4);
    TASK_CHECK(valid_result(m, U, G, S, FS, J, I));
    return 0;
  }

  int compatibility_bad_inputs_keep_outputs()
  {
    TestMesh good = octa_mesh();
    const MatrixXd sentinelU = MatrixXd::Constant(2, 3, 17.0);
    const MatrixXd sentinelS = MatrixXd::Constant(2, 2, 19.0);
    const MatrixXi sentinelG = MatrixXi::Constant(2, 3, 23);
    const MatrixXi sentinelFS = MatrixXi::Constant(2, 3, 29);
    const VectorXi sentinelJ = VectorXi::Constant(2, 31);
    const VectorXi sentinelI = VectorXi::Constant(3, 37);

    const auto expect_bad = [&](
      const MatrixXd & V, const MatrixXi & F,
      const MatrixXd & T, const MatrixXi & FT,
      const int max_m, const double weight) -> int
    {
      MatrixXd U = sentinelU, S = sentinelS;
      MatrixXi G = sentinelG, FS = sentinelFS;
      VectorXi J = sentinelJ, I = sentinelI;
      bool threw = false;
      try
      {
        igl::qslim_with_uv(V, F, T, FT, max_m, weight, U, G, S, FS, J, I);
      }
      catch(const std::invalid_argument &)
      {
        threw = true;
      }
      TASK_CHECK(threw);
      TASK_CHECK(U.rows() == 2 && U(0, 0) == 17.0);
      TASK_CHECK(S.rows() == 2 && S(0, 0) == 19.0);
      TASK_CHECK(G.rows() == 2 && G(0, 0) == 23);
      TASK_CHECK(FS.rows() == 2 && FS(0, 0) == 29);
      TASK_CHECK(J.size() == 2 && J(0) == 31);
      TASK_CHECK(I.size() == 3 && I(0) == 37);
      return 0;
    };

    MatrixXi emptyF(0, 3);
    TASK_CHECK(expect_bad(good.V, emptyF, good.T, good.FT, 0, 1.0) == 0);
    TASK_CHECK(expect_bad(good.V, good.F, good.T, good.FT, -1, 1.0) == 0);
    TASK_CHECK(expect_bad(good.V, good.F, good.T, good.FT, 0, 0.0) == 0);
    TASK_CHECK(expect_bad(good.V, good.F, good.T, good.FT, 0, -2.0) == 0);
    TASK_CHECK(expect_bad(good.V, good.F, good.T, good.FT, 0,
                          std::numeric_limits<double>::infinity()) == 0);
    TASK_CHECK(expect_bad(good.V, good.F, good.T, good.FT, 0,
                          std::numeric_limits<double>::quiet_NaN()) == 0);

    MatrixXd badV = good.V;
    badV(0, 0) = std::numeric_limits<double>::quiet_NaN();
    TASK_CHECK(expect_bad(badV, good.F, good.T, good.FT, 0, 1.0) == 0);

    MatrixXi badF = good.F;
    badF(0, 0) = 99;
    TASK_CHECK(expect_bad(good.V, badF, good.T, good.FT, 0, 1.0) == 0);

    MatrixXi badFT = good.FT;
    badFT(0, 0) = 99;
    TASK_CHECK(expect_bad(good.V, good.F, good.T, badFT, 0, 1.0) == 0);

    MatrixXd degV(3, 3);
    degV << 0, 0, 0, 1, 0, 0, 2, 0, 0;
    MatrixXi degF(1, 3);
    degF << 0, 1, 2;
    MatrixXd degT(3, 2);
    degT << 0, 0, 1, 0, 2, 0;
    MatrixXi degFT = degF;
    TASK_CHECK(expect_bad(degV, degF, degT, degFT, 0, 1.0) == 0);

    MatrixXd goodTbadUv = good.T;
    goodTbadUv.row(4) << 0.2, 0.3;
    goodTbadUv.row(5) << 0.2, 0.3;
    // Force one referenced UV triangle to become collinear.
    MatrixXd collinearT(3, 2);
    collinearT << 0, 0, 1, 1, 2, 2;
    TASK_CHECK(expect_bad(degV, degF, collinearT, degFT, 0, 1.0) == 0);

    MatrixXi openF = good.F.topRows(7).eval();
    TASK_CHECK(expect_bad(good.V, openF, good.T, good.FT.topRows(7).eval(),
                          6, 1.0) == 0);
    return 0;
  }
}

const struct task_case task_cases[] = {
  {"objective", "joint_small_matrices", objective_joint_small_matrices},
  {"objective", "octa_end_to_end", objective_octa_end_to_end},
  {"seams", "sphere_identities_and_interior_collapses", seams_real_continuous_decimation},
  {"seams", "aggressive_ring_and_maps", seams_aggressive_keeps_ring_and_maps},
  {"sequence", "reject_invalid_placement", sequence_rejects_invalid_joint_placement},
  {"compatibility", "noop_original_and_decimate", compatibility_noop_and_original_qslim},
  {"compatibility", "tetra_cannot_continue", compatibility_tetra_cannot_continue},
  {"compatibility", "bad_inputs", compatibility_bad_inputs_keep_outputs},
};

const size_t task_case_count =
  sizeof(task_cases) / sizeof(task_cases[0]);
