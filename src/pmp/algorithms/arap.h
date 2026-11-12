// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include "pmp/surface_mesh.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace pmp {

struct ArapSolverCache;

//! \addtogroup algorithms
//! @{

//! A hard position constraint for one vertex, used by ArapDeformation.
struct ArapHandle
{
    //! The constrained vertex.
    Vertex vertex;
    //! The target position the vertex is moved to.
    Point target;
};

//! \brief As-rigid-as-possible (ARAP) surface deformation.
//! \details Implements the local/global ARAP deformation of
//! \cite sorkine_2007_arap with cotangent weights and per-vertex 3D
//! rotations. The minimized energy is
//! \f[
//!   E(x,R)=\sum_i\sum_{j\in\mathcal N(i)} w_{ij}
//!           \left\|(x_i-x_j)-R_i(p_i-p_j)\right\|^2,
//! \f]
//! where \f$p\f$ are the coordinates captured at construction time (the
//! rest pose). Edge weights are half the sum of the cotangents opposite
//! to the edge in its incident triangles, clamped to non-negative values;
//! boundary edges use their single incident triangle. The weights are
//! computed once from the rest pose and never change.
//!
//! Handles are hard position constraints, implemented by eliminating the
//! constrained degrees of freedom from the sparse linear system (no
//! penalty factors). Each local step fits the best proper rotation
//! (\f$\det R=+1\f$) at every vertex with a non-zero weight; rank
//! deficient neighborhoods still get a minimum over proper rotations.
//!
//! Usage convention: after constructing an ArapDeformation, the caller
//! must not change the mesh topology and must not edit vertex positions
//! directly; all coordinate edits go through this object. The object is
//! intended to be short-lived and must not outlive the mesh.
//! \throw InvalidInputException if the mesh or a handle set is invalid.
//! \throw SolverException if the sparse factorization or a solve fails.
class ArapDeformation
{
public:
    //! Construct from a mesh and record its coordinates as rest pose.
    //! \param mesh The triangle mesh to deform; only finite coordinates,
    //! oriented 2-manifold connectivity and non-degenerate triangles are
    //! supported.
    //! \throw InvalidInputException if the mesh contains non-triangle
    //! faces, non-finite coordinates or zero-area triangles.
    explicit ArapDeformation(SurfaceMesh& mesh);

    //! Destructor (defined in the implementation to hide the solver cache).
    ~ArapDeformation();

    //! Replace the whole handle set.
    //! \details An empty vector releases all constraints. Invalid handles,
    //! duplicate vertices and non-finite target positions are rejected and
    //! leave the previously valid handle set untouched.
    //! \param handles The new set of hard position constraints.
    //! \throw InvalidInputException if any handle is invalid.
    void set_handles(const std::vector<ArapHandle>& handles);

    //! Run \p iterations local/global iterations and commit the result.
    //! \details Starts from the positions of the last successful call,
    //! applies the current handle targets, alternates optimal-rotation
    //! fits and global position solves, and writes the final positions
    //! back to the mesh only after every solve has succeeded.
    //! \param iterations Number of local/global iterations; must be >= 1.
    //! \return \p iterations + 1 energies: the energy at the starting
    //! state (after applying the handle targets) followed by the energy
    //! after each global update. Each energy is evaluated with freshly
    //! fitted optimal rotations on the complete directed energy.
    //! \throw InvalidInputException if \p iterations is zero.
    //! \throw SolverException if the factorization or a solve fails; in
    //! that case the mesh coordinates and the rest pose are unchanged.
    std::vector<double> deform(unsigned int iterations = 10);

    //! Number of sparse numerical factorizations performed so far.
    //! \details Only real factorizations of a non-empty free system are
    //! counted. Reusing a factorization for new handle targets or more
    //! iterations does not increase the count; changing the set of free
    //! vertices does.
    size_t factorization_count() const { return factorization_count_; }

private:
    SurfaceMesh& mesh_;

    std::vector<Point> rest_;           // rest pose positions p
    std::vector<double> edge_weight_;   // one weight per mesh edge
    std::vector<unsigned char> active_; // vertex incident to weight > 0

    std::vector<ArapHandle> handles_;
    std::vector<unsigned char> constrained_; // size vertices_size()

    std::unique_ptr<ArapSolverCache> cache_;
    size_t factorization_count_{0};
};

//! @}

} // namespace pmp
