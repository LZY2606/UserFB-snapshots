//=====================================================================
//
// motion_target.c - hand-computed tests for the continuous shutter.
//
// Every numeric expectation below is derived on paper from the
// screen-space geometry, not sampled.  The device transform used in
// the cases maps world coordinates 1:1 onto pixel coordinates
// (sx = x, sy = y, sz = z), so the hand calculations stay exact.
//
//=====================================================================
#include "../task_support/check.h"
#include "motion.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// Install a transform whose homogenised screen position equals the
// world (x, y) and whose window-z equals world z (w stays 1).
static void set_pixel_transform(device_t *d) {
	int W = d->width, H = d->height;
	matrix_t m;
	matrix_set_zero(&m);
	m.m[0][0] = 2.0f / W;
	m.m[3][0] = -1.0f;
	m.m[1][1] = -2.0f / H;
	m.m[3][1] = 1.0f;
	m.m[2][2] = 1.0f;
	m.m[3][3] = 1.0f;
	d->transform.world = m;
	matrix_set_identity(&d->transform.view);
	matrix_set_identity(&d->transform.projection);
	transform_update(&d->transform);
}

static vertex_t mkvertex(float x, float y, float z, color_t c) {
	vertex_t v;
	v.pos.x = x;
	v.pos.y = y;
	v.pos.z = z;
	v.pos.w = 1.0f;
	v.tc.u = v.tc.v = 0.0f;
	v.color = c;
	v.rhw = 1.0f;
	return v;
}

static double pix(const double *rgb, int W, int x, int y, int ch) {
	return rgb[((size_t)y * W + x) * 3 + ch];
}

//---------------------------------------------------------------------
// events group
//---------------------------------------------------------------------

// A right triangle translates in +x.  Pixel (3,2) centre (3.5,2.5)
// is covered for t in [0.5, 1]; hand-derived barycentric weights are
// l0 = 1/2, l1 = (2t-1)/3, l2 = (2.5-2t)/3.
static int case_entry_exit_window(void) {
	const int W = 8, H = 8;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x000000;

	color_t red = { 1, 0, 0 }, green = { 0, 1, 0 }, blue = { 0, 0, 1 };
	motion_triangle t;
	t.open[0] = mkvertex(1, 1, 0, red);
	t.open[1] = mkvertex(1, 4, 0, green);
	t.open[2] = mkvertex(4, 4, 0, blue);
	t.close[0] = mkvertex(3, 1, 0, red);
	t.close[1] = mkvertex(3, 4, 0, green);
	t.close[2] = mkvertex(6, 4, 0, blue);

	TASK_CHECK(motion_render(&d, &t, 1, rgb) == 1);

	// Integrals on [0.5, 1]: r = 1/4, g = 1/12, b = 1/6.
	TASK_CHECK(fabs(pix(rgb, W, 3, 2, 0) - 0.25) < 1e-7);
	TASK_CHECK(fabs(pix(rgb, W, 3, 2, 1) - (1.0 / 12.0)) < 1e-7);
	TASK_CHECK(fabs(pix(rgb, W, 3, 2, 2) - (1.0 / 6.0)) < 1e-7);
	// Channel sum equals the coverage duration (one opaque face).
	TASK_CHECK(fabs(pix(rgb, W, 3, 2, 0) + pix(rgb, W, 3, 2, 1)
		+ pix(rgb, W, 3, 2, 2) - 0.5) < 1e-7);
	// A pixel never swept stays background (black).
	TASK_CHECK(pix(rgb, W, 7, 7, 0) == 0.0);
	TASK_CHECK(pix(rgb, W, 7, 7, 1) == 0.0);
	TASK_CHECK(pix(rgb, W, 7, 7, 2) == 0.0);

	free(rgb);
	device_destroy(&d);
	return 0;
}

