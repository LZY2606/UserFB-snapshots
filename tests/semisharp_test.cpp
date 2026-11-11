// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include "pmp/algorithms/subdivision.h"
#include "pmp/exceptions.h"

using namespace pmp;

namespace {

// ----- asymmetric test meshes -----

// closed quad mesh: deformed cube, 8 vertices, 6 quads
SurfaceMesh asymmetric_hexahedron()
{
    SurfaceMesh mesh;
    auto v0 = mesh.add_vertex(Point(0.0, 0.0, 0.0));
    auto v1 = mesh.add_vertex(Point(1.3, 0.1, -0.2));
    auto v2 = mesh.add_vertex(Point(1.1, 1.2, 0.3));
    auto v3 = mesh.add_vertex(Point(-0.2, 0.9, 0.1));
    auto v4 = mesh.add_vertex(Point(0.1, -0.1, 1.4));
    auto v5 = mesh.add_vertex(Point(1.2, 0.2, 1.1));
    auto v6 = mesh.add_vertex(Point(0.9, 1.3, 1.2));
    auto v7 = mesh.add_vertex(Point(-0.3, 1.0, 0.9));
    mesh.add_face({v0, v3, v2, v1});
    mesh.add_face({v4, v5, v6, v7});
    mesh.add_face({v0, v1, v5, v4});
    mesh.add_face({v1, v2, v6, v5});
    mesh.add_face({v2, v3, v7, v6});
    mesh.add_face({v3, v0, v4, v7});
    return mesh;
}

// closed triangle mesh: deformed octahedron, 6 vertices, 8 triangles
SurfaceMesh asymmetric_octahedron()
{
    SurfaceMesh mesh;
    auto v0 = mesh.add_vertex(Point(1.2, 0.1, -0.3));
    auto v1 = mesh.add_vertex(Point(-0.4, 1.1, 0.2));
    auto v2 = mesh.add_vertex(Point(-1.3, -0.2, 0.4));
    auto v3 = mesh.add_vertex(Point(0.3, -1.2, -0.1));
    auto v4 = mesh.add_vertex(Point(0.2, 0.3, 1.4));
    auto v5 = mesh.add_vertex(Point(-0.1, 0.2, -1.1));
    mesh.add_face({v0, v1, v4});
    mesh.add_face({v1, v2, v4});
    mesh.add_face({v2, v3, v4});
    mesh.add_face({v3, v0, v4});
    mesh.add_face({v1, v0, v5});
    mesh.add_face({v2, v1, v5});
    mesh.add_face({v3, v2, v5});
    mesh.add_face({v0, v3, v5});
    return mesh;
}

// open quad mesh: deformed 3x3 vertex grid, 9 vertices, 4 quads
SurfaceMesh asymmetric_quad_grid()
{
    SurfaceMesh mesh;
    auto v00 = mesh.add_vertex(Point(0.0, 0.0, 0.1));
    auto v10 = mesh.add_vertex(Point(1.1, 0.1, -0.2));
    auto v20 = mesh.add_vertex(Point(2.2, -0.2, 0.3));
    auto v01 = mesh.add_vertex(Point(-0.1, 1.2, 0.2));
    auto v11 = mesh.add_vertex(Point(1.0, 1.1, 0.4));
    auto v21 = mesh.add_vertex(Point(2.3, 1.0, -0.1));
    auto v02 = mesh.add_vertex(Point(0.2, 2.1, -0.3));
    auto v12 = mesh.add_vertex(Point(0.9, 2.2, 0.1));
    auto v22 = mesh.add_vertex(Point(2.1, 2.3, 0.2));
    mesh.add_face({v00, v10, v11, v01});
    mesh.add_face({v10, v20, v21, v11});
    mesh.add_face({v01, v11, v12, v02});
    mesh.add_face({v11, v21, v22, v12});
    return mesh;
}

// open triangle mesh: deformed fan of five triangles around an interior
// center vertex, 6 vertices, 5 triangles
SurfaceMesh asymmetric_triangle_fan()
{
    SurfaceMesh mesh;
    auto c = mesh.add_vertex(Point(0.1, 0.2, 0.3));
    auto r0 = mesh.add_vertex(Point(1.2, 0.0, -0.1));
    auto r1 = mesh.add_vertex(Point(0.3, 1.1, 0.2));
    auto r2 = mesh.add_vertex(Point(-1.0, 0.4, -0.2));
    auto r3 = mesh.add_vertex(Point(-0.4, -1.1, 0.1));
    auto r4 = mesh.add_vertex(Point(0.9, -0.8, 0.4));
    mesh.add_face({c, r0, r1});
    mesh.add_face({c, r1, r2});
    mesh.add_face({c, r2, r3});
    mesh.add_face({c, r3, r4});
    mesh.add_face({c, r4, r0});
    return mesh;
}

Edge find_edge(const SurfaceMesh& mesh, Vertex a, Vertex b)
{
    const auto h = mesh.find_halfedge(a, b);
    EXPECT_TRUE(h.is_valid());
    return mesh.edge(h);
}

// ----- comparison helpers -----

void expect_point_eq(const Point& a, const Point& b)
{
    EXPECT_EQ(a[0], b[0]);
    EXPECT_EQ(a[1], b[1]);
    EXPECT_EQ(a[2], b[2]);
}

void expect_point_near(const Point& a, const Point& b, double tol = 1e-5)
{
    EXPECT_NEAR(a[0], b[0], tol);
    EXPECT_NEAR(a[1], b[1], tol);
    EXPECT_NEAR(a[2], b[2], tol);
}

// ----- reference formulas transcribed from the semi-sharp crease spec -----

Scalar ref_clamp(Scalar s)
{
    return std::clamp(s, Scalar(0), Scalar(1));
}

// crease blend of the smooth vertex position: the two strongest incident
// creases (permanent hard edges count as infinitely sharp) define the crease
// position (6P+N1+N2)/8, the third strongest fades towards the corner P
Point ref_crease_blend(const SurfaceMesh& mesh, Vertex v, const Point& smooth)
{
    auto points = mesh.get_vertex_property<Point>("v:point");
    auto efeature = mesh.get_edge_property<bool>("e:feature");
    auto esharp = mesh.get_edge_property<Scalar>("e:sharpness");

    std::vector<std::pair<Scalar, Point>> creases;
    for (auto h : mesh.halfedges(v))
    {
        const auto e = mesh.edge(h);
        Scalar s = 0;
        if (efeature && efeature[e])
            s = std::numeric_limits<Scalar>::infinity();
        else if (esharp)
            s = esharp[e];
        creases.emplace_back(s, points[mesh.to_vertex(h)]);
    }
    std::sort(creases.begin(), creases.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    const Scalar u2 =
        creases.size() > 1 ? ref_clamp(creases[1].first) : Scalar(0);
    const Scalar u3 =
        creases.size() > 2 ? ref_clamp(creases[2].first) : Scalar(0);

    Point crease = points[v];
    crease *= 6;
    crease += creases[0].second;
    crease += creases.size() > 1 ? creases[1].second : Point(0, 0, 0);
    crease *= Scalar(0.125);

    return (1 - u2) * smooth + (u2 - u3) * crease + u3 * points[v];
}

// expected positions of one subdivision step: new positions of old vertices,
// new points on old edges, and (Catmull-Clark only) new face points
struct StepReference
{
    std::vector<Point> vertices;
    std::vector<Point> edges;
    std::vector<Point> faces;
};

StepReference reference_cc_step(const SurfaceMesh& mesh,
                                BoundaryHandling boundary_handling)
{
    auto points = mesh.get_vertex_property<Point>("v:point");
    auto vfeature = mesh.get_vertex_property<bool>("v:feature");
    auto efeature = mesh.get_edge_property<bool>("e:feature");
    auto esharp = mesh.get_edge_property<Scalar>("e:sharpness");

    StepReference ref;

    ref.faces.resize(mesh.n_faces());
    for (auto f : mesh.faces())
    {
        Point c(0, 0, 0);
        Scalar n = 0;
        for (auto fv : mesh.vertices(f))
        {
            c += points[fv];
            n += 1;
        }
        ref.faces[f.idx()] = c / n;
    }

    ref.edges.resize(mesh.n_edges());
    for (auto e : mesh.edges())
    {
        const Point sharp = Scalar(0.5) * (points[mesh.vertex(e, 0)] +
                                           points[mesh.vertex(e, 1)]);
        if (mesh.is_boundary(e) || (efeature && efeature[e]))
        {
            ref.edges[e.idx()] = sharp;
        }
        else
        {
            Point smooth(0, 0, 0);
            smooth += points[mesh.vertex(e, 0)];
            smooth += points[mesh.vertex(e, 1)];
            smooth += ref.faces[mesh.face(e, 0).idx()];
            smooth += ref.faces[mesh.face(e, 1).idx()];
            smooth *= Scalar(0.25);
            const Scalar s = esharp ? esharp[e] : Scalar(0);
            ref.edges[e.idx()] =
                (1 - ref_clamp(s)) * smooth + ref_clamp(s) * sharp;
        }
    }

    ref.vertices.resize(mesh.n_vertices());
    for (auto v : mesh.vertices())
    {
        if (mesh.is_isolated(v))
        {
            ref.vertices[v.idx()] = points[v];
        }
        else if (mesh.is_boundary(v))
        {
            if (boundary_handling == BoundaryHandling::Preserve)
            {
                ref.vertices[v.idx()] = points[v];
            }
            else
            {
                auto h1 = mesh.halfedge(v);
                auto h0 = mesh.prev_halfedge(h1);
                Point p = points[v];
                p *= 6;
                p += points[mesh.to_vertex(h1)];
                p += points[mesh.from_vertex(h0)];
                p *= Scalar(0.125);
                ref.vertices[v.idx()] = p;
            }
        }
        else if (vfeature && vfeature[v])
        {
            Point p = points[v];
            p *= 6;
            int count = 0;
            for (auto h : mesh.halfedges(v))
            {
                if (efeature[mesh.edge(h)])
                {
                    p += points[mesh.to_vertex(h)];
                    ++count;
                }
            }
            ref.vertices[v.idx()] =
                count == 2 ? p * Scalar(0.125) : points[v];
        }
        else
        {
            const Scalar k = mesh.valence(v);
            Point smooth(0, 0, 0);
            for (auto vv : mesh.vertices(v))
                smooth += points[vv];
            for (auto f : mesh.faces(v))
                smooth += ref.faces[f.idx()];
            smooth /= (k * k);
            smooth += ((k - 2) / k) * points[v];
            ref.vertices[v.idx()] =
                esharp ? ref_crease_blend(mesh, v, smooth) : smooth;
        }
    }

    return ref;
}

StepReference reference_loop_step(const SurfaceMesh& mesh,
                                  BoundaryHandling boundary_handling)
{
    auto points = mesh.get_vertex_property<Point>("v:point");
    auto vfeature = mesh.get_vertex_property<bool>("v:feature");
    auto efeature = mesh.get_edge_property<bool>("e:feature");
    auto esharp = mesh.get_edge_property<Scalar>("e:sharpness");

    StepReference ref;

    ref.vertices.resize(mesh.n_vertices());
    for (auto v : mesh.vertices())
    {
        if (mesh.is_isolated(v))
        {
            ref.vertices[v.idx()] = points[v];
        }
        else if (mesh.is_boundary(v))
        {
            if (boundary_handling == BoundaryHandling::Preserve)
            {
                ref.vertices[v.idx()] = points[v];
            }
            else
            {
                auto h1 = mesh.halfedge(v);
                auto h0 = mesh.prev_halfedge(h1);
                Point p = points[v];
                p *= 6;
                p += points[mesh.to_vertex(h1)];
                p += points[mesh.from_vertex(h0)];
                p *= Scalar(0.125);
                ref.vertices[v.idx()] = p;
            }
        }
        else if (vfeature && vfeature[v])
        {
            Point p = points[v];
            p *= 6;
            int count = 0;
            for (auto h : mesh.halfedges(v))
            {
                if (efeature[mesh.edge(h)])
                {
                    p += points[mesh.to_vertex(h)];
                    ++count;
                }
            }
            ref.vertices[v.idx()] =
                count == 2 ? p * Scalar(0.125) : points[v];
        }
        else
        {
            Point avg(0, 0, 0);
            Scalar k = 0;
            for (auto vv : mesh.vertices(v))
            {
                avg += points[vv];
                k += 1;
            }
            avg /= k;
            const Scalar beta =
                (0.625 -
                 pow(0.375 + 0.25 * std::cos(2.0 * std::numbers::pi / k), 2.0));
            const Point smooth = points[v] * (1 - beta) + beta * avg;
            ref.vertices[v.idx()] =
                esharp ? ref_crease_blend(mesh, v, smooth) : smooth;
        }
    }

    ref.edges.resize(mesh.n_edges());
    for (auto e : mesh.edges())
    {
        const Point sharp = Scalar(0.5) * (points[mesh.vertex(e, 0)] +
                                           points[mesh.vertex(e, 1)]);
        if (mesh.is_boundary(e) || (efeature && efeature[e]))
        {
            ref.edges[e.idx()] = sharp;
        }
        else
        {
            auto h0 = mesh.halfedge(e, 0);
            auto h1 = mesh.halfedge(e, 1);
            Point smooth = points[mesh.to_vertex(h0)];
            smooth += points[mesh.to_vertex(h1)];
            smooth *= 3;
            smooth += points[mesh.to_vertex(mesh.next_halfedge(h0))];
            smooth += points[mesh.to_vertex(mesh.next_halfedge(h1))];
            smooth *= Scalar(0.125);
            const Scalar s = esharp ? esharp[e] : Scalar(0);
            ref.edges[e.idx()] =
                (1 - ref_clamp(s)) * smooth + ref_clamp(s) * sharp;
        }
    }

    return ref;
}

// compare the mesh against the reference after one Catmull-Clark step:
// old vertices keep their indices, followed by edge points and face points
// in edge/face index order
void expect_cc_step(const SurfaceMesh& mesh, const StepReference& ref,
                    size_t nv, size_t ne, size_t nf)
{
    ASSERT_EQ(mesh.n_vertices(), nv + ne + nf);
    for (size_t i = 0; i < nv; ++i)
        expect_point_near(mesh.position(Vertex(static_cast<IndexType>(i))),
                          ref.vertices[i]);
    for (size_t i = 0; i < ne; ++i)
        expect_point_near(
            mesh.position(Vertex(static_cast<IndexType>(nv + i))),
            ref.edges[i]);
    for (size_t i = 0; i < nf; ++i)
        expect_point_near(
            mesh.position(Vertex(static_cast<IndexType>(nv + ne + i))),
            ref.faces[i]);
}

// compare the mesh against the reference after one Loop step: old vertices
// keep their indices, followed by edge points in edge index order
void expect_loop_step(const SurfaceMesh& mesh, const StepReference& ref,
                      size_t nv, size_t ne)
{
    ASSERT_EQ(mesh.n_vertices(), nv + ne);
    for (size_t i = 0; i < nv; ++i)
        expect_point_near(mesh.position(Vertex(static_cast<IndexType>(i))),
                          ref.vertices[i]);
    for (size_t i = 0; i < ne; ++i)
        expect_point_near(
            mesh.position(Vertex(static_cast<IndexType>(nv + i))),
            ref.edges[i]);
}

// count edges with a given sharpness value
size_t count_sharpness(const SurfaceMesh& mesh,
                       const EdgeProperty<Scalar>& esharp, Scalar value)
{
    size_t count = 0;
    for (auto e : mesh.edges())
        if (esharp[e] == value)
            ++count;
    return count;
}

} // namespace

// pointwise check of a single step on a closed asymmetric mesh for
// sharpness values below, at, and above the fully sharp limit
TEST(SemiSharpTest, catmull_clark_sharpness_pointwise)
{
    for (const Scalar strength : {Scalar(0.5), Scalar(1), Scalar(1.5), Scalar(2.5)})
    {
        auto mesh = asymmetric_hexahedron();
        const auto nv = mesh.n_vertices();
        const auto ne = mesh.n_edges();
        const auto nf = mesh.n_faces();

        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        esharp[find_edge(mesh, Vertex(0), Vertex(1))] = strength;

        const auto ref = reference_cc_step(mesh, BoundaryHandling::Interpolate);
        catmull_clark_subdivision(mesh);
        expect_cc_step(mesh, ref, nv, ne, nf);
    }
}

TEST(SemiSharpTest, loop_sharpness_pointwise)
{
    for (const Scalar strength : {Scalar(0.5), Scalar(1), Scalar(1.5), Scalar(2.5)})
    {
        auto mesh = asymmetric_octahedron();
        const auto nv = mesh.n_vertices();
        const auto ne = mesh.n_edges();

        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        esharp[find_edge(mesh, Vertex(0), Vertex(4))] = strength;

        const auto ref =
            reference_loop_step(mesh, BoundaryHandling::Interpolate);
        loop_subdivision(mesh);
        expect_loop_step(mesh, ref, nv, ne);
    }
}

// an all-zero sharpness property without feature flags behaves exactly like
// plain subdivision
TEST(SemiSharpTest, catmull_clark_zero_sharpness_equals_plain)
{
    auto mesh = asymmetric_hexahedron();
    auto plain = asymmetric_hexahedron();
    mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));

    catmull_clark_subdivision(mesh);
    catmull_clark_subdivision(plain);

    ASSERT_EQ(mesh.n_vertices(), plain.n_vertices());
    for (auto v : mesh.vertices())
        expect_point_eq(mesh.position(v), plain.position(v));
}

