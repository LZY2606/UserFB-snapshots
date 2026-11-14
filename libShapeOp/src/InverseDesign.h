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
#ifndef INVERSEDESIGN_H
#define INVERSEDESIGN_H
///////////////////////////////////////////////////////////////////////////////
#include "Types.h"
#include "Constraint.h"
#include <memory>
#include <vector>
///////////////////////////////////////////////////////////////////////////////
/** \file
This file adds static inverse design to ShapeOp: a converged equilibrium entry
point and an owning snapshot supporting implicit (adjoint) differentiation of
the converged geometry with respect to Closeness targets and constraint weights.

The energy minimized at equilibrium is, with A_c the weighted block produced by
the original Constraint::addConstraint and P_c(X) the weighted projection
produced by the original Constraint::project:
\f[
  E(X) = \frac12 \sum_c \| A_c X^\top - P_c(X)^\top \|_F^2 .
\f]
Only EdgeStrainConstraint, TriangleStrainConstraint and ClosenessConstraint are
supported; any other constraint type is rejected.
*/
///////////////////////////////////////////////////////////////////////////////
namespace ShapeOp {
///////////////////////////////////////////////////////////////////////////////
class Solver;
///////////////////////////////////////////////////////////////////////////////
/** \brief Terminal state of ShapeOp::Solver::solveEquilibrium.*/
enum class EquilibriumStatus {
  Success,            ///< Full energy gradient infinity norm reached the tolerance.
  BudgetExceeded,     ///< Iteration budget spent without reaching the tolerance.
  DegenerateGeometry, ///< Zero length edge or rank deficient triangle at the solution.
  NonSmooth,          ///< A strain value sits within the branch tolerance of a clamp threshold.
  SingularTangent,    ///< The equilibrium tangent is not positive definite / cannot be factorized.
  InvalidInput        ///< Unsupported constraint type or otherwise invalid configuration.
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Options controlling ShapeOp::Solver::solveEquilibrium.*/
struct EquilibriumOptions {
  unsigned int maxIterations = 1000; ///< Local/global iteration budget.
  Scalar tolerance = 1e-10;          ///< Infinity norm tolerance on the full energy gradient.
  /** \brief Distance (on length ratio / singular value scale) below which a
      strain value next to a clamp bound is reported as non-smooth.*/
  Scalar branchTolerance = 1e-8;
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Report returned by ShapeOp::Solver::solveEquilibrium.*/
struct EquilibriumReport {
  EquilibriumStatus status = EquilibriumStatus::InvalidInput; ///< Terminal state.
  Scalar energy = 0.0;            ///< True value of E(X) at the final geometry.
  Scalar gradientResidual = 0.0;  ///< Infinity norm of the gradient of the full energy.
  unsigned int iterations = 0;    ///< Number of local/global iterations actually performed.
  /** \brief True exactly when status is EquilibriumStatus::Success.*/
  bool success() const { return status == EquilibriumStatus::Success; }
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Result of one ShapeOp::InverseDesignSnapshot::pullback call.
Each call returns independent storage; repeated calls never accumulate.*/
struct PullbackResult {
  /** \brief dL/d(target) for every constraint, indexed by the original constraint id.
      Column c holds the 3D derivative with respect to the Closeness target of
      constraint c, and is zero for every non-Closeness constraint.*/
  Matrix3X targetGradients;
  /** \brief dL/d(user weight) for every constraint, indexed by the original
      constraint id. The vector has one entry per constraint (zero is possible).*/
  VectorX weightGradients;
  /** \brief Infinity norm of the true adjoint residual ||K lambda - vec(G)||.*/
  Scalar adjointResidual = 0.0;
  /** \brief False if the linearization could not be solved.*/
  bool valid = false;
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Owning equilibrium snapshot with an implicit-differentiation pullback.

The snapshot owns the converged geometry, all Closeness targets, all material
parameters (rest shapes, strain ranges, weight scalings and user weights,
topology) and the factorized, three-axis coupled sparse equilibrium tangent.
It stays valid after the originating Solver and Constraint objects are
destroyed or edited.

Analytic projection derivatives (including the geometry dependent projection
directions and both the A and P dependence on weights) are used; no finite
differences, no per-parameter re-solves, no automatic differentiation and no
unrolling of the local/global iterations take place. Repeated singular values
inside one smooth projection branch are supported.
*/
class SHAPEOP_API InverseDesignSnapshot {
  friend class Solver;
 public:
  ~InverseDesignSnapshot();
  /** \brief Converged geometry X*, a 3 by n point matrix.*/
  const Matrix3X &points() const { return points_; }
  /** \brief Number of points.*/
  int nPoints() const { return static_cast<int>(points_.cols()); }
  /** \brief Number of constraints (the original addConstraint numbering).*/
  int nConstraints() const { return nConstraints_; }
  /** \brief Terminal status of the equilibrium that produced the snapshot.*/
  EquilibriumStatus status() const { return report_.status; }
  /** \brief Report of the equilibrium that produced the snapshot.*/
  const EquilibriumReport &report() const { return report_; }
  /** \brief Weighted system matrix A (projection rows by point indices).*/
  const SparseMatrix &systemMatrix() const { return A_; }
  /** \brief Weighted projections P(X*) in the 3 by projection-row layout.*/
  const Matrix3X &projections() const { return projections_; }
  /** \brief Factorized three-axis coupled equilibrium tangent K (3n by 3n),
      stored sparsely.*/
  const SparseMatrix &tangent() const { return tangent_; }
  /** \brief Analytic projection Jacobian J_c (axis-major) of constraint c.*/
  const MatrixXX &projectionJacobian(int c) const { return records_.at(c).projectionJacobian; }
  /** \brief First projection row of constraint c.*/
  int constraintRow(int c) const { return records_.at(c).row; }
  /** \brief One-past-last projection row of constraint c.*/
  int constraintRowEnd(int c) const { return records_.at(c).row + records_.at(c).rows; }
  /** \brief Vertex indices of constraint c.*/
  const std::vector<int> &constraintIndices(int c) const { return records_.at(c).ids; }
  /** \brief Adjoint pullback.
      \param G A matrix with the same shape as the point matrix. The scalar
             objective is L = sum_{i,j} G_ij X*_ij.
      \return Derivatives with respect to every Closeness target 3D coordinate
             and every original user constraint weight, plus the true adjoint
             residual. The same snapshot (and its factorization) is reused
             across calls; outputs do not alias each other.
  */
  PullbackResult pullback(const Matrix3X &G) const;

