#include <cinolib/meshes/hexmesh.h>
#include <cinolib/hex_sheet_subdivision.h>
#include <cinolib/subdivision_midpoint.h>
#include <cinolib/subdivision_legacy_hexa_schemes.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

// check.h ships a C-compatible interface; the task runner compiles every
// translation unit with a C++ compiler, so ordinary C++ linkage is correct.
#include "task_support/check.h"

using namespace cinolib;

namespace
{

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// mesh builders
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

// axis aligned nx*ny*nz hexa grid, canonical hexa vertex ordering
Hexmesh<> make_grid(const uint nx, const uint ny, const uint nz,
                    const double ox=0.0, const double oy=0.0, const double oz=0.0)
{
    const uint sx = nx+1, sy = ny+1;
    auto vid = [&](const uint x,const uint y,const uint z){ return x + sx*y + sx*sy*z; };
    std::vector<vec3d> verts;
    for(uint z=0;z<=nz;++z)
    for(uint y=0;y<=ny;++y)
    for(uint x=0;x<=nx;++x) verts.emplace_back(ox+x, oy+y, oz+z);

    std::vector<std::vector<uint>> polys;
    for(uint z=0;z<nz;++z)
    for(uint y=0;y<ny;++y)
    for(uint x=0;x<nx;++x)
    {
        polys.push_back({vid(x,y,z), vid(x+1,y,z), vid(x+1,y+1,z), vid(x,y+1,z),
                         vid(x,y,z+1), vid(x+1,y,z+1), vid(x+1,y+1,z+1), vid(x,y+1,z+1)});
    }
    return Hexmesh<>(verts, polys);
}

uint find_edge(const Hexmesh<> & m, const uint a, const uint b)
{
    int e = m.edge_id(a,b);
    return e < 0 ? m.num_edges() : uint(e);
}

template<class Mesh>
std::vector<uint> all_edges(const Mesh & m)
{
    std::vector<uint> e;
    e.reserve(m.num_edges());
    for(uint i=0;i<m.num_edges();++i) e.push_back(i);
    return e;
}

// independent oriented volume: determinant of the three edges incident to v0
double oriented_volume(const Hexmesh<> & m, const uint pid)
{
    const vec3d e1 = m.poly_vert(pid,1) - m.poly_vert(pid,0);
    const vec3d e3 = m.poly_vert(pid,3) - m.poly_vert(pid,0);
    const vec3d e4 = m.poly_vert(pid,4) - m.poly_vert(pid,0);
    return e1.dot(e3.cross(e4));
}

double total_oriented_volume(const Hexmesh<> & m)
{
    double v = 0.0;
    for(uint p=0;p<m.num_polys();++p) v += oriented_volume(m,p);
    return v;
}

// sorted multiset of each poly's corners, as comparable strings
std::set<std::vector<uint>> poly_corner_sets(const Hexmesh<> & m)
{
    std::set<std::vector<uint>> out;
    for(uint p=0;p<m.num_polys();++p)
    {
        std::vector<uint> c = m.adj_p2v(p);
        std::sort(c.begin(), c.end());
        out.insert(c);
    }
    return out;
}

std::vector<vec3d> sorted_positions(const Hexmesh<> & m)
{
    std::vector<vec3d> p;
    for(uint v=0;v<m.num_verts();++v) p.push_back(m.vert(v));
    std::sort(p.begin(), p.end(), [](const vec3d & a, const vec3d & b)
    {
        if(a.x()!=b.x()) return a.x()<b.x();
        if(a.y()!=b.y()) return a.y()<b.y();
        return a.z()<b.z();
    });
    return p;
}

// every interior face has opposite winding on its two incident cells
bool interior_faces_opposite(const Hexmesh<> & m)
{
    for(uint f=0;f<m.num_faces();++f)
    {
        const auto & ps = m.adj_f2p(f);
        if(ps.size()!=2) continue;
        if(m.poly_face_is_CCW(ps[0],f) == m.poly_face_is_CCW(ps[1],f)) return false;
    }
    return true;
}

// custom attribute types, extending the standard ones
struct MyV : public Vert_std_attributes { int tag = -77; };
struct MyF : public Polygon_std_attributes { int face_tag = -99; };
struct MyP : public Polyhedron_std_attributes { int cell_tag = -123; };

} // anonymous namespace

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// SHEETS group: propagation semantics
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

