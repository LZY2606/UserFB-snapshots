#include "task_support/check.h"

#include <OpenVolumeMesh/Mesh/TetrahedralGeometryKernel.hh>
#include <OpenVolumeMesh/Mesh/TetrahedralMeshTopologyKernel.hh>
#include <OpenVolumeMesh/Geometry/VectorT.hh>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>
#include <vector>

using namespace OpenVolumeMesh;

namespace {

using TopoMesh = TetrahedralMeshTopologyKernel;
using GeomMesh = TetrahedralGeometryKernel<Geometry::Vec3d>;
using Point = Geometry::Vec3d;

struct BipHandles {
    VertexHandle a, b, c, d, e;
    CellHandle x, y;
    FaceHandle inner;
};

// Connectivity of the asymmetric bipyramid used throughout the tests:
// tets (a,b,c,d) and (a,c,b,e) share the internal face abc.
BipHandles build_bip(TopoMesh& _m)
{
    BipHandles h;
    h.a = _m.add_vertex();
    h.b = _m.add_vertex();
    h.c = _m.add_vertex();
    h.d = _m.add_vertex();
    h.e = _m.add_vertex();
    h.x = _m.add_cell(h.a, h.b, h.c, h.d, true);
    h.y = _m.add_cell(h.a, h.c, h.b, h.e, true);
    for (FaceHandle fh : _m.faces()) {
        if (!_m.is_boundary(fh)) { h.inner = fh; break; }
    }
    return h;
}

// Geometric version. _s uniformly scales every coordinate, which must not
// change the behavior of the relative volume threshold.
BipHandles build_bip(GeomMesh& _m, double _s = 1.0)
{
    BipHandles h;
    h.a = _m.add_vertex(Point(0.0, 0.0, 0.0) * _s);
    h.b = _m.add_vertex(Point(1.0, 0.0, 0.0) * _s);
    h.c = _m.add_vertex(Point(0.0, 1.0, 0.0) * _s);
    h.d = _m.add_vertex(Point(0.2, 0.1, 1.0) * _s);
    h.e = _m.add_vertex(Point(0.3, 0.2, -1.0) * _s);
    h.x = _m.add_cell(h.a, h.b, h.c, h.d, true);
    h.y = _m.add_cell(h.a, h.c, h.b, h.e, true);
    for (FaceHandle fh : _m.faces()) {
        if (!_m.is_boundary(fh)) { h.inner = fh; break; }
    }
    return h;
}

FaceHandle first_boundary_face(const TopoMesh& _m)
{
    for (FaceHandle fh : _m.faces()) {
        if (_m.is_boundary(fh)) return fh;
    }
    return TopoMesh::InvalidFaceHandle;
}

unsigned count_boundary_faces(const TopoMesh& _m)
{
    unsigned n = 0;
    for (FaceHandle fh : _m.faces()) {
        if (_m.is_boundary(fh)) ++n;
    }
    return n;
}

std::array<VertexHandle, 3> halfface_tuple(const TopoMesh& _m, HalfFaceHandle _hfh)
{
    const auto v = _m.get_halfface_vertices(_hfh);
    return {v[0], v[1], v[2]};
}

double total_signed_volume(GeomMesh& _m)
{
    double v = 0.0;
    for (CellHandle ch : _m.cells()) v += _m.signed_volume(ch);
    return v;
}

EdgeHandle find_edge(const TopoMesh& _m, VertexHandle _v0, VertexHandle _v1)
{
    HalfEdgeHandle heh = _m.find_halfedge(_v0, _v1);
    return heh.is_valid() ? _m.edge_handle(heh) : TopoMesh::InvalidEdgeHandle;
}

// =========================================================================
// cavity group: combinatorial cavity orientation and adjacency
// =========================================================================

static int cavity_roundtrip()
{
    TopoMesh m;
    BipHandles h = build_bip(m);

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(h.inner, created));
    TASK_CHECK(created.size() == 3);
    for (size_t i = 1; i < created.size(); ++i)
        TASK_CHECK(created[i - 1] < created[i]);
    TASK_CHECK(m.n_vertices() == 5);
    TASK_CHECK(m.n_logical_cells() == 3);
    TASK_CHECK(count_boundary_faces(m) == 6);
    TASK_CHECK(m.n_logical_faces() == 9);

    // The new internal edge de has valence three and is not on the boundary.
    EdgeHandle newEdge = find_edge(m, h.d, h.e);
    TASK_CHECK(newEdge.is_valid());
    TASK_CHECK(!m.is_deleted(newEdge));
    TASK_CHECK(m.valence(newEdge) == 3);
    TASK_CHECK(!m.is_boundary(newEdge));

