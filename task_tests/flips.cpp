// Task tests for the in-place 2->3 and 3->2 tetrahedral flips.
//
// Registered through task_support/check.h in the three groups
// "cavity", "geometry" and "state". Every callback only uses
// TASK_CHECK for its assertions and returns 0 on success.

#include "task_support/check.h"

#include <OpenVolumeMesh/Mesh/TetrahedralMeshTopologyKernel.hh>
#include <OpenVolumeMesh/Mesh/TetrahedralGeometryKernel.hh>
#include <OpenVolumeMesh/Geometry/VectorT.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <vector>

using namespace OpenVolumeMesh;

using TopoMesh  = TetrahedralMeshTopologyKernel;
using GeomMesh  = TetrahedralGeometryKernel<Geometry::Vec3d>;
using Vec3      = Geometry::Vec3d;

namespace {

constexpr double kEps = 1e-10;

struct TopoBip {
    explicit TopoBip(TopoMesh& _m) : m(_m) {}
    TopoMesh& m;
    VertexHandle a{}, b{}, c{}, d{}, e{};
    CellHandle x{}, y{};
    FaceHandle interior{};
    std::vector<FaceHandle> shell{};
};

// Two tets x=(a,b,c,d) and y=(a,c,b,e) sharing the interior face abc.
TopoBip make_topo_bipyramid(TopoMesh& m)
{
    TopoBip bip{m};
    bip.a = m.add_vertex();
    bip.b = m.add_vertex();
    bip.c = m.add_vertex();
    bip.d = m.add_vertex();
    bip.e = m.add_vertex();
    bip.x = m.add_cell(bip.a, bip.b, bip.c, bip.d);
    bip.y = m.add_cell(bip.a, bip.c, bip.b, bip.e);

    for(FaceHandle fh : m.faces()) {
        if(m.is_boundary(fh))
            bip.shell.push_back(fh);
        else
            bip.interior = fh;
    }
    return bip;
}

struct GeomBip {
    explicit GeomBip(GeomMesh& _m) : m(_m) {}
    GeomMesh& m;
    VertexHandle a{}, b{}, c{}, d{}, e{};
    CellHandle x{}, y{};
    FaceHandle interior{};
    std::vector<FaceHandle> shell{};
};

// Geometric version of the asymmetric bipyramid. _scale multiplies all
// coordinates, _e overrides the position of the lower apex.
GeomBip make_geom_bipyramid(GeomMesh& m, double _scale = 1.0,
                            const Vec3& _e = Vec3(0.25, 0.25, -1.0))
{
    GeomBip bip{m};
    auto add = [&](const Vec3& p) { return m.add_vertex(_scale * p); };
    bip.a = add(Vec3(0.0, 0.0, 0.0));
    bip.b = add(Vec3(1.0, 0.0, 0.0));
    bip.c = add(Vec3(0.0, 1.0, 0.0));
    bip.d = add(Vec3(0.25, 0.25, 1.0));
    bip.e = add(_e);
    bip.x = m.add_cell(bip.a, bip.b, bip.c, bip.d);
    bip.y = m.add_cell(bip.a, bip.c, bip.b, bip.e);

    for(FaceHandle fh : m.faces()) {
        if(m.is_boundary(fh))
            bip.shell.push_back(fh);
        else
            bip.interior = fh;
    }
    return bip;
}

std::set<VertexHandle> vertex_set(TopoMesh& m, CellHandle _ch)
{
    std::set<VertexHandle> result;
    for(VertexHandle vh : m.get_cell_vertices(_ch))
        result.insert(vh);
    return result;
}

// The halfface of a boundary face pointing into the mesh.
HalfFaceHandle inner_halfface(TopoMesh& m, FaceHandle _fh)
{
    const HalfFaceHandle h0 = m.halfface_handle(_fh, 0);
    return m.incident_cell(h0).is_valid()
               ? h0
               : m.halfface_handle(_fh, 1);
}

double total_cell_volume(GeomMesh& m)
{
    double sum = 0.0;
    for(CellHandle ch : m.cells())
        sum += m.signed_volume(ch);
    return sum;
}

// Volume from the outward-oriented boundary triangle soup, i.e. the
// oriented cavity shell.
double shell_volume(GeomMesh& m)
{
    double sum = 0.0;
    for(FaceHandle fh : m.faces()) {
        if(m.is_deleted(fh) || !m.is_boundary(fh))
            continue;
        HalfFaceHandle outer = m.halfface_handle(fh, 0);
        if(m.incident_cell(outer).is_valid())
            outer = m.halfface_handle(fh, 1);
        const auto vs = m.get_halfface_vertices(outer);
        const Vec3& p0 = m.vertex(vs[0]);
        const Vec3& p1 = m.vertex(vs[1]);
        const Vec3& p2 = m.vertex(vs[2]);
        sum += p0.dot(p1.cross(p2)) / 6.0;
    }
    return sum;
}

bool is_sorted(const std::vector<CellHandle>& _handles)
{
    return std::is_sorted(_handles.begin(), _handles.end());
}

} // anonymous namespace