static int sheet_strip_propagation()
{
    Hexmesh<> m = make_grid(4,1,1);
    const uint nv = m.num_verts(), nf = m.num_faces();
    // y directed edge at the first corner: (0,0,0)-(0,1,0), ids 0 and 5
    const uint seed = find_edge(m, 0, 5);
    TASK_CHECK(seed < m.num_edges());
    TASK_CHECK(m.poly_sheet(seed).size() == 4);

    HexSheetSubdivision r = m.subdivide_sheets({seed});
    TASK_CHECK(m.num_polys() == 8);                 // every parent cut once -> 2
    TASK_CHECK(r.parent_poly.size() == 8);
    for(uint p=0;p<4;++p)
    {
        uint cnt = 0;
        for(uint par : r.parent_poly) if(par==p) ++cnt;
        TASK_CHECK(cnt == 2);
    }
    // no new cell centers or face centers for a single direction cut
    for(const auto & src : r.vertex_sources) TASK_CHECK(src.size() <= 2);
    TASK_CHECK(r.vertex_sources.size() == m.num_verts());
    TASK_CHECK(nv == 20 && nf == 21);
    return 0;
}

static int sheet_cardinalities_and_meeting()
{
    // two adjacent cells along x
    Hexmesh<> m = make_grid(2,1,1);

    // one direction: y sheet crosses both cells -> 2 + 2 = 4 children
    {
        Hexmesh<> t = m;
        const uint seed = find_edge(t, 0, 3); // y edge at origin
        TASK_CHECK(seed < t.num_edges());
        t.subdivide_sheets({seed});
        TASK_CHECK(t.num_polys() == 4);
    }

    // two directions meeting on the first cell: y sheet (both cells) plus
    // x sheet confined to the first cell -> 4 + 2 = 6 children
    {
        Hexmesh<> t = m;
        const uint ey = find_edge(t, 0, 3);
        const uint ex = find_edge(t, 0, 1);
        TASK_CHECK(ey < t.num_edges() && ex < t.num_edges());
        t.subdivide_sheets({ey, ex});
        TASK_CHECK(t.num_polys() == 6);
        // the x/y intersection cell has 4 children, its neighbor only 2
    }

    // three directions meeting inside a 2x2x2 grid: an x slab, a y slab and a
    // z slab intersect only in cell (0,0,0); the opposite corner cell is not
    // hit by any sheet and is kept whole.
    {
        Hexmesh<> g = make_grid(2,2,2); // ids: v(x,y,z) = x + 3y + 9z
        const uint ex = find_edge(g, 0, 1);  // x directed
        const uint ey = find_edge(g, 0, 3);  // y directed
        const uint ez = find_edge(g, 0, 9);  // z directed
        TASK_CHECK(ex < g.num_edges() && ey < g.num_edges() && ez < g.num_edges());

        const std::vector<uint> old_corners = g.adj_p2v(7); // cell (1,1,1)
        HexSheetSubdivision r = g.subdivide_sheets({ey, ex, ez});

        // per parent child counts, pid = x + 2y + 4z:
        // 8 (3 dirs), 4 (y,z), 4 (x,z), 2 (z), 4 (x,y), 2 (y), 2 (x), 1 (none)
        const uint expected[8] = {8,4,4,2,4,2,2,1};
        uint total = 0;
        for(uint pid=0; pid<8; ++pid)
        {
            uint cnt = 0;
            for(uint par : r.parent_poly) if(par==pid) ++cnt;
            TASK_CHECK(cnt == expected[pid]);
            total += cnt;
        }
        TASK_CHECK(g.num_polys() == total && total == 27);

        // the fully split parent produced a cell center node with 8 sources
        bool has_center = false;
        for(const auto & src : r.vertex_sources)
        {
            if(src.size()==8)
            {
                has_center = true;
                for(auto pr : src) TASK_CHECK(std::fabs(pr.second-0.125)<1e-12);
            }
        }
        TASK_CHECK(has_center);

        // the untouched parent survives as a single child on the same corners
        bool far_kept = false;
        const std::set<uint> want(old_corners.begin(), old_corners.end());
        for(uint p=0;p<g.num_polys();++p)
        {
            if(r.parent_poly.at(p) != 7) continue;
            far_kept = true;
            const std::vector<uint> c = g.adj_p2v(p);
            TASK_CHECK(std::set<uint>(c.begin(),c.end()) == want);
        }
        TASK_CHECK(far_kept);
    }
    return 0;
}