TEST(SemiSharpTest, loop_zero_sharpness_equals_plain)
{
    auto mesh = asymmetric_octahedron();
    auto plain = asymmetric_octahedron();
    mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));

    loop_subdivision(mesh);
    loop_subdivision(plain);

    ASSERT_EQ(mesh.n_vertices(), plain.n_vertices());
    for (auto v : mesh.vertices())
        expect_point_eq(mesh.position(v), plain.position(v));
}

// three rounds of subdivision: sharpness decreases by one per round, child
// edges inherit it, and the crease turns from hard to soft
TEST(SemiSharpTest, catmull_clark_sharpness_decay)
{
    auto mesh = asymmetric_hexahedron();
    auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
    const auto e0 = find_edge(mesh, Vertex(0), Vertex(1));
    esharp[e0] = Scalar(2.5);

    const auto nv = mesh.n_vertices();
    const Point midpoint =
        Scalar(0.5) * (mesh.position(Vertex(0)) + mesh.position(Vertex(1)));

    // round 1: fully sharp, the two children inherit 1.5
    catmull_clark_subdivision(mesh);
    EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(1.5)), size_t(2));
    EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(0)),
              mesh.n_edges() - size_t(2));

    // the edge point on the fully sharp edge is the exact midpoint
    const auto edge_vertex =
        Vertex(static_cast<IndexType>(nv + e0.idx()));
    expect_point_eq(mesh.position(edge_vertex), midpoint);

    // both children are incident to the vertex inserted on the old edge
    for (auto e : mesh.edges())
        if (esharp[e] == Scalar(1.5))
            EXPECT_TRUE(mesh.vertex(e, 0) == edge_vertex ||
                        mesh.vertex(e, 1) == edge_vertex);

    // round 2: still fully sharp, grandchildren inherit 0.5
    catmull_clark_subdivision(mesh);
    EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(0.5)), size_t(4));

    // round 3: sharpness used up
    catmull_clark_subdivision(mesh);
    EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(0)), mesh.n_edges());

    // sharpness exhausted: behaves exactly like plain subdivision
    auto plain = mesh;
    auto plain_sharp = plain.get_edge_property<Scalar>("e:sharpness");
    ASSERT_TRUE(static_cast<bool>(plain_sharp));
    plain.remove_edge_property(plain_sharp);
    catmull_clark_subdivision(mesh);
    catmull_clark_subdivision(plain);
    ASSERT_EQ(mesh.n_vertices(), plain.n_vertices());
    for (auto v : mesh.vertices())
        expect_point_eq(mesh.position(v), plain.position(v));
}