// Brief reveal: the same triangle sweeps ten pixels in x.  Pixel (4,2)
// centre (4.5,2.5) is covered only for t in [0.2, 0.35].  At t = 0,
// 0.5 and 1 the centre is outside, so endpoint/midpoint sampling would
// miss the face entirely; the event-driven path must keep the window.
static int case_brief_traversal(void) {
	const int W = 8, H = 8;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x000000;

	color_t white = { 1, 1, 1 };
	motion_triangle t;
	t.open[0] = mkvertex(1, 1, 0, white);
	t.open[1] = mkvertex(1, 4, 0, white);
	t.open[2] = mkvertex(4, 4, 0, white);
	t.close[0] = mkvertex(11, 1, 0, white);
	t.close[1] = mkvertex(11, 4, 0, white);
	t.close[2] = mkvertex(14, 4, 0, white);

	TASK_CHECK(motion_render(&d, &t, 1, rgb) == 1);

	// White vertices: every channel equals the coverage duration.
	TASK_CHECK(fabs(pix(rgb, W, 4, 2, 0) - 0.15) < 1e-7);
	TASK_CHECK(fabs(pix(rgb, W, 4, 2, 1) - 0.15) < 1e-7);
	TASK_CHECK(fabs(pix(rgb, W, 4, 2, 2) - 0.15) < 1e-7);

	free(rgb);
	device_destroy(&d);
	return 0;
}

// Triangle degenerates exactly at t = 0.5 (vertex 2 crosses the edge
// joining vertices 0 and 1) and flips orientation.  Pixel (3,1)
// centre (3.5,1.5) is covered only on the far side, t in [5/6, 1].
static int case_midpoint_degenerate(void) {
	const int W = 8, H = 8;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x000000;

	color_t red = { 1, 0, 0 }, green = { 0, 1, 0 }, blue = { 0, 0, 1 };
	motion_triangle t;
	t.open[0] = mkvertex(1, 1, 0, red);
	t.open[1] = mkvertex(5, 1, 0, green);
	t.open[2] = mkvertex(3, 0, 0, blue);   // y = 2t, collinear at t=0.5
	t.close[0] = mkvertex(1, 1, 0, red);
	t.close[1] = mkvertex(5, 1, 0, green);
	t.close[2] = mkvertex(3, 2, 0, blue);

	TASK_CHECK(motion_render(&d, &t, 1, rgb) == 1);

	// Coverage duration 1/6; blue weight l2 = 1/(4t-2) integrates to
	// (1/4) ln(3/2) on [5/6, 1].
	double sum = pix(rgb, W, 3, 1, 0) + pix(rgb, W, 3, 1, 1)
	           + pix(rgb, W, 3, 1, 2);
	TASK_CHECK(fabs(sum - (1.0 / 6.0)) < 1e-7);
	TASK_CHECK(fabs(pix(rgb, W, 3, 1, 2) - 0.25 * log(1.5)) < 1e-7);
	// On the near side of the flip the pixel is never covered.
	TASK_CHECK(pix(rgb, W, 3, 4, 0) == 0.0);

	free(rgb);
	device_destroy(&d);
	return 0;
}

// Two triangles share the diagonal of a 4x4 quad, static in time.
// Pixel centres lying exactly on the shared edge must be owned by
// exactly one face (no background seam, no double coverage).
static int case_shared_edge_grid(void) {
	const int W = 8, H = 8;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x000000;

	color_t red = { 1, 0, 0 }, green = { 0, 1, 0 }, blue = { 0, 0, 1 };
	motion_triangle grid[2];
	// T1: (0,0),(4,0),(4,4), interior y <= x.
	grid[0].open[0] = mkvertex(0, 0, 0, red);
	grid[0].open[1] = mkvertex(4, 0, 0, green);
	grid[0].open[2] = mkvertex(4, 4, 0, blue);
	// T2: (0,0),(4,4),(0,4), interior y >= x, owns the diagonal.
	grid[1].open[0] = mkvertex(0, 0, 0, red);
	grid[1].open[1] = mkvertex(4, 4, 0, green);
	grid[1].open[2] = mkvertex(0, 4, 0, blue);
	for (int k = 0; k < 3; k++) {
		grid[0].close[k] = grid[0].open[k];
		grid[1].close[k] = grid[1].open[k];
	}

	TASK_CHECK(motion_render(&d, grid, 2, rgb) == 1);

	// All 16 pixel centres inside the quad: fully covered, no seam.
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++) {
			double s = pix(rgb, W, x, y, 0) + pix(rgb, W, x, y, 1)
			         + pix(rgb, W, x, y, 2);
			TASK_CHECK(fabs(s - 1.0) < 1e-9);
		}
	// Diagonal pixel (1,1): owned by T2, weights (5/8, 3/8, 0).
	TASK_CHECK(fabs(pix(rgb, W, 1, 1, 0) - (5.0 / 8.0)) < 1e-9);
	TASK_CHECK(fabs(pix(rgb, W, 1, 1, 1) - (3.0 / 8.0)) < 1e-9);
	TASK_CHECK(pix(rgb, W, 1, 1, 2) == 0.0);
	// T1 interior pixel (2,1): weights (3/8, 1/4, 3/8).
	TASK_CHECK(fabs(pix(rgb, W, 2, 1, 0) - (3.0 / 8.0)) < 1e-9);
	TASK_CHECK(fabs(pix(rgb, W, 2, 1, 1) - 0.25) < 1e-9);
	TASK_CHECK(fabs(pix(rgb, W, 2, 1, 2) - (3.0 / 8.0)) < 1e-9);

	free(rgb);
	device_destroy(&d);
	return 0;
}

