///////////////////////////////////////////////////////////////////////////////
// Inverse design tests for the static ShapeOp solver.
//
// Registered through the task_support/check.h protocol (task_cases,
// task_case_count and TASK_CHECK). This translation unit defines no main().
// Finite differences appear only here and are never used to generate the
// expected values through the production pullback.
///////////////////////////////////////////////////////////////////////////////
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#include "task_support/check.h"
#include "Solver.h"
#include "Constraint.h"
#include "InverseDesign.h"

using namespace ShapeOp;

namespace {
///////////////////////////////////////////////////////////////////////////////
// A connected, non-planar triangle mesh (5 vertices, 4 triangles) with both
// in-plane triangle strain and edge strain constraints. Coordinates and
// weights are all of unit scale.
///////////////////////////////////////////////////////////////////////////////
struct Fixture {
  Matrix3X rest;
  std::vector<std::vector<int>> tris;
  std::vector<std::pair<int, int>> edges;
  static constexpr int nVerts = 5;
  static constexpr int nEdges = 7;
  static constexpr int nTris = 4;
  // Vertex 0 carries two closeness constraints (multi-control point), plus
  // one closeness constraint on every other vertex.
  static constexpr int nCloseness = 6;
  static constexpr int nConstraints = nEdges + nTris + nCloseness;