TEST(SemiSharpTest, loop_sharpness_decay)
{
    auto mesh = asymmetric_octahedron();
    auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
    const auto e0 = find_edge(mesh, Vertex(0), Vertex(4));
    esharp[e0] = Scalar(2.5);

    const auto nv = mesh.n_vertices();
    const Point midpoint =
        Scalar(0.5) * (mesh.position(Vertex(0)) + mesh.position(Vertex(4)));

    // round 1: fully sharp, the two children inherit 1.5
    loop_subdivision(mesh);
    EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(1.5)), size_t(2));

    const auto edge_vertex =
        Vertex(static_cast<IndexType>(nv + e0.idx()));
    expect_point_eq(mesh.position(edge_vertex), midpoint);
    for (auto e : mesh.edges())
        if (esharp[e] == Scalar(1.5))
            EXPECT_TRUE(mesh.vertex(e, 0) == edge_vertex ||
                        mesh.vertex(e, 1) == edge_vertex);

    // round 2: grandchildren inherit 0.5
    loop_subdivision(mesh);
    EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(0.5)), size_t(4));

    // round 3: sharpness used up
    loop_subdivision(mesh);
    EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(0)), mesh.n_edges());

    // sharpness exhausted: behaves exactly like plain subdivision
    auto plain = mesh;
    auto plain_sharp = plain.get_edge_property<Scalar>("e:sharpness");
    ASSERT_TRUE(static_cast<bool>(plain_sharp));
    plain.remove_edge_property(plain_sharp);
    loop_subdivision(mesh);
    loop_subdivision(plain);
    ASSERT_EQ(mesh.n_vertices(), plain.n_vertices());
    for (auto v : mesh.vertices())
        expect_point_eq(mesh.position(v), plain.position(v));
}

