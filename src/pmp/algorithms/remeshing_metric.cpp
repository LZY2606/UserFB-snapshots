// Copyright 2011-2020 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/remeshing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "pmp/algorithms/barycentric_coordinates.h"
#include "pmp/algorithms/distance_point_triangle.h"
#include "pmp/algorithms/normals.h"
#include "pmp/bounding_box.h"

namespace pmp {
namespace {

using Mat3 = Matrix<Scalar, 3, 3>;

constexpr Scalar split_factor = Scalar(4.0 / 3.0);
constexpr Scalar collapse_factor = Scalar(4.0 / 5.0);
constexpr unsigned int max_passes = 10;
constexpr int no_segment = -1;

bool is_finite(const Mat3& m)
{
    for (int i = 0; i < 9; ++i)
        if (!std::isfinite(m[i]))
            return false;
    return true;
}

//! Check that \p m is finite, symmetric and positive definite.
//! Symmetry is tested against a relative tolerance, positive
//! definiteness through an LDL^T factorization of the symmetric part.
bool is_valid_metric(const Mat3& m)
{
    if (!is_finite(m))
        return false;

    Scalar scale(0);
    for (int i = 0; i < 9; ++i)
        scale = std::max(scale, std::fabs(m[i]));
    scale = std::max(scale, Scalar(1));

    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j)
            if (std::fabs(m(i, j) - m(j, i)) > Scalar(1e-10) * scale)
                return false;

