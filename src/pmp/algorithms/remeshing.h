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
//! \param preserve_texture If true and the mesh stores a halfedge property
//! named \c "h:tex" of type TexCoord, texture coordinates are preserved
//! throughout the remeshing. Faces are grouped into texture charts (connected
//! components across edges whose two incident corner UVs agree within
//! \f$10^{-6}\f$; mesh boundaries also separate charts). Each output corner
//! receives UVs interpolated from the closest point on the closest original
//! triangle of its own chart, so seam and boundary vertices may carry multiple
//! distinct corner UVs. Original seam/boundary vertex positions are locked and
//! seam/boundary polylines are only refined, never moved or crossed. When
//! \p use_projection is true, back-projection is likewise restricted to the
//! vertex's chart. When false, or when the property is absent, behavior is
//! identical to the non-textured algorithm.
//! \pre Input mesh needs to be a triangle mesh with finite coordinates,
//! non-degenerate oriented faces; open boundaries, multiple connected
//! components and UV seams are allowed.
//! \throw InvalidInputException if the input precondition is violated.
//! \ingroup algorithms
void uniform_remeshing(SurfaceMesh& mesh, Scalar edge_length,
                       unsigned int iterations = 10,
                       bool use_projection = true,
                       bool preserve_texture = false);

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
//! \param preserve_texture If true and the mesh stores a halfedge property
//! named \c "h:tex" of type TexCoord, texture coordinates are preserved
//! throughout the remeshing. See uniform_remeshing() for the chart, seam and
//! interpolation conventions.
//! \pre Input mesh needs to be a triangle mesh with finite coordinates,
//! non-degenerate oriented faces; open boundaries, multiple connected
//! components and UV seams are allowed.
//! \throw InvalidInputException if the input precondition is violated.
//! \ingroup algorithms
void adaptive_remeshing(SurfaceMesh& mesh, Scalar min_edge_length,
                        Scalar max_edge_length, Scalar approx_error,
                        unsigned int iterations = 10,
                        bool use_projection = true,
                        bool preserve_texture = false);

} // namespace pmp
