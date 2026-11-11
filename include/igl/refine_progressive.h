// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2026 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_REFINE_PROGRESSIVE_H
#define IGL_REFINE_PROGRESSIVE_H
#include "igl_inline.h"
#include "decimate_progressive.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  /// Selectively refine a progressive mesh produced by
  /// igl::decimate_progressive. The original fine mesh is not needed and
  /// decimation is not re-run: only the compact coarse mesh is copied once and
  /// the requested local vertex splits are applied.
  ///
  /// Two locality-based dependency kinds are honored. The conservative
  /// backward face-star edges stored in ProgressiveSplitRecord::deps are
  /// respected automatically because splits execute in descending record
  /// order: a finer split runs after the coarser one and wins every contested
  /// corner or value. The _legality_ dependencies are completed explicitly by
  /// walking only the record's own stored corner ids through the local
  /// collapse forest: a face can only be restored at its original resolution
  /// once every later collapse that absorbed one of its corners is undone,
  /// otherwise the face would be reborn degenerate or with an extra flap.
  /// Neither walk scans the mesh, and forest paths stay inside one connected
  /// component, so records on independent components never block each other.
  /// Requests may be repeated, free of out-of-range indices but in any order;
  /// duplicates and order do not change the result. An empty request returns
  /// the coarse mesh unchanged.
  ///
  /// The output is a drawable, edge-manifold triangle mesh: no dangling
  /// vertices, no duplicate faces and no references to inactive vertices.
  /// Every output vertex and face carries its permanent id, so results can be
  /// compared against the original mesh by id alignment.
  ///
  /// @param[in] pm  progressive mesh
  /// @param[in] requested  record indices to apply (subset of
  ///   [0, pm.records.size())); any order, repeats allowed
  /// @param[out] RV  #RV by dim output vertex positions
  /// @param[out] RF  #RF by 3 output triangle indices into RV
  /// @param[out] RP  #RV by k output vertex attributes
  /// @param[out] RJ  #RF permanent face ids (rows of the input F)
  /// @param[out] RI  #RV permanent vertex ids (rows of the input V)
  /// @param[out] RL  #RF integer face labels, following permanent face
  ///   identity
  /// @return true on success; false (with all outputs left untouched) if any
  ///   requested index is out of range
  ///
  /// \see decimate_progressive
  IGL_INLINE bool refine_progressive(
    const ProgressiveMesh & pm,
    const std::vector<int> & requested,
    Eigen::MatrixXd & RV,
    Eigen::MatrixXi & RF,
    Eigen::MatrixXd & RP,
    Eigen::VectorXi & RJ,
    Eigen::VectorXi & RI,
    Eigen::VectorXi & RL);
}

#ifndef IGL_STATIC_LIBRARY
#  include "refine_progressive.cpp"
#endif
#endif
