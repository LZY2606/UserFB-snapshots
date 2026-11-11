// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

// Tests for texture-preserving uniform and adaptive remeshing.
//
// All reference values are computed independently from the original input
// mesh: texture charts are reconstructed with a standalone flood fill, and
// reference UVs are evaluated by brute-force nearest-triangle queries within
// the corresponding chart (smallest original face index wins ties).

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "pmp/algorithms/barycentric_coordinates.h"
#include "pmp/algorithms/distance_point_triangle.h"
#include "pmp/algorithms/remeshing.h"
#include "pmp/io/io.h"
#include "pmp/surface_mesh.h"

using namespace pmp;

namespace {

constexpr Scalar kEps = Scalar(1e-4);

// non-affine texture mappings, deliberately outside [0, 1]
TexCoord uv_side_a(const Point& p)
{
    return TexCoord(Scalar(7.3) * p[0] + Scalar(0.5) * p[1] * p[1] + Scalar(3.1),
                    Scalar(-3.1) * p[1] + Scalar(0.4) * p[0] * p[0] -
                        Scalar(2.7));
}

TexCoord uv_side_b(const Point& p)
{
    return TexCoord(Scalar(-5.9) * p[0] + Scalar(0.7) * p[1] * p[1] +
                        Scalar(17.3),
                    Scalar(4.7) * p[1] - Scalar(0.6) * p[0] * p[0] +
                        Scalar(11.9));
}

// asymmetric, mildly curved triangle grid
struct Grid
{
    SurfaceMesh mesh;
    int nx{0};
    int ny{0};
    std::vector<Vertex> v; // nx * ny row-major vertices
    Scalar seam_x{0};

