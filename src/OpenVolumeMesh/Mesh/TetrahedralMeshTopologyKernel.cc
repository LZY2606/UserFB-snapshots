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

#include <OpenVolumeMesh/Mesh/TetrahedralMeshTopologyKernel.hh>

#include <algorithm>
#include <iostream>

namespace OpenVolumeMesh {

namespace {

// Vertices of a halfface in its stored orientation (the orientation the
// owning cell uses for the face).
std::array<VertexHandle, 3> halfface_vertex_tuple(const TetrahedralMeshTopologyKernel& _mesh,
                                                  HalfFaceHandle _hfh)
{
    const auto vhs = _mesh.get_halfface_vertices(_hfh);
    return {vhs[0], vhs[1], vhs[2]};
}

// Orientation (permutation sign) of the triple (v0, v1, v2) relative to the
// stored halfface orientation (a, b, c): +1 for an even permutation
// (same orientation), -1 for an odd one (opposite orientation). The two
// triples are known to contain the same set of vertices.
int orientation_sign(const std::array<VertexHandle, 3>& _abc,
                     VertexHandle _v0, VertexHandle _v1, VertexHandle _v2)
{
    int pos0 = -1, pos1 = -1, pos2 = -1;
    for (int i = 0; i < 3; ++i) {
        if (_abc[i] == _v0) pos0 = i;
        if (_abc[i] == _v1) pos1 = i;
        if (_abc[i] == _v2) pos2 = i;
    }
    const int inversions =
        (pos0 > pos1) + (pos0 > pos2) + (pos1 > pos2);
    return (inversions % 2 == 0) ? 1 : -1;
}

// The halfface of _fh whose incident cell is _owner, i.e. the halfface in
// the orientation the owning cell stores for the face. Valid while the old
// cells are still alive.
HalfFaceHandle cavity_side_halfface(const TetrahedralMeshTopologyKernel& _mesh,
                                    FaceHandle _fh, CellHandle _owner)
{
    const HalfFaceHandle h0 = _mesh.halfface_handle(_fh, 0);
    if (_mesh.incident_cell(h0) == _owner) return h0;
    return _mesh.halfface_handle(_fh, 1);
}

} // anonymous namespace

//========================================================================================

bool TetrahedralMeshTopologyKernel::has_live_edge(VertexHandle _v0,
                                                  VertexHandle _v1) const
{
    const HalfEdgeHandle heh = find_halfedge(_v0, _v1);
    return heh.is_valid() && !is_deleted(edge_handle(heh));
}

//========================================================================================

bool TetrahedralMeshTopologyKernel::analyze_flip23_cavity(
        FaceHandle _fh, FlipCavity& _cavity) const
{
    if (!is_valid(_fh) || is_deleted(_fh)) return false;
    if (valence(_fh) != 3) return false;

    const HalfFaceHandle hfh0 = halfface_handle(_fh, 0);
    const HalfFaceHandle hfh1 = halfface_handle(_fh, 1);

    // The shared face must be internal, with a live tet on both sides.
    const CellHandle c0 = incident_cell(hfh0);
    const CellHandle c1 = incident_cell(hfh1);
    if (!c0.is_valid() || !c1.is_valid() || c0 == c1) return false;
    if (is_deleted(c0) || is_deleted(c1)) return false;
    if (valence(c0) != 4 || valence(c1) != 4) return false;

    const auto face_vhs = get_halfface_vertices(hfh0);
    if (face_vhs.size() != 3) return false;
    const VertexHandle a = face_vhs[0];
    const VertexHandle b = face_vhs[1];
    const VertexHandle c = face_vhs[2];

    const auto v0 = get_cell_vertices(c0);
    const auto v1 = get_cell_vertices(c1);
    if (v0.size() != 4 || v1.size() != 4) return false;

    VertexHandle d = InvalidVertexHandle;
    for (const VertexHandle vh : v0) {
        if (vh != a && vh != b && vh != c) { d = vh; break; }
    }
    VertexHandle e = InvalidVertexHandle;
    for (const VertexHandle vh : v1) {
        if (vh != a && vh != b && vh != c) { e = vh; break; }
    }
    if (!d.is_valid() || !e.is_valid()) return false;

    // Five pairwise distinct cavity vertices.
    if (d == e || d == a || d == b || d == c ||
        e == a || e == b || e == c) return false;

    _cavity.vertices = {a, b, c, d, e};
    _cavity.old_cells = {c0, c1};

    // Six hull faces: every face of the two tets except the shared one.
    std::set<FaceHandle> hull_set;
    for (const CellHandle ch : _cavity.old_cells) {
        for (auto it = cf_iter(ch); it.valid(); ++it) {
            const FaceHandle fh = *it;
            if (fh != _fh) {
                if (is_deleted(fh)) return false;
                hull_set.insert(fh);
            }
        }
    }
    if (hull_set.size() != 6) return false; // non-spherical link / self adjacency

    // The candidate internal edge de must not be in use by a live edge.
    if (has_live_edge(d, e)) return false;

    // Build the three replacement tets (x, y, p, q), where p is the
    // opposite vertex of the tet owning hull face (x, y, p) and q the
    // opposite vertex on the other side.
    const std::array<std::array<VertexHandle, 4>, 3> triples = {{
        {a, b, d, e},
        {b, c, d, e},
        {c, a, d, e}
    }};

    for (const auto& t : triples) {
        const VertexHandle x = t[0], y = t[1], p = t[2], q = t[3];
        std::array<VertexHandle, 4> tet = {x, y, p, q};

        // Two hull halffaces of this candidate tet:
        // (x, y, p) belongs to c0, (y, x, q) belongs to c1.
        const HalfFaceHandle hf_p = find_halfface({x, y, p});
        const HalfFaceHandle hf_q = find_halfface({y, x, q});
        if (!hf_p.is_valid() || !hf_q.is_valid()) return false;
        const FaceHandle fp = face_handle(hf_p);
        const FaceHandle fq = face_handle(hf_q);
        if (is_deleted(fp) || is_deleted(fq)) return false;
        if (hull_set.count(fp) == 0 || hull_set.count(fq) == 0) return false;
        if (fp == fq) return false;

        // The other two boundary triangles are internal candidates: they
        // must not be occupied by another live face.
        const std::array<std::array<VertexHandle, 3>, 4> boundary_tris = {{
            {x, y, p}, {x, p, q}, {x, q, y}, {y, q, p}
        }};
        for (const auto& tri : boundary_tris) {
            const HalfFaceHandle found = find_halfface(
                std::vector<VertexHandle>(tri.begin(), tri.end()));
            if (found.is_valid() && !is_deleted(face_handle(found))) {
                const FaceHandle fh = face_handle(found);
                if (fh != fp && fh != fq) return false;
            }
        }

        // Orient the tet so that its f0 face (x, y, p) reuses the stored
        // halfface orientation of the hull (handles and orientations of
        // the six hull faces must be preserved).
        const HalfFaceHandle hullSide = cavity_side_halfface(*this, fp, c0);
        const auto abc = halfface_vertex_tuple(*this, hullSide);
        if (orientation_sign(abc, x, y, p) < 0) {
            std::swap(tet[2], tet[3]);
        }

        _cavity.new_cells.push_back(tet);
    }

    if (_cavity.new_cells.size() != 3) return false;

    std::copy(hull_set.begin(), hull_set.end(), _cavity.hull_faces.begin());
    _cavity.old_faces.push_back(_fh);
    return true;
}

//========================================================================================

bool TetrahedralMeshTopologyKernel::analyze_flip32_cavity(
        EdgeHandle _eh, FlipCavity& _cavity) const
{
    if (!is_valid(_eh) || is_deleted(_eh)) return false;
    if (valence(_eh) != 3) return false;
    if (is_boundary(_eh)) return false;

    // Exactly three live, distinct incident cells.
    std::set<CellHandle> cell_set;
    for (auto it = ec_iter(_eh); it.valid(); ++it) {
        const CellHandle ch = *it;
        if (is_deleted(ch)) return false;
        cell_set.insert(ch);
    }
    if (cell_set.size() != 3) return false;
    for (const CellHandle ch : cell_set) {
        if (valence(ch) != 4) return false;
    }
    _cavity.old_cells.assign(cell_set.begin(), cell_set.end());

    const auto endpoints = edge_vertices(_eh);
    const VertexHandle a = endpoints[0];
    const VertexHandle b = endpoints[1];

    // The three link vertices, two per incident tet.
    std::set<VertexHandle> link_set;
    for (const CellHandle ch : _cavity.old_cells) {
        const auto vhs = get_cell_vertices(ch);
        for (const VertexHandle vh : vhs) {
            if (vh != a && vh != b) link_set.insert(vh);
        }
    }
    if (link_set.size() != 3) return false; // non-spherical link
    std::vector<VertexHandle> link(link_set.begin(), link_set.end());
    const VertexHandle c = link[0];
    const VertexHandle d = link[1];
    const VertexHandle e = link[2];

    // Five pairwise distinct cavity vertices.
    if (c == a || c == b || d == a || d == b || e == a || e == b) return false;

    _cavity.vertices = {a, b, c, d, e};

    // The three internal faces contain the central edge; the other six
    // faces of the tets are the cavity hull.
    std::set<FaceHandle> internal_set;
    std::set<FaceHandle> hull_set;
    for (const CellHandle ch : _cavity.old_cells) {
        for (auto it = cf_iter(ch); it.valid(); ++it) {
            const FaceHandle fh = *it;
            if (is_deleted(fh)) return false;
            if (is_incident(fh, _eh)) internal_set.insert(fh);
            else hull_set.insert(fh);
        }
    }
    if (internal_set.size() != 3 || hull_set.size() != 6) return false;

    // Every hull face has exactly one cavity side. Its other side is
    // either the mesh boundary or a live cell outside the cavity.
    for (const FaceHandle fh : hull_set) {
        const auto cells = face_cells(fh);
        int inside = 0;
        for (const CellHandle owner : cells) {
            if (!owner.is_valid()) continue; // boundary side is allowed
            if (is_deleted(owner)) return false;
            if (cell_set.count(owner) > 0) ++inside;
        }
        if (inside != 1) return false; // non-spherical link
    }

    // The candidate internal face cde must not be occupied by a live
    // face. Its three edges are hull edges: they already exist live and
    // are reused, so only the face itself can be in conflict here.
    {
        const HalfFaceHandle found = find_halfface({c, d, e});
        if (found.is_valid() && !is_deleted(face_handle(found))) return false;
    }

    // Build the two replacement tets, one per endpoint of the removed edge.
    const std::array<VertexHandle, 2> apices = {{a, b}};
    for (const VertexHandle apex : apices) {
        const VertexHandle x = c, y = d, z = e;
        std::array<VertexHandle, 4> tet = {apex, x, y, z};

        // The three outer halffaces opposite the link vertices must be
        // hull faces: (apex, z, y), (apex, x, z), (apex, y, x).
        std::array<FaceHandle, 3> outer = {
            InvalidFaceHandle, InvalidFaceHandle, InvalidFaceHandle};
        const std::array<std::array<VertexHandle, 3>, 3> outer_tris = {{
            {apex, z, y},
            {apex, x, z},
            {apex, y, x}
        }};
        for (int i = 0; i < 3; ++i) {
            const HalfFaceHandle found = find_halfface(
                std::vector<VertexHandle>(outer_tris[i].begin(),
                                          outer_tris[i].end()));
            if (!found.is_valid() || is_deleted(face_handle(found))) return false;
            const FaceHandle fh = face_handle(found);
            if (hull_set.count(fh) == 0) return false;
            outer[i] = fh;
        }
        if (outer[0] == outer[1] || outer[1] == outer[2] ||
            outer[0] == outer[2]) return false;

        // No other boundary triangle of the candidate tet may be taken.
        const std::array<std::array<VertexHandle, 3>, 4> boundary_tris = {{
            {apex, x, y}, {apex, y, z},
            {apex, z, x}, {x, z, y}
        }};
        for (const auto& tri : boundary_tris) {
            const HalfFaceHandle found = find_halfface(
                std::vector<VertexHandle>(tri.begin(), tri.end()));
            if (found.is_valid() && !is_deleted(face_handle(found))) {
                if (hull_set.count(face_handle(found)) == 0) return false;
            }
        }

        // A cell's own halfface orientation points into the cell; after
        // the flip the same stored halfface must point into the new tet.
        // outer[0] has vertex set {apex, y, z}, which is f1 of
        // (apex, x, y, z), i.e. the sequence (apex, y, z).
        const HalfFaceHandle h0 = halfface_handle(outer[0], 0);
        const HalfFaceHandle h1 = halfface_handle(outer[0], 1);
        const HalfFaceHandle hullSide =
            cell_set.count(incident_cell(h0)) ? h0 : h1;
        const auto abc = halfface_vertex_tuple(*this, hullSide);
        if (orientation_sign(abc, apex, y, z) < 0) {
            // Negate the tet orientation by swapping two vertices.
            std::swap(tet[1], tet[2]);
        }

        _cavity.new_cells.push_back(tet);
    }

    if (_cavity.new_cells.size() != 2) return false;

    std::copy(hull_set.begin(), hull_set.end(), _cavity.hull_faces.begin());
    _cavity.old_faces.assign(internal_set.begin(), internal_set.end());
    _cavity.old_edges.push_back(_eh);
    return true;
}

//========================================================================================

bool TetrahedralMeshTopologyKernel::apply_flip(
        FlipCavity& _cavity, std::vector<CellHandle>& _created)
{
    // All combinatorial validation has to happen before this point: the
    // mesh must remain untouched if the flip is rejected. Determine the
    // cell whose properties are inherited by every replacement cell:
    // the removed cell with the smallest handle.
    CellHandle property_source = _cavity.old_cells.front();
    for (const CellHandle ch : _cavity.old_cells) {
        if (ch < property_source) property_source = ch;
    }

    // Remove the old cells first so that their halffaces become available
    // for the replacement cells.
    for (const CellHandle ch : _cavity.old_cells) {
        delete_cell(ch);
    }
    // The old internal simplices are no longer incident to any live cell.
    for (const FaceHandle fh : _cavity.old_faces) {
        delete_face(fh);
    }
    for (const EdgeHandle eh : _cavity.old_edges) {
        delete_edge(eh);
    }

    // Create the replacement tets. add_cell() reuses existing
    // edges/faces (the hull) and only allocates the internal ones;
    // all bottom-up incidences are updated by the base kernel.
    std::vector<CellHandle> created;
    created.reserve(_cavity.new_cells.size());
    for (const auto& vhs : _cavity.new_cells) {
        CellHandle ch = add_cell(vhs[0], vhs[1], vhs[2], vhs[3]);
        if (!ch.is_valid()) {
            // Should be unreachable after the upfront validation, and only
            // ordinary allocation failures can get us here.
            return false;
        }
        copy_property_elements(property_source, ch);
        created.push_back(ch);
    }

    std::sort(created.begin(), created.end());
    _created = std::move(created);
    return true;
}

//========================================================================================

bool TetrahedralMeshTopologyKernel::flip23(FaceHandle _fh,
                                           std::vector<CellHandle>& _created)
{
    if (!deferred_deletion_enabled() || !has_full_bottom_up_incidences()) {
        return false;
    }

    FlipCavity cavity;
    if (!analyze_flip23_cavity(_fh, cavity)) return false;

    return apply_flip(cavity, _created);
}

//========================================================================================

bool TetrahedralMeshTopologyKernel::flip32(EdgeHandle _eh,
                                           std::vector<CellHandle>& _created)
{
    if (!deferred_deletion_enabled() || !has_full_bottom_up_incidences()) {
        return false;
    }

    FlipCavity cavity;
    if (!analyze_flip32_cavity(_eh, cavity)) return false;

    return apply_flip(cavity, _created);
}

//========================================================================================

FaceHandle TetrahedralMeshTopologyKernel::add_face(std::vector<HalfEdgeHandle> _halfedges, bool _topologyCheck) {

    if(_halfedges.size() != 3) {
#ifndef NDEBUG
        std::cerr << "TetrahedralMeshTopologyKernel::add_face(): Face valence is not three! Returning" << std::endl;
        std::cerr << "invalid handle." << std::endl;
#endif
        return TopologyKernel::InvalidFaceHandle;
    }

    return TopologyKernel::add_face(std::move(_halfedges), _topologyCheck);
}

//========================================================================================


FaceHandle
TetrahedralMeshTopologyKernel::add_face(const std::vector<VertexHandle>& _vertices) {

    if(_vertices.size() != 3) {
#ifndef NDEBUG
        std::cerr << "TetrahedralMeshTopologyKernel::add_face(): Face valence is not three! Returning" << std::endl;
        std::cerr << "invalid handle." << std::endl;
#endif
        return TopologyKernel::InvalidFaceHandle;
    }

    return TopologyKernel::add_face(_vertices);
}

//========================================================================================


CellHandle
TetrahedralMeshTopologyKernel::add_cell(std::vector<HalfFaceHandle> _halffaces, bool _topologyCheck) {

    if(_halffaces.size() != 4) {
// To make this consistent with add_face
#ifndef NDEBUG
        std::cerr << "Cell valence is not four! Aborting." << std::endl;
#endif
        return TopologyKernel::InvalidCellHandle;
    }
    for(const auto &hfh: _halffaces) {
        auto n_halfedges = TopologyKernel::valence(TopologyKernel::face_handle(hfh));
        if (n_halfedges != 3) {
#ifndef NDEBUG
            std::cerr << "TetrahedralMeshTopologyKernel::add_cell(): Incident face " << hfh.idx() << " does not have valence three: " << n_halfedges << "; not adding cell." << std::endl;
#endif
            return TopologyKernel::InvalidCellHandle;
        }
    }

    return TopologyKernel::add_cell(std::move(_halffaces), _topologyCheck);
}


HalfEdgeHandle TetrahedralMeshTopologyKernel::add_halfedge(VertexHandle _fromVertex, VertexHandle _toVertex)
{
    HalfEdgeHandle he = find_halfedge(_fromVertex, _toVertex);
    if (he != InvalidHalfEdgeHandle)
        return he;
    else
        return halfedge_handle(add_edge(_fromVertex, _toVertex), 0);
}

HalfFaceHandle TetrahedralMeshTopologyKernel::add_halfface(const std::vector<HalfEdgeHandle>& _halfedges, bool _topologyCheck)
{
    HalfFaceHandle hf = find_halfface(_halfedges);
    if (hf != InvalidHalfFaceHandle)
        return hf;
    else
        return halfface_handle(add_face(_halfedges, _topologyCheck), 0);
}

HalfFaceHandle TetrahedralMeshTopologyKernel::add_halfface(VertexHandle _vh0, VertexHandle _vh1, VertexHandle _vh2, bool _topologyCheck)
{
    std::vector<HalfEdgeHandle> halfedges;
    halfedges.push_back(add_halfedge(_vh0, _vh1));
    halfedges.push_back(add_halfedge(_vh1, _vh2));
    halfedges.push_back(add_halfedge(_vh2, _vh0));
    return add_halfface(halfedges, _topologyCheck);
}

/*void TetrahedralMeshTopologyKernel::replaceHalfFace(CellHandle ch, HalfFaceHandle hf_del, HalfFaceHandle hf_ins)
{
    Cell& c = cells_[ch.idx()];
    std::vector<HalfFaceHandle> hfs;
    for (unsigned int i = 0; i < c.halffaces().size(); ++i)
        if (c.halffaces()[i] != hf_del)
            hfs.push_back(c.halffaces()[i]);
        else
            hfs.push_back(hf_ins);
    c.set_halffaces(hfs);
}

void TetrahedralMeshTopologyKernel::replaceHalfEdge(HalfFaceHandle hfh, HalfEdgeHandle he_del, HalfEdgeHandle he_ins)
{
    FaceHandle fh = face_handle(hfh);
    unsigned char oppF = hfh.idx() - halfface_handle(fh, 0);
    if (oppF == 1)
    {
        he_del = opposite_halfedge_handle(he_del);
        he_ins = opposite_halfedge_handle(he_ins);
    }
    Face& f = faces_[fh.idx()];
    std::vector<HalfEdgeHandle> hes;
    for (unsigned int i = 0; i < f.halfedges().size(); ++i)
        if (f.halfedges()[i] != he_del)
            hes.push_back(f.halfedges()[i]);
        else
            hes.push_back(he_ins);
    f.set_halfedges(hes);

}*/

/*
void TetrahedralMeshTopologyKernel::collapse_edge(HalfEdgeHandle _heh)
{

    std::vector<bool> deleteTagFaces(faces_.size(), false);
    std::vector<bool> deleteTagEdges(edges_.size(), false);
    std::vector<bool> deleteTagCells(cells_.size(), false);

    for (HalfEdgeHalfFaceIter hehf_it = hehf_iter(_heh); hehf_it.valid(); ++hehf_it)
    {
        CellHandle ch = incident_cell(*hehf_it);
        if (ch.is_valid())
        {
            HalfFaceHandle hf134 = *hehf_it;
            HalfFaceHandle hf143 = opposite_halfface_handle(hf143);
            HalfEdgeHandle he13  = prev_halfedge_in_halfface(_heh, hf134);
            HalfEdgeHandle he41  = next_halfedge_in_halfface(_heh, hf134);
            HalfFaceHandle hf123 = adjacent_halfface_in_cell(hf134, he13);
            HalfFaceHandle hf142 = adjacent_halfface_in_cell(hf134, he41);
            HalfFaceHandle hf132 = opposite_halfface_handle(hf123);
            CellHandle ch0123 = incident_cell(hf132);
            HalfEdgeHandle he32  = next_halfedge_in_halfface(he13, hf132);
            HalfEdgeHandle he23  = opposite_halfedge_handle(he32);
            HalfEdgeHandle he14  = opposite_halfedge_handle(he41);
            HalfEdgeHandle he31  = opposite_halfedge_handle(he13);
            HalfEdgeHandle he42  = next_halfedge_in_halfface(he14, hf142);
            HalfEdgeHandle he24  = opposite_halfedge_handle(he42);
            HalfFaceHandle hf243 = adjacent_halfface_in_cell(hf134, he41);
            HalfFaceHandle hf234 = opposite_halfface_handle(hf234);
            HalfEdgeHandle he12  = next_halfedge_in_halfface(he42, hf142);
            HalfEdgeHandle he21  = opposite_halfedge_handle(he12);

            if (ch0123.is_valid())
            {
                HalfFaceHandle hf031 = adjacent_halfface_in_cell(hf132, he13);
                HalfFaceHandle hf023 = adjacent_halfface_in_cell(hf132, he32);

                replaceHalfEdge(hf031, he31, he41);
                replaceHalfEdge(hf023, he23, he24);
                replaceHalfFace(ch0123, hf132, hf142);
            }

            //copyHalfFaceAndHalfEdgeProperties(hf132, 142);

            incident_cell_per_hf_[hf142.idx()] = ch0123;


            deleteTagCells[ch.idx()] = true;
            deleteTagFaces[face_handle(hf132).idx()] = true;
            deleteTagFaces[face_handle(*hehf_it).idx()] = true;
            deleteTagEdges[edge_handle(he13).idx()] = true;
            deleteTagEdges[edge_handle(he32).idx()] = true;

            std::set<HalfFaceHandle> excludeFaces;
            excludeFaces.insert(hf134);
            excludeFaces.insert(hf143);
            excludeFaces.insert(hf123);
            excludeFaces.insert(hf132);
            excludeFaces.insert(hf243);
            excludeFaces.insert(hf234);

            std::vector<std::pair<HalfEdgeHandle, HalfEdgeHandle> > joinpartners;
            joinpartners.push_back(std::make_pair(he41, he31));
            joinpartners.push_back(std::make_pair(he14, he13));
            joinpartners.push_back(std::make_pair(he42, he32));
            joinpartners.push_back(std::make_pair(he24, he23));

            for (unsigned int i = 0; i < joinpartners.size(); ++i)
            {
                HalfEdgeHandle target = joinpartners[i].first;
                HalfEdgeHandle source = joinpartners[i].second;
                std::vector<HalfFaceHandle> incidentHfs;
                for (unsigned int j = 0; j < incident_hfs_per_he_[target.idx()].size(); ++j)
                {
                    HalfFaceHandle cur_hf = incident_hfs_per_he_[target.idx()][j];
                    if ((excludeFaces.find(cur_hf) == excludeFaces.end()) && !deleteTagFaces[face_handle(cur_hf).idx()])
                        incidentHfs.push_back(cur_hf);
                }
                for (unsigned int i = 0; i < incident_hfs_per_he_[source.idx()].size(); ++i)
                {
                    HalfFaceHandle cur_hf = incident_hfs_per_he_[source.idx()][i];
                    if ((excludeFaces.find(cur_hf) == excludeFaces.end()) && !deleteTagFaces[face_handle(cur_hf).idx()])
                        incidentHfs.push_back(cur_hf);
                }

                std::swap(incident_hfs_per_he_[target], incidentHfs);
            }

            std::vector<HalfFaceHandle>& vec = incident_hfs_per_he_[he21];
            vec.erase(std::remove(vec.begin(), vec.end(), hf132), vec.end());
            std::vector<HalfFaceHandle>& vec2 = incident_hfs_per_he_[he12];
            vec2.erase(std::remove(vec2.begin(), vec2.end(), hf123), vec2.end());

        }
        else
        {
            deleteTagFaces[face_handle(*hehf_it).idx()] = true;
        }
    }


    VertexHandle from_vh = halfedge(_heh).from_vertex();
    VertexHandle to_vh = halfedge(_heh).to_vertex();
    for (VertexOHalfEdgeIter voh_it = voh_iter(from_vh); voh_it.valid(); ++voh_it )
    {
        Edge he = halfedge(*voh_it);
        if (he.to_vertex() == to_vh)
        {
            std::vector<HalfEdgeHandle>& vec = outgoing_hes_per_vertex_[to_vh];
            vec.erase(std::remove(vec.begin(), vec.end(), opposite_halfedge_handle(*voh_it)), vec.end());
        }
        EdgeHandle eh = edge_handle(*voh_it);
        if (!deleteTagEdges[eh.idx()])
        {
            std::vector<HalfEdgeHandle>& vec = outgoing_hes_per_vertex_[to_vh];
            vec.push_back(opposite_halfedge_handle(*voh_it));

            Edge& e = edges_[eh.idx()];
            if (e.from_vertex() == from_vh)
                e.set_from_vertex(to_vh);
            if (e.to_vertex() == from_vh)
                e.set_to_vertex(to_vh);

        }
    }

    outgoing_hes_per_vertex_[from_vh].clear();

    deleteTagEdges[edge_handle(_heh).idx()] = true;

    delete_multiple_cells(deleteTagCells);
    delete_multiple_faces(deleteTagFaces);
    delete_multiple_edges(deleteTagEdges);
    delete_vertex(from_vh);
}
*/


//void TetrahedralMeshTopologyKernel::swapCellProperties(CellHandle source, CellHandle destination)
//{
//    swapPropertyElements(cell_props_begin(), cell_props_end(), source, destination);
//}

//void TetrahedralMeshTopologyKernel::swapHalfFaceProperties(HalfFaceHandle source, HalfFaceHandle destination)
//{
//    swapPropertyElements(halfface_props_begin(), halfface_props_end(), source, destination);
//}

//void TetrahedralMeshTopologyKernel::swapHalfEdgeProperties(HalfEdgeHandle source, HalfEdgeHandle destination)
//{
//    swapPropertyElements(halfedge_props_begin(), halfedge_props_end(), source, destination);
//}

// cppcheck-suppress unusedFunction ; public interface
VertexHandle TetrahedralMeshTopologyKernel::collapse_edge(HalfEdgeHandle _heh)
{
    bool deferred_deletion_tmp = deferred_deletion_enabled();

    if (!deferred_deletion_tmp)
        enable_deferred_deletion(true);

    VertexHandle from_vh = halfedge(_heh).from_vertex();
    VertexHandle to_vh   = halfedge(_heh).to_vertex();


    // find cells that will collapse, i.e. are incident to the collapsing halfedge
    std::set<CellHandle> collapsingCells;
    for (HalfEdgeHalfFaceIter hehf_it = hehf_iter(_heh); hehf_it.valid(); ++hehf_it)
    {
        HalfFaceHandle hfh = *hehf_it;
        CellHandle ch = incident_cell(hfh);
        if (ch.is_valid())
            collapsingCells.insert(ch);
    }

    std::vector<CellHandle> incidentCells;
    for (VertexCellIter vc_it = vc_iter(from_vh); vc_it.valid(); ++vc_it)
        incidentCells.push_back(*vc_it);

    std::vector<std::pair<CellHandle, std::vector<HalfFaceHandle>>> new_cells;

    for (const CellHandle &ch: incidentCells)
    {
        if (collapsingCells.find(ch) != collapsingCells.end())
            continue;

        Cell c = cell(ch);

        std::vector<HalfFaceHandle> newHalffaces;
        newHalffaces.reserve(4);

        for (unsigned int hf_idx = 0; hf_idx < 4; ++hf_idx)
        {
            Face hf = halfface(c.halffaces()[hf_idx]);
            std::vector<HalfEdgeHandle> newHalfedges;

            for (unsigned int j = 0; j < 3; ++j)
            {
                Edge e = halfedge(hf.halfedges()[j]);
                VertexHandle newStart = (e.from_vertex() == from_vh) ? to_vh: e.from_vertex();
                VertexHandle newEnd   = (e.to_vertex()   == from_vh) ? to_vh : e.to_vertex();

                HalfEdgeHandle heh = add_halfedge(newStart, newEnd);
                newHalfedges.push_back(heh);
                swap_property_elements(hf.halfedges()[j], heh);
            }

            HalfFaceHandle hfh = add_halfface(newHalfedges);
            newHalffaces.push_back(hfh);
            swap_property_elements(c.halffaces()[hf_idx], hfh);
        }

        delete_cell(ch);
        new_cells.emplace_back(ch, newHalffaces);

    }
    VertexHandle survivingVertex = to_vh;

    if (!deferred_deletion_tmp)
    {
        if (fast_deletion_enabled())
        {
            // from_vh is swapped with last vertex and then deleted
            if (to_vh.idx() == (int)n_vertices() - 1)
                survivingVertex = from_vh;
        }
        else
        {
            // from_vh is deleted and every vertex id larger than from_vh is reduced by one
            if (from_vh.idx() < to_vh.idx())
                survivingVertex = VertexHandle(to_vh.idx() - 1);
        }
    }

    delete_vertex(from_vh);

    for (const auto &n: new_cells) {
        CellHandle newCell = add_cell(std::move(n.second));
        swap_property_elements(n.first, newCell);
    }


    enable_deferred_deletion(deferred_deletion_tmp);

    return survivingVertex;

}

// cppcheck-suppress unusedFunction ; public interface
void TetrahedralMeshTopologyKernel::split_edge(HalfEdgeHandle _heh, VertexHandle _vh)
{
    bool deferred_deletion_tmp = deferred_deletion_enabled();

    if (!deferred_deletion_tmp)
        enable_deferred_deletion(true);

    std::vector<HalfFaceHandle> incident_halffaces_with_cells;
    for (HalfEdgeHalfFaceIter hehf_it = hehf_iter(_heh); hehf_it.valid(); ++hehf_it)
    {
        CellHandle ch = incident_cell(*hehf_it);
        if (ch.is_valid())
            incident_halffaces_with_cells.push_back(*hehf_it);
    }

    std::vector<std::pair<CellHandle, std::array<VertexHandle, 4>>> new_cells;

    for (auto hfh : incident_halffaces_with_cells)
    {
        CellHandle ch = incident_cell(hfh);

        std::vector<VertexHandle> vertices = get_cell_vertices(hfh, _heh);

        delete_cell(ch);

        new_cells.emplace_back(ch, std::array<VertexHandle, 4>{vertices[0], _vh, vertices[2], vertices[3]});
        new_cells.emplace_back(ch, std::array<VertexHandle, 4>{_vh, vertices[1], vertices[2], vertices[3]});
    }

    delete_edge(edge_handle(_heh));

    for (const auto &n: new_cells) {
        const auto &vhs = n.second;
        CellHandle newCell = add_cell(vhs[0], vhs[1], vhs[2], vhs[3]);
        copy_property_elements(n.first, newCell);
    }


    enable_deferred_deletion(deferred_deletion_tmp);

}

// cppcheck-suppress unusedFunction ; public interface
void TetrahedralMeshTopologyKernel::split_face(FaceHandle _fh, VertexHandle _vh)
{
    bool deferred_deletion_tmp = deferred_deletion_enabled();

    if (!deferred_deletion_tmp)
        enable_deferred_deletion(true);

    std::vector<std::pair<CellHandle, std::array<VertexHandle, 4>>> new_cells;

    for (char i = 0; i < 2; ++i)
    {
        HalfFaceHandle hfh = halfface_handle(_fh, i);
        CellHandle ch = incident_cell(hfh);
        if (ch.is_valid())
        {
            std::vector<VertexHandle> vertices = get_cell_vertices(hfh);

            delete_cell(ch);
            new_cells.emplace_back(ch, std::array<VertexHandle, 4>{vertices[0], vertices[1], _vh, vertices[3]});
            new_cells.emplace_back(ch, std::array<VertexHandle, 4>{vertices[0], _vh, vertices[2], vertices[3]});
            new_cells.emplace_back(ch, std::array<VertexHandle, 4>{_vh, vertices[1], vertices[2], vertices[3]});
        }
    }

    delete_face(_fh);

    for (const auto &n: new_cells) {
        const auto &vhs = n.second;
        CellHandle newCell = add_cell(vhs[0], vhs[1], vhs[2], vhs[3]);
        copy_property_elements(n.first, newCell);
    }

    enable_deferred_deletion(deferred_deletion_tmp);

}


std::vector<VertexHandle> TetrahedralMeshTopologyKernel::get_cell_vertices(CellHandle ch) const
{
    return get_cell_vertices(cell(ch).halffaces().front());
}

std::vector<VertexHandle> TetrahedralMeshTopologyKernel::get_cell_vertices(CellHandle ch, VertexHandle vh) const
{
    auto vhs = get_cell_vertices(cell(ch).halffaces()[0]);

    if (vhs[1]==vh) {return {vhs[1], vhs[2], vhs[0], vhs[3]};}
    if (vhs[2]==vh) {return {vhs[2], vhs[0], vhs[1], vhs[3]};}
    if (vhs[3]==vh) {return {vhs[3], vhs[1], vhs[0], vhs[2]};}
    return vhs;
}

std::vector<VertexHandle> TetrahedralMeshTopologyKernel::get_cell_vertices(HalfFaceHandle hfh) const
{
    const CellHandle ch = incident_cell(hfh);
    assert(ch.is_valid());
    if (!ch.is_valid()) return {};

    const auto& hfhs = cell(ch).halffaces();

    // Start with the 3 vertices of the given halfface
    std::vector<VertexHandle> cell_vhs;
    cell_vhs.reserve(4);
    for (VertexHandle vh : halfface_vertices(hfh)) {
        cell_vhs.push_back(vh);
    }

    // Look for the 4th vertex in another halfface of the cell
    HalfFaceHandle other_hfh = (hfh!=hfhs[0])? hfhs[0] : hfhs[1];
    for (VertexHandle other_vh : halfface_vertices(other_hfh))
    {
        if (cell_vhs[0] != other_vh && cell_vhs[1] != other_vh && cell_vhs[2] != other_vh)
        {
            cell_vhs.push_back(other_vh);
            return cell_vhs;
        }
    }

    // If we end up here, the tet contains less than 4 vertices
    assert(false);
    return {};
}

std::vector<VertexHandle> TetrahedralMeshTopologyKernel::get_cell_vertices(HalfFaceHandle hfh, HalfEdgeHandle heh) const
{
    auto vh0 = from_vertex_handle(heh);
    auto vh1 = to_vertex_handle(heh);

    // Ensure that first 3 vertices are of hfh -> 4th vertex stays fixed
    auto vhs = get_cell_vertices(hfh);

    // Ensure, we start with vh0 (note that vh0 cant be the 4th vertex cause heh must be contained in hfh)
    if (vhs[1] == vh0) vhs = {vhs[1],vhs[2],vhs[0],vhs[3]};
    else if (vhs[2] == vh0) vhs = {vhs[2],vhs[0],vhs[1],vhs[3]};

    // Ensure the 2nd vertex is vh1
    if (vhs[2] == vh1) vhs = {vhs[0],vhs[1],vhs[2],vhs[3]};
    else if (vhs[3] == vh1) vhs = {vhs[0],vhs[1],vhs[2],vhs[3]};

    return vhs;
}

VertexHandle TetrahedralMeshTopologyKernel::halfface_opposite_vertex(HalfFaceHandle hfh) const
{
    return is_boundary(hfh)? InvalidVertexHandle : get_cell_vertices(hfh)[3];
}

HalfFaceHandle TetrahedralMeshTopologyKernel::vertex_opposite_halfface(CellHandle ch, VertexHandle vh) const
{
    for (HalfFaceHandle hfh : cell(ch).halffaces())
    {
        const auto& vhs = get_halfface_vertices(hfh);
        if (vhs[0] != vh && vhs[1] != vh && vhs[2] != vh)
        {
            return hfh;
        }
    }
    return InvalidHalfFaceHandle;
}


//========================================================================================

CellHandle
TetrahedralMeshTopologyKernel::add_cell(const std::vector<VertexHandle>& _vertices, bool _topologyCheck)
{
    if(_vertices.size() != 4) {
        return CellHandle(-1);
    }

    // debug mode checks
    assert(TopologyKernel::has_full_bottom_up_incidences());

    // release mode checks
    if(!TopologyKernel::has_full_bottom_up_incidences()) {
        return CellHandle(-1);
    }


    HalfFaceHandle hf0, hf1, hf2, hf3;

    std::vector<VertexHandle> vs;

    vs.push_back(_vertices[0]);
    vs.push_back(_vertices[1]);
    vs.push_back(_vertices[2]);
    hf0 = TopologyKernel::find_halfface(vs);
    if(!hf0.is_valid()) {
        FaceHandle fh = TopologyKernel::add_face(vs);
        hf0 = halfface_handle(fh, 0);
    }
    vs.clear();

    vs.push_back(_vertices[0]);
    vs.push_back(_vertices[2]);
    vs.push_back(_vertices[3]);
    hf1 = TopologyKernel::find_halfface(vs);
    if(!hf1.is_valid()) {
        FaceHandle fh = TopologyKernel::add_face(vs);
        hf1 = halfface_handle(fh, 0);
    }
    vs.clear();

    vs.push_back(_vertices[0]);
    vs.push_back(_vertices[3]);
    vs.push_back(_vertices[1]);
    hf2 = TopologyKernel::find_halfface(vs);
    if(!hf2.is_valid()) {
        FaceHandle fh = TopologyKernel::add_face(vs);
        hf2 = halfface_handle(fh, 0);
    }
    vs.clear();

    vs.push_back(_vertices[1]);
    vs.push_back(_vertices[3]);
    vs.push_back(_vertices[2]);
    hf3 = TopologyKernel::find_halfface(vs);
    if(!hf3.is_valid()) {
        FaceHandle fh = TopologyKernel::add_face(vs);
        hf3 = halfface_handle(fh, 0);
    }
    vs.clear();

    assert(hf0.is_valid());
    assert(hf1.is_valid());
    assert(hf2.is_valid());
    assert(hf3.is_valid());


    std::vector<HalfFaceHandle> hfs;
    hfs.push_back(hf0);
    hfs.push_back(hf1);
    hfs.push_back(hf2);
    hfs.push_back(hf3);

    if (_topologyCheck) {
        /*
        * Test if all halffaces are connected and form a two-manifold
        * => Cell is closed
        *
        * This test is simple: The number of involved half-edges has to be
        * exactly twice the number of involved edges.
        */

        std::set<HalfEdgeHandle> incidentHalfedges;
        std::set<EdgeHandle>     incidentEdges;

        for(std::vector<HalfFaceHandle>::const_iterator it = hfs.begin(),
                end = hfs.end(); it != end; ++it) {

            OpenVolumeMeshFace hface = halfface(*it);
            for(std::vector<HalfEdgeHandle>::const_iterator he_it = hface.halfedges().begin(),
                    he_end = hface.halfedges().end(); he_it != he_end; ++he_it) {
                incidentHalfedges.insert(*he_it);
                incidentEdges.insert(edge_handle(*he_it));
            }
        }

        if(incidentHalfedges.size() != (incidentEdges.size() * 2u)) {
#ifndef NDEBUG
            std::cerr << "The specified halffaces are not connected!" << std::endl;
#endif
            return InvalidCellHandle;
        }
        // The halffaces are now guaranteed to form a two-manifold

        if(has_face_bottom_up_incidences()) {

            for(std::vector<HalfFaceHandle>::const_iterator it = hfs.begin(),
                    end = hfs.end(); it != end; ++it) {
                if(incident_cell(*it) != InvalidCellHandle) {
#ifndef NDEBUG
                    std::cerr << "Warning: One of the specified half-faces is already incident to another cell!" << std::endl;
#endif
                    return InvalidCellHandle;
                }
            }

        }

    }

    return TopologyKernel::add_cell(hfs, false);
}

CellHandle TetrahedralMeshTopologyKernel::add_cell(VertexHandle _vh0, VertexHandle _vh1, VertexHandle _vh2, VertexHandle _vh3, bool _topologyCheck)
{
    std::vector<HalfFaceHandle> halffaces;
    halffaces.push_back(add_halfface(_vh0, _vh1, _vh2));
    halffaces.push_back(add_halfface(_vh0, _vh2, _vh3));
    halffaces.push_back(add_halfface(_vh0, _vh3, _vh1));
    halffaces.push_back(add_halfface(_vh1, _vh3, _vh2));
    return add_cell(halffaces, _topologyCheck);
}

//========================================================================================

} // Namespace OpenVolumeMesh
