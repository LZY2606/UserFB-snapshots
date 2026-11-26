#include "task_support/check.h"

#include "mini3d.h"
#include "motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Test helpers                                                         */
/* ------------------------------------------------------------------ */

static vertex_t vtx(float x, float y, float z, float r, float g, float b) {
	vertex_t v;
	memset(&v, 0, sizeof(v));
	v.pos.x = x;
	v.pos.y = y;
	v.pos.z = z;
	v.pos.w = 1.0f;
	v.color.r = r;
	v.color.g = g;
	v.color.b = b;
	v.rhw = 1.0f;
	return v;
}

static motion_triangle tri_make(const vertex_t o[3], const vertex_t c[3]) {
	motion_triangle t;
	memcpy(t.open, o, sizeof(t.open));
	memcpy(t.close, c, sizeof(t.close));
	return t;
}

/* Map world coordinates directly to screen coordinates, with y downward. */
static device_t pixel_device(int w, int h, IUINT32 background) {
	device_t d;
	matrix_t *m;
	device_init(&d, w, h, NULL);
	d.background = background;
	device_clear(&d, 0);
	d.render_state = RENDER_STATE_COLOR;
	m = &d.transform.transform;
	matrix_set_identity(m);
	m->m[0][0] = 2.0f / (float)w;
	m->m[3][0] = -1.0f;
	m->m[1][1] = -2.0f / (float)h;
	m->m[3][1] = 1.0f;
	m->m[2][2] = 1.0f;
	m->m[3][3] = 1.0f;
	return d;
}

static motion_triangle reversed(const motion_triangle *t) {
	motion_triangle r;
	int k;
	for (k = 0; k < 3; ++k) {
		r.open[k] = t->close[k];
		r.close[k] = t->open[k];
	}
	return r;
}

static int near_double(double a, double b, double eps) {
	return fabs(a - b) <= eps;
}

/* ------------------------------------------------------------------ */
/* events                                                               */
/* ------------------------------------------------------------------ */

/*
 * A 1/128-wide strip translates through the only pixel center during
 * [1/8, 3/16].  It is absent at t=0, t=0.5 and t=1.  All coordinates are
 * binary fractions, so the hand calculation is not perturbed by float
 * decimal rounding.
 */
static int case_brief_glint(void) {
	device_t d = pixel_device(1, 1, 0x000000);
	double rgb[3] = { -1, -1, -1 };
	vertex_t o[3] = {
		vtx(61.0f / 128.0f, 0.0f, 0.0f, 1, 0, 0),
		vtx(31.0f / 64.0f, 0.5f, 0.0f, 1, 0, 0),
		vtx(61.0f / 128.0f, 1.0f, 0.0f, 1, 0, 0)
	};
	vertex_t c[3] = {
		vtx(77.0f / 128.0f, 0.0f, 0.0f, 1, 0, 0),
		vtx(39.0f / 64.0f, 0.5f, 0.0f, 1, 0, 0),
		vtx(77.0f / 128.0f, 1.0f, 0.0f, 1, 0, 0)
	};
	motion_triangle t = tri_make(o, c);
	int ok = motion_render(&d, &t, 1, rgb);
	TASK_CHECK(ok);
	TASK_CHECK(near_double(rgb[0], 1.0 / 16.0, 1e-10));
	TASK_CHECK(near_double(rgb[1], 0.0, 1e-12));
	TASK_CHECK(near_double(rgb[2], 0.0, 1e-12));
	TASK_CHECK(d.framebuffer[0][0] == 0x100000);
	device_destroy(&d);
	return 0;
}

