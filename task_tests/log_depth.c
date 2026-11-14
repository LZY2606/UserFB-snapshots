/*
 * Regression tests for the optional floating point logarithmic depth
 * buffer. Every case drives real TinyGL geometry; nothing here pokes the
 * rasterizer with hand-built ZBufferPoints.
 *
 * Groups:
 *   geometry   - encoding, coverage, perspective texture and range rejects
 *   visibility - far/near ordering, point/line occlusion, test and mask
 *   lifecycle  - API validation, mode switches, clear depth, resize, close
 */

#include <GL/gl.h>
#include <zbuffer.h>
#include "../task_support/check.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static const PIXEL PIX_BLACK = 0;

static int is_red(PIXEL p) { return GET_RED(p) > 0x80 && GET_GREEN(p) < 0x40 && GET_BLUE(p) < 0x40; }
static int is_green(PIXEL p) { return GET_GREEN(p) > 0x80 && GET_RED(p) < 0x40 && GET_BLUE(p) < 0x40; }
static int is_blue(PIXEL p) { return GET_BLUE(p) > 0x80 && GET_RED(p) < 0x40 && GET_GREEN(p) < 0x40; }

static PIXEL pixel_at(ZBuffer* zb, int x, int y) { return zb->pbuf[y * zb->xsize + x]; }

static void use_frustum(float nearv, float farv) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glFrustum(-1.0, 1.0, -1.0, 1.0, nearv, farv);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

/* full view quad on the z=-d plane (d positive) inside a symmetric frustum */
static void quad_at(float d, float nearv) {
	float hw = 0.8f * d / nearv;
	float z = -d;
	glBegin(GL_TRIANGLE_STRIP);
	glVertex3f(-hw, -hw, z);
	glVertex3f(hw, -hw, z);
	glVertex3f(-hw, hw, z);
	glVertex3f(hw, hw, z);
	glEnd();
}

/* emit a vertex whose x/y are normalized device coordinates ([-1,1]) at
   eye distance d, accounting for the frustum near plane scale */
static void ndc_vertex(float nx, float ny, float d, float nearv) {
	glVertex3f(nx * d / nearv, ny * d / nearv, -d);
}

static ZBuffer* fresh_context(int w, int h) {
	ZBuffer* zb = ZB_open(w, h, ZB_MODE_RGBA, 0);
	if (!zb)
		return 0;
	glInit(zb);
	glViewport(0, 0, w, h);
	glEnable(GL_DEPTH_TEST);
	glClearColor(0, 0, 0, 0);
	/* far clear: under larger-D-wins this stores D=0 so any fragment
	   inside [near,far] can pass; matches standard glClearDepth(1.0) */
	glClearDepth(1.0f);
	return zb;
}

static int count_color(ZBuffer* zb, int (*match)(PIXEL)) {
	int i, n = 0, total = zb->xsize * zb->ysize;
	for (i = 0; i < total; i++)
		if (match(zb->pbuf[i]))
			n++;
	return n;
}

/* ------------------------------------------------------------------ */
/* geometry group                                                      */
/* ------------------------------------------------------------------ */

static int geo_flat_covers(void) {
	ZBuffer* zb = fresh_context(W, H);
	TASK_CHECK(zb != 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 1000.0f) == 1);
	use_frustum(1.0f, 1000.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	quad_at(10.0f, 1.0f);
	TASK_CHECK(count_color(zb, is_red) > W * H / 4);
	/* the float store must hold encoded values where the triangle drew */
	TASK_CHECK(zb->log_depth[H / 2 * zb->xsize + W / 2] > 0.0f);
	glClose();
	ZB_close(zb);
	return 0;
}

static int geo_smooth_covers(void) {
	ZBuffer* zb = fresh_context(W, H);
	TASK_CHECK(zb != 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 1000.0f) == 1);
	use_frustum(1.0f, 1000.0f);
	glShadeModel(GL_SMOOTH);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glColor3f(1, 0, 0);
	ndc_vertex(-0.9f, -0.9f, 10.0f, 1.0f);
	glColor3f(0, 1, 0);
	ndc_vertex(0.9f, -0.9f, 10.0f, 1.0f);
	glColor3f(0, 0, 1);
	ndc_vertex(0.0f, 0.9f, 10.0f, 1.0f);
	glEnd();
	/* all three vertex colors must survive the smooth interpolation */
	TASK_CHECK(count_color(zb, is_red) > 5);
	TASK_CHECK(count_color(zb, is_green) > 5);
	TASK_CHECK(count_color(zb, is_blue) > 5);
	TASK_CHECK(zb->log_depth[H / 2 * zb->xsize + W / 2] > 0.0f);
	glClose();
	ZB_close(zb);
	return 0;
}

