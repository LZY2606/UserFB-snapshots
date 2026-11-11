// Copyright 2011-2022 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/subdivision.h"
#include "pmp/algorithms/differential_geometry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace pmp {

namespace {

//! Clamp a crease strength to the [0,1] blending range.
Scalar clamped_sharpness(Scalar s)
{
    return std::clamp(s, Scalar(0), Scalar(1));
}

//! Check that all semi-sharp crease strengths are finite and non-negative.
//! \throw InvalidInputException on negative, NaN, or infinite values.
void check_sharpness(const SurfaceMesh& mesh,
                     const EdgeProperty<Scalar>& esharp, const char* caller)
{
    for (auto e : mesh.edges())
    {
        if (!std::isfinite(esharp[e]) || esharp[e] < 0)
        {
            auto what = std::string{caller} +
                        ": e:sharpness values must be finite and "
                        "non-negative.";
            throw InvalidInputException(what);
        }
    }
}

//! Compute the sharpness inherited by the two child edges of each current
//! edge: strengths decrease by one per subdivision step.
EdgeProperty<Scalar> inherited_sharpness(SurfaceMesh& mesh, const char* name,
                                         const EdgeProperty<Scalar>& esharp)
{
    auto next = mesh.add_edge_property<Scalar>(name);
    for (auto e : mesh.edges())
        next[e] = std::max(esharp[e] - Scalar(1), Scalar(0));
    return next;
}

//! Blend between the smooth and the sharp (midpoint) position of an edge
//! point according to the edge sharpness.
Point semi_sharp_edge_position(const Point& smooth, const Point& sharp,
                               Scalar strength)
{
    const Scalar t = clamped_sharpness(strength);
    return (Scalar(1) - t) * smooth + t * sharp;
}

//! Compute the position of an interior vertex under the semi-sharp crease
//! rule, given its smooth position \p smooth. Blends between the smooth
//! position, the crease position (6P+N1+N2)/8 through the two strongest
//! incident creases, and the corner position P for three or more creases.
Point semi_sharp_vertex_position(const SurfaceMesh& mesh,
                                 const VertexProperty<Point>& points, Vertex v,
                                 const EdgeProperty<bool>& efeature,
                                 const EdgeProperty<Scalar>& esharp,
                                 const Point& smooth)
{
    // gather incident crease strengths and neighbor positions
    struct Crease
    {
        Scalar strength;
        IndexType neighbor;
        Point point;
    };
    std::vector<Crease> creases;
    creases.reserve(mesh.valence(v));
    for (auto h : mesh.halfedges(v))
    {
        const auto e = mesh.edge(h);
        // permanent hard edges count as infinitely sharp
        const Scalar strength = (efeature && efeature[e])
                                    ? std::numeric_limits<Scalar>::infinity()
                                    : esharp[e];
        const auto neighbor = mesh.to_vertex(h);
        creases.push_back({strength, neighbor.idx(), points[neighbor]});
    }

    // strongest first; break ties by neighbor index for determinism
    std::sort(creases.begin(), creases.end(),
              [](const Crease& a, const Crease& b) {
                  return a.strength != b.strength
                             ? a.strength > b.strength
                             : a.neighbor < b.neighbor;
              });

    const Scalar s2 = creases.size() > 1 ? creases[1].strength : Scalar(0);
    const Scalar s3 = creases.size() > 2 ? creases[2].strength : Scalar(0);
    const Scalar u2 = clamped_sharpness(s2);
    const Scalar u3 = clamped_sharpness(s3);

    // less than two creases: stick to the smooth rule
    if (u2 == Scalar(0))
        return smooth;

    // crease position through the two strongest creases
    Point crease = points[v];
    crease *= 6.0;
    crease += creases[0].point;
    crease += creases[1].point;
    crease *= 0.125;

    return (Scalar(1) - u2) * smooth + (u2 - u3) * crease +
           u3 * points[v];
}

} // namespace

