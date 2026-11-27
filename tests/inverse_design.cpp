///////////////////////////////////////////////////////////////////////////////
// Inverse design tests: equilibrium solving, implicit adjoint pullback and
// snapshot lifecycle. Finite differences appear only in this translation
// unit, never in the production code; the expected values are computed from
// independent re-derivations of the energy and its projection.
///////////////////////////////////////////////////////////////////////////////
#include "task_support/check.h"
#include "Solver.h"
#include "Constraint.h"
#include "InverseDesign.h"
#include <memory>
#include <vector>
#include <cmath>

using namespace ShapeOp;

namespace {

enum Kind { EDGE, TRIANGLE, CLOSENESS };
struct Spec {
  Kind kind;
  std::vector<int> idx;
  double w;
  double rmin = 1.0, rmax = 1.0;
  Vector3 target = Vector3::Zero();
};

const double kTol = 1e-11;       // equilibrium gradient tolerance
const double kBranch = 1e-8;     // branch tolerance
const unsigned kBudget = 200000;

// Connected, non-planar, deliberately non-axis-aligned reference mesh (a
// tetrahedron corner), 4 vertices, so the tangent has genuine three-axis
// cross-coupling rather than an artificial block-diagonal structure.
Matrix3X referenceMesh() {
  Matrix3X p(3, 4);
  p << 1.00, 1.30, 0.20, 0.15,
       0.20, 0.40, 1.25, 0.30,
       0.10, 0.35, 0.20, 1.10;
  return p;
}

// The main constraint mix: edge (with an active clamp and a fixed stretch),
// triangles, and four closeness controls (two stiff pins, two soft controls).
std::vector<Spec> mainSpecs() {
  std::vector<Spec> s;
  s.push_back({EDGE, {0, 1}, 2.0, 0.5, 0.8});
  s.push_back({EDGE, {0, 1}, 1.5, 1.0, 1.0});      // fixed stretch, conflicts
  s.push_back({EDGE, {1, 2}, 1.0, 0.9, 1.1});
  s.push_back({EDGE, {0, 3}, 1.5, 0.9, 1.1});
  s.push_back({TRIANGLE, {0, 1, 2}, 1.0, 0.7, 1.3});
  s.push_back({TRIANGLE, {0, 1, 3}, 1.0, 0.7, 1.3});
  Matrix3X p = referenceMesh();
  s.push_back({CLOSENESS, {0}, 50.0, 1.0, 1.0, p.col(0)});
  s.push_back({CLOSENESS, {1}, 1.0, 1.0, 1.0, p.col(1)});
  s.push_back({CLOSENESS, {2}, 50.0, 1.0, 1.0, p.col(2)});
  // Soft control with a target far enough from the rest pose to strain the
  // incident edges past their bounds, so its residual is nonzero and its
  // weight measurably moves the equilibrium.
  s.push_back({CLOSENESS, {3}, 2.0, 1.0, 1.0,
               p.col(3) + Vector3(0.80, -0.50, 0.40)});
  return s;
}

void addSpec(Solver &solver, const Spec &sp, const Matrix3X &p) {
  if (sp.kind == EDGE)
    solver.addConstraint(std::make_shared<EdgeStrainConstraint>(
        sp.idx, sp.w, p, sp.rmin, sp.rmax));
  else if (sp.kind == TRIANGLE)
    solver.addConstraint(std::make_shared<TriangleStrainConstraint>(
        sp.idx, sp.w, p, sp.rmin, sp.rmax));
  else {
    auto c = std::make_shared<ClosenessConstraint>(sp.idx, sp.w, p);
    c->setPosition(sp.target);
    solver.addConstraint(c);
  }
}

std::shared_ptr<Solver> buildSolver(const std::vector<Spec> &specs,
                                    const Matrix3X &p, const Matrix3X &start) {
  auto solver = std::make_shared<Solver>();
  solver->setPoints(p);
  for (const Spec &sp : specs) addSpec(*solver, sp, p);
  solver->initialize(false);
  solver->setPoints(start);
  return solver;
}

Matrix3X perturbedStart() {
  Matrix3X p0 = referenceMesh();
  p0.col(1) += Vector3(0.15, -0.10, 0.05);
  p0.col(2) += Vector3(-0.10, 0.20, 0.10);
  p0.col(3) += Vector3(0.05, 0.10, -0.15);
  return p0;
}

double clampV(double v, double lo, double hi) {
  return std::min(std::max(v, lo), hi);
}

// Build a constraint temporarily to recover its intrinsic scaled weight.
double scaledWeightOf(const Spec &sp, const Matrix3X &p) {
  Solver tmp;
  tmp.setPoints(p);
  addSpec(tmp, sp, p);
  return tmp.getConstraint(0)->getScaledWeight();
}

// Independent weighted projection for one constraint, written from scratch.
Matrix3X projectBlock(const Spec &sp, const Matrix3X &p, const Matrix3X &X) {
  const double wbar = scaledWeightOf(sp, p);
  if (sp.kind == EDGE) {
    Matrix3X out(3, 1);
    Vector3 e = X.col(sp.idx[1]) - X.col(sp.idx[0]);
    double l = e.norm();
    if (l <= 1e-12) { out.setZero(); return out; }
    double restInverse = 1.0 / (p.col(sp.idx[1]) - p.col(sp.idx[0])).norm();
    double q = clampV(l * restInverse, sp.rmin, sp.rmax);
    out.col(0) = wbar * q * e / l;
    return out;
  }
  if (sp.kind == TRIANGLE) {
    Matrix32 edges;
    edges.col(0) = X.col(sp.idx[1]) - X.col(sp.idx[0]);
    edges.col(1) = X.col(sp.idx[2]) - X.col(sp.idx[0]);
    Matrix3X out(3, 2);
    if ((edges.transpose() * edges).determinant() <= 1e-24) { out.setZero(); return out; }
    Matrix32 Pf;
    Pf.col(0) = edges.col(0).normalized();
    Pf.col(1) = (edges.col(1) - edges.col(1).dot(Pf.col(0)) * Pf.col(0)).normalized();
    Matrix22 rest = (Pf.transpose() * edges).inverse(); // rest geometry == p
    // Recompute rest from the reference mesh exactly as the constraint does.
    Matrix32 e0;
    e0.col(0) = p.col(sp.idx[1]) - p.col(sp.idx[0]);
    e0.col(1) = p.col(sp.idx[2]) - p.col(sp.idx[0]);
    Matrix32 P0;
    P0.col(0) = e0.col(0).normalized();
    P0.col(1) = (e0.col(1) - e0.col(1).dot(P0.col(0)) * P0.col(0)).normalized();
    rest = (P0.transpose() * e0).inverse();
    Matrix22 F = Pf.transpose() * edges * rest;
    Eigen::JacobiSVD<Matrix22> svd(F, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Vector2 s = svd.singularValues();
    s(0) = clampV(s(0), sp.rmin, sp.rmax);
    s(1) = clampV(s(1), sp.rmin, sp.rmax);
    Matrix22 Fc = svd.matrixU() * s.asDiagonal() * svd.matrixV().transpose();
    return wbar * Pf * Fc;
  }
  Matrix3X out(3, 1);
  out.col(0) = wbar * sp.target;
  return out;
}

// Independent A_c X^T block (the "actual" side of the residual).
Matrix3X actualBlock(const Spec &sp, const Matrix3X &p, const Matrix3X &X) {
  const double wbar = scaledWeightOf(sp, p);
  if (sp.kind == EDGE) {
    Matrix3X out(3, 1);
    double restInverse = 1.0 / (p.col(sp.idx[1]) - p.col(sp.idx[0])).norm();
    out.col(0) = wbar * restInverse * (X.col(sp.idx[1]) - X.col(sp.idx[0]));
    return out;
  }
  if (sp.kind == TRIANGLE) {
    Matrix32 e0;
    e0.col(0) = p.col(sp.idx[1]) - p.col(sp.idx[0]);
    e0.col(1) = p.col(sp.idx[2]) - p.col(sp.idx[0]);
    Matrix32 P0;
    P0.col(0) = e0.col(0).normalized();
    P0.col(1) = (e0.col(1) - e0.col(1).dot(P0.col(0)) * P0.col(0)).normalized();
    Matrix22 rest = (P0.transpose() * e0).inverse();
    Matrix32 edges;
    edges.col(0) = X.col(sp.idx[1]) - X.col(sp.idx[0]);
    edges.col(1) = X.col(sp.idx[2]) - X.col(sp.idx[0]);
    Matrix32 Pf;
    Pf.col(0) = edges.col(0).normalized();
    Pf.col(1) = (edges.col(1) - edges.col(1).dot(Pf.col(0)) * Pf.col(0)).normalized();
    Matrix3X out(3, 2);
    out.block<3, 2>(0, 0) = wbar * Pf * (Pf.transpose() * edges * rest);
    return out;
  }
  Matrix3X out(3, 1);
  out.col(0) = wbar * X.col(sp.idx[0]);
  return out;
}

double independentEnergy(const std::vector<Spec> &specs, const Matrix3X &p,
                         const Matrix3X &X) {
  double E = 0.0;
  for (const Spec &sp : specs) {
    Matrix3X r = actualBlock(sp, p, X) - projectBlock(sp, p, X);
    E += 0.5 * r.squaredNorm();
  }
  return E;
}

Matrix3X independentGradient(const std::vector<Spec> &specs, const Matrix3X &p,
                             const Matrix3X &X) {
  // Central finite differences of the independently evaluated energy.
  const double h = 1e-7;
  Matrix3X G(3, X.cols());
  for (int j = 0; j < X.cols(); ++j)
    for (int a = 0; a < 3; ++a) {
      Matrix3X xp = X, xm = X;
      xp(a, j) += h; xm(a, j) -= h;
      G(a, j) = (independentEnergy(specs, p, xp) -
                 independentEnergy(specs, p, xm)) / (2 * h);
    }
  return G;
}

// Thin test wrapper around the equilibrium entry: returns the owning snapshot
// and optionally the convergence report.
std::shared_ptr<EquilibriumSnapshot>
runEquilibrium(InverseDesign &design, unsigned int iterations, Scalar tolerance,
               Scalar branchTolerance, EquilibriumReport *report = nullptr) {
  EquilibriumResult result = design.equilibrium(iterations, tolerance, branchTolerance);
  if (report) *report = result.report;
  return result.snapshot;
}

bool close(double a, double b) {
  return std::abs(a - b) <= 2e-5 + 2e-5 * std::abs(b);
}

// Fresh converged solution with one target or weight perturbed.
Matrix3X reSolve(const std::vector<Spec> &specs, const Matrix3X &p,
                 const Matrix3X &start, int pc, int axis, double dw, double dt,
                 InverseStatus *statusOut = nullptr) {
  std::vector<Spec> perturbed = specs;
  if (pc >= 0) {
    if (dw != 0.0) perturbed[pc].w += dw;
    if (dt != 0.0) perturbed[pc].target(axis) += dt;
  }
  auto solver = buildSolver(perturbed, p, start);
  InverseDesign design(*solver);
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch);
  if (statusOut) *statusOut = snap->status();
  if (snap->status() != InverseStatus::Success)
    return Matrix3X();
  return snap->points();
}

} // namespace