// =====================================================================
// cavity group
// =====================================================================

// Asymmetric bipyramid: 23 flip, immediate inverse 32 flip, and a chain
// of consecutive inverse flips.
static int cavity_roundtrip()
{
    TopoMesh m;
    TopoBip bip = make_topo_bipyramid(m);
    TASK_CHECK(bip.x.is_valid() && bip.y.is_valid());
    TASK_CHECK(bip.shell.size() == 6);
    TASK_CHECK(bip.interior.is_valid());

    const size_t n_vertices = m.n_vertices();
    const size_t n_edges    = m.n_edges();
    const size_t n_faces    = m.n_faces();
    const size_t n_cells    = m.n_cells();

    // Record shell halfface orientations and their outside neighbors.
    std::vector<std::vector<VertexHandle>> shell_orient;
    for(FaceHandle fh : bip.shell)
        shell_orient.push_back(m.get_halfface_vertices(inner_halfface(m, fh)));

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(created.size() == 3);
    TASK_CHECK(is_sorted(created));

    // No vertex is added, the old slots are retained.
    TASK_CHECK(m.n_vertices() == n_vertices);
    TASK_CHECK(m.n_edges() == n_edges + 1);
    TASK_CHECK(m.n_faces() == n_faces + 3);
    TASK_CHECK(m.n_cells() == n_cells + 3);
    TASK_CHECK(m.n_logical_cells() == 3);

    // Old cells and the old internal face are tagged deleted.
    TASK_CHECK(m.is_deleted(bip.x));
    TASK_CHECK(m.is_deleted(bip.y));
    TASK_CHECK(m.is_deleted(bip.interior));

    // The new internal edge joins the two former opposite vertices.
    const HalfEdgeHandle he_de = m.find_halfedge(bip.d, bip.e);
    TASK_CHECK(he_de.is_valid());
    const EdgeHandle new_edge = he_de.edge_handle();
    TASK_CHECK(!m.is_deleted(new_edge));
    TASK_CHECK(!m.is_boundary(new_edge));
    TASK_CHECK(m.valence(new_edge) == 3); // the three new internal faces

    // The six shell faces keep handles, deletion status and orientation.
    TASK_CHECK(bip.shell.size() == 6);
    for(size_t i = 0; i < bip.shell.size(); ++i) {
        const FaceHandle fh = bip.shell[i];
        TASK_CHECK(!m.is_deleted(fh));
        TASK_CHECK(m.is_boundary(fh));
        const HalfFaceHandle inner = inner_halfface(m, fh);
        TASK_CHECK(m.get_halfface_vertices(inner) == shell_orient[i]);
        // The inner side now points at one of the new cells.
        const CellHandle owner = m.incident_cell(inner);
        TASK_CHECK(std::find(created.begin(), created.end(), owner) != created.end());
        TASK_CHECK(!m.incident_cell(m.opposite_halfface_handle(inner)).is_valid());
    }

    // Per-halfface adjacency of the three new internal faces.
    unsigned internal_faces = 0;
    for(FaceHandle fh : m.faces()) {
        if(m.is_deleted(fh) || m.is_boundary(fh))
            continue;
        ++internal_faces;
        const auto cells = m.face_cells(fh);
        TASK_CHECK(cells[0].is_valid() && cells[1].is_valid());
        TASK_CHECK(cells[0] != cells[1]);
        TASK_CHECK(std::find(created.begin(), created.end(), cells[0]) != created.end());
        TASK_CHECK(std::find(created.begin(), created.end(), cells[1]) != created.end());
    }
    TASK_CHECK(internal_faces == 3);

    // Cell vertex traversal sees exactly the four expected vertices.
    for(CellHandle ch : created) {
        const auto vs = m.get_cell_vertices(ch);
        TASK_CHECK(vs.size() == 4);
        TASK_CHECK(std::set<VertexHandle>(vs.begin(), vs.end()).size() == 4);
    }

    // Boundary query still reports exactly six boundary faces.
    unsigned boundary_count = 0;
    for(BoundaryFaceIter it = m.bf_iter(); it.valid(); ++it)
        ++boundary_count;
    TASK_CHECK(boundary_count == 6);

    // Inverse 32 flip on the new internal edge.
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(new_edge, back));
    TASK_CHECK(back.size() == 2);
    TASK_CHECK(is_sorted(back));
    TASK_CHECK(m.n_logical_cells() == 2);
    TASK_CHECK(m.is_deleted(new_edge));

    // The restored tets span the original five-vertex bipyramid.
    const std::set<VertexHandle> expected{bip.a, bip.b, bip.c, bip.d, bip.e};
    for(CellHandle ch : back) {
        const auto vs = vertex_set(m, ch);
        TASK_CHECK(vs.size() == 4);
        for(VertexHandle vh : vs)
            TASK_CHECK(expected.count(vh) == 1);
    }
    unsigned boundary_after = 0;
    for(FaceHandle fh : m.faces())
        if(!m.is_deleted(fh) && m.is_boundary(fh))
            ++boundary_after;
    TASK_CHECK(boundary_after == 6);

    // Consecutive inverse flips without garbage collection in between.
    FaceHandle flip_face = FaceHandle(-1);
    for(int round = 0; round < 4; ++round) {
        for(FaceHandle fh : m.faces())
            if(!m.is_deleted(fh) && !m.is_boundary(fh))
                flip_face = fh;
        std::vector<CellHandle> out;
        TASK_CHECK(m.flip23(flip_face, out));
        TASK_CHECK(out.size() == 3);
        const HalfEdgeHandle he = m.find_halfedge(bip.d, bip.e);
        TASK_CHECK(he.is_valid());
        std::vector<CellHandle> out2;
        TASK_CHECK(m.flip32(he.edge_handle(), out2));
        TASK_CHECK(out2.size() == 2);
    }
    TASK_CHECK(m.n_vertices() == n_vertices);
    TASK_CHECK(m.needs_garbage_collection());
    return 0;
}