    // Old simplices keep their slots but are marked deleted.
    TASK_CHECK(m.is_deleted(h.x));
    TASK_CHECK(m.is_deleted(h.y));
    TASK_CHECK(m.is_deleted(h.inner));

    // Inverse 3->2 flip on the freshly created edge.
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(newEdge, back));
    TASK_CHECK(back.size() == 2);
    TASK_CHECK(back[0] < back[1]);
    TASK_CHECK(m.n_logical_cells() == 2);
    TASK_CHECK(count_boundary_faces(m) == 6);
    TASK_CHECK(m.n_logical_faces() == 7);
    TASK_CHECK(m.is_deleted(newEdge));
    TASK_CHECK(m.n_vertices() == 5);
    return 0;
}

static int cavity_hull_preservation()
{
    TopoMesh m;
    BipHandles h = build_bip(m);

    // Snapshot the six hull faces: handles, both halfface orientations and
    // boundary status.
    struct HullSnapshot {
        FaceHandle face;
        std::array<VertexHandle, 3> h0;
        std::array<VertexHandle, 3> h1;
        bool boundary;
    };
    std::vector<HullSnapshot> hull;
    for (FaceHandle fh : m.faces()) {
        if (fh == h.inner) continue;
        hull.push_back({fh,
                        halfface_tuple(m, m.halfface_handle(fh, 0)),
                        halfface_tuple(m, m.halfface_handle(fh, 1)),
                        m.is_boundary(fh)});
    }
    TASK_CHECK(hull.size() == 6);

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(h.inner, created));

    for (const HullSnapshot& s : hull) {
        TASK_CHECK(!m.is_deleted(s.face));
        TASK_CHECK(halfface_tuple(m, m.halfface_handle(s.face, 0)) == s.h0);
        TASK_CHECK(halfface_tuple(m, m.halfface_handle(s.face, 1)) == s.h1);
        TASK_CHECK(m.is_boundary(s.face) == s.boundary);
    }

    // And again after the inverse 3->2 flip.
    EdgeHandle newEdge = find_edge(m, h.d, h.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(newEdge, back));
    for (const HullSnapshot& s : hull) {
        TASK_CHECK(!m.is_deleted(s.face));
        TASK_CHECK(halfface_tuple(m, m.halfface_handle(s.face, 0)) == s.h0);
        TASK_CHECK(halfface_tuple(m, m.halfface_handle(s.face, 1)) == s.h1);
        TASK_CHECK(m.is_boundary(s.face) == s.boundary);
    }
    return 0;
}

static int cavity_halfface_adjacency()
{
    TopoMesh m;
    BipHandles h = build_bip(m);
    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(h.inner, created));

    // Every new cell: its four halffaces point back to it.
    for (CellHandle ch : created) {
        unsigned nHalffaces = 0;
        for (HalfFaceHandle hfh : m.cell_halffaces(ch)) {
            ++nHalffaces;
            TASK_CHECK(m.incident_cell(hfh) == ch);
        }
        TASK_CHECK(nHalffaces == 4);
    }

    // Per-halfface adjacency: internal faces have two distinct live
    // incident cells, boundary halffaces have none.
    for (FaceHandle fh : m.faces()) {
        const HalfFaceHandle h0 = m.halfface_handle(fh, 0);
        const HalfFaceHandle h1 = m.halfface_handle(fh, 1);
        const CellHandle c0 = m.incident_cell(h0);
        const CellHandle c1 = m.incident_cell(h1);
        if (m.is_boundary(fh)) {
            TASK_CHECK(!c0.is_valid() || !c1.is_valid());
            if (c0.is_valid()) TASK_CHECK(!m.is_deleted(c0));
            if (c1.is_valid()) TASK_CHECK(!m.is_deleted(c1));
        } else {
            TASK_CHECK(c0.is_valid() && c1.is_valid());
            TASK_CHECK(c0 != c1);
            TASK_CHECK(!m.is_deleted(c0) && !m.is_deleted(c1));
            TASK_CHECK(m.face_cells(fh)[0] == c0);
            TASK_CHECK(m.face_cells(fh)[1] == c1);
        }
    }
    return 0;
}

