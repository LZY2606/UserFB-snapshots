#pragma once
/*===========================================================================*\
 *                                                                           *
 *                            OpenVolumeMesh                                 *
 *        Copyright (C) 2011 by Computer Graphics Group, RWTH Aachen         *
 *                        www.openvolumemesh.org                             *
 *                                                                           *
 *---------------------------------------------------------------------------*
 *  This file is part of OpenVolumeMesh.                                     *
 *                                                                           *
 *  OpenVolumeMesh is free software: you can redistribute it and/or modify   *
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
 *  OpenVolumeMesh is distributed in the hope that it will be useful,        *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
 *  GNU Lesser General Public License for more details.                      *
 *                                                                           *
 *  You should have received a copy of the GNU LesserGeneral Public          *
 *  License along with OpenVolumeMesh.  If not,                              *
 *  see <http://www.gnu.org/licenses/>.                                      *
 *                                                                           *
\*===========================================================================*/


#include <cassert>
#include <iostream>

#include <OpenVolumeMesh/Geometry/VectorT.hh>
#include <OpenVolumeMesh/Core/GeometryKernel.hh>
#include <OpenVolumeMesh/Mesh/TetrahedralMeshTopologyKernel.hh>

namespace OpenVolumeMesh {

template <class VecT, class TopologyKernelT = TetrahedralMeshTopologyKernel>
class TetrahedralGeometryKernel : public GeometryKernel<VecT, TopologyKernelT> {
public:

    typedef VecT PointT;
    typedef TopologyKernelT KernelT;
    typedef GeometryKernel<VecT, TopologyKernelT> ParentT;

    /// Constructor
    TetrahedralGeometryKernel() {}

    /// Destructor
    ~TetrahedralGeometryKernel() {}

    VertexHandle split_edge(HalfEdgeHandle heh, double alpha = 0.5)
    {
        OpenVolumeMeshEdge e = TopologyKernelT::halfedge(heh);
        PointT newPos = alpha*ParentT::vertex(e.from_vertex()) + (1.0-alpha)*ParentT::vertex(e.to_vertex());
        VertexHandle splitVertex = ParentT::add_vertex(newPos);
        TopologyKernelT::split_edge(heh, splitVertex);
        return splitVertex;
    }

    VertexHandle split_edge(EdgeHandle eh)
    {
        return split_edge(TopologyKernelT::halfedge_handle(eh,0));
    }

    VertexHandle split_face(FaceHandle fh, PointT pos)
    {
        VertexHandle splitVertex = ParentT::add_vertex(pos);
        TopologyKernelT::split_face(fh, splitVertex);
        return splitVertex;
    }

    VertexHandle split_face(FaceHandle fh)
    {
        VertexHandle splitVertex = ParentT::add_vertex(ParentT::barycenter(fh));
        TopologyKernelT::split_face(fh, splitVertex);
        return splitVertex;
    }

    typename VecT::value_type signed_volume(CellHandle ch) {

        auto vertices = this->get_cell_vertices(ch);

        PointT e1 = this->vertex(vertices[1]) - this->vertex(vertices[0]);
        PointT e2 = this->vertex(vertices[2]) - this->vertex(vertices[0]);
        PointT e3 = this->vertex(vertices[3]) - this->vertex(vertices[0]);

        // volume using scalar triple product
        return e1.dot(e2.cross(e3)) / 6.;
    }

    /// \brief Geometry-checked in-place 2-to-3 flip (specialized for
    /// Vec3d-style point types).
    ///
    /// Runs the same combinatorial validation as
    /// TetrahedralMeshTopologyKernel::flip23 and, additionally, checks
    /// the signed volumes before any entity is written:
    ///   - every old cavity tetrahedron must be positively oriented
    ///     (signed volume > 0);
    ///   - every candidate tetrahedron must have a signed volume larger
    ///     than \p _rel_volume_threshold times the cube of the longest
    ///     edge length of the cavity. The threshold is relative, so the
    ///     check is scale independent.
    /// A candidate that would invert the cavity (negative or zero
    /// signed volume) is therefore rejected instead of being accepted
    /// on its absolute volume. On failure nothing is changed.
    ///
    /// Example:
    /// \code
    /// std::vector<CellHandle> created;
    /// if (!mesh.flip23(shared_face, created)) {
    ///     // boundary face, bad topology or degenerate geometry
    /// }
    /// \endcode
    bool flip23(FaceHandle _fh, std::vector<CellHandle>& _created,
                double _rel_volume_threshold = 1e-12)
    {
        typename TopologyKernelT::FlipPlan plan;
        if(!TopologyKernelT::plan_flip23(_fh, plan))
            return false;
        if(!flip_geometry_acceptable(plan, _rel_volume_threshold))
            return false;
        return TopologyKernelT::execute_flip(plan, _created);
    }

    /// \brief Geometry-checked in-place 3-to-2 flip.
    ///
    /// See flip23() for the volume checks. This is the geometry-aware
    /// inverse operation that restores a positively oriented two-tet
    /// cavity around a new interior face.
    bool flip32(EdgeHandle _eh, std::vector<CellHandle>& _created,
                double _rel_volume_threshold = 1e-12)
    {
        typename TopologyKernelT::FlipPlan plan;
        if(!TopologyKernelT::plan_flip32(_eh, plan))
            return false;
        if(!flip_geometry_acceptable(plan, _rel_volume_threshold))
            return false;
        return TopologyKernelT::execute_flip(plan, _created);
    }

protected:

    using ScalarT = typename VecT::value_type;

    ScalarT candidate_signed_volume(const std::array<VertexHandle, 4>& _tet) const
    {
        const PointT& p0 = ParentT::vertex(_tet[0]);
        const PointT e1 = ParentT::vertex(_tet[1]) - p0;
        const PointT e2 = ParentT::vertex(_tet[2]) - p0;
        const PointT e3 = ParentT::vertex(_tet[3]) - p0;
        return e1.dot(e2.cross(e3)) / ScalarT(6.);
    }

    bool flip_geometry_acceptable(const typename TopologyKernelT::FlipPlan& _plan,
                                  double _rel_volume_threshold)
    {
        // All old cavity tets must be positively oriented.
        for(CellHandle ch : _plan.old_cells) {
            if(!(this->signed_volume(ch) > ScalarT(0.)))
                return false;
        }

        // Longest edge length of the cavity, measured over all pairs
        // of the five cavity vertices, so the threshold scales with
        // the mesh (works for non-unit scales).
        ScalarT longest = ScalarT(0.);
        for(size_t i = 0; i < _plan.vertices.size(); ++i) {
            for(size_t j = i + 1; j < _plan.vertices.size(); ++j) {
                const ScalarT len =
                    (ParentT::vertex(_plan.vertices[j]) -
                     ParentT::vertex(_plan.vertices[i])).norm();
                if(len > longest)
                    longest = len;
            }
        }
        if(!(longest > ScalarT(0.)))
            return false;

        const ScalarT limit =
            static_cast<ScalarT>(_rel_volume_threshold) *
            longest * longest * longest;

        // Every candidate must keep the orientation induced by the
        // preserved shell halffaces (strictly positive signed volume).
        for(const auto& tet : _plan.new_tets) {
            if(!(candidate_signed_volume(tet) > limit))
                return false;
        }
        return true;
    }

public:


};

} // Namespace OpenVolumeMesh