void catmull_clark_subdivision(SurfaceMesh& mesh,
                               BoundaryHandling boundary_handling)
{
    // validate optional semi-sharp creases before changing anything
    auto esharp = mesh.get_edge_property<Scalar>("e:sharpness");
    if (esharp)
        check_sharpness(mesh, esharp, __func__);

    auto points = mesh.vertex_property<Point>("v:point");
    auto vfeature = mesh.get_vertex_property<bool>("v:feature");
    auto efeature = mesh.get_edge_property<bool>("e:feature");

    // reserve memory
    const size_t nv = mesh.n_vertices();
    const size_t ne = mesh.n_edges();
    const size_t nf = mesh.n_faces();
    mesh.reserve(nv + ne + nf, 2 * ne + 4 * nf, 4 * nf);

    // get properties
    auto vpoint = mesh.add_vertex_property<Point>("catmull:vpoint");
    auto epoint = mesh.add_edge_property<Point>("catmull:epoint");
    auto fpoint = mesh.add_face_property<Point>("catmull:fpoint");

    // sharpness inherited by the two children of each split edge
    EdgeProperty<Scalar> esharp_next;
    if (esharp)
        esharp_next = inherited_sharpness(mesh, "catmull:esharp_next", esharp);

    // compute face vertices
    for (auto f : mesh.faces())
    {
        fpoint[f] = centroid(mesh, f);
    }

    // compute edge vertices
    for (auto e : mesh.edges())
    {
        // boundary or feature edge?
        if (mesh.is_boundary(e) || (efeature && efeature[e]))
        {
            epoint[e] =
                0.5f * (points[mesh.vertex(e, 0)] + points[mesh.vertex(e, 1)]);
        }

        // interior edge
        else
        {
            Point p(0, 0, 0);
            p += points[mesh.vertex(e, 0)];
            p += points[mesh.vertex(e, 1)];
            p += fpoint[mesh.face(e, 0)];
            p += fpoint[mesh.face(e, 1)];
            p *= 0.25f;

            // blend towards the midpoint for semi-sharp creases
            if (esharp)
            {
                const Point sharp =
                    0.5f *
                    (points[mesh.vertex(e, 0)] + points[mesh.vertex(e, 1)]);
                p = semi_sharp_edge_position(p, sharp, esharp[e]);
            }

            epoint[e] = p;
        }
    }

    // compute new positions for old vertices
    for (auto v : mesh.vertices())
    {
        // isolated vertex?
        if (mesh.is_isolated(v))
        {
            vpoint[v] = points[v];
        }

        // boundary vertex?
        else if (mesh.is_boundary(v))
        {
            if (boundary_handling == BoundaryHandling::Preserve)
            {
                vpoint[v] = points[v];
            }
            else
            {
                auto h1 = mesh.halfedge(v);
                auto h0 = mesh.prev_halfedge(h1);

                Point p = points[v];
                p *= 6.0;
                p += points[mesh.to_vertex(h1)];
                p += points[mesh.from_vertex(h0)];
                p *= 0.125;

                vpoint[v] = p;
            }
        }

        // interior feature vertex?
        else if (vfeature && vfeature[v])
        {
            Point p = points[v];
            p *= 6.0;
            int count(0);

            for (auto h : mesh.halfedges(v))
            {
                if (efeature[mesh.edge(h)])
                {
                    p += points[mesh.to_vertex(h)];
                    ++count;
                }
            }

            if (count == 2) // vertex is on feature edge
            {
                p *= 0.125;
                vpoint[v] = p;
            }
            else // keep fixed
            {
                vpoint[v] = points[v];
            }
        }

        // interior vertex
        else
        {
            // weights from SIGGRAPH paper "Subdivision Surfaces in Character Animation"

            const Scalar k = mesh.valence(v);
            Point p(0, 0, 0);

            for (auto vv : mesh.vertices(v))
                p += points[vv];

            for (auto f : mesh.faces(v))
                p += fpoint[f];

            p /= (k * k);

            p += ((k - 2.0f) / k) * points[v];

            // blend towards crease/corner positions for semi-sharp creases
            if (esharp)
                p = semi_sharp_vertex_position(mesh, points, v, efeature,
                                               esharp, p);

            vpoint[v] = p;
        }
    }

    // assign new positions to old vertices
    for (auto v : mesh.vertices())
    {
        points[v] = vpoint[v];
    }

    // split edges
    for (auto e : mesh.edges())
    {
        const bool is_feature = efeature && efeature[e];

        // plain edge: nothing to propagate
        if (!is_feature && !esharp)
        {
            mesh.insert_vertex(e, epoint[e]);
            continue;
        }

        auto h = mesh.insert_vertex(e, epoint[e]);
        auto e0 = mesh.edge(h);
        auto e1 = mesh.edge(mesh.next_halfedge(h));

        // propagate feature marker
        if (is_feature)
        {
            auto v = mesh.to_vertex(h);
            vfeature[v] = true;
            efeature[e0] = true;
            efeature[e1] = true;
        }

        // both children inherit the decayed sharpness of the old edge
        if (esharp)
        {
            esharp[e0] = esharp_next[e];
            esharp[e1] = esharp_next[e];
        }
    }

    // split faces
    for (auto f : mesh.faces())
    {
        auto h0 = mesh.halfedge(f);
        auto h_in = mesh.insert_edge(h0, mesh.next_halfedge(mesh.next_halfedge(h0)));

        // edges introduced in the interior of faces are smooth
        if (esharp)
            esharp[mesh.edge(h_in)] = Scalar(0);

        auto h1 = mesh.next_halfedge(h0);
        auto h_in_v = mesh.insert_vertex(mesh.edge(h1), fpoint[f]);
        if (esharp)
        {
            esharp[mesh.edge(h_in_v)] = Scalar(0);
            esharp[mesh.edge(mesh.next_halfedge(h_in_v))] = Scalar(0);
        }

        auto h = mesh.next_halfedge(mesh.next_halfedge(mesh.next_halfedge(h1)));
        while (h != h0)
        {
            auto h_spoke = mesh.insert_edge(h1, h);
            if (esharp)
                esharp[mesh.edge(h_spoke)] = Scalar(0);
            h = mesh.next_halfedge(mesh.next_halfedge(mesh.next_halfedge(h1)));
        }
    }

    // clean-up properties
    mesh.remove_vertex_property(vpoint);
    mesh.remove_edge_property(epoint);
    mesh.remove_face_property(fpoint);
    if (esharp)
        mesh.remove_edge_property(esharp_next);
}

