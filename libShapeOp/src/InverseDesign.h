///////////////////////////////////////////////////////////////////////////////
// This file is part of ShapeOp, a lightweight C++ library
// for static and dynamic geometry processing.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
///////////////////////////////////////////////////////////////////////////////
#ifndef INVERSE_DESIGN_H
#define INVERSE_DESIGN_H
///////////////////////////////////////////////////////////////////////////////
#include "Types.h"
#include "Constraint.h"
#include <memory>
#include <vector>
#include <string>
///////////////////////////////////////////////////////////////////////////////
/** \file
This file adds static equilibrium solving with a true gradient-infinity-norm
convergence criterion and implicit differentiation (adjoint pullback) of the
converged shape with respect to Closeness targets and user constraint weights.

Only EdgeStrainConstraint, TriangleStrainConstraint and ClosenessConstraint
are supported. There are no dynamics and no external forces; the reference
shapes (rest edge lengths, rest triangle matrices and closeness targets) are
fixed while a snapshot is alive.

The minimized energy is, with the same convention as the local/global solver:
\f[
  E(X) = \frac12 \sum_c \| A_c X^T - P_c(X)^T \|_F^2,
\f]
where A_c is the weighted block produced by the constraint's addConstraint and
P_c(X) is its weighted projection. A static equilibrium is a stationary point
\f$X^\star\f$ of E, i.e. \f$\nabla E(X^\star) = A^T(A X^T - P(X)^T) = 0\f$
(rows of the 3 by n point matrices are the three spatial axes).
*/
///////////////////////////////////////////////////////////////////////////////
namespace ShapeOp {
///////////////////////////////////////////////////////////////////////////////
class Solver;
class EquilibriumSnapshot;
///////////////////////////////////////////////////////////////////////////////
/** \brief Reason codes reported by EquilibriumReport and AdjointReport.*/
enum class InverseStatus {
  Success,            ///< Everything converged and the linearization is valid.
  InvalidInput,       ///< Unsupported constraint type or illegal arguments.
  DegenerateGeometry, ///< Zero-length edge or rank-deficient (degenerate) triangle.
  Nonsmooth,          ///< A projection is within the branch tolerance of a kink.
  SingularTangent,    ///< The equilibrium tangent is not positive definite / is singular.
  BudgetExceeded      ///< The iteration budget was used before the tolerance was met.
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Report of a static equilibrium run.*/
struct EquilibriumReport {
  InverseStatus status = InverseStatus::InvalidInput; ///< Outcome of the run.
  Scalar energy = 0.0;        ///< True energy E(X) at the published geometry.
  Scalar gradientResidual = 0.0; ///< Infinity norm of the full energy gradient.
  unsigned int iterations = 0; ///< Number of local/global iterations actually run.
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Bundle returned by InverseDesign::equilibrium: the convergence
    report together with the owning equilibrium snapshot. The snapshot is
    always returned; on failure its status() matches the report and its
    pullback() is refused.*/
struct EquilibriumResult {
  EquilibriumReport report;                       ///< Convergence report.
  std::shared_ptr<EquilibriumSnapshot> snapshot;  ///< Owning geometry/linearization.
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Report of an adjoint (pullback) solve.*/
struct AdjointReport {
  InverseStatus status = InverseStatus::InvalidInput; ///< Outcome of the pullback.
  Scalar adjointResidual = 0.0; ///< Infinity norm of the residual of the adjoint linear system.
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Result structure of EquilibriumSnapshot::pullback.

    Both arrays are indexed by the original addConstraint id. dTargets holds
    \f$\partial L/\partial t_c\f$ for every ClosenessConstraint target (entries
    for non-closeness constraints are zero vectors); dWeights holds
    \f$\partial L/\partial w_c\f$ for the user weight of every constraint.
*/
struct PullbackResult {
  AdjointReport report;                 ///< Solver status and adjoint residual.
  std::vector<Vector3> dTargets;        ///< Target VJPs, indexed by constraint id.
  VectorX dWeights;                     ///< Weight VJPs, indexed by constraint id.
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Owning snapshot of a converged static equilibrium and its linearization.