static int cavity_outside_neighbor()
{
    TopoMesh m;
    BipHandles h = build_bip(m);

    // Attach an external tet to the hull face {b,c,e} using a sixth
    // vertex. The old cell y's outward halfface is (c,e,b), so the
    // neighbor must use the opposite orientation (b,e,c).
    VertexHandle f = m.add_vertex();
    CellHandle neighbor = m.add_cell(h.b, h.e, h.c, f, true);
    TASK_CHECK(neighbor.is_valid());
    FaceHandle bce;
    {
        HalfFaceHandle found = m.find_halfface({h.b, h.e, h.c});
        TASK_CHECK(found.is_valid());
        bce = m.face_handle(found);
    }
    TASK_CHECK(!m.is_boundary(bce));

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(h.inner, created));
    TASK_CHECK(created.size() == 3);

    // The outside neighbor keeps its handle, its slot and its vertices.
    TASK_CHECK(!m.is_deleted(neighbor));
    auto neighborVerts = m.get_cell_vertices(neighbor);
    std::set<VertexHandle> neighborSet(neighborVerts.begin(), neighborVerts.end());
    TASK_CHECK(neighborSet.count(h.b) && neighborSet.count(h.c) &&
               neighborSet.count(h.e) && neighborSet.count(f));

    // The shared hull face now separates the neighbor from exactly one of
    // the new cells, on both halffaces.
    TASK_CHECK(!m.is_deleted(bce));
    const auto sides = m.face_cells(bce);
    TASK_CHECK(sides[0].is_valid() && sides[1].is_valid());
    TASK_CHECK(sides[0] != sides[1]);
    TASK_CHECK(sides[0] == neighbor || sides[1] == neighbor);
    const CellHandle newOwner = (sides[0] == neighbor) ? sides[1] : sides[0];
    TASK_CHECK(std::find(created.begin(), created.end(), newOwner) != created.end());

    // Inverse flip still works with the neighbor present.
    EdgeHandle newEdge = find_edge(m, h.d, h.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(newEdge, back));
    TASK_CHECK(back.size() == 2);
    TASK_CHECK(!m.is_deleted(neighbor));
    TASK_CHECK(!m.is_deleted(bce));
    TASK_CHECK(m.face_cells(bce)[0].is_valid() && m.face_cells(bce)[1].is_valid());
    return 0;
}

static int cavity_continuous_editing()
{
    TopoMesh m;
    BipHandles h = build_bip(m);
    VertexHandle f = m.add_vertex();
    CellHandle neighbor = m.add_cell(h.b, h.c, h.e, f, true);

    // A chain of inverse flips must keep the mesh usable at every step.
    std::vector<CellHandle> step1;
    TASK_CHECK(m.flip23(h.inner, step1));
    TASK_CHECK(step1.size() == 3);
    EdgeHandle e1 = find_edge(m, h.d, h.e);

    std::vector<CellHandle> step2;
    TASK_CHECK(m.flip32(e1, step2));
    TASK_CHECK(step2.size() == 2);

    FaceHandle innerAgain;
    for (FaceHandle fh : m.faces()) {
        if (!m.is_boundary(fh)) { innerAgain = fh; break; }
    }
    TASK_CHECK(innerAgain.is_valid());
    std::vector<CellHandle> step3;
    TASK_CHECK(m.flip23(innerAgain, step3));
    TASK_CHECK(step3.size() == 3);
    EdgeHandle e3 = find_edge(m, h.d, h.e);

    std::vector<CellHandle> step4;
    TASK_CHECK(m.flip32(e3, step4));
    TASK_CHECK(step4.size() == 2);

    TASK_CHECK(m.n_logical_cells() == 3); // two cavity tets + neighbor
    TASK_CHECK(m.n_vertices() == 6);
    TASK_CHECK(!m.is_deleted(neighbor));
    TASK_CHECK(count_boundary_faces(m) >= 6);

    // Cell-vertex traversal works on every live cell after the chain.
    for (CellHandle ch : m.cells()) {
        auto verts = m.get_cell_vertices(ch);
        TASK_CHECK(verts.size() == 4);
        std::set<VertexHandle> uniq(verts.begin(), verts.end());
        TASK_CHECK(uniq.size() == 4);
    }
    return 0;
}

// =========================================================================
// geometry group: signed-volume gating and scale invariance
// =========================================================================