void loop_subdivision(SurfaceMesh& mesh, BoundaryHandling boundary_handling)
{
    // validate optional semi-sharp creases before changing anything
    auto esharp = mesh.get_edge_property<Scalar>("e:sharpness");
    if (esharp)
        check_sharpness(mesh, esharp, __func__);

    if (!mesh.is_triangle_mesh())
    {
        auto what = std::string{__func__} + ": Not a triangle mesh.";
        throw InvalidInputException(what);
    }

    auto points = mesh.vertex_property<Point>("v:point");
    auto vfeature = mesh.get_vertex_property<bool>("v:feature");
    auto efeature = mesh.get_edge_property<bool>("e:feature");

    // reserve memory
    const size_t nv = mesh.n_vertices();
    const size_t ne = mesh.n_edges();
    const size_t nf = mesh.n_faces();
    mesh.reserve(nv + ne, 2 * ne + 3 * nf, 4 * nf);

    // add properties
    auto vpoint = mesh.add_vertex_property<Point>("loop:vpoint");
    auto epoint = mesh.add_edge_property<Point>("loop:epoint");

    // sharpness inherited by the two children of each split edge
    EdgeProperty<Scalar> esharp_next;
    if (esharp)
        esharp_next = inherited_sharpness(mesh, "loop:esharp_next", esharp);

    // compute vertex positions
    for (auto v : mesh.vertices())
    {
        // isolated vertex?
        if (mesh.is_isolated(v))
        {
            vpoint[v] = points[v];
        }

        // boundary vertex?
        else if (mesh.is_boundary(v))
        {
            if (boundary_handling == BoundaryHandling::Preserve)
            {
                vpoint[v] = points[v];
            }
            else
            {
                auto h1 = mesh.halfedge(v);
                auto h0 = mesh.prev_halfedge(h1);

                Point p = points[v];
                p *= 6.0;
                p += points[mesh.to_vertex(h1)];
                p += points[mesh.from_vertex(h0)];
                p *= 0.125;
                vpoint[v] = p;
            }
        }

        // interior feature vertex?
        else if (vfeature && vfeature[v])
        {
            Point p = points[v];
            p *= 6.0;
            int count(0);

            for (auto h : mesh.halfedges(v))
            {
                if (efeature[mesh.edge(h)])
                {
                    p += points[mesh.to_vertex(h)];
                    ++count;
                }
            }

            if (count == 2) // vertex is on feature edge
            {
                p *= 0.125;
                vpoint[v] = p;
            }
            else // keep fixed
            {
                vpoint[v] = points[v];
            }
        }

        // interior vertex
        else
        {
            Point p(0, 0, 0);
            Scalar k(0);

            for (auto vv : mesh.vertices(v))
            {
                p += points[vv];
                ++k;
            }
            p /= k;

            const Scalar beta =
                (0.625 -
                 pow(0.375 + 0.25 * std::cos(2.0 * std::numbers::pi / k), 2.0));

            p = points[v] * (Scalar)(1.0 - beta) + beta * p;

            // blend towards crease/corner positions for semi-sharp creases
            if (esharp)
                p = semi_sharp_vertex_position(mesh, points, v, efeature,
                                               esharp, p);

            vpoint[v] = p;
        }
    }

    // compute edge positions
    for (auto e : mesh.edges())
    {
        // boundary or feature edge?
        if (mesh.is_boundary(e) || (efeature && efeature[e]))
        {
            epoint[e] =
                (points[mesh.vertex(e, 0)] + points[mesh.vertex(e, 1)]) *
                Scalar(0.5);
        }

        // interior edge
        else
        {
            auto h0 = mesh.halfedge(e, 0);
            auto h1 = mesh.halfedge(e, 1);
            Point p = points[mesh.to_vertex(h0)];
            p += points[mesh.to_vertex(h1)];
            p *= 3.0;
            p += points[mesh.to_vertex(mesh.next_halfedge(h0))];
            p += points[mesh.to_vertex(mesh.next_halfedge(h1))];
            p *= 0.125;

            // blend towards the midpoint for semi-sharp creases
            if (esharp)
            {
                const Point sharp =
                    (points[mesh.vertex(e, 0)] + points[mesh.vertex(e, 1)]) *
                    Scalar(0.5);
                p = semi_sharp_edge_position(p, sharp, esharp[e]);
            }

            epoint[e] = p;
        }
    }

    // set new vertex positions
    for (auto v : mesh.vertices())
    {
        points[v] = vpoint[v];
    }

    // insert new vertices on edges
    for (auto e : mesh.edges())
    {
        const bool is_feature = efeature && efeature[e];

        // plain edge: nothing to propagate
        if (!is_feature && !esharp)
        {
            mesh.insert_vertex(e, epoint[e]);
            continue;
        }

        auto h = mesh.insert_vertex(e, epoint[e]);
        auto e0 = mesh.edge(h);
        auto e1 = mesh.edge(mesh.next_halfedge(h));

        // propagate feature marker
        if (is_feature)
        {
            auto v = mesh.to_vertex(h);
            vfeature[v] = true;
            efeature[e0] = true;
            efeature[e1] = true;
        }

        // both children inherit the decayed sharpness of the old edge
        if (esharp)
        {
            esharp[e0] = esharp_next[e];
            esharp[e1] = esharp_next[e];
        }
    }

    // split faces
    Halfedge h;
    for (auto f : mesh.faces())
    {
        h = mesh.halfedge(f);
        auto h_in0 = mesh.insert_edge(h, mesh.next_halfedge(mesh.next_halfedge(h)));
        h = mesh.next_halfedge(h);
        auto h_in1 = mesh.insert_edge(h, mesh.next_halfedge(mesh.next_halfedge(h)));
        h = mesh.next_halfedge(h);
        auto h_in2 = mesh.insert_edge(h, mesh.next_halfedge(mesh.next_halfedge(h)));

        // edges introduced in the interior of faces are smooth
        if (esharp)
        {
            esharp[mesh.edge(h_in0)] = Scalar(0);
            esharp[mesh.edge(h_in1)] = Scalar(0);
            esharp[mesh.edge(h_in2)] = Scalar(0);
        }
    }

    // clean-up properties
    mesh.remove_vertex_property(vpoint);
    mesh.remove_edge_property(epoint);
    if (esharp)
        mesh.remove_edge_property(esharp_next);
}

