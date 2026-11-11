/*
 * stencil_pipeline.c
 *
 * End to end checks for the 8 bit per pixel stencil buffer added to the
 * TinyGL filled triangle pipeline. Every assertion inspects the real
 * color / depth / stencil pixels; no external image compositing is used.
 */

#include "GL/gl.h"
#include "zbuffer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

/* full intensity colors come out as 0xFE per channel because of the
   COLOR_CORRECTED_MULT_MASK quantization in the engine */
#define RED 0x00fe0000
#define GREEN 0x0000fe00
#define BLUE 0x000000fe
#define BLACK 0x00000000
#define MAGENTA 0x0000ff00ffu

static ZBuffer* zb;

static int pix_index(int x, int y) {
	return y * zb->xsize + x;
}

static GLint geti(GLenum name) {
	GLint v = -123456;
	glGetIntegerv(name, &v);
	return v;
}

static void clear_all(void) {
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

/* full screen quad at a fixed eye depth */
static void quad_z(float z0, float r, float g, float b) {
	glColor3f(r, g, b);
	glBegin(GL_QUADS);
	glVertex3f(-1.0f, -1.0f, z0);
	glVertex3f(1.0f, -1.0f, z0);
	glVertex3f(1.0f, 1.0f, z0);
	glVertex3f(-1.0f, 1.0f, z0);
	glEnd();
}

/* quad spanning [x0,x1] in normalized device x, full height */
static void quad_region(float x0, float x1, float z0) {
	glBegin(GL_QUADS);
	glVertex3f(x0, -1.0f, z0);
	glVertex3f(x1, -1.0f, z0);
	glVertex3f(x1, 1.0f, z0);
	glVertex3f(x0, 1.0f, z0);
	glEnd();
}

static void reset_stencil(GLint func, GLint ref, GLuint vm, GLenum sfail, GLenum dpfail, GLenum dppass) {
	glStencilFunc(func, ref, vm);
	glStencilOp(sfail, dpfail, dppass);
	glStencilMask(0xff);
	glClearStencil(0);
}

static int count_sbuf(GLubyte value) {
	int i, n = zb->xsize * zb->ysize, c = 0;
	for (i = 0; i < n; i++)
		if (zb->sbuf[i] == value)
			c++;
	return c;
}

static int count_color(GLuint value) {
	int i, n = zb->xsize * zb->ysize, c = 0;
	for (i = 0; i < n; i++)
		if ((zb->pbuf[i] & 0xffffffu) == (value & 0xffffffu))
			c++;
	return c;
}

static void test_defaults_and_disabled(void) {
	GLint i;

	/* the stencil buffer now really exists */
	assert(zb->sbuf != NULL);
	assert(geti(GL_STENCIL_BITS) == 8);
	assert(geti(GL_STENCIL_TEST) == 0);
	assert(geti(GL_STENCIL_FUNC) == GL_ALWAYS);
	assert(geti(GL_STENCIL_REF) == 0);
	assert(geti(GL_STENCIL_VALUE_MASK) == 255);
	assert(geti(GL_STENCIL_WRITEMASK) == 255);
	assert(geti(GL_STENCIL_FAIL) == GL_KEEP);
	assert(geti(GL_STENCIL_PASS_DEPTH_FAIL) == GL_KEEP);
	assert(geti(GL_STENCIL_PASS_DEPTH_PASS) == GL_KEEP);
	assert(geti(GL_STENCIL_CLEAR_VALUE) == 0);

	/* disabled stencil must leave the legacy rendering path untouched */
	clear_all();
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_DEPTH_TEST);
	quad_z(0.0f, 1, 0, 0);
	assert(count_color(RED) > 100);
	for (i = 0; i < zb->xsize * zb->ysize; i++)
		assert(zb->sbuf[i] == 0);
}

/* sfail / dpfail / dppass with real color, zbuf and sbuf checks.
   In this engine the stored depth grows toward the viewer inside the
   normalized volume: NDC z=0 stores 32768, z=-0.5 stores 49152 (nearer,
   passes the >= test) and z=+0.5 stores 16384 (farther, fails). */
