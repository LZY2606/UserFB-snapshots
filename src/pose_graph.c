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

/* Pose graph implementation: weighted relative-pose observations over the
 * node forest, solved as a rigid-pose nonlinear least squares problem.
 *
 * Variables are world rigid poses W_i = (R_i, p_i) of the non-anchor nodes
 * (anchors are pinned to the independently sampled animation pose). For each
 * observation (i,j) with target D_ij = (Rd, td) and weights wt, wr:
 *   Q_ij = inverse(W_i) W_j, Rq = Ri^T Rj, tq = Ri^T (p_j - p_i)
 *   residual r = (sqrt(wt)(tq - td), sqrt(wr) Log(Rd^T Rq))
 * and the minimized cost is sum |r|^2. Log is the axis-angle vector in
 * radians (never a quaternion component difference).
 *
 * Each solve re-samples the original animation from scratch through the
 * track APIs only (no matrix caches, no previous graph result), starts the
 * iteration at the sampled poses, and uses Gauss-Newton with Levenberg
 * damping and a backtracking line search so that rotation and translation
 * are optimized jointly and the cost never increases.
 *
 * All solver-internal arithmetic is double precision (the public matrices
 * stay single precision), so the reported gradient reflects a true local
 * stationary point rather than float round-off floor.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "pose_graph.h"
#include "cgmath/cgmath.h"

#define RIGID_TOL		1e-4f
#define SCALE_TOL		1e-5f
#define DEFAULT_GTOL	1e-7f
#define DEFAULT_MAXIT	100
#define MAX_LAMBDA_TRY	40

struct pg_edge {
	int i, j;				/* node indices */
	double rd[9];			/* target rotation, column-major 3x3 */
	double td[3];			/* target translation */
	double wt, wr;
};

struct anm_pose_graph {
	struct anm_node **nodes;
	int num_nodes;
	int *is_anchor;
	int *owner_mark;		/* 1 while this graph owns the node */
	double parent_wt, parent_wr;

	struct anm_pg_observation *obs;
	int num_obs;

	int has_pub;
	anm_time_t pub_time;
	float *pub_w;			/* num_nodes * 16 published world matrices */

	struct anm_pg_report report;
};

/* ---------- float input validation ---------- */

static int finite_m4(const float *m)
{
	int i;
	for(i = 0; i < 16; i++) {
		if(!isfinite(m[i])) return 0;
	}
	return 1;
}

/* checks finiteness, homogeneous structure, orthogonality and det == +1 */
static int valid_rigid_m4(const float *m)
{
	float r[9], prod[9], det, err;
	int i, j, k;

	if(!finite_m4(m)) return 0;
	if(fabsf(m[3]) > RIGID_TOL || fabsf(m[7]) > RIGID_TOL ||
			fabsf(m[11]) > RIGID_TOL || fabsf(m[15] - 1.0f) > RIGID_TOL) {
		return 0;
	}

	r[0] = m[0];  r[1] = m[1];  r[2] = m[2];
	r[3] = m[4];  r[4] = m[5];  r[5] = m[6];
	r[6] = m[8];  r[7] = m[9];  r[8] = m[10];

	for(i = 0; i < 3; i++) {
		for(j = 0; j < 3; j++) {
			float s = 0;
			for(k = 0; k < 3; k++) {
				s += r[j * 3 + k] * r[i * 3 + k];
			}
			prod[i * 3 + j] = s;
		}
	}
	err = 0;
	for(i = 0; i < 3; i++) {
		for(j = 0; j < 3; j++) {
			float expect = (i == j) ? 1.0f : 0.0f;
			err += fabsf(prod[i * 3 + j] - expect);
		}
	}
	if(err > RIGID_TOL) return 0;

	det = r[0] * (r[4] * r[8] - r[5] * r[7])
		- r[3] * (r[1] * r[8] - r[2] * r[7])
		+ r[6] * (r[1] * r[5] - r[2] * r[4]);
	if(fabsf(det - 1.0f) > RIGID_TOL) return 0;
	return 1;
}

/* ---------- double precision rigid math (column-major 3x3) ---------- */

static void m4_to_rigid(const float *m, double *r, double *t)
{
	if(t) {
		t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
	}
	r[0] = m[0];  r[1] = m[1];  r[2] = m[2];
	r[3] = m[4];  r[4] = m[5];  r[5] = m[6];
	r[6] = m[8];  r[7] = m[9];  r[8] = m[10];
}

static void rigid_to_m4(float *m, const double *r, const double *t)
{
	memset(m, 0, 16 * sizeof *m);
	m[0] = (float)r[0]; m[1] = (float)r[1]; m[2] = (float)r[2];
	m[4] = (float)r[3]; m[5] = (float)r[4]; m[6] = (float)r[5];
	m[8] = (float)r[6]; m[9] = (float)r[7]; m[10] = (float)r[8];
	m[12] = (float)t[0]; m[13] = (float)t[1]; m[14] = (float)t[2];
	m[15] = 1.0f;
}