void quad_tri_subdivision(SurfaceMesh& mesh, BoundaryHandling boundary_handling)
{
    auto points = mesh.vertex_property<Point>("v:point");

    // split each edge evenly into two parts
    for (auto e : mesh.edges())
    {
        mesh.insert_vertex(
            e, 0.5f * (points[mesh.vertex(e, 0)] + points[mesh.vertex(e, 1)]));
    }

    // subdivide faces without repositioning
    for (auto f : mesh.faces())
    {
        const size_t f_val = mesh.valence(f) / 2;
        if (f_val == 3)
        {
            // face was a triangle
            Halfedge h0 = mesh.halfedge(f);
            Halfedge h1 = mesh.next_halfedge(mesh.next_halfedge(h0));
            mesh.insert_edge(h0, h1);

            h0 = mesh.next_halfedge(h0);
            h1 = mesh.next_halfedge(mesh.next_halfedge(h0));
            mesh.insert_edge(h0, h1);

            h0 = mesh.next_halfedge(h0);
            h1 = mesh.next_halfedge(mesh.next_halfedge(h0));
            mesh.insert_edge(h0, h1);
        }
        else
        {
            // quadrangulate the rest
            const Halfedge h0 = mesh.halfedge(f);
            Halfedge h1 = mesh.next_halfedge(mesh.next_halfedge(h0));
            //NOTE: It's important to calculate the centroid before inserting the new edge
            auto cen = centroid(mesh, f);
            h1 = mesh.insert_edge(h0, h1);
            mesh.insert_vertex(mesh.edge(h1), cen);

            auto h =
                mesh.next_halfedge(mesh.next_halfedge(mesh.next_halfedge(h1)));
            while (h != h0)
            {
                mesh.insert_edge(h1, h);
                h = mesh.next_halfedge(
                    mesh.next_halfedge(mesh.next_halfedge(h1)));
            }
        }
    }

    auto new_pos =
        mesh.add_vertex_property<Point>("quad_tri:new_position", Point(0));

    for (auto v : mesh.vertices())
    {
        if (mesh.is_boundary(v))
        {
            if (boundary_handling == BoundaryHandling::Preserve)
            {
                new_pos[v] = points[v];
            }
            else
            {
                new_pos[v] = 0.5 * points[v];

                // add neighboring vertices on boundary
                for (auto vv : mesh.vertices(v))
                {
                    if (mesh.is_boundary(vv))
                    {
                        new_pos[v] += 0.25 * points[vv];
                    }
                }
            }
        }
        else
        {
            // count the number of faces and quads surrounding the vertex
            int n_faces = 0;
            int n_quads = 0;
            for (auto f : mesh.faces(v))
            {
                n_faces++;
                if (mesh.valence(f) == 4)
                    n_quads++;
            }

            if (n_quads == 0)
            {
                // vertex is surrounded only by triangles
                const double a =
                    2.0 *
                    pow(3.0 / 8.0 +
                            (std::cos(2.0 * std::numbers::pi / n_faces) - 1.0) /
                                4.0,
                        2.0);
                const double b = (1.0 - a) / n_faces;

                new_pos[v] = a * points[v];
                for (auto vv : mesh.vertices(v))
                {
                    new_pos[v] += b * points[vv];
                }
            }
            else if (n_quads == n_faces)
            {
                // vertex is surrounded only by quads
                const double c = (n_faces - 3.0) / n_faces;
                const double d = 2.0 / pow(n_faces, 2.0);
                const double e = 1.0 / pow(n_faces, 2.0);

                new_pos[v] = c * points[v];
                for (auto h : mesh.halfedges(v))
                {
                    new_pos[v] += d * points[mesh.to_vertex(h)];
                    new_pos[v] +=
                        e * points[mesh.to_vertex(mesh.next_halfedge(h))];
                }
            }
            else
            {
                // vertex is surrounded by triangles and quads
                const double alpha =
                    1.0 / (1.0 + 0.5 * n_faces + 0.25 * n_quads);
                const double beta = 0.5 * alpha;
                const double gamma = 0.25 * alpha;

                new_pos[v] = alpha * points[v];
                for (auto h : mesh.halfedges(v))
                {
                    new_pos[v] += beta * points[mesh.to_vertex(h)];
                    if (mesh.valence(mesh.face(h)) == 4)
                    {
                        new_pos[v] +=
                            gamma *
                            points[mesh.to_vertex(mesh.next_halfedge(h))];
                    }
                }
            }
        }
    }

    // apply new positions to the mesh
    for (auto v : mesh.vertices())
    {
        points[v] = new_pos[v];
    }

    mesh.remove_vertex_property(new_pos);
}