// A triangle whose vertices stay collinear for the whole exposure has
// zero projected area throughout and must be ignored even though it
// geometrically overlaps a real face (and would win the depth tie by
// index if it were ever rasterised).
static int case_zero_area_ignored(void) {
	const int W = 8, H = 8;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x000000;

	color_t red = { 1, 0, 0 }, green = { 0, 1, 0 };
	motion_triangle tr[2];
	// Degenerate: three points sliding along the line y = 1, so they
	// stay collinear (zero projected area) for the whole exposure.
	tr[0].open[0] = mkvertex(1, 1, 0, red);
	tr[0].open[1] = mkvertex(3, 1, 0, red);
	tr[0].open[2] = mkvertex(5, 1, 0, red);
	tr[0].close[0] = mkvertex(2, 1, 0, red);
	tr[0].close[1] = mkvertex(4, 1, 0, red);
	tr[0].close[2] = mkvertex(6, 1, 0, red);
	// Solid green triangle underneath.
	tr[1].open[0] = mkvertex(0, 0, 0.5f, green);
	tr[1].open[1] = mkvertex(6, 0, 0.5f, green);
	tr[1].open[2] = mkvertex(0, 6, 0.5f, green);
	for (int k = 0; k < 3; k++) tr[1].close[k] = tr[1].open[k];

	TASK_CHECK(motion_render(&d, tr, 2, rgb) == 1);
	// Pixel (2,1) centre (2.5,1.5): inside green, the red line must
	// contribute nothing.
	TASK_CHECK(pix(rgb, W, 2, 1, 0) == 0.0);
	TASK_CHECK(fabs(pix(rgb, W, 2, 1, 1) - 1.0) < 1e-9);

	free(rgb);
	device_destroy(&d);
	return 0;
}

