/*===========================================================================*\
 *                                                                           *
 *                               OpenMesh                                    *
 *      Copyright (C) 2001-2013 by Computer Graphics Group, RWTH Aachen      *
 *                           www.openmesh.org                                *
 *                                                                           *
 *---------------------------------------------------------------------------*
 *  This file is part of OpenMesh.                                           *
 *                                                                           *
 *  OpenMesh is free software: you can redistribute it and/or modify         *
 *  it under the terms of the GNU Lesser General Public License as           *
 *  published by the Free Software Foundation, either version 3 of           *
 *  the License, or (at your option) any later version with the              *
 *  following exceptions:                                                    *
 *                                                                           *
 *  If other files instantiate templates or use macros                       *
 *  or inline functions from this file, or you compile this file and         *
 *  link it with other files to produce an executable, this file does        *
 *  not by itself cause the resulting executable to be covered by the        *
 *  GNU Lesser General Public License. This exception does not however       *
 *  invalidate any other reasons why the executable file might be            *
 *  covered by the GNU Lesser General Public License.                        *
 *                                                                           *
 *  OpenMesh is distributed in the hope that it will be useful,              *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU Lesser General Public License for more details.                      *
 *                                                                           *
 *  You should have received a copy of the GNU LesserGeneral Public          *
 *  License along with OpenMesh.  If not,                                    *
 *  see <http://www.gnu.org/licenses/>.                                      *
 *                                                                           *
\*===========================================================================*/

/*===========================================================================*\
 *                                                                           *
 *   $Revision$                                                         *
 *   $Date$                   *
 *                                                                           *
\*===========================================================================*/

/** \file ModQuadricT.cc
    Bodies of template member function.
 */

//=============================================================================
//
//  CLASS ModQuadric - IMPLEMENTATION
//
//=============================================================================

#define OPENMESH_DECIMATER_MODQUADRIC_CC

//== INCLUDES =================================================================

#include <OpenMesh/Tools/Decimater/ModQuadricT.hh>

#include <cmath>


//== NAMESPACE ===============================================================

namespace OpenMesh { // BEGIN_NS_OPENMESH
namespace Decimater { // BEGIN_NS_DECIMATER


//== IMPLEMENTATION ==========================================================


template<class DecimaterType>
void
ModQuadricT<DecimaterType>::
initialize()
{
  using Geometry::Quadricd;
  // alloc quadrics
  if (!quadrics_.is_valid())
    Base::mesh().add_property( quadrics_ );

  // clear quadrics
  typename Mesh::VertexIter  v_it  = Base::mesh().vertices_begin(),
                             v_end = Base::mesh().vertices_end();

  for (; v_it != v_end; ++v_it)
    Base::mesh().property(quadrics_, *v_it).clear();

  // calc (normal weighted) quadric
  typename Mesh::FaceIter          f_it  = Base::mesh().faces_begin(),
                                   f_end = Base::mesh().faces_end();

  typename Mesh::FaceVertexIter    fv_it;
  typename Mesh::VertexHandle      vh0, vh1, vh2;
  typedef Vec3d                    Vec3;

  for (; f_it != f_end; ++f_it)
  {
    // Deleted faces may remain in the mesh between collapses; re-running
    // initialize() (required after changing relocation mode) must not
    // trip over their invalid connectivity.
    if (Base::mesh().has_face_status() &&
        Base::mesh().status(*f_it).deleted())
      continue;
    fv_it = Base::mesh().fv_iter(*f_it);
    vh0 = *fv_it;  ++fv_it;
    vh1 = *fv_it;  ++fv_it;
    vh2 = *fv_it;

    Vec3 v0, v1, v2;
    {
      using namespace OpenMesh;

      v0 = vector_cast<Vec3>(Base::mesh().point(vh0));
      v1 = vector_cast<Vec3>(Base::mesh().point(vh1));
      v2 = vector_cast<Vec3>(Base::mesh().point(vh2));
    }

    Vec3 n = (v1-v0) % (v2-v0);
    double area = n.norm();
    if (area > FLT_MIN)
    {
      n /= area;
      area *= 0.5;
    }

    const double a = n[0];
    const double b = n[1];
    const double c = n[2];
    const double d = -(vector_cast<Vec3>(Base::mesh().point(vh0))|n);

    Quadricd q(a, b, c, d);
    q *= area;

    Base::mesh().property(quadrics_, vh0) += q;
    Base::mesh().property(quadrics_, vh1) += q;
    Base::mesh().property(quadrics_, vh2) += q;
  }
}

//-----------------------------------------------------------------------------

/** Solve the 3x3 system A x = b with a symmetric matrix given by the upper
 *  triangle of the quadric.  The system is solved by Gaussian elimination
 *  with partial pivoting.  A pivot whose magnitude does not exceed the
 *  relative threshold (1e-12 times the largest absolute matrix element) is
 *  treated as singular.  Returns false on singularity or a non-finite
 *  solution.
 */
static bool solve_quadric_minimizer(double a, double b, double c,
                                    double e, double f, double h,
                                    double bx, double by, double bz,
                                    double out[3])
{
  double m[3][4] = { { a, b, c, bx },
                     { b, e, f, by },
                     { c, f, h, bz } };

  double max_abs = 0.0;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
    {
      const double v = std::fabs(m[i][j]);
      if (v > max_abs) max_abs = v;
    }

  // Relative singularity threshold.  A zero matrix yields max_abs == 0,
  // hence threshold 0, and is rejected by the pivot test below as well.
  const double threshold = max_abs * 1e-12;

  for (int col = 0; col < 3; ++col)
  {
    // partial pivoting
    int pivot = col;
    for (int row = col + 1; row < 3; ++row)
      if (std::fabs(m[row][col]) > std::fabs(m[pivot][col]))
        pivot = row;

    if (!(std::fabs(m[pivot][col]) > threshold))
      return false;

    if (pivot != col)
      for (int j = col; j < 4; ++j)
      {
        const double t = m[col][j];
        m[col][j] = m[pivot][j];
        m[pivot][j] = t;
      }

    for (int row = col + 1; row < 3; ++row)
    {
      const double factor = m[row][col] / m[col][col];
      for (int j = col; j < 4; ++j)
        m[row][j] -= factor * m[col][j];
    }
  }

  // back substitution
  for (int row = 2; row >= 0; --row)
  {
    double sum = m[row][3];
    for (int j = row + 1; j < 3; ++j)
      sum -= m[row][j] * out[j];
    out[row] = sum / m[row][row];
  }

  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(out[i]))
      return false;

