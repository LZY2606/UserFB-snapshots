/*
libanim pose graph CPU example
Copyright (C) 2024 John Tsiombikas <nuclear@member.fsf.org>

This program is free software. You can redistribute it and/or modify it
under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

This is a headless (no GLUT) demonstration of anm_pose_graph: two hands and
a prop form a closed loop of soft relative-pose observations, the graph
solves a weighted compromise, and vertices are bound through the usual
world * inverse-bind transform.
*/
#include <stdio.h>
#include <math.h>
#include <assert.h>

#include "anim.h"
#include "pose_graph.h"
#include "cgmath/cgmath.h"

/* transform a point by a column-major 4x4 matrix */
static void xform_point(const float *m, const float *p, float *out)
{
	cgm_vec4 v;
	cgm_wcons(&v, p[0], p[1], p[2], 1.0f);
	cgm_wmul_m4v4(&v, m);
	out[0] = v.x; out[1] = v.y; out[2] = v.z;
}

static int close3(const float *a, float x, float y, float z, float tol)
{
	return fabsf(a[0] - x) <= tol && fabsf(a[1] - y) <= tol &&
			fabsf(a[2] - z) <= tol;
}

int main(void)
{
	/* rig: root with two hands and a prop hanging off the root. The extra
	 * observations hand->prop close a loop across the branches.
	 */
	struct anm_node root, lhand, rhand, prop;
	struct anm_node *nodes[4];
	struct anm_node *anchors[3];
	struct anm_pose_graph *pg;
	struct anm_pg_observation obs[2];
	struct anm_pg_options opts = {1e-8f, 100};
	struct anm_pg_report rep;
	float bind_prop_inv[16], bind_lhand_inv[16];
	/* vertices are given in bind-pose world coordinates, so the runtime
	 * world * inverse-bind formula applies literally
	 */
	float v_prop[3] = {0.5f, 0.0f, 0.0f};
	float v_hand[3] = {1.0f, 0.25f, 0.0f};
	float skinned_prop[3], skinned_hand[3];
	float Wprop[16], skin_m[16];
	const anm_time_t tm = 0;

	anm_init_node(&root);
	anm_init_node(&lhand);
	anm_init_node(&rhand);
	anm_init_node(&prop);
	anm_link_node(&root, &lhand);
	anm_link_node(&root, &rhand);
	anm_link_node(&root, &prop);

	/* sampled animation poses: hands pinned by anchors, prop at the origin */
	anm_set_position3f(&lhand, 1.0f, 0.0f, 0.0f, tm);
	anm_set_position3f(&rhand, -1.0f, 0.0f, 0.0f, tm);
	anm_set_position3f(&prop, 0.0f, 0.0f, 0.0f, tm);

	nodes[0] = &root; nodes[1] = &lhand; nodes[2] = &rhand; nodes[3] = &prop;
	anchors[0] = &root; anchors[1] = &lhand; anchors[2] = &rhand;

	/* bind pose capture, before any graph exists: plain animation state */
	anm_get_inv_matrix(&prop, bind_prop_inv, tm);
	anm_get_inv_matrix(&lhand, bind_lhand_inv, tm);

	pg = anm_pg_create(nodes, 4, anchors, 3, 1.0f, 1.0f);
	assert(pg);

	/* left hand wants the prop 4 units to its right (weight 4);
	 * right hand wants it 3 units to its right (weight 1). Together with
	 * the parent edge pulling the prop to the origin, the loop is
	 * inconsistent, so the solver finds the weighted compromise.
	 */
	obs[0].node_i = &lhand; obs[0].node_j = &prop;
	obs[0].wt = 4.0f; obs[0].wr = 1.0f;
	cgm_midentity(obs[0].target); obs[0].target[12] = 4.0f;

	obs[1].node_i = &rhand; obs[1].node_j = &prop;
	obs[1].wt = 1.0f; obs[1].wr = 1.0f;
	cgm_midentity(obs[1].target); obs[1].target[12] = 3.0f;

	assert(anm_pg_replace_observations(pg, obs, 2) == ANM_PG_OK);
	assert(anm_pg_solve(pg, tm, &opts) == ANM_PG_OK);
	anm_pg_get_report(pg, &rep);
	assert(rep.status == ANM_PG_OK);

	/* independent 1D expectation for the prop position x:
	 *   parent edge:   (x - 0)^2
	 *   left hand:     4 (x - (1 + 4))^2 = 4(x - 5)^2
	 *   right hand:    1 (x - (-1 + 3))^2 = (x - 2)^2
	 * derivative: 2x + 8(x-5) + 2(x-2) = 12x - 44 = 0  ->  x = 11/3
	 */
	anm_get_matrix(&prop, Wprop, tm);
	{
		const float expect_x = 11.0f / 3.0f;
		assert(fabsf(Wprop[12] - expect_x) <= 2e-4f);
		assert(fabsf(Wprop[13]) <= 2e-4f && fabsf(Wprop[14]) <= 2e-4f);
	}

	/* bind a prop vertex: skin = W_prop * inverse(bind W_prop) * v.
	 * cgm_mmul(a,b) computes b*a, so the standard product W*bindInv is
	 * written mmul(bindInv, W)
	 */
	cgm_mcopy(skin_m, bind_prop_inv);
	cgm_mmul(skin_m, Wprop);
	xform_point(skin_m, v_prop, skinned_prop);
	/* bind prop world was the identity, so expect expect_x + v.x */
	assert(close3(skinned_prop, 11.0f / 3.0f + 0.5f, 0.0f, 0.0f, 2e-4f));

	/* an anchored hand does not move: its skinned vertex stays at the
	 * bind-local offset from the fixed hand world pose
	 */
	{
		float Whand[16];
		anm_get_matrix(&lhand, Whand, tm);
		cgm_mcopy(skin_m, bind_lhand_inv);
		cgm_mmul(skin_m, Whand);
		xform_point(skin_m, v_hand, skinned_hand);
		assert(close3(skinned_hand, 1.0f, 0.25f, 0.0f, 2e-4f));
	}

	/* at any other time the original animation (prop at origin) is used */
	{
		float Wother[16];
		anm_get_matrix(&prop, Wother, 999);
		assert(fabsf(Wother[12]) <= 2e-4f);
	}

	anm_pg_destroy(pg);
	anm_destroy_node(&prop);
	anm_destroy_node(&rhand);
	anm_destroy_node(&lhand);
	anm_destroy_node(&root);

	puts("POSE_GRAPH_DEMO_OK prop_x=11/3 skinned=25/6 anchored_hand_fixed");
	return 0;
}