// Shell faces with external neighbor cells: outside adjacency must be
// preserved through both flips.
static int cavity_external_neighbors()
{
    TopoMesh m;
    TopoBip bip = make_topo_bipyramid(m);

    // Glue an extra tet onto one shell face.
    const FaceHandle anchor = bip.shell.front();
    const HalfFaceHandle outer0 = m.halfface_handle(anchor, 0);
    const HalfFaceHandle outer = m.incident_cell(outer0).is_valid()
                                     ? m.halfface_handle(anchor, 1)
                                     : outer0;
    const auto outer_vs = m.get_halfface_vertices(outer);
    const VertexHandle f = m.add_vertex();
    const CellHandle neighbor =
        m.add_cell(outer_vs[0], outer_vs[1], outer_vs[2], f);
    TASK_CHECK(neighbor.is_valid());
    TASK_CHECK(m.incident_cell(outer) == neighbor);

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(created.size() == 3);

    // The neighbor keeps its handle and stays on the outside of the
    // same shell face, whose handle and orientation are preserved.
    TASK_CHECK(!m.is_deleted(neighbor));
    TASK_CHECK(!m.is_deleted(anchor));
    TASK_CHECK(m.incident_cell(outer) == neighbor);
    const CellHandle new_owner = m.incident_cell(m.opposite_halfface_handle(outer));
    TASK_CHECK(std::find(created.begin(), created.end(), new_owner) != created.end());
    TASK_CHECK(vertex_set(m, neighbor).count(f) == 1);

    // The neighbor's other three faces are untouched.
    unsigned neighbor_faces = 0;
    for(HalfFaceHandle hfh : m.cell(neighbor).halffaces()) {
        TASK_CHECK(!m.is_deleted(m.face_handle(hfh)));
        ++neighbor_faces;
    }
    TASK_CHECK(neighbor_faces == 4);

    const HalfEdgeHandle he = m.find_halfedge(bip.d, bip.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(he.edge_handle(), back));
    TASK_CHECK(back.size() == 2);
    TASK_CHECK(m.incident_cell(outer) == neighbor);
    TASK_CHECK(!m.is_deleted(neighbor));
    return 0;
}

