// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2026 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DECIMATE_PROGRESSIVE_H
#define IGL_DECIMATE_PROGRESSIVE_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  /// A single, locally scoped vertex-split record.
  ///
  /// One record is created per **successful** edge collapse. Failed collapse
  /// candidates (e.g. those rejected by igl::edge_collapse_is_valid) consume no
  /// record index. Every index stored here is a _permanent_ id: the index the
  /// vertex or face had in the original input mesh. Working slots are never
  /// compacted while decimation runs, so a permanent id is also the slot at
  /// which the element is reactivated during refinement.
  ///
  /// The record stores only what that one collapse touched:
  ///
  /// - the two endpoints `s` (survivor) and `d` (detached vertex),
  /// - the detached vertex's position `vd` and continuous attributes `pd`,
  /// - the two faces killed by the collapse, with their full corner id
  ///   triples and integer labels,
  /// - the ring of surviving faces incident on `s` after the collapse (each
  ///   with the two non-`s` corner ids in winding order),
  /// - conservative dependency record indices.
  ///
  /// There are no full V/F snapshots anywhere in this structure.
  struct ProgressiveSplitRecord
  {
    /// Survivor vertex permanent id (the lesser endpoint of the collapsed edge,
    /// following igl::collapse_edge).
    int s = -1;
    /// Detached vertex permanent id; reactivated by the inverse vertex split.
    int d = -1;
    /// Position of `s` immediately before the collapse. The midpoint
    /// placement overwrites the survivor, so the split restores this. When a
    /// vertex survives several collapses, applying all splits reproduces its
    /// original position.
    Eigen::RowVectorXd vs;
    /// Continuous attributes of `s` immediately before the collapse (empty if
    /// the input carried no attributes).
    Eigen::RowVectorXd ps;
    /// Position of `d` immediately before the collapse (#dim vector).
    Eigen::RowVectorXd vd;
    /// Continuous attributes of `d` immediately before the collapse (#attr
    /// columns; empty if the input carried no attributes).
    Eigen::RowVectorXd pd;
    /// One entry per surviving face of the post-collapse star of `s`. Each
    /// row is [face permanent id, c, a, b], where c is the corner position of
    /// `s` within that face and (a,b) are the other two corner ids (in
    /// winding order), captured immediately after the collapse. Corner
    /// positions are never permuted by later collapses (only vertex ids are
    /// substituted), so refinement locates the current representative at the
    /// same position.
    Eigen::MatrixXi ring;
    /// For each row of `ring`, 1 if the face was incident on `d` before the
    /// collapse (the vertex split reassigns its survivor corner to `d`), and
    /// 0 if it stayed on the `s` side. A ring row whose face was killed by a
    /// _later_ collapse (and whose killing record is not being refined) is
    /// skipped by refine_progressive.
    Eigen::VectorXi ring_side;
    /// Permanent ids of the two faces killed by the collapse.
    int dead_face[2] = {-1,-1};
    /// Full corner id triples of the two killed faces, in their original
    /// winding order; row k corresponds to dead_face[k].
    Eigen::MatrixXi dead_corners;
    /// Integer labels of the two killed faces; row k corresponds to
    /// dead_face[k]. Labels follow the permanent face identity and are never
    /// interpolated.
    Eigen::VectorXi dead_labels;
    /// Indices of earlier records whose collapse rewrote a face that this
    /// collapse also rewrites. This is a _conservative backward_ dependency
    /// set: an edge is added whenever the two collapses touched any face with
    /// the same permanent id (their killed faces plus reconnected rings
    /// intersect). It documents the topological relationship it protects —
    /// the shared faces' corner slots, winding and the s/d substitution
    /// performed by igl::collapse_edge. The set is not required to be
    /// minimal; two collapses with disjoint face stars have no dependency
    /// even if they are far apart in collapse order, and records on
    /// different connected components never depend on each other.
    /// refine_progressive applies splits in descending record order, so a
    /// finer record listed here runs after the coarser one and wins any
    /// contested corner or value automatically; these indices are therefore
    /// descriptive of the face-star relationship rather than a separate
    /// global chain.
    std::vector<int> deps;
    /// Connected component (of the input mesh) on which the collapse happened.
    int component = -1;
  };

  /// A selectively refinable progressive mesh.
  ///
  /// Built by igl::decimate_progressive and consumed by igl::refine_progressive.
  /// Besides the compact coarse mesh it stores permanent vertex/face ids and a
  /// list of local vertex-split records with explicit, locality-based
  /// dependencies. The total record storage is linear in the number of
  /// corners and vertices actually rewritten by the collapses (each collapse
  /// touches only its own edge star), never in the size of full mesh
  /// snapshots.
  struct ProgressiveMesh
  {
    /// Compact coarse vertex positions (#CV by dim).
    Eigen::MatrixXd CV;
    /// Compact coarse triangle indices into CV (#CF by 3).
    Eigen::MatrixXi CF;
    /// Compact coarse vertex attributes (#CV by #attr columns; may have zero
    /// columns).
    Eigen::MatrixXd CP;
    /// Permanent vertex id of each coarse vertex (row of the input V).
    Eigen::VectorXi CI;
    /// Permanent face id of each coarse face (row of the input F).
    Eigen::VectorXi CJ;
    /// Integer label of each coarse face, indexed by permanent face identity.
    Eigen::VectorXi CL;
    /// Local vertex-split records, indexed in successful-collapse order.
    std::vector<ProgressiveSplitRecord> records;
    /// Number of vertices in the original fine mesh.
    int num_vertices = 0;
    /// Number of faces in the original fine mesh.
    int num_faces = 0;
    /// Number of connected components of the input mesh.
    int num_components = 0;
    /// Whether the requested face budget was reached. If false, CV/CF and the
    /// records still describe the best valid coarse mesh obtainable.
    bool reached = false;
  };

  /// Build a progressive mesh from a closed, oriented, edge-manifold triangle
  /// mesh with finite coordinates (possibly consisting of several disconnected
  /// components), using the _same_ engine as igl::decimate: shortest-edge
  /// cost, edge-midpoint placement, igl::edge_collapse_is_valid legality
  /// checks and the timestamped priority-queue collapse loop. No separate
  /// simplifier is introduced; on the same inputs the coarse mesh is identical
  /// to igl::decimate(V,F,max_m,false,...) up to permanent-id alignment.
  ///
  /// @param[in] V  #V by dim list of finite vertex positions
  /// @param[in] F  #F by 3 list of oriented triangle indices into V
  /// @param[in] P  #V by k list of continuous per-vertex attributes (may have
  ///   zero columns). At a collapse the survivor receives the mean of the two
  ///   endpoint attribute rows.
  /// @param[in] L  #F list of integer per-face labels (may be empty, which is
  ///   treated as all-zero labels). Labels travel with the permanent face
  ///   identity and are never interpolated.
  /// @param[in] max_m  requested maximum number of coarse faces
  /// @param[out] pm  progressive mesh, including the valid coarse mesh even
  ///   when the target could not be reached
  /// @return true if `max_m` was reached, false otherwise (mirroring
  ///   igl::decimate's return value)
  ///
  /// \see decimate, refine_progressive
  ///
  /// #### Example
  ///
  /// \code{cpp}
  /// Eigen::MatrixXd V, P;
  /// Eigen::MatrixXi F;
  /// Eigen::VectorXi L;
  /// // ... fill a closed manifold triangle mesh, attributes and labels ...
  /// igl::ProgressiveMesh pm;
  /// const bool reached = igl::decimate_progressive(V,F,P,L,F.rows()/2,pm);
  ///
  /// // Refine only the records the viewer needs around the camera.
  /// std::vector<int> requests = { /* record indices, any order, repeats ok */ };
  /// Eigen::MatrixXd RV, RP;
  /// Eigen::MatrixXi RF;
  /// Eigen::VectorXi RI, RJ, RL;
  /// igl::refine_progressive(pm, requests, RV, RF, RP, RJ, RI, RL);
  /// // RJ(k)/RI(i) align every output face/vertex with the original mesh.
  /// \endcode
  IGL_INLINE bool decimate_progressive(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const Eigen::MatrixXd & P,
    const Eigen::VectorXi & L,
    const int max_m,
    ProgressiveMesh & pm);
}

#ifndef IGL_STATIC_LIBRARY
#  include "decimate_progressive.cpp"
#endif
#endif