    Vertex at(int i, int j) const { return v[j * nx + i]; }
};

Grid make_grid(int nx, int ny, bool with_seam)
{
    Grid g;
    g.nx = nx;
    g.ny = ny;
    g.mesh.reserve(nx * ny, 3 * nx * ny, 2 * (nx - 1) * (ny - 1));
    g.v.resize(nx * ny);

    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
        {
            const Scalar x = Scalar(0.37) * i + Scalar(0.05) * (i % 2);
            const Scalar y = Scalar(0.41) * j - Scalar(0.03) * (j % 3);
            const Scalar z =
                Scalar(0.03) * std::sin(Scalar(0.7) * x) * std::cos(Scalar(0.5) * y);
            g.v[j * nx + i] = g.mesh.add_vertex(Point(x, y, z));
        }

    for (int j = 0; j + 1 < ny; ++j)
        for (int i = 0; i + 1 < nx; ++i)
        {
            // alternating diagonal direction, asymmetric triangulation
            if ((i + j) % 2 == 0)
            {
                g.mesh.add_triangle(g.at(i, j), g.at(i + 1, j),
                                    g.at(i + 1, j + 1));
                g.mesh.add_triangle(g.at(i, j), g.at(i + 1, j + 1),
                                    g.at(i, j + 1));
            }
            else
            {
                g.mesh.add_triangle(g.at(i, j), g.at(i + 1, j),
                                    g.at(i, j + 1));
                g.mesh.add_triangle(g.at(i + 1, j), g.at(i + 1, j + 1),
                                    g.at(i, j + 1));
            }
        }

    auto tex = g.mesh.halfedge_property<TexCoord>("h:tex");

    if (with_seam)
    {
        const int k = nx / 2;
        g.seam_x = Scalar(0.37) * k + Scalar(0.05) * (k % 2) - Scalar(0.18);
        for (auto f : g.mesh.faces())
        {
            Scalar cx = 0;
            for (auto vv : g.mesh.vertices(f))
                cx += g.mesh.position(vv)[0];
            cx /= 3.0f;
            const bool side_a = cx < g.seam_x;
            for (auto h : g.mesh.halfedges(f))
            {
                const auto& p = g.mesh.position(g.mesh.to_vertex(h));
                tex[h] = side_a ? uv_side_a(p) : uv_side_b(p);
            }
        }
    }
    else
    {
        for (auto f : g.mesh.faces())
            for (auto h : g.mesh.halfedges(f))
                tex[h] = uv_side_a(g.mesh.position(g.mesh.to_vertex(h)));
    }

    return g;
}

// true if an interior edge is a texture seam: the two faces disagree on
// the UV at either endpoint (each halfedge stores the UV of the vertex it
// points to)
bool is_seam_edge(const SurfaceMesh& mesh, Edge e)
{
    if (mesh.is_boundary(e))
        return false;
    auto tex = mesh.get_halfedge_property<TexCoord>("h:tex");
    const Halfedge h0 = mesh.halfedge(e, 0);
    const Halfedge h1 = mesh.halfedge(e, 1);
    auto differs = [](const TexCoord& a, const TexCoord& b) {
        return std::abs(a[0] - b[0]) > Scalar(1e-6) ||
               std::abs(a[1] - b[1]) > Scalar(1e-6);
    };
    return differs(tex[h0], tex[mesh.prev_halfedge(h1)]) ||
           differs(tex[h1], tex[mesh.prev_halfedge(h0)]);
}

// independent texture-chart flood fill on an arbitrary mesh
std::vector<int> compute_charts(const SurfaceMesh& mesh, int& n_charts)
{
    auto tex = mesh.get_halfedge_property<TexCoord>("h:tex");
    std::vector<int> chart(mesh.faces_size(), -1);
    n_charts = 0;
    std::vector<Face> stack;

    for (auto seed : mesh.faces())
    {
        if (chart[seed.idx()] != -1)
            continue;
        const int c = n_charts++;
        chart[seed.idx()] = c;
        stack.clear();
        stack.push_back(seed);
        while (!stack.empty())
        {
            const Face f = stack.back();
            stack.pop_back();
            for (auto h : mesh.halfedges(f))
            {
                const Halfedge oh = mesh.opposite_halfedge(h);
                const Face nf = mesh.face(oh);
                if (!nf.is_valid() || chart[nf.idx()] != -1)
                    continue;
                if (is_seam_edge(mesh, mesh.edge(h)))
                    continue;
                chart[nf.idx()] = c;
                stack.push_back(nf);
            }
        }
    }
    return chart;
}

struct RefTriangle
{
    Face face;
    int chart;
    Point p[3];
    TexCoord uv[3];
};

// brute-force reference built from the original, untouched mesh
class ReferenceSurface
{
public:
    explicit ReferenceSurface(const SurfaceMesh& mesh)
    {
        int n = 0;
        charts_ = compute_charts(mesh, n);
        n_charts_ = n;
        auto tex = mesh.get_halfedge_property<TexCoord>("h:tex");
        for (auto f : mesh.faces())
        {
            RefTriangle rt;
            rt.face = f;
            rt.chart = charts_[f.idx()];
            int k = 0;
            for (auto h : mesh.halfedges(f))
            {
                rt.p[k] = mesh.position(mesh.to_vertex(h));
                rt.uv[k] = tex[h];
                ++k;
            }
            tris_.push_back(rt);
        }
    }

    int n_charts() const { return n_charts_; }
    int chart_of(Face f) const { return charts_[f.idx()]; }

    // closest point on the closest original triangle within one chart.
    // exact distance ties resolve to the smallest original face index.
    TexCoord evaluate(int chart, const Point& q, Point& nearest) const
    {
        Scalar best = std::numeric_limits<Scalar>::max();
        const RefTriangle* best_rt = nullptr;
        Point best_n;
        for (const auto& rt : tris_)
        {
            if (rt.chart != chart)
                continue;
            Point n;
            const Scalar d =
                dist_point_triangle(q, rt.p[0], rt.p[1], rt.p[2], n);
            if (d < best)
            {
                best = d;
                best_n = n;
                best_rt = &rt;
            }
        }
        EXPECT_NE(best_rt, nullptr);
        const Point b = barycentric_coordinates(best_n, best_rt->p[0],
                                                 best_rt->p[1], best_rt->p[2]);
        nearest = best_n;
        return best_rt->uv[0] * b[0] + best_rt->uv[1] * b[1] +
               best_rt->uv[2] * b[2];
    }