// Topological conflicts must be rejected before anything is written.
static int cavity_topology_conflicts()
{
    TopoMesh m;
    TopoBip bip = make_topo_bipyramid(m);

    const size_t n_edges = m.n_edges();
    const size_t n_faces = m.n_faces();
    const size_t n_cells = m.n_cells();

    // Boundary face cannot be flipped 23.
    std::vector<CellHandle> sentinel{CellHandle(99)};
    std::vector<CellHandle> out = sentinel;
    TASK_CHECK(!m.flip23(bip.shell.front(), out));
    TASK_CHECK(out == sentinel);
    TASK_CHECK(m.n_edges() == n_edges && m.n_faces() == n_faces && m.n_cells() == n_cells);
    TASK_CHECK(!m.is_deleted(bip.x) && !m.is_deleted(bip.y));

    // A boundary edge has the wrong valence for a 32 flip.
    const HalfEdgeHandle he_ab = m.find_halfedge(bip.a, bip.b);
    TASK_CHECK(he_ab.is_valid());
    TASK_CHECK(!m.flip32(he_ab.edge_handle(), out));
    TASK_CHECK(out == sentinel);

    // Candidate internal edge already occupied by a live standalone edge.
    const EdgeHandle blocker = m.add_edge(bip.d, bip.e);
    TASK_CHECK(blocker.is_valid());
    TASK_CHECK(!m.flip23(bip.interior, out));
    TASK_CHECK(out == sentinel);
    TASK_CHECK(m.n_edges() == n_edges + 1);
    TASK_CHECK(!m.is_deleted(bip.x) && !m.is_deleted(bip.y));

    // Remove the blocker again; the same flip now succeeds.
    m.delete_edge(blocker);
    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(created.size() == 3);

    // Candidate internal face occupied: create a standalone link face,
    // then the 32 flip must be refused.
    TASK_CHECK(m.find_halfedge(bip.a, bip.b).is_valid());
    const FaceHandle face_blocker =
        m.add_face(std::vector<VertexHandle>{bip.a, bip.b, bip.c});
    TASK_CHECK(face_blocker.is_valid());
    const HalfEdgeHandle he_de = m.find_halfedge(bip.d, bip.e);
    TASK_CHECK(!m.flip32(he_de.edge_handle(), out));
    TASK_CHECK(out == sentinel);
    TASK_CHECK(m.n_logical_cells() == 3);

    // Duplicate edges and faces within the cavity are rejected.
    {
        TopoMesh dm;
        TopoBip dbip = make_topo_bipyramid(dm);
        const size_t de = dm.n_edges();
        const size_t df = dm.n_faces();
        const size_t dc = dm.n_cells();
        std::vector<CellHandle> dout;

        const EdgeHandle dup_edge = dm.add_edge(dbip.a, dbip.b, true);
        TASK_CHECK(dup_edge.is_valid());
        TASK_CHECK(!dm.flip23(dbip.interior, dout));
        TASK_CHECK(dout.empty());
        TASK_CHECK(dm.n_edges() == de + 1 && dm.n_faces() == df && dm.n_cells() == dc);
        dm.delete_edge(dup_edge);
        TASK_CHECK(dm.flip23(dbip.interior, dout));
        TASK_CHECK(dout.size() == 3);

        TopoMesh fm;
        TopoBip fbip = make_topo_bipyramid(fm);
        const size_t ff = fm.n_faces();
        const size_t fc = fm.n_cells();
        const auto hes = fm.face(fbip.interior).halfedges();
        const FaceHandle dup_face = fm.add_face(std::vector<HalfEdgeHandle>(hes));
        TASK_CHECK(dup_face.is_valid());
        std::vector<CellHandle> fout;
        TASK_CHECK(!fm.flip23(fbip.interior, fout));
        TASK_CHECK(fout.empty());
        TASK_CHECK(fm.n_faces() == ff + 1 && fm.n_cells() == fc);
        fm.delete_face(dup_face);
        TASK_CHECK(fm.flip23(fbip.interior, fout));
        TASK_CHECK(fout.size() == 3);
    }
    return 0;
}

// The flips require deferred deletion and all bottom-up incidences and
// must not change those switches themselves.
static int cavity_required_switches()
{
    {
        TopoMesh m;
        TopoBip bip = make_topo_bipyramid(m);
        m.enable_deferred_deletion(false); // also runs garbage collection
        TASK_CHECK(!m.deferred_deletion_enabled());
        std::vector<CellHandle> out;
        TASK_CHECK(!m.flip23(bip.interior, out));
        TASK_CHECK(!m.deferred_deletion_enabled());
        TASK_CHECK(out.empty());
    }
    {
        TopoMesh m;
        TopoBip bip = make_topo_bipyramid(m);
        m.enable_face_bottom_up_incidences(false);
        TASK_CHECK(!m.has_full_bottom_up_incidences());
        std::vector<CellHandle> out;
        TASK_CHECK(!m.flip23(bip.interior, out));
        TASK_CHECK(!m.flip32(EdgeHandle(0), out));
        TASK_CHECK(!m.has_full_bottom_up_incidences());
        TASK_CHECK(out.empty());
    }
    {
        TopoMesh m;
        TopoBip bip = make_topo_bipyramid(m);
        m.enable_edge_bottom_up_incidences(false);
        std::vector<CellHandle> out;
        TASK_CHECK(!m.flip23(bip.interior, out));
        TASK_CHECK(!m.has_full_bottom_up_incidences());
    }
    return 0;
}