  /** \brief Per-constraint record of the owned linearization. Exposed for the
      analytic projection derivatives implemented in the translation unit.*/
  struct Record {
    InverseDesignType type = InverseDesignType::Unsupported;
    std::vector<int> ids;
    int row = 0;          // First projection row.
    int rows = 0;         // Projection rows (1 edge/closeness, 2 triangle).
    Scalar userWeight = 0.0;
    Scalar weightScale = 1.0;
    Scalar scaledWeight = 1.0;
    // Edge.
    Scalar restInverseLength = 0.0;
    Scalar edgeMin = 1.0, edgeMax = 1.0;
    // Triangle.
    Matrix22 restInverse;
    Scalar triMin = 1.0, triMax = 1.0;
    // Closeness target.
    Vector3 target;
    // Analytic projection Jacobian in axis-major ordering,
    // (3*rows) by (3*(ids.size())).
    MatrixXX projectionJacobian;
  };

 private:
  InverseDesignSnapshot();
  // Number of point coordinates.
  int n_ = 0;
  // Number of constraints.
  int nConstraints_ = 0;
  // Number of projection rows.
  int nRows_ = 0;
  // Converged geometry.
  Matrix3X points_;
  // Weighted projections P(X*) (3 by nRows_, ShapeOp layout).
  Matrix3X projections_;
  // Weighted system matrix A (nRows_ by n_) and its transpose.
  SparseMatrix A_, At_;
  // Factorized, three-axis coupled equilibrium tangent K (3n by 3n).
  SparseMatrix tangent_;
  mutable std::shared_ptr<class SnapshotLinearization> linearization_;
  std::vector<Record> records_;
  EquilibriumReport report_;
};
///////////////////////////////////////////////////////////////////////////////
/** \par Compilable usage example
The equilibrium entry returns an owning snapshot. Its pullback gives the
derivative of a mesh-target error with respect to the soft control targets
(and the constraint weights). A small gradient step on the targets, followed by
a re-solve, reduces the error:

\code
#include "Solver.h"
#include "Constraint.h"
#include "InverseDesign.h"
#include <cassert>
#include <memory>

using namespace ShapeOp;

void optimizeSoftTargets(Solver& solver, const Matrix3X& desiredMesh) {
  EquilibriumOptions options;
  options.maxIterations = 5000;
  options.tolerance = 1e-12;
  EquilibriumReport report;
  std::shared_ptr<InverseDesignSnapshot> snapshot = solver.solveEquilibrium(options, report);
  if (!snapshot) return;                       // Failure: nothing was published.

  // Objective L = 1/2 ||X* - desiredMesh||^2, so G = X* - desiredMesh.
  Matrix3X G = snapshot->points() - desiredMesh;
  PullbackResult d = snapshot->pullback(G);

  const Scalar step = 0.25;
  for (int c = 0; c < snapshot->nConstraints(); ++c) {
    auto constraint = solver.getConstraint(c);
    if (constraint->inverseDesignType() != InverseDesignType::Closeness) continue;
    auto closeness = std::dynamic_pointer_cast<ClosenessConstraint>(constraint);
    // targetGradients is indexed by the original addConstraint id.
    closeness->setPosition(closeness->getPosition() - step * d.targetGradients.col(c));
  }

  // Re-solve with the updated targets and confirm the mesh error decreased.
  EquilibriumReport report2;
  std::shared_ptr<InverseDesignSnapshot> snapshot2 = solver.solveEquilibrium(options, report2);
  if (snapshot2) {
    Scalar errorBefore = (snapshot->points()  - desiredMesh).squaredNorm();
    Scalar errorAfter  = (snapshot2->points() - desiredMesh).squaredNorm();
    assert(errorAfter < errorBefore);
  }
}
\endcode
*/
///////////////////////////////////////////////////////////////////////////////
} // namespace ShapeOp
///////////////////////////////////////////////////////////////////////////////
#ifdef SHAPEOP_HEADER_ONLY
#include "InverseDesign.cpp"
#endif
///////////////////////////////////////////////////////////////////////////////
#endif // INVERSEDESIGN_H
///////////////////////////////////////////////////////////////////////////////