///////////////////////////////////////////////////////////////////////////////
// equilibrium group
///////////////////////////////////////////////////////////////////////////////
static int test_energy_and_gradient() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  EquilibriumReport report;
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch, &report);
  TASK_CHECK(snap->status() == InverseStatus::Success);
  TASK_CHECK(snap->iterations() > 0);
  TASK_CHECK(snap->gradientResidual() <= kTol);
  // The returned report is the real energy/residual/iteration bookkeeping and
  // stays consistent with the snapshot accessors.
  TASK_CHECK(report.status == InverseStatus::Success);
  TASK_CHECK(report.iterations == snap->iterations());
  TASK_CHECK(report.energy == snap->energy());
  TASK_CHECK(report.gradientResidual == snap->gradientResidual());
  TASK_CHECK(report.energy > 0.0);

  double Eref = independentEnergy(specs, p, snap->points());
  TASK_CHECK(std::abs(Eref - snap->energy()) <= 1e-10 * (1.0 + std::abs(Eref)));

  Matrix3X Gref = independentGradient(specs, p, snap->points());
  TASK_CHECK(std::abs(Gref.cwiseAbs().maxCoeff() - snap->gradientResidual())
             <= 1e-7 * (1.0 + Gref.cwiseAbs().maxCoeff()));

  // An active strain clamp must be present in the converged geometry.
  bool activeClamp = false;
  for (std::size_t c = 0; c < specs.size(); ++c) {
    if (specs[c].kind != EDGE) continue;
    Vector3 e = snap->points().col(specs[c].idx[1]) - snap->points().col(specs[c].idx[0]);
    double restInverse = 1.0 / (p.col(specs[c].idx[1]) - p.col(specs[c].idx[0])).norm();
    double q = e.norm() * restInverse;
    if (q > specs[c].rmax + 1e-6 || q < specs[c].rmin - 1e-6) activeClamp = true;
  }
  TASK_CHECK(activeClamp);
  return 0;
}

