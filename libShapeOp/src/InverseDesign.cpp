///////////////////////////////////////////////////////////////////////////////
// This file is part of ShapeOp, a lightweight C++ library
// for static and dynamic geometry processing.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
///////////////////////////////////////////////////////////////////////////////
#include "InverseDesign.h"
#include "Solver.h"
#include <Eigen/SparseCholesky>
#include <Eigen/SVD>
#include <stdexcept>
///////////////////////////////////////////////////////////////////////////////
namespace ShapeOp {
///////////////////////////////////////////////////////////////////////////////
namespace {
/// Geometry below this absolute size is treated as degenerate (zero edge /
/// rank-deficient triangle). No unstated regularization is ever added.
const Scalar kDegenerateEps = 1e-12;
/// Relative pivot threshold for declaring a sparse factorization singular.
const Scalar kPivotRelEps = 1e-12;

SHAPEOP_INLINE Scalar clampScalar(Scalar v, Scalar vMin, Scalar vMax) {
  Scalar result = v > vMin ? v : vMin;
  return result > vMax ? vMax : result;
}

enum ConstraintKind { KIND_EDGE, KIND_TRIANGLE, KIND_CLOSENESS };

/// Owned copy of everything defining one constraint, independent of the live
/// Constraint object in the Solver.
struct ConstraintData {
  int id = -1;
  ConstraintKind kind = KIND_EDGE;
  std::vector<int> idx;
  Scalar userWeight = 1.0;
  Scalar weightScale = 1.0;
  Scalar scaledWeight() const { return std::sqrt(userWeight) * weightScale; }
  // Edge
  Scalar restInverse = 0.0;
  Scalar rangeMin = 1.0;
  Scalar rangeMax = 1.0;
  // Triangle (rest_ is the inverse rest edge matrix used by the projection)
  Matrix22 rest = Matrix22::Identity();
  // Closeness
  Vector3 target = Vector3::Zero();
  // Layout in the stacked projection matrix
  int rowStart = 0;
  int nRows = 0;
};

/// Analytic projection evaluation at a geometry together with the analytic
/// derivative of the weighted projection with respect to the involved vertices.
struct ProjectionEval {
  Matrix3X block;   ///< weighted projection P_c(X) (3 by nRows)
  MatrixXX jacV;    ///< d vec(P_c) / d vec(vertices), vertices column major
  bool degenerate = false;
  bool nonsmooth = false;
};

SHAPEOP_INLINE VectorX flatten32(const Matrix32 &M) {
  VectorX v(6);
  v.segment<3>(0) = M.col(0);
  v.segment<3>(3) = M.col(1);
  return v;
}

/// Differential of the triangle projection Z = Pframe * Fcl at fixed geometry,
/// given a differential dE of the two edge vectors. Keeps the dependence of the
/// Gram-Schmidt frame on the geometry (the projection direction moves with X).
/// rest is the constraint's rest matrix with F = Pframe^T E rest (it is the
/// inverse rest-edge matrix), and dPhi is the differential of the SVD clamp.
SHAPEOP_INLINE Matrix32 triangleProjectionDifferential(
    const Matrix32 &E, const Matrix32 &Pframe, const Matrix22 &Fcl,
    const Matrix22 &rest, const Matrix44 &dPhi, const Matrix32 &dE) {
  Vector3 e1 = E.col(0), e2 = E.col(1);
  Scalar n1 = e1.norm();
  Vector3 p1 = e1 / n1;
  Vector3 w2 = e2 - (e2.dot(p1)) * p1;
  Scalar n2 = w2.norm();
  Vector3 p2 = w2 / n2;

  Vector3 de1 = dE.col(0), de2 = dE.col(1);
  Vector3 dp1 = (de1 - (de1.dot(p1)) * p1) / n1;
  Vector3 dw2 = de2 - (de2.dot(p1) + e2.dot(dp1)) * p1 - (e2.dot(p1)) * dp1;
  Vector3 dp2 = (dw2 - (dw2.dot(p2)) * p2) / n2;

  // dF = d(P^T E) rest, since F = P^T E rest: frame motion plus edge motion.
  Matrix22 dPtE;
  dPtE.row(0) = dp1.transpose() * E;
  dPtE.row(1) = dp2.transpose() * E;
  Matrix22 dF = (Pframe.transpose() * dE + dPtE) * rest;
  VectorX dFv(4);
  dFv << dF(0, 0), dF(1, 0), dF(0, 1), dF(1, 1);
  VectorX dFclv = dPhi * dFv;
  Matrix22 dFcl;
  dFcl(0, 0) = dFclv(0); dFcl(1, 0) = dFclv(1);
  dFcl(0, 1) = dFclv(2); dFcl(1, 1) = dFclv(3);

  // dZ = d(P Fcl) = dP_frame Fcl + P dFcl. The two columns of dP_frame already
  // carry the frame motion multiplied by the rows of Fcl, so it must not be
  // multiplied by Fcl a second time.
  Matrix32 dPframe;
  dPframe.col(0) = dp1 * Fcl(0, 0) + dp2 * Fcl(1, 0);
  dPframe.col(1) = dp1 * Fcl(0, 1) + dp2 * Fcl(1, 1);
  return dPframe + Pframe * dFcl;
}

SHAPEOP_INLINE ProjectionEval evaluateProjection(const ConstraintData &cd, const Matrix3X &X,
                                                 Scalar branchTolerance) {
  ProjectionEval ev;
  const Scalar wbar = cd.scaledWeight();
  if (cd.kind == KIND_EDGE) {
    int i0 = cd.idx[0], i1 = cd.idx[1];
    Vector3 edge = X.col(i1) - X.col(i0);
    Scalar l = edge.norm();
    ev.block.setZero(3, 1);
    ev.jacV = MatrixXX::Zero(3, 6);
    if (l <= kDegenerateEps) { ev.degenerate = true; return ev; }
    Vector3 u = edge / l;
    Scalar q = l * cd.restInverse;
    Scalar qc = clampScalar(q, cd.rangeMin, cd.rangeMax);
    ev.block.col(0) = wbar * qc * u;
    // Analytic derivative of p(e) = wbar qc(l) e/l with respect to the edge
    // vector e = x1 - x0: dp/de = (qc/l) I + (qc' restInv - qc/l) u u^T.
    bool interior = (q > cd.rangeMin) && (q < cd.rangeMax);
    Scalar a = qc / l;
    Scalar b = (interior ? cd.restInverse : 0.0) - qc / l;
    Matrix33 Je = wbar * (a * Matrix33::Identity() + b * (u * u.transpose()));
    ev.jacV.template block<3, 3>(0, 0) = -Je; // derivative w.r.t. x0
    ev.jacV.template block<3, 3>(0, 3) =  Je; // derivative w.r.t. x1
    if (cd.rangeMin < cd.rangeMax &&
        (std::abs(q - cd.rangeMin) < branchTolerance ||
         std::abs(q - cd.rangeMax) < branchTolerance))
      ev.nonsmooth = true;
    return ev;
  }
  if (cd.kind == KIND_TRIANGLE) {
    ev.block.setZero(3, 2);
    ev.jacV = MatrixXX::Zero(6, 9);
    Matrix32 E;
    E.col(0) = X.col(cd.idx[1]) - X.col(cd.idx[0]);
    E.col(1) = X.col(cd.idx[2]) - X.col(cd.idx[0]);
    Scalar area2 = std::sqrt(std::max<Scalar>(0.0, (E.transpose() * E).determinant()));
    if (area2 <= kDegenerateEps) { ev.degenerate = true; return ev; }
    Matrix32 Pframe;
    Pframe.col(0) = E.col(0).normalized();
    Pframe.col(1) = (E.col(1) - E.col(1).dot(Pframe.col(0)) * Pframe.col(0)).normalized();
    Matrix22 F = Pframe.transpose() * E * cd.rest;
    Eigen::JacobiSVD<Matrix22> svd(F, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Vector2 s = svd.singularValues();
    Vector2 sc;
    sc(0) = clampScalar(s(0), cd.rangeMin, cd.rangeMax);
    sc(1) = clampScalar(s(1), cd.rangeMin, cd.rangeMax);
    Matrix22 U = svd.matrixU(), V = svd.matrixV();
    Matrix22 Fcl = U * sc.asDiagonal() * V.transpose();
    ev.block = wbar * Pframe * Fcl;

    const bool fixedStretch = (cd.rangeMin == cd.rangeMax);
    if (!fixedStretch) {
      for (int k = 0; k < 2; ++k)
        if (std::abs(s(k) - cd.rangeMin) < branchTolerance ||
            std::abs(s(k) - cd.rangeMax) < branchTolerance)
          ev.nonsmooth = true;
    }

    // Analytic derivative of the SVD clamp map Phi: F -> U C V^T with
    // C = diag(clamp(s_i)). With Z = U^T dF V and distinct singular values,
    // dFcl = U W V^T where
    //   W_ii = k_i Z_ii, k_i = 1 on the interior branch, 0 when clamped,
    //   W_ij = [(c_j s_j - c_i s_i) Z_ij + (c_j s_i - c_i s_j) Z_ji]
    //          / (s_j^2 - s_i^2), i != j,
    // which keeps the polar rotation term (essential for fixed stretch and for
    // clamped branches). Repeated singular values never cause a rejection on
    // non-unique SVD vectors: on a common smooth branch the local map is
    // k_bar F (+ constant), and the equal-singularity limits are used directly.
    Vector2 kp;
    Eigen::Matrix<int, 2, 1> clampedFlag;
    for (int i = 0; i < 2; ++i) {
      bool clamped = fixedStretch || s(i) <= cd.rangeMin || s(i) >= cd.rangeMax;
      clampedFlag(i) = clamped ? 1 : 0;
      kp(i) = clamped ? 0.0 : 1.0;
    }
    Matrix44 dPhi = Matrix44::Zero();
    const Scalar kRepeatEps = 1e-10;
    bool repeated = std::abs(s(0) - s(1)) < kRepeatEps * (std::max)(s(0), s(1));
    if (repeated && clampedFlag(0) != clampedFlag(1)) {
      ev.nonsmooth = true; // crossing of two different clamp branches
    }
    if (!ev.nonsmooth) {
      if (repeated) {
        if (kp(0) > 0.0) {
          // Both interior: clamp is the identity on this branch.
          dPhi = Matrix44::Identity();
        } else {
          // Both clamped (including fixed stretch): Fcl = c R, only the skew
          // part of Z survives, W = c/(2 s) (Z - Z^T).
          Scalar cc = sc(0), ss = s(0);
          for (int unit = 0; unit < 4; ++unit) {
            Matrix22 dF = Matrix22::Zero();
            dF(unit % 2, unit / 2) = 1.0;
            Matrix22 Z = U.transpose() * dF * V;
            Matrix22 W = (cc / (2.0 * ss)) * (Z - Z.transpose());
            Matrix22 out = U * W * V.transpose();
            dPhi.col(unit) << out(0, 0), out(1, 0), out(0, 1), out(1, 1);
          }
        }
      } else {
        for (int unit = 0; unit < 4; ++unit) {
          Matrix22 dF = Matrix22::Zero();
          dF(unit % 2, unit / 2) = 1.0;
          Matrix22 Z = U.transpose() * dF * V;
          Matrix22 W = Matrix22::Zero();
          W(0, 0) = kp(0) * Z(0, 0);
          W(1, 1) = kp(1) * Z(1, 1);
          for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j) {
              if (i == j) continue;
              Scalar alpha = sc(j) * s(j) - sc(i) * s(i);
              Scalar gamma = sc(j) * s(i) - sc(i) * s(j);
              W(i, j) = (alpha * Z(i, j) + gamma * Z(j, i)) /
                        (s(j) * s(j) - s(i) * s(i));
            }
          Matrix22 out = U * W * V.transpose();
          dPhi.col(unit) << out(0, 0), out(1, 0), out(0, 1), out(1, 1);
        }
      }
    }
    // F = P^T E cd.rest, so the frame differential is chained with cd.rest.

    // Chain with the edge-vertex incidence: E col k = x_{k+1} - x0.
    for (int lj = 0; lj < 3; ++lj) for (int bj = 0; bj < 3; ++bj) {
      Matrix32 dE = Matrix32::Zero();
      Vector3 eb = Vector3::Unit(bj);
      if (lj == 0) { dE.col(0) -= eb; dE.col(1) -= eb; }
      else if (lj == 1) dE.col(0) += eb;
      else dE.col(1) += eb;
      Matrix32 dZ = triangleProjectionDifferential(E, Pframe, Fcl, cd.rest, dPhi, dE);
      ev.jacV.col(lj * 3 + bj) = wbar * flatten32(dZ);
    }
    return ev;
  }
  // Closeness: weighted constant target, derivative with respect to X is zero.
  ev.block.setZero(3, 1);
  ev.block.col(0) = wbar * cd.target;
  ev.jacV = MatrixXX::Zero(3, 3);
  return ev;
}
} // namespace
///////////////////////////////////////////////////////////////////////////////
struct EquilibriumSnapshot::Impl {
  int nPoints = 0;
  Matrix3X X;
  std::vector<ConstraintData> constraints;
  SparseMatrix A;                 // stacked weighted blocks (m by n)
  SparseMatrix AtA;               // A^T A (n by n)
  Matrix3X P;                     // weighted projections at X*
  SparseMatrix tangent;           // true 3-axis coupled tangent (3n by 3n)
  std::unique_ptr<Eigen::SimplicialLDLT<SparseMatrix> > factorization;
  Scalar energyVal = 0.0;
  Scalar gradResVal = 0.0;
  Scalar adjResVal = 0.0;
  unsigned int iterationVal = 0;
  InverseStatus statusVal = InverseStatus::InvalidInput;