    A snapshot owns copies of the geometry, closeness targets, user weights,
    rest data and the factorized equilibrium tangent. It stays valid after the
    originating Solver and Constraint objects have been destroyed or edited.
    Repeated pullback() calls are independent: results never accumulate and the
    stored linearization is reused.
*/
class SHAPEOP_API EquilibriumSnapshot {
 public:
  ~EquilibriumSnapshot();
  /** \brief Converged geometry X* (3 by n point matrix).*/
  const Matrix3X &points() const;
  /** \brief True energy at the converged geometry.*/
  Scalar energy() const;
  /** \brief Infinity norm of the energy gradient at the converged geometry.*/
  Scalar gradientResidual() const;
  /** \brief Number of local/global iterations used to reach equilibrium.*/
  unsigned int iterations() const;
  /** \brief Final equilibrium status. Anything other than InverseStatus::Success
      means pullback() must be refused.*/
  InverseStatus status() const;
  /** \brief Number of points.*/
  int nPoints() const;
  /** \brief Number of constraints.*/
  int nConstraints() const;
  /** \brief Original addConstraint id of constraint c.*/
  int constraintId(int c) const;
  /** \brief User weight of constraint c at snapshot time.*/
  Scalar userWeight(int c) const;
  /** \brief Closeness target of constraint c at snapshot time.
      Only valid for closeness constraints.*/
  const Vector3 &target(int c) const;
  /** \brief Implicit adjoint pullback.

      \param G A 3 by n matrix, same shape as the point matrix.
      \return The VJP of L = sum_ij G_ij X*_ij with respect to every closeness
              target coordinate and every user weight, indexed by the original
              constraint numbering. The stored tangent linearization is reused;
              calling this repeatedly with different G does not accumulate.
  */
  PullbackResult pullback(const Matrix3X &G) const;
  /** \brief Apply the factorized equilibrium tangent to a point-space matrix,
      i.e. evaluate the linearized equilibrium residual K vec(dX). The tangent
      stays sparse; no vertex-count-squared dense matrix is allocated.*/
  Matrix3X tangentMultiply(const Matrix3X &dX) const;
 private:
  friend class InverseDesign;
  EquilibriumSnapshot();
  // Non-copyable through the public interface, movable internally.
  EquilibriumSnapshot(const EquilibriumSnapshot &) = delete;
  EquilibriumSnapshot &operator=(const EquilibriumSnapshot &) = delete;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
///////////////////////////////////////////////////////////////////////////////
/** \brief Entry point for static equilibrium solving and implicit differentiation.*/
class SHAPEOP_API InverseDesign {
 public:
  /** \brief Build the inverse design interface on top of an existing Solver.

      The solver must only contain EdgeStrainConstraint, TriangleStrainConstraint
      and ClosenessConstraint instances, all with strictly positive weights,
      and must have been initialized at least once. On a successful equilibrium
      the converged geometry is published back into the solver.
      \throws std::invalid_argument if an unsupported constraint type, a
              non-positive weight or an illegal strain range is present, or the
              solver has not been initialized.
  */
  explicit InverseDesign(Solver &solver);
  ~InverseDesign();
  /** \brief Run local/global iterations until the infinity norm of the full
      energy gradient drops below tolerance, or the budget is exhausted.

      On success the solver's points are replaced by the converged geometry and
      the returned snapshot holds the factorized tangent. On failure the
      solver's points are left untouched, no successful snapshot is modified,
      and the returned report/snapshot explain the failure (their pullback is
      refused). A failed run never publishes trial positions.
      \return An EquilibriumResult bundling the report and the owning snapshot.
  */
  EquilibriumResult equilibrium(unsigned int maxIterations, Scalar tolerance,
                               Scalar branchTolerance);
  /** \brief Update the user weight of a constraint by its original addConstraint id.

      Validates the input, updates the constraint and rebuilds every matrix
      participating in the solve (A, A^T and the system matrix). Returns false
      (and changes nothing) for an unknown id or a non-positive weight.
  */
  bool setConstraintWeight(int id, Scalar weight);
 private:
  InverseDesign(const InverseDesign &) = delete;
  InverseDesign &operator=(const InverseDesign &) = delete;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
///////////////////////////////////////////////////////////////////////////////
} // namespace ShapeOp
///////////////////////////////////////////////////////////////////////////////
// C++ usage example (also exercised by tests/inverse_design.cpp):
//
//   using namespace ShapeOp;
//   // p is a 3 by n matrix of initial vertex positions.
//   Solver solver;
//   solver.setPoints(p);
//   int idEdge = solver.addConstraint(std::make_shared<EdgeStrainConstraint>(
//       std::vector<int>{0, 1}, 1.0, p, 0.9, 1.1));
//   int idTri  = solver.addConstraint(std::make_shared<TriangleStrainConstraint>(
//       std::vector<int>{0, 1, 2}, 1.0, p, 0.9, 1.1));
//   int idPin  = solver.addConstraint(std::make_shared<ClosenessConstraint>(
//       std::vector<int>{0}, 10.0, p));
//   int idSoft = solver.addConstraint(std::make_shared<ClosenessConstraint>(
//       std::vector<int>{3}, 1.0, p));
//   solver.initialize(false);
//
//   InverseDesign design(solver);
//   EquilibriumResult er = design.equilibrium(10000,  // maxIterations
//                                            1e-12,  // gradient tolerance
//                                            1e-8);  // branch tolerance
//   const auto &snap = er.snapshot;
//   if (er.report.status != InverseStatus::Success) return 1;
//
//   // Objective: drive the soft-controlled vertex towards a desired location.
//   Vector3 desired(1.0, 0.0, 0.0);
//   Matrix3X G = Matrix3X::Zero(3, solver.getPoints().cols());
//   G.col(3) = snap->points().col(3) - desired;      // grid of target error
//   PullbackResult pb = snap->pullback(G);
//   if (pb.report.status != InverseStatus::Success) return 1;
//
//   // Gradient-descent step on the closeness target of the soft control point.
//   // dTargets is indexed by the original addConstraint id (here idSoft == 3).
//   const Scalar step = 0.25;
//   auto soft = std::dynamic_pointer_cast<ClosenessConstraint>(
//       solver.getConstraint(idSoft));
//   soft->setPosition(soft->getPosition() - step * pb.dTargets[idSoft]);
//
//   // Re-solve and confirm the mesh target error actually decreased.
//   Scalar before = (snap->points().col(3) - desired).norm();
//   EquilibriumResult er2 = design.equilibrium(10000, 1e-12, 1e-8);
//   Scalar after = (er2.snapshot->points().col(3) - desired).norm();
//   assert(after < before);
///////////////////////////////////////////////////////////////////////////////
#ifdef SHAPEOP_HEADER_ONLY
#include "InverseDesign.cpp"
#endif
///////////////////////////////////////////////////////////////////////////////
#endif // INVERSE_DESIGN_H
///////////////////////////////////////////////////////////////////////////////
