/*
 * Small visual reproducer for the perspective-texture bug.
 *
 * The image contains two side-by-side panels which use the exact same
 * screen-space geometry and texture coordinates.  The left panel has
 * varying vertex depth (z = -0.8, 0.8, -0.8, 0.8); the right panel has
 * a constant depth (z = 0).  With an orthographic projection the vertex
 * homogeneous w is constant, so both panels must be pixel-identical.
 *
 * With the old rasterizer the texture lookup was divided by window z,
 * which made the left panel swim.  The two halves of the image are then
 * visibly different.  After the fix they are identical.
 *
 * Build from the repository root (compile every source file under src and
 * link it with this file, or point at a prebuilt library):
 *   cc -std=c99 -O2 -Iinclude checks/perspective_ppm.c libTinyGL.a -lm
 * Run:
 *   ./a.out checks/ppm/perspective_after.ppm
 */
#include "zbuffer.h"
#include <stdio.h>
#include <stdlib.h>

#define PANEL 64
#define WIDTH (PANEL * 2)
#define HEIGHT PANEL

static void make_texture(unsigned char* tex) {
	int x, y;
	for (y = 0; y < 256; y++)
		for (x = 0; x < 256; x++) {
			unsigned char* p = tex + (y * 256 + x) * 3;
			p[0] = (unsigned char)x;
			p[1] = (unsigned char)y;
			p[2] = (unsigned char)((x >> 5) ^ (y >> 5) ? 0xff : 0x20);
		}
}

static void draw_quad(float z0, float z1, float z2, float z3) {
	glBegin(GL_QUADS);
	glTexCoord2f(0.0f, 0.0f);
	glVertex3f(-0.92f, -0.92f, z0);
	glTexCoord2f(1.0f, 0.0f);
	glVertex3f(0.92f, -0.92f, z1);
	glTexCoord2f(1.0f, 1.0f);
	glVertex3f(0.92f, 0.92f, z2);
	glTexCoord2f(0.0f, 1.0f);
	glVertex3f(-0.92f, 0.92f, z3);
	glEnd();
}

int main(int argc, char** argv) {
	ZBuffer* z;
	unsigned char* tex;
	GLuint texture;
	FILE* f;
	int x, y;

	if (argc != 2) {
		fprintf(stderr, "usage: %s output.ppm\n", argv[0]);
		return 2;
	}

	z = ZB_open(WIDTH, HEIGHT, ZB_MODE_RGBA, 0);
	if (!z)
		return 2;
	glInit(z);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
	glShadeModel(GL_SMOOTH);
	glColor3f(1.0f, 1.0f, 1.0f);

	tex = malloc(256 * 256 * 3);
	if (!tex)
		return 2;
	make_texture(tex);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, 3, 256, 256, 0, GL_RGB, GL_UNSIGNED_BYTE, tex);

	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	/* Left: depth varies. */
	glViewport(0, 0, PANEL, PANEL);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	draw_quad(-0.8f, 0.8f, -0.8f, 0.8f);

	/* Right: depth is constant. */
	glViewport(PANEL, 0, PANEL, PANEL);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	draw_quad(0.0f, 0.0f, 0.0f, 0.0f);

	f = fopen(argv[1], "wb");
	if (!f)
		return 2;
	fprintf(f, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	for (y = 0; y < HEIGHT; y++) {
		for (x = 0; x < WIDTH; x++) {
			unsigned p = ((PIXEL*)z->pbuf)[y * z->xsize + x];
			unsigned char rgb[3];
			rgb[0] = (unsigned char)((p >> 16) & 0xff);
			rgb[1] = (unsigned char)((p >> 8) & 0xff);
			rgb[2] = (unsigned char)(p & 0xff);
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);

	glClose();
	ZB_close(z);
	free(tex);
	return 0;
}
