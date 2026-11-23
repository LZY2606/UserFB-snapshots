// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

// Contract cases for pmp::IntrinsicTransfer, driven by task_support/driver.c.

#include "task_support/check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include <Eigen/Dense>

#include "pmp/algorithms/intrinsic_transfer.h"

using namespace pmp;

namespace {

constexpr double tol = 1e-9;

bool close(double a, double b, double t = tol)
{
    return std::abs(a - b) <= t;
}

// ---------------------------------------------------------------------------
// Independent reference helpers (test-side, planar/Euclidean implementation)
// ---------------------------------------------------------------------------

double cross2d(const Eigen::Vector2d& a, const Eigen::Vector2d& b)
{
    return a[0] * b[1] - a[1] * b[0];
}

// barycentric coordinates of p with respect to the 2D triangle (a,b,c)
Eigen::Vector3d bary2d(const Eigen::Vector2d& p, const Eigen::Vector2d& a,
                       const Eigen::Vector2d& b, const Eigen::Vector2d& c)
{
    const double d = cross2d(b - a, c - a);
    const double u = cross2d(p - a, c - a) / d;
    const double v = cross2d(b - a, p - a) / d;
    return {1.0 - u - v, u, v};
}

std::vector<Eigen::Vector2d> clip_halfplane(
    const std::vector<Eigen::Vector2d>& poly, const Eigen::Vector2d& p0,
    const Eigen::Vector2d& p1)
{
    std::vector<Eigen::Vector2d> out;
    const Eigen::Vector2d dir = p1 - p0;
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i)
    {
        const Eigen::Vector2d s = poly[i];
        const Eigen::Vector2d g = poly[(i + 1) % n];
        const double ds = cross2d(dir, s - p0);
        const double dg = cross2d(dir, g - p0);
        if (dg >= 0.0)
        {
            if (ds < 0.0)
                out.push_back(s + ds / (ds - dg) * (g - s));
            out.push_back(g);
        }
        else if (ds >= 0.0)
        {
            out.push_back(s + ds / (ds - dg) * (g - s));
        }
    }
    return out;
}

std::vector<Eigen::Vector2d> clip_triangle(
    std::vector<Eigen::Vector2d> poly,
    const std::array<Eigen::Vector2d, 3>& tri)
{
    for (int i = 0; i < 3; ++i)
    {
        poly = clip_halfplane(poly, tri[i], tri[(i + 1) % 3]);
        if (poly.empty())
            break;
    }
    return poly;
}

// Consistent mass matrix of the current mesh assembled from the 3D
// embedding (the test meshes are flat, so Euclidean and intrinsic coincide).
Eigen::MatrixXd mass_from_positions(const SurfaceMesh& m, size_t n)
{
    Eigen::MatrixXd M = Eigen::MatrixXd::Zero(n, n);
    for (auto f : m.faces())
    {
        int id[3];
        Point p[3];
        Halfedge h = m.halfedge(f);
        for (int i = 0; i < 3; ++i)
        {
            id[i] = static_cast<int>(m.to_vertex(h).idx());
            p[i] = m.position(m.to_vertex(h));
            h = m.next_halfedge(h);
        }
        const double area = 0.5 * norm(cross(p[1] - p[0], p[2] - p[0]));
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                M(id[i], id[j]) += area / 12.0 * (i == j ? 2.0 : 1.0);
    }
    return M;
}