static int test_budget_failure_publishes_nothing() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  Matrix3X before = solver->getPoints();
  InverseDesign design(*solver);
  // A two-iteration budget cannot reach the tight gradient tolerance from the
  // perturbed starting geometry.
  EquilibriumReport report;
  auto snap = runEquilibrium(design, 2, kTol, kBranch, &report);
  TASK_CHECK(snap->status() == InverseStatus::BudgetExceeded);
  TASK_CHECK(snap->gradientResidual() > kTol);
  TASK_CHECK(snap->iterations() == 2);
  TASK_CHECK(report.status == InverseStatus::BudgetExceeded);
  TASK_CHECK(report.iterations == 2);
  TASK_CHECK(report.gradientResidual > kTol);
  // Trial positions must not be published.
  TASK_CHECK((solver->getPoints() - before).cwiseAbs().maxCoeff() == 0.0);
  // A failed snapshot refuses pullback.
  PullbackResult pb = snap->pullback(Matrix3X::Zero(3, 4));
  TASK_CHECK(pb.report.status != InverseStatus::Success);
  return 0;
}

static int test_nonsmooth_and_clamp_switch() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  // A generous branch tolerance catches a projection within its kink band.
  auto near = runEquilibrium(design, kBudget, kTol, 0.5);
  TASK_CHECK(near->status() == InverseStatus::Nonsmooth);
  // The same geometry with a tight branch tolerance is a valid smooth branch.
  auto smooth = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(smooth->status() == InverseStatus::Success);
  return 0;
}