static void test_three_outcomes(void) {
	int p = pix_index(10, 10);
	GLushort z_first;

	clear_all();
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glEnable(GL_STENCIL_TEST);

	/* pass 1: always pass, replace with 1, geometry at z=0 */
	reset_stencil(GL_ALWAYS, 1, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	assert((zb->pbuf[p] & 0xffffffu) == RED);
	assert(zb->sbuf[p] == 1);
	z_first = zb->zbuf[p];
	assert(z_first == 32768);

	/* dpfail: stencil equal passes, geometry at z=+0.5 is farther and
	   loses the depth test; only dpfail (INCR) runs, color/depth stay */
	glStencilFunc(GL_EQUAL, 1, 0xff);
	glStencilOp(GL_KEEP, GL_INCR, GL_KEEP);
	quad_z(0.5f, 0, 0, 1);
	assert((zb->pbuf[p] & 0xffffffu) == RED);
	assert(zb->zbuf[p] == z_first);
	assert(zb->sbuf[p] == 2);

	/* sfail: NEVER loses the stencil test even though the geometry is
	   nearer and would pass depth; only sfail (INCR) runs */
	glStencilFunc(GL_NEVER, 0, 0xff);
	glStencilOp(GL_INCR, GL_KEEP, GL_KEEP);
	quad_z(-0.5f, 0, 1, 0);
	assert((zb->pbuf[p] & 0xffffffu) == RED);
	assert(zb->zbuf[p] == z_first);
	assert(zb->sbuf[p] == 3);

	/* dppass: both pass, dppass REPLACE writes ref 5, color/depth update */
	glStencilFunc(GL_ALWAYS, 5, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(-0.5f, 0, 1, 0);
	assert((zb->pbuf[p] & 0xffffffu) == GREEN);
	assert(zb->zbuf[p] == 49152);
	assert(zb->zbuf[p] > z_first);
	assert(zb->sbuf[p] == 5);

	/* depth write mask only gates depth storage, not op selection */
	glDepthMask(GL_FALSE);
	glStencilFunc(GL_ALWAYS, 6, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(-0.5f, 0, 0, 1); /* nearer, depth pass */
	assert((zb->pbuf[p] & 0xffffffu) == BLUE);
	assert(zb->sbuf[p] == 6);

	/* depth failing while depth writes are off still runs dpfail */
	{
		GLushort z_kept = zb->zbuf[p];
		glStencilFunc(GL_ALWAYS, 0, 0xff);
		glStencilOp(GL_KEEP, GL_ZERO, GL_KEEP);
		quad_z(0.5f, 1, 1, 1); /* far, depth fail */
		assert(zb->zbuf[p] == z_kept);
		assert(zb->sbuf[p] == 0);
	}
	glDepthMask(GL_TRUE);
	glDisable(GL_DEPTH_TEST);
}

/* value mask compares, write mask merges, only low 8 bits count */
static void test_masks(void) {
	int p = pix_index(12, 12);

	clear_all();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);

	/* plant 0x0F everywhere */
	reset_stencil(GL_ALWAYS, 0x0f, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 0x0f);

	/* value mask 0x0F: ref 0xFF compares as 0x0F == stored 0x0F -> pass */
	glStencilFunc(GL_EQUAL, 0xff, 0x0f);
	glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
	quad_z(0.0f, 0, 1, 0);
	assert(zb->sbuf[p] == 0);

	/* rebuild 0x0F, then ref 0xF0 under the same mask compares 0 != 0x0F */
	reset_stencil(GL_ALWAYS, 0x0f, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	glStencilFunc(GL_EQUAL, 0xf0, 0x0f);
	glStencilOp(GL_INCR, GL_KEEP, GL_KEEP); /* sfail would increment */
	quad_z(0.0f, 0, 1, 0);
	assert(zb->sbuf[p] == 0x10);

	/* write mask merge: only the low nibble of the result lands */
	reset_stencil(GL_ALWAYS, 0, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 0, 0, 1);
	assert(zb->sbuf[p] == 0);
	glStencilMask(0x0f);
	glStencilFunc(GL_ALWAYS, 0xff, 0xff);
	quad_z(0.0f, 0, 0, 1);
	assert(zb->sbuf[p] == 0x0f);
	glStencilMask(0xf0);
	quad_z(0.0f, 0, 0, 1);
	assert(zb->sbuf[p] == 0xff);

	/* masks keep only the low 8 bits */
	glStencilMask(0x1ff); /* -> 0xff */
	glStencilFunc(GL_ALWAYS, 0x104, 0x10f); /* ref 260 clamps to 255, mask -> 0x0f */
	assert(geti(GL_STENCIL_WRITEMASK) == 0xff);
	assert(geti(GL_STENCIL_REF) == 255);
	assert(geti(GL_STENCIL_VALUE_MASK) == 0x0f);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 1, 1);
	assert(zb->sbuf[p] == 255);

	/* the reference is clamped, not wrapped: negatives go to 0, >255 to 255 */
	glStencilFunc(GL_ALWAYS, -5, 0xff);
	assert(geti(GL_STENCIL_REF) == 0);
	glStencilFunc(GL_ALWAYS, 300, 0xff);
	assert(geti(GL_STENCIL_REF) == 255);
}

/* all eight comparison functions, value mask applied to both sides */
static void check_func(GLenum func, GLint ref, GLuint mask, GLint expect_pass) {
	int p = pix_index(32, 32);
	clear_all();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	/* plant stored value 5 */
	reset_stencil(GL_ALWAYS, 5, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	/* ops KEEP so the stored stencil value is not disturbed */
	glStencilFunc(func, ref, mask);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	quad_z(0.0f, 0, 0, 1);
	if (expect_pass) {
		assert((zb->pbuf[p] & 0xffffffu) == BLUE);
		assert(zb->sbuf[p] == 5);
	} else {
		assert((zb->pbuf[p] & 0xffffffu) == RED);
		assert(zb->sbuf[p] == 5);
	}
}

static void test_compare_funcs(void) {
	check_func(GL_NEVER, 5, 0xff, 0);
	check_func(GL_ALWAYS, 5, 0xff, 1);
	check_func(GL_EQUAL, 5, 0xff, 1);
	check_func(GL_EQUAL, 4, 0xff, 0);
	check_func(GL_NOTEQUAL, 4, 0xff, 1);
	check_func(GL_NOTEQUAL, 5, 0xff, 0);
	check_func(GL_LESS, 4, 0xff, 1);
	check_func(GL_LESS, 5, 0xff, 0);
	check_func(GL_LEQUAL, 5, 0xff, 1);
	check_func(GL_LEQUAL, 6, 0xff, 0);
	check_func(GL_GREATER, 6, 0xff, 1);
	check_func(GL_GREATER, 5, 0xff, 0);
	check_func(GL_GEQUAL, 5, 0xff, 1);
	check_func(GL_GEQUAL, 4, 0xff, 0);
}

/* masked comparison: the same value mask applies to both the clamped
   reference and the stored value */
static void test_masked_compare(void) {
	int p = pix_index(32, 32);
	clear_all();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	reset_stencil(GL_ALWAYS, 0x0f, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 0x0f);

	/* ref 0xFF under mask 0x0F compares 0x0F == stored 0x0F -> pass */
	glStencilFunc(GL_EQUAL, 0xff, 0x0f);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	quad_z(0.0f, 0, 0, 1);
	assert((zb->pbuf[p] & 0xffffffu) == BLUE);

	/* ref 0x10 under mask 0x0F compares 0 < 0x0F -> LESS passes */
	glStencilFunc(GL_LESS, 0x10, 0x0f);
	quad_z(0.0f, 1, 1, 1);
	assert((zb->pbuf[p] & 0xffffffu) == 0x00fefefe);

	/* ref 0x1F under mask 0x0F compares 0x0F > 0x0F -> GREATER fails */
	glStencilFunc(GL_GREATER, 0x1f, 0x0f);
	quad_z(0.0f, 1, 0, 0);
	assert((zb->pbuf[p] & 0xffffffu) == 0x00fefefe); /* unchanged cyan */
}

/* INCR/DECR saturate at 0 and 255 */
static void test_saturation_and_ops(void) {
	int p = pix_index(20, 20);

	clear_all();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);

	reset_stencil(GL_ALWAYS, 255, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 255);
	glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 255); /* saturated high */

	reset_stencil(GL_ALWAYS, 254, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 255);

	reset_stencil(GL_ALWAYS, 0, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 0); /* saturated low */

	reset_stencil(GL_ALWAYS, 1, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 0);

	/* INVERT and KEEP */
	reset_stencil(GL_ALWAYS, 0x55, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 0x55);
	glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 0xaa);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 0xaa);
}

