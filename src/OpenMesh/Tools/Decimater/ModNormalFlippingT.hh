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

/** \file ModNormalFlippingT.hh

 */

//=============================================================================
//
//  CLASS ModNormalFlipping
//
//=============================================================================


#ifndef OPENMESH_DECIMATER_MODNORMALFLIPPING_HH
#define OPENMESH_DECIMATER_MODNORMALFLIPPING_HH


//== INCLUDES =================================================================

#include <OpenMesh/Tools/Decimater/ModBaseT.hh>

//== NAMESPACES ===============================================================

namespace OpenMesh { // BEGIN_NS_OPENMESH
namespace Decimater { // BEGIN_NS_DECIMATER


//== CLASS DEFINITION =========================================================

/** Decimating module to avoid flipping of faces.
 *
 *  This module can be used only as a binary module. The criterion
 *  of allowing/disallowing the collapse is the angular deviation between
 *  the face normal of the original faces and normals of the faces after the
 *  collapse. The collapse will pass the test, if the deviation is below
 *  a given threshold.
 */
template <typename MeshT>
class ModNormalFlippingT : public ModBaseT< MeshT >
{
public:

  DECIMATING_MODULE( ModNormalFlippingT, MeshT, NormalFlipping );

public:

  /// Constructor
  ModNormalFlippingT( MeshT &_mesh) : Base(_mesh, true)
  {
    set_max_normal_deviation( 90.0f );
  }


  ~ModNormalFlippingT()
  { }


public:

  /// This module is binary and understands relocated collapse targets.
  virtual bool relocation_compatible() const { return true; }

  /** Compute collapse priority due to angular deviation of face normals
   *  before and after a collapse.
   *
   *  -# Compute for each adjacent face of \c _ci.v0 the face
   *  normal if the collpase would be executed.
   *
   *  -# Prevent the collapse, if the cosine of the angle between the
   *     original and the new normal is below a given threshold.
   *
   *  If the candidate relocates the remaining vertex (\c _ci.relocate), the
   *  faces on the other side of \c v1 move as well, so they are checked,
   *  too.  The test uses the actual target position \c _ci.p_target and
   *  restores the mesh before returning.
   *
   *  \param _ci The collapse description
   *  \return LEGAL_COLLAPSE or ILLEGAL_COLLAPSE
   *
   *  \see set_max_normal_deviation()
   */
  float collapse_priority(const CollapseInfo& _ci)
  {
    Mesh& mesh = Base::mesh();

    // Simulate the collapse on the actual target position.  In default mode
    // p_target == p1 and this reproduces the classic endpoint test exactly.
    mesh.set_point(_ci.v0, _ci.p_target);
    if (_ci.relocate)
      mesh.set_point(_ci.v1, _ci.p_target);

    bool illegal = false;

    // faces around the removed vertex (fl and fr are deleted)
    illegal = check_ring(_ci, _ci.v0);

    // In relocation mode v1 moves, which changes the faces on its other
    // side.  Those are not covered by the v0 ring, so check them as well.
    if (!illegal && _ci.relocate)
      illegal = check_ring(_ci, _ci.v1);

    // undo simulation changes
    mesh.set_point(_ci.v0, _ci.p0);
    if (_ci.relocate)
      mesh.set_point(_ci.v1, _ci.p1);

    return float( illegal ? Base::ILLEGAL_COLLAPSE : Base::LEGAL_COLLAPSE );
  }

  /// set the percentage of maximum normal deviation
  void set_error_tolerance_factor(double _factor) {
    if (_factor >= 0.0 && _factor <= 1.0) {
      // the smaller the factor, the smaller max_deviation_ gets
      // thus creating a stricter constraint
      // division by error_tolerance_factor_ is for normalization
      double max_normal_deviation = (max_deviation_ * 180.0/M_PI) * _factor / this->error_tolerance_factor_;
      set_max_normal_deviation(max_normal_deviation);
      this->error_tolerance_factor_ = _factor;
    }
  }


public:

  /// get normal deviation
  double max_normal_deviation() const { return max_deviation_ / M_PI * 180.0; }

  /** Set normal deviation
   *
   *  Set the maximum angular deviation of the orignal normal and the new
   *  normal in degrees.
   */
  void set_max_normal_deviation(double _d) {
    max_deviation_ = _d / 180.0 * M_PI;
    min_cos_       = cos(max_deviation_);
  }

private:

  /// Test the faces around _center after the collapse simulation.
  bool check_ring(const CollapseInfo& _ci, typename Mesh::VertexHandle _center)
  {
    typename Mesh::ConstVertexFaceIter vf_it(Base::mesh(), _center);
    for (; vf_it.is_valid(); ++vf_it)
    {
      const typename Mesh::FaceHandle fh = *vf_it;
      if (fh == _ci.fl || fh == _ci.fr)
        continue;

      // A face that degenerates during the collapse cannot be validated
      // by its normal; treat it as a flip.
      const typename Mesh::Normal n1 = Base::mesh().normal(fh);
      const typename Mesh::Normal n2 = Base::mesh().calc_face_normal(fh);
      if (dot(n1, n2) < min_cos_)
        return true;
    }
    return false;
  }

  // hide this method
  void set_binary(bool _b) {}

private:

  // maximum normal deviation
  double max_deviation_, min_cos_;
};


//=============================================================================
} // END_NS_DECIMATER
} // END_NS_OPENMESH
//=============================================================================
#endif // OPENACG_MODNORMALFLIPPING_HH defined
//=============================================================================

