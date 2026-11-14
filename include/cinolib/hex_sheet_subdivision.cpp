/********************************************************************************
*  This file is part of CinoLib                                                 *
*  Copyright(C) 2016: Marco Livesu                                              *
*                                                                               *
*  The MIT License                                                              *
*                                                                               *
*  Permission is hereby granted, free of charge, to any person obtaining a      *
*  copy of this software and associated documentation files (the "Software"),   *
*  to deal in the Software without restriction, including without limitation    *
*  the rights to use, copy, modify, merge, publish, distribute, sublicense,     *
*  and/or sell copies of the Software, and to permit persons to whom the        *
*  Software is furnished to do so, subject to the following conditions:         *
*                                                                               *
*  The above copyright notice and this permission notice shall be included in   *
*  all copies or substantial portions of the Software.                          *
*                                                                               *
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR   *
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,     *
*  FITNESS FOR A PARTICULAR PURPOSE AND NON INFRINGEMENT. IN NO EVENT SHALL THE *
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER       *
*  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING      *
*  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS *
*  IN THE SOFTWARE.                                                             *
*                                                                               *
*  Author(s):                                                                   *
*                                                                               *
*     Marco Livesu (marco.livesu@gmail.com)                                     *
*     http://pers.ge.imati.cnr.it/livesu/                                       *
*                                                                               *
*     Italian National Research Council (CNR)                                   *
*     Institute for Applied Mathematics and Information Technologies (IMATI)    *
*     Via de Marini, 6                                                          *
*     16149 Genoa,                                                              *
*     Italy                                                                     *
*********************************************************************************/
#include <cinolib/hex_sheet_subdivision.h>
#include <cinolib/standard_elements_tables.h>

