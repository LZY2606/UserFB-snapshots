// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <vector>

#include "pmp/surface_mesh.h"

namespace pmp {

//! \brief Intrinsic edge-flip editing session with L2 projection of
//! piecewise linear scalar fields.
//! \details The session owns a private deep copy of the input triangle mesh
//! (including custom properties). The intrinsic metric is defined by the 3D
//! edge lengths of the input mesh. Flipping an edge unfolds the two incident
//! triangles into the plane and replaces the shared diagonal of the
//! resulting quadrilateral, updating only the intrinsic length of the
//! flipped edge; vertex positions and identities are left untouched.
//!
//! A continuous piecewise linear scalar field given by values at the
//! initial vertices can be transferred onto the current triangulation at
//! any time with project(): the consistent mass matrix system
//! \f$ M_\text{current}\, u = b \f$ is assembled and solved, where
//! \f$ b_i \f$ integrates the source field against the current hat
//! function \f$ \phi_i \f$ through the common refinement of the initial
//! and the current triangulation. All integrals are evaluated exactly on
//! intrinsic triangles.
//! \ingroup algorithms
class IntrinsicTransfer
{
public:
    //! \brief Construct a session from \p mesh.
    //! \details Deep-copies the triangulation and all custom properties.
    //! Later modifications of the input mesh do not affect the session and
    //! vice versa.
    //! \throw std::invalid_argument if a face is not a triangle or a
    //! triangle is degenerate.
    explicit IntrinsicTransfer(const SurfaceMesh& mesh);

    //! destructor
    ~IntrinsicTransfer();

    //! move constructor
    IntrinsicTransfer(IntrinsicTransfer&&) noexcept;
    //! move assignment
    IntrinsicTransfer& operator=(IntrinsicTransfer&&) noexcept;

    IntrinsicTransfer(const IntrinsicTransfer&) = delete;
    IntrinsicTransfer& operator=(const IntrinsicTransfer&) = delete;

    //! \brief The current triangulation (read-only).
    const SurfaceMesh& mesh() const;

    //! \brief Current intrinsic length of edge \p e.
    //! \details Returns 0 for invalid or deleted edge handles.
    double edge_length(Edge e) const;

    //! \brief Intrinsically flip edge \p e.
    //! \details Unfolds the two incident triangles and flips the diagonal of
    //! the unfolded quadrilateral. The new edge length is the unfolded
    //! distance of the two opposite vertices, not their 3D chord length.
    //! \return false for boundary edges, already existing diagonals, invalid
    //! or deleted handles, and unfoldings that do not form a non-degenerate
    //! strictly convex quadrilateral. In that case the mesh, the metric,
    //! and the source field correspondence are left unchanged.
    bool flip(Edge e);

    //! \brief L2-project a source field onto the current triangulation.
    //! \details \p source_values has one entry per initial vertex slot
    //! (initial vertices_size()); values at live vertices have to be finite,
    //! deleted slots are ignored. The source field is always the piecewise
    //! linear field on the initial surface; repeated calls do not
    //! accumulate. The result uses the same layout: deleted slots are zero,
    //! isolated live vertices keep their input value, and on vertices
    //! incident to faces the result solves the consistent mass matrix
    //! system \f$ M_\text{current}\, u = b \f$ assembled from intrinsic
    //! triangles.
    //! \throw std::invalid_argument for wrong array size or non-finite
    //! values at live vertices. The session remains usable.
    std::vector<double> project(const std::vector<double>& source_values) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pmp
