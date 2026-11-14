// Copyright 2011-2020 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/remeshing.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

#include "pmp/algorithms/barycentric_coordinates.h"
#include "pmp/algorithms/normals.h"
#include "pmp/algorithms/triangle_kd_tree.h"

namespace pmp {

namespace {

using Metric = Matrix<Scalar, 3, 3>;

// A geometric edit is rejected if it would make a triangle degenerate or
// nearly tangent to its frozen reference normal. Requiring a positive
// cosine floor (rather than only a positive sign) keeps out near-degenerate
// sliver triangles, whose orientation is too unstable to be trustworthy.
constexpr Scalar MIN_FACE_COS = Scalar(0.5);

//! Tensor-driven anisotropic remeshing.
//!
//! The reference surface and the metric tensor field are frozen when the
//! algorithm starts. Every new or moved vertex is projected onto that
//! reference surface and its metric is barycentrically interpolated from the
//! reference triangle, so the repeatedly edited current mesh never replaces
//! the reference field.
//!
//! Orientation is anchored directly to the frozen reference surface: the
//! reference normal nearest to a triangle's centroid is its absolute
//! orientation. Every geometric edit is rejected if it would make a triangle
//! degenerate or orient it against that normal. Because the reference is a
//! fixed consistently oriented manifold, neighboring triangles always get
//! mutually consistent anchors, which prevents orientation flips from
//! cascading.
class AnisotropicRemeshing
{
public:
    explicit AnisotropicRemeshing(SurfaceMesh& mesh);

    void run(unsigned int iterations);

private:
    void validate_inputs() const;
    void preprocessing();
    void postprocessing();

    void split_long_edges();
    void collapse_short_edges();
    void flip_edges();
    void tangential_smoothing(unsigned int iterations);

    // metric geometry
    Scalar metric_length(Vertex v0, Vertex v1) const;
    bool is_too_long(Vertex v0, Vertex v1) const
    {
        return metric_length(v0, v1) > Scalar(4.0 / 3.0);
    }
    bool is_too_short(Vertex v0, Vertex v1) const
    {
        return metric_length(v0, v1) < Scalar(4.0 / 5.0);
    }
    Scalar triangle_quality(Vertex v0, Vertex v1, Vertex v2) const;

    // reference field and orientation bookkeeping
    void transport_metric(Vertex v, bool move_position);
    void reference_projection(Vertex v, Point& projected_point,
                              Normal& projected_normal,
                              Metric& projected_metric) const;
    Normal reference_normal_at(const Point& x) const;
    bool face_stays_oriented(Face f, const Point& moved_position,
                             Vertex moved_vertex) const;
    bool displacement_valid(Vertex v, const Point& new_position) const;
    bool collapse_geometry_ok(Halfedge h) const;

    SurfaceMesh& mesh_;
    std::shared_ptr<SurfaceMesh> refmesh_;
    std::unique_ptr<detail::TriangleKdTree> kd_tree_;

    VertexProperty<Point> points_;
    VertexProperty<Point> vnormal_;
    VertexProperty<Metric> vmetric_;
    VertexProperty<bool> vfeature_;
    EdgeProperty<bool> efeature_;
    VertexProperty<bool> vlocked_;
    EdgeProperty<bool> elocked_;

    VertexProperty<Point> refpoints_;
    VertexProperty<Point> refnormals_;
    VertexProperty<Metric> refmetrics_;