    Scalar distance_to_chart(int chart, const Point& q) const
    {
        Scalar best = std::numeric_limits<Scalar>::max();
        for (const auto& rt : tris_)
        {
            if (rt.chart != chart)
                continue;
            Point n;
            best = std::min(best, dist_point_triangle(q, rt.p[0], rt.p[1],
                                                       rt.p[2], n));
        }
        return best;
    }

private:
    std::vector<RefTriangle> tris_;
    std::vector<int> charts_;
    int n_charts_{0};
};

// match each output chart to an original chart via face-centroid proximity
std::vector<int> match_charts(const SurfaceMesh& out,
                              const ReferenceSurface& ref)
{
    int out_n = 0;
    auto out_charts = compute_charts(out, out_n);
    EXPECT_EQ(out_n, ref.n_charts());

    std::vector<int> mapping(out_n, -1);
    for (int c = 0; c < out_n; ++c)
    {
        Scalar best = std::numeric_limits<Scalar>::max();
        for (auto f : out.faces())
        {
            if (out_charts[f.idx()] != c)
                continue;
            Point centroid(0);
            for (auto vv : out.vertices(f))
                centroid += out.position(vv);
            centroid /= Scalar(3.0);
            for (int rc = 0; rc < ref.n_charts(); ++rc)
            {
                const Scalar d = ref.distance_to_chart(rc, centroid);
                if (d < best)
                {
                    best = d;
                    mapping[c] = rc;
                }
            }
        }
        EXPECT_NE(mapping[c], -1);
    }

    // every original chart must be represented exactly once
    std::set<int> used(mapping.begin(), mapping.end());
    EXPECT_EQ(used.size(), static_cast<size_t>(out_n));
    return mapping;
}

void check_corner_uvs(const SurfaceMesh& out, const ReferenceSurface& ref)
{
    int out_n = 0;
    auto out_charts = compute_charts(out, out_n);
    const auto mapping = match_charts(out, ref);

    for (auto f : out.faces())
    {
        const int rc = mapping[out_charts[f.idx()]];
        for (auto h : out.halfedges(f))
        {
            const Point& q = out.position(out.to_vertex(h));
            Point nearest;
            const TexCoord want = ref.evaluate(rc, q, nearest);
            const TexCoord got =
                out.get_halfedge_property<TexCoord>("h:tex")[h];
            EXPECT_LT(norm(got - want), kEps)
                << "face " << f << " halfedge " << h;
            EXPECT_LT(norm(q - nearest), Scalar(0.05));
        }
    }
}

void check_finite_geometry(const SurfaceMesh& mesh)
{
    for (auto v : mesh.vertices())
        for (int k = 0; k < 3; ++k)
            EXPECT_TRUE(std::isfinite(mesh.position(v)[k]));
}

void check_temp_properties_removed(const SurfaceMesh& mesh)
{
    for (const auto& name : mesh.vertex_properties())
    {
        EXPECT_NE(name, "v:locked");
        EXPECT_NE(name, "v:texseam");
        EXPECT_NE(name, "v:texcharts");
    }
    for (const auto& name : mesh.edge_properties())
    {
        EXPECT_NE(name, "e:locked");
        EXPECT_NE(name, "e:texseam");
    }
    for (const auto& name : mesh.face_properties())
        EXPECT_NE(name, "f:texchart");
}

// number of distinct UV values carried by the corners of one vertex
int distinct_corner_uvs(const SurfaceMesh& mesh, Vertex v)
{
    auto tex = mesh.get_halfedge_property<TexCoord>("h:tex");
    std::vector<TexCoord> uvs;
    for (auto h : mesh.halfedges(v))
    {
        const Halfedge ch = mesh.opposite_halfedge(h);
        if (!mesh.face(ch).is_valid())
            continue;
        const TexCoord t = tex[ch];
        bool known = false;
        for (const auto& u : uvs)
            if (norm(u - t) <= Scalar(1e-6))
            {
                known = true;
                break;
            }
        if (!known)
            uvs.push_back(t);
    }
    return static_cast<int>(uvs.size());
}

// triangle fan around a high-valence center vertex
SurfaceMesh make_fan(int n)
{
    SurfaceMesh mesh;
    const Vertex center = mesh.add_vertex(Point(0, 0, Scalar(0.02)));
    std::vector<Vertex> ring;
    for (int i = 0; i < n; ++i)
    {
        const Scalar a =
            Scalar(2.0) * static_cast<Scalar>(std::numbers::pi) * i / n;
        ring.push_back(mesh.add_vertex(
            Point(Scalar(1.3) * std::cos(a), Scalar(0.9) * std::sin(a), 0)));
    }
    for (int i = 0; i < n; ++i)
        mesh.add_triangle(center, ring[i], ring[(i + 1) % n]);

    auto tex = mesh.halfedge_property<TexCoord>("h:tex");
    for (auto f : mesh.faces())
        for (auto h : mesh.halfedges(f))
            tex[h] = uv_side_a(mesh.position(mesh.to_vertex(h)));
    return mesh;
}

// two planar shells in one mesh, closer together than the target edge
// length, each with its own texture chart and a very different UV mapping
SurfaceMesh make_close_shells()
{
    SurfaceMesh mesh;
    const int n = 6;
    const Scalar gap = Scalar(0.02);

    for (int shell = 0; shell < 2; ++shell)
    {
        const Scalar z = shell * gap;
        const bool side_a = (shell == 0);
        std::vector<Vertex> verts(n * n);
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
                verts[j * n + i] = mesh.add_vertex(
                    Point(Scalar(0.3) * i, Scalar(0.3) * j, z));
        for (int j = 0; j + 1 < n; ++j)
            for (int i = 0; i + 1 < n; ++i)
            {
                mesh.add_triangle(verts[j * n + i], verts[j * n + i + 1],
                                  verts[(j + 1) * n + i + 1]);
                mesh.add_triangle(verts[j * n + i],
                                  verts[(j + 1) * n + i + 1],
                                  verts[(j + 1) * n + i]);
            }

        auto tex = mesh.halfedge_property<TexCoord>("h:tex");
        for (auto f : mesh.faces())
        {
            // only fill the faces of this shell
            Scalar fz = 0;
            for (auto vv : mesh.vertices(f))
                fz += mesh.position(vv)[2];
            fz /= 3.0f;
            if (std::abs(fz - z) > Scalar(1e-6))
                continue;
            for (auto h : mesh.halfedges(f))
            {
                const auto& p = mesh.position(mesh.to_vertex(h));
                tex[h] = side_a ? uv_side_a(p) : uv_side_b(p);
            }
        }
    }
    return mesh;
}

} // namespace