    Scalar L[3][3] = {};
    Scalar D[3] = {};
    for (int j = 0; j < 3; ++j)
    {
        Scalar d = m(j, j);
        for (int k = 0; k < j; ++k)
            d -= L[j][k] * L[j][k] * D[k];
        if (!(d > Scalar(1e-12) * scale))
            return false;
        D[j] = d;
        L[j][j] = 1.0;
        for (int i = j + 1; i < 3; ++i)
        {
            Scalar s = m(i, j);
            for (int k = 0; k < j; ++k)
                s -= L[i][k] * L[j][k] * D[k];
            L[i][j] = s / d;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------

//! Static kd-tree over the triangles of the frozen reference surface.
class ReferenceKdTree
{
public:
    struct Nearest
    {
        Scalar dist{std::numeric_limits<Scalar>::max()};
        unsigned int face{0};
        Point point;
    };

    explicit ReferenceKdTree(const std::vector<Point>& tri_points)
    {
        root_ = new Node();
        face_points_ = tri_points;
        root_->faces.reserve(tri_points.size() / 3);
        for (unsigned int f = 0; f < tri_points.size() / 3; ++f)
            root_->faces.push_back(f);
        build_recurse(root_, 30);
    }

    ~ReferenceKdTree() { delete root_; }

    Nearest nearest(const Point& p) const
    {
        Nearest data;
        nearest_recurse(root_, p, data);
        return data;
    }

private:
    struct Node
    {
        ~Node()
        {
            delete left_child;
            delete right_child;
        }

        unsigned char axis{0};
        Scalar split{0};
        std::vector<unsigned int> faces;
        Node* left_child{nullptr};
        Node* right_child{nullptr};
    };

    void build_recurse(Node* node, unsigned int depth)
    {
        if (depth == 0 || node->faces.size() <= 10)
            return;

        BoundingBox bbox;
        for (unsigned int f : node->faces)
        {
            bbox += face_points_[3 * f];
            bbox += face_points_[3 * f + 1];
            bbox += face_points_[3 * f + 2];
        }

        const Point bb = bbox.max() - bbox.min();
        Scalar length = bb[0];
        int axis = 0;
        if (bb[1] > length)
            length = bb[(axis = 1)];
        if (bb[2] > length)
            length = bb[(axis = 2)];

        const Scalar split = bbox.center()[axis];
        auto* left = new Node();
        auto* right = new Node();

        for (unsigned int f : node->faces)
        {
            bool l = false, r = false;
            for (int k = 0; k < 3; ++k)
            {
                if (face_points_[3 * f + k][axis] <= split)
                    l = true;
                else
                    r = true;
            }
            if (l)
                left->faces.push_back(f);
            if (r)
                right->faces.push_back(f);
        }

        if (left->faces.size() == node->faces.size() ||
            right->faces.size() == node->faces.size())
        {
            node->faces.shrink_to_fit();
            delete left;
            delete right;
            return;
        }

        node->axis = static_cast<unsigned char>(axis);
        node->split = split;
        node->left_child = left;
        node->right_child = right;
        build_recurse(left, depth - 1);
        build_recurse(right, depth - 1);
    }

    void nearest_recurse(const Node* node, const Point& point,
                         Nearest& data) const
    {
        if (!node->left_child)
        {
            // faces are stored and visited in increasing index order; on
            // an exact distance tie the smallest initial face index wins
            for (unsigned int f : node->faces)
            {
                Point nearest;
                const Scalar d =
                    dist_point_triangle(point, face_points_[3 * f],
                                        face_points_[3 * f + 1],
                                        face_points_[3 * f + 2], nearest);
                if (d < data.dist ||
                    (d == data.dist && f < data.face))
                {
                    data.dist = d;
                    data.face = f;
                    data.point = nearest;
                }
            }
            return;
        }

        const Scalar dist = point[node->axis] - node->split;
        if (dist <= 0.0)
        {
            nearest_recurse(node->left_child, point, data);
            if (std::fabs(dist) < data.dist)
                nearest_recurse(node->right_child, point, data);
        }
        else
        {
            nearest_recurse(node->right_child, point, data);
            if (std::fabs(dist) < data.dist)
                nearest_recurse(node->left_child, point, data);
        }
    }

    Node* root_{nullptr};
    std::vector<Point> face_points_;
};

// ---------------------------------------------------------------------------

//! Tensor-driven incremental remeshing. All geometric operations are
//! governed by the per-vertex symmetric positive-definite metric tensors
//! stored in "v:metric", measured in world coordinates.
class AnisotropicRemeshing
{
public:
    explicit AnisotropicRemeshing(SurfaceMesh& mesh) : mesh_(mesh)
    {
        if (!mesh_.is_triangle_mesh())
            throw InvalidInputException("Input is not a triangle mesh!");
    }

    void run(unsigned int iterations)
    {
        // Validate everything before touching the mesh. A zero-iteration
        // call therefore only performs this read-only check.
        validate_input();
        if (iterations == 0)
            return;

        freeze_reference();
        setup_properties();

        for (unsigned int i = 0; i < iterations; ++i)
        {
            split_long_edges();
            vertex_normals(mesh_);
            collapse_short_edges();
            flip_edges();
            tangential_smoothing(5);
        }

        teardown_properties();
    }

private:
    struct RefTriangle
    {
        Point p[3];
        Mat3 m[3];
    };

    struct RefSegment
    {
        Point a, b;
        Mat3 ma, mb;
    };

    // ----- validation -----------------------------------------------------

    void validate_input()
    {
        if (!mesh_.has_vertex_property("v:metric"))
            throw InvalidInputException(
                "Anisotropic remeshing requires vertex property "
                "\"v:metric\".");

        auto metric = mesh_.get_vertex_property<Mat3>("v:metric");
        if (!metric)
            throw InvalidInputException(
                "Vertex property \"v:metric\" has the wrong type "
                "(expected Matrix<Scalar,3,3>).");

        auto points = mesh_.get_vertex_property<Point>("v:point");
        for (auto v : mesh_.vertices())
        {
            for (int k = 0; k < 3; ++k)
                if (!std::isfinite(points[v][k]))
                    throw InvalidInputException(
                        "Anisotropic remeshing requires finite vertex "
                        "coordinates.");
            if (!is_valid_metric(metric[v]))
                throw InvalidInputException(
                    "Vertex property \"v:metric\" must contain finite, "
                    "symmetric and positive-definite matrices.");
        }
    }

    // ----- reference field -----------------------------------------------

    void freeze_reference()
    {
        metric_ = mesh_.get_vertex_property<Mat3>("v:metric");
        points_ = mesh_.get_vertex_property<Point>("v:point");

        // The reference surface and the tensor field are frozen once, in
        // the initial face ordering, and never updated from the evolving
        // working mesh.
        ref_triangles_.reserve(mesh_.n_faces());
        std::vector<Point> tree_points;
        tree_points.reserve(3 * mesh_.n_faces());
        BoundingBox bbox;

        for (auto f : mesh_.faces())
        {
            RefTriangle t;
            int k = 0;
            for (auto v : mesh_.vertices(f))
            {
                t.p[k] = points_[v];
                t.m[k] = metric_[v];
                tree_points.push_back(points_[v]);
                bbox += points_[v];
                ++k;
            }
            ref_triangles_.push_back(t);
        }

        kd_tree_ = std::make_unique<ReferenceKdTree>(tree_points);
        area_tolerance_ =
            Scalar(1e-18) * dot(bbox.max() - bbox.min(),
                                bbox.max() - bbox.min());

        freeze_polylines();
    }

    void freeze_polylines()
    {
        ref_segments_.clear();
        const bool has_feature_edges =
            mesh_.has_edge_property("e:feature");
        auto efeature =
            has_feature_edges
                ? mesh_.get_edge_property<bool>("e:feature")
                : EdgeProperty<bool>();

        erefseg_ = mesh_.add_edge_property<int>("e:refseg", no_segment);

        for (auto e : mesh_.edges())
        {
            if (!mesh_.is_boundary(e) &&
                !(efeature && efeature[e]))
                continue;

            const Vertex v0 = mesh_.vertex(e, 0);
            const Vertex v1 = mesh_.vertex(e, 1);
            const int idx = static_cast<int>(ref_segments_.size());
            ref_segments_.push_back(
                {points_[v0], points_[v1], metric_[v0], metric_[v1]});
            erefseg_[e] = idx;
        }
    }

    // ----- working properties --------------------------------------------

    void setup_properties()
    {
        has_feature_vertices_ = mesh_.has_vertex_property("v:feature");
        has_feature_edges_ = mesh_.has_edge_property("e:feature");
        vfeature_ = mesh_.vertex_property<bool>("v:feature", false);
        efeature_ = mesh_.edge_property<bool>("e:feature", false);
        vlocked_ = mesh_.add_vertex_property<bool>("v:locked", false);
        elocked_ = mesh_.add_edge_property<bool>("e:locked", false);
        vrefseg_ =
            mesh_.add_vertex_property<int>("v:refseg", no_segment);

        // lock unselected vertices if a non-empty selection exists
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
                    elocked_[e] =
                        vlocked_[mesh_.vertex(e, 0)] ||
                        vlocked_[mesh_.vertex(e, 1)];
            }
        }

        // lock feature corners: a feature vertex without exactly two
        // incident feature edges must not move or be removed
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

        // lock boundary polyline corners: boundary vertices where the two
        // boundary edges form a bend must not move or be removed
        for (auto v : mesh_.vertices())
        {
            if (!mesh_.is_boundary(v) || vlocked_[v])
                continue;

            Halfedge h_out = mesh_.halfedge(v);
            while (!mesh_.is_boundary(h_out))
                h_out = mesh_.cw_rotated_halfedge(h_out);
            const Halfedge h_in = mesh_.prev_halfedge(h_out);

            const Vertex vprev = mesh_.from_vertex(h_in);
            const Vertex vnext = mesh_.to_vertex(h_out);
            const Point d_in =
                normalize(points_[v] - points_[vprev]);
            const Point d_out =
                normalize(points_[vnext] - points_[v]);

            // bend of more than 30 degrees is a corner
            if (dot(d_in, d_out) < Scalar(0.8660254037844386))
                vlocked_[v] = true;
        }

        // every polyline vertex remembers its home reference segment,
        // which guarantees that smoothing can never cross a corner
        for (auto v : mesh_.vertices())
        {
            for (auto h : mesh_.halfedges(v))
            {
                const int seg = erefseg_[mesh_.edge(h)];
                if (seg != no_segment)
                {
                    vrefseg_[v] = seg;
                    break;
                }
            }
        }

        vertex_normals(mesh_);
        vnormal_ = mesh_.get_vertex_property<Point>("v:normal");
    }

    void teardown_properties()
    {
        mesh_.remove_vertex_property(vrefseg_);
        mesh_.remove_edge_property(erefseg_);
        mesh_.remove_vertex_property(vlocked_);
        mesh_.remove_edge_property(elocked_);
        if (!has_feature_vertices_)
            mesh_.remove_vertex_property(vfeature_);
        if (!has_feature_edges_)
            mesh_.remove_edge_property(efeature_);
    }

    // ----- metric utilities ----------------------------------------------

    Scalar metric_length(Vertex v0, Vertex v1) const
    {
        const Point d = points_[v1] - points_[v0];
        const Mat3 m = (metric_[v0] + metric_[v1]) * Scalar(0.5);
        return std::sqrt(std::max<Scalar>(0, dot(d, m * d)));
    }

    bool is_too_long(Vertex v0, Vertex v1) const
    {
        return metric_length(v0, v1) > split_factor;
    }

    bool is_too_short(Vertex v0, Vertex v1) const
    {
        return metric_length(v0, v1) < collapse_factor;
    }

    //! Metric triangle quality: 4*sqrt(3) times the Gram area divided by
    //! the sum of the three squared metric edge lengths, using the mean
    //! tensor of the three vertices. In [0,1], 1 for an equilateral
    //! triangle in metric space.
    Scalar triangle_quality(Face f) const
    {
        Vertex v[3];
        int k = 0;
        for (auto fv : mesh_.vertices(f))
            v[k++] = fv;

        const Mat3 mb =
            (metric_[v[0]] + metric_[v[1]] + metric_[v[2]]) /
            Scalar(3.0);

        const Point u = points_[v[1]] - points_[v[0]];
        const Point w = points_[v[2]] - points_[v[0]];
        const Scalar gu = dot(u, mb * u);
        const Scalar gw = dot(w, mb * w);
        const Scalar guw = dot(u, mb * w);
        const Scalar gram = std::max<Scalar>(0, gu * gw - guw * guw);
        const Scalar area = Scalar(0.5) * std::sqrt(gram);

        Scalar sum(0);
        for (int i = 0; i < 3; ++i)
        {
            const Point e =
                points_[v[(i + 1) % 3]] - points_[v[i]];
            sum += dot(e, mb * e);
        }
        if (!(sum > 0))
            return 0;

        return Scalar(4.0 * std::sqrt(3.0)) * area / sum;
    }

    // ----- projection onto the frozen reference --------------------------

    //! Project \p v onto the frozen reference surface and interpolate its
    //! tensor from the containing reference triangle.
    void project_to_reference(Vertex v)
    {
        const auto nn = kd_tree_->nearest(points_[v]);
        const RefTriangle& t = ref_triangles_[nn.face];
        const Point b = barycentric_coordinates(nn.point, t.p[0], t.p[1],
                                                t.p[2]);
        Mat3 m(0);
        for (int k = 0; k < 3; ++k)
            m += t.m[k] * b[k];

        points_[v] = nn.point;
        metric_[v] = m;
    }

    //! Project onto one specific frozen polyline segment and linearly
    //! interpolate its tensor.
    void project_to_segment(Vertex v, int seg)
    {
        const RefSegment& s = ref_segments_[seg];
        const Point d = s.b - s.a;
        Scalar lambda =
            dot(points_[v] - s.a, d) /
            std::max(dot(d, d), std::numeric_limits<Scalar>::min());
        lambda = std::clamp(lambda, Scalar(0), Scalar(1));

        points_[v] = s.a + d * lambda;
        metric_[v] = s.ma * (1.0 - lambda) + s.mb * lambda;
    }

    //! Project a polyline vertex onto its home reference segment. The
    //! home segment never spans a corner, so the vertex cannot cross one.
    void project_to_polyline(Vertex v)
    {
        int seg = vrefseg_[v];
        if (seg == no_segment)
        {
            // fallback: nearest segment of the frozen polylines
            Scalar best = std::numeric_limits<Scalar>::max();
            for (int i = 0; i < static_cast<int>(ref_segments_.size());
                 ++i)
            {
                const RefSegment& s = ref_segments_[i];
                const Point d = s.b - s.a;
                Scalar lambda = dot(points_[v] - s.a, d) /
                                std::max(dot(d, d),
                                         std::numeric_limits<Scalar>::min());
                lambda = std::clamp(lambda, Scalar(0), Scalar(1));
                const Scalar dist =
                    dot(points_[v] - (s.a + d * lambda),
                        points_[v] - (s.a + d * lambda));
                if (dist < best)
                {
                    best = dist;
                    seg = i;
                }
            }
            if (seg == no_segment)
                return;
            vrefseg_[v] = seg;
        }
        project_to_segment(v, seg);
    }

    // ----- geometric safety checks ---------------------------------------

    //! Check that moving \p v to its current candidate position creates
    //! neither degenerate nor flipped incident triangles.
    bool move_is_valid(Vertex v) const
    {
        for (auto h : mesh_.halfedges(v))
        {
            if (mesh_.is_boundary(h))
                continue;
            const Face f = mesh_.face(h);
            Point n(0);
            int k = 0;
            Point p[3];
            for (auto fv : mesh_.vertices(f))
                p[k++] = points_[fv];

            n = cross(p[1] - p[0], p[2] - p[0]);
            if (dot(n, n) <= area_tolerance_)
                return false;
        }
        return true;
    }

    //! Orientation of the incident triangles must not flip relative to
    //! \p old_normal, which is recorded before the candidate move.
    bool orientation_preserved(Vertex v, const Point& old_normal) const
    {
        for (auto h : mesh_.halfedges(v))
        {
            if (mesh_.is_boundary(h))
                continue;
            const Face f = mesh_.face(h);
            Point p[3];
            int k = 0;
            for (auto fv : mesh_.vertices(f))
                p[k++] = points_[fv];

            const Point n = cross(p[1] - p[0], p[2] - p[0]);
            if (dot(n, n) <= area_tolerance_)
                return false;
            if (dot(old_normal, n) <= 0)
                return false;
        }
        return true;
    }

    //! Check the triangles surviving a collapse of \p v1 into \p v0.
    bool collapse_is_valid(Vertex v0, Vertex v1) const
    {
        const Halfedge h10 = mesh_.find_halfedge(v1, v0);
        const Face removed0 = mesh_.face(h10);
        const Face removed1 =
            mesh_.face(mesh_.opposite_halfedge(h10));

        for (auto h : mesh_.halfedges(v1))
        {
            if (mesh_.is_boundary(h))
                continue;
            const Face f = mesh_.face(h);
            if (f == removed0 || f == removed1)
                continue;

            Point old[3], cand[3];
            int k = 0;
            for (auto fv : mesh_.vertices(f))
            {
                old[k] = points_[fv];
                cand[k] = (fv == v1) ? points_[v0] : points_[fv];
                ++k;
            }

            const Point old_n =
                cross(old[1] - old[0], old[2] - old[0]);
            const Point new_n =
                cross(cand[1] - cand[0], cand[2] - cand[0]);

            if (dot(new_n, new_n) <= area_tolerance_)
                return false;
            if (dot(old_n, new_n) <= 0)
                return false;
        }
        return true;
    }

    // ----- split ----------------------------------------------------------

    void split_long_edges()
    {
        for (unsigned int pass = 0; pass < max_passes; ++pass)
        {
            bool ok = true;

            for (auto e : mesh_.edges())
            {
                if (elocked_[e])
                    continue;

                const Vertex v0 = mesh_.vertex(e, 0);
                const Vertex v1 = mesh_.vertex(e, 1);
                if (!is_too_long(v0, v1))
                    continue;

                const bool is_feature = efeature_[e];
                const int home_seg = erefseg_[e];
                const Vertex vfar =
                    mesh_.to_vertex(mesh_.halfedge(e, 1));

                // geometric midpoint first, then update from the frozen
                // reference field. The midpoint is guaranteed to keep
                // every incident triangle non-degenerate and is used as
                // the positional fallback if a projection would not.
                const Point midpoint =
                    (points_[v0] + points_[v1]) * Scalar(0.5);

                // reference orientation from the triangles about to be
                // split; the child triangles must keep this orientation
                Point reference_normal(0);
                for (auto h : mesh_.halfedges(v0))
                {
                    if (mesh_.is_boundary(h) ||
                        mesh_.edge(h) != e)
                        continue;
                    const Face f = mesh_.face(h);
                    reference_normal += face_normal(f);
                }
                if (dot(reference_normal, reference_normal) <= 0)
                    reference_normal = vnormal_[v0];
                reference_normal = normalize(reference_normal);

                const Vertex vnew = mesh_.add_vertex(midpoint);
                mesh_.split(e, vnew);

                if (is_feature)
                {
                    // the original edge e stays feature and is shortened
                    // to v0-vnew; the new edge vnew-vfar is its twin
                    const Edge enew = mesh_.find_edge(vnew, vfar);
                    if (mesh_.is_valid(enew))
                        efeature_[enew] = true;
                    vfeature_[vnew] = true;
                }

                if (home_seg != no_segment)
                {
                    // split on a boundary/feature polyline: both child
                    // edges and the new vertex stay on the same segment
                    const Edge e0 = mesh_.find_edge(vnew, v0);
                    const Edge e1 = mesh_.find_edge(vnew, v1);
                    if (mesh_.is_valid(e0))
                        erefseg_[e0] = home_seg;
                    if (mesh_.is_valid(e1))
                        erefseg_[e1] = home_seg;
                    vrefseg_[vnew] = home_seg;
                    project_to_segment(vnew, home_seg);
                }
                else
                {
                    project_to_reference(vnew);
                }

                if (!move_is_valid(vnew) ||
                    !orientation_preserved(vnew, reference_normal))
                {
                    // keep the tensor interpolated from the frozen
                    // reference field, only roll the position back
                    points_[vnew] = midpoint;
                }

                vnormal_[vnew] = vertex_normal(mesh_, vnew);
                ok = false;
            }

            if (ok)
                break;
        }
    }

    // ----- collapse -------------------------------------------------------

    void collapse_short_edges()
    {
        for (unsigned int pass = 0; pass < max_passes; ++pass)
        {
            bool ok = true;

            for (auto e : mesh_.edges())
            {
                if (mesh_.is_deleted(e) || elocked_[e])
                    continue;

                const Halfedge h10 = mesh_.halfedge(e, 0);
                const Halfedge h01 = mesh_.halfedge(e, 1);
                const Vertex v0 = mesh_.to_vertex(h10);
                const Vertex v1 = mesh_.to_vertex(h01);

                if (!is_too_short(v0, v1))
                    continue;

                const bool b0 = mesh_.is_boundary(v0);
                const bool b1 = mesh_.is_boundary(v1);
                const bool l0 = vlocked_[v0];
                const bool l1 = vlocked_[v1];
                const bool f0 = vfeature_[v0];
                const bool f1 = vfeature_[v1];
                bool hcol01 = true, hcol10 = true;

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

                // feature rules: never collapse across a feature corner
                if (f0 && f1)
                {
                    if (!efeature_[e])
                        continue;

                    Halfedge h = mesh_.prev_halfedge(h01);
                    Halfedge hh = mesh_.next_halfedge(h10);
                    if (efeature_[mesh_.edge(h)] ||
                        efeature_[mesh_.edge(hh)])
                        hcol01 = false;
                    h = mesh_.prev_halfedge(h10);
                    hh = mesh_.next_halfedge(h01);
                    if (efeature_[mesh_.edge(h)] ||
                        efeature_[mesh_.edge(hh)])
                        hcol10 = false;
                }
                else if (f0)
                    hcol01 = false;
                else if (f1)
                    hcol10 = false;

                const bool collapse_ok = mesh_.is_collapse_ok(h01);
                if (hcol01)
                    hcol01 = collapse_ok;
                if (hcol10)
                    hcol10 = collapse_ok;

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
                    for (auto vv : mesh_.vertices(v1))
                    {
                        if (is_too_long(v0, vv))
                        {
                            hcol10 = false;
                            break;
                        }
                    }
                    if (hcol10 && !collapse_is_valid(v0, v1))
                        hcol10 = false;

                    if (hcol10)
                    {
                        mesh_.collapse(h10);
                        ok = false;
                    }
                }
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
                    if (hcol01 && !collapse_is_valid(v1, v0))
                        hcol01 = false;

                    if (hcol01)
                    {
                        mesh_.collapse(h01);
                        ok = false;
                    }
                }
            }

            if (ok)
                break;
        }