    bool has_feature_vertices_{false};
    bool has_feature_edges_{false};
};

AnisotropicRemeshing::AnisotropicRemeshing(SurfaceMesh& mesh) : mesh_(mesh)
{
    validate_inputs();
}

void AnisotropicRemeshing::validate_inputs() const
{
    if (!mesh_.is_triangle_mesh())
        throw InvalidInputException("Input is not a triangle mesh!");

    auto metric = mesh_.get_vertex_property<Metric>("v:metric");
    if (!metric)
        throw InvalidInputException(
            "Anisotropic remeshing requires vertex property \"v:metric\".");

    auto points = mesh_.get_vertex_property<Point>("v:point");

    constexpr Scalar sym_eps = Scalar(1e-9);
    constexpr Scalar spd_eps = Scalar(1e-10);

    for (auto v : mesh_.vertices())
    {
        // finite coordinates
        for (int k = 0; k < 3; ++k)
        {
            if (!std::isfinite(points[v][k]))
                throw InvalidInputException(
                    "Anisotropic remeshing: non-finite vertex coordinate.");
        }

        const Metric& m = metric[v];

        // finite entries
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                if (!std::isfinite(m(i, j)))
                    throw InvalidInputException(
                        "Anisotropic remeshing: non-finite metric entry.");

        // symmetry
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j)
            {
                const Scalar tol =
                    sym_eps * (Scalar(1.0) + std::fabs(m(i, j)) +
                               std::fabs(m(j, i)));
                if (std::fabs(m(i, j) - m(j, i)) > tol)
                    throw InvalidInputException(
                        "Anisotropic remeshing: metric is not symmetric.");
            }

        // positive definiteness via symmetric eigen decomposition
        Scalar e1, e2, e3;
        Point ev1, ev2, ev3;
        if (!symmetric_eigendecomposition(m, e1, e2, e3, ev1, ev2, ev3))
            throw InvalidInputException(
                "Anisotropic remeshing: eigendecomposition of metric failed.");

        const Scalar scale =
            std::max(Scalar(1.0), std::max(std::fabs(e1),
                                           std::max(std::fabs(e2),
                                                    std::fabs(e3))));
        if (e1 <= spd_eps * scale || e2 <= spd_eps * scale ||
            e3 <= spd_eps * scale)
            throw InvalidInputException(
                "Anisotropic remeshing: metric is not positive definite.");
    }