#include <array>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace cinolib
{

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
//  LOCAL HELPERS
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

namespace
{

// In a quad face, the edge opposite to eid is the unique face edge sharing no
// vertex with it. Purely topological: no world coordinate is ever involved.
template<class Mesh>
CINO_INLINE
uint opposite_face_edge(const Mesh & m, const uint fid, const uint eid)
{
    for(uint e2 : m.adj_f2e(fid))
    {
        if(e2 != eid && !m.edges_are_adjacent(eid, e2)) return e2;
    }
    assert(false);
    return 0; // warning killer
}

// Canonical hexa edge axis (0=x, 1=y, 2=z) from the canonical edge offset.
CINO_INLINE
uint canonical_edge_axis(const uint ce)
{
    // HEXA_EDGES: x directed edges are {0,2,4,6}, y directed {1,3,5,7},
    // z directed {8,9,10,11}
    if(ce==0 || ce==2 || ce==4 || ce==6) return 0;
    if(ce==1 || ce==3 || ce==5 || ce==7) return 1;
    return 2;
}

// Canonical hexa corner index associated with grid coordinates. The canonical
// ordering is ring based on each quad (v0,v1,v2,v3 = (0,0),(1,0),(1,1),(0,1)),
// not binary i + 2j.
CINO_INLINE
uint corner_index(const uint gi, const uint gj, const uint gk)
{
    const uint i = gi/2, j = gj/2, k = gk/2;
    const uint ring = (i==0) ? (j==0 ? 0u : 3u) : (j==0 ? 1u : 2u);
    return 4u*k + ring;
}

// Weighted average of a color.
template<class V>
CINO_INLINE
Color interp_color(const std::vector<V> & vdata,
                   const std::vector<std::pair<uint,double>> & src)
{
    float r=0,g=0,b=0,a=0;
    for(auto [vid,w] : src)
    {
        const Color & c = vdata.at(vid).color;
        r += float(c.r*w); g += float(c.g*w); b += float(c.b*w); a += float(c.a*w);
    }
    return Color(r,g,b,a);
}

// Weighted average of a vec3d vertex field.
template<class V>
CINO_INLINE
vec3d interp_vec3(const std::vector<V> & vdata,
                  const std::vector<std::pair<uint,double>> & src)
{
    vec3d acc(0,0,0);
    for(auto [vid,w] : src) acc += vdata.at(vid).uvw * w;
    return acc;
}
// Everything the rebuild phase needs, fully expressed with input mesh ids.
// Output vertex ids are assigned here, so once a plan is built the input mesh
// can be cleared and rebuilt even when input and output are the same object.
template<class M, class V, class E, class F, class P>
struct SheetPlan
{
    const Hexmesh<M,V,E,F,P>                       & m_in;
    std::vector<vec3d>                               coords;   // output vertex positions
    std::vector<std::vector<std::pair<uint,double>>> sources;  // output vertex sources
    std::vector<std::vector<uint>>                   polys;    // output hexahedra (8 vids, canonical order)
    std::vector<uint>                                parent;   // parent pid per output hexahedron
    std::map<std::vector<uint>,int>                  face_src; // sorted face vids -> input fid (-1 if new)
    std::map<uint,uint>                              edge_verts;
    std::map<uint,uint>                              face_verts;
    std::map<uint,uint>                              poly_verts;

    explicit SheetPlan(const Hexmesh<M,V,E,F,P> & m) : m_in(m)
    {
        coords.reserve(m.num_verts());
        sources.reserve(m.num_verts());
        for(uint vid=0; vid<m.num_verts(); ++vid)
        {
            coords.push_back(m.vert(vid));
            sources.push_back({{vid,1.0}});
        }
    }

    uint add_node(const std::vector<std::pair<uint,double>> & src)
    {
        vec3d p(0,0,0);
        for(auto [vid,w] : src) p += m_in.vert(vid) * w;
        const uint nid = uint(coords.size());
        coords.push_back(p);
        sources.push_back(src);
        return nid;
    }

    // Output id of the grid node at (gi,gj,gk) of parent cell pid. A node with
    // one, two or three mid coordinates resolves respectively to an edge
    // midpoint, a quad face center or the cell center.
    uint node_id(const uint pid, const uint gi, const uint gj, const uint gk)
    {
        const uint n_mid = (gi==1) + (gj==1) + (gk==1);

        if(n_mid == 0)
        {
            return m_in.poly_vert_id(pid, corner_index(gi,gj,gk));
        }

        if(n_mid == 1)
        {
            uint va=0, vb=0;
            if(gi==1)      { va = m_in.poly_vert_id(pid, corner_index(0,gj,gk)); vb = m_in.poly_vert_id(pid, corner_index(2,gj,gk)); }
            else if(gj==1) { va = m_in.poly_vert_id(pid, corner_index(gi,0,gk)); vb = m_in.poly_vert_id(pid, corner_index(gi,2,gk)); }
            else           { va = m_in.poly_vert_id(pid, corner_index(gi,gj,0)); vb = m_in.poly_vert_id(pid, corner_index(gi,gj,2)); }
            int eid = m_in.edge_id(va,vb);
            assert(eid >= 0);
            return edge_verts.at(uint(eid));
        }

        if(n_mid == 2)
        {
            const uint fixed[3] = {gi, gj, gk};
            std::vector<uint> corners;
            for(uint mask=0; mask<4; ++mask)
            {
                uint g[3];
                uint bit = 0;
                for(uint d=0; d<3; ++d)
                {
                    if(fixed[d] == 1) g[d] = ((mask >> bit++) & 1u) ? 2u : 0u;
                    else              g[d] = fixed[d];
                }
                corners.push_back(m_in.poly_vert_id(pid, corner_index(g[0],g[1],g[2])));
            }
            int fid = m_in.face_id(corners);
            assert(fid >= 0);
            return face_verts.at(uint(fid));
        }

        return poly_verts.at(pid);
    }
};

} // anonymous namespace

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
//  Hexmesh::subdivide_sheets
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

template<class M, class V, class E, class F, class P>
CINO_INLINE
HexSheetSubdivision Hexmesh<M,V,E,F,P>::subdivide_sheets(const std::vector<uint> & marked_edges)
{
    HexSheetSubdivision result;

    for(uint eid : marked_edges)
    {
        if(eid >= this->num_edges())
            throw std::invalid_argument("subdivide_sheets: edge id out of range");
    }
    if(marked_edges.empty()) return result;

    //----------------------------------------------------------------------
    // 1) Sheet closure. From every selected edge, cross each incident quad
    //    through its topologically opposite edge. The relation never relies
    //    on world-space parallelism, so rotated local numberings, curved
    //    sheets and sheets returning to the same cell are handled.
    //----------------------------------------------------------------------
    std::unordered_set<uint> hit_edges(marked_edges.begin(), marked_edges.end());
    {
        std::queue<uint> q;
        for(uint eid : hit_edges) q.push(eid);
        while(!q.empty())
        {
            const uint eid = q.front();
            q.pop();
            for(uint pid : this->adj_e2p(eid))
            for(uint fid : this->poly_e2f(pid,eid))
            {
                const uint e2 = opposite_face_edge(*this, fid, eid);
                if(hit_edges.insert(e2).second) q.push(e2);
            }
        }
    }

    //----------------------------------------------------------------------
    // 2) Per cell, map each canonical hexa edge to its global edge id and
    //    record which of the three canonical axes are hit. Opposite edges of
    //    a quad share the same canonical axis, so no world geometry is used.
    //----------------------------------------------------------------------
    std::vector<std::array<bool,3>> cell_split(this->num_polys());
    for(uint pid=0; pid<this->num_polys(); ++pid)
    {
        for(uint ce=0; ce<12; ++ce)
        {
            uint va = this->poly_vert_id(pid, HEXA_EDGES[ce][0]);
            uint vb = this->poly_vert_id(pid, HEXA_EDGES[ce][1]);
            int  eid = this->edge_id(va,vb);
            assert(eid >= 0);
            if(hit_edges.count(uint(eid)))
                cell_split[pid][canonical_edge_axis(ce)] = true;
        }
    }

    //----------------------------------------------------------------------
    // 3) Build the full output plan before touching the mesh.
    //----------------------------------------------------------------------
    SheetPlan<M,V,E,F,P> plan(*this);

    // 3a) edge midpoint nodes (input edge id order makes new ids deterministic
    //     and independent of the order/duplication of the marked edges)
    for(uint eid=0; eid<this->num_edges(); ++eid)
    {
        if(!hit_edges.count(eid)) continue;
        std::vector<uint> ev = this->edge_vert_ids(eid);
        plan.edge_verts[eid] = plan.add_node({{ev[0],0.5},{ev[1],0.5}});
    }
    // 3b) quad face centers: a face gets a center iff all four of its edges
    //     are cut (both in-plane local directions are split)
    for(uint fid=0; fid<this->num_faces(); ++fid)
    {
        uint n_hit = 0;
        for(uint eid : this->adj_f2e(fid)) if(hit_edges.count(eid)) ++n_hit;
        if(n_hit != 4) continue;
        std::vector<std::pair<uint,double>> src;
        for(uint vid : this->adj_f2v(fid)) src.push_back({vid,0.25});
        plan.face_verts[fid] = plan.add_node(src);
    }
    // 3c) cell centers for the fully split cells
    for(uint pid=0; pid<this->num_polys(); ++pid)
    {
        const uint nd = uint(cell_split[pid][0]) +
                        uint(cell_split[pid][1]) +
                        uint(cell_split[pid][2]);
        if(nd != 3) continue;
        std::vector<std::pair<uint,double>> src;
        for(uint off=0; off<8; ++off) src.push_back({this->poly_vert_id(pid,off),0.125});
        plan.poly_verts[pid] = plan.add_node(src);
    }

    // 3d) child hexahedra and child-face classification, parents processed in
    //     ascending id and children emitted in lexicographic (i,j,k) order
    std::map<std::vector<uint>,int> face_src;
    for(uint pid=0; pid<this->num_polys(); ++pid)
    {
        const auto & split = cell_split[pid];

        // parent face on the low/high side of each canonical axis
        std::array<std::array<uint,2>,3> pf;
        static const uint SIDE_CORNERS[3][2][4] =
        {
            {{0,3,4,7},{1,2,5,6}}, // axis x
            {{0,1,4,5},{2,3,6,7}}, // axis y
            {{0,1,2,3},{4,5,6,7}}  // axis z
        };
        for(uint d=0; d<3; ++d)
        for(uint s=0; s<2; ++s)
        {
            std::vector<uint> corners(4);
            for(uint i=0; i<4; ++i)
                corners[i] = this->poly_vert_id(pid, SIDE_CORNERS[d][s][i]);
            int fid = this->face_id(corners);
            assert(fid >= 0);
            pf[d][s] = uint(fid);
        }

        const std::array<std::vector<uint>,3> axis =
        {{
            split[0] ? std::vector<uint>{0,1,2} : std::vector<uint>{0,2},
            split[1] ? std::vector<uint>{0,1,2} : std::vector<uint>{0,2},
            split[2] ? std::vector<uint>{0,1,2} : std::vector<uint>{0,2}
        }};

        for(uint ka=0; ka+1<axis[2].size(); ++ka)
        for(uint ja=0; ja+1<axis[1].size(); ++ja)
        for(uint ia=0; ia+1<axis[0].size(); ++ia)
        {
            const uint ilo=axis[0][ia], ihi=axis[0][ia+1];
            const uint jlo=axis[1][ja], jhi=axis[1][ja+1];
            const uint klo=axis[2][ka], khi=axis[2][ka+1];

            std::vector<uint> hex(8);
            for(uint k=0; k<2; ++k)
            for(uint j=0; j<2; ++j)
            for(uint i=0; i<2; ++i)
            {
                // canonical hexa ordering is ring based within each quad,
                // not binary i + 2j + 4k
                const uint ring = (i==0) ? (j==0 ? 0u : 3u) : (j==0 ? 1u : 2u);
                hex[4u*k + ring] =
                    plan.node_id(pid, i ? ihi : ilo, j ? jhi : jlo, k ? khi : klo);
            }
            plan.polys.push_back(hex);
            plan.parent.push_back(pid);

            // classify the six child faces
            const uint lo[3] = {ilo,jlo,klo};
            const uint hi[3] = {ihi,jhi,khi};
            for(uint d=0; d<3; ++d)
            for(uint s=0; s<2; ++s)
            {
                const uint fixed = s ? hi[d] : lo[d];
                std::vector<uint> fv;
                for(uint b=0; b<2; ++b)
                for(uint a=0; a<2; ++a)
                {
                    uint g[3];
                    g[d] = fixed;
                    if(d==0)      { g[1] = a ? jhi : jlo; g[2] = b ? khi : klo; }
                    else if(d==1) { g[0] = a ? ihi : ilo; g[2] = b ? khi : klo; }
                    else          { g[0] = a ? ihi : ilo; g[1] = b ? jhi : jlo; }
                    fv.push_back(plan.node_id(pid,g[0],g[1],g[2]));
                }
                std::sort(fv.begin(), fv.end());
                if(face_src.find(fv)==face_src.end())
                    face_src[fv] = (fixed==1) ? -1 : int(pf[d][s]);
            }
        }
    }
    plan.face_src = std::move(face_src);

    //----------------------------------------------------------------------
    // 4) Snapshot the data that must survive, then rebuild the mesh.
    //----------------------------------------------------------------------
    const uint nv_in = this->num_verts();
    std::vector<V> old_v_data(nv_in);
    for(uint vid=0; vid<nv_in; ++vid) old_v_data[vid] = this->vert_data(vid);
    std::vector<F> old_f_data(this->num_faces());
    for(uint fid=0; fid<this->num_faces(); ++fid) old_f_data[fid] = this->face_data(fid);
    std::vector<P> old_p_data(this->num_polys());
    for(uint pid=0; pid<this->num_polys(); ++pid) old_p_data[pid] = this->poly_data(pid);
    const M old_m_data = this->mesh_data();

    this->clear();

    // 4a) vertices: input ids are preserved; new nodes append in plan order
    for(uint vid=0; vid<plan.coords.size(); ++vid)
    {
        this->vert_add(plan.coords.at(vid));
        if(vid < nv_in)
        {
            this->vert_data(vid) = old_v_data.at(vid);
        }
        else
        {
            const auto & src = plan.sources.at(vid);
            V data; // every non interpolated field keeps its type default
            data.uvw   = interp_vec3(old_v_data, src);
            data.color = interp_color(old_v_data, src);
            this->vert_data(vid) = data;
        }
    }

    // 4b) hexahedra: poly_add reuses an already registered topological face
    //     instead of duplicating it, and recomputes the per-poly face winding
    //     from geometry, so the two sides of an interior face always get
    //     opposite windings; no hanging nodes are possible.
    for(uint i=0; i<plan.polys.size(); ++i)
    {
        uint pid = this->poly_add(plan.polys.at(i));
        this->poly_data(pid) = old_p_data.at(plan.parent.at(i));
    }

    // 4c) face attributes: sub faces of an input face inherit its attributes,
    //     brand new internal faces keep the default attribute set
    for(uint fid=0; fid<this->num_faces(); ++fid)
    {
        std::vector<uint> key = this->adj_f2v(fid);
        std::sort(key.begin(), key.end());
        auto it = plan.face_src.find(key);
        if(it != plan.face_src.end() && it->second >= 0)
            this->face_data(fid) = old_f_data.at(uint(it->second));
    }

    this->mesh_data() = old_m_data;
    this->update_normals();
    this->update_quality();
    this->update_bbox();

    //----------------------------------------------------------------------
    // 5) Result expressed with the numbering in effect before the call.
    //----------------------------------------------------------------------
    result.parent_poly    = plan.parent;
    result.vertex_sources = plan.sources;
    return result;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
//  Midpoint hexahedral branch
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

template<class M, class V, class E, class F, class P>
CINO_INLINE
void subdivision_midpoint_hexa(const Hexmesh<M,V,E,F,P> & m_in,
                                     Hexmesh<M,V,E,F,P> & m_out,
                                     std::map<uint,uint> & edge_verts,
                                     std::map<uint,uint> & face_verts,
                                     std::map<uint,uint> & poly_verts)
{
    edge_verts.clear();
    face_verts.clear();
    poly_verts.clear();

    // Snapshot the input topology and size: for in-place calls m_in is the
    // very object that is about to be rewritten.
    const uint nv0 = m_in.num_verts();
    std::vector<std::set<uint>> edge_v(m_in.num_edges());
    for(uint eid=0; eid<m_in.num_edges(); ++eid)
    {
        std::vector<uint> ev = m_in.edge_vert_ids(eid);
        edge_v[eid] = std::set<uint>(ev.begin(), ev.end());
    }
    std::vector<std::set<uint>> face_v(m_in.num_faces());
    for(uint fid=0; fid<m_in.num_faces(); ++fid)
    {
        std::vector<uint> fv = m_in.adj_f2v(fid);
        face_v[fid] = std::set<uint>(fv.begin(), fv.end());
    }
    std::vector<std::set<uint>> poly_v(m_in.num_polys());
    for(uint pid=0; pid<m_in.num_polys(); ++pid)
    {
        std::vector<uint> pv = m_in.adj_p2v(pid);
        poly_v[pid] = std::set<uint>(pv.begin(), pv.end());
    }

    std::vector<uint> all_edges;
    all_edges.reserve(m_in.num_edges());
    for(uint eid=0; eid<m_in.num_edges(); ++eid) all_edges.push_back(eid);

    HexSheetSubdivision info;
    if(&m_in == &m_out)
    {
        info = m_out.subdivide_sheets(all_edges);
    }
    else
    {
        m_out = m_in;
        info = m_out.subdivide_sheets(all_edges);
    }

    // recover the three maps by matching the source vertex sets
    for(uint vid=nv0; vid<info.vertex_sources.size(); ++vid)
    {
        const auto & src = info.vertex_sources.at(vid);
        std::set<uint> s;
        for(auto p : src) s.insert(p.first);

        if(src.size()==2)
        {
            for(uint eid=0; eid<edge_v.size(); ++eid)
                if(s==edge_v[eid]) { edge_verts[eid]=vid; break; }
        }
        else if(src.size()==4)
        {
            for(uint fid=0; fid<face_v.size(); ++fid)
                if(s==face_v[fid]) { face_verts[fid]=vid; break; }
        }
        else if(src.size()==8)
        {
            for(uint pid=0; pid<poly_v.size(); ++pid)
                if(s==poly_v[pid]) { poly_verts[pid]=vid; break; }
        }
    }
}

} // namespace cinolib