// Right-hand side b_i = integral of (source field) * (current hat_i) through
// an independent planar common refinement of the two triangulations.
Eigen::VectorXd reference_rhs(
    const SurfaceMesh& cur,
    const std::vector<std::array<Eigen::Vector2d, 3>>& src_tris,
    const std::vector<std::array<double, 3>>& src_vals, size_t n)
{
    Eigen::VectorXd b = Eigen::VectorXd::Zero(n);
    for (auto f : cur.faces())
    {
        int id[3];
        Eigen::Vector2d t[3];
        Halfedge h = cur.halfedge(f);
        for (int i = 0; i < 3; ++i)
        {
            const auto v = cur.to_vertex(h);
            id[i] = static_cast<int>(v.idx());
            const auto p = cur.position(v);
            t[i] = Eigen::Vector2d(p[0], p[1]);
            h = cur.next_halfedge(h);
        }
        for (size_t s = 0; s < src_tris.size(); ++s)
        {
            const auto poly =
                clip_triangle({t[0], t[1], t[2]}, src_tris[s]);
            for (size_t k = 1; k + 1 < poly.size(); ++k)
            {
                const Eigen::Vector2d q[3] = {poly[0], poly[k], poly[k + 1]};
                const double at = 0.5 * cross2d(q[1] - q[0], q[2] - q[0]);
                if (!(at > 0.0))
                    continue;
                double fv[3], w[3][3];
                for (int j = 0; j < 3; ++j)
                {
                    const auto ls = bary2d(q[j], src_tris[s][0],
                                           src_tris[s][1], src_tris[s][2]);
                    fv[j] = ls[0] * src_vals[s][0] + ls[1] * src_vals[s][1] +
                            ls[2] * src_vals[s][2];
                    const auto lc = bary2d(q[j], t[0], t[1], t[2]);
                    w[j][0] = lc[0];
                    w[j][1] = lc[1];
                    w[j][2] = lc[2];
                }
                const double vs = fv[0] + fv[1] + fv[2];
                for (int i = 0; i < 3; ++i)
                {
                    double dot = 0.0, sum = 0.0;
                    for (int j = 0; j < 3; ++j)
                    {
                        dot += w[j][i] * fv[j];
                        sum += w[j][i];
                    }
                    b[id[i]] += at / 12.0 * (dot + sum * vs);
                }
            }
        }
    }
    return b;
}

// ---------------------------------------------------------------------------
// metric group
// ---------------------------------------------------------------------------

int metric_folded_quad_new_diagonal()
{
    SurfaceMesh m;
    auto a = m.add_vertex(Point(-1, 0, 0));
    auto b = m.add_vertex(Point(0, 1, 0));
    auto c = m.add_vertex(Point(1, 0, 0));
    auto d = m.add_vertex(Point(0, 0, 1));
    m.add_triangle(a, b, c);
    m.add_triangle(a, c, d);

    IntrinsicTransfer t(m);
    const auto e = m.find_edge(a, c);
    TASK_CHECK(e.is_valid());
    // initial metric is the 3D embedding
    TASK_CHECK(close(t.edge_length(e), 2.0, 1e-12));
    TASK_CHECK(close(t.edge_length(m.find_edge(a, b)), std::sqrt(2.0), 1e-12));

    TASK_CHECK(t.flip(e));
    // PMP slot convention: the same edge handle now connects b and d
    TASK_CHECK(t.mesh().find_edge(b, d) == e);
    TASK_CHECK(t.mesh().n_vertices() == 4 && t.mesh().n_faces() == 2);
    // unfolding gives a planar square: intrinsic diagonal is 2, while the
    // 3D chord of the new endpoints is only sqrt(2)
    TASK_CHECK(close(t.edge_length(e), 2.0, 1e-12));
    TASK_CHECK(std::abs(t.edge_length(e) - std::sqrt(2.0)) > 0.1);
    // vertex positions are untouched by intrinsic flips
    TASK_CHECK(norm(t.mesh().position(b) - Point(0, 1, 0)) < 1e-15);
    TASK_CHECK(norm(t.mesh().position(d) - Point(0, 0, 1)) < 1e-15);

    // flipping back restores the original diagonal and length
    TASK_CHECK(t.flip(e));
    TASK_CHECK(t.mesh().find_edge(a, c) == e);
    TASK_CHECK(close(t.edge_length(e), 2.0, 1e-12));
    return 0;
}

