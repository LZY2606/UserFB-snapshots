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


//=============================================================================
//
//  CLASS ModQuadricT
//
//=============================================================================

#ifndef OSG_MODQUADRIC_HH
#define OSG_MODQUADRIC_HH


//== INCLUDES =================================================================

#include <float.h>
#include <cmath>
#include <OpenMesh/Tools/Decimater/ModBaseT.hh>
#include <OpenMesh/Core/Utils/Property.hh>
#include <OpenMesh/Core/Utils/vector_cast.hh>
#include <OpenMesh/Core/Geometry/QuadricT.hh>


//== NAMESPACE ================================================================

namespace OpenMesh  {
namespace Decimater {


//== CLASS DEFINITION =========================================================


/** \brief Mesh decimation module computing collapse priority based on error quadrics.
 *
 *  This module can be used as a binary and non-binary module.
 */
template <class MeshT>
class ModQuadricT : public ModBaseT<MeshT>
{
public:

  // Defines the types Self, Handle, Base, Mesh, and CollapseInfo
  // and the memberfunction name()
  DECIMATING_MODULE( ModQuadricT, MeshT, Quadric );

  typedef typename Mesh::Point Point;
  typedef typename Mesh::VertexHandle VertexHandle;

public:

  /** Constructor
   *  \internal
   */
  ModQuadricT( MeshT &_mesh )
    : Base(_mesh, false)
  {
    unset_max_err();
    Base::mesh().add_property( quadrics_ );
  }


  /// Destructor
  virtual ~ModQuadricT()
  {
    Base::mesh().remove_property(quadrics_);
  }


public: // inherited

  /// Initalize the module and prepare the mesh for decimation.
  virtual void initialize(void);

  /** Compute collapse priority based on error quadrics.
   *
   *  The error is evaluated at the target position carried by _ci.  In
   *  the classic mode that is the remaining endpoint p1; with relocation
   *  enabled it may be an unconstrained minimizer of the merged quadric.
   *
   *  \see ModBaseT::collapse_priority() for return values
   *  \see set_max_err()
   */
  virtual float collapse_priority(const CollapseInfo& _ci)
  {
    typedef Geometry::QuadricT<double> Q;

    Q q = Base::mesh().property(quadrics_, _ci.v0);
    q += Base::mesh().property(quadrics_, _ci.v1);

    double err = q(_ci.p_target);

    return float( (err < max_err_) ? err : float( Base::ILLEGAL_COLLAPSE ) );
  }

  /// Quadric-based priority modules can relocate the remaining vertex.
  virtual bool supports_relocation() const { return true; }

  /** Target position of the remaining vertex for a candidate collapse.
   *
   *  When relocation is disabled this is the classic endpoint p1.  When
   *  it is enabled, an interior edge whose two endpoints are not on the
   *  boundary, are unlocked and carry no feature flag is collapsed to the
   *  unconstrained minimizer of the merged quadric.  If that 3x3 system is
   *  numerically singular (relative to the largest matrix entry) or the
   *  solution is not finite, the lowest-error of p1, p0 and their midpoint
   *  is used, with p1 taking precedence on ties.  All other candidates are
   *  left at p1.
   */
  virtual Point relocate(const CollapseInfo& _ci);

  /** Compute the unconstrained minimizer of a merged quadric.
   *
   *  Returns the point minimizing \c _q.  If the 3x3 system is
   *  numerically singular (its largest absolute entry is zero, or a pivot
   *  falls below 1e-12 times that entry) or the solution is not finite,
   *  the point with the lowest error among p1, p0 and their midpoint is
   *  returned instead, preferring p1 on ties, then p0, then the midpoint.
   *
   *  This is exposed as a static helper so callers and tests can reason
   *  about the placement independent of a particular mesh.
   */
  static Point optimal_position(const Geometry::QuadricT<double>& _q,
                                const Point& _p0, const Point& _p1);

  /// Read-only access to the accumulated quadric of a vertex.
  const Geometry::QuadricT<double>& quadric(VertexHandle _vh) const
  {
    return const_cast<ModQuadricT*>(this)->Base::mesh().property(quadrics_, _vh);
  }


  /// Post-process halfedge collapse (accumulate quadrics)
  virtual void postprocess_collapse(const CollapseInfo& _ci)
  {
    Base::mesh().property(quadrics_, _ci.v1) +=
      Base::mesh().property(quadrics_, _ci.v0);
  }

  /// set the percentage of maximum quadric error
  void set_error_tolerance_factor(double _factor);



public: // specific methods

  /** Set maximum quadric error constraint and enable binary mode.
   *  \param _err    Maximum error allowed
   *  \param _binary Let the module work in non-binary mode in spite of the
   *                 enabled constraint.
   *  \see unset_max_err()
   */
  void set_max_err(double _err, bool _binary=true)
  {
    max_err_ = _err;
    Base::set_binary(_binary);
  }

  /// Unset maximum quadric error constraint and restore non-binary mode.
  /// \see set_max_err()
  void unset_max_err(void)
  {
    max_err_ = DBL_MAX;
    Base::set_binary(false);
  }

  /// Return value of max. allowed error.
  double max_err() const { return max_err_; }


private:

  // maximum quadric error
  double max_err_;

  // this vertex property stores a quadric for each vertex
  VPropHandleT< Geometry::QuadricT<double> >  quadrics_;
};

//=============================================================================
} // END_NS_DECIMATER
} // END_NS_OPENMESH
//=============================================================================
#if defined(OM_INCLUDE_TEMPLATES) && !defined(OPENMESH_DECIMATER_MODQUADRIC_CC)
#define OSG_MODQUADRIC_TEMPLATES
#include "ModQuadricT.cc"
#endif
//=============================================================================
#endif // OSG_MODQUADRIC_HH defined
//=============================================================================