static void load_half_texture(void) {
	static GLubyte pixels[256 * 256 * 3];
	int x, y;
	for (y = 0; y < 256; y++)
		for (x = 0; x < 256; x++) {
			GLubyte* px = &pixels[(y * 256 + x) * 3];
			if (x < 128) {
				px[0] = 255;
				px[1] = 0;
				px[2] = 0;
			} else {
				px[0] = 0;
				px[1] = 255;
				px[2] = 0;
			}
		}
	glBindTexture(GL_TEXTURE_2D, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
	glEnable(GL_TEXTURE_2D);
}

static int geo_textured_quad(void) {
	ZBuffer* zb = fresh_context(W, H);
	TASK_CHECK(zb != 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 100.0f) == 1);
	use_frustum(1.0f, 100.0f);
	load_half_texture();
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glBegin(GL_TRIANGLE_STRIP);
	glTexCoord2f(0, 0);
	ndc_vertex(-0.8f, -0.8f, 10.0f, 1.0f);
	glTexCoord2f(1, 0);
	ndc_vertex(0.8f, -0.8f, 10.0f, 1.0f);
	glTexCoord2f(0, 1);
	ndc_vertex(-0.8f, 0.8f, 10.0f, 1.0f);
	glTexCoord2f(1, 1);
	ndc_vertex(0.8f, 0.8f, 10.0f, 1.0f);
	glEnd();
	TASK_CHECK(count_color(zb, is_red) > 10);
	TASK_CHECK(count_color(zb, is_green) > 10);
	TASK_CHECK(zb->log_depth[H / 2 * zb->xsize + W / 2] > 0.0f);
	glClose();
	ZB_close(zb);
	return 0;
}

static int geo_encoded_value_matches_formula(void) {
	ZBuffer* zb = fresh_context(W, H);
	float expected;
	int i, found = 0;
	TASK_CHECK(zb != 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 100.0f) == 1);
	use_frustum(0.5f, 200.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 1, 1);
	glBegin(GL_POINTS);
	glVertex3f(0, 0, -10.0f); /* d = 10 */
	glEnd();
	expected = 1.0f - logf(10.0f) / logf(100.0f); /* 0.5 */
	for (i = 0; i < W * H; i++)
		if (zb->log_depth[i] != 0.0f) {
			TASK_CHECK(fabsf(zb->log_depth[i] - expected) < 1e-5f);
			found++;
		}
	TASK_CHECK(found == 1); /* a point fills exactly one fragment */
	glClose();
	ZB_close(zb);
	return 0;
}

static int geo_outside_range_rejected(void) {
	ZBuffer* zb = fresh_context(W, H);
	int idx = H / 2 * W + W / 2;
	TASK_CHECK(zb != 0);
	/* frustum wider than the logarithmic range so fragments reach the
	   rasterizer even though their distance is outside [near,far] */
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 100.0f) == 1);
	use_frustum(0.1f, 1000.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	quad_at(500.0f, 0.1f); /* beyond far: rejected */
	TASK_CHECK(zb->pbuf[idx] == PIX_BLACK);
	TASK_CHECK(zb->log_depth[idx] == 0.0f);
	glColor3f(0, 1, 0);
	quad_at(0.5f, 0.1f); /* nearer than near: rejected */
	TASK_CHECK(zb->pbuf[idx] == PIX_BLACK);
	TASK_CHECK(zb->log_depth[idx] == 0.0f);
	glColor3f(0, 0, 1);
	quad_at(10.0f, 0.1f); /* inside: drawn */
	TASK_CHECK(is_blue(zb->pbuf[idx]));
	TASK_CHECK(zb->log_depth[idx] > 0.0f);
	glClose();
	ZB_close(zb);
	return 0;
}

/* ------------------------------------------------------------------ */
/* visibility group                                                    */
/* ------------------------------------------------------------------ */

/* Two distant surfaces collapse onto the same 16-bit window z but stay
   separable as floats; swapping the draw order must not change the winner
   while logarithmic depth is active. */