int metric_rejections_preserve_state()
{
    // unfolding of these two triangles is not strictly convex
    SurfaceMesh m;
    auto a = m.add_vertex(Point(0, 0, 0));
    auto b = m.add_vertex(Point(1, 1, 0));
    auto c = m.add_vertex(Point(2, 0, 0));
    auto d = m.add_vertex(Point(-0.1, -0.01, 0));
    m.add_triangle(a, b, c);
    m.add_triangle(a, c, d);

    IntrinsicTransfer t(m);
    const auto e = m.find_edge(a, c);
    TASK_CHECK(e.is_valid());
    const double before = t.edge_length(e);
    TASK_CHECK(!t.flip(e));
    // nothing changed
    TASK_CHECK(t.edge_length(e) == before);
    TASK_CHECK(t.mesh().find_edge(a, c) == e);
    TASK_CHECK(!t.mesh().find_edge(b, d).is_valid());
    TASK_CHECK(t.mesh().n_faces() == 2);

    // boundary edge
    TASK_CHECK(!t.flip(m.find_edge(a, b)));
    // invalid handles
    TASK_CHECK(!t.flip(Edge()));
    TASK_CHECK(!t.flip(Edge(999)));

    // deleted edge handle
    SurfaceMesh m2;
    auto a2 = m2.add_vertex(Point(0, 0, 0));
    auto b2 = m2.add_vertex(Point(1, 0, 0));
    auto c2 = m2.add_vertex(Point(1, 1, 0));
    auto d2 = m2.add_vertex(Point(0, 1, 0));
    m2.add_triangle(a2, b2, c2);
    m2.add_triangle(a2, c2, d2);
    const auto e2 = m2.find_edge(a2, c2);
    m2.delete_edge(e2);
    IntrinsicTransfer t2(m2);
    TASK_CHECK(!t2.flip(e2));

    // closed tetrahedron: every flip would recreate an existing diagonal
    SurfaceMesh tet;
    auto t0 = tet.add_vertex(Point(0, 0, 0));
    auto t1 = tet.add_vertex(Point(1, 0, 0));
    auto t2v = tet.add_vertex(Point(0, 1, 0));
    auto t3 = tet.add_vertex(Point(0, 0, 1));
    tet.add_triangle(t0, t2v, t1);
    tet.add_triangle(t0, t1, t3);
    tet.add_triangle(t1, t2v, t3);
    tet.add_triangle(t2v, t0, t3);
    IntrinsicTransfer tt(tet);
    for (auto te : tt.mesh().edges())
        TASK_CHECK(!tt.flip(te));
    TASK_CHECK(tt.mesh().n_edges() == 6);
    return 0;
}

// ---------------------------------------------------------------------------
// projection group
// ---------------------------------------------------------------------------

int projection_square_flip_nonaffine()
{
    SurfaceMesh m;
    auto a = m.add_vertex(Point(0, 0, 0));
    auto b = m.add_vertex(Point(1, 0, 0));
    auto c = m.add_vertex(Point(1, 1, 0));
    auto d = m.add_vertex(Point(0, 1, 0));
    m.add_triangle(a, b, c);
    m.add_triangle(a, c, d);

    IntrinsicTransfer t(m);
    TASK_CHECK(t.flip(m.find_edge(a, c)));

    // non-affine source field; analytic L2 projection onto the flipped
    // triangulation is u = (0, 1/4, 0, 5/4)
    const std::vector<double> s{0.0, 1.0, 0.0, 2.0};
    const auto u = t.project(s);
    TASK_CHECK(u.size() == 4);
    const double expect[4] = {0.0, 0.25, 0.0, 1.25};
    for (int i = 0; i < 4; ++i)
        TASK_CHECK(close(u[i], expect[i]));

    // independent mass matrix and analytic right-hand side
    const auto M = mass_from_positions(t.mesh(), 4);
    Eigen::Vector4d bref;
    bref << 3.0 / 48.0, 7.0 / 48.0, 3.0 / 48.0, 11.0 / 48.0;
    const Eigen::Vector4d uu(u[0], u[1], u[2], u[3]);
    TASK_CHECK((M * uu - bref).norm() <= tol * bref.norm());

    // per-component integral conservation: source integral is 1/2
    TASK_CHECK(close((M * uu).sum(), 0.5));

    // constants are reproduced
    const auto one = t.project(std::vector<double>(4, 1.0));
    for (int i = 0; i < 4; ++i)
        TASK_CHECK(close(one[i], 1.0));
    return 0;
}