// Swapping open and close reverses time; the exposure integral must
// be unchanged (uses the orientation-flip scene).
static int case_time_reversal(void) {
	const int W = 8, H = 8;
	device_t d;
	double *a = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	double *b = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(a != NULL && b != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x102030;

	color_t red = { 1, 0, 0 }, green = { 0, 1, 0 }, blue = { 0, 0, 1 };
	motion_triangle t, rev;
	t.open[0] = mkvertex(1, 1, 0, red);
	t.open[1] = mkvertex(5, 1, 0, green);
	t.open[2] = mkvertex(3, 0, 0, blue);
	t.close[0] = mkvertex(1, 1, 0, red);
	t.close[1] = mkvertex(5, 1, 0, green);
	t.close[2] = mkvertex(3, 2, 0, blue);
	for (int k = 0; k < 3; k++) {
		rev.open[k] = t.close[k];
		rev.close[k] = t.open[k];
	}

	TASK_CHECK(motion_render(&d, &t, 1, a) == 1);
	TASK_CHECK(motion_render(&d, &rev, 1, b) == 1);
	for (int i = 0; i < W * H * 3; i++)
		TASK_CHECK(fabs(a[i] - b[i]) < 1e-10);

	free(a);
	free(b);
	device_destroy(&d);
	return 0;
}

//---------------------------------------------------------------------
// integrals group
//---------------------------------------------------------------------

// Stationary triangle: the integral equals the instantaneous
// barycentric colour, and non-constant vertex colours are exercised.
static int case_static_bary_color(void) {
	const int W = 8, H = 8;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x000000;

	color_t red = { 1, 0, 0 }, green = { 0, 1, 0 }, blue = { 0, 0, 1 };
	motion_triangle t;
	t.open[0] = mkvertex(0, 0, 0, red);
	t.open[1] = mkvertex(8, 0, 0, green);
	t.open[2] = mkvertex(0, 8, 0, blue);
	for (int k = 0; k < 3; k++) t.close[k] = t.open[k];

	TASK_CHECK(motion_render(&d, &t, 1, rgb) == 1);

	// Pixel (2,3) centre (2.5,3.5): weights (1/4, 5/16, 7/16).
	// A = 64; l0 = 16/64, l1 = 20/64, l2 = 28/64.
	TASK_CHECK(fabs(pix(rgb, W, 2, 3, 0) - 0.25) < 1e-9);
	TASK_CHECK(fabs(pix(rgb, W, 2, 3, 1) - (5.0 / 16.0)) < 1e-9);
	TASK_CHECK(fabs(pix(rgb, W, 2, 3, 2) - (7.0 / 16.0)) < 1e-9);

	free(rgb);
	device_destroy(&d);
	return 0;
}

// Two faces with identical, static screen footprints exchange depth.
// Per-vertex depths differ and are dyadic; the crossing at the pixel
// is t = 15/26, whereas the face-centre crossing is t = 1/2, so the
// event cannot be derived from centre depths.
static int case_depth_swap_pixel(void) {
	const int W = 8, H = 6;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x000000;

	color_t greenc = { 0, 1, 0 }, redc = { 1, 0, 0 };
	motion_triangle tr[2];
	// Face 0 (red): screen triangle (0,0),(8,0),(0,6), static depths.
	tr[0].open[0] = mkvertex(0, 0, 0.25f, redc);
	tr[0].open[1] = mkvertex(8, 0, 0.75f, redc);
	tr[0].open[2] = mkvertex(0, 6, 0.5f, redc);
	for (int k = 0; k < 3; k++) tr[0].close[k] = tr[0].open[k];
	// Face 1 (green): same footprint; vertex 0 depth rises 0.25 -> 0.75.
	tr[1].open[0] = mkvertex(0, 0, 0.25f, greenc);
	tr[1].open[1] = mkvertex(8, 0, 0.5f, greenc);
	tr[1].open[2] = mkvertex(0, 6, 0.5f, greenc);
	tr[1].close[0] = mkvertex(0, 0, 0.75f, greenc);
	tr[1].close[1] = tr[1].open[1];
	tr[1].close[2] = tr[1].open[2];

	TASK_CHECK(motion_render(&d, tr, 2, rgb) == 1);

	// Pixel (2,2): bary weights 13/48, 15/48, 20/48.  Green wins on
	// [0, 15/26], red on [15/26, 1].
	TASK_CHECK(fabs(pix(rgb, W, 2, 2, 1) - (15.0 / 26.0)) < 1e-7);
	TASK_CHECK(fabs(pix(rgb, W, 2, 2, 0) - (11.0 / 26.0)) < 1e-7);
	TASK_CHECK(pix(rgb, W, 2, 2, 2) == 0.0);
	// The tie is a single instant and must not leave background.
	TASK_CHECK(fabs(pix(rgb, W, 2, 2, 0) + pix(rgb, W, 2, 2, 1) - 1.0)
		< 1e-7);

	free(rgb);
	device_destroy(&d);
	return 0;
}

// Depth tie over a whole interval is broken by the input index: the
// smaller index wins and no background appears.
static int case_depth_tie_index_rule(void) {
	const int W = 4, H = 4;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x000000;

	color_t redc = { 1, 0, 0 }, greenc = { 0, 1, 0 };
	motion_triangle tr[2];
	tr[0].open[0] = mkvertex(0, 0, 0.5f, redc);
	tr[0].open[1] = mkvertex(4, 0, 0.5f, redc);
	tr[0].open[2] = mkvertex(0, 4, 0.5f, redc);
	tr[1].open[0] = mkvertex(0, 0, 0.5f, greenc);
	tr[1].open[1] = mkvertex(4, 0, 0.5f, greenc);
	tr[1].open[2] = mkvertex(0, 4, 0.5f, greenc);
	for (int k = 0; k < 3; k++) {
		tr[0].close[k] = tr[0].open[k];
		tr[1].close[k] = tr[1].open[k];
	}

	TASK_CHECK(motion_render(&d, tr, 2, rgb) == 1);
	// Index 0 (red) wins for the whole exposure wherever covered;
	// the uncovered corners stay on the black background.
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			double s = (double)x + 0.5 + (double)y + 0.5;
			if (s <= 4.5) {
				TASK_CHECK(fabs(pix(rgb, W, x, y, 0) - 1.0) < 1e-9);
				TASK_CHECK(pix(rgb, W, x, y, 1) == 0.0);
			} else {
				TASK_CHECK(pix(rgb, W, x, y, 0) == 0.0);
				TASK_CHECK(pix(rgb, W, x, y, 1) == 0.0);
			}
		}

	free(rgb);
	device_destroy(&d);
	return 0;
}