/* Even a 2^-15 exposure away from t=1/2 must produce its exact integral. */
static int case_ultra_short_glint(void) {
	device_t d = pixel_device(1, 1, 0x000000);
	double rgb[3];
	const double duration = 1.0 / 32768.0;
	const float left_open = 126975.0f / 262144.0f;
	const float right_open = 31.0f / 64.0f;
	vertex_t o[3] = {
		vtx(left_open, 0.0f, 0, 1, 1, 1),
		vtx(right_open, 0.5f, 0, 1, 1, 1),
		vtx(left_open, 1.0f, 0, 1, 1, 1)
	};
	vertex_t c[3] = {
		vtx(left_open + 0.125f, 0.0f, 0, 1, 1, 1),
		vtx(right_open + 0.125f, 0.5f, 0, 1, 1, 1),
		vtx(left_open + 0.125f, 1.0f, 0, 1, 1, 1)
	};
	motion_triangle t = tri_make(o, c);
	TASK_CHECK(motion_render(&d, &t, 1, rgb));
	TASK_CHECK(near_double(rgb[0], duration, 2e-10));
	TASK_CHECK(near_double(rgb[1], duration, 2e-10));
	TASK_CHECK(near_double(rgb[2], duration, 2e-10));
	/* Too short to survive 8-bit quantization, but rgb still knows it. */
	TASK_CHECK(d.framebuffer[0][0] == 0x000000);
	device_destroy(&d);
	return 0;
}

/*
 * q_k(t) = p + (t-1/2) r_k: all three vertices meet at the pixel center
 * exactly in the middle, while p is the centroid at every other instant.
 */
static int case_middle_degenerate(void) {
	device_t d = pixel_device(1, 1, 0x000000);
	double rgb[3];
	vertex_t o[3] = {
		vtx(0.625f, 0.625f, 0.0f, 1, 0, 0),
		vtx(0.375f, 0.625f, 0.0f, 0, 1, 0),
		vtx(0.500f, 0.250f, 0.0f, 0, 0, 1)
	};
	vertex_t c[3] = {
		vtx(0.375f, 0.375f, 0.0f, 1, 0, 0),
		vtx(0.625f, 0.375f, 0.0f, 0, 1, 0),
		vtx(0.500f, 0.750f, 0.0f, 0, 0, 1)
	};
	motion_triangle t = tri_make(o, c);
	TASK_CHECK(motion_render(&d, &t, 1, rgb));
	TASK_CHECK(near_double(rgb[0], 1.0 / 3.0, 1e-9));
	TASK_CHECK(near_double(rgb[1], 1.0 / 3.0, 1e-9));
	TASK_CHECK(near_double(rgb[2], 1.0 / 3.0, 1e-9));
	TASK_CHECK(d.framebuffer[0][0] == 0x555555);
	device_destroy(&d);
	return 0;
}

/* A persistent vertical common edge belongs to the triangle on its right. */
static int case_shared_vertical_edge(void) {
	device_t d = pixel_device(2, 1, 0x000000);
	double rgb[6];
	vertex_t lo[3] = {
		vtx(0.5f, 0.0f, 0.2f, 0, 0, 1),
		vtx(0.5f, 1.0f, 0.2f, 0, 0, 1),
		vtx(0.0f, 0.5f, 0.2f, 0, 0, 1)
	};
	vertex_t ro[3] = {
		vtx(0.5f, 0.0f, 0.2f, 1, 0, 0),
		vtx(0.5f, 1.0f, 0.2f, 1, 0, 0),
		vtx(1.0f, 0.5f, 0.2f, 1, 0, 0)
	};
	motion_triangle tl = tri_make(lo, lo);
	motion_triangle tr = tri_make(ro, ro);
	motion_triangle ts[2] = { tl, tr };
	TASK_CHECK(motion_render(&d, ts, 2, rgb));
	/* Pixel (0,0) center is exactly (0.5,0.5). */
	TASK_CHECK(near_double(rgb[0], 1.0, 1e-10));
	TASK_CHECK(near_double(rgb[2], 0.0, 1e-12));
	TASK_CHECK(d.framebuffer[0][0] == 0xff0000);
	/* The other pixel remains background. */
	TASK_CHECK(d.framebuffer[0][1] == 0x000000);

	/* Put the same persistent edge opposite barycentric index zero. */
	{
		vertex_t l0[3] = { lo[2], lo[0], lo[1] };
		vertex_t r0[3] = { ro[2], ro[0], ro[1] };
		motion_triangle a0 = tri_make(l0, l0);
		motion_triangle b0 = tri_make(r0, r0);
		motion_triangle pair0[2] = { a0, b0 };
		TASK_CHECK(motion_render(&d, pair0, 2, rgb));
		TASK_CHECK(near_double(rgb[0], 1.0, 1e-10));
		TASK_CHECK(d.framebuffer[0][0] == 0xff0000);
	}
	device_destroy(&d);
	return 0;
}