  void buildA() {
    std::vector<Triplet> triplets;
    int row = 0;
    for (ConstraintData &cd : constraints) {
      cd.rowStart = row;
      const Scalar wbar = cd.scaledWeight();
      if (cd.kind == KIND_EDGE) {
        cd.nRows = 1;
        triplets.push_back(Triplet(row, cd.idx[0], -wbar * cd.restInverse));
        triplets.push_back(Triplet(row, cd.idx[1],  wbar * cd.restInverse));
        row += 1;
      } else if (cd.kind == KIND_TRIANGLE) {
        cd.nRows = 2;
        for (int k = 0; k < 2; ++k) {
          triplets.push_back(Triplet(row + k, cd.idx[0],
                                     -wbar * (cd.rest(0, k) + cd.rest(1, k))));
          triplets.push_back(Triplet(row + k, cd.idx[1], wbar * cd.rest(0, k)));
          triplets.push_back(Triplet(row + k, cd.idx[2], wbar * cd.rest(1, k)));
        }
        row += 2;
      } else {
        cd.nRows = 1;
        triplets.push_back(Triplet(row, cd.idx[0], wbar));
        row += 1;
      }
    }
    A = SparseMatrix(row, nPoints);
    A.setFromTriplets(triplets.begin(), triplets.end());
    AtA = SparseMatrix(A.transpose() * A);
  }

