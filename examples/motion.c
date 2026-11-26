/*
 * examples/motion.c
 *
 * Build from the repository root (MINI3D_HEADLESS is also set below):
 *   cc -std=c99 -O2 examples/motion.c mini3d.c motion.c -lm -o motion_example
 *   ./motion_example
 *
 * The program writes motion_shutter.ppm (continuous-time integration)
 * and motion_single.ppm (one ordinary device_draw_primitive frame).
 */
#ifndef MINI3D_HEADLESS
#define MINI3D_HEADLESS
#endif
#include "mini3d.h"
#include "motion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 120
#define H 90

static vertex_t make_vertex(float x, float y, float z,
	float r, float g, float b) {
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

/* World units are target pixels; y grows downward. */
static void use_pixel_transform(device_t *d) {
	matrix_t *m = &d->transform.transform;
	matrix_set_identity(m);
	m->m[0][0] = 2.0f / (float)d->width;
	m->m[3][0] = -1.0f;
	m->m[1][1] = -2.0f / (float)d->height;
	m->m[3][1] = 1.0f;
	m->m[2][2] = 1.0f;
	m->m[3][3] = 1.0f;
}

static void write_ppm(const char *path, const device_t *d) {
	FILE *f = fopen(path, "wb");
	int x, y;
	if (!f) {
		fprintf(stderr, "cannot write %s\n", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", d->width, d->height);
	for (y = 0; y < d->height; ++y) {
		for (x = 0; x < d->width; ++x) {
			IUINT32 c = d->framebuffer[y][x];
			unsigned char rgb[3] = {
				(unsigned char)((c >> 16) & 255U),
				(unsigned char)((c >> 8) & 255U),
				(unsigned char)(c & 255U)
			};
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	printf("wrote %s\n", path);
}

static motion_triangle make_moving_triangle(int kind) {
	motion_triangle t;
	if (kind == 0) {
		/* Rainbow triangle whose vertices follow different paths. */
		t.open[0] = make_vertex(10, 15, .20f, 1.0f, .15f, .10f);
		t.open[1] = make_vertex(105, 20, .55f, .10f, 1.0f, .25f);
		t.open[2] = make_vertex(45, 82, .35f, .15f, .35f, 1.0f);
		t.close[0] = make_vertex(28, 7, .45f, 1.0f, .15f, .10f);
		t.close[1] = make_vertex(112, 55, .20f, .10f, 1.0f, .25f);
		t.close[2] = make_vertex(18, 70, .80f, .15f, .35f, 1.0f);
	} else if (kind == 1) {
		/* A warm card crossing the middle of the rainbow triangle. */
		t.open[0] = make_vertex(35, 30, .15f, 1.0f, .75f, .10f);
		t.open[1] = make_vertex(78, 34, .70f, 1.0f, .25f, .05f);
		t.open[2] = make_vertex(42, 62, .40f, 1.0f, .95f, .35f);
		t.close[0] = make_vertex(52, 22, .65f, 1.0f, .75f, .10f);
		t.close[1] = make_vertex(88, 52, .25f, 1.0f, .25f, .05f);
		t.close[2] = make_vertex(30, 68, .85f, 1.0f, .95f, .35f);
	} else {
		/* Cool card moving in the opposite direction. */
		t.open[0] = make_vertex(70, 18, .80f, .10f, .85f, 1.0f);
		t.open[1] = make_vertex(110, 62, .30f, .35f, .30f, 1.0f);
		t.open[2] = make_vertex(58, 72, .55f, .10f, 1.0f, .75f);
		t.close[0] = make_vertex(42, 28, .25f, .10f, .85f, 1.0f);
		t.close[1] = make_vertex(82, 78, .75f, .35f, .30f, 1.0f);
		t.close[2] = make_vertex(36, 48, .40f, .10f, 1.0f, .75f);
	}
	return t;
}

int main(void) {
	device_t shutter_device, single_device;
	motion_triangle triangles[3];
	double *rgb;
	int i;

	for (i = 0; i < 3; ++i) triangles[i] = make_moving_triangle(i);

	device_init(&shutter_device, W, H, NULL);
	shutter_device.background = 0x0b1020;
	shutter_device.render_state = RENDER_STATE_COLOR;
	use_pixel_transform(&shutter_device);

	rgb = (double *)malloc((size_t)W * H * 3 * sizeof(double));
	if (!rgb) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	if (!motion_render(&shutter_device, triangles, 3, rgb)) {
		fprintf(stderr, "motion_render failed\n");
		free(rgb);
		device_destroy(&shutter_device);
		return 1;
	}
	write_ppm("motion_shutter.ppm", &shutter_device);
	free(rgb);

	device_init(&single_device, W, H, NULL);
	single_device.background = 0x0b1020;
	single_device.render_state = RENDER_STATE_COLOR;
	use_pixel_transform(&single_device);
	device_clear(&single_device, 0);
	for (i = 0; i < 3; ++i) {
		device_draw_primitive(&single_device,
			&triangles[i].open[0],
			&triangles[i].open[1],
			&triangles[i].open[2]);
	}
	write_ppm("motion_single.ppm", &single_device);

	device_destroy(&shutter_device);
	device_destroy(&single_device);
	return 0;
}