// 1. continuous, non-affine UVs on an asymmetric grid: every output corner of
//    both remeshing entry points matches the independent chart-local reference
TEST(TextureRemeshingTest, continuous_transfer)
{
    for (const bool adaptive : {false, true})
    {
        Grid g = make_grid(8, 6, false);
        const ReferenceSurface ref(g.mesh);
        const size_t faces_before = g.mesh.n_faces();

        if (adaptive)
            adaptive_remeshing(g.mesh, Scalar(0.12), Scalar(0.35),
                               Scalar(0.02), 6, true, true);
        else
            uniform_remeshing(g.mesh, Scalar(0.22), 6, true, true);

        check_finite_geometry(g.mesh);
        check_temp_properties_removed(g.mesh);
        EXPECT_TRUE(g.mesh.is_triangle_mesh());
        EXPECT_NE(g.mesh.n_faces(), faces_before);
        check_corner_uvs(g.mesh, ref);
    }
}

// 2. a long texture seam must be refined, stay on the original polyline, and
//    carry multiple corner UVs at seam vertices
TEST(TextureRemeshingTest, seam_refinement)
{
    Grid g = make_grid(9, 7, true);
    const ReferenceSurface ref(g.mesh);
    ASSERT_EQ(ref.n_charts(), 2);

    // original seam and boundary polyline segments, plus the exact seam
    // vertex positions
    std::vector<std::pair<Point, Point>> segments;
    std::vector<Point> seam_vertices;
    for (auto e : g.mesh.edges())
    {
        if (g.mesh.is_boundary(e))
        {
            segments.emplace_back(g.mesh.position(g.mesh.vertex(e, 0)),
                                  g.mesh.position(g.mesh.vertex(e, 1)));
            continue;
        }
        if (is_seam_edge(g.mesh, e))
        {
            segments.emplace_back(g.mesh.position(g.mesh.vertex(e, 0)),
                                  g.mesh.position(g.mesh.vertex(e, 1)));
            seam_vertices.push_back(g.mesh.position(g.mesh.vertex(e, 0)));
            seam_vertices.push_back(g.mesh.position(g.mesh.vertex(e, 1)));
        }
    }
    ASSERT_FALSE(segments.empty());

    // count original seam edges (long ones must end up split)
    size_t seam_edges_before = 0;
    Scalar seam_length = 0;
    for (auto e : g.mesh.edges())
    {
        if (g.mesh.is_boundary(e))
            continue;
        if (is_seam_edge(g.mesh, e))
        {
            ++seam_edges_before;
            seam_length += distance(g.mesh.position(g.mesh.vertex(e, 0)),
                                    g.mesh.position(g.mesh.vertex(e, 1)));
        }
    }
    ASSERT_GT(seam_edges_before, 0u);

    uniform_remeshing(g.mesh, Scalar(0.18), 8, true, true);
    check_finite_geometry(g.mesh);
    check_temp_properties_removed(g.mesh);

    // the long seam was actually subdivided
    size_t seam_edges_after = 0;
    size_t multi_uv_vertices = 0;
    for (auto e : g.mesh.edges())
    {
        if (!g.mesh.is_boundary(e) && is_seam_edge(g.mesh, e))
            ++seam_edges_after;
    }
    for (auto v : g.mesh.vertices())
    {
        if (distinct_corner_uvs(g.mesh, v) > 1)
            ++multi_uv_vertices;
    }
    EXPECT_GT(seam_edges_after, seam_edges_before);
    EXPECT_GT(multi_uv_vertices, 0u);

    // every seam/boundary vertex stays on the original polylines
    for (auto v : g.mesh.vertices())
    {
        const bool on_seam = distinct_corner_uvs(g.mesh, v) > 1 ||
                             g.mesh.is_boundary(v);
        if (!on_seam)
            continue;
        Scalar best = std::numeric_limits<Scalar>::max();
        for (const auto& s : segments)
        {
            Point n;
            best = std::min(best, dist_point_line_segment(g.mesh.position(v),
                                                           s.first, s.second,
                                                           n));
        }
        EXPECT_LT(best, kEps) << "seam vertex left its polyline";
    }

    // original seam vertices keep their exact positions
    for (const auto& p : seam_vertices)
    {
        Scalar best = std::numeric_limits<Scalar>::max();
        for (auto v : g.mesh.vertices())
            best = std::min(best, distance(p, g.mesh.position(v)));
        EXPECT_LT(best, Scalar(1e-6));
    }

    // the original seam polyline is fully covered: dense samples on every
    // original segment must land next to an output seam edge
    for (const auto& s : segments)
    {
        for (int k = 0; k <= 10; ++k)
        {
            const Point q = s.first + (s.second - s.first) * (Scalar(k) / 10.0f);
            Scalar best = std::numeric_limits<Scalar>::max();
            for (auto e : g.mesh.edges())
            {
                Point n;
                best = std::min(best,
                                dist_point_line_segment(
                                    q, g.mesh.position(g.mesh.vertex(e, 0)),
                                    g.mesh.position(g.mesh.vertex(e, 1)), n));
            }
            EXPECT_LT(best, kEps);
        }
    }

    // both sides keep their own numerical UV mapping
    check_corner_uvs(g.mesh, ref);
    EXPECT_GT(seam_length, Scalar(0));
}

