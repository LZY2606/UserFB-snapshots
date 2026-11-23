// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/intrinsic_transfer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#include <Eigen/Dense>

#include "pmp/algorithms/numerics.h"

namespace pmp {

namespace {

using Vec2 = Eigen::Vector2d;

inline double cross2(const Vec2& a, const Vec2& b)
{
    return a[0] * b[1] - a[1] * b[0];
}

//! Affine map in the plane, determined by three point correspondences of a
//! non-degenerate source triangle.
struct AffineMap
{
    Eigen::Matrix2d linear;
    Vec2 shift;

    Vec2 apply(const Vec2& p) const { return linear * p + shift; }
};

AffineMap affine_from_triangle(const std::array<Vec2, 3>& from,
                               const std::array<Vec2, 3>& to)
{
    Eigen::Matrix2d s, d;
    s.col(0) = from[1] - from[0];
    s.col(1) = from[2] - from[0];
    d.col(0) = to[1] - to[0];
    d.col(1) = to[2] - to[0];
    AffineMap map;
    map.linear = d * s.inverse();
    map.shift = to[0] - map.linear * from[0];
    return map;
}

} // namespace

struct IntrinsicTransfer::Impl
{
    //! One vertex of a common-refinement piece: position in the local frame
    //! of the owning current face plus barycentric coordinates with respect
    //! to the source (initial) face the piece lies in.
    struct PieceVertex
    {
        Vec2 xy;
        Eigen::Vector3d lambda;
    };

    //! Connected component of the intersection of a current face with one
    //! source triangle; a convex, counter-clockwise polygon.
    struct Piece
    {
        int source_face = -1;
        std::vector<PieceVertex> verts;
    };

    explicit Impl(const SurfaceMesh& input);

    //! Local 2D frame of a face: coordinates of the to-vertices of its
    //! three halfedges, first edge on the positive x-axis, CCW orientation.
    std::array<Vec2, 3> face_frame(Face f) const;