// two semi-sharp creases meeting at a vertex produce the crease position,
// three or more the corner position, a single one keeps the smooth rule
TEST(SemiSharpTest, catmull_clark_two_and_three_creases)
{
    // two creases at v0: crease position (6P+N1+N2)/8
    {
        auto mesh = asymmetric_hexahedron();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        esharp[find_edge(mesh, Vertex(0), Vertex(1))] = 2;
        esharp[find_edge(mesh, Vertex(0), Vertex(3))] = 2;

        const Point p0 = mesh.position(Vertex(0));
        const Point p1 = mesh.position(Vertex(1));
        const Point p3 = mesh.position(Vertex(3));

        catmull_clark_subdivision(mesh);

        Point expected = p0;
        expected *= 6;
        expected += p1;
        expected += p3;
        expected *= Scalar(0.125);
        expect_point_near(mesh.position(Vertex(0)), expected, 1e-6);
    }

    // three creases at v0: corner position, vertex stays put
    {
        auto mesh = asymmetric_hexahedron();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        esharp[find_edge(mesh, Vertex(0), Vertex(1))] = 2;
        esharp[find_edge(mesh, Vertex(0), Vertex(3))] = 2;
        esharp[find_edge(mesh, Vertex(0), Vertex(4))] = 2;

        const Point p0 = mesh.position(Vertex(0));

        catmull_clark_subdivision(mesh);

        expect_point_eq(mesh.position(Vertex(0)), p0);
    }

    // single crease at v0: smooth Catmull-Clark rule still applies
    {
        auto mesh = asymmetric_hexahedron();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        esharp[find_edge(mesh, Vertex(0), Vertex(1))] = 2;

        const Point p0 = mesh.position(Vertex(0));
        const Point p1 = mesh.position(Vertex(1));
        const Point p2 = mesh.position(Vertex(2));
        const Point p3 = mesh.position(Vertex(3));
        const Point p4 = mesh.position(Vertex(4));
        const Point p5 = mesh.position(Vertex(5));
        const Point p7 = mesh.position(Vertex(7));
        // face points of the three quads incident to v0
        const Point f0 = (p0 + p3 + p2 + p1) * Scalar(0.25);
        const Point f2 = (p0 + p1 + p5 + p4) * Scalar(0.25);
        const Point f5 = (p3 + p0 + p4 + p7) * Scalar(0.25);

        catmull_clark_subdivision(mesh);

        // (sum of neighbors + sum of face points) / 9 + (1/3) P0
        const Point expected =
            (p1 + p3 + p4 + f0 + f2 + f5) / Scalar(9) + p0 / Scalar(3);
        // hand-evaluated: neighbors sum to (1.2,0.9,1.3), face points to
        // (1.1,1.05,1.225), P0 is the origin
        expect_point_near(expected, Point(2.3 / 9.0, 1.95 / 9.0, 2.525 / 9.0),
                          1e-6);
        expect_point_near(mesh.position(Vertex(0)), expected, 1e-6);
    }
}

