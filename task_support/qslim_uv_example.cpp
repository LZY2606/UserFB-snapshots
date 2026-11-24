// Minimal C++17 example for igl::qslim_with_uv.
//
// Build from the repository root with the bundled Eigen:
//   c++ -std=c++17 -I. -Iinclude -I.deps/eigen \
//       task_support/qslim_uv_example.cpp -o .task-build/qslim_uv_example
//
// Scope: qslim_with_uv validates finite, positive-area, closed oriented
// two-manifold geometry and non-degenerate UV triangles whose seam graph is a
// disjoint union of simple loops of at least three edges.  It reuses igl's
// decimate/collapse_least_cost_edge/collapse_edge queue and topology commit,
// while keeping UV wedges independent across seams.
#include <igl/qslim.h>

#include <iostream>

int main()
{
  Eigen::MatrixXd V(6, 3);
  V << 1, 0, 0,
      -1, 0, 0,
       0, 1, 0,
       0,-1, 0,
       0, 0, 1,
       0, 0,-1;

  Eigen::MatrixXi F(8, 3);
  F << 0, 2, 4,
       2, 1, 4,
       1, 3, 4,
       3, 0, 4,
       2, 0, 5,
       1, 2, 5,
       3, 1, 5,
       0, 3, 5;

  // A compact UV atlas.  Distinct FT rows with equal T values remain distinct
  // wedge identities when a texture seam is present.
  Eigen::MatrixXd T(6, 2);
  T << 1, 0, -1, 0, 0, 1, 0, -1, 0.2, 0.3, -0.2, -0.3;
  Eigen::MatrixXi FT = F;

  Eigen::MatrixXd U, S;
  Eigen::MatrixXi G, FS;
  Eigen::VectorXi J, I;

  constexpr int max_faces = 6;
  constexpr double uv_weight = 1.0;
  const bool reached = igl::qslim_with_uv(
    V, F, T, FT, max_faces, uv_weight, U, G, S, FS, J, I);

  std::cout << "reached=" << reached
            << " faces=" << G.rows()
            << " geometry_vertices=" << U.rows()
            << " uv_vertices=" << S.rows() << '\n';
  return G.rows() <= max_faces ? 0 : 1;
}