    SurfaceMesh mesh;                  //!< current triangulation (deep copy)
    EdgeProperty<double> edge_length_; //!< intrinsic metric
    std::vector<std::array<int, 3>> source_faces_; //!< initial face corners
    std::vector<std::vector<Piece>> pieces_;       //!< common refinement
    size_t vertex_slots_ = 0;                      //!< initial vertices_size()
};

IntrinsicTransfer::Impl::Impl(const SurfaceMesh& input) : mesh(input)
{
    edge_length_ = mesh.add_edge_property<double>("e:intrinsic_length", 0.0);
    vertex_slots_ = mesh.vertices_size();

    // intrinsic metric from the 3D embedding of the input mesh
    for (auto e : mesh.edges())
    {
        const auto h = mesh.halfedge(e, 0);
        const double l =
            norm(mesh.position(mesh.to_vertex(h)) -
                 mesh.position(mesh.from_vertex(h)));
        if (!std::isfinite(l) || !(l > 0.0))
            throw std::invalid_argument(
                "IntrinsicTransfer: degenerate edge length");
        edge_length_[e] = l;
    }

    source_faces_.assign(mesh.faces_size(), {{-1, -1, -1}});
    pieces_.resize(mesh.faces_size());

    for (auto f : mesh.faces())
    {
        if (mesh.valence(f) != 3)
            throw std::invalid_argument(
                "IntrinsicTransfer: mesh has to be a triangle mesh");

        const Halfedge h0 = mesh.halfedge(f);
        const Halfedge h1 = mesh.next_halfedge(h0);
        const Halfedge h2 = mesh.next_halfedge(h1);
        const double l0 = edge_length_[mesh.edge(h0)];
        const double l1 = edge_length_[mesh.edge(h1)];
        const double l2 = edge_length_[mesh.edge(h2)];
        if (!(l0 + l1 > l2) || !(l1 + l2 > l0) || !(l2 + l0 > l1))
            throw std::invalid_argument(
                "IntrinsicTransfer: degenerate input triangle");

        source_faces_[f.idx()] = {{static_cast<int>(mesh.to_vertex(h0).idx()),
                                   static_cast<int>(mesh.to_vertex(h1).idx()),
                                   static_cast<int>(mesh.to_vertex(h2).idx())}};

        // initially the common refinement is the triangulation itself
        const auto frame = face_frame(f);
        Piece piece;
        piece.source_face = static_cast<int>(f.idx());
        for (int i = 0; i < 3; ++i)
        {
            PieceVertex pv;
            pv.xy = frame[i];
            pv.lambda.setZero();
            pv.lambda[i] = 1.0;
            piece.verts.push_back(pv);
        }
        pieces_[f.idx()].push_back(std::move(piece));
    }
}

std::array<Vec2, 3> IntrinsicTransfer::Impl::face_frame(Face f) const
{
    const Halfedge h0 = mesh.halfedge(f);
    const Halfedge h1 = mesh.next_halfedge(h0);
    const Halfedge h2 = mesh.next_halfedge(h1);
    const double l0 = edge_length_[mesh.edge(h0)]; // |x2 - x0|
    const double l1 = edge_length_[mesh.edge(h1)]; // |x0 - x1|
    const double l2 = edge_length_[mesh.edge(h2)]; // |x1 - x2|
    const double x = (l1 * l1 + l0 * l0 - l2 * l2) / (2.0 * l1);
    const double y = std::sqrt(std::max(0.0, l0 * l0 - x * x));
    return {Vec2(0.0, 0.0), Vec2(l1, 0.0), Vec2(x, y)};
}

IntrinsicTransfer::IntrinsicTransfer(const SurfaceMesh& mesh)
    : impl_(std::make_unique<Impl>(mesh))
{
}

IntrinsicTransfer::~IntrinsicTransfer() = default;
IntrinsicTransfer::IntrinsicTransfer(IntrinsicTransfer&&) noexcept = default;
IntrinsicTransfer& IntrinsicTransfer::operator=(IntrinsicTransfer&&) noexcept =
    default;

const SurfaceMesh& IntrinsicTransfer::mesh() const
{
    return impl_->mesh;
}

double IntrinsicTransfer::edge_length(Edge e) const
{
    const auto& im = *impl_;
    if (!e.is_valid() || !im.mesh.is_valid(e) || im.mesh.is_deleted(e))
        return 0.0;
    return im.edge_length_[e];
}

bool IntrinsicTransfer::flip(Edge e)
{
    auto& im = *impl_;
    auto& m = im.mesh;

    if (!e.is_valid() || !m.is_valid(e) || m.is_deleted(e))
        return false;
    if (m.is_boundary(e))
        return false;
    if (!m.is_flip_ok(e)) // also covers an already existing diagonal
        return false;

    const Halfedge a0 = m.halfedge(e, 0);
    const Halfedge b0 = m.halfedge(e, 1);
    const Halfedge a1 = m.next_halfedge(a0);
    const Halfedge a2 = m.next_halfedge(a1);
    const Halfedge b1 = m.next_halfedge(b0);
    const Halfedge b2 = m.next_halfedge(b1);
    const Face fa = m.face(a0);
    const Face fb = m.face(b0);

    const Vertex va0 = m.to_vertex(a0);
    const Vertex va1 = m.to_vertex(a1);
    const Vertex vb0 = m.to_vertex(b0);
    const Vertex vb1 = m.to_vertex(b1);

    const double le = im.edge_length_[e];
    const double la1 = im.edge_length_[m.edge(a1)];
    const double la2 = im.edge_length_[m.edge(a2)];
    const double lb1 = im.edge_length_[m.edge(b1)];
    const double lb2 = im.edge_length_[m.edge(b2)];

    // unfold the two incident triangles into the plane; the shared edge
    // maps to the segment (0,0)-(le,0) with the two apices on opposite sides
    const Vec2 A(0.0, 0.0); // va0
    const Vec2 B(le, 0.0);  // vb0
    double x = (le * le + la1 * la1 - la2 * la2) / (2.0 * le);
    double y2 = la1 * la1 - x * x;
    if (!(y2 > 0.0))
        return false;
    const Vec2 C(x, std::sqrt(y2)); // va1
    x = (le * le + lb2 * lb2 - lb1 * lb1) / (2.0 * le);
    y2 = lb2 * lb2 - x * x;
    if (!(y2 > 0.0))
        return false;
    const Vec2 D(x, -std::sqrt(y2)); // vb1

    // require a non-degenerate strictly convex quadrilateral A, C, B, D
    const std::array<Vec2, 4> quad{A, C, B, D};
    const double tol = 1e-13 * le * le;
    double orientation = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        const Vec2 e1 = quad[(i + 1) & 3] - quad[i];
        const Vec2 e2 = quad[(i + 2) & 3] - quad[(i + 1) & 3];
        const double turn = cross2(e1, e2);
        if (std::abs(turn) <= tol)
            return false;
        const double s = turn > 0.0 ? 1.0 : -1.0;
        if (orientation == 0.0)
            orientation = s;
        else if (s != orientation)
            return false;
    }

    const double new_length = (C - D).norm();
    if (!(new_length > 0.0))
        return false;