int projection_same_basis_identity()
{
    SurfaceMesh m;
    auto a = m.add_vertex(Point(0, 0, 0));
    auto b = m.add_vertex(Point(1, 0, 0));
    auto c = m.add_vertex(Point(1, 1, 0));
    auto d = m.add_vertex(Point(0, 1, 0));
    m.add_triangle(a, b, c);
    m.add_triangle(a, c, d);

    // without flips the projection is the identity
    IntrinsicTransfer t(m);
    const std::vector<double> s{0.3, 1.7, -2.1, 0.9};
    const auto u = t.project(s);
    TASK_CHECK(u.size() == 4);
    for (int i = 0; i < 4; ++i)
        TASK_CHECK(close(u[i], s[i]));

    // residual against an independently integrated right-hand side
    std::vector<std::array<Eigen::Vector2d, 3>> src_tris{
        {Eigen::Vector2d(0, 0), Eigen::Vector2d(1, 0), Eigen::Vector2d(1, 1)},
        {Eigen::Vector2d(0, 0), Eigen::Vector2d(1, 1), Eigen::Vector2d(0, 1)}};
    std::vector<std::array<double, 3>> src_vals{
        {s[0], s[1], s[2]}, {s[0], s[2], s[3]}};
    const auto M = mass_from_positions(t.mesh(), 4);
    const auto bref = reference_rhs(t.mesh(), src_tris, src_vals, 4);
    const Eigen::Vector4d uu(u[0], u[1], u[2], u[3]);
    TASK_CHECK((M * uu - bref).norm() <= tol * bref.norm());
    TASK_CHECK(close((M * uu).sum(), bref.sum()));

    // single triangle
    SurfaceMesh tri;
    auto p0 = tri.add_vertex(Point(0, 0, 0));
    auto p1 = tri.add_vertex(Point(2, 0, 0));
    auto p2 = tri.add_vertex(Point(0, 1, 0));
    tri.add_triangle(p0, p1, p2);
    IntrinsicTransfer tt(tri);
    const std::vector<double> st{4.0, -1.5, 2.5};
    const auto ut = tt.project(st);
    for (int i = 0; i < 3; ++i)
        TASK_CHECK(close(ut[i], st[i]));
    return 0;
}

// ---------------------------------------------------------------------------
// history group
// ---------------------------------------------------------------------------

struct RectangleFixture
{
    SurfaceMesh m;
    std::vector<Point> pts;
    RectangleFixture()
    {
        pts = {Point(0, 0, 0),   Point(1.1, 0.1, 0), Point(2, 0, 0),
               Point(0.1, 1, 0), Point(1, 1.2, 0),   Point(2.1, 0.9, 0)};
        for (const auto& p : pts)
            m.add_vertex(p);
        m.add_triangle(Vertex(0), Vertex(1), Vertex(4));
        m.add_triangle(Vertex(0), Vertex(4), Vertex(3));
        m.add_triangle(Vertex(1), Vertex(2), Vertex(5));
        m.add_triangle(Vertex(1), Vertex(5), Vertex(4));
    }
    std::vector<double> field() const
    {
        std::vector<double> s(6);
        for (int i = 0; i < 6; ++i)
            s[i] = pts[i][0] * pts[i][0] + 2.0 * pts[i][1]; // non-affine
        return s;
    }
    std::vector<std::array<Eigen::Vector2d, 3>> source_tris() const
    {
        auto xy = [&](int i) { return Eigen::Vector2d(pts[i][0], pts[i][1]); };
        return {{xy(0), xy(1), xy(4)},
                {xy(0), xy(4), xy(3)},
                {xy(1), xy(2), xy(5)},
                {xy(1), xy(5), xy(4)}};
    }
    std::vector<std::array<double, 3>> source_vals(
        const std::vector<double>& s) const
    {
        return {{s[0], s[1], s[4]},
                {s[0], s[4], s[3]},
                {s[1], s[2], s[5]},
                {s[1], s[5], s[4]}};
    }
};