// 3. dense meshes really coarsen, high-valence fans really flip, and the
//    texture stays correct throughout; both entry points are exercised
TEST(TextureRemeshingTest, coarsen_and_flip)
{
    // coarsening with uniform remeshing
    {
        Grid g = make_grid(12, 10, false);
        const ReferenceSurface ref(g.mesh);
        const size_t before = g.mesh.n_faces();
        uniform_remeshing(g.mesh, Scalar(0.9), 6, true, true);
        EXPECT_LT(g.mesh.n_faces(), before / 2);
        check_finite_geometry(g.mesh);
        check_corner_uvs(g.mesh, ref);
    }

    // valence flips on a triangle fan, uniform entry
    {
        SurfaceMesh mesh = make_fan(14);
        const ReferenceSurface ref(mesh);
        const Vertex center(0);
        const size_t valence_before = mesh.valence(center);
        uniform_remeshing(mesh, Scalar(0.45), 4, true, true);
        EXPECT_LT(mesh.valence(center), valence_before);
        check_corner_uvs(mesh, ref);
    }

    // valence flips on a triangle fan, adaptive entry
    {
        SurfaceMesh mesh = make_fan(14);
        const ReferenceSurface ref(mesh);
        const Vertex center(0);
        const size_t valence_before = mesh.valence(center);
        adaptive_remeshing(mesh, Scalar(0.2), Scalar(0.6), Scalar(0.05), 4,
                           true, true);
        EXPECT_LT(mesh.valence(center), valence_before);
        check_corner_uvs(mesh, ref);
    }
}