        mesh_.garbage_collection();
    }

    // ----- flip -----------------------------------------------------------

    void flip_edges()
    {
        for (unsigned int pass = 0; pass < max_passes; ++pass)
        {
            bool ok = true;

            for (auto e : mesh_.edges())
            {
                if (elocked_[e] || efeature_[e] || mesh_.is_boundary(e))
                    continue;
                if (!mesh_.is_flip_ok(e))
                    continue;

                const Halfedge h0 = mesh_.halfedge(e, 0);
                const Halfedge h1 = mesh_.halfedge(e, 1);
                const Vertex v0 = mesh_.to_vertex(h0);
                const Vertex v2 =
                    mesh_.to_vertex(mesh_.next_halfedge(h0));
                const Vertex v1 = mesh_.to_vertex(h1);
                const Vertex v3 =
                    mesh_.to_vertex(mesh_.next_halfedge(h1));
                if (vlocked_[v0] || vlocked_[v1] || vlocked_[v2] ||
                    vlocked_[v3])
                    continue;

                const Face fa = mesh_.face(h0);
                const Face fb = mesh_.face(h1);

                // worst metric quality of the two triangles before and
                // after the flip; accept only a strict improvement
                const Scalar q_before = std::min(triangle_quality(fa),
                                                 triangle_quality(fb));
                const Point old_n[2] = {face_normal(fa),
                                        face_normal(fb)};

                mesh_.flip(e);

                const Scalar q_after = std::min(triangle_quality(fa),
                                                triangle_quality(fb));
                const Point new_n[2] = {face_normal(fa),
                                        face_normal(fb)};

                bool accept =
                    q_after > q_before + Scalar(1e-12);
                for (int i = 0; i < 2 && accept; ++i)
                {
                    if (dot(new_n[i], new_n[i]) <= area_tolerance_ ||
                        dot(old_n[i], new_n[i]) <= 0)
                        accept = false;
                }

                if (!accept)
                    mesh_.flip(e);
                else
                    ok = false;
            }

            if (ok)
                break;
        }
    }

