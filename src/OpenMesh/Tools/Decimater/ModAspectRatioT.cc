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
 *   $Revision$                                                        *
 *   $Date$                   *
 *                                                                           *
 \*===========================================================================*/

/** \file ModAspectRatioT.cc
 */

//=============================================================================
//
//  CLASS ModAspectRatioT - IMPLEMENTATION
//
//=============================================================================
#define OPENMESH_DECIMATER_MODASPECTRATIOT_C

//== INCLUDES =================================================================

#include "ModAspectRatioT.hh"

//== NAMESPACES ===============================================================

namespace OpenMesh {
namespace Decimater {

//== IMPLEMENTATION ==========================================================

template<class MeshT>
typename ModAspectRatioT<MeshT>::Scalar ModAspectRatioT<MeshT>::aspectRatio(
    const Point& _v0, const Point& _v1, const Point& _v2) {
  Point d0 = _v0 - _v1;
  Point d1 = _v1 - _v2;

  // finds the max squared edge length
  Scalar l2, maxl2 = d0.sqrnorm();
  if ((l2 = d1.sqrnorm()) > maxl2)
    maxl2 = l2;
  // keep searching for the max squared edge length
  d1 = _v2 - _v0;
  if ((l2 = d1.sqrnorm()) > maxl2)
    maxl2 = l2;

  // squared area of the parallelogram spanned by d0 and d1
  Scalar a2 = (d0 % d1).sqrnorm();

  // the area of the triangle would be
  // sqrt(a2)/2 or length * height / 2
  // aspect ratio = length / height
  //              = length * length / (2*area)
  //              = length * length / sqrt(a2)

  // returns the length of the longest edge
  //         divided by its corresponding height
  return sqrt((maxl2 * maxl2) / a2);
}

//-----------------------------------------------------------------------------

template<class MeshT>
void ModAspectRatioT<MeshT>::initialize() {
  typename Mesh::FaceIter f_it, f_end(mesh_.faces_end());
  typename Mesh::FVIter fv_it;

  for (f_it = mesh_.faces_begin(); f_it != f_end; ++f_it) {
    if (mesh_.has_face_status() && mesh_.status(*f_it).deleted())
      continue;
    fv_it = mesh_.fv_iter(*f_it);
    typename Mesh::Point& p0 = mesh_.point(*fv_it);
    typename Mesh::Point& p1 = mesh_.point(*(++fv_it));
    typename Mesh::Point& p2 = mesh_.point(*(++fv_it));

    mesh_.property(aspect_, *f_it) = 1.0 / aspectRatio(p0, p1, p2);
  }
}

//-----------------------------------------------------------------------------

template<class MeshT>
void ModAspectRatioT<MeshT>::preprocess_collapse(const CollapseInfo& _ci) {
  typename Mesh::FaceHandle fh;
  typename Mesh::FVIter fv_it;

  for (typename Mesh::VFIter vf_it = mesh_.vf_iter(_ci.v0); vf_it.is_valid(); ++vf_it) {
    fh = *vf_it;
    if (fh != _ci.fl && fh != _ci.fr) {
      fv_it = mesh_.fv_iter(fh);
      typename Mesh::Point& p0 = mesh_.point(*fv_it);
      typename Mesh::Point& p1 = mesh_.point(*(++fv_it));
      typename Mesh::Point& p2 = mesh_.point(*(++fv_it));

      mesh_.property(aspect_, fh) = 1.0 / aspectRatio(p0, p1, p2);
    }
  }
}

//-----------------------------------------------------------------------------

template<class MeshT>
float ModAspectRatioT<MeshT>::collapse_priority(const CollapseInfo& _ci) {
  if (_ci.relocate) {
    // The remaining vertex moves, so the faces on both sides of the edge
    // change.  Apply the binary rule to the union of the two one-rings,
    // comparing the worst quality before and after the collapse against
    // each other and against the configured minimum.  Both values are
    // computed from the current geometry, so the test never relies on a
    // cache that another module or candidate could have left behind.
    Scalar r0, r1, r0_min(1.0), r1_min(1.0);
    const typename Mesh::Point& target = _ci.p_target;

    const typename Mesh::VertexHandle centers[2] = { _ci.v0, _ci.v1 };
    for (int c = 0; c < 2; ++c) {
      for (typename Mesh::ConstVertexFaceIter vf_it(mesh_, centers[c]);
           vf_it.is_valid(); ++vf_it) {
        const typename Mesh::FaceHandle fh = *vf_it;
        if (fh == _ci.fl || fh == _ci.fr)
          continue;

        typename Mesh::ConstFaceVertexIter fv_it = mesh_.cfv_iter(fh);
        const typename Mesh::VertexHandle a = *fv_it; ++fv_it;
        const typename Mesh::VertexHandle b = *fv_it; ++fv_it;
        const typename Mesh::VertexHandle d = *fv_it;

        const typename Mesh::Point& pa = mesh_.point(a);
        const typename Mesh::Point& pb = mesh_.point(b);
        const typename Mesh::Point& pd = mesh_.point(d);

        if ((r0 = 1.0 / aspectRatio(pa, pb, pd)) < r0_min)
          r0_min = r0;

        const typename Mesh::Point& qa = (a == _ci.v0 || a == _ci.v1) ? target : pa;
        const typename Mesh::Point& qb = (b == _ci.v0 || b == _ci.v1) ? target : pb;
        const typename Mesh::Point& qd = (d == _ci.v0 || d == _ci.v1) ? target : pd;

        if ((r1 = 1.0 / aspectRatio(qa, qb, qd)) < r1_min)
          r1_min = r1;
      }
    }

    return ((r1_min > r0_min) || (r1_min > min_aspect_))
        ? Base::LEGAL_COLLAPSE : Base::ILLEGAL_COLLAPSE;
  }

  typename Mesh::VertexHandle v2, v3;
  typename Mesh::FaceHandle fh;
  const typename Mesh::Point *p1(&_ci.p_target), *p2, *p3;
  typename Mesh::Scalar r0, r1, r0_min(1.0), r1_min(1.0);
  typename Mesh::ConstVertexOHalfedgeIter voh_it(mesh_, _ci.v0);

  v3 = mesh_.to_vertex_handle(*voh_it);
  p3 = &mesh_.point(v3);

  while (voh_it.is_valid()) {
    v2 = v3;
    p2 = p3;

    ++voh_it;
    v3 = mesh_.to_vertex_handle(*voh_it);
    p3 = &mesh_.point(v3);

    fh = mesh_.face_handle(*voh_it);

    // if not boundary
    if (fh.is_valid()) {
      // aspect before
      if ((r0 = mesh_.property(aspect_, fh)) < r0_min)
        r0_min = r0;

      // aspect after
      if (!(v2 == _ci.v1 || v3 == _ci.v1))
        if ((r1 = 1.0 / aspectRatio(*p1, *p2, *p3)) < r1_min)
          r1_min = r1;
    }
  }

  if (Base::is_binary()) {
    return
        ((r1_min > r0_min) || (r1_min > min_aspect_)) ? Base::LEGAL_COLLAPSE :
            Base::ILLEGAL_COLLAPSE;

  } else {
    if (r1_min > r0_min)
      return 1.0 - r1_min;
    else
      return
          (r1_min > min_aspect_) ? 2.0 - r1_min : float(Base::ILLEGAL_COLLAPSE);
  }
}

//-----------------------------------------------------------------------------

template<class MeshT>
void ModAspectRatioT<MeshT>::postprocess_collapse(const CollapseInfo& _ci) {
  if (!_ci.relocate)
    return;

  // Moving v1 changes the geometry of all faces around it; the cached
  // aspect ratios of those faces have to follow the new geometry.
  for (typename Mesh::VFIter vf_it = mesh_.vf_iter(_ci.v1); vf_it.is_valid(); ++vf_it) {
    const typename Mesh::FaceHandle fh = *vf_it;
    if (mesh_.status(fh).deleted())
      continue;

    typename Mesh::FVIter fv_it = mesh_.fv_iter(fh);
    const typename Mesh::Point& p0 = mesh_.point(*fv_it);
    const typename Mesh::Point& p1 = mesh_.point(*(++fv_it));
    const typename Mesh::Point& p2 = mesh_.point(*(++fv_it));

    mesh_.property(aspect_, fh) = 1.0 / aspectRatio(p0, p1, p2);
  }
}

//-----------------------------------------------------------------------------

template<class MeshT>
void ModAspectRatioT<MeshT>::set_error_tolerance_factor(double _factor) {
  if (_factor >= 0.0 && _factor <= 1.0) {
    // the smaller the factor, the larger min_aspect_ gets
    // thus creating a stricter constraint
    // division by (2.0 - error_tolerance_factor_) is for normalization
    double min_aspect = min_aspect_ * (2.0 - _factor) / (2.0 - this->error_tolerance_factor_);
    set_aspect_ratio(1.0/min_aspect);
    this->error_tolerance_factor_ = _factor;
  }
}

//=============================================================================
}
}
//=============================================================================