static int geometry_roundtrip_volumes()
{
    GeomMesh m;
    BipHandles h = build_bip(m);
    const double before = total_signed_volume(m);
    TASK_CHECK(before > 0.0);

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(h.inner, created));
    TASK_CHECK(created.size() == 3);
    for (CellHandle ch : created) TASK_CHECK(m.signed_volume(ch) > 0.0);
    double after23 = total_signed_volume(m);
    TASK_CHECK(std::abs(after23 - before) < 1e-10);

    EdgeHandle newEdge = find_edge(m, h.d, h.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(newEdge, back));
    TASK_CHECK(back.size() == 2);
    for (CellHandle ch : back) TASK_CHECK(m.signed_volume(ch) > 0.0);
    double after32 = total_signed_volume(m);
    TASK_CHECK(std::abs(after32 - before) < 1e-10);
    TASK_CHECK(count_boundary_faces(m) == 6);
    return 0;
}

static int geometry_reject_folded_cavity()
{
    GeomMesh m;
    VertexHandle a = m.add_vertex({0.0, 0.0, 0.0});
    VertexHandle b = m.add_vertex({1.0, 0.0, 0.0});
    VertexHandle c = m.add_vertex({0.0, 1.0, 0.0});
    VertexHandle d = m.add_vertex({0.2, 0.1, 1.0});
    // Far-away opposite vertex: old tets stay positive, but a candidate tet
    // of the 2->3 flip folds outside the cavity (negative signed volume).
    VertexHandle e = m.add_vertex({10.0, 10.0, -1.0});
    CellHandle x = m.add_cell(a, b, c, d, true);
    CellHandle y = m.add_cell(a, c, b, e, true);
    TASK_CHECK(m.signed_volume(x) > 0.0 && m.signed_volume(y) > 0.0);
    FaceHandle inner;
    for (FaceHandle fh : m.faces()) if (!m.is_boundary(fh)) inner = fh;

    const size_t nCells = m.n_cells(), nFaces = m.n_faces(), nEdges = m.n_edges();
    std::vector<CellHandle> sentinel{CellHandle(4242)};
    TASK_CHECK(!m.flip23(inner, sentinel));
    TASK_CHECK(sentinel.size() == 1 && sentinel[0] == CellHandle(4242));
    TASK_CHECK(m.n_cells() == nCells && m.n_faces() == nFaces && m.n_edges() == nEdges);
    TASK_CHECK(!m.is_deleted(x) && !m.is_deleted(y) && !m.is_deleted(inner));
    TASK_CHECK(m.n_logical_cells() == 2);
    return 0;
}

static int geometry_reject_sliver()
{
    GeomMesh m;
    VertexHandle a = m.add_vertex({0.0, 0.0, 0.0});
    VertexHandle b = m.add_vertex({1.0, 0.0, 0.0});
    VertexHandle c = m.add_vertex({0.0, 1.0, 0.0});
    VertexHandle d = m.add_vertex({0.2, 0.1, 1.0});
    // e is almost coplanar with abc: a replacement tet degenerates.
    VertexHandle e = m.add_vertex({0.5, 0.5, -1e-13});
    CellHandle x = m.add_cell(a, b, c, d, true);
    CellHandle y = m.add_cell(a, c, b, e, true);
    TASK_CHECK(m.signed_volume(x) > 0.0 && m.signed_volume(y) > 0.0);
    FaceHandle inner;
    for (FaceHandle fh : m.faces()) if (!m.is_boundary(fh)) inner = fh;

    std::vector<CellHandle> created;
    TASK_CHECK(!m.flip23(inner, created)); // default 1e-12 threshold
    TASK_CHECK(created.empty());
    TASK_CHECK(m.n_logical_cells() == 2);
    TASK_CHECK(!m.is_deleted(x) && !m.is_deleted(y));
    return 0;
}

static int geometry_non_unit_scale()
{
    for (double s : {1000.0, 1e-3}) {
        GeomMesh m;
        BipHandles h = build_bip(m, s);
        const double before = total_signed_volume(m);

        std::vector<CellHandle> created;
        TASK_CHECK(m.flip23(h.inner, created));
        TASK_CHECK(created.size() == 3);
        TASK_CHECK(std::abs(total_signed_volume(m) - before) < 1e-6 * std::abs(before));

        EdgeHandle newEdge = find_edge(m, h.d, h.e);
        std::vector<CellHandle> back;
        TASK_CHECK(m.flip32(newEdge, back));
        TASK_CHECK(back.size() == 2);
        TASK_CHECK(std::abs(total_signed_volume(m) - before) < 1e-6 * std::abs(before));

        // The volume scales with s^3, proving the threshold is relative.
        GeomMesh ref;
        BipHandles href = build_bip(ref, 1.0);
        (void)href;
        TASK_CHECK(std::abs(before - total_signed_volume(ref) * s * s * s) <
                   1e-6 * std::abs(before));
    }
    return 0;
}