    Point face_normal(Face f) const
    {
        Point p[3];
        int k = 0;
        for (auto v : mesh_.vertices(f))
            p[k++] = points_[v];
        return cross(p[1] - p[0], p[2] - p[0]);
    }

    // ----- smoothing ------------------------------------------------------

    void tangential_smoothing(unsigned int iterations)
    {
        auto update = mesh_.add_vertex_property<Point>("v:update");
        auto update_metric =
            mesh_.add_vertex_property<Mat3>("v:update_metric");

        // vertices introduced by splitting need valid reference data
        for (auto v : mesh_.vertices())
        {
            if (vlocked_[v])
                continue;
            if (vrefseg_[v] != no_segment)
                project_to_polyline(v);
            else
                project_to_reference(v);
        }

        for (unsigned int it = 0; it < iterations; ++it)
        {
            vertex_normals(mesh_);

            for (auto v : mesh_.vertices())
            {
                if (vlocked_[v])
                    continue;

                if (vrefseg_[v] != no_segment)
                    smooth_polyline_vertex(v, update, update_metric);
                else if (!mesh_.is_boundary(v))
                    smooth_surface_vertex(v, update, update_metric);
                else
                {
                    update[v] = points_[v];
                    update_metric[v] = metric_[v];
                }
            }

            // commit the frozen-neighbor updates in one sweep
            for (auto v : mesh_.vertices())
            {
                if (vlocked_[v])
                    continue;
                points_[v] = update[v];
                metric_[v] = update_metric[v];
            }

            vertex_normals(mesh_);
        }

        // final reference projection
        for (auto v : mesh_.vertices())
        {
            if (vlocked_[v])
                continue;
            if (vrefseg_[v] != no_segment)
                project_to_polyline(v);
            else
                project_to_reference(v);
        }

        mesh_.remove_vertex_property(update);
        mesh_.remove_vertex_property(update_metric);
    }