static int sheet_distant_component()
{
    Hexmesh<> strip = make_grid(4,1,1);
    std::vector<vec3d> verts = strip.vector_verts();
    const uint base = verts.size();
    // topologically independent, spatially remote second component
    Hexmesh<> far = make_grid(1,1,1, 100.0, 100.0, 100.0);
    for(const vec3d & p : far.vector_verts()) verts.push_back(p);
    std::vector<std::vector<uint>> polys;
    for(uint p=0;p<strip.num_polys();++p) polys.push_back(strip.adj_p2v(p));
    std::vector<uint> far_corners = far.adj_p2v(0);
    for(uint & v : far_corners) v += base;
    polys.push_back(far_corners);
    Hexmesh<> m(verts, polys);
    TASK_CHECK(m.num_polys() == 5);

    const uint seed = find_edge(m, 0, 5); // y edge of the strip component
    TASK_CHECK(seed < m.num_edges());
    HexSheetSubdivision r = m.subdivide_sheets({seed});
    TASK_CHECK(m.num_polys() == 9); // 4*2 refined + 1 untouched
    bool far_kept = false;
    for(uint p=0;p<m.num_polys();++p)
    {
        if(r.parent_poly.at(p) == 4)
        {
            far_kept = true;
            std::vector<uint> c = m.adj_p2v(p);
            std::set<uint> got(c.begin(), c.end()), want(far_corners.begin(), far_corners.end());
            TASK_CHECK(got == want);
        }
    }
    TASK_CHECK(far_kept);
    return 0;
}

static int sheet_order_duplicates_empty_invalid()
{
    Hexmesh<> m = make_grid(2,1,1);
    const uint ey = find_edge(m,0,3), ex = find_edge(m,0,1), ez = find_edge(m,0,6);

    Hexmesh<> a = m; a.subdivide_sheets({ez, ey, ex});
    Hexmesh<> b = m; b.subdivide_sheets({ex, ex, ey, ez, ey}); // duplicates + shuffled
    TASK_CHECK(a.num_verts()==b.num_verts() && a.num_faces()==b.num_faces() &&
               a.num_polys()==b.num_polys() && a.num_edges()==b.num_edges());
    TASK_CHECK(poly_corner_sets(a) == poly_corner_sets(b));

    Hexmesh<> c = m;
    HexSheetSubdivision r = c.subdivide_sheets({});
    TASK_CHECK(c.num_verts()==m.num_verts() && c.num_polys()==m.num_polys());
    TASK_CHECK(r.parent_poly.empty() && r.vertex_sources.empty());

    bool threw = false;
    try { c.subdivide_sheets({c.num_edges()+1}); }
    catch(const std::invalid_argument &) { threw = true; }
    TASK_CHECK(threw);
    return 0;
}

static int sheet_two_successive_refinements()
{
    Hexmesh<> m = make_grid(2,1,1);
    const double vol0 = total_oriented_volume(m);
    const uint seed = find_edge(m, 0, 3);
    m.subdivide_sheets({seed});
    TASK_CHECK(m.num_polys() == 4);

    // second call takes the current mesh as input: cut an x sheet near v0
    const int ex = m.edge_id(0u,1u);
    TASK_CHECK(ex >= 0);
    HexSheetSubdivision r2 = m.subdivide_sheets({uint(ex)});
    TASK_CHECK(!r2.vertex_sources.empty());
    const uint nv_at_call = uint(r2.vertex_sources.size());
    for(const auto & src : r2.vertex_sources)
    {
        double w = 0.0;
        for(auto pr : src)
        {
            TASK_CHECK(pr.second >= 0.0);
            TASK_CHECK(pr.first < nv_at_call);
            w += pr.second;
        }
        TASK_CHECK(std::fabs(w-1.0) < 1e-12);
    }
    for(uint par : r2.parent_poly) TASK_CHECK(par < 4);
    TASK_CHECK(std::fabs(total_oriented_volume(m)-vol0) < 1e-12);
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// TOPOLOGY group: numbering, conformality and geometry
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

static int topology_rotated_local_numbering()
{
    std::vector<vec3d> verts;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<2;++x) verts.emplace_back(x,y,z);
    // orientation preserving 90 degree rotation about z, canonical ring form
    Hexmesh<> m(verts, std::vector<std::vector<uint>>{{1,3,2,0,5,7,6,4}});
    TASK_CHECK(m.poly_data(0).quality > 0.0);
    const double vol0 = total_oriented_volume(m);

    // a single sheet cut must still produce exactly two children
    {
        Hexmesh<> t = m;
        t.subdivide_sheets({0});
        TASK_CHECK(t.num_polys() == 2);
        for(uint p=0;p<t.num_polys();++p) TASK_CHECK(oriented_volume(t,p) > 0.0);
    }
    // full cut produces 8 positive children preserving the volume
    Hexmesh<> t = m;
    t.subdivide_sheets(all_edges(t));
    TASK_CHECK(t.num_polys() == 8);
    for(uint p=0;p<t.num_polys();++p) TASK_CHECK(oriented_volume(t,p) > 0.0);
    TASK_CHECK(std::fabs(total_oriented_volume(t)-vol0) < 1e-12);
    return 0;
}

