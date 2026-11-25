//=====================================================================
//
// examples/radiosity_demo.c - a small scene lit only by radiosity
//
// Everything is constructed in plain C: no window library, no external
// assets.  A horizontal emitter panel lights a floor and a back wall
// (the two receivers); an opaque plate hangs in between and casts a
// shadow.  The scene is solved for one bounce and then for several
// bounces; both results are written as binary PPM images so the
// difference between direct-only and converged indirect light is easy
// to see.
//
// Note: the original device_draw_primitive rejects a whole triangle as
// soon as one vertex leaves the view frustum, so the demo keeps the
// geometry comfortably inside the 90 degree field of view.
//
// build:
//   cc -std=c99 -O2 -DMINI3D_HEADLESS -I. \
//      examples/radiosity_demo.c radiosity.c mini3d.c -lm -o radiosity_demo
//
// run:
//   ./radiosity_demo        # writes radiosity_1bounce.ppm / _nbounce.ppm
//
//=====================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mini3d.h"
#include "radiosity.h"

#define WIDTH   480
#define HEIGHT  360

static point_t v3(float x, float y, float z) {
	point_t p;
	p.x = x; p.y = y; p.z = z; p.w = 1.0f;
	return p;
}

static radiosity_patch make_patch(point_t a, point_t b, point_t c,
		float er, float eg, float eb,
		float rr, float rg, float rb) {
	radiosity_patch p;
	p.v[0] = a; p.v[1] = b; p.v[2] = c;
	p.emission.r = er; p.emission.g = eg; p.emission.b = eb;
	p.reflectance.r = rr; p.reflectance.g = rg; p.reflectance.b = rb;
	return p;
}

// Append the two triangles of a quad.  The caller gives the four corners
// in the winding that yields the wanted outward normal.
static size_t add_quad(radiosity_patch *ps, size_t n,
		point_t a, point_t b, point_t c, point_t d,
		float er, float eg, float eb,
		float rr, float rg, float rb) {
	ps[n + 0] = make_patch(a, b, c, er, eg, eb, rr, rg, rb);
	ps[n + 1] = make_patch(a, c, d, er, eg, eb, rr, rg, rb);
	return n + 2;
}

static void write_ppm(const char *path, const device_t *device) {
	FILE *f = fopen(path, "wb");
	int y, x;
	if (!f) {
		fprintf(stderr, "cannot write %s\n", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", device->width, device->height);
	for (y = 0; y < device->height; y++) {
		const IUINT32 *row = device->framebuffer[y];
		for (x = 0; x < device->width; x++) {
			unsigned char rgb[3];
			rgb[0] = (unsigned char)((row[x] >> 16) & 255);
			rgb[1] = (unsigned char)((row[x] >> 8) & 255);
			rgb[2] = (unsigned char)(row[x] & 255);
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
	printf("wrote %s\n", path);
}

// Solve with at most `shots` shooting steps and render to `path`.
static void solve_and_draw(radiosity_scene *scene, device_t *device,
		size_t shots, float exposure, const char *path) {
	radiosity_reset(scene);
	radiosity_step(scene, shots);
	device_clear(device, 0);
	radiosity_draw(scene, device, exposure);
	write_ppm(path, device);
}

int main(void) {
	radiosity_patch patches[16];
	size_t n = 0;
	radiosity_scene *scene;
	device_t device;
	// Look from the front-right, slightly above the floor, at the
	// corner shared by the floor and the back wall.
	point_t eye = { 2.2f, -4.0f, 2.0f, 1.0f };
	point_t at = { 0.0f, 0.6f, 0.5f, 1.0f };
	point_t up = { 0.0f, 0.0f, 1.0f, 1.0f };

	// Floor receiver, facing up (+z), neutral grey.
	n = add_quad(patches, n,
		v3(-2.6f, -1.6f, 0), v3(2.6f, -1.6f, 0),
		v3(2.6f, 2.4f, 0), v3(-2.6f, 2.4f, 0),
		0, 0, 0,  0.7f, 0.7f, 0.7f);
	// Back wall receiver, facing the camera (-y), light blue.
	n = add_quad(patches, n,
		v3(-2.6f, 2.4f, 0), v3(2.6f, 2.4f, 0),
		v3(2.6f, 2.4f, 2.2f), v3(-2.6f, 2.4f, 2.2f),
		0, 0, 0,  0.55f, 0.62f, 0.78f);
	// Horizontal emitter panel above the floor, facing down (-z).
	// Emission only; it sends energy to the two receivers.
	n = add_quad(patches, n,
		v3(-1.6f, -0.8f, 2.0f), v3(-1.6f, 1.8f, 2.0f),
		v3(1.6f, 1.8f, 2.0f), v3(1.6f, -0.8f, 2.0f),
		7.0f, 7.0f, 7.0f,  0, 0, 0);
	// Opaque plate hanging toward the floor, facing the camera (-y).
	// It blocks part of the emitter and casts a soft shadow.
	n = add_quad(patches, n,
		v3(-0.8f, -0.2f, 0.9f), v3(0.8f, -0.2f, 0.9f),
		v3(0.8f, -0.2f, 1.8f), v3(-0.8f, -0.2f, 1.8f),
		0, 0, 0,  0.03f, 0.03f, 0.03f);

	// resolution 64 and offset 0.05 are plenty for this scene.
	scene = radiosity_create(patches, n, 64, 0.05f);
	if (!scene) {
		fprintf(stderr, "radiosity_create failed\n");
		return 1;
	}

	device_init(&device, WIDTH, HEIGHT, NULL);
	matrix_set_lookat(&device.transform.view, &eye, &at, &up);
	transform_update(&device.transform);
	device.render_state = RENDER_STATE_COLOR;

	// One shot: only the emitter fires, so this is direct light plus the
	// first bounce.  The shadow under the plate is clearly visible.
	solve_and_draw(scene, &device, 1, 1.0f, "radiosity_1bounce.ppm");

	// Many shots: the receivers themselves fire, so the room fills with
	// indirect light and the shadow softens.
	solve_and_draw(scene, &device, 64, 0.6f, "radiosity_nbounce.ppm");

	device_destroy(&device);
	radiosity_destroy(scene);
	return 0;
}