static void d_identity(double *r)
{
	static const double id[9] = {1,0,0, 0,1,0, 0,0,1};
	memcpy(r, id, sizeof id);
}

static void d_mul(double *c, const double *a, const double *b)
{
	double res[9];
	int i, j, k;
	for(j = 0; j < 3; j++) {
		for(i = 0; i < 3; i++) {
			double s = 0;
			for(k = 0; k < 3; k++) {
				s += a[k * 3 + i] * b[j * 3 + k];
			}
			res[j * 3 + i] = s;
		}
	}
	memcpy(c, res, sizeof res);
}

static void d_transpose(double *out, const double *r)
{
	double t[9];
	int i, j;
	for(j = 0; j < 3; j++) {
		for(i = 0; i < 3; i++) {
			t[i * 3 + j] = r[j * 3 + i];
		}
	}
	memcpy(out, t, sizeof t);
}

static void d_apply(const double *r, const double *v, double *out)
{
	double res[3];
	int i, j;
	for(i = 0; i < 3; i++) {
		double s = 0;
		for(j = 0; j < 3; j++) {
			s += r[j * 3 + i] * v[j];
		}
		res[i] = s;
	}
	out[0] = res[0]; out[1] = res[1]; out[2] = res[2];
}

static void d_skew(double *k, const double *v)
{
	k[0] = 0;      k[3] = -v[2];  k[6] = v[1];
	k[1] = v[2];   k[4] = 0;      k[7] = -v[0];
	k[2] = -v[1];  k[5] = v[0];   k[8] = 0;
}

/* Rodrigues: r = Exp(w), w is the radians axis-angle vector */
static void d_exp(double *r, const double *w)
{
	double angle = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	double k[9], k2[9], s, c, f;
	int i;

	d_identity(r);
	if(angle < 1e-12) return;

	d_skew(k, w);
	d_mul(k2, k, k);
	s = sin(angle);
	c = 1.0 - cos(angle);
	f = c / (angle * angle);
	for(i = 0; i < 9; i++) {
		r[i] += (s / angle) * k[i] + f * k2[i];
	}
}

/* axis-angle logarithm, angle in (-pi, pi].
 * Log(R) = f (R32-R23, R13-R31, R21-R12), R(i,j) at r[j*3+i]
 */
static void d_log(const double *r, double *w)
{
	double cosang = (r[0] + r[4] + r[8] - 1.0) * 0.5;
	double angle, s, f;

	if(cosang > 1.0) cosang = 1.0;
	if(cosang < -1.0) cosang = -1.0;
	angle = acos(cosang);
	s = sin(angle);

	if(s < 1e-9) {
		/* near identity (near pi is excluded by the problem assumptions) */
		f = 0.5;
	} else {
		f = angle / (2.0 * s);
	}
	w[0] = f * (r[5] - r[7]);
	w[1] = f * (r[6] - r[2]);
	w[2] = f * (r[1] - r[3]);
}

/* inverse SO(3) right Jacobian Jr^-1(w) = I + 1/2 [w]x + c [w]x^2,
 * c = 1/theta^2 - (1+cos theta)/(2 theta sin theta)
 */
static void d_jr_inverse(double *out, const double *w)
{
	double k[9], k2[9], theta2, theta, c;
	int i;

	theta2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
	theta = sqrt(theta2);
	d_skew(k, w);
	d_mul(k2, k, k);
	d_identity(out);

	if(theta < 1e-6) {
		c = 1.0 / 12.0;	/* series limit */
	} else {
		c = 1.0 / theta2 -
			(1.0 + cos(theta)) / (2.0 * theta * sin(theta));
	}
	for(i = 0; i < 9; i++) {
		out[i] += 0.5 * k[i] + c * k2[i];
	}
}

/* ---------- graph internals ---------- */

static int find_node(struct anm_pose_graph *pg, struct anm_node *node)
{
	int i;
	for(i = 0; i < pg->num_nodes; i++) {
		if(pg->nodes[i] == node) return i;
	}
	return -1;
}

static int active_transition(struct anm_node *node)
{
	/* the high-level auto transition only mutates animation state while a
	 * second animation slot is active; flag exactly that combination
	 */
	return node->blend_dur >= 0 && node->cur_anim[1] >= 0;
}

/* sample the node-local matrix straight from the tracks, bypassing the
 * matrix caches and any published pose-graph override. returns 1 ok,
 * 0 non-finite/non-rigid, -1 non-unit scale.
 */