TEST(SemiSharpTest, loop_two_and_three_creases)
{
    // two creases at v0: crease position (6P+N1+N2)/8
    {
        auto mesh = asymmetric_octahedron();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        esharp[find_edge(mesh, Vertex(0), Vertex(4))] = 2;
        esharp[find_edge(mesh, Vertex(0), Vertex(1))] = 2;

        const Point p0 = mesh.position(Vertex(0));
        const Point p1 = mesh.position(Vertex(1));
        const Point p4 = mesh.position(Vertex(4));

        loop_subdivision(mesh);

        Point expected = p0;
        expected *= 6;
        expected += p4;
        expected += p1;
        expected *= Scalar(0.125);
        expect_point_near(mesh.position(Vertex(0)), expected, 1e-6);
    }

    // three creases at v0: corner position, vertex stays put
    {
        auto mesh = asymmetric_octahedron();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        esharp[find_edge(mesh, Vertex(0), Vertex(4))] = 2;
        esharp[find_edge(mesh, Vertex(0), Vertex(1))] = 2;
        esharp[find_edge(mesh, Vertex(0), Vertex(3))] = 2;

        const Point p0 = mesh.position(Vertex(0));

        loop_subdivision(mesh);

        expect_point_eq(mesh.position(Vertex(0)), p0);
    }

    // single crease at v0: smooth Loop rule still applies
    {
        auto mesh = asymmetric_octahedron();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        esharp[find_edge(mesh, Vertex(0), Vertex(4))] = 2;

        const Point p0 = mesh.position(Vertex(0));
        const Point p1 = mesh.position(Vertex(1));
        const Point p3 = mesh.position(Vertex(3));
        const Point p4 = mesh.position(Vertex(4));
        const Point p5 = mesh.position(Vertex(5));

        loop_subdivision(mesh);

        // (1-beta) P0 + beta * average of the four neighbors
        const Scalar beta =
            (0.625 -
             pow(0.375 + 0.25 * std::cos(2.0 * std::numbers::pi / 4.0), 2.0));
        const Point avg = (p1 + p3 + p4 + p5) * Scalar(0.25);
        const Point expected = (1 - beta) * p0 + beta * avg;
        // hand-evaluated with beta = 0.484375 and avg = (0,0.1,0.1)
        expect_point_near(expected, Point(0.61875, 0.1, -0.10625), 1e-6);
        expect_point_near(mesh.position(Vertex(0)), expected, 1e-6);
    }
}

