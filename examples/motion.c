//=====================================================================
//
// examples/motion.c - continuous shutter vs single instant output.
//
// A small triangle moves a long way during one exposure, so a
// single-frame (instantaneous) renderer catches it at one position
// while the continuous-shutter renderer records the whole swept
// path.  Two binary PPM images are written:
//
//   motion_instant.ppm  - open == close, one instant, no shutter
//   motion_shutter.ppm  - full t in [0,1] exposure, motion blurred
//
// Build:
//   cc -std=c99 -O2 -DMINI3D_HEADLESS -I. examples/motion.c \
//      mini3d.c motion.c -lm -o motion_example
//
//=====================================================================
#include "motion.h"

#include <stdio.h>
#include <stdlib.h>

#define IMG_W 160
#define IMG_H 120

// World (x, y) maps 1:1 onto pixels; window-z equals world z.
static void install_pixel_transform(device_t *d) {
	matrix_t m;
	matrix_set_zero(&m);
	m.m[0][0] = 2.0f / d->width;
	m.m[3][0] = -1.0f;
	m.m[1][1] = -2.0f / d->height;
	m.m[3][1] = 1.0f;
	m.m[2][2] = 1.0f;
	m.m[3][3] = 1.0f;
	d->transform.world = m;
	matrix_set_identity(&d->transform.view);
	matrix_set_identity(&d->transform.projection);
	transform_update(&d->transform);
}

static vertex_t vtx(float x, float y, float z, color_t c) {
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

// Build the moving scene.  A red-green-blue triangle sweeps most of
// the width; a slower blue triangle crosses behind it and swaps depth
// part way through the exposure.
static size_t build_scene(motion_triangle *tr, int shutter) {
	color_t red = { 1, 0, 0 }, green = { 0, 1, 0 }, blue = { 0, 0, 1 };
	color_t navy = { 0.1f, 0.2f, 0.8f };

	float x0 = 10.0f, x1 = 130.0f;     // long horizontal sweep
	if (!shutter) x1 = x0;             // instantaneous: freeze at open

	tr[0].open[0] = vtx(x0, 30, 0.2f, red);
	tr[0].open[1] = vtx(x0 + 24, 30, 0.3f, green);
	tr[0].open[2] = vtx(x0, 54, 0.4f, blue);
	tr[0].close[0] = vtx(x1, 30, 0.2f, red);
	tr[0].close[1] = vtx(x1 + 24, 30, 0.3f, green);
	tr[0].close[2] = vtx(x1, 54, 0.4f, blue);

	// Background face: in front at first, dives behind via vertex z.
	tr[1].open[0] = vtx(40, 60, 0.1f, navy);
	tr[1].open[1] = vtx(120, 60, 0.1f, navy);
	tr[1].open[2] = vtx(40, 100, 0.1f, navy);
	tr[1].close[0] = vtx(40, 60, 0.9f, navy);
	tr[1].close[1] = vtx(120, 60, 0.9f, navy);
	tr[1].close[2] = vtx(40, 100, 0.9f, navy);
	return 2;
}

static int write_ppm(const char *path, const device_t *d) {
	FILE *fp = fopen(path, "wb");
	if (fp == NULL) return 0;
	fprintf(fp, "P6\n%d %d\n255\n", d->width, d->height);
	for (int y = 0; y < d->height; y++)
		for (int x = 0; x < d->width; x++) {
			IUINT32 c = d->framebuffer[y][x];
			unsigned char rgb[3] = {
				(unsigned char)((c >> 16) & 255),
				(unsigned char)((c >> 8) & 255),
				(unsigned char)(c & 255)
			};
			if (fwrite(rgb, 1, 3, fp) != 3) {
				fclose(fp);
				return 0;
			}
		}
	fclose(fp);
	return 1;
}

static int render_to_ppm(int shutter, const char *path) {
	device_t d;
	double *rgb = (double *)malloc(
		(size_t)IMG_W * IMG_H * 3 * sizeof(double));
	if (rgb == NULL) return 0;
	device_init(&d, IMG_W, IMG_H, NULL);
	install_pixel_transform(&d);
	d.background = 0x141414;

	motion_triangle tr[2];
	size_t n = build_scene(tr, shutter);
	int ok = motion_render(&d, tr, n, rgb);
	if (ok) ok = write_ppm(path, &d);

	free(rgb);
	device_destroy(&d);
	return ok;
}

int main(void) {
	if (!render_to_ppm(0, "motion_instant.ppm")) {
		fprintf(stderr, "failed to write motion_instant.ppm\n");
		return 1;
	}
	if (!render_to_ppm(1, "motion_shutter.ppm")) {
		fprintf(stderr, "failed to write motion_shutter.ppm\n");
		return 1;
	}
	printf("wrote motion_instant.ppm and motion_shutter.ppm (%dx%d)\n",
		IMG_W, IMG_H);
	return 0;
}