/* A small conforming grid must leave neither background holes nor doubles. */
static int case_shared_grid(void) {
	device_t d = pixel_device(4, 4, 0x102030);
	double *rgb = calloc(48, sizeof(double));
	motion_triangle ts[8];
	vertex_t q[9];
	int i, n = 0;
	TASK_CHECK(rgb != NULL);
	for (i = 0; i < 9; ++i) {
		int x = i % 3, y = i / 3;
		q[i] = vtx((float)x * 2.0f, (float)y * 2.0f, 0.5f,
			0.8f, 0.2f, 0.4f);
	}
	/* Two triangles per cell, consistently indexed. */
	#define ADD(a,b,c) do { \
		vertex_t oo[3] = { q[a], q[b], q[c] }; \
		ts[n++] = tri_make(oo, oo); } while (0)
	ADD(0, 1, 4); ADD(0, 4, 3);
	ADD(1, 2, 5); ADD(1, 5, 4);
	ADD(3, 4, 7); ADD(3, 7, 6);
	ADD(4, 5, 8); ADD(4, 8, 7);
	#undef ADD
	TASK_CHECK(n == 8);
	TASK_CHECK(motion_render(&d, ts, (size_t)n, rgb));
	for (i = 0; i < 16; ++i) {
		/* The grid covers screen [0,4]^2; every pixel center is covered. */
		TASK_CHECK(rgb[i * 3 + 0] + rgb[i * 3 + 1] + rgb[i * 3 + 2]
			> 0.999);
		TASK_CHECK((d.framebuffer[i / 4][i % 4] & 0xffffffU) !=
			0x102030U);
	}
	free(rgb);
	device_destroy(&d);
	return 0;
}

/* ------------------------------------------------------------------ */
/* integrals                                                           */
/* ------------------------------------------------------------------ */

/*
 * Right triangle (0,0),(1+t,0),(0,1), p=(.5,.5).  lambda0 is
 * t/(2(1+t)); with only vertex zero red, the red integral is
 * (1-ln2)/2.
 */
static int case_varying_vertex_color(void) {
	device_t d = pixel_device(2, 2, 0x000000);
	double rgb[12];
	vertex_t o[3] = {
		vtx(0, 0, 0, 1, 0, 0),
		vtx(1, 0, 0, 0, 0, 0),
		vtx(0, 1, 0, 0, 0, 0)
	};
	vertex_t c[3] = {
		vtx(0, 0, 0, 1, 0, 0),
		vtx(2, 0, 0, 0, 0, 0),
		vtx(0, 1, 0, 0, 0, 0)
	};
	motion_triangle t = tri_make(o, c);
	double expected = 0.5 * (1.0 - log(2.0));
	TASK_CHECK(motion_render(&d, &t, 1, rgb));
	TASK_CHECK(near_double(rgb[0], expected, 1e-9));
	TASK_CHECK(near_double(rgb[1], 0, 1e-12));
	TASK_CHECK(near_double(rgb[2], 0, 1e-12));
	device_destroy(&d);
	return 0;
}

/*
 * Off-center barycentric weights are (1/4,1/4,1/2).  Depth crosses at
 * t=.6 at the pixel, but at the centroid it crosses at t=.9.
 */