static int geometry_negative_old_cell_rejected()
{
    GeomMesh m;
    VertexHandle a = m.add_vertex({0.0, 0.0, 0.0});
    VertexHandle b = m.add_vertex({1.0, 0.0, 0.0});
    VertexHandle c = m.add_vertex({0.0, 1.0, 0.0});
    // d lies on the negative side of plane abc: the first tet then has a
    // negative signed volume while the connectivity stays a valid
    // two-cell complex sharing face abc.
    VertexHandle d = m.add_vertex({0.2, 0.1, -1.0});
    VertexHandle e = m.add_vertex({0.3, 0.2, -2.0});
    CellHandle x = m.add_cell(a, b, c, d, true);
    CellHandle y = m.add_cell(a, c, b, e, true);
    TASK_CHECK(m.signed_volume(x) < 0.0);
    TASK_CHECK(m.signed_volume(y) > 0.0);
    FaceHandle inner;
    for (FaceHandle fh : m.faces()) if (!m.is_boundary(fh)) inner = fh;
    TASK_CHECK(inner.is_valid());

    std::vector<CellHandle> created;
    TASK_CHECK(!m.flip23(inner, created));
    TASK_CHECK(created.empty());
    TASK_CHECK(m.n_logical_cells() == 2);

    // The purely topological kernel accepts the same combinatorial input.
    TopoMesh tm;
    BipHandles th = build_bip(tm);
    (void)th;
    std::vector<CellHandle> topoCreated;
    FaceHandle topoInner;
    for (FaceHandle fh : tm.faces()) if (!tm.is_boundary(fh)) topoInner = fh;
    TASK_CHECK(tm.flip23(topoInner, topoCreated));
    TASK_CHECK(topoCreated.size() == 3);
    return 0;
}

static int geometry_threshold_parameter()
{
    GeomMesh m;
    BipHandles h = build_bip(m);

    // An absolute threshold of 1.0 can never be met by a unit-sized cavity.
    std::vector<CellHandle> rejected;
    TASK_CHECK(!m.flip23(h.inner, rejected, 1.0));
    TASK_CHECK(rejected.empty());
    TASK_CHECK(m.n_logical_cells() == 2);

    // The default relative threshold accepts the well-shaped bipyramid.
    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(h.inner, created));
    TASK_CHECK(created.size() == 3);
    for (size_t i = 1; i < created.size(); ++i)
        TASK_CHECK(created[i - 1] < created[i]);

    EdgeHandle newEdge = find_edge(m, h.d, h.e);
    std::vector<CellHandle> rejected32;
    TASK_CHECK(!m.flip32(newEdge, rejected32, 1.0));
    TASK_CHECK(rejected32.empty());
    TASK_CHECK(m.n_logical_cells() == 3);
    return 0;
}

// =========================================================================
// state group: switches, properties, deferred deletion, failure atomicity
// =========================================================================

struct CellTag {
    int id = -1;
    std::string label;
    bool operator==(const CellTag& _other) const
    {
        return id == _other.id && label == _other.label;
    }
};

static int state_switches_required()
{
    {
        TopoMesh m;
        build_bip(m);
        m.enable_deferred_deletion(false); // performs garbage collection
        TASK_CHECK(!m.deferred_deletion_enabled());
        FaceHandle inner;
        for (FaceHandle fh : m.faces()) if (!m.is_boundary(fh)) inner = fh;
        std::vector<CellHandle> created;
        TASK_CHECK(!m.flip23(inner, created));
        TASK_CHECK(created.empty());

        m.enable_deferred_deletion(true);
        TASK_CHECK(m.flip23(inner, created));
        TASK_CHECK(created.size() == 3);
        TASK_CHECK(m.deferred_deletion_enabled()); // flip must not toggle it
    }
    {
        TopoMesh m;
        BipHandles h = build_bip(m);
        m.enable_vertex_bottom_up_incidences(false);
        std::vector<CellHandle> created;
        TASK_CHECK(!m.flip23(h.inner, created));
        m.enable_bottom_up_incidences(true);
        TASK_CHECK(m.flip23(h.inner, created));
        TASK_CHECK(created.size() == 3);

        EdgeHandle newEdge = find_edge(m, h.d, h.e);
        m.enable_face_bottom_up_incidences(false);
        std::vector<CellHandle> back;
        TASK_CHECK(!m.flip32(newEdge, back));
        m.enable_face_bottom_up_incidences(true);
        TASK_CHECK(m.flip32(newEdge, back));
        TASK_CHECK(back.size() == 2);
    }
    return 0;
}