/* points and lines keep their legacy behavior and never touch stencil */
static void test_points_lines_untouched(void) {
	int i, n;

	clear_all();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	reset_stencil(GL_ALWAYS, 7, 0xff, GL_INCR, GL_INCR, GL_INCR);

	glBegin(GL_POINTS);
	glVertex3f(-0.5f, -0.5f, 0);
	glVertex3f(0.5f, 0.5f, 0);
	glEnd();
	glBegin(GL_LINES);
	glVertex3f(-0.9f, 0.0f, 0);
	glVertex3f(0.9f, 0.0f, 0);
	glEnd();

	n = zb->xsize * zb->ysize;
	for (i = 0; i < n; i++)
		assert(zb->sbuf[i] == 0);
}

/* smooth shading and the blending raster path still drive stencil */
static void test_smooth_and_blend(void) {
	int p = pix_index(8, 8), q = pix_index(56, 56);

	clear_all();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	glShadeModel(GL_SMOOTH);
	reset_stencil(GL_ALWAYS, 3, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	glColor3f(1, 0, 0);
	glBegin(GL_QUADS);
	glColor3f(1, 0, 0);
	glVertex3f(-1, -1, 0);
	glColor3f(0, 1, 0);
	glVertex3f(1, -1, 0);
	glColor3f(0, 0, 1);
	glVertex3f(1, 1, 0);
	glColor3f(1, 1, 0);
	glVertex3f(-1, 1, 0);
	glEnd();
	assert(zb->sbuf[p] == 3 && zb->sbuf[q] == 3);
	assert((zb->pbuf[p] & 0xffffffu) != (zb->pbuf[q] & 0xffffffu));
	glShadeModel(GL_FLAT);

	/* blending variant of the filled triangle path */
	clear_all();
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	reset_stencil(GL_ALWAYS, 4, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 4);
	assert(count_color(RED) > 100);
	quad_z(0.0f, 1, 0, 0); /* additive second pass brightens */
	assert((zb->pbuf[p] & 0xffffffu) != BLACK);
	glDisable(GL_BLEND);
}

/* textured triangle with TGL_NO_DRAW_COLOR texel discard and polygon
   stipple: both happen before the stencil comparison */
static void make_texture(GLuint texid) {
	int u, v;
	static GLubyte rgb[256 * 256 * 3];
	for (v = 0; v < 256; v++)
		for (u = 0; u < 256; u++) {
			int k = (v * 256 + u) * 3;
			if (v >= 96 && v <= 103) {
				rgb[k + 0] = 255;
				rgb[k + 1] = 0;
				rgb[k + 2] = 255; /* TGL_NO_DRAW_COLOR magenta */
			} else {
				rgb[k + 0] = 0;
				rgb[k + 1] = 255;
				rgb[k + 2] = 0;
			}
		}
	glBindTexture(GL_TEXTURE_2D, texid);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb);
}

