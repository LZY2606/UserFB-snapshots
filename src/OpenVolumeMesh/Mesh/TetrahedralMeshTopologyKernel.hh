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
#include <array>
#include <set>
#include <vector>

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

    // ======================= Local flips (2->3 and 3->2) =====================

    /**
     * \brief In-place 2-to-3 flip of an internal triangular face.
     *
     * The two tetrahedra sharing the internal face \p _fh are replaced by
     * three tetrahedra arranged around a new internal edge that connects the
     * two vertices opposite to \p _fh. No vertices are added and no vertex
     * coordinates are changed. The six faces of the cavity hull keep their
     * handles, half-face orientations and properties; the old internal face
     * and the two old cells are marked deleted (deferred deletion, no garbage
     * collection is triggered).
     *
     * The function only performs combinatorial checks. It returns false
     * (without modifying the mesh) when deferred deletion or the full
     * bottom-up incidences are disabled, when the handle is invalid or
     * deleted, when the face is on the boundary, when the five involved
     * vertices are not pairwise distinct, when the cavity has a
     * non-spherical link, when duplicate edges/faces are present, or when a
     * candidate internal edge/face is already in use by another live entity.
     *
     * \param _fh Internal face shared by exactly two tetrahedra.
     * \param _created On success, the three new cell handles, sorted ascending.
     * \return true on success, false if the flip was rejected.
     *
     * Example:
     * \code
     * TetrahedralMeshTopologyKernel mesh;
     * // ... two tets sharing an internal face fh ...
     * std::vector<CellHandle> created;
     * if (mesh.flip23(fh, created)) {
     *     // created holds the three tets around the new internal edge
     * }
     * \endcode
     */
    bool flip23(FaceHandle _fh, std::vector<CellHandle>& _created);

    /**
     * \brief In-place 3-to-2 flip of an internal edge of valence three.
     *
     * The three tetrahedra around the internal edge \p _eh are replaced by
     * two tetrahedra sharing a new internal triangular face whose vertices
     * are the three vertices opposite to \p _eh in the cavity. This is the
     * exact inverse of flip23(). No vertices are added and no vertex
     * coordinates are changed. The six faces of the cavity hull keep their
     * handles, half-face orientations and properties; the old internal edge,
     * the old internal faces and the three old cells are marked deleted
     * (deferred deletion, no garbage collection is triggered).
     *
     * The function only performs combinatorial checks. It returns false
     * (without modifying the mesh) when deferred deletion or the full
     * bottom-up incidences are disabled, when the handle is invalid or
     * deleted, when the edge is on the boundary or does not have valence
     * three, when the five involved vertices are not pairwise distinct, when
     * the cavity has a non-spherical link, when duplicate edges/faces are
     * present, or when a candidate internal face is already in use by another
     * live entity.
     *
     * \param _eh Internal edge incident to exactly three tetrahedra.
     * \param _created On success, the two new cell handles, sorted ascending.
     * \return true on success, false if the flip was rejected.
     *
     * Example:
     * \code
     * std::vector<CellHandle> created;
     * if (mesh.flip32(edge, created)) {
     *     // created holds the two tets sharing the new internal face
     * }
     * \endcode
     */
    bool flip32(EdgeHandle _eh, std::vector<CellHandle>& _created);

protected:
    void split_edge(HalfEdgeHandle _heh, VertexHandle _vh);
    void split_face(FaceHandle _fh, VertexHandle _vh);

    /// Combinatorial description of a flip cavity. Shared by the geometry
    /// kernel, which adds the geometric validity checks before applying it.
    struct FlipCavity {
        // The five cavity vertices. For a 2->3 flip: vertices of the shared
        // face (a, b, c) and the two opposite vertices (d, e). For a 3->2
        // flip: endpoints of the central edge (a, b) and the three link
        // vertices (c, d, e).
        std::array<VertexHandle, 5> vertices{};
        // The two (2->3) or three (3->2) cells that get removed.
        std::vector<CellHandle> old_cells;
        // The six hull faces (handles and orientations stay untouched).
        std::array<FaceHandle, 6> hull_faces{};
        // Replacement tets as oriented vertex quadruples; their outer
        // halffaces are exactly the six hull faces in their stored
        // orientation.
        std::vector<std::array<VertexHandle, 4>> new_cells;
        // Simplices that get removed in addition to the old cells.
        std::vector<EdgeHandle> old_edges;
        std::vector<FaceHandle> old_faces;
    };

    bool analyze_flip23_cavity(FaceHandle _fh, FlipCavity& _cavity) const;
    bool analyze_flip32_cavity(EdgeHandle _eh, FlipCavity& _cavity) const;
    bool apply_flip(FlipCavity& _cavity, std::vector<CellHandle>& _created);

    /// True if a live (non-deleted) edge between the two vertices exists.
    bool has_live_edge(VertexHandle _v0, VertexHandle _v1) const;

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