// permanent hard edges (e:feature) mix with semi-sharp creases: they count
// as infinitely sharp, keep their boolean markers, and semi-sharp creases
// never set boolean markers themselves
TEST(SemiSharpTest, catmull_clark_mixed_feature_and_sharpness)
{
    auto mesh = asymmetric_hexahedron();
    auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
    auto efeature = mesh.add_edge_property<bool>("e:feature", false);
    auto vfeature = mesh.add_vertex_property<bool>("v:feature", false);

    const auto nv = mesh.n_vertices();
    const auto e_hard = find_edge(mesh, Vertex(0), Vertex(1));
    const auto e_semi = find_edge(mesh, Vertex(0), Vertex(3));
    efeature[e_hard] = true;
    esharp[e_semi] = 2;

    const Point p0 = mesh.position(Vertex(0));
    const Point p1 = mesh.position(Vertex(1));
    const Point p3 = mesh.position(Vertex(3));

    catmull_clark_subdivision(mesh);

    // the permanent hard edge and the semi-sharp crease are the two
    // strongest creases at v0: crease position (6P+N1+N2)/8
    Point expected = p0;
    expected *= 6;
    expected += p1;
    expected += p3;
    expected *= Scalar(0.125);
    expect_point_near(mesh.position(Vertex(0)), expected, 1e-6);

    // the boolean marker propagated to both children of the hard edge
    size_t feature_edges = 0;
    for (auto e : mesh.edges())
        if (efeature[e])
            ++feature_edges;
    EXPECT_EQ(feature_edges, size_t(2));
    const auto hard_vertex = Vertex(static_cast<IndexType>(nv + e_hard.idx()));
    for (auto e : mesh.edges())
        if (efeature[e])
            EXPECT_TRUE(mesh.vertex(e, 0) == hard_vertex ||
                        mesh.vertex(e, 1) == hard_vertex);

    // the semi-sharp crease set no boolean markers: only the vertex
    // inserted on the hard edge is flagged
    size_t feature_vertices = 0;
    for (auto v : mesh.vertices())
        if (vfeature[v])
            ++feature_vertices;
    EXPECT_EQ(feature_vertices, size_t(1));
    EXPECT_TRUE(vfeature[hard_vertex]);

    // the children of the semi-sharp edge inherit the decayed sharpness
    EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(1)), size_t(2));
}

TEST(SemiSharpTest, loop_mixed_feature_and_sharpness)
{
    auto mesh = asymmetric_octahedron();
    auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
    auto efeature = mesh.add_edge_property<bool>("e:feature", false);
    auto vfeature = mesh.add_vertex_property<bool>("v:feature", false);

    const auto nv = mesh.n_vertices();
    const auto e_hard = find_edge(mesh, Vertex(0), Vertex(4));
    const auto e_semi = find_edge(mesh, Vertex(0), Vertex(1));
    efeature[e_hard] = true;
    esharp[e_semi] = 2;

    const Point p0 = mesh.position(Vertex(0));
    const Point p1 = mesh.position(Vertex(1));
    const Point p4 = mesh.position(Vertex(4));

    loop_subdivision(mesh);

    // the permanent hard edge and the semi-sharp crease are the two
    // strongest creases at v0: crease position (6P+N1+N2)/8
    Point expected = p0;
    expected *= 6;
    expected += p4;
    expected += p1;
    expected *= Scalar(0.125);
    expect_point_near(mesh.position(Vertex(0)), expected, 1e-6);

    // the boolean marker propagated to both children of the hard edge
    size_t feature_edges = 0;
    for (auto e : mesh.edges())
        if (efeature[e])
            ++feature_edges;
    EXPECT_EQ(feature_edges, size_t(2));
    const auto hard_vertex = Vertex(static_cast<IndexType>(nv + e_hard.idx()));
    for (auto e : mesh.edges())
        if (efeature[e])
            EXPECT_TRUE(mesh.vertex(e, 0) == hard_vertex ||
                        mesh.vertex(e, 1) == hard_vertex);

    // the semi-sharp crease set no boolean markers
    size_t feature_vertices = 0;
    for (auto v : mesh.vertices())
        if (vfeature[v])
            ++feature_vertices;
    EXPECT_EQ(feature_vertices, size_t(1));
    EXPECT_TRUE(vfeature[hard_vertex]);

    // the children of the semi-sharp edge inherit the decayed sharpness
    EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(1)), size_t(2));
}

