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
#include <array>
#include <iostream>
#include <map>
#include <set>

namespace OpenVolumeMesh {

namespace {

bool same_vertex_set(const std::vector<VertexHandle>& _halfface_vertices,
                     std::array<VertexHandle, 3> _triple)
{
    if(_halfface_vertices.size() != 3)
        return false;
    std::set<VertexHandle> lhs(_halfface_vertices.begin(), _halfface_vertices.end());
    std::set<VertexHandle> rhs(_triple.begin(), _triple.end());
    return lhs == rhs && lhs.size() == 3;
}

// Whether a live edge connects the two given vertices.
bool edge_exists(const TopologyKernel& _mesh, VertexHandle _v0, VertexHandle _v1)
{
    return _mesh.find_halfedge(_v0, _v1).is_valid();
}

// Whether a live triangular face with the given vertex set exists,
// independent of its orientation.
bool face_exists(const TopologyKernel& _mesh,
                 VertexHandle _v0, VertexHandle _v1, VertexHandle _v2)
{
    return _mesh.find_halfface(std::vector<VertexHandle>{_v0, _v1, _v2}).is_valid() ||
           _mesh.find_halfface(std::vector<VertexHandle>{_v0, _v2, _v1}).is_valid();
}

// Reject duplicate edges or triangular faces among the cavity vertices:
// every pair of cavity vertices may be joined by at most one live edge,
// and every vertex triple of cavity vertices may span at most one live
// face.
bool has_duplicate_simplices(const TopologyKernel& _mesh,
                             const std::array<VertexHandle, 5>& _vertices)
{
    for(size_t i = 0; i < _vertices.size(); ++i) {
        for(size_t j = i + 1; j < _vertices.size(); ++j) {
            unsigned edge_count = 0;
            for(EdgeHandle eh : _mesh.edges()) {
                if(_mesh.is_deleted(eh))
                    continue;
                const TopologyKernel::Edge& ed = _mesh.edge(eh);
                if((ed.from_vertex() == _vertices[i] && ed.to_vertex() == _vertices[j]) ||
                   (ed.from_vertex() == _vertices[j] && ed.to_vertex() == _vertices[i]))
                    ++edge_count;
            }
            if(edge_count > 1u)
                return true;
        }
    }

    std::map<std::set<VertexHandle>, unsigned> face_counts;
    for(FaceHandle fh : _mesh.faces()) {
        if(_mesh.is_deleted(fh) || _mesh.valence(fh) != 3u)
            continue;
        std::set<VertexHandle> fvs;
        bool inside = true;
        for(VertexHandle vh : _mesh.get_halfface_vertices(_mesh.halfface_handle(fh, 0))) {
            if(std::find(_vertices.begin(), _vertices.end(), vh) == _vertices.end()) {
                inside = false;
                break;
            }
            fvs.insert(vh);
        }
        if(inside && fvs.size() == 3u)
            ++face_counts[fvs];
    }
    for(const auto& entry : face_counts) {
        if(entry.second > 1u)
            return true;
    }
    return false;
}

} // anonymous namespace

//========================================================================================

bool TetrahedralMeshTopologyKernel::plan_flip23(FaceHandle _fh, FlipPlan& _plan) const
{
    _plan = FlipPlan{};
    _plan.kind = FlipPlan::Kind::Flip23;

    if(!deferred_deletion_enabled() || !has_full_bottom_up_incidences())
        return false;

    if(!is_valid(_fh) || is_deleted(_fh) || valence(_fh) != 3u)
        return false;

    // The shared face must not lie on the mesh boundary.
    if(is_boundary(_fh))
        return false;

    const HalfFaceHandle h0 = halfface_handle(_fh, 0);
    const HalfFaceHandle h1 = halfface_handle(_fh, 1);
    const CellHandle c0 = incident_cell(h0);
    const CellHandle c1 = incident_cell(h1);

    // The face has to be interior, shared by two distinct live tets.
    if(!c0.is_valid() || !c1.is_valid() || c0 == c1)
        return false;
    if(is_deleted(c0) || is_deleted(c1))
        return false;
    if(valence(c0) != 4u || valence(c1) != 4u)
        return false;

    const std::vector<VertexHandle> fvs = get_halfface_vertices(h0);
    if(fvs.size() != 3u)
        return false;
    const VertexHandle a = fvs[0], b = fvs[1], c = fvs[2];
    if(a == b || b == c || a == c)
        return false;

    const std::vector<VertexHandle> vs0 = get_cell_vertices(c0);
    const std::vector<VertexHandle> vs1 = get_cell_vertices(c1);
    if(vs0.size() != 4u || vs1.size() != 4u)
        return false;
    if(std::set<VertexHandle>(vs0.begin(), vs0.end()).size() != 4u ||
       std::set<VertexHandle>(vs1.begin(), vs1.end()).size() != 4u)
        return false;

    auto pick_opposite = [&](const std::vector<VertexHandle>& _cell_vs) {
        for(VertexHandle v : _cell_vs)
            if(v != a && v != b && v != c)
                return v;
        return InvalidVertexHandle;
    };
    const VertexHandle d = pick_opposite(vs0);
    const VertexHandle e = pick_opposite(vs1);
    if(!d.is_valid() || !e.is_valid() || d == e)
        return false;
    if(d == a || d == b || d == c || e == a || e == b || e == c)
        return false;

    _plan.vertices = {a, b, c, d, e};
    _plan.old_cells = {c0, c1};
    _plan.old_internal_faces = {_fh};

    // Collect the six shell faces, keeping the halfface that pointed
    // into the cavity.
    std::set<FaceHandle> shell_set;
    for(CellHandle ch : _plan.old_cells) {
        for(HalfFaceHandle hfh : cell(ch).halffaces()) {
            const FaceHandle fh = face_handle(hfh);
            if(fh == _fh)
                continue;
            if(is_deleted(fh))
                return false;
            if(!shell_set.insert(fh).second)
                return false; // two shell faces coincide: non-spherical link
            _plan.shell_faces.push_back({fh, hfh});
        }
    }
    if(_plan.shell_faces.size() != 6u || shell_set.size() != 6u)
        return false;

    // Reject duplicate edges or faces within the cavity.
    if(has_duplicate_simplices(*this, _plan.vertices))
        return false;

    // The new internal edge and the three new internal faces must not
    // already be occupied by live entities.
    if(edge_exists(*this, d, e))
        return false;
    if(face_exists(*this, d, e, a) || face_exists(*this, d, e, b) ||
       face_exists(*this, d, e, c))
        return false;

    // Build the three replacement tets, each oriented through one of
    // the old shell halffaces so that shell orientations are preserved.
    const std::array<std::array<VertexHandle, 3>, 3> shell_triples = {{
        {{a, b, d}}, {{b, c, d}}, {{c, a, d}}
    }};
    for(const auto& tri : shell_triples) {
        const HalfFaceHandle* shell_hfh = nullptr;
        for(const auto& sf : _plan.shell_faces) {
            if(same_vertex_set(get_halfface_vertices(sf.inner_halfface), tri)) {
                shell_hfh = &sf.inner_halfface;
                break;
            }
        }
        if(shell_hfh == nullptr)
            return false;
        const std::vector<VertexHandle> oriented = get_halfface_vertices(*shell_hfh);
        VertexHandle w = InvalidVertexHandle;
        for(VertexHandle v : {d, e})
            if(v != oriented[0] && v != oriented[1] && v != oriented[2])
                w = v;
        if(!w.is_valid())
            return false;
        _plan.new_tets.push_back({oriented[0], oriented[1], oriented[2], w});
    }

    return _plan.new_tets.size() == 3u;
}

//========================================================================================

bool TetrahedralMeshTopologyKernel::plan_flip32(EdgeHandle _eh, FlipPlan& _plan) const
{
    _plan = FlipPlan{};
    _plan.kind = FlipPlan::Kind::Flip32;

    if(!deferred_deletion_enabled() || !has_full_bottom_up_incidences())
        return false;

    if(!is_valid(_eh) || is_deleted(_eh))
        return false;

    // The edge must be interior: flipping a boundary edge would change
    // the mesh boundary instead of only reorganizing a cavity.
    if(is_boundary(_eh))
        return false;

    const VertexHandle u = edge(_eh).from_vertex();
    const VertexHandle v = edge(_eh).to_vertex();

    // Exactly three distinct, live incident cells.
    std::vector<CellHandle> incident;
    for(HalfEdgeCellIter it = hec_iter(halfedge_handle(_eh, 0)); it.valid(); ++it) {
        const CellHandle ch = *it;
        if(is_deleted(ch))
            return false;
        if(std::find(incident.begin(), incident.end(), ch) == incident.end())
            incident.push_back(ch);
    }
    if(incident.size() != 3u)
        return false;

    std::set<VertexHandle> link_set;
    std::set<FaceHandle> internal_set;
    for(CellHandle ch : incident) {
        if(valence(ch) != 4u)
            return false;
        const std::vector<VertexHandle> cvs = get_cell_vertices(ch);
        if(cvs.size() != 4u ||
           std::set<VertexHandle>(cvs.begin(), cvs.end()).size() != 4u)
            return false;
        const bool has_u = std::find(cvs.begin(), cvs.end(), u) != cvs.end();
        const bool has_v = std::find(cvs.begin(), cvs.end(), v) != cvs.end();
        if(!has_u || !has_v)
            return false;
        for(VertexHandle x : cvs)
            if(x != u && x != v)
                link_set.insert(x);
        for(HalfFaceHandle hfh : cell(ch).halffaces()) {
            const FaceHandle fh = face_handle(hfh);
            if(is_deleted(fh))
                return false;
            const std::vector<VertexHandle> fvs = get_halfface_vertices(hfh);
            const bool contains_u = std::find(fvs.begin(), fvs.end(), u) != fvs.end();
            const bool contains_v = std::find(fvs.begin(), fvs.end(), v) != fvs.end();
            if(contains_u && contains_v) {
                if(is_boundary(fh))
                    return false;
                internal_set.insert(fh);
            }
        }
    }

    // Spherical link: three internal faces separating exactly three
    // tets, three distinct link vertices.
    if(internal_set.size() != 3u || link_set.size() != 3u)
        return false;

    for(FaceHandle fh : internal_set) {
        const CellHandle i0 = incident_cell(halfface_handle(fh, 0));
        const CellHandle i1 = incident_cell(halfface_handle(fh, 1));
        if(!i0.is_valid() || !i1.is_valid() || i0 == i1)
            return false;
        if(std::find(incident.begin(), incident.end(), i0) == incident.end() ||
           std::find(incident.begin(), incident.end(), i1) == incident.end())
            return false;
    }

    std::vector<VertexHandle> link(link_set.begin(), link_set.end());
    const VertexHandle a = link[0], b = link[1], c = link[2];
    if(a == u || a == v || b == u || b == v || c == u || c == v)
        return false;

    _plan.vertices = {u, v, a, b, c};
    _plan.old_cells = incident;
    _plan.old_internal_edge = _eh;
    _plan.old_internal_faces.assign(internal_set.begin(), internal_set.end());

    // Six shell faces.
    std::set<FaceHandle> shell_set;
    for(CellHandle ch : incident) {
        for(HalfFaceHandle hfh : cell(ch).halffaces()) {
            const FaceHandle fh = face_handle(hfh);
            if(internal_set.count(fh) != 0u)
                continue;
            if(!shell_set.insert(fh).second)
                return false;
            _plan.shell_faces.push_back({fh, hfh});
        }
    }
    if(_plan.shell_faces.size() != 6u || shell_set.size() != 6u)
        return false;

    // Reject duplicate edges or faces within the cavity.
    if(has_duplicate_simplices(*this, _plan.vertices))
        return false;

    // The link edges have to exist (they are shell edges) and the
    // candidate internal face must not be occupied by a live face.
    if(!edge_exists(*this, a, b) || !edge_exists(*this, b, c) ||
       !edge_exists(*this, c, a))
        return false;
    if(face_exists(*this, a, b, c))
        return false;

    // Two replacement tets, each oriented through one of its old shell
    // halffaces. The u-tet is oriented by a face containing u, the
    // v-tet by one containing v.
    for(VertexHandle apex : {u, v}) {
        const HalfFaceHandle* shell_hfh = nullptr;
        for(const auto& sf : _plan.shell_faces) {
            const std::vector<VertexHandle> tri = get_halfface_vertices(sf.inner_halfface);
            const bool has_apex = std::find(tri.begin(), tri.end(), apex) != tri.end();
            if(has_apex) {
                shell_hfh = &sf.inner_halfface;
                break;
            }
        }
        if(shell_hfh == nullptr)
            return false;
        const std::vector<VertexHandle> oriented = get_halfface_vertices(*shell_hfh);
        VertexHandle w = InvalidVertexHandle;
        for(VertexHandle x : link)
            if(x != oriented[0] && x != oriented[1] && x != oriented[2])
                w = x;
        if(!w.is_valid())
            return false;
        _plan.new_tets.push_back({oriented[0], oriented[1], oriented[2], w});
    }

    return _plan.new_tets.size() == 2u;
}

//========================================================================================

bool TetrahedralMeshTopologyKernel::execute_flip(const FlipPlan& _plan,
                                                 std::vector<CellHandle>& _created)
{
    // 1) Remove the old cells (deferred: only flags + incidence updates).
    for(CellHandle ch : _plan.old_cells)
        delete_cell(ch);

    // 2) Remove the old internal simplices. For a 32 flip the edge is
    //    deleted non-cascadingly, since its faces and cells were
    //    removed above and shell faces must stay untouched.
    for(FaceHandle fh : _plan.old_internal_faces)
        delete_face(fh);
    if(_plan.kind == FlipPlan::Kind::Flip32)
        delete_detached_edge(_plan.old_internal_edge);

    // 3) Create the new internal edge of a 23 flip.
    if(_plan.kind == FlipPlan::Kind::Flip23) {
        const VertexHandle d = _plan.vertices[3];
        const VertexHandle e = _plan.vertices[4];
        add_edge(d, e);
    }

    // 4) Create the replacement tets. Existing shell halffaces are
    //    reused, new internal faces are created with default properties.
    CellHandle property_template = _plan.old_cells.front();
    for(CellHandle ch : _plan.old_cells)
        if(ch < property_template)
            property_template = ch;

    std::vector<CellHandle> created;
    created.reserve(_plan.new_tets.size());
    for(const auto& tet : _plan.new_tets) {
        CellHandle ch = add_cell(tet[0], tet[1], tet[2], tet[3]);
        if(!ch.is_valid())
            return false;
        created.push_back(ch);
        // New cells inherit every cell property of the smallest-handled
        // removed cell, including caller-defined property types.
        copy_property_elements(property_template, ch);
    }

    std::sort(created.begin(), created.end());
    _created = std::move(created);
    return true;
}

//========================================================================================

bool TetrahedralMeshTopologyKernel::flip23(FaceHandle _fh, std::vector<CellHandle>& _created)
{
    FlipPlan plan;
    if(!plan_flip23(_fh, plan))
        return false;
    return execute_flip(plan, _created);
}

//========================================================================================

bool TetrahedralMeshTopologyKernel::flip32(EdgeHandle _eh, std::vector<CellHandle>& _created)
{
    FlipPlan plan;
    if(!plan_flip32(_eh, plan))
        return false;
    return execute_flip(plan, _created);
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