    //! Interior vertex: minimize the sum of squared metric edge lengths
    //! against the frozen one-ring, restrict to the tangent plane, then
    //! project to the reference surface.
    void smooth_surface_vertex(Vertex v,
                               VertexProperty<Point>& update,
                               VertexProperty<Mat3>& update_metric)
    {
        Mat3 a(0);
        Point b(0);

        for (auto h : mesh_.halfedges(v))
        {
            const Vertex vv = mesh_.to_vertex(h);
            const Mat3 m =
                (metric_[v] + metric_[vv]) * Scalar(0.5);
            a += m;
            b += m * points_[vv];
        }

        Point target;
        if (!invert_symmetric(a, target, b))
            target = points_[v];

        Point displacement = target - points_[v];
        const Point n = vnormal_[v];
        displacement -= n * dot(displacement, n);
        target = points_[v] + displacement;

        if (!commit_with_step_control(v, target, update, update_metric,
                                      false))
        {
            update[v] = points_[v];
            update_metric[v] = metric_[v];
        }
    }

    //! Boundary/feature vertex: 1D minimization along the polyline
    //! tangent, followed by projection onto the home reference segment.
    void smooth_polyline_vertex(Vertex v,
                                VertexProperty<Point>& update,
                                VertexProperty<Mat3>& update_metric)
    {
        Point tangent(0);
        int count = 0;
        for (auto h : mesh_.halfedges(v))
        {
            if (erefseg_[mesh_.edge(h)] == no_segment)
                continue;
            const Vertex vv = mesh_.to_vertex(h);
            const Point dir = normalize(points_[vv] - points_[v]);
            tangent = (count == 0) ? tangent + dir : tangent - dir;
            ++count;
        }

        if (count != 2 || dot(tangent, tangent) <= 0)
        {
            update[v] = points_[v];
            update_metric[v] = metric_[v];
            return;
        }
        tangent = normalize(tangent);

        Scalar numerator(0), denominator(0);
        for (auto h : mesh_.halfedges(v))
        {
            if (erefseg_[mesh_.edge(h)] == no_segment)
                continue;
            const Vertex vv = mesh_.to_vertex(h);
            const Mat3 m =
                (metric_[v] + metric_[vv]) * Scalar(0.5);
            numerator +=
                dot(tangent, m * (points_[vv] - points_[v]));
            denominator += dot(tangent, m * tangent);
        }

        if (!(denominator > 0))
        {
            update[v] = points_[v];
            update_metric[v] = metric_[v];
            return;
        }

        const Point target =
            points_[v] + tangent * (numerator / denominator);

        if (!commit_with_step_control(v, target, update, update_metric,
                                      true))
        {
            update[v] = points_[v];
            update_metric[v] = metric_[v];
        }
    }