// boundary handling stays in charge on the boundary in both modes, and
// sharpness on boundary edges propagates to the child edges
TEST(SemiSharpTest, catmull_clark_boundary_modes)
{
    for (const auto mode :
         {BoundaryHandling::Interpolate, BoundaryHandling::Preserve})
    {
        auto mesh = asymmetric_quad_grid();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        // sharpness on a boundary edge and on an interior edge
        const auto e_bnd = find_edge(mesh, Vertex(0), Vertex(1));
        const auto e_int = find_edge(mesh, Vertex(1), Vertex(4));
        esharp[e_bnd] = 2;
        esharp[e_int] = 3;

        const auto nv = mesh.n_vertices();
        const auto ne = mesh.n_edges();
        const auto nf = mesh.n_faces();
        const auto ref = reference_cc_step(mesh, mode);

        const Point p00 = mesh.position(Vertex(0));
        const Point p10 = mesh.position(Vertex(1));
        const Point p01 = mesh.position(Vertex(3));
        const Point p11 = mesh.position(Vertex(4));

        catmull_clark_subdivision(mesh, mode);
        expect_cc_step(mesh, ref, nv, ne, nf);

        // boundary vertex follows the boundary rule
        if (mode == BoundaryHandling::Preserve)
        {
            expect_point_eq(mesh.position(Vertex(0)), p00);
        }
        else
        {
            Point expected = p00;
            expected *= 6;
            expected += p10;
            expected += p01;
            expected *= Scalar(0.125);
            expect_point_near(mesh.position(Vertex(0)), expected, 1e-6);
        }

        // the boundary edge point is the midpoint, independent of sharpness
        const auto bnd_vertex =
            Vertex(static_cast<IndexType>(nv + e_bnd.idx()));
        expect_point_eq(mesh.position(bnd_vertex),
                        Scalar(0.5) * (p00 + p10));

        // the fully sharp interior edge point is the exact midpoint
        const auto int_vertex =
            Vertex(static_cast<IndexType>(nv + e_int.idx()));
        expect_point_eq(mesh.position(int_vertex),
                        Scalar(0.5) * (p10 + p11));

        // sharpness propagated across the boundary: children of the
        // boundary edge inherit 1, children of the interior edge inherit 2
        EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(1)), size_t(2));
        EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(2)), size_t(2));
    }
}

TEST(SemiSharpTest, loop_boundary_modes)
{
    for (const auto mode :
         {BoundaryHandling::Interpolate, BoundaryHandling::Preserve})
    {
        auto mesh = asymmetric_triangle_fan();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        // sharpness on a boundary edge and on an interior (spoke) edge
        const auto e_bnd = find_edge(mesh, Vertex(1), Vertex(2));
        const auto e_int = find_edge(mesh, Vertex(0), Vertex(1));
        esharp[e_bnd] = 2;
        esharp[e_int] = 3;

        const auto nv = mesh.n_vertices();
        const auto ne = mesh.n_edges();
        const auto ref = reference_loop_step(mesh, mode);

        const Point pr0 = mesh.position(Vertex(1));
        const Point pr1 = mesh.position(Vertex(2));
        const Point pr4 = mesh.position(Vertex(5));
        const Point pc = mesh.position(Vertex(0));

        loop_subdivision(mesh, mode);
        expect_loop_step(mesh, ref, nv, ne);

        // boundary vertex follows the boundary rule
        if (mode == BoundaryHandling::Preserve)
        {
            expect_point_eq(mesh.position(Vertex(1)), pr0);
        }
        else
        {
            Point expected = pr0;
            expected *= 6;
            expected += pr1;
            expected += pr4;
            expected *= Scalar(0.125);
            expect_point_near(mesh.position(Vertex(1)), expected, 1e-6);
        }

        // the boundary edge point is the midpoint, independent of sharpness
        const auto bnd_vertex =
            Vertex(static_cast<IndexType>(nv + e_bnd.idx()));
        expect_point_eq(mesh.position(bnd_vertex),
                        Scalar(0.5) * (pr0 + pr1));

        // the fully sharp interior edge point is the exact midpoint
        const auto int_vertex =
            Vertex(static_cast<IndexType>(nv + e_int.idx()));
        expect_point_eq(mesh.position(int_vertex), Scalar(0.5) * (pc + pr0));

        // sharpness propagated across the boundary
        EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(1)), size_t(2));
        EXPECT_EQ(count_sharpness(mesh, esharp, Scalar(2)), size_t(2));
    }
}