// A disconnected component far away from the cavity is not rebuilt.
static int cavity_distant_component()
{
    TopoMesh m;
    TopoBip bip = make_topo_bipyramid(m);

    const VertexHandle f = m.add_vertex();
    const VertexHandle g = m.add_vertex();
    const VertexHandle h = m.add_vertex();
    const VertexHandle i = m.add_vertex();
    const CellHandle distant = m.add_cell(f, g, h, i);
    TASK_CHECK(distant.is_valid());
    auto distant_vertices = vertex_set(m, distant);
    std::set<FaceHandle> distant_faces;
    for(HalfFaceHandle hfh : m.cell(distant).halffaces())
        distant_faces.insert(m.face_handle(hfh));

    auto prop = m.request_cell_property<int>("distant_tag", 0);
    prop[distant] = 4242;

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(created.size() == 3);
    const HalfEdgeHandle he = m.find_halfedge(bip.d, bip.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(he.edge_handle(), back));

    TASK_CHECK(!m.is_deleted(distant));
    TASK_CHECK(vertex_set(m, distant) == distant_vertices);
    TASK_CHECK(prop[distant] == 4242);
    for(FaceHandle fh : distant_faces) {
        TASK_CHECK(!m.is_deleted(fh));
        TASK_CHECK(m.is_boundary(fh));
    }
    return 0;
}

// =====================================================================
// geometry group
// =====================================================================

// Signed volumes are conserved and agree with the oriented shell, and
// every new tet is positively oriented.
static int geometry_volume_conservation()
{
    GeomMesh m;
    GeomBip bip = make_geom_bipyramid(m);
    TASK_CHECK(m.signed_volume(bip.x) > 0.0);
    TASK_CHECK(m.signed_volume(bip.y) > 0.0);

    const double before = total_cell_volume(m);
    const double shell_before = shell_volume(m);
    TASK_CHECK(std::abs(before - shell_before) < kEps);

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(created.size() == 3);
    for(CellHandle ch : created)
        TASK_CHECK(m.signed_volume(ch) > 0.0);
    TASK_CHECK(std::abs(total_cell_volume(m) - before) < kEps);
    TASK_CHECK(std::abs(shell_volume(m) - before) < kEps);

    const HalfEdgeHandle he = m.find_halfedge(bip.d, bip.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(he.edge_handle(), back));
    TASK_CHECK(back.size() == 2);
    for(CellHandle ch : back)
        TASK_CHECK(m.signed_volume(ch) > 0.0);
    TASK_CHECK(std::abs(total_cell_volume(m) - before) < kEps);
    TASK_CHECK(std::abs(shell_volume(m) - before) < kEps);
    return 0;
}

// A coplanar candidate tet has zero signed volume and is rejected even
// though the two old tets are positive; the mesh stays untouched.
static int geometry_degenerate_rejected()
{
    GeomMesh m;
    // e lies in the plane through a, b, d, so the 23 candidate tet
    // (a,b,d,e) has exactly zero signed volume.
    GeomBip bip = make_geom_bipyramid(m, 1.0, Vec3(0.5, -0.25, -1.0));
    TASK_CHECK(m.signed_volume(bip.x) > 0.0);
    TASK_CHECK(m.signed_volume(bip.y) > 0.0);

    const size_t n_cells = m.n_cells();
    const size_t n_faces = m.n_faces();
    const size_t n_edges = m.n_edges();

    std::vector<CellHandle> out;
    TASK_CHECK(!m.flip23(bip.interior, out));
    TASK_CHECK(out.empty());
    TASK_CHECK(m.n_cells() == n_cells && m.n_faces() == n_faces && m.n_edges() == n_edges);
    TASK_CHECK(!m.is_deleted(bip.x) && !m.is_deleted(bip.y));

    // A threshold of zero still cannot rescue a zero volume.
    TASK_CHECK(!m.flip23(bip.interior, out, 0.0));
    TASK_CHECK(out.empty());

    // Directly build a three-tet ring where one 32 candidate (the tet
    // under new face abc with apex d) is degenerate.
    GeomMesh ring;
    auto a = ring.add_vertex(Vec3(0.0, 0.0, 0.0));
    auto b = ring.add_vertex(Vec3(1.0, 0.0, 0.0));
    auto c = ring.add_vertex(Vec3(0.0, 1.0, 0.0));
    auto d = ring.add_vertex(Vec3(0.25, 0.25, 0.0));  // coplanar with abc
    auto e = ring.add_vertex(Vec3(0.25, 0.25, -1.0));
    auto t0 = ring.add_cell(a, d, b, e);
    auto t1 = ring.add_cell(b, d, c, e);
    auto t2 = ring.add_cell(c, d, a, e);
    TASK_CHECK(t0.is_valid() && t1.is_valid() && t2.is_valid());
    TASK_CHECK(ring.signed_volume(t0) > 0.0);
    TASK_CHECK(ring.signed_volume(t1) > 0.0);
    TASK_CHECK(ring.signed_volume(t2) > 0.0);
    const HalfEdgeHandle he = ring.find_halfedge(d, e);
    TASK_CHECK(he.is_valid());
    const size_t ring_cells = ring.n_cells();
    std::vector<CellHandle> ring_out;
    TASK_CHECK(!ring.flip32(he.edge_handle(), ring_out));
    TASK_CHECK(ring_out.empty());
    TASK_CHECK(ring.n_cells() == ring_cells);
    TASK_CHECK(!ring.is_deleted(t0) && !ring.is_deleted(t1) && !ring.is_deleted(t2));
    return 0;
}

