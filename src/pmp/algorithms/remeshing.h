// Copyright 2011-2020 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#pragma once

#include "pmp/surface_mesh.h"

namespace pmp {

//! \brief Perform uniform remeshing.
//! \details Performs incremental remeshing based
//! on edge collapse, split, flip, and tangential relaxation.
//! See \cite botsch_2004_remeshing and \cite dunyach_2013_adaptive for details.
//! \param mesh The input mesh, modified in place.
//! \param edge_length The target edge length.
//! \param iterations The number of iterations
//! \param use_projection Use back-projection to the input surface.
//! \pre Input mesh needs to be a triangle mesh.
//! \throw InvalidInputException if the input precondition is violated.
//! \ingroup algorithms
void uniform_remeshing(SurfaceMesh& mesh, Scalar edge_length,
                       unsigned int iterations = 10,
                       bool use_projection = true);

//! \brief Perform adaptive remeshing.
//! \details Performs incremental remeshing based
//! on edge collapse, split, flip, and tangential relaxation.
//! See \cite botsch_2004_remeshing and \cite dunyach_2013_adaptive for details.
//! \param mesh The input mesh, modified in place.
//! \param min_edge_length The minimum edge length.
//! \param max_edge_length The maximum edge length.
//! \param approx_error The maximum approximation error.
//! \param iterations The number of iterations.
//! \param use_projection Use back-projection to the input surface.
//! \pre Input mesh needs to be a triangle mesh.
//! \throw InvalidInputException if the input precondition is violated.
//! \ingroup algorithms
void adaptive_remeshing(SurfaceMesh& mesh, Scalar min_edge_length,
                        Scalar max_edge_length, Scalar approx_error,
                        unsigned int iterations = 10,
                        bool use_projection = true);

//! \brief Perform anisotropic (tensor-driven) remeshing.
//! \details Incremental remeshing driven by a symmetric positive definite
//! metric tensor field stored in the vertex property \c "v:metric" as
//! Matrix<Scalar,3,3> matrices expressed in world coordinates. Edge lengths
//! are measured in metric space as
//! \f$\sqrt{d^T \frac{M_0+M_1}{2} d}\f$, edges longer than \c 4/3 are split,
//! edges shorter than \c 4/5 are considered for collapse. Edge flips are
//! accepted only if the worst metric triangle quality of the two incident
//! triangles strictly improves, and tangential smoothing minimizes the sum
//! of squared metric edge lengths.
//!
//! The input surface and tensor field are frozen when the function is called.
//! New and moved vertices are projected onto that reference surface and their
//! metrics are barycentrically interpolated from the reference triangle and
//! written back to \c "v:metric". The reference field is never replaced by
//! the repeatedly interpolated current mesh.
//!
//! Boundary polylines and feature polylines (\c "e:feature") are preserved
//! (they may still be refined or collapsed along the polyline, but never
//! crossed), and feature corners (\c "v:feature") as well as locked or
//! unselected vertices are not moved. No operation creates degenerate or
//! inverted faces.
//! \param mesh The input mesh, modified in place.
//! \param iterations The number of iterations.
//! \pre Input mesh needs to be a triangle mesh with a finite, symmetric,
//! positive definite \c "v:metric" matrix for every vertex.
//! \throw InvalidInputException if the input precondition is violated.
//! \ingroup algorithms
void anisotropic_remeshing(SurfaceMesh& mesh,
                           unsigned int iterations = 10);

} // namespace pmp