int history_adjacent_flips_piece_transfer()
{
    RectangleFixture fix;
    IntrinsicTransfer t(fix.m);
    const auto eA = fix.m.find_edge(Vertex(0), Vertex(4));
    const auto eC = fix.m.find_edge(Vertex(1), Vertex(4));
    const auto eB = fix.m.find_edge(Vertex(1), Vertex(5));
    TASK_CHECK(eA.is_valid() && eC.is_valid() && eB.is_valid());

    // three adjacent flips; source pieces are clipped and redistributed
    TASK_CHECK(t.flip(eA));
    TASK_CHECK(t.flip(eC));
    TASK_CHECK(t.flip(eB));
    TASK_CHECK(t.mesh().find_edge(Vertex(1), Vertex(3)) == eA);
    TASK_CHECK(t.mesh().find_edge(Vertex(3), Vertex(5)) == eC);
    TASK_CHECK(t.mesh().find_edge(Vertex(2), Vertex(3)) == eB);

    // the mesh is flat: intrinsic lengths equal planar distances
    for (auto e : t.mesh().edges())
    {
        const auto h = t.mesh().halfedge(e, 0);
        const double planar =
            norm(t.mesh().position(t.mesh().to_vertex(h)) -
                 t.mesh().position(t.mesh().from_vertex(h)));
        TASK_CHECK(close(t.edge_length(e), planar, 1e-12));
    }

    const auto s = fix.field();
    const auto u = t.project(s);
    TASK_CHECK(u.size() == 6);

    // independent planar common-refinement reference
    const auto M = mass_from_positions(t.mesh(), 6);
    const auto bref =
        reference_rhs(t.mesh(), fix.source_tris(), fix.source_vals(s), 6);
    Eigen::VectorXd uu(6);
    for (int i = 0; i < 6; ++i)
        uu[i] = u[i];
    TASK_CHECK((M * uu - bref).norm() <= tol * bref.norm());
    const Eigen::VectorXd uref = M.ldlt().solve(bref);
    for (int i = 0; i < 6; ++i)
        TASK_CHECK(close(u[i], uref[i]));

    // integral conservation against the analytic source integral
    double src_integral = 0.0;
    const auto tris = fix.source_tris();
    const auto vals = fix.source_vals(s);
    for (size_t k = 0; k < tris.size(); ++k)
    {
        const double area =
            0.5 * cross2d(tris[k][1] - tris[k][0], tris[k][2] - tris[k][0]);
        src_integral += area / 3.0 * (vals[k][0] + vals[k][1] + vals[k][2]);
    }
    TASK_CHECK(close(bref.sum(), src_integral));
    TASK_CHECK(close((M * uu).sum(), src_integral));
    return 0;
}

int history_flip_back_restores()
{
    RectangleFixture fix;
    IntrinsicTransfer t(fix.m);
    const auto eA = fix.m.find_edge(Vertex(0), Vertex(4));
    const auto eC = fix.m.find_edge(Vertex(1), Vertex(4));
    const auto eB = fix.m.find_edge(Vertex(1), Vertex(5));
    const auto s = fix.field();

    TASK_CHECK(t.flip(eA));
    TASK_CHECK(t.flip(eC));
    TASK_CHECK(t.flip(eB));

    // the flips actually changed the projection
    const auto u_mid = t.project(s);
    double max_diff = 0.0;
    for (int i = 0; i < 6; ++i)
        max_diff = std::max(max_diff, std::abs(u_mid[i] - s[i]));
    TASK_CHECK(max_diff > 1e-6);

    // flipping the same edge slots back restores the original triangulation
    TASK_CHECK(t.flip(eB));
    TASK_CHECK(t.flip(eC));
    TASK_CHECK(t.flip(eA));
    TASK_CHECK(t.mesh().find_edge(Vertex(0), Vertex(4)) == eA);
    TASK_CHECK(t.mesh().find_edge(Vertex(1), Vertex(4)) == eC);
    TASK_CHECK(t.mesh().find_edge(Vertex(1), Vertex(5)) == eB);
    TASK_CHECK(close(t.edge_length(eA), norm(fix.pts[0] - fix.pts[4]), 1e-12));
    TASK_CHECK(close(t.edge_length(eC), norm(fix.pts[1] - fix.pts[4]), 1e-12));
    TASK_CHECK(close(t.edge_length(eB), norm(fix.pts[1] - fix.pts[5]), 1e-12));

    // and the projection is the identity again: the source field is
    // recovered relative to the initial surface, without accumulation
    const auto u = t.project(s);
    TASK_CHECK(u.size() == 6);
    for (int i = 0; i < 6; ++i)
        TASK_CHECK(close(u[i], s[i]));

    // repeated projection does not accumulate either
    const auto u2 = t.project(u);
    for (int i = 0; i < 6; ++i)
        TASK_CHECK(close(u2[i], u[i]));
    return 0;
}

// ---------------------------------------------------------------------------
// state group
// ---------------------------------------------------------------------------

