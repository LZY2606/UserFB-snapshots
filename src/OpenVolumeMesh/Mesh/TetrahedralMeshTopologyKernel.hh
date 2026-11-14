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


#ifndef NDEBUG
#include <iostream>
#endif
#include <set>

#include <OpenVolumeMesh/Core/TopologyKernel.hh>
#include <OpenVolumeMesh/Mesh/TetrahedralMeshIterators.hh>
#include <OpenVolumeMesh/Config/Export.hh>

namespace OpenVolumeMesh {

/**
 * \class TetrahedralMeshTopologyKernel
 *
 * \brief A data structure based on PolyhedralMesh with specializations for tetrahedra.
 *
 */

class OVM_EXPORT TetrahedralMeshTopologyKernel : public TopologyKernel {
public:

    TetrahedralMeshTopologyKernel() = default;
    ~TetrahedralMeshTopologyKernel() override = default;

    FaceHandle add_face(std::vector<HalfEdgeHandle> _halfedges, bool _topologyCheck = false) override;

    FaceHandle add_face(const std::vector<VertexHandle>& _vertices) override;

    CellHandle add_cell(std::vector<HalfFaceHandle> _halffaces, bool _topologyCheck = false) override;

    CellHandle add_cell(const std::vector<VertexHandle>& _vertices, bool _topologyCheck = false);

    CellHandle add_cell(VertexHandle _vh0, VertexHandle _vh1, VertexHandle _vh2, VertexHandle _vh3, bool _topologyCheck = false);

    HalfFaceHandle add_halfface(const std::vector<HalfEdgeHandle>& _halfedges, bool _topologyCheck = false);
    HalfFaceHandle add_halfface(VertexHandle _vh0, VertexHandle _vh1, VertexHandle _vh2, bool _topologyCheck = false);

    HalfEdgeHandle add_halfedge(VertexHandle _fromVertex, VertexHandle _toVertex);

    /// Get the 4 vertices of the tet ch in a specific order:
    /// 1.-3. vertices of ch's first halfface, ccw, starting with the
    /// first from_vertex of the halfface's first halfedge. 4. Then comes the 4th vertex of the tet.
    std::vector<VertexHandle> get_cell_vertices(CellHandle ch) const;

    /// Get the 4 vertices of the tet ch in a specific order, starting with vh.
    std::vector<VertexHandle> get_cell_vertices(CellHandle ch, VertexHandle vh) const;

    /// Get the 4 vertices of hfh's incident cell in a specific order:
    /// 1.-3. vertices of hfh, ccw, starting with the
    /// first from_vertex of the halfface's first halfedge. 4. Then comes the 4th vertex of the tet.
    /// Returns an empty vector, if the incident cell is invalid (hfh is boundary).
    std::vector<VertexHandle> get_cell_vertices(HalfFaceHandle hfh) const;

    /// Get the 4 vertices of hfh's incident cell in a specific order:
    /// 1. heh.to_vertex, 2. heh.to_vertex, 3. 3rd vertex of hfh, 4. 4th vertex of the tet.
    /// heh is expected to be incident to hfh
    std::vector<VertexHandle> get_cell_vertices(HalfFaceHandle hfh, HalfEdgeHandle heh) const;

    /// Get the vertex of the halfface's incident cell that is not contained in the halfface hfh.
    /// If the incident cell is invalid (hfh is boundary), returns the invalid vertex handle
    VertexHandle halfface_opposite_vertex(HalfFaceHandle hfh) const;

    /// Get the first halfface of the tet ch that does not contain the vertex vh.
    HalfFaceHandle vertex_opposite_halfface(CellHandle ch, VertexHandle vh) const;


    VertexHandle collapse_edge(HalfEdgeHandle _heh);

    /// \brief In-place 2-to-3 flip.
    ///
    /// Replaces the two tetrahedra sharing the interior face \p _fh by
    /// three tetrahedra arranged around the edge that connects the two
    /// vertices opposite to \p _fh. No vertices are created and no
    /// coordinates change. The six boundary (cavity shell) faces keep
    /// their handles, halfface orientations and all their properties;
    /// the new internal edge and the new internal faces are created with
    /// default properties. Every new cell copies all cell properties of
    /// the removed cell with the smallest handle. Old cells and old
    /// internal simplices are only marked as deleted (deferred deletion,
    /// no garbage collection).
    ///
    /// Requires deferred deletion and all bottom-up incidences to be
    /// enabled. The request is rejected (returns \c false and changes
    /// nothing) for boundary faces, non-manifold or non-spherical
    /// neighborhoods, duplicate edges/faces, five not mutually distinct
    /// vertices or already occupied candidate simplices.
    ///
    /// \param _fh      Interior face shared by exactly two cells.
    /// \param _created On success, all replacement cells, sorted by
    ///                 handle. Untouched on failure.
    /// \return \c true if the flip was performed.
    ///
    /// Example:
    /// \code
    /// std::vector<CellHandle> new_cells;
    /// if (mesh.flip23(shared_face, new_cells)) {
    ///     // new_cells.size() == 3, the two old cells are tagged deleted
    /// }
    /// \endcode
    bool flip23(FaceHandle _fh, std::vector<CellHandle>& _created);