static int topology_rotated_interface()
{
    // two cells along x; the second cell lists its corners with a rotated
    // local numbering, so the two sides of the shared quad do not align their
    // local slots. Sheet propagation and face reuse must remain topological.
    std::vector<vec3d> v;
    for(int z=0;z<2;++z)for(int y=0;y<2;++y)for(int x=0;x<3;++x) v.emplace_back(x,y,z);
    auto vid = [](const int x,const int y,const int z){ return uint(x+3*y+6*z); };
    const std::vector<uint> A =
        {vid(0,0,0),vid(1,0,0),vid(1,1,0),vid(0,1,0),
         vid(0,0,1),vid(1,0,1),vid(1,1,1),vid(0,1,1)};
    const std::vector<uint> Bbase =
        {vid(1,0,0),vid(2,0,0),vid(2,1,0),vid(1,1,0),
         vid(1,0,1),vid(2,0,1),vid(2,1,1),vid(1,1,1)};
    // 90 degree rotation about x on the canonical corner rings
    const std::vector<uint> perm = {3,0,1,2, 7,4,5,6};
    std::vector<uint> B(8);
    for(int i=0;i<8;++i) B[i] = Bbase[perm[i]];

    Hexmesh<> m(v, std::vector<std::vector<uint>>{A,B});
    TASK_CHECK(m.poly_data(0).quality > 0.0 && m.poly_data(1).quality > 0.0);
    double vol0 = 0.0;
    for(uint p=0;p<m.num_polys();++p) vol0 += m.poly_volume(p);

    // a single y-directed sheet must still propagate through both cells
    const uint seed = find_edge(m, 0, 3);
    TASK_CHECK(seed < m.num_edges());
    Hexmesh<> cut = m;
    cut.subdivide_sheets({seed});
    TASK_CHECK(cut.num_polys() == 4);
    TASK_CHECK(interior_faces_opposite(cut));

    // full cut keeps the rotated interface conforming, with shared faces
    Hexmesh<> full = m;
    full.subdivide_sheets(all_edges(full));
    TASK_CHECK(full.num_polys() == 16);
    TASK_CHECK(interior_faces_opposite(full));
    double vol1 = 0.0;
    for(uint p=0;p<full.num_polys();++p)
    {
        TASK_CHECK(full.poly_data(p).quality > 0.0);
        vol1 += full.poly_volume(p);
    }
    TASK_CHECK(std::fabs(vol0-vol1) < 1e-12);
    return 0;
}

static int topology_conformality_and_winding()
{
    Hexmesh<> m = make_grid(2,1,1);
    const uint ey = find_edge(m,0,3), ex = find_edge(m,0,1);
    m.subdivide_sheets({ey, ex}); // mixed 1 and 2 direction cells

    // every face is manifold (one boundary or two volume cells)
    for(uint f=0;f<m.num_faces();++f)
    {
        TASK_CHECK(m.adj_f2p(f).size() >= 1 && m.adj_f2p(f).size() <= 2);
        // every face vertex belongs to every incident cell
        for(uint pid : m.adj_f2p(f))
            for(uint vid : m.adj_f2v(f)) TASK_CHECK(m.poly_contains_vert(pid,vid));
    }
    TASK_CHECK(interior_faces_opposite(m));

    // Euler characteristic of the ball complex: V - E + F - P == 1
    const int chi = int(m.num_verts()) - int(m.num_edges()) +
                    int(m.num_faces()) - int(m.num_polys());
    TASK_CHECK(chi == 1);

    // full cut on the same base mesh
    Hexmesh<> full = make_grid(2,1,1);
    full.subdivide_sheets(all_edges(full));
    TASK_CHECK(full.num_verts()==45 && full.num_faces()==68 && full.num_polys()==16);
    TASK_CHECK(int(full.num_verts())-int(full.num_edges())+
               int(full.num_faces())-int(full.num_polys()) == 1);
    TASK_CHECK(interior_faces_opposite(full));
    return 0;
}

