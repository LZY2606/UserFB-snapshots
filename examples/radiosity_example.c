//=====================================================================
//
// radiosity_example.c - pure C progressive radiosity demo
//
// Builds a small offline scene entirely in C: one glowing ceiling
// patch, two receiving patches (floor and back wall) and one occluding
// panel. It renders a PPM after a single bounce and another after many
// bounces, both drawn through the original mini3d color pipeline.
//
// build:
//   cc -std=c99 -O2 -DMINI3D_HEADLESS -I. \
//      examples/radiosity_example.c mini3d.c radiosity.c -lm \
//      -o radiosity_example
//
// run:
//   ./radiosity_example [output_dir]
//
//=====================================================================
#include "radiosity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EX_COUNT 4

static void vset(point_t *p, float x, float y, float z) {
	p->x = x; p->y = y; p->z = z; p->w = 1.0f;
}

static void cset(color_t *c, float r, float g, float b) {
	c->r = r; c->g = g; c->b = b;
}

// three vertices, reflectance and emission for one triangle patch
static void make_patch(radiosity_patch *p,
	float x0, float y0, float z0,
	float x1, float y1, float z1,
	float x2, float y2, float z2,
	float rr, float rg, float rb,
	float er, float eg, float eb) {
	vset(&p->v[0], x0, y0, z0);
	vset(&p->v[1], x1, y1, z1);
	vset(&p->v[2], x2, y2, z2);
	cset(&p->reflectance, rr, rg, rb);
	cset(&p->emission, er, eg, eb);
}

static int write_ppm(const char *path, const device_t *device) {
	FILE *f = fopen(path, "wb");
	int x, y;
	if (f == NULL) return 0;
	fprintf(f, "P6\n%d %d\n255\n", device->width, device->height);
	for (y = 0; y < device->height; y++) {
		for (x = 0; x < device->width; x++) {
			IUINT32 c = device->framebuffer[y][x];
			unsigned char rgb[3];
			rgb[0] = (unsigned char)((c >> 16) & 0xff);
			rgb[1] = (unsigned char)((c >> 8) & 0xff);
			rgb[2] = (unsigned char)(c & 0xff);
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	return 1;
}

static void setup_camera(device_t *device) {
	point_t eye = { 0.0f, 5.5f, -6.0f, 1.0f };
	point_t at = { 0.0f, 0.0f, 0.3f, 1.0f };
	point_t up = { 0.0f, 0.0f, 1.0f, 1.0f };
	matrix_set_lookat(&device->transform.view, &eye, &at, &up);
	transform_update(&device->transform);
}

int main(int argc, char **argv) {
	radiosity_patch patches[EX_COUNT];
	radiosity_scene *scene;
	device_t one, multi;
	const char *dir = (argc > 1) ? argv[1] : ".";
	char path_one[512], path_multi[512];
	size_t total = 0, ran;

	// emitter on the ceiling, normal -Y (winding order), glowing white
	make_patch(&patches[0],
		-1.0f, 3.0f, -1.0f,  1.0f, 3.0f, -1.0f,  1.0f, 3.0f, 1.0f,
		0.0f, 0.0f, 0.0f,  4.0f, 4.0f, 4.0f);
	// floor receiver, normal +Y
	make_patch(&patches[1],
		-3.0f, 0.0f, -3.0f,  3.0f, 0.0f, 3.0f,  3.0f, 0.0f, -3.0f,
		0.7f, 0.6f, 0.4f,  0.0f, 0.0f, 0.0f);
	// back wall receiver, normal -Z
	make_patch(&patches[2],
		-3.0f, 0.0f, 3.0f,  3.0f, 3.0f, 3.0f,  3.0f, 0.0f, 3.0f,
		0.3f, 0.5f, 0.7f,  0.0f, 0.0f, 0.0f);
	// vertical occluding panel between emitter and floor, normal +X
	make_patch(&patches[3],
		0.0f, 0.5f, -1.0f,  0.0f, 2.0f, 1.0f,  0.0f, 0.5f, 1.0f,
		0.4f, 0.4f, 0.4f,  0.0f, 0.0f, 0.0f);

	scene = radiosity_create(patches, EX_COUNT, 16, 0.3f);
	if (scene == NULL) {
		fprintf(stderr, "radiosity_create failed\n");
		return 1;
	}

	device_init(&one, 96, 96, NULL);
	device_init(&multi, 96, 96, NULL);
	device_clear(&one, 0);
	device_clear(&multi, 0);
	setup_camera(&one);
	setup_camera(&multi);

	// one bounce: shoot the emitter exactly once
	total += radiosity_step(scene, 1);
	radiosity_draw(scene, &one, 2.0f);

	// many bounces: keep shooting until no unshot energy remains
	while ((ran = radiosity_step(scene, 1)) != 0)
		total += ran;
	printf("shooting steps to convergence after the first bounce: %zu\n",
		total - 1);
	radiosity_draw(scene, &multi, 2.0f);

	snprintf(path_one, sizeof(path_one), "%s/radiosity_one.ppm", dir);
	snprintf(path_multi, sizeof(path_multi), "%s/radiosity_multi.ppm", dir);
	if (!write_ppm(path_one, &one) || !write_ppm(path_multi, &multi)) {
		fprintf(stderr, "failed to write PPM files\n");
		device_destroy(&one);
		device_destroy(&multi);
		radiosity_destroy(scene);
		return 1;
	}
	printf("wrote %s\n", path_one);
	printf("wrote %s\n", path_multi);

	device_destroy(&one);
	device_destroy(&multi);
	radiosity_destroy(scene);
	return 0;
}