// 4. chart-local projection: nearby charts must not attract each other's
//    vertices, with and without back-projection
TEST(TextureRemeshingTest, chart_projection)
{
    // two disjoint shells in one mesh, separated by far less than the
    // target edge length: a global nearest-surface search would pull every
    // vertex onto one of the two planes
    {
        SurfaceMesh mesh = make_close_shells();
        const ReferenceSurface ref(mesh);
        ASSERT_EQ(ref.n_charts(), 2);

        uniform_remeshing(mesh, Scalar(0.16), 6, true, true);

        for (auto v : mesh.vertices())
        {
            const Scalar z = mesh.position(v)[2];
            EXPECT_TRUE(std::abs(z) < kEps ||
                        std::abs(z - Scalar(0.02)) < kEps);
        }
        check_corner_uvs(mesh, ref);
        check_temp_properties_removed(mesh);
    }

    // without projection the geometry is not force-projected, but UV transfer
    // still follows the chart-local nearest-point rule
    {
        SurfaceMesh mesh = make_close_shells();
        const ReferenceSurface ref(mesh);
        uniform_remeshing(mesh, Scalar(0.16), 4, false, true);
        check_corner_uvs(mesh, ref);
        check_temp_properties_removed(mesh);
    }

    // two charts joined at a seam: output vertices of each side stay on
    // their own side of the seam line instead of jumping across it
    Grid g = make_grid(8, 8, true);
    const ReferenceSurface ref(g.mesh);
    uniform_remeshing(g.mesh, Scalar(0.15), 8, true, true);
    int out_n = 0;
    auto out_charts = compute_charts(g.mesh, out_n);
    ASSERT_EQ(out_n, 2);
    const auto mapping = match_charts(g.mesh, ref);
    for (auto f : g.mesh.faces())
    {
        const int rc = mapping[out_charts[f.idx()]];
        for (auto vv : g.mesh.vertices(f))
        {
            const Point& q = g.mesh.position(vv);
            const Scalar own = ref.distance_to_chart(rc, q);
            EXPECT_LT(own, kEps);
            // the other chart must not be closer (allow rounding at points
            // lying exactly on the shared seam)
            const int other = rc == 0 ? 1 : 0;
            const Scalar foreign = ref.distance_to_chart(other, q);
            EXPECT_GE(foreign + Scalar(1e-6), own);
        }
    }
    check_corner_uvs(g.mesh, ref);
}