static int vis_far_near_ordering(void) {
	ZBuffer* zb = fresh_context(W, H);
	int idx = H / 2 * W + W / 2;
	TASK_CHECK(zb != 0);
	use_frustum(1.0f, 1000000.0f);

	/* integer mode first: the depth ratio difference is below one window
	   z step, so the far surface ties and overwrites the near one */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 0, 1);
	quad_at(500000.0f, 1.0f); /* near */
	glColor3f(1, 0, 0);
	quad_at(500005.0f, 1.0f); /* far, same window z */
	TASK_CHECK(is_red(zb->pbuf[idx]));

	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 1000000.0f) == 1);

	/* near then far: near must win */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 0, 1);
	quad_at(500000.0f, 1.0f);
	glColor3f(1, 0, 0);
	quad_at(500005.0f, 1.0f);
	TASK_CHECK(is_blue(zb->pbuf[idx]));

	/* far then near: near still wins */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	quad_at(500005.0f, 1.0f);
	glColor3f(0, 0, 1);
	quad_at(500000.0f, 1.0f);
	TASK_CHECK(is_blue(zb->pbuf[idx]));
	glClose();
	ZB_close(zb);
	return 0;
}

static int vis_points_and_lines_occlude(void) {
	ZBuffer* zb = fresh_context(W, H);
	int idx = H / 2 * W + W / 2;
	float nearv = 1.0f;
	float near_d;
	TASK_CHECK(zb != 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 1000.0f) == 1);
	use_frustum(nearv, 1000.0f);

	/* near point beats a far quad: the point writes exactly one red
	   fragment whose stored depth encodes the near distance */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 0, 1);
	quad_at(100.0f, nearv);
	glColor3f(1, 0, 0);
	glBegin(GL_POINTS);
	glVertex3f(0, 0, -10.0f);
	glEnd();
	TASK_CHECK(count_color(zb, is_red) == 1);
	TASK_CHECK(count_color(zb, is_blue) > 10);
	near_d = ZB_logEncode(zb, 10.0f);
	{
		int i, matched = 0;
		for (i = 0; i < W * H; i++)
			if (is_red(zb->pbuf[i])) {
				TASK_CHECK(fabsf(zb->log_depth[i] - near_d) < 1e-5f);
				matched++;
			}
		TASK_CHECK(matched == 1);
	}

	/* far line loses against a near quad: no red fragment survives */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 0, 1);
	quad_at(10.0f, nearv);
	glColor3f(1, 0, 0);
	glBegin(GL_LINES);
	glVertex3f(-0.6f * 100.0f, 0, -100.0f);
	glVertex3f(0.6f * 100.0f, 0, -100.0f);
	glEnd();
	TASK_CHECK(count_color(zb, is_red) == 0);
	TASK_CHECK(is_blue(zb->pbuf[idx]));

	/* near line wins against a far quad: green fragments appear */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	quad_at(100.0f, nearv);
	glColor3f(0, 1, 0);
	glBegin(GL_LINES);
	glVertex3f(-0.6f * 10.0f, 0, -10.0f);
	glVertex3f(0.6f * 10.0f, 0, -10.0f);
	glEnd();
	TASK_CHECK(count_color(zb, is_green) > 5);
	TASK_CHECK(count_color(zb, is_red) > 5);
	glClose();
	ZB_close(zb);
	return 0;
}

static int vis_depth_test_off_paints(void) {
	ZBuffer* zb = fresh_context(W, H);
	int idx = H / 2 * W + W / 2;
	TASK_CHECK(zb != 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 1000.0f) == 1);
	use_frustum(1.0f, 1000.0f);
	glDisable(GL_DEPTH_TEST);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 0, 1);
	quad_at(10.0f, 1.0f); /* near */
	glColor3f(1, 0, 0);
	quad_at(100.0f, 1.0f); /* far, but test is off so it still paints */
	TASK_CHECK(is_red(zb->pbuf[idx]));
	glClose();
	ZB_close(zb);
	return 0;
}