static int topology_sources_and_parents()
{
    Hexmesh<> m = make_grid(2,1,1);
    const std::vector<vec3d> base_verts = m.vector_verts();
    const uint nv0 = m.num_verts();
    HexSheetSubdivision r = m.subdivide_sheets(all_edges(m));

    TASK_CHECK(r.parent_poly.size() == m.num_polys());
    TASK_CHECK(r.vertex_sources.size() == m.num_verts());
    for(uint par : r.parent_poly) TASK_CHECK(par < 2);

    for(uint v=0;v<m.num_verts();++v)
    {
        const auto & src = r.vertex_sources.at(v);
        TASK_CHECK(!src.empty());
        double w = 0.0;
        vec3d pos(0,0,0);
        for(auto pr : src)
        {
            TASK_CHECK(pr.first < nv0 && pr.second >= 0.0);
            w   += pr.second;
            pos += base_verts.at(pr.first) * pr.second;
        }
        TASK_CHECK(std::fabs(w-1.0) < 1e-12);
        TASK_CHECK((pos - m.vert(v)).norm() < 1e-12);
        if(v < nv0)
        {
            TASK_CHECK(src.size()==1 && src[0].first==v && std::fabs(src[0].second-1.0)<1e-12);
        }
        else if(src.size()==2)
        {
            TASK_CHECK(std::fabs(src[0].second-0.5)<1e-12 && std::fabs(src[1].second-0.5)<1e-12);
        }
        else if(src.size()==4) for(auto pr : src) TASK_CHECK(std::fabs(pr.second-0.25)<1e-12);
        else if(src.size()==8) for(auto pr : src) TASK_CHECK(std::fabs(pr.second-0.125)<1e-12);
        else TASK_CHECK(false);
    }
    return 0;
}