static int state_failure_atomicity()
{
    TopoMesh m;
    BipHandles h = build_bip(m);

    const size_t nv = m.n_vertices(), ne = m.n_edges();
    const size_t nf = m.n_faces(), nc = m.n_cells();
    std::vector<bool> deletedCells(nc), deletedFaces(nf), deletedEdges(ne);
    for (size_t i = 0; i < nc; ++i) deletedCells[i] = m.is_deleted(CellHandle((int)i));
    for (size_t i = 0; i < nf; ++i) deletedFaces[i] = m.is_deleted(FaceHandle((int)i));
    for (size_t i = 0; i < ne; ++i) deletedEdges[i] = m.is_deleted(EdgeHandle((int)i));

    FaceHandle boundary = first_boundary_face(m);
    TASK_CHECK(boundary.is_valid());
    EdgeHandle boundaryEdge;
    for (EdgeHandle eh : m.edges()) {
        if (m.is_boundary(eh)) { boundaryEdge = eh; break; }
    }
    TASK_CHECK(boundaryEdge.is_valid());

    std::vector<CellHandle> out{CellHandle(7), CellHandle(8)};
    TASK_CHECK(!m.flip23(boundary, out));            // boundary face
    TASK_CHECK(out.size() == 2 && out[0] == CellHandle(7) && out[1] == CellHandle(8));
    TASK_CHECK(!m.flip23(FaceHandle(9999), out));    // invalid handle
    TASK_CHECK(out.size() == 2);
    TASK_CHECK(!m.flip32(boundaryEdge, out));        // boundary edge
    TASK_CHECK(out.size() == 2);
    TASK_CHECK(!m.flip32(EdgeHandle(9999), out));    // invalid handle
    TASK_CHECK(out.size() == 2);

    TASK_CHECK(m.n_vertices() == nv && m.n_edges() == ne &&
               m.n_faces() == nf && m.n_cells() == nc);
    TASK_CHECK(m.n_logical_cells() == 2);
    for (size_t i = 0; i < nc; ++i)
        TASK_CHECK(m.is_deleted(CellHandle((int)i)) == deletedCells[i]);
    for (size_t i = 0; i < nf; ++i)
        TASK_CHECK(m.is_deleted(FaceHandle((int)i)) == deletedFaces[i]);
    for (size_t i = 0; i < ne; ++i)
        TASK_CHECK(m.is_deleted(EdgeHandle((int)i)) == deletedEdges[i]);

    // A successful flip remains possible right after the rejections.
    TASK_CHECK(m.flip23(h.inner, out));
    TASK_CHECK(out.size() == 3);
    return 0;
}

static int state_custom_properties()
{
    TopoMesh m;
    BipHandles h = build_bip(m);

    auto cellTag = m.request_cell_property<CellTag>("cellTag", CellTag{});
    auto faceInt = m.request_face_property<int>("faceInt", 0);
    auto edgeInt = m.request_edge_property<int>("edgeInt", 0);

    cellTag[h.x] = {1, "alpha"};
    cellTag[h.y] = {2, "beta"};
    const CellTag sourceTag = (h.x < h.y) ? cellTag[h.x] : cellTag[h.y];
    const CellHandle sourceHandle = std::min(h.x, h.y);

    int faceCounter = 100;
    for (FaceHandle fh : m.faces()) faceInt[fh] = ++faceCounter;
    int edgeCounter = 1000;
    for (EdgeHandle eh : m.edges()) edgeInt[eh] = ++edgeCounter;

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(h.inner, created));
    TASK_CHECK(created.size() == 3);

    // Every replacement cell copies all cell properties (custom type
    // included) from the removed cell with the smallest handle.
    TASK_CHECK(sourceHandle == h.x || sourceHandle == h.y);
    for (CellHandle ch : created) TASK_CHECK(cellTag[ch] == sourceTag);

    // Hull faces keep their property values; new internal faces get the
    // default; the deleted internal face is untouched in its old slot.
    for (FaceHandle fh : m.faces()) {
        if (fh == h.inner) continue;
        if (m.is_deleted(fh)) continue;
        if (faceInt[fh] != 0) {
            TASK_CHECK(faceInt[fh] > 100 && faceInt[fh] <= faceCounter);
        }
    }
    unsigned defaultFaces = 0;
    for (FaceHandle fh : m.faces()) {
        if (!m.is_deleted(fh) && faceInt[fh] == 0) ++defaultFaces;
    }
    TASK_CHECK(defaultFaces == 3); // three new internal faces

    // The new internal edge carries the default edge property, hull edges
    // keep theirs.
    EdgeHandle newEdge = find_edge(m, h.d, h.e);
    TASK_CHECK(newEdge.is_valid());
    TASK_CHECK(edgeInt[newEdge] == 0);
    for (EdgeHandle eh : m.edges()) {
        if (!m.is_deleted(eh) && eh != newEdge) TASK_CHECK(edgeInt[eh] > 1000);
    }

    // 3->2: the new cells inherit from the smallest of the removed handles,
    // whose tag is itself the inherited source tag.
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(newEdge, back));
    TASK_CHECK(back.size() == 2);
    for (CellHandle ch : back) TASK_CHECK(cellTag[ch] == sourceTag);
    return 0;
}