static int test_degenerate_geometry_rejected() {
  // Start directly from a collapsed configuration: vertices 0 and 1 coincide,
  // so the edge has zero length and the triangle has rank-deficient edges.
  // The stiff, coincident closeness anchors keep the fixed point degenerate;
  // no regularization is allowed to hide it.
  Matrix3X p = referenceMesh();
  std::vector<Spec> specs;
  specs.push_back({EDGE, {0, 1}, 1.0, 0.9, 1.1});
  specs.push_back({TRIANGLE, {0, 1, 2}, 1.0, 0.5, 1.5});
  Vector3 collapse(0.2, 0.2, 0.2);
  specs.push_back({CLOSENESS, {0}, 1e6, 1.0, 1.0, collapse});
  specs.push_back({CLOSENESS, {1}, 1e6, 1.0, 1.0, collapse});
  specs.push_back({CLOSENESS, {2}, 1e6, 1.0, 1.0, p.col(2)});
  specs.push_back({CLOSENESS, {3}, 1e6, 1.0, 1.0, p.col(3)});
  Matrix3X start = p;
  start.col(0) = collapse;
  start.col(1) = collapse;
  auto solver = buildSolver(specs, p, start);
  InverseDesign design(*solver);
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap->status() == InverseStatus::DegenerateGeometry);
  TASK_CHECK(snap->pullback(Matrix3X::Zero(3, 4)).report.status !=
             InverseStatus::Success);
  // The degenerate trial geometry must not be published.
  TASK_CHECK((solver->getPoints() - start).cwiseAbs().maxCoeff() == 0.0);
  return 0;
}

static int test_singular_tangent_rejected() {
  // No closeness control on vertex 1: clamped edge directions leave null
  // modes, so the equilibrium tangent is singular.
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  std::vector<Spec> specs;
  specs.push_back({EDGE, {0, 1}, 2.0, 0.5, 0.8});
  specs.push_back({EDGE, {0, 1}, 1.5, 1.0, 1.0});
  specs.push_back({EDGE, {1, 2}, 1.0, 0.9, 1.1});
  specs.push_back({EDGE, {0, 3}, 1.5, 0.9, 1.1});
  specs.push_back({TRIANGLE, {0, 1, 2}, 1.0, 0.7, 1.3});
  specs.push_back({TRIANGLE, {0, 1, 3}, 1.0, 0.7, 1.3});
  specs.push_back({CLOSENESS, {0}, 50.0, 1.0, 1.0, p.col(0)});
  specs.push_back({CLOSENESS, {2}, 50.0, 1.0, 1.0, p.col(2)});
  specs.push_back({CLOSENESS, {3}, 2.0, 1.0, 1.0, p.col(3)});
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap->status() == InverseStatus::SingularTangent);
  TASK_CHECK(snap->gradientResidual() <= kTol); // equilibrium itself is fine
  return 0;
}

static int test_invalid_inputs_rejected() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  // Unsupported constraint type is refused at construction.
  bool threw = false;
  try {
    Solver bad;
    bad.setPoints(p);
    bad.addConstraint(std::make_shared<UniformLaplacianConstraint>(
        std::vector<int>{0, 1}, 1.0, p, false));
    bad.initialize(false);
    InverseDesign design(bad);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  TASK_CHECK(threw);

  InverseDesign design(*solver);
  TASK_CHECK(!design.setConstraintWeight(0, 0.0));
  TASK_CHECK(!design.setConstraintWeight(0, -1.0));
  TASK_CHECK(!design.setConstraintWeight(999, 1.0));
  auto badTol = runEquilibrium(design, kBudget, 0.0, kBranch);
  TASK_CHECK(badTol->status() == InverseStatus::InvalidInput);
  auto badBranch = runEquilibrium(design, kBudget, kTol, -1.0);
  TASK_CHECK(badBranch->status() == InverseStatus::InvalidInput);
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// adjoint group
///////////////////////////////////////////////////////////////////////////////
static int test_pullback_vs_central_differences() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap->status() == InverseStatus::Success);

  Matrix3X G(3, 4);
  // All three axes and all vertices carry nonzero entries so cross-axis
  // responses are exercised.
  G << 0.7, -1.1, 0.3, 0.5,
      -0.4, 0.9, -0.6, 1.3,
       1.2, 0.2, -0.8, -0.3;
  PullbackResult pb = snap->pullback(G);
  TASK_CHECK(pb.report.status == InverseStatus::Success);
  TASK_CHECK(pb.report.adjointResidual < 1e-9);

  auto Lof = [&](const Matrix3X &X) { return (G.array() * X.array()).sum(); };
  const double h1 = 1e-4, h2 = 1e-5;

  // Target VJPs for every closeness target (multi-objective).
  for (std::size_t c = 0; c < specs.size(); ++c) {
    if (specs[c].kind != CLOSENESS) continue;
    for (int a = 0; a < 3; ++a) {
      Matrix3X xp = reSolve(specs, p, p0, (int)c, a, 0.0, +h1);
      Matrix3X xm = reSolve(specs, p, p0, (int)c, a, 0.0, -h1);
      double fd1 = (Lof(xp) - Lof(xm)) / (2 * h1);
      xp = reSolve(specs, p, p0, (int)c, a, 0.0, +h2);
      xm = reSolve(specs, p, p0, (int)c, a, 0.0, -h2);
      double fd2 = (Lof(xp) - Lof(xm)) / (2 * h2);
      TASK_CHECK(close(pb.dTargets[c](a), fd2));
      TASK_CHECK(std::abs(fd1 - fd2) <= 1e-4 * (1.0 + std::abs(fd2)));
    }
  }
  // Weight VJPs for every constraint in original order.
  for (std::size_t c = 0; c < specs.size(); ++c) {
    Matrix3X xp = reSolve(specs, p, p0, (int)c, -1, +h1, 0.0);
    Matrix3X xm = reSolve(specs, p, p0, (int)c, -1, -h1, 0.0);
    double fd1 = (Lof(xp) - Lof(xm)) / (2 * h1);
    xp = reSolve(specs, p, p0, (int)c, -1, +h2, 0.0);
    xm = reSolve(specs, p, p0, (int)c, -1, -h2, 0.0);
    double fd2 = (Lof(xp) - Lof(xm)) / (2 * h2);
    TASK_CHECK(close(pb.dWeights(c), fd2));
    TASK_CHECK(std::abs(fd1 - fd2) <= 1e-4 * (1.0 + std::abs(fd2)));
  }
  return 0;
}