static int topology_oriented_volume()
{
    // affine grids: exact volume conservation for every cut cardinality
    Hexmesh<> m = make_grid(3,2,2);
    const double vol0 = total_oriented_volume(m);
    const uint ey = find_edge(m, 0, 4); // grid stride is 4 along y here
    TASK_CHECK(ey < m.num_edges());
    m.subdivide_sheets({ey});
    for(uint p=0;p<m.num_polys();++p) TASK_CHECK(oriented_volume(m,p) > 0.0);
    TASK_CHECK(std::fabs(total_oriented_volume(m)-vol0) < 1e-12);

    // curved (non affine) but valid trilinear cell: children stay positive,
    // total library volume matches up to the midpoint grid approximation error
    std::vector<vec3d> cv =
    {vec3d(0,0,0),vec3d(1,0,0.05),vec3d(1,1,-0.03),vec3d(0,1,0.02),
     vec3d(0,0,1),vec3d(1.02,0,1),vec3d(1,1,0.97),vec3d(0,0.98,1.04)};
    Hexmesh<> curved(cv, std::vector<std::vector<uint>>{{0,1,2,3,4,5,6,7}});
    TASK_CHECK(curved.poly_data(0).quality > 0.0);
    double cvol = 0.0;
    for(uint p=0;p<curved.num_polys();++p) cvol += curved.poly_volume(p);
    curved.subdivide_sheets(all_edges(curved));
    double svol = 0.0;
    for(uint p=0;p<curved.num_polys();++p)
    {
        TASK_CHECK(curved.poly_data(p).quality > 0.0);
        TASK_CHECK(oriented_volume(curved,p) > 0.0);
        svol += curved.poly_volume(p);
    }
    TASK_CHECK(std::fabs(svol-cvol)/cvol < 1e-3);
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
// ATTRIBUTES group: data propagation and entry points
//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

static int attributes_custom_types_and_sources()
{
    const Hexmesh<> grid = make_grid(2,1,1);
    std::vector<std::vector<uint>> polys;
    for(uint p=0;p<grid.num_polys();++p) polys.push_back(grid.adj_p2v(p));
    Hexmesh<Mesh_std_attributes,MyV,Edge_std_attributes,MyF,MyP> m(grid.vector_verts(), polys);
    for(uint v=0;v<m.num_verts();++v)
    {
        m.vert_data(v).uvw   = vec3d(v, 2.0*v, 3.0*v);
        m.vert_data(v).color = Color(0.1f*v, 0.2f, 0.3f, 0.4f);
        m.vert_data(v).tag   = 1000 + int(v);
    }
    m.poly_data(0).cell_tag = 11;
    m.poly_data(1).cell_tag = 22;
    int boundary_tagged = -1, interface_tagged = -1;
    for(uint f=0;f<m.num_faces();++f)
    {
        if(m.adj_f2p(f).size()==1 && boundary_tagged < 0)
        {
            m.face_data(f).face_tag = 777; boundary_tagged = int(f);
        }
        if(m.adj_f2p(f).size()==2)
        {
            m.face_data(f).face_tag = 444; interface_tagged = int(f);
        }
    }
    TASK_CHECK(boundary_tagged >= 0 && interface_tagged >= 0);

    HexSheetSubdivision r = m.subdivide_sheets(all_edges(m));
    const uint nv0 = 12;
    TASK_CHECK(m.num_verts()==45 && m.num_polys()==16);

    // old vertices keep every field and their position
    for(uint v=0;v<nv0;++v)
    {
        TASK_CHECK(m.vert_data(v).tag == 1000+int(v));
        TASK_CHECK(std::fabs(m.vert_data(v).uvw.y()-2.0*v)<1e-12);
    }
    // new vertices: uvw/color interpolated, custom and remaining fields default
    for(uint v=nv0;v<m.num_verts();++v)
    {
        TASK_CHECK(m.vert_data(v).tag == -77);
        const auto & src = r.vertex_sources.at(v);
        vec3d uvw(0,0,0);
        for(auto pr : src) uvw += vec3d(pr.first, 2.0*pr.first, 3.0*pr.first) * pr.second;
        TASK_CHECK((m.vert_data(v).uvw - uvw).norm() < 1e-10);
    }
    // children copy the parent's user poly attributes
    for(uint p=0;p<m.num_polys();++p)
    {
        const int expected = r.parent_poly.at(p)==0 ? 11 : 22;
        TASK_CHECK(m.poly_data(p).cell_tag == expected);
    }
    // sub faces of input faces inherit the face attribute (both boundary and
    // interface input quads); brand new cut faces keep the default value
    uint boundary_inherited = 0, interface_inherited = 0, internal_default = 0;
    for(uint f=0;f<m.num_faces();++f)
    {
        if(m.face_data(f).face_tag == 777) ++boundary_inherited;
        if(m.face_data(f).face_tag == 444)
        {
            ++interface_inherited;
            // inherited interface sub faces remain shared by two cells
            TASK_CHECK(m.adj_f2p(f).size() == 2);
        }
        if(m.adj_f2p(f).size()==2 && m.face_data(f).face_tag == -99) ++internal_default;
    }
    TASK_CHECK(boundary_inherited == 4);    // one tagged input quad -> four
    TASK_CHECK(interface_inherited == 4);   // the interface quad -> four
    TASK_CHECK(internal_default > 0);       // brand new internal cut faces
    return 0;
}

static int attributes_full_cut_equivalence()
{
    const Hexmesh<> base = make_grid(2,2,2);

    Hexmesh<> via_sheets = base; via_sheets.subdivide_sheets(all_edges(via_sheets));
    Hexmesh<> via_legacy = base; via_legacy.poly_subdivide(hex_to_grid_2x2x2);
    Hexmesh<> via_midpt;  subdivision_midpoint(base, via_midpt);

    TASK_CHECK(via_sheets.num_verts()==via_legacy.num_verts() &&
               via_sheets.num_verts()==via_midpt.num_verts());
    TASK_CHECK(via_sheets.num_polys()==via_legacy.num_polys() &&
               via_sheets.num_polys()==via_midpt.num_polys());
    TASK_CHECK(sorted_positions(via_sheets)==sorted_positions(via_legacy));
    TASK_CHECK(sorted_positions(via_sheets)==sorted_positions(via_midpt));
    TASK_CHECK(poly_corner_sets(via_sheets)==poly_corner_sets(via_legacy));
    TASK_CHECK(poly_corner_sets(via_sheets)==poly_corner_sets(via_midpt));
    return 0;
}

static int attributes_midpoint_entry_points_and_in_place()
{
    const Hexmesh<> base = make_grid(2,1,1);

    // no-map entry point, separate output
    Hexmesh<> out;
    subdivision_midpoint(base, out);
    TASK_CHECK(out.num_verts()==45 && out.num_polys()==16);
    TASK_CHECK(interior_faces_opposite(out));

    // mapped entry point: std::map arguments are actually callable
    Hexmesh<> out2;
    std::map<uint,uint> em, fm, pm;
    subdivision_midpoint(base, out2, em, fm, pm);
    TASK_CHECK(em.size()==base.num_edges() && fm.size()==base.num_faces() &&
               pm.size()==base.num_polys());
    for(const auto & kv : em)
    {
        // the mapped node is a freshly created vertex, not an input one
        TASK_CHECK(kv.second >= base.num_verts());
    }
    // validate one edge / face / poly mapping through vertex sources by
    // rebuilding sources from a sheet call on the same base mesh
    HexSheetSubdivision info = [&]{ Hexmesh<> tmp = base;
                                    return tmp.subdivide_sheets(all_edges(tmp)); }();
    for(const auto & kv : em)
    {
        // edge_vert_ids returns a fresh vector each call, so bind one copy
        // before building a range (using two calls would mix two temporaries)
        const std::vector<uint> ev = base.edge_vert_ids(kv.first);
        const std::set<uint> endpoints(ev.begin(), ev.end());
        std::set<uint> got;
        for(auto pr : info.vertex_sources.at(kv.second)) got.insert(pr.first);
        TASK_CHECK(got == endpoints);
    }
    for(const auto & kv : fm)
    {
        std::set<uint> corners(base.adj_f2v(kv.first).begin(), base.adj_f2v(kv.first).end());
        std::set<uint> got;
        for(auto pr : info.vertex_sources.at(kv.second)) got.insert(pr.first);
        TASK_CHECK(got == corners);
    }
    for(const auto & kv : pm)
    {
        std::set<uint> corners(base.adj_p2v(kv.first).begin(), base.adj_p2v(kv.first).end());
        std::set<uint> got;
        for(auto pr : info.vertex_sources.at(kv.second)) got.insert(pr.first);
        TASK_CHECK(got == corners);
    }

    // in-place calls: same object as input and output
    Hexmesh<> ip1 = base; subdivision_midpoint(ip1, ip1);
    TASK_CHECK(ip1.num_verts()==45 && ip1.num_polys()==16);
    Hexmesh<> ip2 = base; std::map<uint,uint> em2,fm2,pm2;
    subdivision_midpoint(ip2, ip2, em2, fm2, pm2);
    TASK_CHECK(ip2.num_verts()==45 && em2.size()==base.num_edges());
    Hexmesh<> ip3 = base; ip3.subdivide_sheets(all_edges(ip3));
    TASK_CHECK(ip3.num_verts()==45 && ip3.num_polys()==16);
    Hexmesh<> ip4 = base; ip4.poly_subdivide(hex_to_grid_2x2x2);
    TASK_CHECK(ip4.num_verts()==45 && ip4.num_polys()==16);
    return 0;
}

//::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

const struct task_case task_cases[] =
{
    {"sheets",     "strip_propagation",                 sheet_strip_propagation},
    {"sheets",     "cardinalities_and_meeting",         sheet_cardinalities_and_meeting},
    {"sheets",     "distant_component",                 sheet_distant_component},
    {"sheets",     "order_duplicates_empty_invalid",    sheet_order_duplicates_empty_invalid},
    {"sheets",     "two_successive_refinements",        sheet_two_successive_refinements},
    {"topology",   "rotated_local_numbering",           topology_rotated_local_numbering},
    {"topology",   "rotated_interface",                 topology_rotated_interface},
    {"topology",   "conformality_and_winding",          topology_conformality_and_winding},
    {"topology",   "sources_and_parents",               topology_sources_and_parents},
    {"topology",   "oriented_volume",                   topology_oriented_volume},
    {"attributes", "custom_types_and_sources",          attributes_custom_types_and_sources},
    {"attributes", "full_cut_equivalence",              attributes_full_cut_equivalence},
    {"attributes", "midpoint_entry_points_and_in_place",attributes_midpoint_entry_points_and_in_place},
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