static int state_deferred_slots_and_iteration()
{
    TopoMesh m;
    BipHandles h = build_bip(m);
    const size_t physicalCells = m.n_cells();
    const size_t physicalFaces = m.n_faces();

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(h.inner, created));

    // No garbage collection: deleted slots remain present and flagged,
    // the physical arrays only grow.
    TASK_CHECK(m.needs_garbage_collection());
    TASK_CHECK(m.n_cells() == physicalCells + 3);
    TASK_CHECK(m.n_logical_cells() == 3);
    TASK_CHECK(m.n_faces() == physicalFaces + 3);
    TASK_CHECK(m.n_logical_faces() == 9);
    TASK_CHECK(m.is_deleted(h.x) && m.is_deleted(h.y));

    // Range iteration skips the deleted slots automatically.
    unsigned seen = 0;
    for (CellHandle ch : m.cells()) {
        TASK_CHECK(!m.is_deleted(ch));
        ++seen;
    }
    TASK_CHECK(seen == 3);

    // A second edit works without any garbage collection in between.
    EdgeHandle newEdge = find_edge(m, h.d, h.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(newEdge, back));
    TASK_CHECK(back.size() == 2);
    TASK_CHECK(m.needs_garbage_collection());
    TASK_CHECK(m.n_logical_cells() == 2);
    seen = 0;
    for (CellHandle ch : m.cells()) {
        TASK_CHECK(!m.is_deleted(ch));
        ++seen;
    }
    TASK_CHECK(seen == 2);

    // The original vertex handles are still valid and unmodified.
    for (VertexHandle vh : std::array<VertexHandle, 5>{h.a, h.b, h.c, h.d, h.e}) {
        TASK_CHECK(vh.is_valid() && !m.is_deleted(vh));
    }
    return 0;
}