// 5. feature and selection constraints keep their meaning alongside texture
//    preservation, and texture-off mode reproduces the plain algorithm
TEST(TextureRemeshingTest, feature_selection)
{
    // selection: unselected vertices are frozen, selected region is remeshed
    {
        Grid g = make_grid(7, 7, true);
        auto selected = g.mesh.add_vertex_property<bool>("v:selected");
        std::vector<std::pair<Vertex, Point>> frozen;
        for (auto v : g.mesh.vertices())
        {
            if (g.mesh.position(v)[1] > Scalar(0.9))
            {
                selected[v] = true;
            }
            else
            {
                frozen.emplace_back(v, g.mesh.position(v));
            }
        }
        uniform_remeshing(g.mesh, Scalar(0.2), 6, true, true);
        for (const auto& [v, p] : frozen)
            EXPECT_LT(norm(g.mesh.position(v) - p), Scalar(1e-6));
        check_temp_properties_removed(g.mesh);
    }

    // feature edges/vertices survive and texture constraints never create
    // permanent feature flags on their own
    {
        Grid g = make_grid(6, 5, false);
        auto vfeature = g.mesh.add_vertex_property<bool>("v:feature", false);
        auto efeature = g.mesh.add_edge_property<bool>("e:feature", false);
        const Edge fe(4);
        efeature[fe] = true;
        const Vertex fv0 = g.mesh.vertex(fe, 0);
        const Vertex fv1 = g.mesh.vertex(fe, 1);
        vfeature[fv0] = true;
        vfeature[fv1] = true;
        const Point p0 = g.mesh.position(fv0);
        const Point p1 = g.mesh.position(fv1);

        uniform_remeshing(g.mesh, Scalar(0.22), 6, true, true);

        EXPECT_TRUE(g.mesh.has_vertex_property("v:feature"));
        EXPECT_TRUE(g.mesh.has_edge_property("e:feature"));
        // feature endpoints are preserved in place
        Scalar d0 = std::numeric_limits<Scalar>::max();
        Scalar d1 = std::numeric_limits<Scalar>::max();
        for (auto v : g.mesh.vertices())
        {
            d0 = std::min(d0, distance(p0, g.mesh.position(v)));
            d1 = std::min(d1, distance(p1, g.mesh.position(v)));
        }
        EXPECT_LT(d0, Scalar(1e-6));
        EXPECT_LT(d1, Scalar(1e-6));
        // at least one feature edge remains
        bool any_feature = false;
        auto ef = g.mesh.get_edge_property<bool>("e:feature");
        for (auto e : g.mesh.edges())
            any_feature = any_feature || ef[e];
        EXPECT_TRUE(any_feature);
        check_temp_properties_removed(g.mesh);
    }

    // a mesh without feature properties must not gain any permanent feature
    // flags from texture preservation
    {
        Grid g = make_grid(6, 5, true);
        ASSERT_FALSE(g.mesh.has_vertex_property("v:feature"));
        ASSERT_FALSE(g.mesh.has_edge_property("e:feature"));
        uniform_remeshing(g.mesh, Scalar(0.22), 6, true, true);
        EXPECT_FALSE(g.mesh.has_vertex_property("v:feature"));
        EXPECT_FALSE(g.mesh.has_edge_property("e:feature"));
        check_temp_properties_removed(g.mesh);
    }

    // preserve_texture=false is geometrically identical to a mesh without
    // the texture property
    {
        Grid plain = make_grid(7, 5, true);
        Grid textured = make_grid(7, 5, true);
        auto plain_tex =
            plain.mesh.get_halfedge_property<TexCoord>("h:tex");
        plain.mesh.remove_halfedge_property(plain_tex);

        uniform_remeshing(plain.mesh, Scalar(0.23), 5, true, false);
        uniform_remeshing(textured.mesh, Scalar(0.23), 5, true, false);

        ASSERT_EQ(plain.mesh.n_vertices(), textured.mesh.n_vertices());
        ASSERT_EQ(plain.mesh.n_faces(), textured.mesh.n_faces());
        for (size_t i = 0; i < plain.mesh.positions().size(); ++i)
            EXPECT_LT(norm(plain.mesh.positions()[i] -
                           textured.mesh.positions()[i]),
                      Scalar(1e-6));
    }
}