    /// \brief In-place 3-to-2 flip.
    ///
    /// Replaces the three tetrahedra around the interior, valence-three
    /// edge \p _eh by two tetrahedra sharing the new face that connects
    /// the three vertices of the edge's link. This is the inverse of
    /// flip23(). The same preservation and rejection rules as for
    /// flip23() apply: six shell faces keep their handles and
    /// orientations, new internal simplices get default properties, new
    /// cells copy the cell properties of the smallest-handled removed
    /// cell and nothing is garbage collected.
    ///
    /// \param _eh      Interior edge with exactly three incident cells.
    /// \param _created On success, the two replacement cells sorted by
    ///                 handle. Untouched on failure.
    /// \return \c true if the flip was performed.
    ///
    /// Example:
    /// \code
    /// std::vector<CellHandle> created_23, created_32;
    /// mesh.flip23(face_handle, created_23);
    /// // flip back immediately:
    /// mesh.flip32(new_internal_edge, created_32);
    /// \endcode
    bool flip32(EdgeHandle _eh, std::vector<CellHandle>& _created);

protected:
    void split_edge(HalfEdgeHandle _heh, VertexHandle _vh);
    void split_face(FaceHandle _fh, VertexHandle _vh);

    /// Fully validated description of a flip cavity that is ready to be
    /// written. Built by the planning methods without mutating the mesh
    /// so that geometry kernels can inspect the candidate tets before
    /// committing via execute_flip().
    struct FlipPlan {
        enum class Kind { Flip23, Flip32 } kind = Kind::Flip23;

        // The five cavity vertices (for a 23: three shared face vertices
        // plus the two opposite vertices; for a 32: the edge endpoints
        // plus the three link vertices).
        std::array<VertexHandle, 5> vertices{};

        // Cells, internal faces and (32 only) internal edge to remove.
        std::vector<CellHandle> old_cells;
        std::vector<FaceHandle> old_internal_faces;
        EdgeHandle old_internal_edge = InvalidEdgeHandle;

        // Oriented vertex quadruples of the replacement tets. The order
        // is chosen such that add_cell(v[0], v[1], v[2], v[3]) reuses
        // the existing, identically oriented shell halffaces.
        std::vector<std::array<VertexHandle, 4>> new_tets;

        // Shell faces: the face handle together with the halfface that
        // pointed into the cavity, whose orientation is reused by the
        // corresponding replacement tet.
        struct ShellFace {
            FaceHandle face;
            HalfFaceHandle inner_halfface;
        };
        std::vector<ShellFace> shell_faces;
    };

    /// Validate a 23 request and fill \p _plan without touching the mesh.
    bool plan_flip23(FaceHandle _fh, FlipPlan& _plan) const;

    /// Validate a 32 request and fill \p _plan without touching the mesh.
    bool plan_flip32(EdgeHandle _eh, FlipPlan& _plan) const;

    /// Apply a previously planned flip to the mesh.
    bool execute_flip(const FlipPlan& _plan, std::vector<CellHandle>& _created);

public:


    // ======================= Specialized Iterators =============================

    friend class TetVertexIter;

    typedef class TetVertexIter TetVertexIter;

    /// Returns an iterator to iterate over the four vertices of a tetrahedron in a specific order:
    /// 1.-3. vertices of the tet's first halfface,
    /// starting with the halfface's first halfedge's from_vertex, then 4. the remaining fourth vertex of the tet.
    /// Uses get_cell_vertices(ch).
    TetVertexIter tv_iter(CellHandle _ref_h, int _max_laps = 1) const {
        return TetVertexIter(_ref_h, this, _max_laps);
    }

    std::pair<TetVertexIter,TetVertexIter> tet_vertices(CellHandle _ref_h, int _max_laps = 1) const {
        TetVertexIter begin = tv_iter(_ref_h, _max_laps);
        TetVertexIter end   = make_end_circulator(begin);
        return std::make_pair(begin, end);
    }

private:
   // void replaceHalfFace(CellHandle ch, HalfFaceHandle hf_del, HalfFaceHandle hf_ins);
   // void replaceHalfEdge(HalfFaceHandle hfh, HalfEdgeHandle he_del, HalfEdgeHandle he_ins);

    template <typename PropIterator, typename Handle>
    void swapPropertyElements(PropIterator begin, PropIterator end, Handle source, Handle destination)
    {
        PropIterator p_iter =  begin;
        for (; p_iter != end; ++p_iter)
            (*p_iter)->swap_elements(source, destination);
    }
};

} // Namespace OpenVolumeMesh