int state_multi_component_sparse_handles()
{
    SurfaceMesh m;
    // component 1: unit square
    auto a = m.add_vertex(Point(0, 0, 0));
    auto b = m.add_vertex(Point(1, 0, 0));
    auto c = m.add_vertex(Point(1, 1, 0));
    auto d = m.add_vertex(Point(0, 1, 0));
    m.add_triangle(a, b, c);
    m.add_triangle(a, c, d);
    // component 2: single triangle
    auto p = m.add_vertex(Point(10, 0, 0));
    auto q = m.add_vertex(Point(11, 0, 0));
    auto r = m.add_vertex(Point(10, 1, 0));
    m.add_triangle(p, q, r);
    // isolated live vertices (never incident to any face)
    auto w = m.add_vertex(Point(5, 5, 5));
    auto w2 = m.add_vertex(Point(6, 6, 6));
    // deleted component: leaves garbage slots behind (deleting the only
    // triangle of a component also deletes its vertices in PMP)
    auto x = m.add_vertex(Point(20, 0, 0));
    auto y = m.add_vertex(Point(21, 0, 0));
    auto z = m.add_vertex(Point(20, 1, 0));
    m.add_triangle(x, y, z);
    m.delete_vertex(x);
    TASK_CHECK(m.is_deleted(x));
    TASK_CHECK(m.is_deleted(y) && m.is_deleted(z));
    TASK_CHECK(m.vertices_size() > m.n_vertices());

    IntrinsicTransfer t(m);
    const size_t slots = m.vertices_size();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> s(slots, 0.0);
    s[a.idx()] = s[b.idx()] = s[c.idx()] = s[d.idx()] = 3.0;
    s[p.idx()] = 1.0;
    s[q.idx()] = 2.0;
    s[r.idx()] = 3.0;
    s[w.idx()] = 42.0;
    s[w2.idx()] = -13.0;
    s[x.idx()] = nan; // deleted slots: ignored
    s[y.idx()] = nan;
    s[z.idx()] = nan;

    auto u = t.project(s);
    TASK_CHECK(u.size() == slots);
    TASK_CHECK(u[x.idx()] == 0.0 && u[y.idx()] == 0.0 && u[z.idx()] == 0.0);
    TASK_CHECK(u[w.idx()] == 42.0 && u[w2.idx()] == -13.0);
    for (const auto v : {a, b, c, d})
        TASK_CHECK(close(u[v.idx()], 3.0));
    TASK_CHECK(close(u[p.idx()], 1.0));
    TASK_CHECK(close(u[q.idx()], 2.0));
    TASK_CHECK(close(u[r.idx()], 3.0));

    // flip inside component 1; component 2 and the isolated vertices are
    // not affected
    TASK_CHECK(t.flip(m.find_edge(a, c)));
    std::vector<double> g(slots, 0.0);
    g[a.idx()] = 0.0;
    g[b.idx()] = 1.0;
    g[c.idx()] = 0.0;
    g[d.idx()] = 2.0; // integral 1/2 over component 1
    g[p.idx()] = 1.0;
    g[q.idx()] = 2.0;
    g[r.idx()] = 3.0;
    g[w.idx()] = -7.0;
    g[w2.idx()] = 13.0;
    const auto v = t.project(g);

    // per-component conservation on component 1
    const auto M = mass_from_positions(t.mesh(), slots);
    Eigen::VectorXd vv(slots);
    for (size_t i = 0; i < slots; ++i)
        vv[i] = v[i];
    const Eigen::VectorXd Mv = M * vv;
    double int1 = 0.0;
    for (const auto vtx : {a, b, c, d})
        int1 += Mv[vtx.idx()];
    TASK_CHECK(close(int1, 0.5));
    // component 2 keeps its identity projection
    TASK_CHECK(close(v[p.idx()], 1.0));
    TASK_CHECK(close(v[q.idx()], 2.0));
    TASK_CHECK(close(v[r.idx()], 3.0));
    // isolated and deleted slots
    TASK_CHECK(v[w.idx()] == -7.0 && v[w2.idx()] == 13.0);
    TASK_CHECK(v[x.idx()] == 0.0 && v[y.idx()] == 0.0 && v[z.idx()] == 0.0);

    // the session flip did not touch the input mesh
    TASK_CHECK(m.find_edge(a, c).is_valid());
    TASK_CHECK(!m.find_edge(b, d).is_valid());
    return 0;
}