// 6. OBJ round-trip with per-halfedge texture coordinates preserves both
//    geometry and every corner UV
TEST(TextureRemeshingTest, obj_roundtrip)
{
    for (const bool adaptive : {false, true})
    {
        Grid g = make_grid(8, 6, true);
        if (adaptive)
            adaptive_remeshing(g.mesh, Scalar(0.14), Scalar(0.4), Scalar(0.02),
                               6, true, true);
        else
            uniform_remeshing(g.mesh, Scalar(0.2), 6, true, true);

        const auto path =
            std::filesystem::temp_directory_path() /
            ("pmp_texture_remesh_" + std::to_string(adaptive) + ".obj");

        IOFlags flags;
        flags.use_halfedge_texcoords = true;
        write(g.mesh, path, flags);

        SurfaceMesh loaded;
        read(loaded, path);
        std::filesystem::remove(path);

        ASSERT_EQ(loaded.n_faces(), g.mesh.n_faces());
        ASSERT_EQ(loaded.n_vertices(), g.mesh.n_vertices());
        ASSERT_TRUE(loaded.has_halfedge_property("h:tex"));

        // compare the unordered set of (position, corner UV) triangle
        // signatures so the check does not depend on handle ordering
        using Sig = std::array<Scalar, 15>;
        auto signature = [](const SurfaceMesh& m, Face f) {
            Sig s{};
            auto tex = m.get_halfedge_property<TexCoord>("h:tex");
            int k = 0;
            for (auto h : m.halfedges(f))
            {
                const auto& p = m.position(m.to_vertex(h));
                const auto& t = tex[h];
                s[k++] = p[0];
                s[k++] = p[1];
                s[k++] = p[2];
                s[k++] = t[0];
                s[k++] = t[1];
            }
            return s;
        };

        std::vector<Sig> before;
        std::vector<Sig> after;
        for (auto f : g.mesh.faces())
            before.push_back(signature(g.mesh, f));
        for (auto f : loaded.faces())
            after.push_back(signature(loaded, f));
        std::sort(before.begin(), before.end());
        std::sort(after.begin(), after.end());
        ASSERT_EQ(before.size(), after.size());
        for (size_t i = 0; i < before.size(); ++i)
            for (int k = 0; k < 15; ++k)
                EXPECT_NEAR(before[i][k], after[i][k], kEps);

        // corner UVs at seam vertices remain duplicated after the round trip
        bool duplicated = false;
        for (auto v : loaded.vertices())
            if (distinct_corner_uvs(loaded, v) > 1)
                duplicated = true;
        EXPECT_TRUE(duplicated);
    }
}
