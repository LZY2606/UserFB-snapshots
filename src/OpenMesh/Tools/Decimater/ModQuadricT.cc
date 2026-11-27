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

#include <algorithm>
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

//-----------------------------------------------------------------------------

template<class MeshT>
typename ModQuadricT<MeshT>::Point
ModQuadricT<MeshT>::
optimal_position(const Geometry::QuadricT<double>& _q,
                 const Point& _p0, const Point& _p1)
{
  // The 3x3 block of the quadric is the symmetric matrix A below, the
  // linear part is b.
  const Geometry::QuadricT<double>& q = _q;

  const double A[3][3] = {
    { q.a(), q.b(), q.c() },
    { q.b(), q.e(), q.f() },
    { q.c(), q.f(), q.h() }
  };
  const double b[3] = { -q.d(), -q.g(), -q.i() };

  // Relative singularity threshold: 1e-12 times the largest absolute
  // entry of the matrix.  A zero matrix is singular as well.
  double scale = 0.0;
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      scale = std::max(scale, std::fabs(A[r][c]));
  const double threshold = 1e-12 * scale;

  // Gaussian elimination with partial pivoting on the 3x3 system A x = b.
  double m[3][4] = {
    { A[0][0], A[0][1], A[0][2], b[0] },
    { A[1][0], A[1][1], A[1][2], b[1] },
    { A[2][0], A[2][1], A[2][2], b[2] }
  };
  bool singular = scale == 0.0;
  for (int c = 0; c < 3 && !singular; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 3; ++r)
      if (std::fabs(m[r][c]) > std::fabs(m[pivot][c]))
        pivot = r;
    if (std::fabs(m[pivot][c]) <= threshold) {
      singular = true;
      break;
    }
    if (pivot != c)
      for (int k = c; k < 4; ++k) {
        double t = m[c][k]; m[c][k] = m[pivot][k]; m[pivot][k] = t;
      }
    for (int r = c + 1; r < 3; ++r) {
      double f = m[r][c] / m[c][c];
      for (int k = c; k < 4; ++k)
        m[r][k] -= f * m[c][k];
    }
  }

  Point solution;
  bool finite = !singular;
  if (finite) {
    double x[3];
    for (int r = 2; r >= 0; --r) {
      double rhs = m[r][3];
      for (int k = r + 1; k < 3; ++k)
        rhs -= m[r][k] * x[k];
      x[r] = rhs / m[r][r];
      finite = finite && std::isfinite(x[r]);
    }
    if (finite)
      solution = Point(x[0], x[1], x[2]);
  }

  if (finite)
    return solution;

  // Singular or non-finite: fall back to the lowest error among p1, p0 and
  // their midpoint.  Ties prefer p1, then p0, then the midpoint.
  const Point midpoint = (_p0 + _p1) * 0.5;
  Point best = _p1;
  double best_err = q(_p1);
  double err = q(_p0);
  if (err < best_err) { best_err = err; best = _p0; }
  err = q(midpoint);
  if (err < best_err) { best = midpoint; }
  return best;
}

//-----------------------------------------------------------------------------

template<class MeshT>
typename ModQuadricT<MeshT>::Point
ModQuadricT<MeshT>::
relocate(const CollapseInfo& _ci)
{
  typedef Geometry::QuadricT<double> Q;
  typedef typename Mesh::Point Point;

  const Mesh& mesh = Base::mesh();

  // Only interior edges between two non-boundary, unlocked, non-feature
  // vertices may move the remaining vertex away from its endpoint.
  if (mesh.is_boundary(_ci.v0) || mesh.is_boundary(_ci.v1) ||
      mesh.status(_ci.v0).locked() || mesh.status(_ci.v1).locked() ||
      mesh.status(_ci.v0).feature() || mesh.status(_ci.v1).feature() ||
      mesh.is_boundary(_ci.v0v1) || mesh.is_boundary(_ci.v1v0))
    return _ci.p1;

  Q q = mesh.property(quadrics_, _ci.v0);
  q += mesh.property(quadrics_, _ci.v1);
  return optimal_position(q, _ci.p0, _ci.p1);
}

//=============================================================================
} // END_NS_DECIMATER
} // END_NS_OPENMESH
//=============================================================================