  InverseStatus projectAll(Scalar branchTolerance) {
    P.setZero(3, static_cast<int>(A.rows()));
    bool degenerate = false, nonsmooth = false;
    for (const ConstraintData &cd : constraints) {
      ProjectionEval ev = evaluateProjection(cd, X, branchTolerance);
      if (ev.degenerate) degenerate = true;
      if (ev.nonsmooth) nonsmooth = true;
      P.block(0, cd.rowStart, 3, cd.nRows) = ev.block;
    }
    if (degenerate) return InverseStatus::DegenerateGeometry;
    if (nonsmooth) return InverseStatus::Nonsmooth;
    return InverseStatus::Success;
  }

  void energyAndGradient(Scalar &energy, Scalar &gradRes) const {
    energy = 0.0;
    gradRes = 0.0;
    for (int a = 0; a < 3; ++a) {
      VectorX residual = A * X.row(a).transpose() - P.row(a).transpose();
      energy += 0.5 * residual.squaredNorm();
      VectorX g = A.transpose() * residual;
      gradRes = (std::max)(gradRes, g.cwiseAbs().maxCoeff());
    }
  }

  /// Assemble the true equilibrium tangent d/dX of the full gradient, keeping
  /// the projection directions geometry dependent and the three axes coupled.
  InverseStatus assembleTangent() {
    const int n = nPoints;
    std::vector<Triplet> triplets;
    for (int k = 0; k < AtA.outerSize(); ++k)
      for (SparseMatrix::InnerIterator it(AtA, k); it; ++it)
        for (int a = 0; a < 3; ++a)
          triplets.push_back(Triplet(3 * it.row() + a, 3 * it.col() + a, it.value()));

    for (const ConstraintData &cd : constraints) {
      if (cd.kind == KIND_CLOSENESS) continue;
      ProjectionEval ev = evaluateProjection(cd, X, 0.0);
      if (ev.degenerate) return InverseStatus::DegenerateGeometry;
      const int nv = static_cast<int>(cd.idx.size());
      // ev.jacV already differentiates with respect to the involved vertices,
      // including the edge-dof incidence signs.
      for (int li = 0; li < nv; ++li)
        for (int lj = 0; lj < nv; ++lj)
          for (int ai = 0; ai < 3; ++ai)
            for (int bj = 0; bj < 3; ++bj) {
              Scalar v = 0.0;
              for (int k = 0; k < cd.nRows; ++k)
                v += A.coeff(cd.rowStart + k, cd.idx[li]) *
                     ev.jacV(k * 3 + ai, lj * 3 + bj);
              if (v != 0.0)
                triplets.push_back(Triplet(3 * cd.idx[li] + ai,
                                           3 * cd.idx[lj] + bj, -v));
            }
    }
    tangent = SparseMatrix(3 * n, 3 * n);
    tangent.setFromTriplets(triplets.begin(), triplets.end());
    factorization.reset(new Eigen::SimplicialLDLT<SparseMatrix>());
    factorization->compute(tangent);
    if (factorization->info() != Eigen::Success)
      return InverseStatus::SingularTangent;
    VectorX pivots = factorization->vectorD();
    Scalar scale = pivots.cwiseAbs().maxCoeff();
    if (!(scale > 0.0)) return InverseStatus::SingularTangent;
    for (int i = 0; i < pivots.size(); ++i)
      if (pivots(i) <= kPivotRelEps * scale) return InverseStatus::SingularTangent;
    return InverseStatus::Success;
  }
};
///////////////////////////////////////////////////////////////////////////////
EquilibriumSnapshot::EquilibriumSnapshot() : impl_(new Impl()) {}
EquilibriumSnapshot::~EquilibriumSnapshot() {}
const Matrix3X &EquilibriumSnapshot::points() const { return impl_->X; }
Scalar EquilibriumSnapshot::energy() const { return impl_->energyVal; }
Scalar EquilibriumSnapshot::gradientResidual() const { return impl_->gradResVal; }
unsigned int EquilibriumSnapshot::iterations() const { return impl_->iterationVal; }
InverseStatus EquilibriumSnapshot::status() const { return impl_->statusVal; }
int EquilibriumSnapshot::nPoints() const { return impl_->nPoints; }
int EquilibriumSnapshot::nConstraints() const { return static_cast<int>(impl_->constraints.size()); }
int EquilibriumSnapshot::constraintId(int c) const { return impl_->constraints[c].id; }
Scalar EquilibriumSnapshot::userWeight(int c) const { return impl_->constraints[c].userWeight; }
const Vector3 &EquilibriumSnapshot::target(int c) const { return impl_->constraints[c].target; }
///////////////////////////////////////////////////////////////////////////////
PullbackResult EquilibriumSnapshot::pullback(const Matrix3X &G) const {
  PullbackResult result;
  const int nc = static_cast<int>(impl_->constraints.size());
  result.dWeights = VectorX::Zero(nc);
  result.dTargets.assign(nc, Vector3::Zero());
  if (G.rows() != 3 || G.cols() != impl_->nPoints) {
    result.report.status = InverseStatus::InvalidInput;
    return result;
  }
  if (impl_->statusVal != InverseStatus::Success || !impl_->factorization) {
    result.report.status = impl_->statusVal;
    return result;
  }
  const int n = impl_->nPoints;
  VectorX g(3 * n);
  for (int j = 0; j < n; ++j)
    for (int a = 0; a < 3; ++a) g[3 * j + a] = G(a, j);
  VectorX lambda = impl_->factorization->solve(-g);
  if (impl_->factorization->info() != Eigen::Success) {
    result.report.status = InverseStatus::SingularTangent;
    return result;
  }
  // True residual of the adjoint linearized equilibrium equation.
  result.report.adjointResidual = (impl_->tangent * lambda + g).cwiseAbs().maxCoeff();
  result.report.status = InverseStatus::Success;

  Matrix3X Lam(3, n);
  for (int j = 0; j < n; ++j)
    for (int a = 0; a < 3; ++a) Lam(a, j) = lambda[3 * j + a];

  for (int c = 0; c < nc; ++c) {
    const ConstraintData &cd = impl_->constraints[c];
    // Parameter RHS of the implicit equation F(X, p) = A^T(A X^T - P^T) = 0.
    // Both A and P are linear in wbar = sqrt(w) * scale, so F_c is quadratic
    // in wbar and dF_c/dw = F_c / w. The adjoint VJP is lambda^T times that
    // RHS, which keeps the weight influence in both A and P.
    Matrix3X lamBlock(3, cd.nRows);   // A_c^T lambda on the constraint rows
    Matrix3X residual(3, cd.nRows);   // A_c X^T - P_c^T
    for (int k = 0; k < cd.nRows; ++k) {
      for (int a = 0; a < 3; ++a) {
        Scalar lv = 0.0;
        for (int li = 0; li < static_cast<int>(cd.idx.size()); ++li)
          lv += impl_->A.coeff(cd.rowStart + k, cd.idx[li]) * Lam(a, cd.idx[li]);
        lamBlock(a, k) = lv;
        Scalar ax = 0.0;
        for (int li = 0; li < static_cast<int>(cd.idx.size()); ++li)
          ax += impl_->A.coeff(cd.rowStart + k, cd.idx[li]) * impl_->X(a, cd.idx[li]);
        residual(a, k) = ax - impl_->P(a, cd.rowStart + k);
      }
    }
    Scalar vjpW = 0.0;
    for (int k = 0; k < cd.nRows; ++k)
      for (int a = 0; a < 3; ++a)
        vjpW += lamBlock(a, k) * residual(a, k);
    result.dWeights(c) = vjpW / cd.userWeight;

    if (cd.kind == KIND_CLOSENESS) {
      // P row = wbar * target, so dF_c/d t_a = -wbar A_c^T e_a and the VJP is
      // lambda^T dF_c/dt = -wbar (A_c^T lambda), i.e. -wbar * lamBlock.
      Scalar wbar = cd.scaledWeight();
      for (int a = 0; a < 3; ++a)
        result.dTargets[c](a) = -wbar * lamBlock(a, 0);
    }
  }
  return result;
}
///////////////////////////////////////////////////////////////////////////////
Matrix3X EquilibriumSnapshot::tangentMultiply(const Matrix3X &dX) const {
  Matrix3X out = Matrix3X::Zero(3, impl_->nPoints);
  if (impl_->statusVal != InverseStatus::Success ||
      dX.rows() != 3 || dX.cols() != impl_->nPoints)
    return out;
  VectorX v(3 * impl_->nPoints);
  for (int j = 0; j < impl_->nPoints; ++j)
    for (int a = 0; a < 3; ++a) v[3 * j + a] = dX(a, j);
  VectorX Kv = impl_->tangent * v;
  for (int j = 0; j < impl_->nPoints; ++j)
    for (int a = 0; a < 3; ++a) out(a, j) = Kv[3 * j + a];
  return out;
}
///////////////////////////////////////////////////////////////////////////////
struct InverseDesign::Impl {
  Solver *solver = nullptr;
  std::vector<ConstraintData> constraints;