static int vis_depth_mask_gates_float_store(void) {
	ZBuffer* zb = fresh_context(W, H);
	int idx = H / 2 * W + W / 2;
	TASK_CHECK(zb != 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 1000.0f) == 1);
	use_frustum(1.0f, 1000.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glColor3f(0, 0, 1);
	quad_at(100.0f, 1.0f); /* far, writes depth */
	glDepthMask(GL_FALSE);
	glColor3f(1, 0, 0);
	quad_at(10.0f, 1.0f); /* near, paints color but must not write depth */
	TASK_CHECK(is_red(zb->pbuf[idx]));
	glDepthMask(GL_TRUE);
	glColor3f(0, 1, 0);
	quad_at(50.0f, 1.0f); /* middle: stored depth is still the far one, so it passes */
	TASK_CHECK(is_green(zb->pbuf[idx]));
	glClose();
	ZB_close(zb);
	return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle group                                                     */
/* ------------------------------------------------------------------ */

static int life_parameter_validation(void) {
	ZBuffer* zb = fresh_context(W, H);
	TASK_CHECK(zb != 0);
	TASK_CHECK(zb->log_depth == 0);
	/* invalid requests are rejected and keep the current mode */
	TASK_CHECK(ZB_setLogDepth(zb, -1.0f, 10.0f) == 0);
	TASK_CHECK(ZB_setLogDepth(zb, 0.0f, 10.0f) == 0);
	TASK_CHECK(ZB_setLogDepth(zb, 10.0f, 10.0f) == 0);
	TASK_CHECK(ZB_setLogDepth(zb, 5.0f, 2.0f) == 0);
	TASK_CHECK(ZB_setLogDepth(zb, INFINITY, 10.0f) == 0);
	TASK_CHECK(ZB_setLogDepth(zb, NAN, 10.0f) == 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 0.0f) == 0);
	TASK_CHECK(zb->log_depth == 0);

	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 100.0f) == 1);
	TASK_CHECK(zb->log_depth != 0);
	/* invalid while enabled leaves the enabled range intact */
	TASK_CHECK(ZB_setLogDepth(zb, 2.0f, 1.0f) == 0);
	TASK_CHECK(zb->log_depth != 0);
	TASK_CHECK(zb->log_near == 1.0f && zb->log_far == 100.0f);

	TASK_CHECK(ZB_setLogDepth(zb, 0.0f, 0.0f) == 1);
	TASK_CHECK(zb->log_depth == 0);
	/* disabling while already disabled is still a valid request */
	TASK_CHECK(ZB_setLogDepth(zb, 0.0f, 0.0f) == 1);
	TASK_CHECK(zb->log_depth == 0);
	glClose();
	ZB_close(zb);
	return 0;
}

static int life_switch_keeps_color_resets_depth(void) {
	ZBuffer* zb = fresh_context(W, H);
	int i, total;
	int idx = H / 2 * W + W / 2;
	TASK_CHECK(zb != 0);
	use_frustum(1.0f, 1000.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	quad_at(10.0f, 1.0f);
	TASK_CHECK(is_red(zb->pbuf[idx]));

	/* enabling keeps color and resets the new float depth to 0 */
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 1000.0f) == 1);
	TASK_CHECK(is_red(zb->pbuf[idx]));
	total = zb->xsize * zb->ysize;
	for (i = 0; i < total; i++)
		TASK_CHECK(zb->log_depth[i] == 0.0f);

	/* draw something, then change ranges: depth back to 0, color kept */
	glClear(GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 0, 1);
	quad_at(20.0f, 1.0f);
	TASK_CHECK(zb->log_depth[idx] > 0.0f);
	TASK_CHECK(ZB_setLogDepth(zb, 2.0f, 500.0f) == 1);
	TASK_CHECK(zb->log_near == 2.0f && zb->log_far == 500.0f);
	TASK_CHECK(is_blue(zb->pbuf[idx]));
	for (i = 0; i < total; i++)
		TASK_CHECK(zb->log_depth[i] == 0.0f);

	/* disable: color retained, storage released */
	TASK_CHECK(ZB_setLogDepth(zb, 0.0f, 0.0f) == 1);
	TASK_CHECK(is_blue(zb->pbuf[idx]));
	TASK_CHECK(zb->log_depth == 0);
	glClose();
	ZB_close(zb);
	return 0;
}