static int test_frozen_projection_tangent_is_wrong() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap->status() == InverseStatus::Success);

  // Build the frozen (A^T A only) tangent independently: block diagonal over
  // the three axes, ignoring the geometry dependence of the projections.
  std::vector<Triplet> trips;
  int idO = 0;
  for (std::size_t c = 0; c < specs.size(); ++c)
    solver->getConstraint((int)c)->addConstraint(trips, idO);
  SparseMatrix A(idO, 4);
  A.setFromTriplets(trips.begin(), trips.end());
  SparseMatrix AtA = A.transpose() * A;
  const int n = 4;
  std::vector<Triplet> ft;
  for (int k = 0; k < AtA.outerSize(); ++k)
    for (SparseMatrix::InnerIterator it(AtA, k); it; ++it)
      for (int a = 0; a < 3; ++a)
        ft.push_back(Triplet(3 * it.row() + a, 3 * it.col() + a, it.value()));
  SparseMatrix Kf(3 * n, 3 * n);
  Kf.setFromTriplets(ft.begin(), ft.end());
  Eigen::SimplicialLDLT<SparseMatrix> frozen;
  frozen.compute(Kf);
  TASK_CHECK(frozen.info() == Eigen::Success);

  Matrix3X G(3, n);
  G << 0.7, -1.1, 0.3, 0.5,
      -0.4, 0.9, -0.6, 1.3,
       1.2, 0.2, -0.8, -0.3;
  VectorX g(3 * n);
  for (int j = 0; j < n; ++j) for (int a = 0; a < 3; ++a) g[3 * j + a] = G(a, j);
  VectorX lamFrozen = frozen.solve(-g);

  // Frozen target VJP for the soft control on vertex 3 (constraint 9).
  double wbar = solver->getConstraint(9)->getScaledWeight();
  double frozenVjp = -wbar * lamFrozen[3 * 3 + 0]; // target x coordinate
  Matrix3X xp = reSolve(specs, p, p0, 9, 0, 0.0, +1e-5);
  Matrix3X xm = reSolve(specs, p, p0, 9, 0, 0.0, -1e-5);
  double fd = ((G.array() * xp.array()).sum() - (G.array() * xm.array()).sum()) / 2e-5;
  // The frozen tangent must miss the true derivative by a clear margin, while
  // the production pullback (checked elsewhere) matches.
  TASK_CHECK(std::abs(frozenVjp - fd) > 1e-3);

  // The production tangent really is three-axis coupled: off-axis entries
  // (different spatial axes) must be present and nonzero.
  PullbackResult pb = snap->pullback(G);
  TASK_CHECK(pb.report.status == InverseStatus::Success);
  bool crossAxis = false;
  for (int i = 0; i < n && !crossAxis; ++i)
    for (int j = 0; j < n && !crossAxis; ++j)
      for (int a = 0; a < 3 && !crossAxis; ++a)
        for (int b = 0; b < 3; ++b)
          if (a != b) {
            Matrix3X ei = Matrix3X::Zero(3, n), col;
            ei(b, j) = 1.0;
            col = snap->tangentMultiply(ei);
            if (std::abs(col(a, i)) > 1e-8) { crossAxis = true; break; }
          }
  TASK_CHECK(crossAxis);
  return 0;
}

