// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QSLIM_H
#define IGL_QSLIM_H
#include "igl_inline.h"
#include "decimate_callback_types.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  /// Decimate (simplify) a triangle mesh in nD according to the paper
  /// "Simplifying Surfaces with Color and Texture using Quadric Error Metrics"
  /// by [Garland and Heckbert, 1987] (technically a followup to qslim). The
  /// mesh can have open boundaries but should be edge-manifold.
  ///
  /// @param[in] V  #V by dim list of vertex positions. Assumes that vertices with
  ///     infinite coordinates are "points at infinity" being used to close up
  ///     boundary edges with faces. This allows special subspace quadrice for
  ///     boundary edges: There should never be more than one "point at
  ///     infinity" in a single triangle.
  /// @param[in] F  #F by 3 list of triangle indices into V
  /// @param[in] max_m  desired number of output faces
  /// @param[in] block_intersections  whether to block intersections (see
  ///   intersection_blocking_collapse_edge_callbacks)
  /// @param[out] U  #U by dim list of output vertex posistions (can be same ref as V)
  /// @param[out] G  #G by 3 list of output face indices into U (can be same ref as F)
  /// @param[out] J  #G list of indices into F of birth face
  /// @param[out] I  #U list of indices into V of birth vertices
  IGL_INLINE bool qslim(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const int max_m,
    const bool block_intersections,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::VectorXi & J,
    Eigen::VectorXi & I);

  /// \overload
  ///
  /// \brief Decimate with quadric error metrics while applying a list of
  /// user-supplied callback decorators (e.g., igl::block_self_intersections,
  /// igl::block_intersections_with_input, or a custom decorator). Each decorator
  /// wraps the result of the previous, so they are cascaded in order.
  ///
  /// @param[in] decorators  list of pre/post-collapse callback decorators to
  ///   cascade (may be empty)
  ///
  /// \see block_self_intersections, block_intersections_with_input,
  ///   decimate_pre_post_collapse_callbacks_decorator
  IGL_INLINE bool qslim(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const int max_m,
    const std::vector<decimate_pre_post_collapse_callbacks_decorator> & decorators,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::VectorXi & J,
    Eigen::VectorXi & I);

  /// Decimate a closed manifold while preserving per-corner UV wedges.
  ///
  /// Geometry uses the real igl::decimate / collapse_least_cost_edge /
  /// collapse_edge queue and topology commit. UV quadrics are stored per
  /// wedge and endpoint wedges share one geometry placement while retaining
  /// independent UV coordinates across texture seams.
  ///
  /// Throws `std::invalid_argument` for inputs outside the validated finite,
  /// positively oriented closed two-manifold and simple closed seam-loop
  /// contract. Outputs are not assigned until validation succeeds.
  ///
  /// @param[in] V  #V by 3 geometry positions
  /// @param[in] F  #F by 3 geometry face indices
  /// @param[in] T  #T by 2 UV coordinates
  /// @param[in] FT  #F by 3 UV indices
  /// @param[in] max_m  target maximum number of output faces
  /// @param[in] uv_weight  positive finite scale multiplying UV coordinates
  /// @param[out] U  #U by 3 compacted output geometry positions
  /// @param[out] G  #G by 3 output faces
  /// @param[out] S  #S by 2 compacted output UV coordinates
  /// @param[out] FS  #G by 3 compacted output UV indices
  /// @param[out] J  #G birth-face indices into F
  /// @param[out] I  #U birth-vertex indices into V
  /// @return true iff the face target was reached
  IGL_INLINE bool qslim_with_uv(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const Eigen::MatrixXd & T,
    const Eigen::MatrixXi & FT,
    const int max_m,
    const double uv_weight,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::MatrixXd & S,
    Eigen::MatrixXi & FS,
    Eigen::VectorXi & J,
    Eigen::VectorXi & I);
}
#ifndef IGL_STATIC_LIBRARY
#  include "qslim.cpp"
#endif
#endif