static int case_depth_swap_offcenter(void) {
	device_t d = pixel_device(2, 2, 0x000000);
	double rgb[12], center_cross;
	vertex_t ao[3] = {
		vtx(0, 0, 0.1250f, 1, 0, 0),
		vtx(2, 0, 0.3125f, 1, 0, 0),
		vtx(0, 1, 0.2500f, 1, 0, 0)
	};
	vertex_t ac[3] = {
		vtx(0, 0, 0.1250f, 1, 0, 0),
		vtx(2, 0, 0.3125f, 1, 0, 0),
		vtx(0, 1, 1.0000f, 1, 0, 0)
	};
	vertex_t bo[3] = {
		vtx(0, 0, 0.6250f, 0, 0, 1),
		vtx(2, 0, 0.4375f, 0, 0, 1),
		vtx(0, 1, 0.5000f, 0, 0, 1)
	};
	vertex_t bc[3] = {
		vtx(0, 0, 0.6250f, 0, 0, 1),
		vtx(2, 0, 0.4375f, 0, 0, 1),
		vtx(0, 1, 0.3125f, 0, 0, 1)
	};
	motion_triangle ts[2] = { tri_make(ao, ac), tri_make(bo, bc) };
	TASK_CHECK(motion_render(&d, ts, 2, rgb));
	TASK_CHECK(near_double(rgb[0], 0.60, 1e-9));
	TASK_CHECK(near_double(rgb[1], 0.0, 1e-10));
	TASK_CHECK(near_double(rgb[2], 0.40, 1e-9));
	/* The centroid uses 1/3 weights; independently derive its cross. */
	{
		double ac0 = (ao[0].pos.z + ao[1].pos.z + ao[2].pos.z) / 3.0;
		double ac1 = (ac[0].pos.z + ac[1].pos.z + ac[2].pos.z) / 3.0;
		double bc0 = (bo[0].pos.z + bo[1].pos.z + bo[2].pos.z) / 3.0;
		double bc1 = (bc[0].pos.z + bc[1].pos.z + bc[2].pos.z) / 3.0;
		center_cross = (bc0 - ac0) / ((ac1 - ac0) - (bc1 - bc0));
	}
	TASK_CHECK(near_double(center_cross, 14.0 / 15.0, 1e-6));
	TASK_CHECK(fabs(center_cross - 0.6) > 0.30);
	TASK_CHECK(d.framebuffer[0][0] == 0x990066);
	device_destroy(&d);
	return 0;
}

/* Reversing open/close reparameterizes the same path and integral. */
static int case_time_reversal(void) {
	device_t d1 = pixel_device(2, 2, 0x000000);
	device_t d2 = pixel_device(2, 2, 0x000000);
	double a[12], b[12];
	vertex_t ao[3] = {
		vtx(0, 0, 0.10f, 1, .2f, 0),
		vtx(2, 0, 0.30f, .1f, 1, .2f),
		vtx(0, 1, 0.20f, .2f, .3f, 1)
	};
	vertex_t ac[3] = {
		vtx(.2f, .1f, 0.10f, 1, .2f, 0),
		vtx(1.8f, .2f, 0.30f, .1f, 1, .2f),
		vtx(.1f, .9f, 1.00f, .2f, .3f, 1)
	};
	vertex_t bo[3] = {
		vtx(0, 0, 0.60f, 0, 1, 1),
		vtx(2, 0, 0.40f, 1, 1, 0),
		vtx(0, 1, 0.50f, 1, 0, 1)
	};
	vertex_t bc[3] = {
		vtx(.1f, .2f, 0.60f, 0, 1, 1),
		vtx(1.7f, .1f, 0.40f, 1, 1, 0),
		vtx(.2f, .8f, 0.30f, 1, 0, 1)
	};
	motion_triangle ts[2] = { tri_make(ao, ac), tri_make(bo, bc) };
	motion_triangle rs[2] = { reversed(&ts[0]), reversed(&ts[1]) };
	int x, y;
	TASK_CHECK(motion_render(&d1, ts, 2, a));
	TASK_CHECK(motion_render(&d2, rs, 2, b));
	for (x = 0; x < 12; ++x)
		TASK_CHECK(near_double(a[x], b[x], 2e-9));
	for (y = 0; y < 2; ++y)
		for (x = 0; x < 2; ++x)
			TASK_CHECK(d1.framebuffer[y][x] == d2.framebuffer[y][x]);
	device_destroy(&d1);
	device_destroy(&d2);
	return 0;
}

/* Static colored triangle: analytic barycentric color is constant. */
static int case_static_barycentric(void) {
	device_t d = pixel_device(2, 2, 0xffffff);
	double rgb[12];
	vertex_t o[3] = {
		vtx(0, 0, 0, 1, 0, 0),
		vtx(2, 0, 0, 0, 1, 0),
		vtx(0, 2, 0, 0, 0, 1)
	};
	motion_triangle t = tri_make(o, o);
	TASK_CHECK(motion_render(&d, &t, 1, rgb));
	/* p=(.5,.5): lambda=(.5,.25,.25). */
	TASK_CHECK(near_double(rgb[0], .5, 1e-10));
	TASK_CHECK(near_double(rgb[1], .25, 1e-10));
	TASK_CHECK(near_double(rgb[2], .25, 1e-10));
	device_destroy(&d);
	return 0;
}