static int test_repeated_singular_values() {
  // Equilibrium geometry is a uniform similarity of the rest triangle, so
  // both singular values coincide and stay on the same interior branch.
  Matrix3X p = referenceMesh();
  std::vector<Spec> specs;
  specs.push_back({TRIANGLE, {0, 1, 2}, 1.0, 0.5, 1.5});
  specs.push_back({TRIANGLE, {0, 1, 3}, 1.0, 0.5, 1.5});
  const double lambda = 1.05;
  for (int j = 0; j < 4; ++j)
    specs.push_back({CLOSENESS, {j}, 10.0, 1.0, 1.0, lambda * p.col(j)});
  Matrix3X start = p; // already close to the similarity solution
  auto solver = buildSolver(specs, p, start);
  InverseDesign design(*solver);
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap->status() == InverseStatus::Success);

  // Confirm the repeated singular values explicitly.
  Matrix32 edges;
  edges.col(0) = snap->points().col(1) - snap->points().col(0);
  edges.col(1) = snap->points().col(2) - snap->points().col(0);
  Matrix32 Pf;
  Pf.col(0) = edges.col(0).normalized();
  Pf.col(1) = (edges.col(1) - edges.col(1).dot(Pf.col(0)) * Pf.col(0)).normalized();
  Matrix32 e0;
  e0.col(0) = p.col(1) - p.col(0);
  e0.col(1) = p.col(2) - p.col(0);
  Matrix32 P0;
  P0.col(0) = e0.col(0).normalized();
  P0.col(1) = (e0.col(1) - e0.col(1).dot(P0.col(0)) * P0.col(0)).normalized();
  Matrix22 F = Pf.transpose() * edges * (P0.transpose() * e0).inverse();
  Eigen::JacobiSVD<Matrix22> svd(F, Eigen::ComputeFullU | Eigen::ComputeFullV);
  TASK_CHECK(std::abs(svd.singularValues()(0) - svd.singularValues()(1)) < 1e-8);

  Matrix3X G(3, 4);
  G.setRandom();
  PullbackResult pb = snap->pullback(G);
  TASK_CHECK(pb.report.status == InverseStatus::Success);
  TASK_CHECK(pb.report.adjointResidual < 1e-9);
  Matrix3X xp = reSolve(specs, p, start, 4, 0, 0.0, +1e-5);
  Matrix3X xm = reSolve(specs, p, start, 4, 0, 0.0, -1e-5);
  double fd = ((G.array() * xp.array()).sum() - (G.array() * xm.array()).sum()) / 2e-5;
  TASK_CHECK(close(pb.dTargets[4](0), fd));
  return 0;
}

static int test_pullback_reuse_and_independence() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap->status() == InverseStatus::Success);

  Matrix3X G1(3, 4), G2(3, 4);
  G1.setRandom(); G2.setRandom();
  PullbackResult a1 = snap->pullback(G1);
  PullbackResult b = snap->pullback(G2);
  PullbackResult a2 = snap->pullback(G1); // linearization reused, no accumulation
  TASK_CHECK(a1.report.status == InverseStatus::Success);
  TASK_CHECK(b.report.status == InverseStatus::Success);
  TASK_CHECK(a1.report.adjointResidual < 1e-9);
  TASK_CHECK(b.report.adjointResidual < 1e-9);
  for (std::size_t c = 0; c < specs.size(); ++c) {
    TASK_CHECK((a1.dTargets[c] - a2.dTargets[c]).cwiseAbs().maxCoeff() == 0.0);
    TASK_CHECK(std::abs(a1.dWeights(c) - a2.dWeights(c)) == 0.0);
  }
  // The second pullback is linear in G: zero G must give zero VJPs, proving
  // the stored linearization does not accumulate results across calls.
  PullbackResult zero = snap->pullback(Matrix3X::Zero(3, 4));
  TASK_CHECK(zero.report.status == InverseStatus::Success);
  for (std::size_t c = 0; c < specs.size(); ++c) {
    TASK_CHECK(zero.dTargets[c].cwiseAbs().maxCoeff() == 0.0);
    TASK_CHECK(zero.dWeights(c) == 0.0);
  }
  // Linearity: pullback(G1+G2) == pullback(G1) + pullback(G2).
  PullbackResult sum = snap->pullback(G1 + G2);
  TASK_CHECK(sum.report.status == InverseStatus::Success);
  for (std::size_t c = 0; c < specs.size(); ++c) {
    TASK_CHECK((sum.dTargets[c] - (a1.dTargets[c] + b.dTargets[c]))
                   .cwiseAbs().maxCoeff() < 1e-12);
    TASK_CHECK(std::abs(sum.dWeights(c) - (a1.dWeights(c) + b.dWeights(c))) < 1e-12);
  }
  return 0;
}