static int state_rejections_and_conflicts()
{
    // Boundary face / non-valence-three edge / invalid handles.
    {
        TopoMesh m;
        build_bip(m);
        FaceHandle boundary = first_boundary_face(m);
        std::vector<CellHandle> out;
        TASK_CHECK(!m.flip23(boundary, out));
        TASK_CHECK(!m.flip23(TopoMesh::InvalidFaceHandle, out));
        TASK_CHECK(!m.flip32(TopoMesh::InvalidEdgeHandle, out));

        EdgeHandle boundaryEdge;
        for (EdgeHandle eh : m.edges()) {
            if (m.is_boundary(eh)) { boundaryEdge = eh; break; }
        }
        TASK_CHECK(boundaryEdge.is_valid());
        TASK_CHECK(!m.flip32(boundaryEdge, out));
        TASK_CHECK(out.empty());
    }

    // 2->3 conflict: candidate internal edge already exists as a live edge.
    {
        TopoMesh m;
        BipHandles h = build_bip(m);
        EdgeHandle obstruction = m.add_edge(h.d, h.e);
        TASK_CHECK(obstruction.is_valid() && !m.is_deleted(obstruction));
        std::vector<CellHandle> out;
        TASK_CHECK(!m.flip23(h.inner, out));
        TASK_CHECK(out.empty());
        TASK_CHECK(m.n_logical_cells() == 2);
        m.delete_edge(obstruction);
        TASK_CHECK(m.flip23(h.inner, out));
        TASK_CHECK(out.size() == 3);
    }

    // 3->2 conflict: candidate internal face already exists as a live face.
    {
        TopoMesh m;
        auto a = m.add_vertex(), b = m.add_vertex();
        auto c = m.add_vertex(), d = m.add_vertex(), e = m.add_vertex();
        m.add_cell(a, b, c, d, true);
        m.add_cell(a, b, d, e, true);
        m.add_cell(a, b, e, c, true);
        EdgeHandle ab = find_edge(m, a, b);
        TASK_CHECK(ab.is_valid() && m.valence(ab) == 3 && !m.is_boundary(ab));
        FaceHandle obstruction = m.add_face(std::vector<VertexHandle>{c, d, e});
        TASK_CHECK(obstruction.is_valid());
        std::vector<CellHandle> out;
        TASK_CHECK(!m.flip32(ab, out));
        TASK_CHECK(out.empty());
        m.delete_face(obstruction);
        TASK_CHECK(m.flip32(ab, out));
        TASK_CHECK(out.size() == 2);
    }

    // Five vertices not pairwise distinct (self-adjacent degenerate cavity).
    {
        TopoMesh m;
        auto a = m.add_vertex(), b = m.add_vertex();
        auto c = m.add_vertex(), d = m.add_vertex();
        m.add_cell(a, b, c, d, true);
        m.add_cell(a, c, b, d, true); // same four vertices, opposite apex d
        FaceHandle inner;
        for (FaceHandle fh : m.faces()) if (!m.is_boundary(fh)) inner = fh;
        std::vector<CellHandle> out;
        TASK_CHECK(!m.flip23(inner, out));
        TASK_CHECK(out.empty());
    }

    // Non-spherical link around a valence-three edge.
    {
        TopoMesh m;
        auto a = m.add_vertex(), b = m.add_vertex();
        auto c = m.add_vertex(), d = m.add_vertex(), e = m.add_vertex();
        m.add_cell(a, b, c, d, true);
        m.add_cell(a, b, d, c, true); // reuses c,d: link collapses
        m.add_cell(a, b, c, e, true);
        EdgeHandle ab = find_edge(m, a, b);
        std::vector<CellHandle> out;
        TASK_CHECK(!m.flip32(ab, out));
        TASK_CHECK(out.empty());
    }
    return 0;
}

static int state_created_handles_complete()
{
    TopoMesh m;
    BipHandles h = build_bip(m);

    std::set<CellHandle> before;
    for (CellHandle ch : m.cells()) before.insert(ch);

    std::vector<CellHandle> created;
    TASK_CHECK(m.flip23(h.inner, created));
    std::set<CellHandle> after23;
    for (CellHandle ch : m.cells()) after23.insert(ch);
    TASK_CHECK(after23.size() == created.size());
    for (CellHandle ch : created) {
        TASK_CHECK(after23.count(ch) > 0);
        TASK_CHECK(before.count(ch) == 0); // genuinely new handles
    }
    TASK_CHECK(std::is_sorted(created.begin(), created.end()));

    EdgeHandle newEdge = find_edge(m, h.d, h.e);
    std::vector<CellHandle> back;
    TASK_CHECK(m.flip32(newEdge, back));
    std::set<CellHandle> after32;
    for (CellHandle ch : m.cells()) after32.insert(ch);
    TASK_CHECK(after32.size() == back.size());
    for (CellHandle ch : back) {
        TASK_CHECK(after32.count(ch) > 0);
        TASK_CHECK(after23.count(ch) == 0);
    }
    TASK_CHECK(std::is_sorted(back.begin(), back.end()));
    return 0;
}

} // anonymous namespace

const struct task_case task_cases[] = {
    {"cavity", "roundtrip", cavity_roundtrip},
    {"cavity", "hull_preservation", cavity_hull_preservation},
    {"cavity", "halfface_adjacency", cavity_halfface_adjacency},
    {"cavity", "outside_neighbor", cavity_outside_neighbor},
    {"cavity", "continuous_editing", cavity_continuous_editing},

    {"geometry", "roundtrip_volumes", geometry_roundtrip_volumes},
    {"geometry", "reject_folded_cavity", geometry_reject_folded_cavity},
    {"geometry", "reject_sliver", geometry_reject_sliver},
    {"geometry", "non_unit_scale", geometry_non_unit_scale},
    {"geometry", "negative_old_cell", geometry_negative_old_cell_rejected},
    {"geometry", "threshold_parameter", geometry_threshold_parameter},

    {"state", "switches_required", state_switches_required},
    {"state", "failure_atomicity", state_failure_atomicity},
    {"state", "custom_properties", state_custom_properties},
    {"state", "deferred_slots", state_deferred_slots_and_iteration},
    {"state", "rejections_and_conflicts", state_rejections_and_conflicts},
    {"state", "created_handles_complete", state_created_handles_complete},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