static int case_equal_depth_uses_index(void) {
	device_t d = pixel_device(1, 1, 0x000000);
	double rgb[3];
	vertex_t r[3] = {
		vtx(.7f, .3f, .5f, 1, 0, 0),
		vtx(.3f, .3f, .5f, 1, 0, 0),
		vtx(.5f, .7f, .5f, 1, 0, 0)
	};
	vertex_t b[3] = {
		vtx(.7f, .3f, .5f, 0, 0, 1),
		vtx(.3f, .3f, .5f, 0, 0, 1),
		vtx(.5f, .7f, .5f, 0, 0, 1)
	};
	motion_triangle tr = tri_make(r, r);
	motion_triangle tb = tri_make(b, b);
	motion_triangle order_r[2] = { tr, tb };
	motion_triangle order_b[2] = { tb, tr };

	TASK_CHECK(motion_render(&d, order_r, 2, rgb));
	TASK_CHECK(near_double(rgb[0], 1.0, 1e-10));
	TASK_CHECK(near_double(rgb[2], 0.0, 1e-12));
	TASK_CHECK(d.framebuffer[0][0] == 0xff0000);
	TASK_CHECK(motion_render(&d, order_b, 2, rgb));
	TASK_CHECK(near_double(rgb[2], 1.0, 1e-10));
	TASK_CHECK(near_double(rgb[0], 0.0, 1e-12));
	TASK_CHECK(d.framebuffer[0][0] == 0x0000ff);
	device_destroy(&d);
	return 0;
}

/* ------------------------------------------------------------------ */
/* pipeline                                                             */
/* ------------------------------------------------------------------ */

static int case_count_zero_background(void) {
	device_t d = pixel_device(3, 2, 0x123456);
	double rgb[18];
	int x, y;
	TASK_CHECK(motion_render(&d, NULL, 0, rgb));
	for (y = 0; y < 2; ++y) {
		for (x = 0; x < 3; ++x) {
			int k = (y * 3 + x) * 3;
			TASK_CHECK(near_double(rgb[k + 0], 0x12 / 255.0, 1e-12));
			TASK_CHECK(near_double(rgb[k + 1], 0x34 / 255.0, 1e-12));
			TASK_CHECK(near_double(rgb[k + 2], 0x56 / 255.0, 1e-12));
			TASK_CHECK(d.framebuffer[y][x] == 0x123456);
		}
	}
	device_destroy(&d);
	return 0;
}

static int case_invalid_pointers(void) {
	device_t d = pixel_device(2, 2, 0xabcdef);
	double rgb[12];
	IUINT32 before = d.framebuffer[0][0];
	TASK_CHECK(!motion_render(NULL, NULL, 0, rgb));
	TASK_CHECK(!motion_render(&d, NULL, 1, rgb));
	TASK_CHECK(!motion_render(&d, NULL, 0, NULL));
	TASK_CHECK(!motion_render(NULL, NULL, 0, NULL));
	TASK_CHECK(d.framebuffer[0][0] == before);
	device_destroy(&d);
	return 0;
}

static int case_device_state_preserved(void) {
	device_t d = pixel_device(4, 4, 0x334455);
	transform_t saved_transform;
	float zcopy[16];
	motion_triangle t;
	vertex_t o[3] = {
		vtx(0, 0, .2f, .9f, .1f, .1f),
		vtx(4, 0, .4f, .1f, .9f, .1f),
		vtx(0, 4, .6f, .1f, .1f, .9f)
	};
	double rgb[48];
	int x, y;
	t = tri_make(o, o);
	saved_transform = d.transform;
	for (y = 0; y < 4; ++y)
		for (x = 0; x < 4; ++x)
			zcopy[y * 4 + x] = d.zbuffer[y][x] = -3.25f;
	TASK_CHECK(motion_render(&d, &t, 1, rgb));
	TASK_CHECK(memcmp(&d.transform, &saved_transform, sizeof(saved_transform)) == 0);
	TASK_CHECK(d.width == 4 && d.height == 4);
	TASK_CHECK(d.render_state == RENDER_STATE_COLOR);
	TASK_CHECK(d.background == 0x334455);
	for (y = 0; y < 4; ++y)
		for (x = 0; x < 4; ++x)
			TASK_CHECK(d.zbuffer[y][x] == zcopy[y * 4 + x]);
	device_destroy(&d);
	return 0;
}