// isolated vertices stay put, also with semi-sharp creases around
TEST(SemiSharpTest, catmull_clark_isolated_vertex_unchanged)
{
    auto mesh = asymmetric_hexahedron();
    const auto isolated = mesh.add_vertex(Point(5.0, 6.0, 7.0));
    mesh.add_edge_property<Scalar>("e:sharpness", Scalar(1));

    catmull_clark_subdivision(mesh);

    expect_point_eq(mesh.position(isolated), Point(5.0, 6.0, 7.0));
}

TEST(SemiSharpTest, loop_isolated_vertex_unchanged)
{
    auto mesh = asymmetric_octahedron();
    const auto isolated = mesh.add_vertex(Point(5.0, 6.0, 7.0));
    mesh.add_edge_property<Scalar>("e:sharpness", Scalar(1));

    loop_subdivision(mesh);

    expect_point_eq(mesh.position(isolated), Point(5.0, 6.0, 7.0));
}

// negative, NaN, or infinite sharpness is rejected before anything changes
TEST(SemiSharpTest, catmull_clark_invalid_sharpness_rejected)
{
    const Scalar nan = std::numeric_limits<Scalar>::quiet_NaN();
    const Scalar inf = std::numeric_limits<Scalar>::infinity();
    for (const Scalar bad : {Scalar(-1), Scalar(-0.5), nan, inf})
    {
        auto mesh = asymmetric_hexahedron();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        const auto e_bad = find_edge(mesh, Vertex(0), Vertex(1));
        esharp[e_bad] = bad;

        const auto nv = mesh.n_vertices();
        const auto ne = mesh.n_edges();
        const auto nf = mesh.n_faces();
        std::vector<Point> before;
        for (auto v : mesh.vertices())
            before.push_back(mesh.position(v));

        EXPECT_THROW(catmull_clark_subdivision(mesh), InvalidInputException);

        // the mesh is completely unchanged
        EXPECT_EQ(mesh.n_vertices(), nv);
        EXPECT_EQ(mesh.n_edges(), ne);
        EXPECT_EQ(mesh.n_faces(), nf);
        size_t i = 0;
        for (auto v : mesh.vertices())
            expect_point_eq(mesh.position(v), before[i++]);
        for (auto e : mesh.edges())
        {
            if (e == e_bad)
                EXPECT_TRUE(std::isnan(esharp[e]) || esharp[e] == bad);
            else
                EXPECT_EQ(esharp[e], Scalar(0));
        }

        // no temporary properties left behind
        EXPECT_TRUE(!mesh.get_vertex_property<Point>("catmull:vpoint"));
        EXPECT_TRUE(!mesh.get_edge_property<Point>("catmull:epoint"));
        EXPECT_TRUE(!mesh.get_face_property<Point>("catmull:fpoint"));
        EXPECT_TRUE(!mesh.get_edge_property<Scalar>("catmull:esharp_next"));
    }
}

TEST(SemiSharpTest, loop_invalid_sharpness_rejected)
{
    const Scalar nan = std::numeric_limits<Scalar>::quiet_NaN();
    const Scalar inf = std::numeric_limits<Scalar>::infinity();
    for (const Scalar bad : {Scalar(-1), Scalar(-0.5), nan, inf})
    {
        auto mesh = asymmetric_octahedron();
        auto esharp = mesh.add_edge_property<Scalar>("e:sharpness", Scalar(0));
        const auto e_bad = find_edge(mesh, Vertex(0), Vertex(4));
        esharp[e_bad] = bad;

        const auto nv = mesh.n_vertices();
        const auto ne = mesh.n_edges();
        const auto nf = mesh.n_faces();
        std::vector<Point> before;
        for (auto v : mesh.vertices())
            before.push_back(mesh.position(v));

        EXPECT_THROW(loop_subdivision(mesh), InvalidInputException);

        // the mesh is completely unchanged
        EXPECT_EQ(mesh.n_vertices(), nv);
        EXPECT_EQ(mesh.n_edges(), ne);
        EXPECT_EQ(mesh.n_faces(), nf);
        size_t i = 0;
        for (auto v : mesh.vertices())
            expect_point_eq(mesh.position(v), before[i++]);
        for (auto e : mesh.edges())
        {
            if (e == e_bad)
                EXPECT_TRUE(std::isnan(esharp[e]) || esharp[e] == bad);
            else
                EXPECT_EQ(esharp[e], Scalar(0));
        }

        // no temporary properties left behind
        EXPECT_TRUE(!mesh.get_vertex_property<Point>("loop:vpoint"));
        EXPECT_TRUE(!mesh.get_edge_property<Point>("loop:epoint"));
        EXPECT_TRUE(!mesh.get_edge_property<Scalar>("loop:esharp_next"));
    }
}

// Loop keeps rejecting non-triangle meshes, also with sharpness present
TEST(SemiSharpTest, loop_requires_triangle_mesh)
{
    auto mesh = asymmetric_hexahedron();
    EXPECT_THROW(loop_subdivision(mesh), InvalidInputException);

    auto mesh2 = asymmetric_hexahedron();
    mesh2.add_edge_property<Scalar>("e:sharpness", Scalar(0));
    EXPECT_THROW(loop_subdivision(mesh2), InvalidInputException);
    EXPECT_EQ(mesh2.n_vertices(), size_t(8));
    EXPECT_EQ(mesh2.n_edges(), size_t(12));
    EXPECT_EQ(mesh2.n_faces(), size_t(6));
}