static void textured_quad(void) {
	glBegin(GL_QUADS);
	glTexCoord2f(0, 0);
	glVertex3f(-1, -1, 0);
	glTexCoord2f(1, 0);
	glVertex3f(1, -1, 0);
	glTexCoord2f(1, 1);
	glVertex3f(1, 1, 0);
	glTexCoord2f(0, 1);
	glVertex3f(-1, 1, 0);
	glEnd();
}

/* screen rows covered by the magenta discard texel band (96 <= v <= 103) */
static void test_textured_discard_and_stipple(GLuint texid, GLint blend_on) {
	GLubyte pat[128];
	int x, y;
	int pass_rows = 0, discard_rows = 0;

	memset(pat, 0xAA, sizeof(pat)); /* bit set only for odd x within a byte */
	clear_all();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texid);
	glPolygonStipple(pat);
	glEnable(GL_POLYGON_STIPPLE);

	if (blend_on) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		reset_stencil(GL_ALWAYS, 6, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	} else {
		reset_stencil(GL_ALWAYS, 7, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	}
	textured_quad();

	/* the discarded magenta texels must never appear in the color buffer */
	assert(count_color(MAGENTA) == 0);
	/* stencil values are only 0 or the reference */
	for (y = 0; y < zb->ysize; y++)
		for (x = 0; x < zb->xsize; x++) {
			GLubyte expect = blend_on ? 6 : 7;
			assert(zb->sbuf[pix_index(x, y)] == 0 || zb->sbuf[pix_index(x, y)] == expect);
		}

	for (y = 0; y < zb->ysize; y++) {
		int row_writes = 0, row_black = 1;
		for (x = 0; x < zb->xsize; x++) {
			GLubyte expect = blend_on ? 6 : 7;
			/* odd x is exactly where the 0xAA stipple lets fragments through */
			if ((x & 1) && zb->sbuf[pix_index(x, y)] == expect)
				row_writes++;
			if ((zb->pbuf[pix_index(x, y)] & 0xffffffu) != BLACK)
				row_black = 0;
		}
		if (row_writes > 0) {
			/* surviving row: stipple admits the odd-x fragments */
			assert(row_writes == zb->xsize / 2);
			pass_rows++;
		} else {
			/* fully discarded texel row: no color and no stencil write */
			assert(row_black);
			discard_rows++;
		}
	}
	assert(discard_rows >= 1);
	assert(pass_rows + discard_rows == zb->ysize);

	/* on a surviving row stipple gates the write: odd x written, even x not */
	for (y = 0; y < zb->ysize; y++) {
		GLubyte expect = blend_on ? 6 : 7;
		if (zb->sbuf[pix_index(1, y)] == expect) {
			assert(zb->sbuf[pix_index(2, y)] == 0);
			break;
		}
	}

	glDisable(GL_POLYGON_STIPPLE);
	glDisable(GL_TEXTURE_2D);
	if (blend_on)
		glDisable(GL_BLEND);
}

