/*
libanim - hierarchical keyframe animation library
Copyright (C) 2012-2024 John Tsiombikas <nuclear@member.fsf.org>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Pose graph: soft relative-pose constraints over animation rig nodes.
 *
 * A pose graph treats the existing parent/child forest as weighted relative
 * pose observations and adds arbitrary extra observations between nodes
 * (cross-branch contacts, prop attachments, inconsistent closed loops). At a
 * given time it solves for a set of world rigid poses which minimize the
 * weighted translation/rotation discrepancy of all observations, then
 * publishes the result so that the regular animation matrix APIs consume it
 * at that exact time only.
 *
 * This is a soft pose-constraint solver for animation assembly, NOT rigid
 * body dynamics: there is no velocity, inertia or momentum, and bone segment
 * lengths are not preserved (nothing stops a solution from stretching).
 *
 * Scope/limitations of this version:
 * - single threaded
 * - rigid world poses only; animated nodes must use unit scaling
 * - node pivots, fixed dual-animation mixing (anm_use_node_animations) and
 *   animation time offsets are supported
 * - an active high-level automatic transition (anm_transition /
 *   anm_node_transition, marked by blend_dur >= 0) is rejected explicitly
 */
#ifndef ANM_POSE_GRAPH_H_
#define ANM_POSE_GRAPH_H_

#include "anim.h"

#ifdef __cplusplus
extern "C" {
#endif

/* distinguishable status/error codes */
enum anm_pg_status {
	ANM_PG_OK = 0,
	ANM_PG_EINVAL = 1,		/* null/empty/count mismatch arguments */
	ANM_PG_ENODE = 2,		/* node reference not part of this graph */
	ANM_PG_EWEIGHT = 3,		/* non-positive or non-finite weight */
	ANM_PG_ERIGID = 4,		/* non-finite or non-rigid transform */
	ANM_PG_EANCHOR = 5,		/* component without an anchor, bad anchor */
	ANM_PG_ETRANSITION = 6,/* active high-level auto transition */
	ANM_PG_ESCALE = 7,		/* non-unit animated scaling at solve time */
	ANM_PG_ECONVERGE = 8,	/* solver did not reach the gradient tolerance */
	ANM_PG_ENOMEM = 9		/* allocation failure */
};

/* one extra relative-pose observation: target rigid transform D_ij such that
 * inverse(W_i) * W_j should match it, with positive translation/rotation
 * weights. matrices are 16 floats, column-major (OpenGL convention).
 */
struct anm_pg_observation {
	struct anm_node *node_i;
	struct anm_node *node_j;
	float target[16];
	float wt;
	float wr;
};

struct anm_pg_options {
	float grad_tol;	/* positive inf-norm gradient tolerance (6D local pert.) */
	int max_iter;	/* positive iteration cap */
};

struct anm_pg_report {
	int status;		/* ANM_PG_OK or one of the ANM_PG_E* codes */
	float cost;		/* final objective value (sum of weighted residuals) */
	float grad_inf;	/* inf norm of the 6D local-perturbation gradient */
	int iterations;	/* iterations used; 0 if the initial point already passes */
};

struct anm_pose_graph;

/* Create a pose graph over the given nodes.
 *
 * nodes must stay alive until the graph is destroyed. The array carries full
 * ancestor chains: every non-null parent of a member node is a member too,
 * and the original parent/child relations stay a forest. Each node may belong
 * to at most one live graph. Anchors are member nodes whose world pose is
 * pinned exactly to the independently sampled animation pose at solve time;
 * every undirected connected component of the constraint graph needs one.
 *
 * Parent/child edges are generated automatically at solve time with the
 * sampled local matrix as target and the uniform weights parent_wt/wr.
 */
struct anm_pose_graph *anm_pg_create(struct anm_node **nodes, int num_nodes,
		struct anm_node **anchors, int num_anchors,
		float parent_wt, float parent_wr);

/* destroys the graph, detaches all overrides and lets the member node caches
 * be recomputed from the original animation.
 */
void anm_pg_destroy(struct anm_pose_graph *pg);

/* Replace the extra-observation table. The whole new table is validated
 * (node references, finiteness, rigid targets, positive weights, anchored
 * components together with the parent edges) before anything takes effect;
 * on failure the previously active table and any published result remain
 * untouched. Pass num_obs 0 / obs 0 to clear the extra observations.
 */
int anm_pg_replace_observations(struct anm_pose_graph *pg,
		const struct anm_pg_observation *obs, int num_obs);

/* Drop the currently published pose override (if any). Subsequent matrix
 * queries at every time come from the original animation again. The
 * observation table is kept.
 */
int anm_pg_clear_overrides(struct anm_pose_graph *pg);

/* Independently re-sample the original animation at tm and solve the graph.
 * The initial point is always the sampled world pose A_i(tm); no result is
 * carried over from previous solves. On success the result is published
 * atomically for exactly tm; on failure (including non-convergence) the last
 * published result, the tracks and the caches stay untouched. opts may be 0
 * for defaults (grad_tol 1e-7, max_iter 100).
 */
int anm_pg_solve(struct anm_pose_graph *pg, anm_time_t tm,
		const struct anm_pg_options *opts);

/* report of the most recent solve attempt (zeroed if none happened yet) */
int anm_pg_get_report(const struct anm_pose_graph *pg, struct anm_pg_report *rep);

/* internal hook used by anim.c: if node belongs to a graph with a published
 * override active at exactly tm, copies that world matrix (16 floats,
 * column-major) into out and returns 1; otherwise returns 0.
 */
int anm_pg_apply(const struct anm_node *node, anm_time_t tm, float *out);

#ifdef __cplusplus
}
#endif

#endif	/* ANM_POSE_GRAPH_H_ */