static int test_multiple_closeness_on_one_point() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  // A second closeness control acts on the already controlled vertex 3.
  specs.push_back({CLOSENESS, {3}, 0.7, 1.0, 1.0,
                   p.col(3) + Vector3(0.1, -0.1, 0.2)});
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap->status() == InverseStatus::Success);
  Matrix3X G(3, 4);
  G.setRandom();
  PullbackResult pb = snap->pullback(G);
  TASK_CHECK(pb.report.status == InverseStatus::Success);
  // Both controls on vertex 3 get independent, FD-verified derivatives.
  for (int id : {9, 10}) {
    Matrix3X xp = reSolve(specs, p, p0, id, 1, 0.0, +1e-5);
    Matrix3X xm = reSolve(specs, p, p0, id, 1, 0.0, -1e-5);
    double fd = ((G.array() * xp.array()).sum() - (G.array() * xm.array()).sum()) / 2e-5;
    TASK_CHECK(close(pb.dTargets[id](1), fd));
  }
  TASK_CHECK(pb.dTargets.size() == specs.size());
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// lifecycle group
///////////////////////////////////////////////////////////////////////////////
static int test_snapshot_outlives_solver() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  std::shared_ptr<EquilibriumSnapshot> snap;
  {
    auto solver = buildSolver(specs, p, p0);
    InverseDesign design(*solver);
    snap = runEquilibrium(design, kBudget, kTol, kBranch);
    TASK_CHECK(snap->status() == InverseStatus::Success);
  } // solver and all constraints destroyed here
  Matrix3X G(3, 4);
  G.setRandom();
  PullbackResult pb = snap->pullback(G);
  TASK_CHECK(pb.report.status == InverseStatus::Success);
  TASK_CHECK(snap->nPoints() == 4);
  TASK_CHECK(snap->nConstraints() == (int)specs.size());
  TASK_CHECK(snap->energy() > 0.0);
  Matrix3X xp = reSolve(specs, p, p0, 9, 0, 0.0, +1e-5);
  Matrix3X xm = reSolve(specs, p, p0, 9, 0, 0.0, -1e-5);
  double fd = ((G.array() * xp.array()).sum() - (G.array() * xm.array()).sum()) / 2e-5;
  TASK_CHECK(close(pb.dTargets[9](0), fd));
  return 0;
}

static int test_interleaved_snapshots_stay_independent() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specsA = mainSpecs();
  auto specsB = mainSpecs();
  specsB[9].target += Vector3(0.2, 0.0, -0.2); // different soft target
  auto solverA = buildSolver(specsA, p, p0);
  auto solverB = buildSolver(specsB, p, p0);
  InverseDesign dA(*solverA), dB(*solverB);
  auto snapA = runEquilibrium(dA, kBudget, kTol, kBranch);
  auto snapB = runEquilibrium(dB, kBudget, kTol, kBranch);
  TASK_CHECK(snapA->status() == InverseStatus::Success);
  TASK_CHECK(snapB->status() == InverseStatus::Success);
  TASK_CHECK((snapA->points() - snapB->points()).cwiseAbs().maxCoeff() > 1e-6);

  Matrix3X G(3, 4);
  G.setRandom();
  PullbackResult a1 = snapA->pullback(G);
  PullbackResult b1 = snapB->pullback(G);
  PullbackResult a2 = snapA->pullback(G);
  PullbackResult b2 = snapB->pullback(G);
  TASK_CHECK(a1.report.status == InverseStatus::Success);
  TASK_CHECK(b1.report.status == InverseStatus::Success);
  for (std::size_t c = 0; c < specsA.size(); ++c) {
    TASK_CHECK((a1.dTargets[c] - a2.dTargets[c]).cwiseAbs().maxCoeff() == 0.0);
    TASK_CHECK((b1.dTargets[c] - b2.dTargets[c]).cwiseAbs().maxCoeff() == 0.0);
    TASK_CHECK(std::abs(a1.dWeights(c) - a2.dWeights(c)) == 0.0);
    TASK_CHECK(std::abs(b1.dWeights(c) - b2.dWeights(c)) == 0.0);
  }
  return 0;
}

static int test_failed_run_keeps_previous_snapshot() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  auto good = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(good->status() == InverseStatus::Success);
  Matrix3X G(3, 4);
  G.setRandom();
  PullbackResult before = good->pullback(G);
  TASK_CHECK(before.report.status == InverseStatus::Success);

  Matrix3X pointsBefore = solver->getPoints();
  // Move the live solver away from the fixed point so a one-iteration budget
  // genuinely cannot reach the gradient tolerance.
  Matrix3X restart = pointsBefore;
  restart.col(1) += Vector3(0.12, -0.08, 0.06);
  restart.col(3) += Vector3(-0.07, 0.09, 0.04);
  solver->setPoints(restart);
  auto failed = runEquilibrium(design, 1, kTol, kBranch); // budget exhausted
  TASK_CHECK(failed->status() == InverseStatus::BudgetExceeded);
  TASK_CHECK(failed->iterations() == 1);
  TASK_CHECK(failed->gradientResidual() > kTol);
  TASK_CHECK((solver->getPoints() - restart).cwiseAbs().maxCoeff() == 0.0);

  // The earlier successful snapshot is untouched and still answers pullback.
  PullbackResult after = good->pullback(G);
  TASK_CHECK(after.report.status == InverseStatus::Success);
  for (std::size_t c = 0; c < specs.size(); ++c)
    TASK_CHECK((before.dTargets[c] - after.dTargets[c]).cwiseAbs().maxCoeff() == 0.0);
  return 0;
}