static int raw_local_matrix(struct anm_node *node, anm_time_t tm, float *m)
{
	int i;
	float rmat[16];
	cgm_vec3 pos, scale;
	cgm_quat rot;

	anm_get_node_position(node, &pos.x, tm);
	anm_get_node_rotation(node, &rot.x, tm);
	anm_get_node_scaling(node, &scale.x, tm);

	if(!isfinite(pos.x) || !isfinite(pos.y) || !isfinite(pos.z) ||
			!isfinite(rot.x) || !isfinite(rot.y) ||
			!isfinite(rot.z) || !isfinite(rot.w)) {
		return 0;
	}
	if(fabsf(scale.x - 1.0f) > SCALE_TOL ||
			fabsf(scale.y - 1.0f) > SCALE_TOL ||
			fabsf(scale.z - 1.0f) > SCALE_TOL) {
		return -1;
	}

	cgm_mtranslation(m, node->pivot[0], node->pivot[1], node->pivot[2]);
	cgm_mrotation_quat(rmat, &rot);
	for(i = 0; i < 3; i++) {
		m[i] = rmat[i];
		m[4 + i] = rmat[4 + i];
		m[8 + i] = rmat[8 + i];
	}
	m[12] += pos.x; m[13] += pos.y; m[14] += pos.z;
	cgm_mpretranslate(m, -node->pivot[0], -node->pivot[1], -node->pivot[2]);

	if(!valid_rigid_m4(m)) return 0;
	return 1;
}

static void invalidate_member_caches(struct anm_pose_graph *pg)
{
	int i;
	for(i = 0; i < pg->num_nodes; i++) {
		struct anm_node *node = pg->nodes[i];
#ifdef ANIM_THREAD_SAFE
		struct mat_cache *c = node->cache_list;
		while(c) {
			c->time = c->inv_time = ANM_TIME_INVAL;
			c = c->next;
		}
#else
		node->cache.time = node->cache.inv_time = ANM_TIME_INVAL;
#endif
	}
}

/* build parent + extra edges for the current table */
static int build_edges(struct anm_pose_graph *pg, anm_time_t tm,
		struct pg_edge **out_edges, int *out_count)
{
	int i, n, cap;
	struct pg_edge *edges;

	cap = pg->num_nodes + pg->num_obs;
	if(!(edges = malloc((cap > 0 ? cap : 1) * sizeof *edges))) {
		return ANM_PG_ENOMEM;
	}
	n = 0;

	/* parent/child observations: target is the sampled local pose L_j(t) */
	for(i = 0; i < pg->num_nodes; i++) {
		struct anm_node *node = pg->nodes[i];
		float lm[16];
		int rc, pidx;

		if(!node->parent) continue;
		pidx = find_node(pg, node->parent);
		if(pidx < 0) continue;	/* cannot happen, create validates ancestry */

		rc = raw_local_matrix(node, tm, lm);
		if(rc == -1) { free(edges); return ANM_PG_ESCALE; }
		if(rc == 0) { free(edges); return ANM_PG_ERIGID; }

		edges[n].i = pidx;
		edges[n].j = i;
		m4_to_rigid(lm, edges[n].rd, edges[n].td);
		edges[n].wt = pg->parent_wt;
		edges[n].wr = pg->parent_wr;
		n++;
	}

	for(i = 0; i < pg->num_obs; i++) {
		struct pg_edge *e;
		int idx_i, idx_j;

		idx_i = find_node(pg, pg->obs[i].node_i);
		idx_j = find_node(pg, pg->obs[i].node_j);
		if(idx_i < 0 || idx_j < 0) {
			free(edges);
			return ANM_PG_ENODE;
		}
		e = edges + n++;
		e->i = idx_i;
		e->j = idx_j;
		m4_to_rigid(pg->obs[i].target, e->rd, e->td);
		e->wt = pg->obs[i].wt;
		e->wr = pg->obs[i].wr;
	}

	*out_edges = edges;
	*out_count = n;
	return ANM_PG_OK;
}

/* every undirected component of the edge graph must contain an anchor */
static int check_components(struct anm_pose_graph *pg, const struct pg_edge *edges,
		int num_edges)
{
	char *seen;
	int *stack;
	int i, top, ok = 1;

	if(!(seen = calloc((size_t)pg->num_nodes, 1))) return ANM_PG_ENOMEM;
	if(!(stack = malloc((size_t)pg->num_nodes * sizeof *stack))) {
		free(seen);
		return ANM_PG_ENOMEM;
	}

	for(i = 0; i < pg->num_nodes; i++) {
		int k, has_anchor;

		if(seen[i]) continue;
		seen[i] = 1;
		stack[0] = i;
		top = 1;
		has_anchor = pg->is_anchor[i];

		while(top > 0) {
			int cur = stack[--top];
			for(k = 0; k < num_edges; k++) {
				int other = -1;
				if(edges[k].i == cur) other = edges[k].j;
				else if(edges[k].j == cur) other = edges[k].i;
				if(other >= 0 && !seen[other]) {
					seen[other] = 1;
					if(pg->is_anchor[other]) has_anchor = 1;
					stack[top++] = other;
				}
			}
		}
		if(!has_anchor) { ok = 0; break; }
	}

	free(seen);
	free(stack);
	return ok ? ANM_PG_OK : ANM_PG_EANCHOR;
}

