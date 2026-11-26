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
#include <vector>

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

    /**
     * \brief Geometric 2-to-3 flip with a signed-volume quality gate.
     *
     * Behaves like TetrahedralMeshTopologyKernel::flip23(), but before any
     * write to the mesh it additionally verifies the geometry:
     *   - every removed cell must have a strictly positive signed volume
     *     under the orientation stored in the mesh;
     *   - every candidate replacement tet must have a signed volume
     *     \f$V\f$ with \f$V > \epsilon \, L^3\f$, where \f$L\f$ is the
     *     longest distance between the five cavity vertices. The test uses
     *     signed (not absolute) volumes, so an inverted candidate is
     *     rejected even if its absolute volume is large.
     *
     * If a check fails the function returns false and the mesh, the
     * properties and the deletion flags are left untouched. Requires
     * deferred deletion and full bottom-up incidences, just like the
     * topological version.
     *
     * Example:
     * \code
     * TetrahedralGeometryKernel<Vec3d> mesh;
     * // ... build two tets sharing the internal face fh ...
     * std::vector<CellHandle> created;
     * if (mesh.flip23(fh, created)) {
     *     // created contains the three new tets, sorted by handle
     * }
     * \endcode
     *
     * \param _fh Internal face shared by exactly two tetrahedra.
     * \param _created On success, the three new cell handles, sorted ascending.
     * \param _threshold Relative volume threshold, defaults to 1e-12.
     * \return true on success, false if the flip was rejected.
     */
    bool flip23(FaceHandle _fh, std::vector<CellHandle>& _created,
                double _threshold = 1e-12)
    {
        if (!KernelT::deferred_deletion_enabled() ||
            !KernelT::has_full_bottom_up_incidences()) {
            return false;
        }

        typename KernelT::FlipCavity cavity;
        if (!KernelT::analyze_flip23_cavity(_fh, cavity)) return false;
        if (!flip_cavity_geometry_valid(cavity, _threshold)) return false;

        return KernelT::apply_flip(cavity, _created);
    }

    /**
     * \brief Geometric 3-to-2 flip with a signed-volume quality gate.
     *
     * Geometric counterpart of TetrahedralMeshTopologyKernel::flip32() and
     * exact inverse of the geometric flip23(). Before writing, every
     * removed cell must have a positive signed volume and every candidate
     * replacement tet must satisfy \f$V > \epsilon \, L^3\f$, where
     * \f$L\f$ is the longest distance between the five cavity vertices.
     * Signed volumes are used, so inverted candidates cannot pass the
     * gate. On rejection the mesh is left untouched.
     *
     * Example:
     * \code
     * TetrahedralGeometryKernel<Vec3d> mesh;
     * // ... three tets around the internal edge eh ...
     * std::vector<CellHandle> created;
     * if (mesh.flip32(eh, created)) {
     *     // created contains the two new tets, sorted by handle
     * }
     * \endcode
     *
     * \param _eh Internal edge incident to exactly three tetrahedra.
     * \param _created On success, the two new cell handles, sorted ascending.
     * \param _threshold Relative volume threshold, defaults to 1e-12.
     * \return true on success, false if the flip was rejected.
     */
    bool flip32(EdgeHandle _eh, std::vector<CellHandle>& _created,
                double _threshold = 1e-12)
    {
        if (!KernelT::deferred_deletion_enabled() ||
            !KernelT::has_full_bottom_up_incidences()) {
            return false;
        }

        typename KernelT::FlipCavity cavity;
        if (!KernelT::analyze_flip32_cavity(_eh, cavity)) return false;
        if (!flip_cavity_geometry_valid(cavity, _threshold)) return false;

        return KernelT::apply_flip(cavity, _created);
    }

protected:

    /// Signed volume of an oriented vertex quadruple (scalar triple
    /// product divided by six), matching signed_volume()'s convention.
    typename VecT::value_type tet_volume(
            VertexHandle _v0, VertexHandle _v1,
            VertexHandle _v2, VertexHandle _v3) const
    {
        const PointT& p0 = ParentT::vertex(_v0);
        const PointT e1 = ParentT::vertex(_v1) - p0;
        const PointT e2 = ParentT::vertex(_v2) - p0;
        const PointT e3 = ParentT::vertex(_v3) - p0;
        return e1.dot(e2.cross(e3)) / 6.;
    }

    /// Geometric validity gate shared by both geometric flips.
    bool flip_cavity_geometry_valid(
            const typename KernelT::FlipCavity& _cavity,
            double _threshold) const
    {
        using Scalar = typename VecT::value_type;

        // The removed cells must be positively oriented.
        for (const CellHandle ch : _cavity.old_cells) {
            const auto vhs = KernelT::get_cell_vertices(ch);
            if (tet_volume(vhs[0], vhs[1], vhs[2], vhs[3]) <= Scalar(0)) {
                return false;
            }
        }

        // Longest distance between any two of the five cavity vertices;
        // this makes the threshold independent of the mesh's length scale.
        Scalar longest = Scalar(0);
        for (int i = 0; i < 5; ++i) {
            for (int j = i + 1; j < 5; ++j) {
                const Scalar dist =
                    (ParentT::vertex(_cavity.vertices[j]) -
                     ParentT::vertex(_cavity.vertices[i])).norm();
                if (dist > longest) longest = dist;
            }
        }
        const Scalar limit =
            static_cast<Scalar>(_threshold) * longest * longest * longest;

        // Every candidate must be positively oriented and large enough.
        // Comparing the signed (not absolute) volume rejects tets that
        // would fold outside the cavity.
        for (const auto& vhs : _cavity.new_cells) {
            const Scalar vol =
                tet_volume(vhs[0], vhs[1], vhs[2], vhs[3]);
            if (!(vol > limit)) return false;
        }
        return true;
    }

private:

};

} // Namespace OpenVolumeMesh

