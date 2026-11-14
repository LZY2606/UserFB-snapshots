// Copyright 2011-2020 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "pmp/algorithms/distance_point_triangle.h"
#include "pmp/bounding_box.h"
#include "pmp/surface_mesh.h"

namespace pmp {
namespace detail {

//! Static kd-tree over the triangles of a fixed SurfaceMesh.
//! Used by the remeshing algorithms to back-project points onto a frozen
//! reference surface.
class TriangleKdTree
{
public:
    TriangleKdTree(std::shared_ptr<const SurfaceMesh> mesh,
                   unsigned int max_faces = 10, unsigned int max_depth = 30);

    ~TriangleKdTree() { delete root_; }

    struct NearestNeighbor
    {
        Scalar dist;
        Face face;
        Point nearest;
    };

    NearestNeighbor nearest(const Point& p) const;

private:
    using Faces = std::vector<Face>;

    struct Node
    {
        Node() = default;

        ~Node()
        {
            delete faces;
            delete left_child;
            delete right_child;
        }

        unsigned char axis;
        Scalar split;
        Faces* faces{nullptr};
        Node* left_child{nullptr};
        Node* right_child{nullptr};
    };

    void build_recurse(Node* node, unsigned int max_handles,
                       unsigned int depth);

    void nearest_recurse(Node* node, const Point& point,
                         NearestNeighbor& data) const;

    Node* root_;
    std::vector<std::array<Point, 3>> face_points_;
};

inline TriangleKdTree::TriangleKdTree(
    std::shared_ptr<const SurfaceMesh> mesh, unsigned int max_faces,
    unsigned int max_depth)
{
    // init
    root_ = new Node();
    root_->faces = new Faces();

    // collect faces and points
    root_->faces->reserve(mesh->n_faces());
    face_points_.reserve(mesh->n_faces());
    auto points = mesh->get_vertex_property<Point>("v:point");

    for (const auto& f : mesh->faces())
    {
        root_->faces->push_back(f);

        auto v = mesh->vertices(f);
        const auto& p0 = points[*v];
        ++v;
        const auto& p1 = points[*v];
        ++v;
        const auto& p2 = points[*v];
        face_points_.push_back({p0, p1, p2});
    }

    // call recursive helper
    build_recurse(root_, max_faces, max_depth);
}

inline void TriangleKdTree::build_recurse(Node* node,
                                          unsigned int max_faces,
                                          unsigned int depth)
{
    // should we stop at this level ?
    if ((depth == 0) || (node->faces->size() <= max_faces))
        return;

    // compute bounding box
    BoundingBox bbox;
    for (const auto& f : *node->faces)
    {
        const auto idx = f.idx();
        bbox += face_points_[idx][0];
        bbox += face_points_[idx][1];
        bbox += face_points_[idx][2];
    }

    // split longest side of bounding box
    Point bb = bbox.max() - bbox.min();
    Scalar length = bb[0];
    int axis = 0;
    if (bb[1] > length)
        length = bb[(axis = 1)];
    if (bb[2] > length)
        length = bb[(axis = 2)];

    // split in the middle
    const Scalar split = bbox.center()[axis];

    // create children
    auto* left = new Node();
    left->faces = new Faces();
    left->faces->reserve(node->faces->size() / 2);
    auto* right = new Node();
    right->faces = new Faces;
    right->faces->reserve(node->faces->size() / 2);

    // partition for left and right child
    for (const auto& f : *node->faces)
    {
        bool l = false, r = false;

        const auto& pos = face_points_[f.idx()];

        if (pos[0][axis] <= split)
            l = true;
        else
            r = true;
        if (pos[1][axis] <= split)
            l = true;
        else
            r = true;
        if (pos[2][axis] <= split)
            l = true;
        else
            r = true;

        if (l)
        {
            left->faces->push_back(f);
        }

        if (r)
        {
            right->faces->push_back(f);
        }
    }

    // stop here?
    if (left->faces->size() == node->faces->size() ||
        right->faces->size() == node->faces->size())
    {
        // compact my memory
        node->faces->shrink_to_fit();

        // delete new nodes
        delete left;
        delete right;

        // stop recursion
        return;
    }

    // or recurse further?
    else
    {
        // free my memory
        delete node->faces;
        node->faces = nullptr;

        // store internal data
        node->axis = axis;
        node->split = split;
        node->left_child = left;
        node->right_child = right;

        // recurse to children
        build_recurse(node->left_child, max_faces, depth - 1);
        build_recurse(node->right_child, max_faces, depth - 1);
    }
}

inline TriangleKdTree::NearestNeighbor
TriangleKdTree::nearest(const Point& p) const
{
    NearestNeighbor data;
    data.dist = std::numeric_limits<Scalar>::max();
    nearest_recurse(root_, p, data);
    return data;
}

inline void TriangleKdTree::nearest_recurse(Node* node, const Point& point,
                                            NearestNeighbor& data) const
{
    // terminal node?
    if (!node->left_child)
    {
        for (const auto& f : *node->faces)
        {
            Point n;
            const auto& pos = face_points_[f.idx()];
            auto d = dist_point_triangle(point, pos[0], pos[1], pos[2], n);
            // equidistant triangles resolve to the smallest face index,
            // so projection onto the frozen reference mesh is deterministic
            const Scalar tol =
                Scalar(1e-12) *
                (Scalar(1.0) + std::fabs(data.dist));
            if (d < data.dist - tol ||
                (std::fabs(d - data.dist) <= tol &&
                 (!data.face.is_valid() || f.idx() < data.face.idx())))
            {
                data.dist = d;
                data.face = f;
                data.nearest = n;
            }
        }
    }

    // non-terminal node
    else
    {
        const Scalar dist = point[node->axis] - node->split;

        if (dist <= 0.0)
        {
            nearest_recurse(node->left_child, point, data);
            if (fabs(dist) < data.dist)
                nearest_recurse(node->right_child, point, data);
        }
        else
        {
            nearest_recurse(node->right_child, point, data);
            if (fabs(dist) < data.dist)
                nearest_recurse(node->left_child, point, data);
        }
    }
}

} // namespace detail
} // namespace pmp
