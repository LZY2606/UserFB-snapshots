///////////////////////////////////////////////////////////////////////////////
// This file is part of ShapeOp, a lightweight C++ library
// for static and dynamic geometry processing.
//
// Copyright (C) 2014 Sofien Bouaziz <sofien.bouaziz@gmail.com>
// Copyright (C) 2014 LGG EPFL
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
///////////////////////////////////////////////////////////////////////////////
#include "InverseDesign.h"
#include "Solver.h"
#include "LSSolver.h"
#include <stdexcept>
#include <cmath>
#include <limits>
///////////////////////////////////////////////////////////////////////////////
namespace ShapeOp {
///////////////////////////////////////////////////////////////////////////////
// Factorization wrapper keeping the snapshot self contained.
///////////////////////////////////////////////////////////////////////////////
class SnapshotLinearization {
 public:
  Eigen::SimplicialLDLT<SparseMatrix> ldlt;
  Eigen::ComputationInfo info = Eigen::InvalidInput;
};
///////////////////////////////////////////////////////////////////////////////
namespace {
///////////////////////////////////////////////////////////////////////////////
// Axis-major flattening: coordinate (axis a, vertex j) -> index 3*j + a.
///////////////////////////////////////////////////////////////////////////////
VectorX flattenAxisMajor(const Matrix3X &M) {
  int n = static_cast<int>(M.cols());
  VectorX v(3 * n);
  for (int j = 0; j < n; ++j)
    for (int a = 0; a < 3; ++a) v[3 * j + a] = M(a, j);
  return v;
}
///////////////////////////////////////////////////////////////////////////////
Scalar clampScalar(Scalar v, Scalar lo, Scalar hi) {
  return std::max(lo, std::min(hi, v));
}
///////////////////////////////////////////////////////////////////////////////
// Health / branch classification of one strain value.
///////////////////////////////////////////////////////////////////////////////
enum class Branch { Interior, LowerClamp, UpperClamp, Fixed };
///////////////////////////////////////////////////////////////////////////////
Branch classifyStrain(Scalar s, Scalar lo, Scalar hi, Scalar branchTol, bool &nonSmooth) {
  if (lo == hi) return Branch::Fixed;
  if (std::abs(s - lo) <= branchTol || std::abs(s - hi) <= branchTol) {
    nonSmooth = true;
    return s < 0.5 * (lo + hi) ? Branch::LowerClamp : Branch::UpperClamp;
  }
  if (s < lo) return Branch::LowerClamp;
  if (s > hi) return Branch::UpperClamp;
  return Branch::Interior;
}
///////////////////////////////////////////////////////////////////////////////
// Result of differentiating one weighted projection P_c at the geometry X*.
///////////////////////////////////////////////////////////////////////////////
struct ProjectionDiff {
  MatrixXX J;                 // (3*rows) x (3*m), axis major both sides.
  bool degenerate = false;    // Zero edge / rank deficient triangle.
  bool nonSmooth = false;     // A strain value sits on a clamp threshold.
};
///////////////////////////////////////////////////////////////////////////////
// Edge strain: weighted projection p = w * clamp(l*r0) * u, u = (x1-x0)/l.
///////////////////////////////////////////////////////////////////////////////
ProjectionDiff differentiateEdge(const Matrix3X &X, const InverseDesignSnapshot::Record &rec,
                                 Scalar branchTol, Scalar zeroTol) {
  ProjectionDiff out;
  out.J.setZero(3, 6);
  Vector3 x0 = X.col(rec.ids[0]);
  Vector3 x1 = X.col(rec.ids[1]);
  Vector3 d = x1 - x0;
  Scalar l = d.norm();
  if (!(l > zeroTol)) { out.degenerate = true; return out; }
  Vector3 u = d / l;
  Scalar r0 = rec.restInverseLength;
  Scalar s = l * r0;
  bool nonSmooth = false;
  Branch br = classifyStrain(s, rec.edgeMin, rec.edgeMax, branchTol, nonSmooth);
  out.nonSmooth = nonSmooth;
  Scalar b = clampScalar(s, rec.edgeMin, rec.edgeMax);
  Scalar alpha = (br == Branch::Interior) ? 1.0 : 0.0;
  Matrix33 Pouter = u * u.transpose();
  // Dp/Dd = w * (b/l (I - u u^T) + alpha * r0 * u u^T): the u direction
  // moves with the geometry, it is not frozen.
  Matrix33 Dp = rec.scaledWeight * (b / l * (Matrix33::Identity() - Pouter) + alpha * r0 * Pouter);
  for (int k = 0; k < 2; ++k) {
    Scalar sign = (k == 0) ? -1.0 : 1.0;
    for (int a = 0; a < 3; ++a) {
      Vector3 ea = Vector3::Zero(); ea[a] = 1.0;
      out.J.col(3 * k + a) = sign * (Dp * ea);
    }
  }
  return out;
}
///////////////////////////////////////////////////////////////////////////////
// SVD projection derivative for a 2x2 deformation gradient inside one smooth
// branch. F = U diag(sigma) V^T, Fbar = U diag(sbar) V^T with sbar the clamped
// singular values and alpha_i = d(sbar_i)/d(sigma_i) (1 interior, 0 clamped).
// Repeated singular values are handled by the smooth branch limit, so no
// rejection is made on non-unique SVD vectors.
///////////////////////////////////////////////////////////////////////////////
Matrix22 svdProjectionDifferential(const Matrix22 &U, const Matrix22 &V,
                                   const Vector2 &sigma, const Vector2 &sbar,
                                   const Vector2 &alpha, const Matrix22 &dF) {
  Matrix22 Z = U.transpose() * dF * V;
  Matrix22 M = Matrix22::Zero();
  for (int i = 0; i < 2; ++i) M(i, i) = alpha[i] * Z(i, i);
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2; ++j) {
      if (i == j) continue;
      Scalar denom = sigma[j] * sigma[j] - sigma[i] * sigma[i];
      if (std::abs(denom) > 1e-12) {
        // General (non-symmetric dF) smooth-branch formula.
        Scalar cij = (sbar[j] * sigma[j] - sbar[i] * sigma[i]) / denom;
        Scalar cji = (sbar[j] * sigma[i] - sbar[i] * sigma[j]) / denom;
        M(i, j) = cij * Z(i, j) + cji * Z(j, i);
      } else {
        // Smooth branch limit at repeated singular values.
        bool iIn = alpha[i] > 0.5, jIn = alpha[j] > 0.5;
        if (iIn && jIn) {
          M(i, j) = Z(i, j); // Identity projection derivative.
        } else if (!iIn && !jIn && std::abs(sbar[i] - sbar[j]) <= 1e-12) {
          // Equal clamped singular values: pure rotational response.
          M(i, j) = (sbar[i] / (sigma[i] + sigma[j])) * (Z(i, j) - Z(j, i));
        } else {
          // Mixed branch at coincident values: non-smooth, freeze to zero.
          M(i, j) = 0.0;
        }
      }
    }
  }
  return U * M * V.transpose();
}
///////////////////////////////////////////////////////////////////////////////
// Triangle strain: weighted projection Y = w * P * Fbar(F), with P the moving
// in-plane frame, F = P^T E R, E the 3x2 edge matrix and R the rest inverse.
///////////////////////////////////////////////////////////////////////////////
ProjectionDiff differentiateTriangle(const Matrix3X &X, const InverseDesignSnapshot::Record &rec,
                                     Scalar branchTol, Scalar zeroTol) {
  ProjectionDiff out;
  out.J.setZero(6, 9);
  Vector3 x0 = X.col(rec.ids[0]);
  Vector3 x1 = X.col(rec.ids[1]);
  Vector3 x2 = X.col(rec.ids[2]);
  Vector3 e1 = x1 - x0;
  Vector3 e2 = x2 - x0;
  Scalar a = e1.norm();
  if (!(a > zeroTol)) { out.degenerate = true; return out; }
  Vector3 q1 = e1 / a;
  Scalar c01 = e2.dot(q1);
  Vector3 h = e2 - c01 * q1;
  Scalar bLen = h.norm();
  if (!(bLen > zeroTol)) { out.degenerate = true; return out; }
  Vector3 q2 = h / bLen;
  Matrix32 Pframe;
  Pframe.col(0) = q1;
  Pframe.col(1) = q2;
  Matrix32 E;
  E.col(0) = e1;
  E.col(1) = e2;
  Matrix22 G = Pframe.transpose() * E;
  Matrix22 F = G * rec.restInverse;
  Eigen::JacobiSVD<Matrix22> svd(F, Eigen::ComputeFullU | Eigen::ComputeFullV);
  Vector2 sigma = svd.singularValues();
  Vector2 sbar;
  Vector2 alpha;
  for (int i = 0; i < 2; ++i) {
    bool nonSmooth = false;
    Branch br = classifyStrain(sigma[i], rec.triMin, rec.triMax, branchTol, nonSmooth);
    out.nonSmooth = out.nonSmooth || nonSmooth;
    sbar[i] = clampScalar(sigma[i], rec.triMin, rec.triMax);
    alpha[i] = (br == Branch::Interior) ? 1.0 : 0.0;
  }
  Matrix22 Fbar = svd.matrixU() * sbar.asDiagonal() * svd.matrixV().transpose();
  Matrix22 R = rec.restInverse;
  Scalar w = rec.scaledWeight;
  Matrix22 U = svd.matrixU(), V = svd.matrixV();

  // dY/dE by seeding all six axis-major edge coordinate directions.
  MatrixXX dYdE(6, 6);
  for (int seed = 0; seed < 6; ++seed) {
    int k = seed / 3, axis = seed % 3;
    Vector3 ea = Vector3::Zero(); ea[axis] = 1.0;
    Vector3 de1 = (k == 0) ? ea : Vector3::Zero();
    Vector3 de2 = (k == 1) ? ea : Vector3::Zero();
    // Moving frame derivatives.
    Vector3 dq1 = (Matrix33::Identity() - q1 * q1.transpose()) / a * de1;
    Scalar dc01 = de2.dot(q1) + e2.dot(dq1);
    Vector3 dh = de2 - dc01 * q1 - c01 * dq1;
    Vector3 dq2 = (Matrix33::Identity() - q2 * q2.transpose()) / bLen * dh;
    Matrix32 dP; dP.col(0) = dq1; dP.col(1) = dq2;
    Matrix32 dE; dE.col(0) = de1; dE.col(1) = de2;
    Matrix22 dG = dP.transpose() * E + Pframe.transpose() * dE;
    Matrix22 dF = dG * R;
    Matrix22 dFbar = svdProjectionDifferential(U, V, sigma, sbar, alpha, dF);
    Matrix32 dY = w * (dP * Fbar + Pframe * dFbar);
    for (int kk = 0; kk < 2; ++kk)
      for (int aa = 0; aa < 3; ++aa) dYdE(3 * kk + aa, seed) = dY(aa, kk);
  }
  // E = B Xc with e1 = x1-x0, e2 = x2-x0.
  MatrixXX Btr(6, 9);
  Btr.setZero();
  for (int aa = 0; aa < 3; ++aa) {
    Btr(aa, 3 * 0 + aa) = -1.0; Btr(aa, 3 * 1 + aa) = 1.0;
    Btr(3 + aa, 3 * 0 + aa) = -1.0; Btr(3 + aa, 3 * 2 + aa) = 1.0;
  }
  out.J = dYdE * Btr;
  return out;
}
///////////////////////////////////////////////////////////////////////////////
// Dense I3 kron Ac for one constraint.
///////////////////////////////////////////////////////////////////////////////
MatrixXX axisKron(const MatrixXX &Ac, int rows, int m) {
  MatrixXX out = MatrixXX::Zero(3 * rows, 3 * m);
  for (int r = 0; r < rows; ++r)
    for (int j = 0; j < m; ++j)
      for (int a = 0; a < 3; ++a)
        out(3 * r + a, 3 * j + a) = Ac(r, j);
  return out;
}
///////////////////////////////////////////////////////////////////////////////
// Rebuild the weighted A matrix from the current constraints (same path as
// Solver::initialize) and return it together with row offsets.
///////////////////////////////////////////////////////////////////////////////
struct BuiltSystem {
  SparseMatrix A;
  std::vector<int> rowOffset;
};
///////////////////////////////////////////////////////////////////////////////
// Authoritative gradient of the full energy E(X) = 1/2 ||A3 X - P(X)||^2:
// g = (A3 - J3)^T r with r = A3 X - P(X). Evaluated sparsely from the
// analytic projection Jacobians; no n^2 dense matrix is formed.
///////////////////////////////////////////////////////////////////////////////
VectorX trueEnergyGradient(const Matrix3X &X,
                           const std::vector<std::shared_ptr<Constraint>> &constraints,
                           const SparseMatrix &A, const std::vector<int> &rowOffset,
                           Scalar branchTolerance, Scalar zeroTol,
                           bool &degenerate, bool &nonSmooth) {
  int nPoints = static_cast<int>(X.cols());
  int nRows = static_cast<int>(A.rows());
  Matrix3X P(3, nRows);
  for (auto &c : constraints) c->project(X, P);
  MatrixXX residual = A * X.transpose() - P.transpose(); // nRows x 3
  VectorX grad = VectorX::Zero(3 * nPoints);
  for (std::size_t c = 0; c < constraints.size(); ++c) {
    const auto &constr = constraints[c];
    InverseDesignType type = constr->inverseDesignType();
    const std::vector<int> &ids = constr->indices();
    int row = rowOffset[c];
    InverseDesignSnapshot::Record rec;
    rec.ids = ids;
    rec.userWeight = constr->getWeight();
    rec.weightScale = constr->weightScale();
    rec.scaledWeight = constr->scaledWeight();
    ProjectionDiff diff;
    if (type == InverseDesignType::EdgeStrain) {
      auto e = std::static_pointer_cast<EdgeStrainConstraint>(constr);
      rec.rows = 1;
      rec.restInverseLength = e->inverseRestLength();
      rec.edgeMin = e->rangeMin();
      rec.edgeMax = e->rangeMax();
      diff = differentiateEdge(X, rec, branchTolerance, zeroTol);
    } else if (type == InverseDesignType::TriangleStrain) {
      auto t = std::static_pointer_cast<TriangleStrainConstraint>(constr);
      rec.rows = 2;
      rec.restInverse = t->inverseRestMatrix();
      rec.triMin = t->rangeMin();
      rec.triMax = t->rangeMax();
      diff = differentiateTriangle(X, rec, branchTolerance, zeroTol);
    } else {
      rec.rows = 1; // Closeness: J = 0.
    }
    degenerate |= diff.degenerate;
    nonSmooth |= diff.nonSmooth;
    if (diff.degenerate) continue;
    int m = static_cast<int>(ids.size());
    // Local residual rc and local A3 block.
    VectorX rc(3 * rec.rows);
    for (int rr = 0; rr < rec.rows; ++rr)
      for (int a = 0; a < 3; ++a) rc[3 * rr + a] = residual(row + rr, a);
    MatrixXX Ac(rec.rows, m);
    for (int rr = 0; rr < rec.rows; ++rr)
      for (int j = 0; j < m; ++j) Ac(rr, j) = A.coeff(row + rr, ids[j]);
    MatrixXX A3 = axisKron(Ac, rec.rows, m);
    VectorX local = A3.transpose() * rc;                 // A3_c^T r_c
    if (diff.J.size() > 0) local -= diff.J.transpose() * rc; // -J3_c^T r_c
    for (int k = 0; k < m; ++k)
      for (int a = 0; a < 3; ++a) grad[3 * ids[k] + a] += local[3 * k + a];
  }
  return grad;
}
///////////////////////////////////////////////////////////////////////////////
BuiltSystem buildMatrix(const std::vector<std::shared_ptr<Constraint>> &constraints, int nPoints) {
  std::vector<Triplet> triplets;
  BuiltSystem sys;
  sys.rowOffset.resize(constraints.size());
  int idO = 0;
  for (std::size_t c = 0; c < constraints.size(); ++c) {
    sys.rowOffset[c] = idO;
    constraints[c]->addConstraint(triplets, idO);
  }
  sys.A = SparseMatrix(idO, nPoints);
  sys.A.setFromTriplets(triplets.begin(), triplets.end());
  return sys;
}
///////////////////////////////////////////////////////////////////////////////
} // namespace
///////////////////////////////////////////////////////////////////////////////
InverseDesignSnapshot::InverseDesignSnapshot() {}
InverseDesignSnapshot::~InverseDesignSnapshot() {}
///////////////////////////////////////////////////////////////////////////////
PullbackResult InverseDesignSnapshot::pullback(const Matrix3X &G) const {
  PullbackResult result;
  if (G.rows() != 3 || G.cols() != points_.cols())
    throw std::invalid_argument("pullback: G must have the same shape as the point matrix");
  if (!linearization_ || linearization_->info != Eigen::Success)
    throw std::runtime_error("pullback: snapshot linearization is not valid");
  result.targetGradients = Matrix3X::Zero(3, nConstraints_);
  result.weightGradients = VectorX::Zero(nConstraints_);

  VectorX g = flattenAxisMajor(G);
  VectorX lambda = linearization_->ldlt.solve(g);
  if (linearization_->ldlt.info() != Eigen::Success) { result.valid = false; return result; }
  // True adjoint residual of the actually solved coupled system.
  result.adjointResidual = (tangent_ * lambda - g).lpNorm<Eigen::Infinity>();

  // Full axis-major residual r3 = vec(A X^T - P^T).
  Matrix3X XP = points_ * A_.transpose() - projections_;
  VectorX r3 = flattenAxisMajor(XP);

  for (int c = 0; c < nConstraints_; ++c) {
    const Record &rec = records_[c];
    int m = static_cast<int>(rec.ids.size());
    MatrixXX Ac = MatrixXX::Zero(rec.rows, m);
    for (int r = 0; r < rec.rows; ++r)
      for (int j = 0; j < m; ++j) Ac(r, j) = A_.coeff(rec.row + r, rec.ids[j]);
    MatrixXX A3 = axisKron(Ac, rec.rows, m);
    VectorX rc(3 * rec.rows);
    for (int r = 0; r < rec.rows; ++r)
      for (int a = 0; a < 3; ++a) rc[3 * r + a] = r3[3 * (rec.row + r) + a];
    VectorX lambdac(3 * m);
    for (int k = 0; k < m; ++k)
      for (int a = 0; a < 3; ++a) lambdac[3 * k + a] = lambda[3 * rec.ids[k] + a];
    // Weight enters both A (through sqrt(w)) and P; dg/dw = (1/w) A3^T r.
    VectorX gradContrib = A3.transpose() * rc;
    result.weightGradients[c] = -lambdac.dot(gradContrib) / rec.userWeight;
    if (rec.type == InverseDesignType::Closeness) {
      // dL/dt = w_user * lambda on the controlled vertex.
      for (int a = 0; a < 3; ++a)
        result.targetGradients(a, c) = rec.userWeight * lambda[3 * rec.ids[0] + a];
    }
  }
  result.valid = true;
  return result;
}
///////////////////////////////////////////////////////////////////////////////
} // namespace ShapeOp
///////////////////////////////////////////////////////////////////////////////
// Solver integration.
///////////////////////////////////////////////////////////////////////////////
#include "Constraint.h"
namespace ShapeOp {
///////////////////////////////////////////////////////////////////////////////
std::shared_ptr<InverseDesignSnapshot>
Solver::solveEquilibrium(const EquilibriumOptions &options, EquilibriumReport &report) {
  report = EquilibriumReport();
  int nPoints = static_cast<int>(points_.cols());
  if (nPoints == 0 || constraints_.empty() || !solver_) {
    report.status = EquilibriumStatus::InvalidInput;
    return nullptr;
  }
  if (dynamic_ || !forces_.empty()) {
    report.status = EquilibriumStatus::InvalidInput;
    return nullptr;
  }
  for (auto &c : constraints_) {
    if (c->inverseDesignType() == InverseDesignType::Unsupported) {
      report.status = EquilibriumStatus::InvalidInput;
      return nullptr;
    }
  }
  if (!(options.tolerance > 0.0) || options.maxIterations == 0 ||
      !(options.branchTolerance >= 0.0)) {
    report.status = EquilibriumStatus::InvalidInput;
    return nullptr;
  }

  BuiltSystem sys = buildMatrix(constraints_, nPoints);
  const SparseMatrix &A = sys.A;
  SparseMatrix At = A.transpose();
  int nRows = static_cast<int>(A.rows());

  // Work on a trial copy: failed equilibria never publish trial positions and
  // cannot damage an earlier snapshot held by the caller.
  Matrix3X X = points_;
  Matrix3X projections = Matrix3X::Zero(3, nRows);
  Matrix3X checkProjections = Matrix3X::Zero(3, nRows);
  Scalar gradInf = std::numeric_limits<Scalar>::infinity();
  Scalar energy = 0.0;
  unsigned int it = 0;
  bool converged = false;
  Scalar scale0 = std::max<Scalar>(1.0, X.cwiseAbs().maxCoeff());
  Scalar zeroTol = 1e-12 * scale0;
  // The fixed point and the energy stationary point coincide. The cheap
  // fixed-point residual A3^T r is used as a gate; success is decided solely
  // by the authoritative full-energy gradient (A3-J3)^T r.
  for (; it < options.maxIterations; ++it) {
    for (auto &c : constraints_) c->project(X, projections);
    if (!projections.allFinite()) {
      report.status = EquilibriumStatus::DegenerateGeometry;
      report.iterations = it;
      return nullptr;
    }
    for (int a = 0; a < 3; ++a) {
      VectorX rhs = At * projections.row(a).transpose();
      X.row(a) = solver_->solve(rhs, X.row(a).transpose()).transpose();
    }
    if (!X.allFinite() || solver_->info() != Eigen::Success) {
      report.status = EquilibriumStatus::DegenerateGeometry;
      report.iterations = it + 1;
      return nullptr;
    }
    // Energy and cheap fixed-point residual A3^T r for the iteration history.
    for (auto &c : constraints_) c->project(X, checkProjections);
    MatrixXX residual = A * X.transpose() - checkProjections.transpose();
    energy = 0.5 * residual.squaredNorm();
    Scalar cheapGrad = (At * residual).cwiseAbs().maxCoeff();
    report.iterations = it + 1;
    // Once the fixed point is reached, decide success solely on the
    // authoritative full-energy gradient; keep iterating otherwise.
    if (cheapGrad <= options.tolerance) {
      bool dg = false, ns = false;
      VectorX trueGrad = trueEnergyGradient(X, constraints_, A, sys.rowOffset,
                                             options.branchTolerance, zeroTol, dg, ns);
      if (dg) {
        report.status = EquilibriumStatus::DegenerateGeometry;
        report.energy = energy;
        report.gradientResidual = cheapGrad;
        return nullptr;
      }
      if (ns) {
        report.status = EquilibriumStatus::NonSmooth;
        report.energy = energy;
        report.gradientResidual = trueGrad.cwiseAbs().maxCoeff();
        return nullptr;
      }
      gradInf = trueGrad.cwiseAbs().maxCoeff();
      if (gradInf <= options.tolerance) { converged = true; break; }
    } else {
      gradInf = cheapGrad;
    }
  }
  report.energy = energy;
  report.gradientResidual = gradInf;
  if (!converged) {
    // Report the authoritative full-energy gradient at the final geometry.
    bool dg = false, ns = false;
    VectorX trueGrad = trueEnergyGradient(X, constraints_, A, sys.rowOffset,
                                           options.branchTolerance, zeroTol, dg, ns);
    if (!dg) report.gradientResidual = trueGrad.cwiseAbs().maxCoeff();
    report.status = EquilibriumStatus::BudgetExceeded;
    return nullptr;
  }

  // From here on build the owning snapshot and its analytic linearization.
  Matrix3X P(3, nRows);
  for (auto &c : constraints_) c->project(X, P);

  auto snapshot = std::shared_ptr<InverseDesignSnapshot>(new InverseDesignSnapshot());
  snapshot->n_ = nPoints;
  snapshot->nConstraints_ = static_cast<int>(constraints_.size());
  snapshot->nRows_ = nRows;
  snapshot->points_ = X;
  snapshot->projections_ = P;
  snapshot->A_ = A;
  snapshot->At_ = At;
  snapshot->records_.resize(constraints_.size());

  bool degenerate = false;
  bool nonSmooth = false;
  std::vector<Triplet> tangentTriplets;

  // The genuine three-axis coupled equilibrium tangent. The local/global
  // fixed point residual is F(X) = A3^T (A3 vec(X) - vec P(X)); its exact
  // Jacobian is K = A3^T A3 - A3^T J with J the analytic projection
  // derivative. A3^T A3 = I3 kron (A^T A) is the shared base and each strain
  // constraint adds -A3_c^T J_c; Closeness has J = 0. Assembled sparsely,
  // never as an n^2 dense matrix.
  SparseMatrix N = At * A;
  for (int k = 0; k < N.outerSize(); ++k)
    for (SparseMatrix::InnerIterator itN(N, k); itN; ++itN)
      for (int a = 0; a < 3; ++a)
        tangentTriplets.emplace_back(3 * itN.row() + a, 3 * itN.col() + a, itN.value());

  for (int c = 0; c < static_cast<int>(constraints_.size()); ++c) {
    auto &constr = constraints_[c];
    auto &rec = snapshot->records_[c];
    rec.type = constr->inverseDesignType();
    rec.ids = constr->indices();
    rec.row = sys.rowOffset[c];
    rec.userWeight = constr->getWeight();
    rec.weightScale = constr->weightScale();
    rec.scaledWeight = constr->scaledWeight();
    if (rec.type == InverseDesignType::EdgeStrain) {
      auto e = std::static_pointer_cast<EdgeStrainConstraint>(constr);
      rec.rows = 1;
      rec.restInverseLength = e->inverseRestLength();
      rec.edgeMin = e->rangeMin();
      rec.edgeMax = e->rangeMax();
      ProjectionDiff diff = differentiateEdge(X, rec, options.branchTolerance, zeroTol);
      rec.projectionJacobian = diff.J;
      degenerate |= diff.degenerate;
      nonSmooth |= diff.nonSmooth;
      if (!diff.degenerate) {
        MatrixXX Ac(1, 2);
        Ac(0, 0) = A.coeff(rec.row, rec.ids[0]);
        Ac(0, 1) = A.coeff(rec.row, rec.ids[1]);
        MatrixXX A3 = axisKron(Ac, 1, 2);
        MatrixXX correction = -A3.transpose() * diff.J;
        for (int i = 0; i < 6; ++i)
          for (int j = 0; j < 6; ++j) {
            int gi = 3 * rec.ids[i / 3] + i % 3;
            int gj = 3 * rec.ids[j / 3] + j % 3;
            tangentTriplets.emplace_back(gi, gj, correction(i, j));
          }
      }
    } else if (rec.type == InverseDesignType::TriangleStrain) {
      auto t = std::static_pointer_cast<TriangleStrainConstraint>(constr);
      rec.rows = 2;
      rec.restInverse = t->inverseRestMatrix();
      rec.triMin = t->rangeMin();
      rec.triMax = t->rangeMax();
      ProjectionDiff diff = differentiateTriangle(X, rec, options.branchTolerance, zeroTol);
      rec.projectionJacobian = diff.J;
      degenerate |= diff.degenerate;
      nonSmooth |= diff.nonSmooth;
      if (!diff.degenerate) {
        MatrixXX Ac(2, 3);
        for (int r = 0; r < 2; ++r)
          for (int j = 0; j < 3; ++j) Ac(r, j) = A.coeff(rec.row + r, rec.ids[j]);
        MatrixXX A3 = axisKron(Ac, 2, 3);
        MatrixXX correction = -A3.transpose() * diff.J;
        for (int i = 0; i < 9; ++i)
          for (int j = 0; j < 9; ++j) {
            int gi = 3 * rec.ids[i / 3] + i % 3;
            int gj = 3 * rec.ids[j / 3] + j % 3;
            tangentTriplets.emplace_back(gi, gj, correction(i, j));
          }
      }
    } else {
      rec.rows = 1;
      auto cl = std::static_pointer_cast<ClosenessConstraint>(constr);
      rec.target = cl->getPosition();
      // Closeness projection is constant in X (J = 0): the shared A^T A base
      // already contains its full tangent block.
    }
  }
  if (degenerate) {
    report.status = EquilibriumStatus::DegenerateGeometry;
    return nullptr;
  }
  if (nonSmooth) {
    report.status = EquilibriumStatus::NonSmooth;
    return nullptr;
  }

  SparseMatrix K(3 * nPoints, 3 * nPoints);
  K.setFromTriplets(tangentTriplets.begin(), tangentTriplets.end());
  auto lin = std::make_shared<SnapshotLinearization>();
  lin->ldlt.compute(K);
  lin->info = lin->ldlt.info();
  if (lin->info != Eigen::Success) {
    report.status = EquilibriumStatus::SingularTangent;
    return nullptr;
  }
  const VectorX &diag = lin->ldlt.vectorD();
  Scalar minPivot = std::numeric_limits<Scalar>::infinity();
  Scalar maxPivot = 0.0;
  for (int i = 0; i < diag.size(); ++i) {
    minPivot = std::min(minPivot, diag[i]);
    maxPivot = std::max(maxPivot, std::abs(diag[i]));
  }
  if (!(minPivot > 1e-12 * std::max<Scalar>(1.0, maxPivot))) {
    report.status = EquilibriumStatus::SingularTangent;
    return nullptr;
  }
  snapshot->tangent_ = K;
  snapshot->linearization_ = lin;
  report.status = EquilibriumStatus::Success;
  snapshot->report_ = report;
  points_ = X; // Publish only on full success.
  return snapshot;
}
///////////////////////////////////////////////////////////////////////////////
} // namespace ShapeOp