    //! Try progressively shorter steps toward \p target. Each candidate
    //! is projected onto the frozen reference and accepted only if every
    //! incident triangle stays non-degenerate and consistently oriented.
    //! Returns false if no legal step exists.
    bool commit_with_step_control(Vertex v, const Point& target,
                                  VertexProperty<Point>& update,
                                  VertexProperty<Mat3>& update_metric,
                                  bool polyline)
    {
        const Point original = points_[v];
        const Mat3 original_metric = metric_[v];
        // vnormal_ was computed before this vertex's move and the
        // candidate positions are rolled back after each test
        const Point old_normal = vnormal_[v];

        for (int attempt = 0; attempt < 12; ++attempt)
        {
            const Scalar scale =
                Scalar(1.0) / Scalar(1 << attempt);
            points_[v] = original + (target - original) * scale;

            if (polyline)
                project_to_polyline(v);
            else
                project_to_reference(v);

            if (move_is_valid(v) &&
                orientation_preserved(v, old_normal))
            {
                update[v] = points_[v];
                update_metric[v] = metric_[v];
                points_[v] = original;
                metric_[v] = original_metric;
                return true;
            }
        }

        points_[v] = original;
        metric_[v] = original_metric;
        return false;
    }

    static bool invert_symmetric(const Mat3& a, Point& x, const Point& b)
    {
        const Scalar det =
            a(0, 0) * (a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1)) -
            a(0, 1) * (a(1, 0) * a(2, 2) - a(1, 2) * a(2, 0)) +
            a(0, 2) * (a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0));
        if (!(std::fabs(det) > std::numeric_limits<Scalar>::min()))
            return false;

        Mat3 inv;
        inv(0, 0) = a(1, 1) * a(2, 2) - a(1, 2) * a(2, 1);
        inv(0, 1) = a(0, 2) * a(2, 1) - a(0, 1) * a(2, 2);
        inv(0, 2) = a(0, 1) * a(1, 2) - a(0, 2) * a(1, 1);
        inv(1, 0) = a(1, 2) * a(2, 0) - a(1, 0) * a(2, 2);
        inv(1, 1) = a(0, 0) * a(2, 2) - a(0, 2) * a(2, 0);
        inv(1, 2) = a(0, 2) * a(1, 0) - a(0, 0) * a(1, 2);
        inv(2, 0) = a(1, 0) * a(2, 1) - a(1, 1) * a(2, 0);
        inv(2, 1) = a(0, 1) * a(2, 0) - a(0, 0) * a(2, 1);
        inv(2, 2) = a(0, 0) * a(1, 1) - a(0, 1) * a(1, 0);
        inv *= Scalar(1.0) / det;

        x = inv * b;
        return true;
    }

    // ----- data -----------------------------------------------------------

    SurfaceMesh& mesh_;

    VertexProperty<Point> points_;
    VertexProperty<Point> vnormal_;
    VertexProperty<Mat3> metric_;
    VertexProperty<bool> vfeature_;
    EdgeProperty<bool> efeature_;
    VertexProperty<bool> vlocked_;
    EdgeProperty<bool> elocked_;
    VertexProperty<int> vrefseg_;
    EdgeProperty<int> erefseg_;

    bool has_feature_vertices_{false};
    bool has_feature_edges_{false};

    std::vector<RefTriangle> ref_triangles_;
    std::vector<RefSegment> ref_segments_;
    std::unique_ptr<ReferenceKdTree> kd_tree_;
    Scalar area_tolerance_{0};
};

} // namespace

void anisotropic_remeshing(SurfaceMesh& mesh, unsigned int iterations)
{
    AnisotropicRemeshing(mesh).run(iterations);
}

} // namespace pmp