/* triangles generated by clipping go through the same stencil path */
static void test_clipping(void) {
	int p = pix_index(32, 32);
	long written = 0;
	int i, n = zb->xsize * zb->ysize;

	clear_all();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	reset_stencil(GL_ALWAYS, 9, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	glColor3f(1, 0, 1);
	glBegin(GL_TRIANGLES);
	glVertex3f(-2.0f, -2.0f, 0);
	glVertex3f(2.0f, -2.0f, 0);
	glVertex3f(0.0f, 2.0f, 0);
	glEnd();
	assert(zb->sbuf[p] == 9);
	for (i = 0; i < n; i++)
		if (zb->sbuf[i] == 9)
			written++;
	assert(written > 200);
}

/* display lists: compile only, compile and execute, replay against the
   live buffer contents */
static void test_display_lists(void) {
	int p = pix_index(16, 16);

	glDisable(GL_STENCIL_TEST);
	clear_all();

	/* COMPILE: state must not move while recording */
	glStencilFunc(GL_ALWAYS, 1, 0xff);
	glStencilMask(0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glClearStencil(0);
	glNewList(1, GL_COMPILE);
	glStencilFunc(GL_EQUAL, 3, 0x0f);
	glStencilMask(0x33);
	glStencilOp(GL_INCR, GL_DECR, GL_INVERT);
	glClearStencil(7);
	glEnable(GL_STENCIL_TEST);
	glEndList();
	assert(geti(GL_STENCIL_FUNC) == GL_ALWAYS);
	assert(geti(GL_STENCIL_REF) == 1);
	assert(geti(GL_STENCIL_VALUE_MASK) == 0xff);
	assert(geti(GL_STENCIL_WRITEMASK) == 0xff);
	assert(geti(GL_STENCIL_FAIL) == GL_KEEP);
	assert(geti(GL_STENCIL_CLEAR_VALUE) == 0);
	assert(geti(GL_STENCIL_TEST) == 0);

	glCallList(1);
	assert(geti(GL_STENCIL_FUNC) == GL_EQUAL);
	assert(geti(GL_STENCIL_REF) == 3);
	assert(geti(GL_STENCIL_VALUE_MASK) == 0x0f);
	assert(geti(GL_STENCIL_WRITEMASK) == 0x33);
	assert(geti(GL_STENCIL_FAIL) == GL_INCR);
	assert(geti(GL_STENCIL_PASS_DEPTH_FAIL) == GL_DECR);
	assert(geti(GL_STENCIL_PASS_DEPTH_PASS) == GL_INVERT);
	assert(geti(GL_STENCIL_CLEAR_VALUE) == 7);
	assert(geti(GL_STENCIL_TEST) == 1);

	/* COMPILE_AND_EXECUTE: state changes immediately and is recorded */
	glNewList(2, GL_COMPILE_AND_EXECUTE);
	glStencilFunc(GL_NEVER, 4, 0x55);
	glStencilMask(0x55);
	glStencilOp(GL_ZERO, GL_REPLACE, GL_KEEP);
	glClearStencil(8);
	glDisable(GL_STENCIL_TEST);
	glEndList();
	assert(geti(GL_STENCIL_FUNC) == GL_NEVER);
	assert(geti(GL_STENCIL_REF) == 4);
	assert(geti(GL_STENCIL_VALUE_MASK) == 0x55);
	assert(geti(GL_STENCIL_WRITEMASK) == 0x55);
	assert(geti(GL_STENCIL_FAIL) == GL_ZERO);
	assert(geti(GL_STENCIL_PASS_DEPTH_FAIL) == GL_REPLACE);
	assert(geti(GL_STENCIL_PASS_DEPTH_PASS) == GL_KEEP);
	assert(geti(GL_STENCIL_CLEAR_VALUE) == 8);
	assert(geti(GL_STENCIL_TEST) == 0);

	/* perturb, then replay: recorded parameters re-apply */
	glStencilFunc(GL_ALWAYS, 99, 0xff);
	glStencilMask(0xff);
	glCallList(2);
	assert(geti(GL_STENCIL_FUNC) == GL_NEVER);
	assert(geti(GL_STENCIL_REF) == 4);
	assert(geti(GL_STENCIL_WRITEMASK) == 0x55);
	assert(geti(GL_STENCIL_TEST) == 0);

	/* a list that renders re-executes against the current stencil buffer */
	glNewList(3, GL_COMPILE);
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xff);
	glStencilMask(0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
	glColor3f(1, 1, 1);
	glBegin(GL_QUADS);
	glVertex3f(-1, -1, 0);
	glVertex3f(1, -1, 0);
	glVertex3f(1, 1, 0);
	glVertex3f(-1, 1, 0);
	glEnd();
	glEndList();

	clear_all();
	glStencilMask(0xff);
	glClearStencil(0);
	clear_all();
	glCallList(3);
	assert(zb->sbuf[p] == 1);
	glCallList(3); /* replay increments the already stored value again */
	assert(zb->sbuf[p] == 2);
	glDisable(GL_STENCIL_TEST);
}

/* stencil clear obeys the write mask and the selected clear bits only */
static void test_clear_rules(void) {
	int p = pix_index(30, 30);
	GLuint kept_color;
	GLushort kept_z;

	clear_all();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST); /* clear must ignore the enable */
	glEnable(GL_STENCIL_TEST);
	reset_stencil(GL_ALWAYS, 0xaa, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[p] == 0xaa);
	glDisable(GL_STENCIL_TEST);

	/* masked clear: low nibble cleared to 0, high nibble preserved */
	glStencilMask(0x0f);
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	assert(zb->sbuf[p] == 0xa0);

	/* clear value only uses its low 8 bits */
	glStencilMask(0xf0);
	glClearStencil(0x12f);
	glClear(GL_STENCIL_BUFFER_BIT);
	assert(zb->sbuf[p] == 0x20);

	/* a stencil-only clear must not erase color or depth */
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	clear_all();
	glDisable(GL_STENCIL_TEST);
	quad_z(0.0f, 0, 1, 0);
	kept_color = zb->pbuf[p];
	kept_z = zb->zbuf[p];
	glStencilMask(0xff);
	glClearStencil(0);
	glClear(GL_STENCIL_BUFFER_BIT);
	assert((zb->pbuf[p] & 0xffffffu) == (kept_color & 0xffffffu));
	assert(zb->zbuf[p] == kept_z);
	glDisable(GL_DEPTH_TEST);
}

/* classic two pass region rendering, plus a viewable PPM dump */
static void test_two_pass_region(const char* ppm_path) {
	int left = pix_index(12, 32), right = pix_index(52, 32);
	FILE* f;
	int i;

	clear_all();
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);

	/* pass 1: mark the left half as region 1 */
	glStencilFunc(GL_ALWAYS, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glStencilMask(0xff);
	glColor3f(1, 1, 1);
	quad_region(-1.0f, 0.0f, 0.0f);
	assert(zb->sbuf[left] == 1);
	assert(zb->sbuf[right] == 0);

	/* wipe color/depth but keep the stencil region */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	assert(zb->sbuf[left] == 1);
	assert(zb->sbuf[right] == 0);

	/* pass 2: draw only where stencil equals 1, never modify stencil */
	glStencilFunc(GL_EQUAL, 1, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	quad_z(0.0f, 0, 0, 1);
	assert((zb->pbuf[left] & 0xffffffu) == BLUE);
	assert((zb->pbuf[right] & 0xffffffu) == BLACK);
	assert(zb->sbuf[left] == 1 && zb->sbuf[right] == 0);
	glDisable(GL_STENCIL_TEST);

	f = fopen(ppm_path, "wb");
	assert(f != NULL);
	fprintf(f, "P6\n%d %d\n255\n", zb->xsize, zb->ysize);
	for (i = 0; i < zb->xsize * zb->ysize; i++) {
		GLuint px = zb->pbuf[i];
		GLubyte rgb[3] = {(GLubyte)((px >> 16) & 0xff), (GLubyte)((px >> 8) & 0xff), (GLubyte)(px & 0xff)};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

/* ZB_resize reallocates a zeroed stencil buffer at the new size and
   rendering continues afterwards */
static void test_resize(void) {
	int i, n;

	ZB_resize(zb, NULL, 48, 48);
	assert(zb->xsize == 48);
	assert(zb->ysize == 48);
	assert(zb->sbuf != NULL);
	n = zb->xsize * zb->ysize;
	for (i = 0; i < n; i++)
		assert(zb->sbuf[i] == 0);

	glViewport(0, 0, 48, 48);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 11, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glStencilMask(0xff);
	quad_z(0.0f, 1, 0, 0);
	assert(zb->sbuf[pix_index(24, 24)] == 11);
	assert(count_sbuf(11) > 100);
	glDisable(GL_STENCIL_TEST);
}

int main(void) {
	const char* tmp = getenv("TMPDIR");
	char ppm_path[512];
	GLuint texid;

	zb = ZB_open(W, H, ZB_MODE_RGBA, 0);
	assert(zb != NULL);
	glInit(zb);
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glClearColor(0, 0, 0, 1);

	test_defaults_and_disabled();
	test_three_outcomes();
	test_compare_funcs();
	test_masked_compare();
	test_masks();
	test_saturation_and_ops();
	test_points_lines_untouched();
	test_smooth_and_blend();

	glGenTextures(1, &texid);
	make_texture(texid);
	test_textured_discard_and_stipple(texid, 0);
	test_textured_discard_and_stipple(texid, 1);

	test_clipping();
	test_display_lists();
	test_clear_rules();

	snprintf(ppm_path, sizeof(ppm_path), "%s/stencil_pipeline.ppm", tmp ? tmp : ".");
	test_two_pass_region(ppm_path);
	printf("STENCIL_PPM=%s\n", ppm_path);

	test_resize();

	glClose();
	ZB_close(zb);
	puts("STENCIL_PIPELINE_OK");
	return 0;
}