void linear_subdivision(SurfaceMesh& mesh)
{
    auto points = mesh.vertex_property<Point>("v:point");

    // linear subdivision of edges
    for (auto e : mesh.edges())
    {
        mesh.insert_vertex(
            e, 0.5f * (points[mesh.vertex(e, 0)] + points[mesh.vertex(e, 1)]));
    }

    // subdivide faces
    for (auto f : mesh.faces())
    {
        const size_t f_val = mesh.valence(f) / 2;

        if (f_val == 3) // triangle
        {
            Halfedge h0 = mesh.halfedge(f);
            Halfedge h1 = mesh.next_halfedge(mesh.next_halfedge(h0));
            mesh.insert_edge(h0, h1);

            h0 = mesh.next_halfedge(h0);
            h1 = mesh.next_halfedge(mesh.next_halfedge(h0));
            mesh.insert_edge(h0, h1);

            h0 = mesh.next_halfedge(h0);
            h1 = mesh.next_halfedge(mesh.next_halfedge(h0));
            mesh.insert_edge(h0, h1);
        }
        else // quadrangulate other faces
        {
            const Halfedge h0 = mesh.halfedge(f);
            Halfedge h1 = mesh.next_halfedge(mesh.next_halfedge(h0));

            // NOTE: It's important to calculate the centroid before inserting the new edge
            auto cen = centroid(mesh, f);
            h1 = mesh.insert_edge(h0, h1);
            mesh.insert_vertex(mesh.edge(h1), cen);

            auto h =
                mesh.next_halfedge(mesh.next_halfedge(mesh.next_halfedge(h1)));
            while (h != h0)
            {
                mesh.insert_edge(h1, h);
                h = mesh.next_halfedge(
                    mesh.next_halfedge(mesh.next_halfedge(h1)));
            }
        }
    }
}

} // namespace pmp