    // maps from the two face frames into the unfolded quadrilateral
    auto unfolded_position = [&](Vertex v) {
        if (v == va0)
            return A;
        if (v == vb0)
            return B;
        if (v == va1)
            return C;
        return D; // vb1
    };
    auto frame_vertices = [&](Face f, std::array<Vertex, 3>& verts) {
        const Halfedge h0 = m.halfedge(f);
        const Halfedge h1 = m.next_halfedge(h0);
        const Halfedge h2 = m.next_halfedge(h1);
        verts = {{m.to_vertex(h0), m.to_vertex(h1), m.to_vertex(h2)}};
    };
    std::array<Vertex, 3> verts;
    frame_vertices(fa, verts);
    const std::array<Vec2, 3> unf_a{unfolded_position(verts[0]),
                                    unfolded_position(verts[1]),
                                    unfolded_position(verts[2])};
    const AffineMap to_unfolded_a =
        affine_from_triangle(im.face_frame(fa), unf_a);
    frame_vertices(fb, verts);
    const std::array<Vec2, 3> unf_b{unfolded_position(verts[0]),
                                    unfolded_position(verts[1]),
                                    unfolded_position(verts[2])};
    const AffineMap to_unfolded_b =
        affine_from_triangle(im.face_frame(fb), unf_b);

    // gather the common-refinement pieces of both faces in unfolded
    // coordinates (nothing is committed yet)
    std::vector<Impl::Piece> moved;
    for (const Face f : {fa, fb})
    {
        const AffineMap& to_unfolded =
            (f == fa) ? to_unfolded_a : to_unfolded_b;
        for (const auto& piece : im.pieces_[f.idx()])
        {
            Impl::Piece q;
            q.source_face = piece.source_face;
            q.verts.reserve(piece.verts.size());
            for (const auto& pv : piece.verts)
                q.verts.push_back({to_unfolded.apply(pv.xy), pv.lambda});
            moved.push_back(std::move(q));
        }
    }

    // commit the topological flip and the intrinsic metric
    m.flip(e);
    im.edge_length_[e] = new_length;

    // maps from unfolded coordinates into the new face frames
    frame_vertices(fa, verts);
    const std::array<Vec2, 3> loc_a{unfolded_position(verts[0]),
                                    unfolded_position(verts[1]),
                                    unfolded_position(verts[2])};
    const AffineMap from_unfolded_a =
        affine_from_triangle(loc_a, im.face_frame(fa));
    frame_vertices(fb, verts);
    const std::array<Vec2, 3> loc_b{unfolded_position(verts[0]),
                                    unfolded_position(verts[1]),
                                    unfolded_position(verts[2])};
    const AffineMap from_unfolded_b =
        affine_from_triangle(loc_b, im.face_frame(fb));

    // clip each piece against the new diagonal and assign it to the face on
    // its side; barycentric source coordinates interpolate linearly
    const Vec2 diagonal = D - C;
    const double side_b = cross2(diagonal, B - C); // nonzero by convexity
    std::vector<Impl::Piece> pieces_a, pieces_b;
    for (const auto& piece : moved)
    {
        for (int side = 0; side < 2; ++side)
        {
            const double keep = side == 0 ? side_b : -side_b;
            std::vector<Impl::PieceVertex> poly;
            const auto& in = piece.verts;
            const size_t n = in.size();
            for (size_t i = 0; i < n; ++i)
            {
                const auto& s = in[i];
                const auto& g = in[(i + 1) % n];
                const double ds = cross2(diagonal, s.xy - C) * keep;
                const double dg = cross2(diagonal, g.xy - C) * keep;
                if (dg >= 0.0)
                {
                    if (ds < 0.0)
                    {
                        const double t = ds / (ds - dg);
                        poly.push_back({s.xy + t * (g.xy - s.xy),
                                        s.lambda + t * (g.lambda - s.lambda)});
                    }
                    poly.push_back(g);
                }
                else if (ds >= 0.0)
                {
                    const double t = ds / (ds - dg);
                    poly.push_back({s.xy + t * (g.xy - s.xy),
                                    s.lambda + t * (g.lambda - s.lambda)});
                }
            }
            if (poly.size() < 3)
                continue;

            const AffineMap& from_unfolded =
                side == 0 ? from_unfolded_a : from_unfolded_b;
            Impl::Piece q;
            q.source_face = piece.source_face;
            q.verts.reserve(poly.size());
            for (const auto& pv : poly)
                q.verts.push_back({from_unfolded.apply(pv.xy), pv.lambda});

            double area = 0.0;
            for (size_t i = 0; i < q.verts.size(); ++i)
                area += cross2(q.verts[i].xy,
                               q.verts[(i + 1) % q.verts.size()].xy);
            if (area < 0.0)
            {
                std::reverse(q.verts.begin(), q.verts.end());
                area = -area;
            }
            if (!(area > 1e-30))
                continue;

            (side == 0 ? pieces_a : pieces_b).push_back(std::move(q));
        }
    }
    im.pieces_[fa.idx()] = std::move(pieces_a);
    im.pieces_[fb.idx()] = std::move(pieces_b);
    return true;
}