static int life_clear_depth_mapping(void) {
	ZBuffer* zb = fresh_context(W, H);
	int idx = H / 2 * W + W / 2;
	TASK_CHECK(zb != 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 100.0f) == 1);

	glClearDepth(0.0f);
	glClear(GL_DEPTH_BUFFER_BIT);
	TASK_CHECK(fabsf(zb->log_depth[idx] - 1.0f) < 1e-6f);

	glClearDepth(0.25f);
	glClear(GL_DEPTH_BUFFER_BIT);
	TASK_CHECK(fabsf(zb->log_depth[idx] - 0.75f) < 1e-6f);

	glClearDepth(1.0f);
	glClear(GL_DEPTH_BUFFER_BIT);
	TASK_CHECK(fabsf(zb->log_depth[idx] - 0.0f) < 1e-6f);

	/* out of range clear values are clamped before mapping */
	glClearDepth(2.0f);
	glClear(GL_DEPTH_BUFFER_BIT);
	TASK_CHECK(fabsf(zb->log_depth[idx] - 0.0f) < 1e-6f);
	glClose();
	ZB_close(zb);
	return 0;
}

static int life_resize_keeps_mode(void) {
	ZBuffer* zb = fresh_context(W, H);
	int idx;
	TASK_CHECK(zb != 0);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 1000.0f) == 1);
	use_frustum(1.0f, 1000.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	quad_at(10.0f, 1.0f);
	TASK_CHECK(zb->log_depth[H / 2 * W + W / 2] > 0.0f);

	ZB_resize(zb, 0, 48, 48);
	/* mode and range survive, depth storage is rebuilt and cleared */
	TASK_CHECK(zb->log_depth != 0);
	TASK_CHECK(zb->log_near == 1.0f && zb->log_far == 1000.0f);
	TASK_CHECK(zb->xsize == 48 && zb->ysize == 48);
	TASK_CHECK(zb->log_depth[0] == 0.0f);

	glViewport(0, 0, 48, 48);
	use_frustum(1.0f, 1000.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 0, 1);
	quad_at(10.0f, 1.0f);
	idx = 24 * zb->xsize + 24;
	TASK_CHECK(is_blue(zb->pbuf[idx]));
	TASK_CHECK(zb->log_depth[idx] > 0.0f);
	glClose();
	ZB_close(zb);
	return 0;
}

static int life_open_close_cycle(void) {
	ZBuffer* zb;
	zb = ZB_open(W, H, ZB_MODE_RGBA, 0);
	TASK_CHECK(zb != 0);
	glInit(zb);
	glViewport(0, 0, W, H);
	glEnable(GL_DEPTH_TEST);
	glClearDepth(1.0f);
	TASK_CHECK(ZB_setLogDepth(zb, 1.0f, 100.0f) == 1);
	use_frustum(1.0f, 100.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(1, 0, 0);
	quad_at(10.0f, 1.0f);
	TASK_CHECK(count_color(zb, is_red) > 10);
	glClose();
	ZB_close(zb);

	/* a fresh context must come up in the default integer mode */
	zb = ZB_open(W, H, ZB_MODE_RGBA, 0);
	TASK_CHECK(zb != 0);
	TASK_CHECK(zb->log_depth == 0);
	glInit(zb);
	glViewport(0, 0, W, H);
	glEnable(GL_DEPTH_TEST);
	use_frustum(1.0f, 100.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor3f(0, 1, 0);
	quad_at(10.0f, 1.0f);
	TASK_CHECK(count_color(zb, is_green) > 10);
	glClose();
	ZB_close(zb);
	return 0;
}

const struct task_case task_cases[] = {
	{"geometry", "flat_triangle_covers", geo_flat_covers},
	{"geometry", "smooth_triangle_covers", geo_smooth_covers},
	{"geometry", "textured_quad_samples", geo_textured_quad},
	{"geometry", "encoded_depth_matches_formula", geo_encoded_value_matches_formula},
	{"geometry", "fragments_outside_range_rejected", geo_outside_range_rejected},

	{"visibility", "far_near_surface_reordering", vis_far_near_ordering},
	{"visibility", "points_and_lines_participate", vis_points_and_lines_occlude},
	{"visibility", "depth_test_off_still_paints", vis_depth_test_off_paints},
	{"visibility", "depth_mask_gates_float_store", vis_depth_mask_gates_float_store},

	{"lifecycle", "set_log_depth_parameter_validation", life_parameter_validation},
	{"lifecycle", "switch_keeps_color_resets_depth", life_switch_keeps_color_resets_depth},
	{"lifecycle", "clear_depth_float_mapping", life_clear_depth_mapping},
	{"lifecycle", "resize_keeps_mode_and_redraws", life_resize_keeps_mode},
	{"lifecycle", "open_close_cycle", life_open_close_cycle},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