static int test_weight_update_rebuilds_matrices() {
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  auto snap0 = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap0->status() == InverseStatus::Success);

  TASK_CHECK(design.setConstraintWeight(9, 7.0));
  TASK_CHECK(std::abs(solver->getConstraint(9)->getUserWeight() - 7.0) == 0.0);
  auto snap1 = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap1->status() == InverseStatus::Success);
  TASK_CHECK(snap1->userWeight(9) == 7.0);
  // The stronger control must visibly move vertex 3 toward its target.
  double d0 = (snap0->points().col(3) - specs[9].target).norm();
  double d1 = (snap1->points().col(3) - specs[9].target).norm();
  TASK_CHECK(d1 < d0);

  // Pullback at the new weight matches fresh finite differences.
  std::vector<Spec> changed = specs;
  changed[9].w = 7.0;
  Matrix3X G(3, 4);
  G.setRandom();
  PullbackResult pb = snap1->pullback(G);
  Matrix3X xp = reSolve(changed, p, p0, 9, -1, +1e-5, 0.0);
  Matrix3X xm = reSolve(changed, p, p0, 9, -1, -1e-5, 0.0);
  double fd = ((G.array() * xp.array()).sum() - (G.array() * xm.array()).sum()) / 2e-5;
  TASK_CHECK(close(pb.dWeights(9), fd));

  // Old snapshot keeps its own material parameters.
  TASK_CHECK(snap0->userWeight(9) == 2.0);
  return 0;
}

static int test_header_usage_example_gradient_step() {
  // Mirror of the compilable example in InverseDesign.h.
  Matrix3X p = referenceMesh(), p0 = perturbedStart();
  auto specs = mainSpecs();
  auto solver = buildSolver(specs, p, p0);
  InverseDesign design(*solver);
  auto snap = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap->status() == InverseStatus::Success);

  const int idSoft = 9;
  Vector3 desired = specs[idSoft].target + Vector3(0.3, -0.2, 0.15);
  Matrix3X G = Matrix3X::Zero(3, 4);
  G.col(3) = snap->points().col(3) - desired;
  PullbackResult pb = snap->pullback(G);
  TASK_CHECK(pb.report.status == InverseStatus::Success);

  const Scalar step = 0.25;
  auto soft = std::dynamic_pointer_cast<ClosenessConstraint>(
      solver->getConstraint(idSoft));
  soft->setPosition(soft->getPosition() - step * pb.dTargets[idSoft]);
  auto snap2 = runEquilibrium(design, kBudget, kTol, kBranch);
  TASK_CHECK(snap2->status() == InverseStatus::Success);
  Scalar before = (snap->points().col(3) - desired).norm();
  Scalar after = (snap2->points().col(3) - desired).norm();
  TASK_CHECK(after < before);
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
extern const struct task_case task_cases[] = {
  {"equilibrium", "energy_and_gradient", test_energy_and_gradient},
  {"equilibrium", "budget_failure_publishes_nothing", test_budget_failure_publishes_nothing},
  {"equilibrium", "nonsmooth_and_clamp_switch", test_nonsmooth_and_clamp_switch},
  {"equilibrium", "degenerate_geometry_rejected", test_degenerate_geometry_rejected},
  {"equilibrium", "singular_tangent_rejected", test_singular_tangent_rejected},
  {"equilibrium", "invalid_inputs_rejected", test_invalid_inputs_rejected},
  {"adjoint", "pullback_vs_central_differences", test_pullback_vs_central_differences},
  {"adjoint", "frozen_projection_tangent_is_wrong", test_frozen_projection_tangent_is_wrong},
  {"adjoint", "repeated_singular_values", test_repeated_singular_values},
  {"adjoint", "pullback_reuse_and_independence", test_pullback_reuse_and_independence},
  {"adjoint", "multiple_closeness_on_one_point", test_multiple_closeness_on_one_point},
  {"lifecycle", "snapshot_outlives_solver", test_snapshot_outlives_solver},
  {"lifecycle", "interleaved_snapshots_stay_independent", test_interleaved_snapshots_stay_independent},
  {"lifecycle", "failed_run_keeps_previous_snapshot", test_failed_run_keeps_previous_snapshot},
  {"lifecycle", "weight_update_rebuilds_matrices", test_weight_update_rebuilds_matrices},
  {"lifecycle", "header_usage_example_gradient_step", test_header_usage_example_gradient_step},
};
extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