//---------------------------------------------------------------------
// pipeline group
//---------------------------------------------------------------------

static int case_background_only(void) {
	const int W = 5, H = 4;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x203040;

	TASK_CHECK(motion_render(&d, NULL, 0, rgb) == 1);
	double er = 0x20 / 255.0, eg = 0x30 / 255.0, eb = 0x40 / 255.0;
	for (int i = 0; i < W * H; i++) {
		TASK_CHECK(fabs(rgb[i * 3] - er) < 1e-12);
		TASK_CHECK(fabs(rgb[i * 3 + 1] - eg) < 1e-12);
		TASK_CHECK(fabs(rgb[i * 3 + 2] - eb) < 1e-12);
	}
	// Quantised framebuffer reproduces the background exactly.
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			TASK_CHECK(d.framebuffer[y][x] == 0x203040);

	free(rgb);
	device_destroy(&d);
	return 0;
}

static int case_invalid_arguments(void) {
	device_t d;
	double rgb[12];
	device_init(&d, 2, 2, NULL);
	motion_triangle one;
	memset(&one, 0, sizeof(one));
	TASK_CHECK(motion_render(NULL, &one, 1, rgb) == 0);
	TASK_CHECK(motion_render(&d, NULL, 1, rgb) == 0);
	TASK_CHECK(motion_render(&d, &one, 1, NULL) == 0);
	device_destroy(&d);
	return 0;
}

// One quantisation level and the once-only floor(255c+0.5) rule.
static int case_quantization(void) {
	const int W = 4, H = 4;
	device_t d;
	double *rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(rgb != NULL);
	device_init(&d, W, H, NULL);
	set_pixel_transform(&d);
	d.background = 0x000000;

	color_t half = { 0.5f, 0.5f, 0.5f };
	color_t third = { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f };
	motion_triangle tr[2];
	// T0 covers the top-left block (x+y <= 2) with uniform colour 1/2.
	tr[0].open[0] = mkvertex(-1, -1, 0, half);
	tr[0].open[1] = mkvertex(3, -1, 0, half);
	tr[0].open[2] = mkvertex(-1, 3, 0, half);
	// T1 covers a separate block around pixel (3,3), colour 1/3.
	tr[1].open[0] = mkvertex(2, 2, 0, third);
	tr[1].open[1] = mkvertex(6, 2, 0, third);
	tr[1].open[2] = mkvertex(2, 6, 0, third);
	for (int k = 0; k < 3; k++) {
		tr[0].close[k] = tr[0].open[k];
		tr[1].close[k] = tr[1].open[k];
	}

	TASK_CHECK(motion_render(&d, tr, 2, rgb) == 1);
	// Pixel (0,0) centre (.5,.5): face 0 colour 1/2 -> 128.
	TASK_CHECK(d.framebuffer[0][0] == 0x808080);
	// Pixel (3,3) centre (3.5,3.5), strictly inside face 1:
	// floor(255/3 + 0.5) = floor(85.5...) = 85.
	TASK_CHECK((d.framebuffer[3][3] & 255) == 85);
	TASK_CHECK(((d.framebuffer[3][3] >> 8) & 255) == 85);
	TASK_CHECK(((d.framebuffer[3][3] >> 16) & 255) == 85);
	// rgb stays the unquantised integral.
	TASK_CHECK(fabs(rgb[0] - 0.5) < 1e-9);

	free(rgb);
	device_destroy(&d);
	return 0;
}