    // non-degenerate faces
    for (auto f : mesh_.faces())
    {
        auto vv = mesh_.vertices(f);
        const Point& p0 = points[*vv];
        ++vv;
        const Point& p1 = points[*vv];
        ++vv;
        const Point& p2 = points[*vv];
        if (norm(cross(p1 - p0, p2 - p0)) == Scalar(0.0))
            throw InvalidInputException(
                "Anisotropic remeshing: degenerate input face.");
    }
}

void AnisotropicRemeshing::preprocessing()
{
    points_ = mesh_.vertex_property<Point>("v:point");
    vmetric_ = mesh_.vertex_property<Metric>("v:metric");

    vertex_normals(mesh_);
    vnormal_ = mesh_.vertex_property<Point>("v:normal");

    has_feature_vertices_ = mesh_.has_vertex_property("v:feature");
    has_feature_edges_ = mesh_.has_edge_property("e:feature");
    vfeature_ = mesh_.vertex_property<bool>("v:feature", false);
    efeature_ = mesh_.edge_property<bool>("e:feature", false);
    vlocked_ = mesh_.add_vertex_property<bool>("v:locked", false);
    elocked_ = mesh_.add_edge_property<bool>("e:locked", false);

    // lock unselected vertices if some vertices are selected
    auto vselected = mesh_.get_vertex_property<bool>("v:selected");
    if (vselected)
    {
        bool has_selection = false;
        for (auto v : mesh_.vertices())
        {
            if (vselected[v])
            {
                has_selection = true;
                break;
            }
        }

        if (has_selection)
        {
            for (auto v : mesh_.vertices())
                vlocked_[v] = !vselected[v];

            for (auto e : mesh_.edges())
                elocked_[e] = (vlocked_[mesh_.vertex(e, 0)] ||
                               vlocked_[mesh_.vertex(e, 1)]);
        }
    }

    // lock feature corners
    for (auto v : mesh_.vertices())
    {
        if (vfeature_[v])
        {
            int c = 0;
            for (auto h : mesh_.halfedges(v))
                if (efeature_[mesh_.edge(h)])
                    ++c;

            if (c != 2)
                vlocked_[v] = true;
        }
    }

    // freeze reference surface and tensor field. The copy constructor deep
    // copies every property (in particular "v:metric"), whereas assign()
    // would only carry the standard properties. The reference mesh keeps the
    // initial face numbering, and TriangleKdTree resolves equidistant
    // reference triangles in favor of the smallest face index, so new and
    // moved vertices are always transported from this frozen field rather
    // than from the repeatedly interpolated current mesh.
    refmesh_ = std::make_shared<SurfaceMesh>(mesh_);
    vertex_normals(*refmesh_);
    refpoints_ = refmesh_->vertex_property<Point>("v:point");
    refnormals_ = refmesh_->vertex_property<Point>("v:normal");
    refmetrics_ = refmesh_->vertex_property<Metric>("v:metric");

    kd_tree_ = std::make_unique<detail::TriangleKdTree>(refmesh_, 0);
}

void AnisotropicRemeshing::postprocessing()
{
    mesh_.remove_vertex_property(vlocked_);
    mesh_.remove_edge_property(elocked_);

    if (!has_feature_vertices_)
        mesh_.remove_vertex_property(vfeature_);
    if (!has_feature_edges_)
        mesh_.remove_edge_property(efeature_);
}

void AnisotropicRemeshing::run(unsigned int iterations)
{
    // zero iterations only validate the input; do not touch the mesh
    if (iterations == 0)
        return;

    preprocessing();

    for (unsigned int i = 0; i < iterations; ++i)
    {
        split_long_edges();

        vertex_normals(mesh_);

        collapse_short_edges();

        flip_edges();

        tangential_smoothing(5);
    }

    vertex_normals(mesh_);
    postprocessing();
}

Scalar AnisotropicRemeshing::metric_length(Vertex v0, Vertex v1) const
{
    const Point d = points_[v1] - points_[v0];
    const Metric m = Scalar(0.5) * (vmetric_[v0] + vmetric_[v1]);
    const Scalar q = dot(d, m * d);
    return std::sqrt(std::max(Scalar(0.0), q));
}

Scalar AnisotropicRemeshing::triangle_quality(Vertex v0, Vertex v1,
                                              Vertex v2) const
{
    const Metric m =
        Scalar(1.0 / 3.0) * (vmetric_[v0] + vmetric_[v1] + vmetric_[v2]);

    const Point e01 = points_[v1] - points_[v0];
    const Point e12 = points_[v2] - points_[v1];
    const Point e20 = points_[v0] - points_[v2];
    const Point e02 = -e20;

    const Scalar l01 = dot(e01, m * e01);
    const Scalar l12 = dot(e12, m * e12);
    const Scalar l20 = dot(e20, m * e20);
    const Scalar g02 = dot(e01, m * e02);

    // Gram determinant gives four times the squared metric (Gram) area
    const Scalar det =
        std::max(Scalar(0.0), l01 * dot(e02, m * e02) - g02 * g02);
    const Scalar area = Scalar(0.5) * std::sqrt(det);

    const Scalar sum = l01 + l12 + l20;
    if (sum == Scalar(0.0))
        return Scalar(0.0);

    return Scalar(4.0) * Scalar(std::numbers::sqrt3) * area / sum;
}

void AnisotropicRemeshing::transport_metric(Vertex v, bool move_position)
{
    Point projected;
    Normal projected_normal;
    Metric metric;
    reference_projection(v, projected, projected_normal, metric);

    if (move_position)
        points_[v] = projected;
    vnormal_[v] = projected_normal;
    vmetric_[v] = metric;
}

void AnisotropicRemeshing::reference_projection(
    Vertex v, Point& projected_point, Normal& projected_normal,
    Metric& projected_metric) const
{
    // find closest triangle of the frozen reference mesh
    auto nn = kd_tree_->nearest(points_[v]);
    const Face f = nn.face;

    auto fv = refmesh_->vertices(f);
    const Point p0 = refpoints_[*fv];
    const Point n0 = refnormals_[*fv];
    const Metric m0 = refmetrics_[*fv];
    ++fv;
    const Point p1 = refpoints_[*fv];
    const Point n1 = refnormals_[*fv];
    const Metric m1 = refmetrics_[*fv];
    ++fv;
    const Point p2 = refpoints_[*fv];
    const Point n2 = refnormals_[*fv];
    const Metric m2 = refmetrics_[*fv];

    // barycentric coordinates on the reference triangle
    Point b = barycentric_coordinates(nn.nearest, p0, p1, p2);

    // guard against tiny numerical excursions outside the triangle
    for (int k = 0; k < 3; ++k)
        b[k] = std::max(Scalar(0.0), b[k]);
    const Scalar sum = b[0] + b[1] + b[2];
    if (sum > Scalar(0.0))
        b *= Scalar(1.0) / sum;

    Point n = n0 * b[0] + n1 * b[1] + n2 * b[2];
    n.normalize();

    projected_point = nn.nearest;
    projected_normal = n;
    projected_metric = m0 * b[0] + m1 * b[1] + m2 * b[2];
}

Normal AnisotropicRemeshing::reference_normal_at(const Point& x) const
{
    // nearest triangle of the frozen reference surface resolves ties to the
    // smallest face index, giving one globally consistent orientation field
    auto nn = kd_tree_->nearest(x);
    auto fv = refmesh_->vertices(nn.face);
    const Normal& n0 = refnormals_[*fv];
    ++fv;
    const Normal& n1 = refnormals_[*fv];
    ++fv;
    const Normal& n2 = refnormals_[*fv];
    Point n = n0 + n1 + n2;
    n.normalize();
    return n;
}

bool AnisotropicRemeshing::face_stays_oriented(
    Face f, const Point& moved_position, Vertex moved_vertex) const
{
    auto vv = mesh_.vertices(f);
    const Point& pa = points_[*vv];
    const Vertex a = *vv;
    ++vv;
    const Point& pb = points_[*vv];
    const Vertex b = *vv;
    ++vv;
    const Point& pc = points_[*vv];
    const Vertex c = *vv;

    const Point& pa2 = (a == moved_vertex) ? moved_position : pa;
    const Point& pb2 = (b == moved_vertex) ? moved_position : pb;
    const Point& pc2 = (c == moved_vertex) ? moved_position : pc;

    const Point n_new = cross(pb2 - pa2, pc2 - pa2);
    const Scalar nn = norm(n_new);
    if (nn == Scalar(0.0))
        return false;

    // anchor to the reference normal at the triangle's centroid
    const Point centroid = (pa2 + pb2 + pc2) * Scalar(1.0 / 3.0);
    const Normal n_ref = reference_normal_at(centroid);
    return dot(n_new, n_ref) > MIN_FACE_COS * nn;
}

bool AnisotropicRemeshing::displacement_valid(
    Vertex v, const Point& new_position) const
{
    for (auto f : mesh_.faces(v))
        if (!face_stays_oriented(f, new_position, v))
            return false;
    return true;
}

bool AnisotropicRemeshing::collapse_geometry_ok(Halfedge h) const
{
    const Vertex vh = mesh_.to_vertex(h);   // surviving vertex
    const Vertex vo = mesh_.from_vertex(h); // removed vertex
    const Halfedge oh = mesh_.opposite_halfedge(h);
    const Face fh = mesh_.face(h);
    const Face fo = mesh_.face(oh);

    // every face incident to the removed vertex survives as a triangle
    // with the surviving vertex taking its place
    for (auto hh : mesh_.halfedges(vo))
    {
        const Face f = mesh_.face(hh);
        if (!f.is_valid() || f == fh || f == fo)
            continue;

        if (!face_stays_oriented(f, points_[vh], vo))
            return false;
    }
    return true;
}

void AnisotropicRemeshing::split_long_edges()
{
    bool ok;
    int i;

    for (ok = false, i = 0; !ok && i < 10; ++i)
    {
        ok = true;

        for (auto e : mesh_.edges())
        {
            const Vertex v0 = mesh_.vertex(e, 0);
            const Vertex v1 = mesh_.vertex(e, 1);

            if (!elocked_[e] && is_too_long(v0, v1))
            {
                const Point& p0 = points_[v0];
                const Point& p1 = points_[v1];

                const bool is_feature = efeature_[e];
                const bool is_boundary = mesh_.is_boundary(e);

                // geometric midpoint first; splitting at a midpoint can
                // never invert an existing oriented triangle
                const Point midpoint = (p0 + p1) * Scalar(0.5);
                const Vertex vnew = mesh_.add_vertex(midpoint);
                mesh_.split(e, vnew);

                vnormal_[vnew] = vertex_normal(mesh_, vnew);
                vmetric_[vnew] =
                    Scalar(0.5) * (vmetric_[v0] + vmetric_[v1]);

                if (is_feature)
                {
                    // stay on the feature polyline, only transport the
                    // tensor from the frozen reference field
                    const Edge enew = is_boundary
                                          ? Edge(mesh_.n_edges() - 2)
                                          : Edge(mesh_.n_edges() - 3);
                    efeature_[enew] = true;
                    vfeature_[vnew] = true;
                    transport_metric(vnew, false);
                }
                else
                {
                    // project onto the reference surface, but only move if
                    // every adjacent face stays oriented against the frozen
                    // reference; otherwise keep the safe midpoint
                    Point projected;
                    Normal pn;
                    Metric pm;
                    reference_projection(vnew, projected, pn, pm);
                    if (displacement_valid(vnew, projected))
                    {
                        points_[vnew] = projected;
                    }
                    vnormal_[vnew] = pn;
                    vmetric_[vnew] = pm;
                }

                ok = false;
            }
        }
    }
}

void AnisotropicRemeshing::collapse_short_edges()
{
    Vertex v0, v1;
    Halfedge h0, h1, h01, h10;
    bool ok, b0, b1, l0, l1, f0, f1;
    int i;
    bool hcol01, hcol10;

    for (ok = false, i = 0; !ok && i < 10; ++i)
    {
        ok = true;

        for (auto e : mesh_.edges())
        {
            if (!mesh_.is_deleted(e) && !elocked_[e])
            {
                h10 = mesh_.halfedge(e, 0);
                h01 = mesh_.halfedge(e, 1);
                v0 = mesh_.to_vertex(h10);
                v1 = mesh_.to_vertex(h01);

                if (is_too_short(v0, v1))
                {
                    // get status
                    b0 = mesh_.is_boundary(v0);
                    b1 = mesh_.is_boundary(v1);
                    l0 = vlocked_[v0];
                    l1 = vlocked_[v1];
                    f0 = vfeature_[v0];
                    f1 = vfeature_[v1];
                    hcol01 = hcol10 = true;

                    // boundary rules
                    if (b0 && b1)
                    {
                        if (!mesh_.is_boundary(e))
                            continue;
                    }
                    else if (b0)
                        hcol01 = false;
                    else if (b1)
                        hcol10 = false;

                    // locked rules
                    if (l0 && l1)
                        continue;
                    else if (l0)
                        hcol01 = false;
                    else if (l1)
                        hcol10 = false;

                    // feature rules
                    if (f0 && f1)
                    {
                        // edge must be feature
                        if (!efeature_[e])
                            continue;

                        // the other two edges removed by collapse must not
                        // be features
                        h0 = mesh_.prev_halfedge(h01);
                        h1 = mesh_.next_halfedge(h10);
                        if (efeature_[mesh_.edge(h0)] ||
                            efeature_[mesh_.edge(h1)])
                            hcol01 = false;
                        h0 = mesh_.prev_halfedge(h10);
                        h1 = mesh_.next_halfedge(h01);
                        if (efeature_[mesh_.edge(h0)] ||
                            efeature_[mesh_.edge(h1)])
                            hcol10 = false;
                    }
                    else if (f0)
                        hcol01 = false;
                    else if (f1)
                        hcol10 = false;

                    // topological rules
                    const bool collapse_ok = mesh_.is_collapse_ok(h01);
                    if (hcol01)
                        hcol01 = collapse_ok;
                    if (hcol10)
                        hcol10 = collapse_ok;

                    // geometric rules: no degenerate or flipped faces
                    if (hcol01)
                        hcol01 = collapse_geometry_ok(h01);
                    if (hcol10)
                        hcol10 = collapse_geometry_ok(h10);

                    // both collapses possible: collapse into vertex w/
                    // higher valence
                    if (hcol01 && hcol10)
                    {
                        if (mesh_.valence(v0) < mesh_.valence(v1))
                            hcol10 = false;
                        else
                            hcol01 = false;
                    }

                    // try v1 -> v0
                    if (hcol10)
                    {
                        // check metric lengths of the new neighbor edges
                        for (auto vv : mesh_.vertices(v1))
                        {
                            if (is_too_long(v0, vv))
                            {
                                hcol10 = false;
                                break;
                            }
                        }

                        if (hcol10)
                        {
                            mesh_.collapse(h10);
                            // refresh the tensor from the reference field,
                            // keeping the surviving vertex in place
                            transport_metric(v0, false);
                            ok = false;
                        }
                    }

                    // try v0 -> v1
                    else if (hcol01)
                    {
                        for (auto vv : mesh_.vertices(v0))
                        {
                            if (is_too_long(v1, vv))
                            {
                                hcol01 = false;
                                break;
                            }
                        }

                        if (hcol01)
                        {
                            mesh_.collapse(h01);
                            transport_metric(v1, false);
                            ok = false;
                        }
                    }
                }
            }
        }
    }

    mesh_.garbage_collection();
}

void AnisotropicRemeshing::flip_edges()
{
    constexpr Scalar strict_eps = Scalar(1e-9);

    bool ok;
    int i;
    for (ok = false, i = 0; !ok && i < 10; ++i)
    {
        ok = true;

        for (auto e : mesh_.edges())
        {
            if (elocked_[e] || efeature_[e] || mesh_.is_boundary(e))
                continue;

            const Halfedge h0 = mesh_.halfedge(e, 0);
            const Halfedge h1 = mesh_.halfedge(e, 1);
            const Vertex va = mesh_.to_vertex(h0);
            const Vertex vb = mesh_.to_vertex(h1);
            const Vertex vc = mesh_.to_vertex(mesh_.next_halfedge(h0));
            const Vertex vd = mesh_.to_vertex(mesh_.next_halfedge(h1));

            if (vlocked_[va] || vlocked_[vb] || vlocked_[vc] ||
                vlocked_[vd])
                continue;

            if (!mesh_.is_flip_ok(e))
                continue;

            // Evaluate the candidate triangulation before touching
            // connectivity, so face-handle reuse inside flip() cannot
            // confuse the orientation test. The quad in cyclic order is
            // (va, vc, vb, vd); flipping replaces diagonal (va,vb) by
            // (vc,vd), giving triangles (vc,vb,vd) and (vd,va,vc).
            const Point& pa = points_[va];
            const Point& pb = points_[vb];
            const Point& pc = points_[vc];
            const Point& pd = points_[vd];

            const Point new_na = cross(pb - pc, pd - pc);
            const Point new_nb = cross(pa - pd, pc - pd);
            if (norm(new_na) == Scalar(0.0) || norm(new_nb) == Scalar(0.0))
                continue;

            // each new triangle must stay on the frozen oriented side of
            // the reference surface and must not become a near-degenerate
            // sliver (enforce the same cosine floor as vertex moves). The
            // anchor is the reference normal at the triangle's centroid.
            auto triangle_ok = [&](const Point& n_new,
                                   const Point& centroid) {
                const Normal n_ref = reference_normal_at(centroid);
                return dot(n_new, n_ref) >
                       MIN_FACE_COS * norm(n_new);
            };
            const Point ca = (pc + pb + pd) * Scalar(1.0 / 3.0);
            const Point cb = (pd + pa + pc) * Scalar(1.0 / 3.0);
            if (!triangle_ok(new_na, ca) || !triangle_ok(new_nb, cb))
                continue;

            // worst metric quality before and after the flip
            const Scalar quality_before =
                std::min(triangle_quality(va, vc, vb),
                         triangle_quality(vb, vd, va));
            const Scalar quality_after =
                std::min(triangle_quality(vc, vb, vd),
                         triangle_quality(vd, va, vc));

            // accept only a strict improvement of the worst quality
            if (quality_after > quality_before + strict_eps)
            {
                mesh_.flip(e);
                ok = false;
            }
        }
    }
}

void AnisotropicRemeshing::tangential_smoothing(unsigned int iterations)
{
    // Project free interior vertices first so that vertices introduced by
    // splitting carry valid normals and reference tensors. Every move is
    // checked against the frozen reference orientation.
    for (auto v : mesh_.vertices())
    {
        if (mesh_.is_boundary(v) || vlocked_[v] || vfeature_[v])
            continue;

        const Point old_position = points_[v];
        const Metric old_metric = vmetric_[v];
        transport_metric(v, true);

        if (!displacement_valid(v, points_[v]))
        {
            points_[v] = old_position;
            vmetric_[v] = old_metric;
            transport_metric(v, false);
        }
    }

    for (unsigned int iters = 0; iters < iterations; ++iters)
    {
        for (auto v : mesh_.vertices())
        {
            if (mesh_.is_boundary(v) || vlocked_[v])
                continue;

            Point update(0.0);

            if (vfeature_[v])
            {
                // smooth along the feature polyline only
                Point u(0.0), t(0.0);
                Scalar ww(0.0);
                int c = 0;

                for (auto h : mesh_.halfedges(v))
                {
                    if (!efeature_[mesh_.edge(h)])
                        continue;

                    const Vertex vv = mesh_.to_vertex(h);
                    const Point mid =
                        Scalar(0.5) * (points_[v] + points_[vv]);
                    const Scalar w = metric_length(v, vv);
                    ww += w;
                    u += w * mid;

                    if (c == 0)
                        t += normalize(points_[vv] - points_[v]);
                    else
                        t -= normalize(points_[vv] - points_[v]);
                    ++c;
                }

                if (c != 2 || ww == Scalar(0.0))
                    continue;

                u *= Scalar(1.0) / ww;
                u -= points_[v];
                t.normalize();
                update = t * dot(u, t);
            }
            else
            {
                // freeze the one-ring positions and tensors of this round
                struct Neighbor
                {
                    Point p;
                    Metric m;
                };
                std::vector<Neighbor> neighbors;
                neighbors.reserve(mesh_.valence(v));

                const Metric mv = vmetric_[v];
                for (auto h : mesh_.halfedges(v))
                {
                    const Vertex vv = mesh_.to_vertex(h);
                    neighbors.push_back(
                        {points_[vv], Scalar(0.5) * (mv + vmetric_[vv])});
                }

                // minimizer of sum_i (p - p_i)^T Mbar_i (p - p_i):
                // A p = b with A = sum_i Mbar_i, b = sum_i Mbar_i p_i
                Metric a(0.0);
                Point b(0.0);
                for (const auto& nb : neighbors)
                {
                    a += nb.m;
                    b += nb.m * nb.p;
                }

                Point target;
                try
                {
                    target = inverse(a) * b;
                }
                catch (SolverException&)
                {
                    Point mean(0.0);
                    for (const auto& nb : neighbors)
                        mean += nb.p;
                    if (!neighbors.empty())
                        mean *= Scalar(1.0) / neighbors.size();
                    target = mean;
                }

                Point u = target - points_[v];
                const Point n = vnormal_[v];
                u -= n * dot(u, n);
                update = u;
            }

            if (norm(update) == Scalar(0.0))
                continue;

            // shorten the step until every adjacent face stays oriented
            // relative to the frozen reference surface
            const Point old_position = points_[v];
            const Metric old_metric = vmetric_[v];
            bool accepted = false;
            Scalar step = Scalar(1.0);
            for (int s = 0; s < 6; ++s, step *= Scalar(0.5))
            {
                const Point candidate = old_position + step * update;
                if (displacement_valid(v, candidate))
                {
                    points_[v] = candidate;
                    accepted = true;
                    break;
                }
            }

            if (!accepted)
            {
                // no legal step: keep the original vertex
                points_[v] = old_position;
                continue;
            }

            if (vfeature_[v])
            {
                // feature vertices stay on their polyline
                transport_metric(v, false);
            }
            else
            {
                // project onto the reference surface and re-check the
                // orientation of the adjacent faces against the reference
                transport_metric(v, true);
                if (!displacement_valid(v, points_[v]))
                {
                    points_[v] = old_position;
                    vmetric_[v] = old_metric;
                    transport_metric(v, false);
                }
            }
        }

        vertex_normals(mesh_);
    }

    // final projection of free interior vertices, again guarded by the
    // reference orientation
    for (auto v : mesh_.vertices())
    {
        if (mesh_.is_boundary(v) || vlocked_[v] || vfeature_[v])
            continue;

        const Point old_position = points_[v];
        const Metric old_metric = vmetric_[v];
        transport_metric(v, true);

        if (!displacement_valid(v, points_[v]))
        {
            points_[v] = old_position;
            vmetric_[v] = old_metric;
            transport_metric(v, false);
        }
    }
}

} // namespace

void anisotropic_remeshing(SurfaceMesh& mesh, unsigned int iterations)
{
    AnisotropicRemeshing remeshing(mesh);
    remeshing.run(iterations);
}

} // namespace pmp