int state_input_mutation_and_exceptions()
{
    SurfaceMesh m;
    auto a = m.add_vertex(Point(0, 0, 0));
    auto b = m.add_vertex(Point(1, 0, 0));
    auto c = m.add_vertex(Point(1, 1, 0));
    auto d = m.add_vertex(Point(0, 1, 0));
    m.add_triangle(a, b, c);
    m.add_triangle(a, c, d);
    auto junk = m.add_vertex(Point(9, 9, 9));
    m.delete_vertex(junk); // sparse slot
    auto tag = m.add_vertex_property<int>("v:tag", 0);
    for (auto vtx : m.vertices())
        tag[vtx] = 10 * static_cast<int>(vtx.idx());

    IntrinsicTransfer t(m);
    // custom properties were deep-copied
    auto ttag = t.mesh().get_vertex_property<int>("v:tag");
    TASK_CHECK(static_cast<bool>(ttag));
    for (auto vtx : t.mesh().vertices())
        TASK_CHECK(ttag[vtx] == 10 * static_cast<int>(vtx.idx()));

    const auto e = m.find_edge(a, c);
    const double length_before = t.edge_length(e);
    std::vector<double> s(m.vertices_size(), 0.0);
    s[a.idx()] = 0.0;
    s[b.idx()] = 1.0;
    s[c.idx()] = 0.0;
    s[d.idx()] = 2.0;
    s[junk.idx()] = std::numeric_limits<double>::quiet_NaN(); // deleted: ok
    const auto before = t.project(s);

    // mutate the input mesh afterwards: positions, properties, topology
    m.position(a) = Point(100, 100, 100);
    tag[a] = -5;
    m.flip(e); // flips the input mesh, not the session
    TASK_CHECK(m.find_edge(b, d).is_valid());

    // the session is unaffected
    TASK_CHECK(t.edge_length(e) == length_before);
    TASK_CHECK(t.mesh().find_edge(a, c) == e);
    TASK_CHECK(ttag[a] == 0);
    TASK_CHECK(norm(t.mesh().position(a) - Point(0, 0, 0)) < 1e-15);
    const auto after = t.project(s);
    TASK_CHECK(after.size() == before.size());
    for (size_t i = 0; i < before.size(); ++i)
        TASK_CHECK(after[i] == before[i]);

    // wrong array size throws and the session stays usable
    bool threw = false;
    try
    {
        t.project(std::vector<double>(3, 1.0));
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    // non-finite value at a live vertex throws
    threw = false;
    auto bad = s;
    bad[b.idx()] = std::numeric_limits<double>::infinity();
    try
    {
        t.project(bad);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    const auto again = t.project(s);
    for (size_t i = 0; i < before.size(); ++i)
        TASK_CHECK(again[i] == before[i]);

    // degenerate input triangles are rejected at construction
    threw = false;
    SurfaceMesh deg;
    auto d0 = deg.add_vertex(Point(0, 0, 0));
    auto d1 = deg.add_vertex(Point(1, 0, 0));
    auto d2 = deg.add_vertex(Point(2, 0, 0));
    deg.add_triangle(d0, d1, d2);
    try
    {
        IntrinsicTransfer bad_session(deg);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    // non-triangle faces are rejected as well
    threw = false;
    SurfaceMesh quad;
    auto q0 = quad.add_vertex(Point(0, 0, 0));
    auto q1 = quad.add_vertex(Point(1, 0, 0));
    auto q2 = quad.add_vertex(Point(1, 1, 0));
    auto q3 = quad.add_vertex(Point(0, 1, 0));
    quad.add_quad(q0, q1, q2, q3);
    try
    {
        IntrinsicTransfer bad_session(quad);
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

} // namespace

const task_case task_cases[] = {
    {"metric", "folded_quad_new_diagonal", metric_folded_quad_new_diagonal},
    {"metric", "rejections_preserve_state", metric_rejections_preserve_state},
    {"projection", "square_flip_nonaffine", projection_square_flip_nonaffine},
    {"projection", "same_basis_identity", projection_same_basis_identity},
    {"history", "adjacent_flips_piece_transfer",
     history_adjacent_flips_piece_transfer},
    {"history", "flip_back_restores", history_flip_back_restores},
    {"state", "multi_component_sparse_handles",
     state_multi_component_sparse_handles},
    {"state", "input_mutation_and_exceptions",
     state_input_mutation_and_exceptions}};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