  return true;
}

//-----------------------------------------------------------------------------

template<class MeshT>
bool ModQuadricT<MeshT>::relocation_target(const CollapseInfo& _ci,
                                           typename Mesh::Point& _p)
{
  using namespace OpenMesh;

  typedef Geometry::QuadricT<double> Q;
  typedef typename Mesh::Point Point;
  typedef typename Mesh::Scalar Scalar;

  Mesh& mesh = Base::mesh();

  // Free placement is only possible for interior edges whose endpoints are
  // neither on the boundary, nor locked, nor feature vertices.  Everything
  // else keeps the ordinary collapse target p1.
  if (_ci.fl.is_valid() == false || _ci.fr.is_valid() == false ||
      mesh.is_boundary(_ci.v0) || mesh.is_boundary(_ci.v1) ||
      mesh.status(_ci.v0).locked() || mesh.status(_ci.v1).locked() ||
      mesh.status(_ci.v0).feature() || mesh.status(_ci.v1).feature())
    return false;

  Q q = mesh.property(quadrics_, _ci.v0);
  q += mesh.property(quadrics_, _ci.v1);

  // Gradient of q at the minimum is zero: A x = -d, where A is the upper
  // 3x3 block and d the first three entries of the fourth column.
  double sol[3];
  const bool solved = solve_quadric_minimizer(
      q.a(), q.b(), q.c(),
      q.e(), q.f(), q.h(),
      -q.d(), -q.g(), -q.i(), sol);

  Point candidate;
  if (solved)
  {
    candidate = Point(Scalar(sol[0]), Scalar(sol[1]), Scalar(sol[2]));

    // A non-finite minimizer cannot be used either.
    if (std::isfinite(double(candidate[0])) &&
        std::isfinite(double(candidate[1])) &&
        std::isfinite(double(candidate[2])))
    {
      _p = candidate;
      return true;
    }
  }

  // Fallback: choose the position with the lowest quadric error among p1,
  // p0 and the midpoint.  Ties keep the earlier candidate, hence the order.
  Point midpoint = (_ci.p0 + _ci.p1) * Scalar(0.5);

  const Point* candidates[3] = { &_ci.p1, &_ci.p0, &midpoint };
  double best_err = q(*candidates[0]);
  const Point* best = candidates[0];
  for (int i = 1; i < 3; ++i)
  {
    const double err = q(*candidates[i]);
    if (err < best_err)
    {
      best_err = err;
      best = candidates[i];
    }
  }

  _p = *best;

  // If the fallback picks p1, the vertex keeps its position and the
  // candidate is an ordinary endpoint collapse.
  return !(*best == _ci.p1);
}

//-----------------------------------------------------------------------------

template<class MeshT>
void ModQuadricT<MeshT>::set_error_tolerance_factor(double _factor) {
  if (this->is_binary()) {
    if (_factor >= 0.0 && _factor <= 1.0) {
      // the smaller the factor, the smaller max_err_ gets
      // thus creating a stricter constraint
      // division by error_tolerance_factor_ is for normalization
      double max_err = max_err_ * _factor / this->error_tolerance_factor_;
      set_max_err(max_err);
      this->error_tolerance_factor_ = _factor;

      initialize();
    }
  }
}

//=============================================================================
} // END_NS_DECIMATER
} // END_NS_OPENMESH
//=============================================================================
