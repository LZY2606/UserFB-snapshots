/*
libanim pose graph tests
Copyright (C) 2024 John Tsiombikas <nuclear@member.fsf.org>

These tests exercise the reusable anm_pose_graph soft relative-pose
constraint solver. Objectives and the 6D local-perturbation gradient are
recomputed independently from the returned world poses (the solver report is
never treated as a reference).
*/
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "anim.h"
#include "pose_graph.h"
#include "cgmath/cgmath.h"
#include "task_support/check.h"

#define MAXN 16
#define EPS_MAT 2e-4f
#define EPS_GRAD 1e-4
#define FD_H 1e-4

/* ---------- independent double-precision recomputation helpers ---------- */

struct t_edge {
	int i, j;
	double rd[9], td[3];
	double wt, wr;
};

static void f4_to_rigid(const float *m, double *r, double *t)
{
	if(t) { t[0] = m[12]; t[1] = m[13]; t[2] = m[14]; }
	r[0]=m[0];  r[1]=m[1];  r[2]=m[2];
	r[3]=m[4];  r[4]=m[5];  r[5]=m[6];
	r[6]=m[8];  r[7]=m[9];  r[8]=m[10];
}

static void d_mul3(const double *a, const double *b, double *c)
{
	double res[9];
	int i, j, k;
	for(j = 0; j < 3; j++)
		for(i = 0; i < 3; i++) {
			double s = 0;
			for(k = 0; k < 3; k++) s += a[k*3+i] * b[j*3+k];
			res[j*3+i] = s;
		}
	memcpy(c, res, sizeof res);
}

static void d_tr3(const double *r, double *o)
{
	int i, j;
	for(j = 0; j < 3; j++)
		for(i = 0; i < 3; i++) o[i*3+j] = r[j*3+i];
}

static void d_ap3(const double *r, const double *v, double *o)
{
	int i, j;
	for(i = 0; i < 3; i++) {
		double s = 0;
		for(j = 0; j < 3; j++) s += r[j*3+i] * v[j];
		o[i] = s;
	}
}

static void d_skew3(double *k, const double *v)
{
	k[0]=0;      k[3]=-v[2];  k[6]=v[1];
	k[1]=v[2];   k[4]=0;      k[7]=-v[0];
	k[2]=-v[1];  k[5]=v[0];   k[8]=0;
}