static int case_input_unchanged(void) {
	device_t d = pixel_device(2, 2, 0);
	double rgb[12];
	vertex_t o[3] = {
		vtx(.2f, .1f, .2f, 1, 1, 0),
		vtx(1.8f, .2f, .4f, 0, 1, 1),
		vtx(.1f, 1.9f, .6f, 1, 0, 1)
	};
	vertex_t c[3] = {
		vtx(.3f, .2f, .3f, 1, 1, 0),
		vtx(1.7f, .3f, .5f, 0, 1, 1),
		vtx(.2f, 1.8f, .7f, 1, 0, 1)
	};
	motion_triangle t = tri_make(o, c), before = t;
	TASK_CHECK(motion_render(&d, &t, 1, rgb));
	TASK_CHECK(memcmp(&t, &before, sizeof(t)) == 0);
	device_destroy(&d);
	return 0;
}

static int case_interleaved_devices(void) {
	device_t a = pixel_device(1, 1, 0);
	device_t b = pixel_device(1, 1, 0xffffff);
	double ra[3], rb[3], ra2[3];
	vertex_t o[3] = {
		vtx(.625f, .625f, 0, 1, 0, 0),
		vtx(.375f, .625f, 0, 0, 1, 0),
		vtx(.500f, .250f, 0, 0, 0, 1)
	};
	vertex_t c[3] = {
		vtx(.375f, .375f, 0, 1, 0, 0),
		vtx(.625f, .375f, 0, 0, 1, 0),
		vtx(.500f, .750f, 0, 0, 0, 1)
	};
	motion_triangle t = tri_make(o, c);
	TASK_CHECK(motion_render(&a, &t, 1, ra));
	TASK_CHECK(motion_render(&b, &t, 1, rb));
	TASK_CHECK(motion_render(&a, &t, 1, ra2));
	TASK_CHECK(near_double(ra[0], ra2[0], 1e-14));
	TASK_CHECK(near_double(ra[1], ra2[1], 1e-14));
	TASK_CHECK(near_double(ra[2], ra2[2], 1e-14));
	TASK_CHECK(near_double(rb[0], ra[0], 1e-9));
	device_destroy(&a);
	device_destroy(&b);
	return 0;
}

/* The old immediate renderer keeps its depth/color behavior. */
static int case_original_draw_still_works(void) {
	device_t d;
	vertex_t a = vtx(-.8f, -.8f, 2, 1, 0, 0);
	vertex_t b = vtx(.8f, -.8f, 2, 1, 0, 0);
	vertex_t c = vtx(0, .8f, 2, 1, 0, 0);
	int n = 0, x, y;
	device_init(&d, 32, 32, NULL);
	device_clear(&d, 0);
	d.render_state = RENDER_STATE_COLOR;
	device_draw_primitive(&d, &a, &b, &c);
	TASK_CHECK(d.framebuffer[16][16] == 0xff0000);
	for (y = 0; y < 32; ++y)
		for (x = 0; x < 32; ++x)
			if (d.zbuffer[y][x] > 0.0f) ++n;
	TASK_CHECK(n > 30);
	device_destroy(&d);
	return 0;
}

const struct task_case task_cases[] = {
	{ "events", "brief_glint", case_brief_glint },
	{ "events", "ultra_short_glint", case_ultra_short_glint },
	{ "events", "middle_degenerate", case_middle_degenerate },
	{ "events", "shared_vertical_edge", case_shared_vertical_edge },
	{ "events", "shared_grid", case_shared_grid },
	{ "integrals", "varying_vertex_color", case_varying_vertex_color },
	{ "integrals", "depth_swap_offcenter", case_depth_swap_offcenter },
	{ "integrals", "time_reversal", case_time_reversal },
	{ "integrals", "static_barycentric", case_static_barycentric },
	{ "integrals", "equal_depth_uses_index", case_equal_depth_uses_index },
	{ "pipeline", "count_zero_background", case_count_zero_background },
	{ "pipeline", "invalid_pointers", case_invalid_pointers },
	{ "pipeline", "device_state_preserved", case_device_state_preserved },
	{ "pipeline", "input_unchanged", case_input_unchanged },
	{ "pipeline", "interleaved_devices", case_interleaved_devices },
	{ "pipeline", "original_draw_still_works", case_original_draw_still_works }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