/* evaluate the full objective for a candidate state (all nodes) */
static double eval_cost(const struct pg_edge *edges,
		int num_edges, const double *rr, const double *pp)
{
	double cost = 0;
	int k;

	for(k = 0; k < num_edges; k++) {
		const struct pg_edge *e = edges + k;
		const double *ri = rr + e->i * 9;
		const double *rj = rr + e->j * 9;
		const double *pi = pp + e->i * 3;
		const double *pj = pp + e->j * 3;
		double rit[9], rq[9], rel[9], rdt[9], w[3], d[3], tq[3], et[3];
		int a;

		d_transpose(rit, ri);
		d_mul(rq, rit, rj);
		for(a = 0; a < 3; a++) d[a] = pj[a] - pi[a];
		d_apply(rit, d, tq);
		for(a = 0; a < 3; a++) et[a] = tq[a] - e->td[a];

		d_transpose(rdt, e->rd);
		d_mul(rel, rdt, rq);
		d_log(rel, w);

		cost += e->wt * (et[0] * et[0] + et[1] * et[1] + et[2] * et[2]);
		cost += e->wr * (w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	}
	return cost;
}

/* Cholesky solve A x = b; A is overwritten, b receives x. returns 0 ok */
static int cholesky_solve(double *a, int n, double *b)
{
	int i, j, k;
	double s;

	for(j = 0; j < n; j++) {
		for(i = j; i < n; i++) {
			s = a[j * n + i];
			for(k = 0; k < j; k++) {
				s -= a[k * n + i] * a[k * n + j];
			}
			if(i == j) {
				if(s <= 1e-24) return -1;
				a[j * n + j] = sqrt(s);
			} else {
				a[j * n + i] = s / a[j * n + j];
			}
		}
	}
	for(i = 0; i < n; i++) {
		s = b[i];
		for(k = 0; k < i; k++) s -= a[k * n + i] * b[k];
		b[i] = s / a[i * n + i];
	}
	for(i = n - 1; i >= 0; i--) {
		s = b[i];
		for(k = i + 1; k < n; k++) s -= a[i * n + k] * b[k];
		b[i] = s / a[i * n + i];
	}
	return 0;
}

/* Gauss-Newton core. state rr/pp covers all nodes; anchors never move. */
static int solve_gn(struct anm_pose_graph *pg, const struct pg_edge *edges,
		int num_edges, double *rr, double *pp, const struct anm_pg_options *opts,
		double *out_cost, double *out_grad, int *out_iters)
{
	int nfree = 0, k, a, b, row, col;
	int *slot;
	double *hbase, *g, *delta, *hwork, *diag, *r6;
	double grad_inf, lambda, cur_cost, grad_tol;
	int iter = 0;

	grad_tol = opts->grad_tol;

	slot = malloc((size_t)pg->num_nodes * sizeof *slot);
	if(!slot) return ANM_PG_ENOMEM;
	for(k = 0; k < pg->num_nodes; k++) {
		slot[k] = pg->is_anchor[k] ? -1 : nfree++;
	}

	if(nfree == 0) {
		*out_cost = eval_cost(edges, num_edges, rr, pp);
		*out_grad = 0;
		*out_iters = 0;
		free(slot);
		return ANM_PG_OK;
	}

	hbase = calloc((size_t)(6 * nfree) * (6 * nfree), sizeof *hbase);
	g = calloc((size_t)(6 * nfree), sizeof *g);
	delta = malloc((size_t)(6 * nfree) * sizeof *delta);
	hwork = malloc((size_t)(6 * nfree) * (6 * nfree) * sizeof *hwork);
	diag = malloc((size_t)(6 * nfree) * sizeof *diag);
	r6 = malloc(6 * sizeof *r6);
	if(!hbase || !g || !delta || !hwork || !diag || !r6) {
		free(slot); free(hbase); free(g); free(delta); free(hwork);
		free(diag); free(r6);
		return ANM_PG_ENOMEM;
	}

	cur_cost = eval_cost(edges, num_edges, rr, pp);

	for(;;) {
		int dim = 6 * nfree;
		int accepted;

		memset(hbase, 0, (size_t)dim * dim * sizeof *hbase);
		memset(g, 0, (size_t)dim * sizeof *g);

		for(k = 0; k < num_edges; k++) {
			const struct pg_edge *e = edges + k;
			double rit[9], rdt[9], rq[9], rqt[9], kt[9], jri[9], jri_rqt[9];
			double tq[3], d[3], et[3], rel[9], w[3];
			double st = sqrt(e->wt), sr = sqrt(e->wr);
			double je[2][6][6];
			int eidx[2], eid;

			d_transpose(rit, rr + e->i * 9);
			for(a = 0; a < 3; a++) d[a] = pp[e->j * 3 + a] - pp[e->i * 3 + a];
			d_apply(rit, d, tq);
			for(a = 0; a < 3; a++) et[a] = tq[a] - e->td[a];

			d_mul(rq, rit, rr + e->j * 9);
			d_transpose(rdt, e->rd);
			d_mul(rel, rdt, rq);
			d_log(rel, w);

			/* local right perturbation per node: p <- p + R dp,
			 * R <- R Exp(dtheta). translation rows of tq = Ri^T(pj-pi):
			 *   d tq/d dp_i = -I, d tq/d dtheta_i = [tq]_x
			 *   d tq/d dp_j = Rq = Ri^T Rj, d tq/d dtheta_j = 0
			 * rotation rows of w = Log(Rd^T Ri^T Rj):
			 *   d w/d dtheta_j = Jr^-1(w), d w/d dtheta_i = -Jr^-1(w) Rq^T
			 */
			d_transpose(rqt, rq);
			d_jr_inverse(jri, w);
			d_mul(jri_rqt, jri, rqt);
			d_skew(kt, tq);

			memset(je, 0, sizeof je);
			for(row = 0; row < 3; row++) {
				for(col = 0; col < 3; col++) {
					je[0][row][col] = (row == col) ? -st : 0.0;
					je[0][row][3 + col] = st * kt[col * 3 + row];
				}
			}
			for(row = 0; row < 3; row++) {
				for(col = 0; col < 3; col++) {
					je[0][3 + row][3 + col] = -sr * jri_rqt[col * 3 + row];
				}
			}
			for(row = 0; row < 3; row++) {
				for(col = 0; col < 3; col++) {
					je[1][row][col] = st * rq[col * 3 + row];
					je[1][3 + row][3 + col] = sr * jri[col * 3 + row];
				}
			}
			for(a = 0; a < 3; a++) {
				r6[a] = st * et[a];
				r6[3 + a] = sr * w[a];
			}
			eidx[0] = slot[e->i];
			eidx[1] = slot[e->j];

			for(eid = 0; eid < 2; eid++) {
				int va = eidx[eid];
				if(va < 0) continue;
				for(row = 0; row < 6; row++) {
					double gr = 0;
					for(a = 0; a < 6; a++) gr += je[eid][a][row] * r6[a];
					g[va * 6 + row] += gr;
				}
			}
			for(a = 0; a < 2; a++) {
				int va = eidx[a];
				if(va < 0) continue;
				for(b = 0; b < 2; b++) {
					int vb = eidx[b];
					if(vb < 0) continue;
					for(row = 0; row < 6; row++) {
						for(col = 0; col < 6; col++) {
							double s = 0;
							int q;
							for(q = 0; q < 6; q++) {
								s += je[a][q][row] * je[b][q][col];
							}
							hbase[(vb * 6 + col) * dim + (va * 6 + row)] += s;
						}
					}
				}
			}
		}

		/* g accumulates J^T r; the true objective gradient dC/dx is 2 J^T r.
		 * The factor 2 cancels between the two sides of the Gauss-Newton
		 * system, so g is used directly for the step, while the reported and
		 * tolerance-checked norm is the full gradient.
		 */
		grad_inf = 0;
		for(k = 0; k < dim; k++) {
			double av = 2.0 * fabs(g[k]);
			if(av > grad_inf) grad_inf = av;
		}
		if(grad_inf <= grad_tol) {
			*out_cost = cur_cost;
			*out_grad = grad_inf;
			*out_iters = iter;
			free(slot); free(hbase); free(g); free(delta);
			free(hwork); free(diag); free(r6);
			return ANM_PG_OK;
		}
		if(iter >= opts->max_iter) break;

		for(k = 0; k < dim; k++) diag[k] = hbase[k * dim + k];
		lambda = 0.0;
		accepted = 0;

		for(b = 0; b < MAX_LAMBDA_TRY; b++) {
			double alpha, base_cost = cur_cost;

			memcpy(hwork, hbase, (size_t)dim * dim * sizeof *hwork);
			if(lambda > 0.0) {
				for(k = 0; k < dim; k++) {
					hwork[k * dim + k] += lambda * (diag[k] > 0 ? diag[k] : 1.0);
				}
			}
			memcpy(delta, g, (size_t)dim * sizeof *delta);
			if(cholesky_solve(hwork, dim, delta) == 0) {
				for(k = 0; k < dim; k++) delta[k] = -delta[k];

				alpha = 1.0;
				for(a = 0; a < 60; a++) {
					double cand;
					for(k = 0; k < pg->num_nodes; k++) {
						int vs = slot[k];
						double dp[3], dth[3], world_dp[3], er[9], nr[9];
						if(vs < 0) continue;
						for(col = 0; col < 3; col++) {
							dp[col] = alpha * delta[vs * 6 + col];
							dth[col] = alpha * delta[vs * 6 + 3 + col];
						}
						/* local perturbation: p <- p + R dp */
						d_apply(rr + k * 9, dp, world_dp);
						for(col = 0; col < 3; col++) {
							pp[k * 3 + col] += world_dp[col];
						}
						d_exp(er, dth);
						d_mul(nr, rr + k * 9, er);
						memcpy(rr + k * 9, nr, sizeof nr);
					}

					cand = eval_cost(edges, num_edges, rr, pp);
					if(cand < base_cost - 1e-18) {
						cur_cost = cand;
						accepted = 1;
						break;
					}
					/* roll back this trial step */
					for(k = 0; k < pg->num_nodes; k++) {
						int vs = slot[k];
						double dp[3], dth[3], world_dp[3], er[9], nr[9];
						if(vs < 0) continue;
						for(col = 0; col < 3; col++) {
							dp[col] = -alpha * delta[vs * 6 + col];
							dth[col] = -alpha * delta[vs * 6 + 3 + col];
						}
						/* undo rotation first, then translate with old R */
						d_exp(er, dth);
						d_mul(nr, rr + k * 9, er);
						memcpy(rr + k * 9, nr, sizeof nr);
						d_apply(rr + k * 9, dp, world_dp);
						for(col = 0; col < 3; col++) {
							pp[k * 3 + col] += world_dp[col];
						}
					}
					alpha *= 0.5;
				}
			}

			if(accepted) break;
			lambda = lambda > 0.0 ? lambda * 10.0 : 1e-12;
		}

		if(!accepted) break;
		iter++;
	}

	*out_cost = cur_cost;
	*out_grad = grad_inf;
	*out_iters = iter;
	free(slot); free(hbase); free(g); free(delta);
	free(hwork); free(diag); free(r6);
	return ANM_PG_ECONVERGE;
}

/* ---------- public API ---------- */

struct anm_pose_graph *anm_pg_create(struct anm_node **nodes, int num_nodes,
		struct anm_node **anchors, int num_anchors,
		float parent_wt, float parent_wr)
{
	struct anm_pose_graph *pg;
	int i, j;

	if(!nodes || num_nodes <= 0 || !anchors || num_anchors <= 0) return 0;
	if(!isfinite(parent_wt) || !isfinite(parent_wr) ||
			parent_wt <= 0.0f || parent_wr <= 0.0f) {
		return 0;
	}

	if(!(pg = calloc(1, sizeof *pg))) return 0;
	pg->nodes = malloc((size_t)num_nodes * sizeof *pg->nodes);
	pg->is_anchor = calloc((size_t)num_nodes, sizeof *pg->is_anchor);
	pg->owner_mark = calloc((size_t)num_nodes, sizeof *pg->owner_mark);
	if(!pg->nodes || !pg->is_anchor || !pg->owner_mark) {
		free(pg->nodes); free(pg->is_anchor); free(pg->owner_mark);
		free(pg);
		return 0;
	}
	pg->num_nodes = num_nodes;
	pg->parent_wt = parent_wt;
	pg->parent_wr = parent_wr;

	for(i = 0; i < num_nodes; i++) {
		if(!nodes[i]) goto fail;
		pg->nodes[i] = nodes[i];
		for(j = 0; j < i; j++) {
			if(nodes[j] == nodes[i]) goto fail;	/* duplicates */
		}
		if(nodes[i]->pg_owner) goto fail;		/* one graph per node */
	}

	/* ancestor closure: every parent of a member is itself a member */
	for(i = 0; i < num_nodes; i++) {
		struct anm_node *p = nodes[i]->parent;
		if(p && find_node(pg, p) < 0) goto fail;
	}

	for(i = 0; i < num_anchors; i++) {
		int idx;
		if(!anchors[i]) goto fail;
		idx = find_node(pg, anchors[i]);
		if(idx < 0) goto fail;
		pg->is_anchor[idx] = 1;
	}

	/* Note: the "every connected component has an anchor" rule is enforced
	 * when an observation table is installed and again at solve time, so a
	 * table connecting an otherwise anchorless root can still be accepted.
	 */
	for(i = 0; i < num_nodes; i++) {
		nodes[i]->pg_owner = pg;
		pg->owner_mark[i] = 1;
	}
	return pg;

fail:
	free(pg->nodes);
	free(pg->is_anchor);
	free(pg->owner_mark);
	free(pg);
	return 0;
}

void anm_pg_destroy(struct anm_pose_graph *pg)
{
	int i;
	if(!pg) return;

	anm_pg_clear_overrides(pg);
	for(i = 0; i < pg->num_nodes; i++) {
		if(pg->owner_mark[i] && pg->nodes[i]->pg_owner == pg) {
			pg->nodes[i]->pg_owner = 0;
		}
	}
	free(pg->nodes);
	free(pg->is_anchor);
	free(pg->owner_mark);
	free(pg->obs);
	free(pg->pub_w);
	free(pg);
}

int anm_pg_replace_observations(struct anm_pose_graph *pg,
		const struct anm_pg_observation *obs, int num_obs)
{
	struct pg_edge *edges;
	struct anm_pg_observation *table;
	int nparent = 0, i, rc, cap;

	if(!pg) return ANM_PG_EINVAL;
	if(num_obs < 0 || (num_obs > 0 && !obs)) return ANM_PG_EINVAL;

	/* validate the whole candidate table first */
	for(i = 0; i < num_obs; i++) {
		if(!obs[i].node_i || !obs[i].node_j) return ANM_PG_ENODE;
		if(find_node(pg, obs[i].node_i) < 0 ||
				find_node(pg, obs[i].node_j) < 0) {
			return ANM_PG_ENODE;
		}
		if(!isfinite(obs[i].wt) || !isfinite(obs[i].wr) ||
				obs[i].wt <= 0.0f || obs[i].wr <= 0.0f) {
			return ANM_PG_EWEIGHT;
		}
		if(!valid_rigid_m4(obs[i].target)) return ANM_PG_ERIGID;
	}

	/* assemble parent + extra edges for the connectivity check */
	cap = pg->num_nodes + (num_obs > 0 ? num_obs : 0);
	edges = malloc((size_t)cap * sizeof *edges);
	if(!edges) return ANM_PG_ENOMEM;
	for(i = 0; i < pg->num_nodes; i++) {
		if(pg->nodes[i]->parent) {
			int pidx = find_node(pg, pg->nodes[i]->parent);
			edges[nparent].i = pidx;
			edges[nparent].j = i;
			nparent++;
		}
	}
	for(i = 0; i < num_obs; i++) {
		struct pg_edge *e = edges + nparent + i;
		e->i = find_node(pg, obs[i].node_i);
		e->j = find_node(pg, obs[i].node_j);
		m4_to_rigid(obs[i].target, e->rd, e->td);
		e->wt = obs[i].wt;
		e->wr = obs[i].wr;
	}

	rc = check_components(pg, edges, nparent + num_obs);
	free(edges);
	if(rc != ANM_PG_OK) return rc;

	/* all checks passed: take the table (struct copy keeps the matrix) */
	if(!(table = malloc((size_t)(num_obs > 0 ? num_obs : 1) * sizeof *table))) {
		return ANM_PG_ENOMEM;
	}
	for(i = 0; i < num_obs; i++) {
		table[i] = obs[i];
	}
	free(pg->obs);
	pg->obs = table;
	pg->num_obs = num_obs;
	return ANM_PG_OK;
}

int anm_pg_clear_overrides(struct anm_pose_graph *pg)
{
	if(!pg) return ANM_PG_EINVAL;
	if(pg->has_pub) {
		pg->has_pub = 0;
		pg->pub_time = ANM_TIME_INVAL;
		invalidate_member_caches(pg);
	}
	return ANM_PG_OK;
}

int anm_pg_solve(struct anm_pose_graph *pg, anm_time_t tm,
		const struct anm_pg_options *opts)
{
	struct anm_pg_options local_opts;
	struct pg_edge *edges;
	int num_edges = 0, rc, i;
	double *rr, *pp, *locals_r, *locals_t;
	float *locals_m;
	double final_cost, final_grad;
	int final_iters;

	if(!pg) return ANM_PG_EINVAL;
	if(opts) {
		local_opts = *opts;
		if(!isfinite(local_opts.grad_tol) || local_opts.grad_tol <= 0.0f ||
				local_opts.max_iter < 0) {
			return ANM_PG_EINVAL;
		}
	} else {
		local_opts.grad_tol = DEFAULT_GTOL;
		local_opts.max_iter = DEFAULT_MAXIT;
	}

	/* every attempt starts from a clean report; a failed attempt never
	 * leaves the previous solve's cost/iteration fields behind
	 */
	memset(&pg->report, 0, sizeof pg->report);

	/* reject active high-level automatic transitions up-front */
	for(i = 0; i < pg->num_nodes; i++) {
		if(active_transition(pg->nodes[i])) {
			pg->report.status = ANM_PG_ETRANSITION;
			return ANM_PG_ETRANSITION;
		}
	}

	rr = calloc((size_t)pg->num_nodes * 9, sizeof *rr);
	pp = calloc((size_t)pg->num_nodes * 3, sizeof *pp);
	locals_m = malloc((size_t)pg->num_nodes * 16 * sizeof *locals_m);
	locals_r = calloc((size_t)pg->num_nodes * 9, sizeof *locals_r);
	locals_t = calloc((size_t)pg->num_nodes * 3, sizeof *locals_t);
	if(!rr || !pp || !locals_m || !locals_r || !locals_t) {
		pg->report.status = ANM_PG_ENOMEM;
		free(rr); free(pp); free(locals_m); free(locals_r); free(locals_t);
		return ANM_PG_ENOMEM;
	}

	/* independently sample the original animation through the track APIs
	 * only: matrix caches and previously published overrides never feed back
	 */
	for(i = 0; i < pg->num_nodes; i++) {
		rc = raw_local_matrix(pg->nodes[i], tm, locals_m + i * 16);
		if(rc == -1) rc = ANM_PG_ESCALE;
		if(rc == 0) rc = ANM_PG_ERIGID;
		if(rc != 1) {
			pg->report.status = rc;
			free(rr); free(pp); free(locals_m); free(locals_r); free(locals_t);
			return rc;
		}
		m4_to_rigid(locals_m + i * 16, locals_r + i * 9, locals_t + i * 3);
	}

	/* compose world poses along ancestor chains (roots first) */
	for(i = 0; i < pg->num_nodes; i++) {
		double wm_r[9], wm_t[3], lr[9], lt[3], tmp_t[3];
		struct anm_node *cur;

		d_identity(wm_r);
		wm_t[0] = wm_t[1] = wm_t[2] = 0.0;
		cur = pg->nodes[i];
		while(cur) {
			int cidx = find_node(pg, cur);
			if(cidx < 0) {
				pg->report.status = ANM_PG_ENODE;
				free(rr); free(pp); free(locals_m); free(locals_r); free(locals_t);
				return ANM_PG_ENODE;
			}
			memcpy(lr, locals_r + cidx * 9, sizeof lr);
			memcpy(lt, locals_t + cidx * 3, sizeof lt);
			/* wm = local * wm: rotation Rl Rw, translation Rl tw + tl */
			d_apply(lr, wm_t, tmp_t);
			wm_t[0] = tmp_t[0] + lt[0];
			wm_t[1] = tmp_t[1] + lt[1];
			wm_t[2] = tmp_t[2] + lt[2];
			d_mul(wm_r, lr, wm_r);
			cur = cur->parent;
		}
		memcpy(rr + i * 9, wm_r, sizeof wm_r);
		memcpy(pp + i * 3, wm_t, sizeof wm_t);
	}
	free(locals_m);
	free(locals_r);
	free(locals_t);

	rc = build_edges(pg, tm, &edges, &num_edges);
	if(rc != ANM_PG_OK) {
		free(rr); free(pp);
		pg->report.status = rc;
		return rc;
	}

	rc = check_components(pg, edges, num_edges);
	if(rc != ANM_PG_OK) {
		free(edges); free(rr); free(pp);
		pg->report.status = rc;
		return rc;
	}

	rc = solve_gn(pg, edges, num_edges, rr, pp, &local_opts,
			&final_cost, &final_grad, &final_iters);

	pg->report.cost = (float)final_cost;
	pg->report.grad_inf = (float)final_grad;
	pg->report.iterations = final_iters;
	pg->report.status = rc;

	if(rc != ANM_PG_OK) {
		/* no publication: previous result, tracks and caches stay intact */
		free(edges); free(rr); free(pp);
		return rc;
	}

	/* atomic publication of this time slice */
	{
		float *newpub = malloc((size_t)pg->num_nodes * 16 * sizeof *newpub);
		if(!newpub) {
			free(edges); free(rr); free(pp);
			pg->report.status = ANM_PG_ENOMEM;
			return ANM_PG_ENOMEM;
		}
		for(i = 0; i < pg->num_nodes; i++) {
			rigid_to_m4(newpub + i * 16, rr + i * 9, pp + i * 3);
		}
		free(pg->pub_w);
		pg->pub_w = newpub;
		pg->has_pub = 1;
		pg->pub_time = tm;
		invalidate_member_caches(pg);
	}

	free(edges);
	free(rr);
	free(pp);
	return ANM_PG_OK;
}

int anm_pg_get_report(const struct anm_pose_graph *pg, struct anm_pg_report *rep)
{
	if(!pg || !rep) return ANM_PG_EINVAL;
	*rep = pg->report;
	return ANM_PG_OK;
}

int anm_pg_apply(const struct anm_node *node, anm_time_t tm, float *out)
{
	struct anm_pose_graph *pg;
	int i;

	if(!node) return 0;
	pg = node->pg_owner;
	if(!pg || !pg->has_pub || pg->pub_time != tm) return 0;

	for(i = 0; i < pg->num_nodes; i++) {
		if(pg->nodes[i] == node) {
			if(out) memcpy(out, pg->pub_w + i * 16, 16 * sizeof *out);
			return 1;
		}
	}
	return 0;
}