// The renderer must not touch the zbuffer, render_state, texture,
// matrices or its input; repeated and interleaved calls are isolated.
static int case_device_state_isolation(void) {
	const int W = 6, H = 6;
	device_t a, b;
	double *ra = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	double *rb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	double *ra2 = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	TASK_CHECK(ra != NULL && rb != NULL && ra2 != NULL);
	device_init(&a, W, H, NULL);
	device_init(&b, W, H, NULL);
	set_pixel_transform(&a);
	set_pixel_transform(&b);
	a.background = b.background = 0x112233;
	a.render_state = RENDER_STATE_WIREFRAME;
	b.render_state = RENDER_STATE_TEXTURE;

	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			a.zbuffer[y][x] = -777.0f;

	color_t red = { 1, 0, 0 }, blue = { 0, 0, 1 };
	motion_triangle ta, tb;
	ta.open[0] = mkvertex(0, 0, 0.2f, red);
	ta.open[1] = mkvertex(5, 0, 0.2f, red);
	ta.open[2] = mkvertex(0, 5, 0.2f, red);
	tb.open[0] = mkvertex(1, 1, 0.1f, blue);
	tb.open[1] = mkvertex(6, 1, 0.1f, blue);
	tb.open[2] = mkvertex(1, 6, 0.1f, blue);
	for (int k = 0; k < 3; k++) {
		ta.close[k] = ta.open[k];
		tb.close[k] = tb.open[k];
	}
	motion_triangle ta_snapshot = ta;

	TASK_CHECK(motion_render(&a, &ta, 1, ra) == 1);
	TASK_CHECK(motion_render(&b, &tb, 1, rb) == 1);
	TASK_CHECK(motion_render(&a, &ta, 1, ra2) == 1);

	// Repeated call on the same device is deterministic.
	for (int i = 0; i < W * H * 3; i++)
		TASK_CHECK(ra[i] == ra2[i]);
	// Inputs are untouched.
	TASK_CHECK(memcmp(&ta, &ta_snapshot, sizeof(ta)) == 0);
	// zbuffer never read or written, render_state / background kept.
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			TASK_CHECK(a.zbuffer[y][x] == -777.0f);
	TASK_CHECK(a.render_state == RENDER_STATE_WIREFRAME);
	TASK_CHECK(b.render_state == RENDER_STATE_TEXTURE);
	TASK_CHECK(a.background == 0x112233);
	// Devices do not share framebuffer contents: b's blue triangle
	// covers (4,4) while a leaves it at the background colour.
	TASK_CHECK(pix(rb, W, 4, 4, 2) > 0.0);
	TASK_CHECK(fabs(pix(ra, W, 4, 4, 0) - (0x11 / 255.0)) < 1e-9);
	TASK_CHECK(fabs(pix(ra, W, 4, 4, 1) - (0x22 / 255.0)) < 1e-9);
	TASK_CHECK(fabs(pix(ra, W, 4, 4, 2) - (0x33 / 255.0)) < 1e-9);

	free(ra);
	free(rb);
	free(ra2);
	device_destroy(&a);
	device_destroy(&b);
	return 0;
}

// The classic single-frame entry point keeps its original behaviour.
static int case_legacy_draw_primitive(void) {
	device_t d;
	device_init(&d, 32, 32, NULL);
	device_clear(&d, 0);
	d.render_state = RENDER_STATE_COLOR;
	vertex_t a = { { -.8f, -.8f, 2, 1 }, { 0, 0 }, { 1, 0, 0 }, 0 };
	vertex_t b = { { .8f, -.8f, 2, 1 }, { 0, 0 }, { 1, 0, 0 }, 0 };
	vertex_t c = { { 0, .8f, 2, 1 }, { 0, 0 }, { 1, 0, 0 }, 0 };
	device_draw_primitive(&d, &a, &b, &c);
	TASK_CHECK(d.framebuffer[16][16] == 0xff0000);
	int n = 0;
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
			if (d.zbuffer[y][x] > 0.0f) n++;
	TASK_CHECK(n > 30);
	device_destroy(&d);
	return 0;
}

const struct task_case task_cases[] = {
	{ "events", "entry_exit_window", case_entry_exit_window },
	{ "events", "brief_traversal", case_brief_traversal },
	{ "events", "midpoint_degenerate", case_midpoint_degenerate },
	{ "events", "zero_area_ignored", case_zero_area_ignored },
	{ "events", "shared_edge_grid", case_shared_edge_grid },
	{ "events", "time_reversal", case_time_reversal },
	{ "integrals", "static_bary_color", case_static_bary_color },
	{ "integrals", "depth_swap_pixel", case_depth_swap_pixel },
	{ "integrals", "depth_tie_index_rule", case_depth_tie_index_rule },
	{ "pipeline", "background_only", case_background_only },
	{ "pipeline", "invalid_arguments", case_invalid_arguments },
	{ "pipeline", "quantization", case_quantization },
	{ "pipeline", "device_state_isolation", case_device_state_isolation },
	{ "pipeline", "legacy_draw_primitive", case_legacy_draw_primitive },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