// Both old tets are positively oriented, but the lower apex is far
// outside the prism over abc so that one replacement tet would leave
// the cavity (negative signed volume). It must be rejected on signed
// volume rather than accepted on its absolute volume.
static int geometry_inversion_rejected()
{
    GeomMesh m;
    auto a = m.add_vertex(Vec3(0.0, 0.0, 0.0));
    auto b = m.add_vertex(Vec3(1.0, 0.0, 0.0));
    auto c = m.add_vertex(Vec3(0.0, 1.0, 0.0));
    auto d = m.add_vertex(Vec3(0.25, 0.25, 1.0));
    auto e = m.add_vertex(Vec3(5.0, 5.0, -0.5));
    auto x = m.add_cell(a, b, c, d);
    auto y = m.add_cell(a, c, b, e);
    TASK_CHECK(m.signed_volume(x) > 0.0);
    TASK_CHECK(m.signed_volume(y) > 0.0);

    FaceHandle interior;
    for(FaceHandle fh : m.faces())
        if(!m.is_boundary(fh))
            interior = fh;

    const size_t n_cells = m.n_cells();
    std::vector<CellHandle> out;
    TASK_CHECK(!m.flip23(interior, out));
    TASK_CHECK(out.empty());
    TASK_CHECK(m.n_cells() == n_cells);
    TASK_CHECK(!m.is_deleted(x) && !m.is_deleted(y));

    // The identical combinatorial cavity is accepted by the
    // topology-only kernel: rejection is purely geometric.
    TopoMesh topo;
    auto ta = topo.add_vertex(), tb = topo.add_vertex(), tc = topo.add_vertex();
    auto td = topo.add_vertex(), te = topo.add_vertex();
    topo.add_cell(ta, tb, tc, td);
    topo.add_cell(ta, tc, tb, te);
    FaceHandle tinterior;
    for(FaceHandle fh : topo.faces())
        if(!topo.is_boundary(fh))
            tinterior = fh;
    std::vector<CellHandle> tout;
    TASK_CHECK(topo.flip23(tinterior, tout));
    TASK_CHECK(tout.size() == 3);
    return 0;
}

// The threshold is relative to the cavity size, so scaling the whole
// mesh does not change the accept/reject decision.
static int geometry_non_unit_scale()
{
    for(double scale : {1e-3, 1.0, 1e3}) {
        GeomMesh m;
        GeomBip bip = make_geom_bipyramid(m, scale);
        const double before = total_cell_volume(m);
        TASK_CHECK(std::abs(before - (1.0 / 3.0) * scale * scale * scale) <
                   kEps * std::max(1.0, scale * scale * scale));

        std::vector<CellHandle> created;
        TASK_CHECK(m.flip23(bip.interior, created));
        TASK_CHECK(created.size() == 3);
        TASK_CHECK(std::abs(total_cell_volume(m) - before) <
                   kEps * std::max(1.0, scale * scale * scale));

        const HalfEdgeHandle he = m.find_halfedge(bip.d, bip.e);
        std::vector<CellHandle> back;
        TASK_CHECK(m.flip32(he.edge_handle(), back));
        TASK_CHECK(back.size() == 2);
        TASK_CHECK(std::abs(total_cell_volume(m) - before) <
                   kEps * std::max(1.0, scale * scale * scale));
    }

    // A zero-volume candidate stays zero under any scaling.
    for(double scale : {1e-3, 1.0, 1e3}) {
        GeomMesh m;
        GeomBip bip = make_geom_bipyramid(m, scale, Vec3(0.5, -0.25, -1.0));
        std::vector<CellHandle> out;
        TASK_CHECK(!m.flip23(bip.interior, out));
        TASK_CHECK(out.empty());
    }
    return 0;
}