static void d_exp3(double *r, const double *w)
{
	double theta = sqrt(w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
	double k[9], k2[9], s, c, f;
	int i;
	static const double id[9] = {1,0,0, 0,1,0, 0,0,1};
	memcpy(r, id, sizeof id);
	if(theta < 1e-12) return;
	d_skew3(k, w);
	d_mul3(k, k, k2);
	s = sin(theta);
	c = 1.0 - cos(theta);
	f = c / (theta * theta);
	for(i = 0; i < 9; i++) r[i] += (s / theta) * k[i] + f * k2[i];
}

static void d_log3(const double *r, double *w)
{
	double ca = (r[0] + r[4] + r[8] - 1.0) * 0.5;
	double ang, s, f;
	if(ca > 1.0) ca = 1.0;
	if(ca < -1.0) ca = -1.0;
	ang = acos(ca);
	s = sin(ang);
	f = s < 1e-9 ? 0.5 : ang / (2.0 * s);
	w[0] = f * (r[5] - r[7]);
	w[1] = f * (r[6] - r[2]);
	w[2] = f * (r[1] - r[3]);
}

/* objective recomputed from a candidate world pose state */
static double t_cost(int n, const double R[][9], const double p[][3],
		const struct t_edge *es, int ne)
{
	double cost = 0;
	int e, a;
	(void)n;
	for(e = 0; e < ne; e++) {
		double rit[9], rq[9], rel[9], rdt[9], w[3];
		double d[3], tq[3], et[3];
		d_tr3(R[es[e].i], rit);
		d_mul3(rit, R[es[e].j], rq);
		for(a = 0; a < 3; a++) d[a] = p[es[e].j][a] - p[es[e].i][a];
		d_ap3(rit, d, tq);
		for(a = 0; a < 3; a++) et[a] = tq[a] - es[e].td[a];
		d_tr3(es[e].rd, rdt);
		d_mul3(rdt, rq, rel);
		d_log3(rel, w);
		cost += es[e].wt * (et[0]*et[0] + et[1]*et[1] + et[2]*et[2]);
		cost += es[e].wr * (w[0]*w[0] + w[1]*w[1] + w[2]*w[2]);
	}
	return cost;
}

static void extract_state(const float W[][16], int n, double R[][9], double p[][3])
{
	int i;
	for(i = 0; i < n; i++) f4_to_rigid(W[i], R[i], p[i]);
}

/* central-difference inf norm of the gradient in 6D local coordinates */
static double t_fd_grad(int n, const int *free_nodes, int nfree,
		const double R0[][9], const double p0[][3],
		const struct t_edge *es, int ne)
{
	double ginf = 0;
	int f, c, i;

	for(f = 0; f < nfree; f++) {
		int node = free_nodes[f];
		for(c = 0; c < 6; c++) {
			double Rp[MAXN][9], Pp[MAXN][3], Rm[MAXN][9], Pm[MAXN][3];
			double cp, cm, g;
			memcpy(Rp, R0, sizeof(double) * 9 * n);
			memcpy(Rm, R0, sizeof(double) * 9 * n);
			memcpy(Pp, p0, sizeof(double) * 3 * n);
			memcpy(Pm, p0, sizeof(double) * 3 * n);
			if(c < 3) {
				double v[3] = {0,0,0}, wp[3], wm[3];
				v[c] = FD_H;
				d_ap3(R0[node], v, wp);
				v[c] = -FD_H;
				d_ap3(R0[node], v, wm);
				for(i = 0; i < 3; i++) {
					Pp[node][i] = p0[node][i] + wp[i];
					Pm[node][i] = p0[node][i] + wm[i];
				}
			} else {
				double vp[3] = {0,0,0}, vm[3] = {0,0,0}, ep[9], em[9];
				vp[c-3] = FD_H;
				vm[c-3] = -FD_H;
				d_exp3(ep, vp);
				d_exp3(em, vm);
				d_mul3(R0[node], ep, Rp[node]);
				d_mul3(R0[node], em, Rm[node]);
			}
			cp = t_cost(n, Rp, Pp, es, ne);
			cm = t_cost(n, Rm, Pm, es, ne);
			g = (cp - cm) / (2.0 * FD_H);
			if(fabs(g) > ginf) ginf = fabs(g);
		}
	}
	return ginf;
}

static float m4_maxdiff(const float *a, const float *b)
{
	float d = 0;
	int i;
	for(i = 0; i < 16; i++) {
		float e = fabsf(a[i] - b[i]);
		if(e > d) d = e;
	}
	return d;
}

static int m4_near(const float *a, const float *b, float tol)
{
	return m4_maxdiff(a, b) <= tol;
}

static void rigid_target(float *m, int axis, float ang,
		float tx, float ty, float tz)
{
	cgm_midentity(m);
	if(ang != 0.0f) cgm_mrotation_axis(m, axis, ang);
	m[12] = tx; m[13] = ty; m[14] = tz;
}

static void edge_from_m4(struct t_edge *e, int i, int j,
		const float *m, float wt, float wr)
{
	e->i = i; e->j = j;
	e->wt = wt; e->wr = wr;
	f4_to_rigid(m, e->rd, e->td);
}

static void world_state(struct anm_node **nodes, int n, anm_time_t tm,
		float W[][16])
{
	int i;
	for(i = 0; i < n; i++) anm_get_matrix(nodes[i], W[i], tm);
}

/* ============================ objective ============================ */

/* weighted translation-only closed loop with an independent analytic optimum */
static int obj_translation_loop(void)
{
	struct anm_node root, a, b;
	struct anm_node *nodes[3];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-7f, 100};
	struct anm_pg_report rep;
	float Worig[3][16], W[3][16], expect_a[16], expect_b[16];
	float la[16], lb[16];
	double R[3][9], p[3][3];
	struct t_edge es[3];
	int ne = 0, free_nodes[2] = {1, 2};
	double c0, c1, fdg;

	anm_init_node(&root); anm_init_node(&a); anm_init_node(&b);
	anm_link_node(&root, &a);
	anm_link_node(&a, &b);
	anm_set_position3f(&a, 1, 0, 0, 0);
	anm_set_position3f(&b, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&a; nodes[2]=&b;
	anchors[0]=&root;

	anm_get_node_matrix(&a, la, 0);
	anm_get_node_matrix(&b, lb, 0);
	world_state(nodes, 3, 0, Worig);

	pg = anm_pg_create(nodes, 3, anchors, 1, 1.0f, 1.0f);
	TASK_CHECK(pg != 0);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	TASK_CHECK(anm_pg_replace_observations(pg, &ob, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	TASK_CHECK(anm_pg_get_report(pg, &rep) == ANM_PG_OK);
	TASK_CHECK(rep.status == ANM_PG_OK && rep.iterations >= 1);

	world_state(nodes, 3, 0, W);
	/* analytic optimum: xa = 7/3, xb = 14/3 */
	cgm_midentity(expect_a); expect_a[12] = 7.0f / 3.0f;
	cgm_midentity(expect_b); expect_b[12] = 14.0f / 3.0f;
	TASK_CHECK(m4_near(W[1], expect_a, EPS_MAT));
	TASK_CHECK(m4_near(W[2], expect_b, EPS_MAT));

	edge_from_m4(&es[ne], 0, 1, la, 1, 1); ne++;
	edge_from_m4(&es[ne], 1, 2, lb, 1, 1); ne++;
	edge_from_m4(&es[ne], 0, 2, ob.target, 4, 1); ne++;
	extract_state(W, 3, R, p);
	c0 = t_cost(3, R, p, es, ne);
	extract_state(Worig, 3, R, p);
	c1 = t_cost(3, R, p, es, ne);
	extract_state(W, 3, R, p);
	fdg = t_fd_grad(3, free_nodes, 2, R, p, es, ne);
	TASK_CHECK(fabs(c0 - rep.cost) <= 1e-5 * (1.0 + c0));
	TASK_CHECK(c0 < c1);
	TASK_CHECK(fdg <= EPS_GRAD);

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&a); anm_destroy_node(&root);
	return 0;
}

/* noncommuting rotation+translation assembly on a forked tree */
static int obj_noncommuting(void)
{
	struct anm_node r, x, y;
	struct anm_node *nodes[3];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 200};
	struct anm_pg_report rep;
	float Worig[3][16], W[3][16], lx[16], ly[16];
	double R[3][9], p[3][3];
	struct t_edge es[3];
	int ne = 0, free_nodes[2] = {1, 2};
	double c0, c1, fdg;

	anm_init_node(&r); anm_init_node(&x); anm_init_node(&y);
	anm_link_node(&r, &x);
	anm_link_node(&r, &y);
	anm_set_position3f(&x, 1, 0, 0, 0);
	anm_set_rotation_axis(&x, 0.3f, 0, 0, 1, 0);
	anm_set_position3f(&y, 0, 1, 0, 0);
	anm_set_rotation_axis(&y, -0.2f, 1, 0, 0, 0);
	nodes[0]=&r; nodes[1]=&x; nodes[2]=&y;
	anchors[0]=&r;

	anm_get_node_matrix(&x, lx, 0);
	anm_get_node_matrix(&y, ly, 0);
	world_state(nodes, 3, 0, Worig);

	pg = anm_pg_create(nodes, 3, anchors, 1, 2.0f, 2.0f);
	TASK_CHECK(pg != 0);
	ob.node_i=&x; ob.node_j=&y; ob.wt=3.0f; ob.wr=5.0f;
	rigid_target(ob.target, 0, 0.4f, 0.5f, 0.5f, 0.2f);
	TASK_CHECK(anm_pg_replace_observations(pg, &ob, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_pg_get_report(pg, &rep);
	TASK_CHECK(rep.status == ANM_PG_OK);

	world_state(nodes, 3, 0, W);
	edge_from_m4(&es[ne], 0, 1, lx, 2, 2); ne++;
	edge_from_m4(&es[ne], 0, 2, ly, 2, 2); ne++;
	edge_from_m4(&es[ne], 1, 2, ob.target, 3, 5); ne++;
	extract_state(W, 3, R, p);
	c0 = t_cost(3, R, p, es, ne);
	extract_state(Worig, 3, R, p);
	c1 = t_cost(3, R, p, es, ne);
	extract_state(W, 3, R, p);
	fdg = t_fd_grad(3, free_nodes, 2, R, p, es, ne);
	TASK_CHECK(fabs(c0 - rep.cost) <= 1e-5 * (1.0 + c0));
	TASK_CHECK(c0 < c1);
	TASK_CHECK(fdg <= EPS_GRAD);
	/* rotations really moved away from the sampled pose */
	TASK_CHECK(m4_maxdiff(W[1], Worig[1]) > 1e-3f ||
			m4_maxdiff(W[2], Worig[2]) > 1e-3f);

	anm_pg_destroy(pg);
	anm_destroy_node(&y); anm_destroy_node(&x); anm_destroy_node(&r);
	return 0;
}

/* nonzero pivots with a consistent closed loop: solution must reproduce the
 * independently composed pivot-aware world matrices exactly
 */
static int obj_pivot_loop(void)
{
	struct anm_node root, a, b;
	struct anm_node *nodes[3];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};
	struct anm_pg_report rep;
	float la[16], lb[16], expect_wb[16], W[3][16];
	double R[3][9], p[3][3];
	struct t_edge es[3];
	int ne = 0, free_nodes[2] = {1, 2};
	double fdg, c0;

	anm_init_node(&root); anm_init_node(&a); anm_init_node(&b);
	anm_link_node(&root, &a);
	anm_link_node(&a, &b);
	anm_set_pivot(&a, 0, 1, 0);
	anm_set_rotation_axis(&a, 1.5707963f, 0, 0, 1, 0);
	anm_set_pivot(&b, 0.5f, 0.5f, 0);
	anm_set_position3f(&b, 0.3f, 0, 0, 0);
	anm_set_rotation_axis(&b, 0.6f, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&a; nodes[2]=&b;
	anchors[0]=&root;

	anm_get_node_matrix(&a, la, 0);
	anm_get_node_matrix(&b, lb, 0);
	/* library convention: child world = local_child * parent_world */
	cgm_mcopy(expect_wb, lb);
	cgm_mmul(expect_wb, la);

	pg = anm_pg_create(nodes, 3, anchors, 1, 2.0f, 2.0f);
	TASK_CHECK(pg != 0);
	ob.node_i=&root; ob.node_j=&b; ob.wt=2.0f; ob.wr=2.0f;
	cgm_mcopy(ob.target, expect_wb);
	TASK_CHECK(anm_pg_replace_observations(pg, &ob, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_pg_get_report(pg, &rep);
	TASK_CHECK(rep.status == ANM_PG_OK);
	TASK_CHECK(rep.cost <= 1e-12f);

	world_state(nodes, 3, 0, W);
	TASK_CHECK(m4_near(W[1], la, EPS_MAT));
	TASK_CHECK(m4_near(W[2], expect_wb, EPS_MAT));

	edge_from_m4(&es[ne], 0, 1, la, 2, 2); ne++;
	edge_from_m4(&es[ne], 1, 2, lb, 2, 2); ne++;
	edge_from_m4(&es[ne], 0, 2, expect_wb, 2, 2); ne++;
	extract_state(W, 3, R, p);
	c0 = t_cost(3, R, p, es, ne);
	fdg = t_fd_grad(3, free_nodes, 2, R, p, es, ne);
	TASK_CHECK(c0 <= 1e-10);
	TASK_CHECK(fdg <= EPS_GRAD);

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&a); anm_destroy_node(&root);
	return 0;
}

/* forked tree with a cross-branch observation between sibling leaves */
static int obj_fork_tree(void)
{
	struct anm_node r, a, b, c;
	struct anm_node *nodes[4];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 200};
	struct anm_pg_report rep;
	float Worig[4][16], W[4][16], la[16], lb[16], lc[16];
	double R[4][9], p[4][3];
	struct t_edge es[4];
	int ne = 0, free_nodes[3] = {1, 2, 3};
	double c0, c1, fdg;

	anm_init_node(&r); anm_init_node(&a); anm_init_node(&b); anm_init_node(&c);
	anm_link_node(&r, &a);
	anm_link_node(&a, &b);
	anm_link_node(&a, &c);
	anm_set_position3f(&a, 1, 0, 0, 0);
	anm_set_position3f(&b, 0, 1, 0, 0);
	anm_set_position3f(&c, 0, 0, 1, 0);
	nodes[0]=&r; nodes[1]=&a; nodes[2]=&b; nodes[3]=&c;
	anchors[0]=&r;

	anm_get_node_matrix(&a, la, 0);
	anm_get_node_matrix(&b, lb, 0);
	anm_get_node_matrix(&c, lc, 0);
	world_state(nodes, 4, 0, Worig);

	pg = anm_pg_create(nodes, 4, anchors, 1, 1.5f, 1.5f);
	TASK_CHECK(pg != 0);
	ob.node_i=&b; ob.node_j=&c; ob.wt=2.5f; ob.wr=1.0f;
	rigid_target(ob.target, 2, 0.35f, 0.2f, -0.1f, 0.5f);
	TASK_CHECK(anm_pg_replace_observations(pg, &ob, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_pg_get_report(pg, &rep);
	TASK_CHECK(rep.status == ANM_PG_OK);

	world_state(nodes, 4, 0, W);
	edge_from_m4(&es[ne], 0, 1, la, 1.5f, 1.5f); ne++;
	edge_from_m4(&es[ne], 1, 2, lb, 1.5f, 1.5f); ne++;
	edge_from_m4(&es[ne], 1, 3, lc, 1.5f, 1.5f); ne++;
	edge_from_m4(&es[ne], 2, 3, ob.target, 2.5f, 1.0f); ne++;
	extract_state(W, 4, R, p);
	c0 = t_cost(4, R, p, es, ne);
	extract_state(Worig, 4, R, p);
	c1 = t_cost(4, R, p, es, ne);
	extract_state(W, 4, R, p);
	fdg = t_fd_grad(4, free_nodes, 3, R, p, es, ne);
	TASK_CHECK(fabs(c0 - rep.cost) <= 1e-5 * (1.0 + c0));
	TASK_CHECK(c0 < c1);
	TASK_CHECK(fdg <= EPS_GRAD);
	TASK_CHECK(m4_maxdiff(W[2], Worig[2]) > 1e-3f);

	anm_pg_destroy(pg);
	anm_destroy_node(&c); anm_destroy_node(&b);
	anm_destroy_node(&a); anm_destroy_node(&r);
	return 0;
}

/* fully consistent constraints: cost vanishes and poses match an
 * independently composed ground truth, needing no iteration
 */
static int obj_consistent_groundtruth(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};
	struct anm_pg_report rep;
	float lb[16], wb[16], W[2][16];

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, -0.3f, 0.2f, 0.9f, 0);
	anm_set_rotation_axis(&b, -0.4f, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	/* the extra observation restates the parent edge exactly, so the
	 * sampled initial point is a zero-cost stationary point
	 */
	anm_get_node_matrix(&b, lb, 0);
	anm_get_matrix(&b, wb, 0);

	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	TASK_CHECK(pg != 0);
	ob.node_i=&root; ob.node_j=&b; ob.wt=1.0f; ob.wr=1.0f;
	cgm_mcopy(ob.target, lb);
	TASK_CHECK(anm_pg_replace_observations(pg, &ob, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_pg_get_report(pg, &rep);
	TASK_CHECK(rep.status == ANM_PG_OK);
	TASK_CHECK(rep.iterations == 0);
	TASK_CHECK(rep.cost <= 1e-12f);
	TASK_CHECK(rep.grad_inf <= opts.grad_tol);

	world_state(nodes, 2, 0, W);
	TASK_CHECK(m4_near(W[1], wb, EPS_MAT));

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* ============================ publication ============================ */

/* all matrix entry points consume the same published world pose, tracks are
 * not modified, inverse caches stay consistent
 */
static int pub_world_apis(void)
{
	struct anm_node root, a, b;
	struct anm_node *nodes[3];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};
	float pos_before[3], pos_after[3];
	float W[3][16], inv[16], prod[16], ident[16];
	int i, j, k;

	anm_init_node(&root); anm_init_node(&a); anm_init_node(&b);
	anm_link_node(&root, &a);
	anm_link_node(&a, &b);
	anm_set_position3f(&a, 1, 0, 0, 0);
	anm_set_position3f(&b, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&a; nodes[2]=&b;
	anchors[0]=&root;

	anm_get_node_position(&b, pos_before, 0);

	pg = anm_pg_create(nodes, 3, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	anm_pg_replace_observations(pg, &ob, 1);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);

	anm_get_matrix(&b, W[2], 0);
	anm_get_matrix(&a, W[1], 0);
	anm_get_matrix(&root, W[0], 0);

	/* anm_eval writes the same world matrices into node.matrix */
	anm_eval(&root, 0);
	TASK_CHECK(m4_near(root.matrix, W[0], 0.0f));
	TASK_CHECK(m4_near(a.matrix, W[1], 0.0f));
	TASK_CHECK(m4_near(b.matrix, W[2], 0.0f));

	/* inverse cache: W * Winv == identity */
	anm_get_inv_matrix(&b, inv, 0);
	cgm_mcopy(prod, W[2]);
	cgm_mmul(prod, inv);
	cgm_midentity(ident);
	TASK_CHECK(m4_near(prod, ident, EPS_MAT));

	/* tracks never receive corrections */
	anm_get_node_position(&b, pos_after, 0);
	TASK_CHECK(pos_before[0] == pos_after[0] &&
			pos_before[1] == pos_after[1] && pos_before[2] == pos_after[2]);

	/* every entry of the published matrix is finite */
	for(i = 0; i < 3; i++)
		for(k = 0; k < 16; k++) TASK_CHECK(isfinite(W[i][k]));
	(void)j;

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&a); anm_destroy_node(&root);
	return 0;
}

/* anm_get_node_matrix returns the local pose relative to the final parent */
static int pub_node_local(void)
{
	struct anm_node root, a, b;
	struct anm_node *nodes[3];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};
	float Wr[16], Wa[16], Wb[16], local[16], expect[16], invp[16];

	anm_init_node(&root); anm_init_node(&a); anm_init_node(&b);
	anm_link_node(&root, &a);
	anm_link_node(&a, &b);
	anm_set_position3f(&a, 1, 0, 0, 0);
	anm_set_position3f(&b, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&a; nodes[2]=&b;
	anchors[0]=&root;

	pg = anm_pg_create(nodes, 3, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	anm_pg_replace_observations(pg, &ob, 1);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);

	anm_get_matrix(&root, Wr, 0);
	anm_get_matrix(&a, Wa, 0);
	anm_get_matrix(&b, Wb, 0);
	anm_get_node_matrix(&b, local, 0);
	cgm_mcopy(invp, Wa);
	cgm_minverse(invp);
	/* expect = inv(Wa) * Wb; cgm_mmul(a,b) computes b*a */
	cgm_mcopy(expect, Wb);
	cgm_mmul(expect, invp);
	TASK_CHECK(m4_near(local, expect, EPS_MAT));

	anm_get_node_matrix(&root, local, 0);
	TASK_CHECK(m4_near(local, Wr, EPS_MAT)); /* root: local == world */

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&a); anm_destroy_node(&root);
	return 0;
}

/* overrides apply only at the published time; other times stay original */
static int pub_only_published_time(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};
	float orig0[16], orig777[16], now0[16], now777[16];

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, 1, 0, 0, 0);
	anm_set_position3f(&b, 3, 0, 0, 1000);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	anm_get_matrix(&b, orig0, 0);
	anm_get_matrix(&b, orig777, 777);

	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	anm_pg_replace_observations(pg, &ob, 1);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);

	anm_get_matrix(&b, now0, 0);
	anm_get_matrix(&b, now777, 777);
	TASK_CHECK(m4_maxdiff(now0, orig0) > 1e-3f);
	TASK_CHECK(m4_near(now777, orig777, 0.0f));

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* independent resampling: reverse-order queries, repeated solves at the same
 * time are bit-stable, and nothing accumulates between solves
 */
static int pub_resample_reverse(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-9f, 200};
	float w0a[16], w0b[16], w500[16], w0c[16], w500b[16];

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, 1, 0, 0, 0);
	anm_set_position3f(&b, 2, 0, 0, 1000);
	anm_set_node_animation_offset(&b, 100, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	anm_pg_replace_observations(pg, &ob, 1);

	TASK_CHECK(anm_pg_solve(pg, 500, &opts) == ANM_PG_OK);
	anm_get_matrix(&b, w500, 500);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_get_matrix(&b, w0a, 0);
	/* reverse order query: published time 0 still active, 500 reverted */
	TASK_CHECK(anm_pg_solve(pg, 500, &opts) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_get_matrix(&b, w0b, 0);
	anm_get_matrix(&b, w500b, 500);
	TASK_CHECK(m4_near(w0a, w0b, 0.0f));
	/* re-solving the same time from scratch reproduces the same numbers */
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_get_matrix(&b, w0c, 0);
	TASK_CHECK(m4_near(w0a, w0c, 0.0f));
	(void)w500b;

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* replacing the observation table leaves the previously published pose alone
 * until a new solve happens
 */
static int pub_replace_keeps_published(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob1, ob2;
	struct anm_pg_options opts = {1e-8f, 100};
	float w_a[16], w_b[16], w_c[16];

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	ob1.node_i=&root; ob1.node_j=&b; ob1.wt=4.0f; ob1.wr=1.0f;
	cgm_midentity(ob1.target); ob1.target[12]=5.0f;
	ob2 = ob1;
	ob2.target[12] = 8.0f;
	anm_pg_replace_observations(pg, &ob1, 1);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_get_matrix(&b, w_a, 0);

	TASK_CHECK(anm_pg_replace_observations(pg, &ob2, 1) == ANM_PG_OK);
	anm_get_matrix(&b, w_b, 0);
	TASK_CHECK(m4_near(w_a, w_b, 0.0f)); /* table swap alone changes nothing */

	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_get_matrix(&b, w_c, 0);
	TASK_CHECK(m4_maxdiff(w_a, w_c) > 1e-3f);

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* an already stationary initial point converges with zero iterations */
static int pub_zero_iterations(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};
	struct anm_pg_report rep;
	float lb[16], W[16];

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, 2.5f, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	anm_get_node_matrix(&b, lb, 0);
	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=3.0f; ob.wr=3.0f;
	cgm_mcopy(ob.target, lb); /* exactly satisfied at the sampled pose */
	anm_pg_replace_observations(pg, &ob, 1);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_pg_get_report(pg, &rep);
	TASK_CHECK(rep.iterations == 0);
	TASK_CHECK(rep.grad_inf <= opts.grad_tol);
	anm_get_matrix(&b, W, 0);
	TASK_CHECK(m4_near(W, lb, EPS_MAT));

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* ============================ lifecycle ============================ */

/* invalid candidate tables are rejected and never disturb a live table or
 * published result
 */
static int life_invalid_tables(void)
{
	struct anm_node root, b, foreign;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation good, bad;
	struct anm_pg_options opts = {1e-8f, 100};
	float published[16], still[16];
	float nanv = nanf("");

	anm_init_node(&root); anm_init_node(&b); anm_init_node(&foreign);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	good.node_i=&root; good.node_j=&b; good.wt=4.0f; good.wr=1.0f;
	cgm_midentity(good.target); good.target[12]=5.0f;
	TASK_CHECK(anm_pg_replace_observations(pg, &good, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_get_matrix(&b, published, 0);

	bad = good;
	bad.node_j = &foreign;
	TASK_CHECK(anm_pg_replace_observations(pg, &bad, 1) == ANM_PG_ENODE);
	bad = good; bad.wt = 0.0f;
	TASK_CHECK(anm_pg_replace_observations(pg, &bad, 1) == ANM_PG_EWEIGHT);
	bad = good; bad.wr = -2.0f;
	TASK_CHECK(anm_pg_replace_observations(pg, &bad, 1) == ANM_PG_EWEIGHT);
	bad = good; bad.wt = nanv;
	TASK_CHECK(anm_pg_replace_observations(pg, &bad, 1) == ANM_PG_EWEIGHT);
	bad = good; bad.target[0] = 2.0f; /* non-unit column, not orthogonal */
	TASK_CHECK(anm_pg_replace_observations(pg, &bad, 1) == ANM_PG_ERIGID);
	bad = good; bad.target[5] = nanv;
	TASK_CHECK(anm_pg_replace_observations(pg, &bad, 1) == ANM_PG_ERIGID);
	bad = good; bad.node_i = 0;
	TASK_CHECK(anm_pg_replace_observations(pg, &bad, 1) == ANM_PG_ENODE);

	/* valid table still accepted, old publication still untouched */
	TASK_CHECK(anm_pg_replace_observations(pg, &good, 1) == ANM_PG_OK);
	anm_get_matrix(&b, still, 0);
	TASK_CHECK(m4_near(published, still, 0.0f));

	anm_pg_destroy(pg);
	anm_destroy_node(&foreign);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* components without anchors are rejected at install/solve time */
static int life_anchorless(void)
{
	struct anm_node r1, c1, r2, c2;
	struct anm_node *nodes[4];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};

	anm_init_node(&r1); anm_init_node(&c1);
	anm_init_node(&r2); anm_init_node(&c2);
	anm_link_node(&r1, &c1);
	anm_link_node(&r2, &c2);
	anm_set_position3f(&c1, 1, 0, 0, 0);
	anm_set_position3f(&c2, 1, 0, 0, 0);
	nodes[0]=&r1; nodes[1]=&c1; nodes[2]=&r2; nodes[3]=&c2;
	anchors[0]=&r1;

	pg = anm_pg_create(nodes, 4, anchors, 1, 1.0f, 1.0f);
	TASK_CHECK(pg != 0);
	/* no extra table: the r2/c2 component is anchorless already */
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_EANCHOR);

	/* observation fully inside the anchorless component stays rejected */
	ob.node_i=&r2; ob.node_j=&c2; ob.wt=1.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=2.0f;
	TASK_CHECK(anm_pg_replace_observations(pg, &ob, 1) == ANM_PG_EANCHOR);

	/* connecting the two components anchors r2's tree: now valid */
	ob.node_i=&c1; ob.node_j=&c2;
	TASK_CHECK(anm_pg_replace_observations(pg, &ob, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);

	anm_pg_destroy(pg);
	anm_destroy_node(&c2); anm_destroy_node(&r2);
	anm_destroy_node(&c1); anm_destroy_node(&r1);
	return 0;
}

/* non-convergence must not publish or touch the previous result */
static int life_nonconverge_rollback(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options ok_opts = {1e-8f, 100};
	struct anm_pg_options bad_opts = {1e-8f, 0};
	struct anm_pg_report rep;
	float w_before[16], w_after[16];

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	anm_pg_replace_observations(pg, &ob, 1);
	TASK_CHECK(anm_pg_solve(pg, 0, &ok_opts) == ANM_PG_OK);
	anm_get_matrix(&b, w_before, 0);

	TASK_CHECK(anm_pg_solve(pg, 0, &bad_opts) == ANM_PG_ECONVERGE);
	anm_pg_get_report(pg, &rep);
	TASK_CHECK(rep.status == ANM_PG_ECONVERGE);
	anm_get_matrix(&b, w_after, 0);
	TASK_CHECK(m4_near(w_before, w_after, 0.0f));

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* clearing overrides restores original animation and rebuildable caches */
static int life_clear_override(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};
	float orig[16], overridden[16], restored[16], inv[16], prod[16], id[16];

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	anm_get_matrix(&b, orig, 0);
	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	anm_pg_replace_observations(pg, &ob, 1);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_get_matrix(&b, overridden, 0);
	TASK_CHECK(m4_maxdiff(overridden, orig) > 1e-3f);

	TASK_CHECK(anm_pg_clear_overrides(pg) == ANM_PG_OK);
	TASK_CHECK(anm_pg_clear_overrides(pg) == ANM_PG_OK); /* idempotent */
	anm_get_matrix(&b, restored, 0);
	TASK_CHECK(m4_near(restored, orig, 0.0f));
	anm_get_inv_matrix(&b, inv, 0);
	cgm_mcopy(prod, restored);
	cgm_mmul(prod, inv);
	cgm_midentity(id);
	TASK_CHECK(m4_near(prod, id, EPS_MAT));

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* destroy detaches every node so original queries resume and nodes can join a
 * new graph afterwards
 */
static int life_destroy_detach(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};
	float orig[16], after[16];

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	anm_get_matrix(&b, orig, 0);
	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	anm_pg_replace_observations(pg, &ob, 1);
	anm_pg_solve(pg, 0, &opts);
	anm_pg_destroy(pg);

	anm_get_matrix(&b, after, 0);
	TASK_CHECK(m4_near(orig, after, 0.0f));

	/* nodes are reusable in a fresh graph */
	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	TASK_CHECK(pg != 0);
	TASK_CHECK(anm_pg_replace_observations(pg, &ob, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);
	anm_pg_destroy(pg);

	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* two independent graphs do not interfere; a node may belong to only one */
static int life_two_graphs(void)
{
	struct anm_node r1, a1, r2, a2;
	struct anm_node *n1[2], *n2[2], *n3[3];
	struct anm_node *an1[1], *an2[1];
	struct anm_pose_graph *g1, *g2, *g3;
	struct anm_pg_observation ob1, ob2;
	struct anm_pg_options opts = {1e-8f, 100};
	float w1[16], w2[16];

	anm_init_node(&r1); anm_init_node(&a1);
	anm_init_node(&r2); anm_init_node(&a2);
	anm_link_node(&r1, &a1);
	anm_link_node(&r2, &a2);
	anm_set_position3f(&a1, 1, 0, 0, 0);
	anm_set_position3f(&a2, 1, 0, 0, 0);
	n1[0]=&r1; n1[1]=&a1;
	n2[0]=&r2; n2[1]=&a2;
	an1[0]=&r1; an2[0]=&r2;

	g1 = anm_pg_create(n1, 2, an1, 1, 1.0f, 1.0f);
	g2 = anm_pg_create(n2, 2, an2, 1, 1.0f, 1.0f);
	TASK_CHECK(g1 != 0 && g2 != 0);

	ob1.node_i=&r1; ob1.node_j=&a1; ob1.wt=4.0f; ob1.wr=1.0f;
	cgm_midentity(ob1.target); ob1.target[12]=5.0f;
	ob2.node_i=&r2; ob2.node_j=&a2; ob2.wt=1.0f; ob2.wr=1.0f;
	cgm_midentity(ob2.target); ob2.target[12]=9.0f;
	TASK_CHECK(anm_pg_replace_observations(g1, &ob1, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_replace_observations(g2, &ob2, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(g1, 0, &opts) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(g2, 0, &opts) == ANM_PG_OK);

	anm_get_matrix(&a1, w1, 0);
	anm_get_matrix(&a2, w2, 0);
	/* g1: parent(1)+extra(5,wt4) -> 4.2; g2: parent(1)+extra(9,wt1) -> 5 */
	TASK_CHECK(fabsf(w1[12] - 4.2f) <= EPS_MAT);
	TASK_CHECK(fabsf(w2[12] - 5.0f) <= EPS_MAT);

	/* a1 already belongs to g1: a graph over overlapping nodes is refused */
	n3[0]=&r1; n3[1]=&a1; n3[2]=&r2;
	g3 = anm_pg_create(n3, 3, an1, 1, 1.0f, 1.0f);
	TASK_CHECK(g3 == 0);

	anm_pg_destroy(g2);
	anm_pg_destroy(g1);
	anm_destroy_node(&a2); anm_destroy_node(&r2);
	anm_destroy_node(&a1); anm_destroy_node(&r1);
	return 0;
}

/* active high-level automatic transitions are rejected, while fixed dual
 * animation mixing keeps working
 */
static int life_transition_rejected(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_add_node_animation(&b);
	anm_set_position3f(&b, 1, 0, 0, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	anm_pg_replace_observations(pg, &ob, 1);

	/* fixed dual animation mix (no running transition) is allowed */
	TASK_CHECK(anm_use_node_animations(&b, 0, 1, 0.5f) == 0);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);

	/* an active automatic transition is explicitly unsupported */
	anm_node_transition(&b, 1, 0, 1000);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_ETRANSITION);

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* non-unit animated scaling is reported; returning to unit scale works and
 * leaves no polluted state
 */
static int life_nonunit_scale(void)
{
	struct anm_node root, b;
	struct anm_node *nodes[2];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};

	anm_init_node(&root); anm_init_node(&b);
	anm_link_node(&root, &b);
	anm_set_position3f(&b, 1, 0, 0, 0);
	anm_set_scaling3f(&b, 2.0f, 1.0f, 1.0f, 0);
	nodes[0]=&root; nodes[1]=&b;
	anchors[0]=&root;

	pg = anm_pg_create(nodes, 2, anchors, 1, 1.0f, 1.0f);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	anm_pg_replace_observations(pg, &ob, 1);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_ESCALE);

	anm_set_scaling3f(&b, 1.0f, 1.0f, 1.0f, 0);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&root);
	return 0;
}

/* node array declared in an order unrelated to the tree topology, with a
 * time offset; ancestry closure still gives the analytic optimum
 */
static int life_unordered_declaration(void)
{
	struct anm_node root, a, b;
	struct anm_node *nodes[3];
	struct anm_node *anchors[1];
	struct anm_pose_graph *pg;
	struct anm_pg_observation ob;
	struct anm_pg_options opts = {1e-8f, 100};
	float W[3][16], expect_a[16], expect_b[16];

	anm_init_node(&root); anm_init_node(&a); anm_init_node(&b);
	anm_link_node(&root, &a);
	anm_link_node(&a, &b);
	anm_set_position3f(&a, 1, 0, 0, 0);
	anm_set_position3f(&b, 1, 0, 0, 0);
	anm_set_node_animation_offset(&a, 250, 0);

	nodes[0]=&b; nodes[1]=&root; nodes[2]=&a; /* deliberately unordered */
	anchors[0]=&root;

	pg = anm_pg_create(nodes, 3, anchors, 1, 1.0f, 1.0f);
	TASK_CHECK(pg != 0);
	ob.node_i=&root; ob.node_j=&b; ob.wt=4.0f; ob.wr=1.0f;
	cgm_midentity(ob.target); ob.target[12]=5.0f;
	TASK_CHECK(anm_pg_replace_observations(pg, &ob, 1) == ANM_PG_OK);
	TASK_CHECK(anm_pg_solve(pg, 0, &opts) == ANM_PG_OK);

	anm_get_matrix(&a, W[2], 0);
	anm_get_matrix(&b, W[0], 0);
	cgm_midentity(expect_a); expect_a[12] = 7.0f / 3.0f;
	cgm_midentity(expect_b); expect_b[12] = 14.0f / 3.0f;
	TASK_CHECK(m4_near(W[2], expect_a, EPS_MAT));
	TASK_CHECK(m4_near(W[0], expect_b, EPS_MAT));

	anm_pg_destroy(pg);
	anm_destroy_node(&b); anm_destroy_node(&a); anm_destroy_node(&root);
	return 0;
}

/* ============================ registration ============================ */

const struct task_case task_cases[] = {
	{"objective", "translation_weighted_loop", obj_translation_loop},
	{"objective", "noncommuting_rotation_translation", obj_noncommuting},
	{"objective", "nonzero_pivot_closed_loop", obj_pivot_loop},
	{"objective", "fork_tree_cross_branch", obj_fork_tree},
	{"objective", "consistent_groundtruth_zero_iter", obj_consistent_groundtruth},

	{"publication", "matrix_apis_consume_same_world", pub_world_apis},
	{"publication", "node_matrix_is_final_local", pub_node_local},
	{"publication", "override_only_at_published_time", pub_only_published_time},
	{"publication", "independent_resample_reverse_query", pub_resample_reverse},
	{"publication", "replace_table_keeps_published_pose", pub_replace_keeps_published},
	{"publication", "stationary_initial_zero_iterations", pub_zero_iterations},

	{"lifecycle", "invalid_table_rollback", life_invalid_tables},
	{"lifecycle", "anchorless_component_rejected", life_anchorless},
	{"lifecycle", "nonconvergence_rollback", life_nonconverge_rollback},
	{"lifecycle", "clear_overrides_restores_animation", life_clear_override},
	{"lifecycle", "destroy_detaches_nodes", life_destroy_detach},
	{"lifecycle", "two_independent_graphs", life_two_graphs},
	{"lifecycle", "active_transition_rejected", life_transition_rejected},
	{"lifecycle", "nonunit_scale_rejected", life_nonunit_scale},
	{"lifecycle", "unordered_node_declaration", life_unordered_declaration},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