  Fixture() {
    rest.resize(3, nVerts);
    rest << 0.0, 1.0, 0.0, 1.0, 0.5,
            0.0, 0.0, 1.0, 1.0, 0.5,
            0.0, 0.0, 0.2, 0.5, 1.2;
    tris = {{0, 1, 4}, {1, 2, 4}, {1, 3, 2}, {2, 3, 4}};
    edges = {{0, 1}, {1, 2}, {2, 3}, {0, 4}, {1, 4}, {2, 4}, {3, 4}};
  }
};
///////////////////////////////////////////////////////////////////////////////
struct Design {
  // One independent target per closeness constraint, in addConstraint order.
  std::vector<Vector3> closenessTargets;
  std::vector<int> closenessVertex;
  std::vector<Scalar> weights;
};
///////////////////////////////////////////////////////////////////////////////
// Build a fresh static solver from a design and an initial geometry.
///////////////////////////////////////////////////////////////////////////////
std::shared_ptr<Solver> makeSolver(const Fixture &fx, const Design &d,
                                   const Matrix3X &initial,
                                   Scalar edgeLo = 0.9, Scalar edgeHi = 1.1,
                                   Scalar triLo = 0.85, Scalar triHi = 1.15) {
  auto s = std::make_shared<Solver>();
  s->setPoints(initial);
  int k = 0;
  for (auto e : fx.edges)
    s->addConstraint(std::make_shared<EdgeStrainConstraint>(
        std::vector<int>{e.first, e.second}, d.weights[k++], fx.rest, edgeLo, edgeHi));
  for (auto t : fx.tris)
    s->addConstraint(std::make_shared<TriangleStrainConstraint>(
        t, d.weights[k++], fx.rest, triLo, triHi));
  // Two controls on vertex 0, then one on every remaining vertex.
  for (std::size_t q = 0; q < d.closenessVertex.size(); ++q) {
    auto c = std::make_shared<ClosenessConstraint>(
        std::vector<int>{d.closenessVertex[q]}, d.weights[k++], fx.rest);
    c->setPosition(d.closenessTargets[q]);
    s->addConstraint(c);
  }
  s->initialize();
  return s;
}
///////////////////////////////////////////////////////////////////////////////
Design defaultDesign(const Fixture &fx) {
  Design d;
  // Closeness order: two controls on vertex 0, then one on vertices 1..4.
  d.closenessVertex = {0, 0, 1, 2, 3, 4};
  Matrix3X base = fx.rest;
  base.row(0) *= 1.45;
  base.row(1) *= 0.65;
  base(2, 0) = 0.10;
  base(2, 1) = 0.15;
  base(2, 2) = 0.40;
  base(2, 3) = 0.60;
  base(2, 4) = 1.70;
  d.closenessTargets.clear();
  for (int v : d.closenessVertex) d.closenessTargets.push_back(base.col(v));
  d.weights.resize(Fixture::nConstraints);
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> ud(0.3, 1.2);
  for (int i = 0; i < Fixture::nEdges + Fixture::nTris; ++i) d.weights[i] = ud(rng);
  for (int i = Fixture::nEdges + Fixture::nTris; i < Fixture::nConstraints; ++i)
    d.weights[i] = 9.0;
  return d;
}
///////////////////////////////////////////////////////////////////////////////
// Original addConstraint id of closeness target index q.
///////////////////////////////////////////////////////////////////////////////
int closenessConstraintId(int q) {
  return Fixture::nEdges + Fixture::nTris + q;
}
///////////////////////////////////////////////////////////////////////////////
// Geometry used as the initial guess for a design (the first control target of
// each vertex, i.e. closeness indices 0 and 2..5).
///////////////////////////////////////////////////////////////////////////////
Matrix3X initialGeometry(const Fixture &fx, const Design &d) {
  Matrix3X init = fx.rest;
  for (std::size_t q = 0; q < d.closenessVertex.size(); ++q)
    init.col(d.closenessVertex[q]) = d.closenessTargets[q];
  return init;
}
///////////////////////////////////////////////////////////////////////////////
EquilibriumOptions tightOptions() {
  EquilibriumOptions o;
  o.maxIterations = 200000;
  o.tolerance = 1e-13;
  o.branchTolerance = 1e-9;
  return o;
}
///////////////////////////////////////////////////////////////////////////////
// Independently rebuild A and the weighted projections for a geometry.
///////////////////////////////////////////////////////////////////////////////
void independentSystem(Solver &s, int nC, const Matrix3X &X,
                       SparseMatrix &A, Matrix3X &P) {
  std::vector<Triplet> trips;
  int idO = 0;
  for (int c = 0; c < nC; ++c) s.getConstraint(c)->addConstraint(trips, idO);
  A = SparseMatrix(idO, X.cols());
  A.setFromTriplets(trips.begin(), trips.end());
  P = Matrix3X(3, idO);
  for (int c = 0; c < nC; ++c) s.getConstraint(c)->project(X, P);
}
///////////////////////////////////////////////////////////////////////////////
// Independent energy E(X) = 1/2 sum_c ||A_c X^T - P_c(X)^T||_F^2.
///////////////////////////////////////////////////////////////////////////////
Scalar independentEnergy(Solver &s, int nC, const Matrix3X &X) {
  SparseMatrix A;
  Matrix3X P;
  independentSystem(s, nC, X, A, P);
  MatrixXX r = A * X.transpose() - P.transpose();
  return 0.5 * r.squaredNorm();
}
///////////////////////////////////////////////////////////////////////////////
// Independent gradient of E by central differences of the scalar energy.
// This never calls the production projection Jacobian or pullback.
///////////////////////////////////////////////////////////////////////////////
Matrix3X independentGradient(Solver &s, int nC, const Matrix3X &X, Scalar h) {
  Matrix3X g(3, X.cols());
  for (int j = 0; j < X.cols(); ++j) {
    for (int a = 0; a < 3; ++a) {
      Matrix3X xp = X, xm = X;
      xp(a, j) += h;
      xm(a, j) -= h;
      g(a, j) = (independentEnergy(s, nC, xp) - independentEnergy(s, nC, xm)) / (2.0 * h);
    }
  }
  return g;
}
///////////////////////////////////////////////////////////////////////////////
bool withinTolerance(Scalar obtained, Scalar expected) {
  return std::abs(obtained - expected) <= 2e-5 + 2e-5 * std::abs(expected);
}
///////////////////////////////////////////////////////////////////////////////
// Re-solve from the same initial guess for a perturbed design, returning the
// converged geometry. Stays on the same smooth branch for small steps.
///////////////////////////////////////////////////////////////////////////////
Matrix3X resolveGeometry(const Fixture &fx, const Design &d, const Matrix3X &initial,
                         const EquilibriumOptions &o) {
  auto s = makeSolver(fx, d, initial);
  EquilibriumReport r;
  auto snap = s->solveEquilibrium(o, r);
  if (!snap) throw std::runtime_error("finite-difference re-solve failed");
  return snap->points();
}
///////////////////////////////////////////////////////////////////////////////
Scalar objective(const Matrix3X &G, const Matrix3X &X) {
  return (G.array() * X.array()).sum();
}
///////////////////////////////////////////////////////////////////////////////
// Count edge length ratios that are strictly beyond a strain bound.
///////////////////////////////////////////////////////////////////////////////
int activeEdgeClamps(const Fixture &fx, const Matrix3X &X,
                     Scalar lo, Scalar hi) {
  int count = 0;
  for (auto e : fx.edges) {
    Scalar ratio = (X.col(e.second) - X.col(e.first)).norm() /
                   (fx.rest.col(e.second) - fx.rest.col(e.first)).norm();
    if (ratio < lo - 1e-6 || ratio > hi + 1e-6) ++count;
  }
  return count;
}
} // namespace
///////////////////////////////////////////////////////////////////////////////
// EQUILIBRIUM GROUP
///////////////////////////////////////////////////////////////////////////////
static int equilibrium_energy_and_gradient() {
  Fixture fx;
  Design d = defaultDesign(fx);
  auto s = makeSolver(fx, d, initialGeometry(fx, d));
  EquilibriumReport r;
  auto snap = s->solveEquilibrium(tightOptions(), r);
  TASK_CHECK(snap != nullptr);
  TASK_CHECK(r.success());
  TASK_CHECK(r.iterations > 1); // A genuinely iterated local/global solve.

  // Independent energy.
  Scalar energy = independentEnergy(*s, Fixture::nConstraints, snap->points());
  TASK_CHECK(std::abs(energy - r.energy) <= 1e-10 * (1.0 + energy));

  // Independent gradient residual by finite-differencing the energy.
  Matrix3X g = independentGradient(*s, Fixture::nConstraints, snap->points(), 1e-6);
  Scalar gradInf = g.cwiseAbs().maxCoeff();
  TASK_CHECK(gradInf <= 1e-7);
  TASK_CHECK(std::abs(gradInf - r.gradientResidual) <= 1e-7);

  // Active clamping is present, yet the projection stays smooth.
  TASK_CHECK(activeEdgeClamps(fx, snap->points(), 0.9, 1.1) >= 3);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int equilibrium_budget_and_unchanged_points() {
  Fixture fx;
  Design d = defaultDesign(fx);
  auto s = makeSolver(fx, d, initialGeometry(fx, d));
  Matrix3X before = s->getPoints();
  EquilibriumOptions o = tightOptions();
  o.maxIterations = 2; // Impossible to converge.
  EquilibriumReport r;
  auto snap = s->solveEquilibrium(o, r);
  TASK_CHECK(snap == nullptr);
  TASK_CHECK(r.status == EquilibriumStatus::BudgetExceeded);
  TASK_CHECK(!r.success());
  // Failed trial steps are not published.
  TASK_CHECK((s->getPoints() - before).cwiseAbs().maxCoeff() < 1e-15);

  // A fixed iteration count is never accepted as convergence.
  EquilibriumOptions loose = tightOptions();
  loose.tolerance = 1e-3;
  EquilibriumReport r2;
  auto snap2 = s->solveEquilibrium(loose, r2);
  TASK_CHECK(snap2 != nullptr);
  TASK_CHECK(r2.gradientResidual <= loose.tolerance);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int equilibrium_rejects_unsupported_and_invalid() {
  Fixture fx;
  Design d = defaultDesign(fx);
  auto s = makeSolver(fx, d, initialGeometry(fx, d));
  // A constraint type outside the supported set must be refused.
  s->addConstraint(std::make_shared<BendingConstraint>(
      std::vector<int>{0, 1, 4, 2}, 1.0, fx.rest, 0.9, 1.1));
  EquilibriumReport r;
  TASK_CHECK(s->solveEquilibrium(tightOptions(), r) == nullptr);
  TASK_CHECK(r.status == EquilibriumStatus::InvalidInput);

  // Non-positive weights are rejected.
  bool threw = false;
  try { s->setConstraintWeight(0, 0.0); } catch (const std::invalid_argument &) { threw = true; }
  TASK_CHECK(threw);
  threw = false;
  try { s->setConstraintWeight(0, -2.0); } catch (const std::invalid_argument &) { threw = true; }
  TASK_CHECK(threw);
  bool outOfRange = false;
  try { s->setConstraintWeight(999, 1.0); } catch (const std::out_of_range &) { outOfRange = true; }
  TASK_CHECK(outOfRange);

  // Bad equilibrium options.
  auto s2 = makeSolver(fx, d, initialGeometry(fx, d));
  EquilibriumOptions bad = tightOptions();
  bad.tolerance = 0.0;
  EquilibriumReport r2;
  TASK_CHECK(s2->solveEquilibrium(bad, r2) == nullptr);
  TASK_CHECK(r2.status == EquilibriumStatus::InvalidInput);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int equilibrium_nonsmooth_and_degenerate() {
  Fixture fx;
  Design d = defaultDesign(fx);

  // A large branch tolerance catches a strain value next to a clamp bound.
  auto s = makeSolver(fx, d, initialGeometry(fx, d));
  EquilibriumOptions o = tightOptions();
  o.branchTolerance = 0.25;
  EquilibriumReport r;
  Matrix3X before = s->getPoints();
  TASK_CHECK(s->solveEquilibrium(o, r) == nullptr);
  TASK_CHECK(r.status == EquilibriumStatus::NonSmooth);
  TASK_CHECK((s->getPoints() - before).cwiseAbs().maxCoeff() < 1e-15);

  // Collapse an edge by pinning its two endpoints to the same target.
  Design dd = d;
  std::fill(dd.weights.begin(), dd.weights.end(), 0.2);
  for (int i = Fixture::nEdges + Fixture::nTris; i < Fixture::nConstraints; ++i)
    dd.weights[i] = 100.0;
  Matrix3X collapsed = fx.rest;
  collapsed.col(1) = collapsed.col(0);
  auto s2 = makeSolver(fx, dd, collapsed);
  EquilibriumReport r2;
  TASK_CHECK(s2->solveEquilibrium(tightOptions(), r2) == nullptr);
  TASK_CHECK(r2.status == EquilibriumStatus::DegenerateGeometry);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int equilibrium_singular_tangent_is_distinct() {
  Fixture fx;
  // Connected graph, a single anchor and tightly fixed strains: the global
  // solve is still well posed, but the equilibrium tangent has unconstrained
  // rigid/transverse modes and must be reported as singular, not silently
  // regularized into a successful gradient.
  Matrix3X init = fx.rest;
  init.row(0) *= 1.5;
  auto s = std::make_shared<Solver>();
  s->setPoints(init);
  for (auto e : fx.edges)
    s->addConstraint(std::make_shared<EdgeStrainConstraint>(
        std::vector<int>{e.first, e.second}, 1.0, fx.rest, 1.0, 1.0));
  for (auto t : fx.tris)
    s->addConstraint(std::make_shared<TriangleStrainConstraint>(
        t, 1.0, fx.rest, 1.0, 1.0));
  auto anchor = std::make_shared<ClosenessConstraint>(std::vector<int>{0}, 50.0, fx.rest);
  s->addConstraint(anchor);
  s->initialize();

  EquilibriumOptions o;
  o.maxIterations = 200000;
  o.tolerance = 1e-12;
  o.branchTolerance = 1e-8;
  EquilibriumReport r;
  Matrix3X before = s->getPoints();
  TASK_CHECK(s->solveEquilibrium(o, r) == nullptr);
  TASK_CHECK(r.status == EquilibriumStatus::SingularTangent);
  TASK_CHECK(!r.success());
  // No trial geometry is published on failure.
  TASK_CHECK((s->getPoints() - before).cwiseAbs().maxCoeff() < 1e-15);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
// ADJOINT GROUP
///////////////////////////////////////////////////////////////////////////////
static int adjoint_pullback_matches_central_differences() {
  Fixture fx;
  Design d = defaultDesign(fx);
  Matrix3X initial = initialGeometry(fx, d);
  auto s = makeSolver(fx, d, initial);
  EquilibriumReport r;
  auto snap = s->solveEquilibrium(tightOptions(), r);
  TASK_CHECK(snap != nullptr);
  TASK_CHECK(activeEdgeClamps(fx, snap->points(), 0.9, 1.1) >= 3);

  std::mt19937 rng(23);
  std::normal_distribution<double> nd(0.0, 1.0);
  Matrix3X G(3, Fixture::nVerts);
  for (int j = 0; j < G.cols(); ++j)
    for (int a = 0; a < 3; ++a) G(a, j) = nd(rng);

  PullbackResult pb = snap->pullback(G);
  TASK_CHECK(pb.valid);
  TASK_CHECK(pb.adjointResidual <= 1e-11);

  const Scalar steps[2] = {1e-4, 3e-5};
  for (Scalar eps : steps) {
    // Every Closeness target 3D coordinate, one independent control at a time,
    // indexed by the original addConstraint id.
    for (std::size_t q = 0; q < d.closenessTargets.size(); ++q) {
      int cid = closenessConstraintId(static_cast<int>(q));
      for (int a = 0; a < 3; ++a) {
        Design dp = d, dm = d;
        dp.closenessTargets[q](a) += eps;
        dm.closenessTargets[q](a) -= eps;
        Scalar fp = objective(G, resolveGeometry(fx, dp, initial, tightOptions()));
        Scalar fm = objective(G, resolveGeometry(fx, dm, initial, tightOptions()));
        Scalar expected = (fp - fm) / (2.0 * eps);
        TASK_CHECK(withinTolerance(pb.targetGradients(a, cid), expected));
      }
    }
    // Every original user constraint weight.
    for (int c = 0; c < Fixture::nConstraints; ++c) {
      Design dp = d, dm = d;
      dp.weights[c] += eps;
      dm.weights[c] -= eps;
      TASK_CHECK(dm.weights[c] > 0.0);
      Scalar fp = objective(G, resolveGeometry(fx, dp, initial, tightOptions()));
      Scalar fm = objective(G, resolveGeometry(fx, dm, initial, tightOptions()));
      Scalar expected = (fp - fm) / (2.0 * eps);
      TASK_CHECK(withinTolerance(pb.weightGradients[c], expected));
    }
  }
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int adjoint_clamp_switch_branch() {
  // As the strain bound sweeps through the realized length ratio, the edge
  // projection switches between clamped and interior branches. On either side
  // the smooth pullback stays finite-difference accurate; exactly on the
  // threshold the non-smooth boundary is reported rather than differentiated.
  Fixture fx;
  Design d = defaultDesign(fx);
  Matrix3X initial = initialGeometry(fx, d);

  auto solveWithEdgeBound = [&](Scalar hi, EquilibriumStatus &st,
                                std::shared_ptr<InverseDesignSnapshot> &snap) {
    auto s = makeSolver(fx, d, initial, 0.9, hi, 0.85, 1.15);
    EquilibriumReport r;
    snap = s->solveEquilibrium(tightOptions(), r);
    st = r.status;
    return s;
  };

  // Tight upper bound: the first edge is upper-clamped (ratio above the limit).
  EquilibriumStatus stTight;
  std::shared_ptr<InverseDesignSnapshot> snapTight;
  auto sTight = solveWithEdgeBound(1.0, stTight, snapTight);
  TASK_CHECK(stTight == EquilibriumStatus::Success);
  TASK_CHECK(snapTight != nullptr);

  // Relaxed upper bound: the same edge moves into the interior branch.
  EquilibriumStatus stLoose;
  std::shared_ptr<InverseDesignSnapshot> snapLoose;
  auto sLoose = solveWithEdgeBound(1.6, stLoose, snapLoose);
  TASK_CHECK(stLoose == EquilibriumStatus::Success);
  TASK_CHECK(snapLoose != nullptr);

  // A target VJP stays accurate on both sides of the branch switch.
  std::mt19937 rng(53);
  std::normal_distribution<double> nd(0.0, 1.0);
  Matrix3X G(3, Fixture::nVerts);
  for (int j = 0; j < G.cols(); ++j)
    for (int a = 0; a < 3; ++a) G(a, j) = nd(rng);
  int qControl = 3, cid = closenessConstraintId(qControl);
  const Scalar eps = 3e-5;
  for (int side = 0; side < 2; ++side) {
    Scalar hi = side == 0 ? 1.0 : 1.6;
    auto snap = side == 0 ? snapTight : snapLoose;
    PullbackResult pb = snap->pullback(G);
    TASK_CHECK(pb.valid);
    Design dp = d, dm = d;
    dp.closenessTargets[qControl](0) += eps;
    dm.closenessTargets[qControl](0) -= eps;
    auto L = [&](const Design &dd) {
      auto ss = makeSolver(fx, dd, initial, 0.9, hi, 0.85, 1.15);
      EquilibriumReport rr;
      auto sn = ss->solveEquilibrium(tightOptions(), rr);
      if (!sn) throw std::runtime_error("branch FD re-solve failed");
      return objective(G, sn->points());
    };
    Scalar expected = (L(dp) - L(dm)) / (2.0 * eps);
    TASK_CHECK(withinTolerance(pb.targetGradients(0, cid), expected));
  }

  // A strain value sitting within the caller's branch tolerance of a bound is
  // non-smooth. Measure the nearest-bound distance of any edge ratio and set a
  // branch tolerance that provably reaches it, so the boundary is refused.
  Scalar nearest = std::numeric_limits<Scalar>::infinity();
  for (auto e : fx.edges) {
    Scalar ratio = (snapTight->points().col(e.second) - snapTight->points().col(e.first)).norm() /
                   (fx.rest.col(e.second) - fx.rest.col(e.first)).norm();
    nearest = std::min(nearest, std::min(std::abs(ratio - 0.9), std::abs(ratio - 1.0)));
  }
  EquilibriumOptions o = tightOptions();
  o.branchTolerance = nearest * 2.0 + 1e-12;
  auto sBoundary = makeSolver(fx, d, initial, 0.9, 1.0, 0.85, 1.15);
  EquilibriumReport rb;
  TASK_CHECK(sBoundary->solveEquilibrium(o, rb) == nullptr);
  TASK_CHECK(rb.status == EquilibriumStatus::NonSmooth);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int adjoint_three_axis_coupling_vs_frozen_tangent() {
  Fixture fx;
  Design d = defaultDesign(fx);
  Matrix3X initial = initialGeometry(fx, d);
  auto s = makeSolver(fx, d, initial);
  EquilibriumReport r;
  auto snap = s->solveEquilibrium(tightOptions(), r);
  TASK_CHECK(snap != nullptr);

  // The real tangent couples the three coordinate axes (off-axis entries).
  const SparseMatrix &K = snap->tangent();
  bool crossAxis = false;
  for (int k = 0; k < K.outerSize(); ++k) {
    for (SparseMatrix::InnerIterator it(K, k); it; ++it) {
      if (it.row() % 3 != it.col() % 3 && std::abs(it.value()) > 1e-12) crossAxis = true;
    }
  }
  TASK_CHECK(crossAxis);

  // A frozen-projection tangent I3 kron (A^T A) must be measurably wrong on
  // the clamped constraints, while the analytic pullback is correct.
  const SparseMatrix &A = snap->systemMatrix();
  SparseMatrix N = A.transpose() * A;
  SparseMatrix frozen(3 * Fixture::nVerts, 3 * Fixture::nVerts);
  std::vector<Triplet> trips;
  for (int k = 0; k < N.outerSize(); ++k)
    for (SparseMatrix::InnerIterator it(N, k); it; ++it)
      for (int a = 0; a < 3; ++a)
        trips.emplace_back(3 * it.row() + a, 3 * it.col() + a, it.value());
  frozen.setFromTriplets(trips.begin(), trips.end());
  Eigen::SimplicialLDLT<SparseMatrix> ld;
  ld.compute(frozen);
  TASK_CHECK(ld.info() == Eigen::Success);

  Matrix3X G(3, Fixture::nVerts);
  G.setZero();
  G(0, 2) = 1.0; // Excitation primarily in x; cross-axis response expected.
  VectorX g(3 * Fixture::nVerts);
  for (int j = 0; j < Fixture::nVerts; ++j)
    for (int a = 0; a < 3; ++a) g[3 * j + a] = G(a, j);
  VectorX lambdaFrozen = ld.solve(g);
  PullbackResult pb = snap->pullback(G);
  TASK_CHECK(pb.valid);

  // Finite-difference reference for one target VJP (the control on vertex 2).
  int cid = Fixture::nEdges + Fixture::nTris + 3;
  int qControl = 3; // closenessTargets index acting on vertex 2.
  const Scalar eps = 3e-5;
  Design dp = d, dm = d;
  dp.closenessTargets[qControl](0) += eps;
  dm.closenessTargets[qControl](0) -= eps;
  Scalar expected = (objective(G, resolveGeometry(fx, dp, initial, tightOptions())) -
                     objective(G, resolveGeometry(fx, dm, initial, tightOptions()))) /
                    (2.0 * eps);
  Scalar frozenVjp = d.weights[cid] * lambdaFrozen[3 * 2 + 0];
  TASK_CHECK(withinTolerance(pb.targetGradients(0, cid), expected));
  TASK_CHECK(std::abs(frozenVjp - expected) > 1e-3);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int adjoint_repeated_singular_values_fixed_extension() {
  Fixture fx;
  // Isotropic, consistent design: every target is a uniform scaling of rest,
  // and all strain bounds are fixed at exactly that scale. At equilibrium each
  // triangle deformation gradient is scale*I: repeated singular values, both
  // clamped to the same fixed bound, inside one smooth projection branch.
  const Scalar scale = 1.05;
  Matrix3X initial = scale * fx.rest;
  Design d;
  d.closenessVertex = {0, 0, 1, 2, 3, 4};
  d.closenessTargets.clear();
  for (int v : d.closenessVertex) d.closenessTargets.push_back(initial.col(v));
  d.weights.resize(Fixture::nConstraints);
  for (int i = 0; i < Fixture::nEdges + Fixture::nTris; ++i) d.weights[i] = 1.0;
  for (int i = Fixture::nEdges + Fixture::nTris; i < Fixture::nConstraints; ++i)
    d.weights[i] = 20.0;

  auto s = makeSolver(fx, d, initial, scale, scale, scale, scale);
  EquilibriumReport r;
  auto snap = s->solveEquilibrium(tightOptions(), r);
  TASK_CHECK(snap != nullptr);
  TASK_CHECK(r.success());

  // Confirm a triangle really has coincident singular values at the fixed bound.
  int triConstraint = Fixture::nEdges; // First triangle.
  const auto &t = fx.tris[0];
  Matrix32 E;
  E.col(0) = snap->points().col(t[1]) - snap->points().col(t[0]);
  E.col(1) = snap->points().col(t[2]) - snap->points().col(t[0]);
  Matrix32 frame;
  frame.col(0) = E.col(0).normalized();
  frame.col(1) = (E.col(1) - E.col(1).dot(frame.col(0)) * frame.col(0)).normalized();
  Matrix22 F = frame.transpose() * E *
               std::static_pointer_cast<TriangleStrainConstraint>(
                   s->getConstraint(triConstraint))->inverseRestMatrix();
  Eigen::JacobiSVD<Matrix22> sv(F);
  Vector2 svd = sv.singularValues();
  TASK_CHECK(std::abs(svd(0) - svd(1)) <= 1e-9);
  TASK_CHECK(std::abs(svd(0) - scale) <= 1e-9);

  // The analytic triangle projection Jacobian stays finite and matches finite
  // differences even though the SVD vectors are not unique.
  const MatrixXX &Ja = snap->projectionJacobian(triConstraint);
  TASK_CHECK(Ja.allFinite());
  int row = snap->constraintRow(triConstraint);
  int rows = snap->constraintRowEnd(triConstraint) - row;
  const auto &ids = snap->constraintIndices(triConstraint);
  SparseMatrix A;
  Matrix3X P;
  independentSystem(*s, Fixture::nConstraints, snap->points(), A, P);
  auto block = [&](const Matrix3X &X) {
    Matrix3X Q(3, A.rows());
    for (int c = 0; c < Fixture::nConstraints; ++c)
      s->getConstraint(c)->project(X, Q);
    VectorX v(3 * rows);
    for (int rr = 0; rr < rows; ++rr)
      for (int a = 0; a < 3; ++a) v[3 * rr + a] = Q(a, row + rr);
    return v;
  };
  Scalar h = 1e-7, worst = 0.0;
  for (int q = 0; q < 3 * static_cast<int>(ids.size()); ++q) {
    Matrix3X xp = snap->points(), xm = snap->points();
    xp(q % 3, ids[q / 3]) += h;
    xm(q % 3, ids[q / 3]) -= h;
    VectorX fd = (block(xp) - block(xm)) / (2.0 * h);
    worst = std::max(worst, (fd - Ja.col(q)).cwiseAbs().maxCoeff());
  }
  TASK_CHECK(worst <= 1e-6);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int adjoint_weight_update_rebuilds_matrices() {
  Fixture fx;
  Design d = defaultDesign(fx);
  auto s = makeSolver(fx, d, initialGeometry(fx, d));
  EquilibriumReport r;
  auto snapBefore = s->solveEquilibrium(tightOptions(), r);
  TASK_CHECK(snapBefore != nullptr);
  Scalar oldEntry = snapBefore->systemMatrix().coeff(0, 0);

  const int id = 0;
  Scalar newWeight = 7.0;
  s->setConstraintWeight(id, newWeight);
  TASK_CHECK(std::abs(s->getConstraint(id)->getWeight() - newWeight) < 1e-15);

  EquilibriumReport r2;
  auto snapAfter = s->solveEquilibrium(tightOptions(), r2);
  TASK_CHECK(snapAfter != nullptr);
  // The participating matrix genuinely changed.
  TASK_CHECK(std::abs(snapAfter->systemMatrix().coeff(0, 0) - oldEntry) > 1e-6);

  // Weight VJP after the update still matches finite differences.
  Design d2 = d;
  d2.weights[id] = newWeight;
  std::mt19937 rng(31);
  std::normal_distribution<double> nd(0.0, 1.0);
  Matrix3X G(3, Fixture::nVerts);
  for (int j = 0; j < G.cols(); ++j)
    for (int a = 0; a < 3; ++a) G(a, j) = nd(rng);
  PullbackResult pb = snapAfter->pullback(G);
  const Scalar eps = 3e-5;
  Design dp = d2, dm = d2;
  dp.weights[id] += eps;
  dm.weights[id] -= eps;
  Scalar expected = (objective(G, resolveGeometry(fx, dp, initialGeometry(fx, d2), tightOptions())) -
                     objective(G, resolveGeometry(fx, dm, initialGeometry(fx, d2), tightOptions()))) /
                    (2.0 * eps);
  TASK_CHECK(withinTolerance(pb.weightGradients[id], expected));
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
// LIFECYCLE GROUP
///////////////////////////////////////////////////////////////////////////////
static int lifecycle_snapshot_survives_destroyed_solver() {
  Fixture fx;
  Design d = defaultDesign(fx);
  Matrix3X G(3, Fixture::nVerts);
  G.setConstant(0.37);
  PullbackResult kept;
  Matrix3X keptPoints;
  std::shared_ptr<InverseDesignSnapshot> surviving;
  {
    auto s = makeSolver(fx, d, initialGeometry(fx, d));
    EquilibriumReport r;
    auto snap = s->solveEquilibrium(tightOptions(), r);
    TASK_CHECK(snap != nullptr);
    surviving = snap; // Owned snapshot outlives the Solver and Constraints.
    kept = snap->pullback(G);
    keptPoints = snap->points();
    // Solver and constraints go out of scope and are destroyed here.
  }
  // The owning snapshot keeps geometry, targets, materials and factorization.
  PullbackResult again = surviving->pullback(G);
  TASK_CHECK(again.valid);
  TASK_CHECK((again.targetGradients - kept.targetGradients).cwiseAbs().maxCoeff() < 1e-18);
  TASK_CHECK((again.weightGradients - kept.weightGradients).cwiseAbs().maxCoeff() < 1e-18);
  TASK_CHECK(again.adjointResidual <= 1e-11);
  TASK_CHECK(keptPoints.allFinite());
  TASK_CHECK((surviving->points() - keptPoints).cwiseAbs().maxCoeff() < 1e-18);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int lifecycle_two_snapshots_interleaved_and_repeatable() {
  Fixture fx;
  Design d1 = defaultDesign(fx);
  Design d2 = d1;
  for (auto &t : d2.closenessTargets) t(0) *= 0.8;
  for (auto &w : d2.weights) w *= 1.3;

  auto s1 = makeSolver(fx, d1, initialGeometry(fx, d1));
  auto s2 = makeSolver(fx, d2, initialGeometry(fx, d2));
  EquilibriumReport r1, r2;
  auto snap1 = s1->solveEquilibrium(tightOptions(), r1);
  auto snap2 = s2->solveEquilibrium(tightOptions(), r2);
  TASK_CHECK(snap1 && snap2);

  std::mt19937 rng(41);
  std::normal_distribution<double> nd(0.0, 1.0);
  Matrix3X G(3, Fixture::nVerts);
  for (int j = 0; j < G.cols(); ++j)
    for (int a = 0; a < 3; ++a) G(a, j) = nd(rng);

  // Interleaved pullbacks on the shared linearizations.
  PullbackResult a1 = snap1->pullback(G);
  PullbackResult b1 = snap2->pullback(G);
  PullbackResult a2 = snap1->pullback(G);
  PullbackResult b2 = snap2->pullback(G);

  // Repeated calls do not accumulate and are bit-for-bit repeatable.
  TASK_CHECK((a1.targetGradients - a2.targetGradients).cwiseAbs().maxCoeff() == 0.0);
  TASK_CHECK((b1.targetGradients - b2.targetGradients).cwiseAbs().maxCoeff() == 0.0);
  TASK_CHECK((a1.weightGradients - a2.weightGradients).cwiseAbs().maxCoeff() == 0.0);
  // The two snapshots describe different geometries and give different VJPs.
  TASK_CHECK((snap1->points() - snap2->points()).cwiseAbs().maxCoeff() > 1e-4);
  TASK_CHECK((a1.targetGradients - b1.targetGradients).cwiseAbs().maxCoeff() > 1e-6);

  // Editing the source constraints after the fact cannot mutate the snapshot.
  Matrix3X oldTargets = a1.targetGradients;
  s1->setConstraintWeight(Fixture::nEdges + Fixture::nTris, 3.0);
  PullbackResult a3 = snap1->pullback(G);
  TASK_CHECK((a3.targetGradients - oldTargets).cwiseAbs().maxCoeff() == 0.0);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
static int lifecycle_gradient_descent_example_reduces_error() {
  // Mirrors the header usage example: pullback on a mesh-target error, take a
  // small gradient step on the soft targets, and confirm the re-solved error
  // decreases.
  Fixture fx;
  Design d = defaultDesign(fx);
  Matrix3X desired = fx.rest;
  auto s = makeSolver(fx, d, initialGeometry(fx, d));
  EquilibriumReport r;
  auto snap = s->solveEquilibrium(tightOptions(), r);
  TASK_CHECK(snap != nullptr);

  Matrix3X G = snap->points() - desired; // Gradient of 1/2 ||X - desired||^2.
  PullbackResult pb = snap->pullback(G);
  Scalar before = (snap->points() - desired).squaredNorm();

  const Scalar step = 0.2;
  Design stepped = d;
  for (std::size_t q = 0; q < d.closenessTargets.size(); ++q) {
    int cid = closenessConstraintId(static_cast<int>(q));
    for (int a = 0; a < 3; ++a)
      stepped.closenessTargets[q](a) -= step * pb.targetGradients(a, cid);
  }
  auto s2 = makeSolver(fx, stepped, initialGeometry(fx, stepped));
  EquilibriumReport r2;
  auto snap2 = s2->solveEquilibrium(tightOptions(), r2);
  TASK_CHECK(snap2 != nullptr);
  Scalar after = (snap2->points() - desired).squaredNorm();
  TASK_CHECK(after < before);
  return 0;
}
///////////////////////////////////////////////////////////////////////////////
// Registration.
///////////////////////////////////////////////////////////////////////////////
#define TASK_CASE(group, name) { group, #name, &name}

extern const struct task_case task_cases[] = {
    TASK_CASE("equilibrium", equilibrium_energy_and_gradient),
    TASK_CASE("equilibrium", equilibrium_budget_and_unchanged_points),
    TASK_CASE("equilibrium", equilibrium_rejects_unsupported_and_invalid),
    TASK_CASE("equilibrium", equilibrium_nonsmooth_and_degenerate),
    TASK_CASE("equilibrium", equilibrium_singular_tangent_is_distinct),
    TASK_CASE("adjoint", adjoint_pullback_matches_central_differences),
    TASK_CASE("adjoint", adjoint_clamp_switch_branch),
    TASK_CASE("adjoint", adjoint_three_axis_coupling_vs_frozen_tangent),
    TASK_CASE("adjoint", adjoint_repeated_singular_values_fixed_extension),
    TASK_CASE("adjoint", adjoint_weight_update_rebuilds_matrices),
    TASK_CASE("lifecycle", lifecycle_snapshot_survives_destroyed_solver),
    TASK_CASE("lifecycle", lifecycle_two_snapshots_interleaved_and_repeatable),
    TASK_CASE("lifecycle", lifecycle_gradient_descent_example_reduces_error),
};
const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