// Explicit thresholds: an oversized threshold rejects a healthy cavity.
static int geometry_custom_threshold()
{
    GeomMesh m;
    GeomBip bip = make_geom_bipyramid(m);
    const size_t n_cells = m.n_cells();

    std::vector<CellHandle> out;
    TASK_CHECK(!m.flip23(bip.interior, out, 1.0));
    TASK_CHECK(out.empty());
    TASK_CHECK(m.n_cells() == n_cells);
    TASK_CHECK(!m.is_deleted(bip.x) && !m.is_deleted(bip.y));

    // The default threshold accepts the same cavity.
    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(created.size() == 3);
    return 0;
}

// =====================================================================
// state group
// =====================================================================

// Caller-defined cell properties are copied from the smallest removed
// cell; new internal simplices get defaults; shell data is preserved.
struct CellData {
    int tag = -7;
    double value = 3.25;
};

static int state_custom_properties()
{
    TopoMesh m;
    TopoBip bip = make_topo_bipyramid(m);
    TASK_CHECK(bip.x < bip.y);

    auto cell_prop = m.request_cell_property<CellData>("cell_data", CellData{});
    auto face_prop = m.request_face_property<int>("face_data", 0);
    auto edge_prop = m.request_edge_property<int>("edge_data", -1);
    auto vert_prop = m.request_vertex_property<int>("vert_data", -1);

    cell_prop[bip.x].tag = 100;
    cell_prop[bip.x].value = 1.5;
    cell_prop[bip.y].tag = 200;
    cell_prop[bip.y].value = 2.5;

    for(FaceHandle fh : bip.shell)
        face_prop[fh] = fh.idx() + 11;
    std::set<EdgeHandle> shell_edges;
    for(FaceHandle fh : bip.shell)
        for(HalfEdgeHandle heh : m.face_halfedges(fh))
            shell_edges.insert(heh.edge_handle());
    for(EdgeHandle eh : shell_edges)
        edge_prop[eh] = eh.idx() + 5;
    for(VertexHandle vh : m.vertices())
        vert_prop[vh] = vh.idx() + 1;

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(created.size() == 3);

    // New cells copy every cell property of the smallest old cell.
    for(CellHandle ch : created) {
        TASK_CHECK(cell_prop[ch].tag == 100);
        TASK_CHECK(cell_prop[ch].value == 1.5);
    }
    // Shell faces keep their property values.
    for(FaceHandle fh : bip.shell)
        TASK_CHECK(face_prop[fh] == fh.idx() + 11);
    // New internal faces carry the default.
    for(FaceHandle fh : m.faces()) {
        if(m.is_deleted(fh) || m.is_boundary(fh))
            continue;
        TASK_CHECK(face_prop[fh] == 0);
    }
    // New internal edge carries the default, shell edges are untouched.
    const HalfEdgeHandle he_de = m.find_halfedge(bip.d, bip.e);
    TASK_CHECK(edge_prop[he_de.edge_handle()] == -1);
    for(EdgeHandle eh : shell_edges)
        TASK_CHECK(edge_prop[eh] == eh.idx() + 5);
    for(VertexHandle vh : m.vertices())
        TASK_CHECK(vert_prop[vh] == vh.idx() + 1);

    // After the 32 flip, new cells inherit from the smallest of the
    // three removed cells, which itself carries the copied x values.
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(he_de.edge_handle(), back));
    TASK_CHECK(back.size() == 2);
    for(CellHandle ch : back) {
        TASK_CHECK(cell_prop[ch].tag == 100);
        TASK_CHECK(cell_prop[ch].value == 1.5);
    }
    for(FaceHandle fh : bip.shell)
        TASK_CHECK(face_prop[fh] == fh.idx() + 11);
    return 0;
}