std::vector<double>
IntrinsicTransfer::project(const std::vector<double>& source_values) const
{
    const auto& im = *impl_;
    const auto& m = im.mesh;

    if (source_values.size() != im.vertex_slots_)
        throw std::invalid_argument(
            "IntrinsicTransfer::project: wrong number of source values");
    for (auto v : m.vertices())
        if (!std::isfinite(source_values[v.idx()]))
            throw std::invalid_argument(
                "IntrinsicTransfer::project: non-finite source value");

    // compressed indices for live vertices incident to a face
    std::vector<int> index(im.vertex_slots_, -1);
    int n = 0;
    for (auto f : m.faces())
    {
        Halfedge h = m.halfedge(f);
        for (int i = 0; i < 3; ++i)
        {
            const int vi = static_cast<int>(m.to_vertex(h).idx());
            if (index[vi] < 0)
                index[vi] = n++;
            h = m.next_halfedge(h);
        }
    }

    std::vector<double> result(im.vertex_slots_, 0.0);
    if (n == 0)
    {
        for (auto v : m.vertices())
            result[v.idx()] = source_values[v.idx()];
        return result;
    }

    std::vector<Triplet> triplets;
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(n);

    for (auto f : m.faces())
    {
        const auto frame = im.face_frame(f);
        const double area =
            0.5 * cross2(frame[1] - frame[0], frame[2] - frame[0]);

        Halfedge h = m.halfedge(f);
        int ids[3];
        for (int i = 0; i < 3; ++i)
        {
            ids[i] = index[m.to_vertex(h).idx()];
            h = m.next_halfedge(h);
        }

        // consistent mass matrix of the intrinsic triangle
        const double w = area / 12.0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                triplets.emplace_back(ids[i], ids[j],
                                      w * (i == j ? 2.0 : 1.0));

        // integrate the source field against the current hat functions on
        // each piece of the common refinement; the integrand is a quadratic
        // polynomial and is integrated exactly
        for (const auto& piece : im.pieces_[f.idx()])
        {
            const auto& corners = im.source_faces_[piece.source_face];
            const auto& poly = piece.verts;
            for (size_t k = 1; k + 1 < poly.size(); ++k)
            {
                const Impl::PieceVertex* tri[3] = {&poly[0], &poly[k],
                                                   &poly[k + 1]};
                const double at =
                    0.5 * cross2(tri[1]->xy - tri[0]->xy,
                                 tri[2]->xy - tri[0]->xy);
                if (!(at > 0.0))
                    continue;

                double value[3];  // source field at the corners
                double hat[3][3]; // current hat functions at the corners
                for (int j = 0; j < 3; ++j)
                {
                    value[j] = 0.0;
                    for (int c = 0; c < 3; ++c)
                        value[j] += tri[j]->lambda[c] *
                                    source_values[corners[c]];
                    const double w2 = tri[j]->xy[1] / frame[2][1];
                    const double w1 =
                        (tri[j]->xy[0] - w2 * frame[2][0]) / frame[1][0];
                    hat[j][0] = 1.0 - w1 - w2;
                    hat[j][1] = w1;
                    hat[j][2] = w2;
                }
                const double value_sum = value[0] + value[1] + value[2];
                for (int i = 0; i < 3; ++i)
                {
                    double dot = 0.0, sum = 0.0;
                    for (int j = 0; j < 3; ++j)
                    {
                        dot += hat[j][i] * value[j];
                        sum += hat[j][i];
                    }
                    rhs[ids[i]] += at / 12.0 * (dot + sum * value_sum);
                }
            }
        }
    }

    SparseMatrix mass(n, n);
    mass.setFromTriplets(triplets.begin(), triplets.end());
    const DenseMatrix u = cholesky_solve(mass, DenseMatrix(rhs));

    for (auto v : m.vertices())
    {
        const int id = index[v.idx()];
        // isolated live vertices keep their input value
        result[v.idx()] = id >= 0 ? u(id, 0) : source_values[v.idx()];
    }
    return result;
}

} // namespace pmp