  InverseStatus syncFromSolver(std::vector<ConstraintData> &out) const {
    out.clear();
    if (!solver->isInitialized()) return InverseStatus::InvalidInput;
    if (solver->points_.cols() == 0 || solver->constraints_.empty())
      return InverseStatus::InvalidInput;
    if (!solver->forces_.empty()) return InverseStatus::InvalidInput;
    int row = 0;
    for (int c = 0; c < static_cast<int>(solver->constraints_.size()); ++c) {
      const auto &cs = solver->constraints_[c];
      ConstraintData cd;
      cd.id = c;
      cd.idx = cs->indices();
      cd.userWeight = cs->getUserWeight();
      if (!(cd.userWeight > 0.0)) return InverseStatus::InvalidInput;
      cd.weightScale = cs->getWeightScale();
      if (auto e = std::dynamic_pointer_cast<EdgeStrainConstraint>(cs)) {
        if (cd.idx.size() != 2) return InverseStatus::InvalidInput;
        cd.kind = KIND_EDGE;
        cd.restInverse = e->getRestInverse();
        cd.rangeMin = e->getRangeMin();
        cd.rangeMax = e->getRangeMax();
        cd.nRows = 1;
      } else if (auto t = std::dynamic_pointer_cast<TriangleStrainConstraint>(cs)) {
        if (cd.idx.size() != 3) return InverseStatus::InvalidInput;
        cd.kind = KIND_TRIANGLE;
        cd.rest = t->getRest();
        cd.rangeMin = t->getRangeMin();
        cd.rangeMax = t->getRangeMax();
        cd.nRows = 2;
      } else if (auto cl = std::dynamic_pointer_cast<ClosenessConstraint>(cs)) {
        if (cd.idx.size() != 1) return InverseStatus::InvalidInput;
        cd.kind = KIND_CLOSENESS;
        cd.target = cl->getPosition();
        cd.nRows = 1;
      } else {
        return InverseStatus::InvalidInput;
      }
      if (cd.kind != KIND_CLOSENESS &&
          (!(cd.rangeMin > 0.0 && cd.rangeMax > 0.0) || cd.rangeMin > cd.rangeMax))
        return InverseStatus::InvalidInput;
      cd.rowStart = row;
      row += cd.nRows;
      out.push_back(cd);
    }
    return InverseStatus::Success;
  }
};
///////////////////////////////////////////////////////////////////////////////
InverseDesign::InverseDesign(Solver &solver) : impl_(new Impl()) {
  impl_->solver = &solver;
  InverseStatus st = impl_->syncFromSolver(impl_->constraints);
  if (st != InverseStatus::Success)
    throw std::invalid_argument("InverseDesign: solver setup is not supported");
}
InverseDesign::~InverseDesign() {}
///////////////////////////////////////////////////////////////////////////////
bool InverseDesign::setConstraintWeight(int id, Scalar weight) {
  // Validate through the owning Solver, which updates the constraint and
  // rebuilds every matrix participating in the solve.
  if (!(weight > 0.0)) return false;
  if (id < 0 || id >= static_cast<int>(impl_->solver->constraints_.size()))
    return false;
  if (!impl_->solver->setConstraintWeight(id, weight)) return false;
  return impl_->syncFromSolver(impl_->constraints) == InverseStatus::Success;
}
///////////////////////////////////////////////////////////////////////////////
EquilibriumResult
InverseDesign::equilibrium(unsigned int maxIterations, Scalar tolerance,
                           Scalar branchTolerance) {
  EquilibriumResult result;
  std::shared_ptr<EquilibriumSnapshot> snap(new EquilibriumSnapshot());
  result.snapshot = snap;
  auto finish = [&]() -> EquilibriumResult {
    result.report.status = snap->impl_->statusVal;
    result.report.energy = snap->impl_->energyVal;
    result.report.gradientResidual = snap->impl_->gradResVal;
    result.report.iterations = snap->impl_->iterationVal;
    return result;
  };
  InverseStatus st = impl_->syncFromSolver(snap->impl_->constraints);
  if (st != InverseStatus::Success) { snap->impl_->statusVal = st; return finish(); }
  if (!(tolerance > 0.0) || !(branchTolerance >= 0.0)) {
    snap->impl_->statusVal = InverseStatus::InvalidInput;
    return finish();
  }
  auto &data = snap->impl_;
  data->nPoints = static_cast<int>(impl_->solver->points_.cols());
  data->X = impl_->solver->points_; // work on a copy: failures never publish
  data->buildA();

  // Global step factorization of the constant A^T A block.
  Eigen::SimplicialLDLT<SparseMatrix> globalSolver;
  globalSolver.compute(data->AtA);
  if (globalSolver.info() != Eigen::Success) {
    data->statusVal = InverseStatus::SingularTangent;
    return finish();
  }
  VectorX pivots = globalSolver.vectorD();
  Scalar pivotScale = pivots.cwiseAbs().maxCoeff();
  if (!(pivotScale > 0.0)) {
    data->statusVal = InverseStatus::SingularTangent;
    return finish();
  }
  for (int i = 0; i < pivots.size(); ++i)
    if (pivots(i) <= kPivotRelEps * pivotScale) {
      data->statusVal = InverseStatus::SingularTangent;
      return finish();
    }

  // Local/global iterations with the full-energy gradient infinity norm as
  // the only convergence criterion.
  Scalar energy = 0.0, gradRes = 0.0;
  unsigned int it = 0;
  st = data->projectAll(branchTolerance);
  if (st != InverseStatus::Success && st != InverseStatus::Nonsmooth) {
    data->statusVal = st; // degenerate geometry at the starting point
    return finish();
  }
  data->energyAndGradient(energy, gradRes);
  while (gradRes > tolerance && it < maxIterations) {
    for (int a = 0; a < 3; ++a) {
      VectorX rhs = data->A.transpose() * data->P.row(a).transpose();
      VectorX sol = globalSolver.solve(rhs);
      if (globalSolver.info() != Eigen::Success) {
        data->statusVal = InverseStatus::SingularTangent;
        return finish();
      }
      data->X.row(a) = sol.transpose();
    }
    ++it;
    st = data->projectAll(branchTolerance);
    if (st == InverseStatus::DegenerateGeometry) {
      data->statusVal = st;
      return finish();
    }
    data->energyAndGradient(energy, gradRes);
  }
  if (gradRes > tolerance) {
    data->statusVal = InverseStatus::BudgetExceeded;
    data->energyVal = energy;
    data->gradResVal = gradRes;
    data->iterationVal = it;
    return finish();
  }

  // Converged: full audit of the projection branches, then true tangent.
  st = data->projectAll(branchTolerance);
  if (st != InverseStatus::Success) {
    data->statusVal = st;
    data->energyAndGradient(data->energyVal, data->gradResVal);
    data->iterationVal = it;
    return finish();
  }
  data->energyAndGradient(data->energyVal, data->gradResVal);
  data->iterationVal = it;
  st = data->assembleTangent();
  if (st != InverseStatus::Success) {
    data->statusVal = st;
    return finish();
  }
  data->statusVal = InverseStatus::Success;
  // Publish only after every check has passed; earlier snapshots are separate
  // owning objects and are never touched.
  impl_->solver->points_ = data->X;
  return finish();
}
///////////////////////////////////////////////////////////////////////////////
} // namespace ShapeOp
///////////////////////////////////////////////////////////////////////////////