// Deferred deletion: old slots remain present and flagged, nothing is
// garbage collected, and editing can continue.
static int state_deferred_slots()
{
    TopoMesh m;
    TopoBip bip = make_topo_bipyramid(m);
    const size_t n_cells = m.n_cells();

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(created.size() == 3);
    TASK_CHECK(m.n_cells() == n_cells + 3);          // slots retained
    TASK_CHECK(m.n_logical_cells() == 3);
    TASK_CHECK(m.needs_garbage_collection());
    TASK_CHECK(m.is_deleted(bip.x) && m.is_deleted(bip.y));
    TASK_CHECK(m.is_deleted(bip.interior));

    const HalfEdgeHandle he = m.find_halfedge(bip.d, bip.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(he.edge_handle(), back));
    TASK_CHECK(back.size() == 2);
    TASK_CHECK(m.n_cells() == n_cells + 5);          // still no compaction
    TASK_CHECK(m.n_logical_cells() == 2);
    TASK_CHECK(m.is_deleted(he.edge_handle()));
    TASK_CHECK(m.needs_garbage_collection());

    // Live iteration never sees the deleted slots.
    unsigned live = 0;
    for(CellHandle ch : m.cells()) {
        TASK_CHECK(!m.is_deleted(ch));
        ++live;
    }
    TASK_CHECK(live == 2);
    unsigned live_faces = 0;
    for(FaceHandle fh : m.faces()) {
        TASK_CHECK(!m.is_deleted(fh));
        ++live_faces;
    }
    TASK_CHECK(live_faces == 7); // six shell faces + one internal face

    // Deleted counters match the bookkeeping: one old internal face
    // plus the three internal faces of the intermediate three-tet ring.
    TASK_CHECK(m.n_cells() - m.n_logical_cells() == m.n_cells() - 2);
    TASK_CHECK(m.n_faces() - m.n_logical_faces() == 4);
    return 0;
}

// A rejected flip leaves entity counts, deletion flags and the output
// vector exactly as they were, and editing works again afterwards.
static int state_failure_is_atomic()
{
    TopoMesh m;
    TopoBip bip = make_topo_bipyramid(m);

    const size_t counts[4] = {m.n_vertices(), m.n_edges(), m.n_faces(), m.n_cells()};
    std::vector<bool> cell_flags(m.n_cells(), false);
    for(CellHandle ch : m.cells())
        cell_flags[ch.idx()] = m.is_deleted(ch);

    auto tag = m.request_cell_property<int>("atomic_tag", 0);
    tag[bip.x] = 31;
    tag[bip.y] = 47;

    std::vector<CellHandle> untouched{CellHandle(7), CellHandle(8)};
    std::vector<CellHandle> out = untouched;
    TASK_CHECK(!m.flip23(bip.shell[0], out));
    TASK_CHECK(out == untouched);
    TASK_CHECK(m.n_vertices() == counts[0] && m.n_edges() == counts[1]);
    TASK_CHECK(m.n_faces() == counts[2] && m.n_cells() == counts[3]);
    for(CellHandle ch : m.cells())
        TASK_CHECK(m.is_deleted(ch) == cell_flags[ch.idx()]);
    // Properties are untouched by the rejected operation.
    TASK_CHECK(tag[bip.x] == 31 && tag[bip.y] == 47);

    // Editing continues normally after the failure.
    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(created.size() == 3);
    TASK_CHECK(m.deferred_deletion_enabled());
    TASK_CHECK(m.has_full_bottom_up_incidences());
    return 0;
}

// Successful flips never touch the deferred-deletion / incidence
// switches and never trigger garbage collection themselves.
static int state_switches_stay_enabled()
{
    GeomMesh m;
    GeomBip bip = make_geom_bipyramid(m);
    TASK_CHECK(m.deferred_deletion_enabled());
    TASK_CHECK(m.has_full_bottom_up_incidences());

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(bip.interior, created));
    TASK_CHECK(m.deferred_deletion_enabled());
    TASK_CHECK(m.has_full_bottom_up_incidences());

    const HalfEdgeHandle he = m.find_halfedge(bip.d, bip.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(he.edge_handle(), back));
    TASK_CHECK(m.deferred_deletion_enabled());
    TASK_CHECK(m.has_full_bottom_up_incidences());
    TASK_CHECK(m.needs_garbage_collection()); // flagged, but not collected
    TASK_CHECK(m.n_vertices() == 5);
    return 0;
}

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
    {"cavity", "roundtrip",            cavity_roundtrip},
    {"cavity", "external_neighbors",   cavity_external_neighbors},
    {"cavity", "topology_conflicts",   cavity_topology_conflicts},
    {"cavity", "required_switches",    cavity_required_switches},
    {"cavity", "distant_component",    cavity_distant_component},

    {"geometry", "volume_conservation", geometry_volume_conservation},
    {"geometry", "degenerate_rejected", geometry_degenerate_rejected},
    {"geometry", "inversion_rejected", geometry_inversion_rejected},
    {"geometry", "non_unit_scale",      geometry_non_unit_scale},
    {"geometry", "custom_threshold",    geometry_custom_threshold},

    {"state", "custom_properties",  state_custom_properties},
    {"state", "deferred_slots",     state_deferred_slots},
    {"state", "failure_is_atomic",  state_failure_is_atomic},
    {"state", "switches_stay_enabled", state_switches_stay_enabled},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
