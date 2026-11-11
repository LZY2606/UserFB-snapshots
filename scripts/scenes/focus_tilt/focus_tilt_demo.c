//
//  focus_tilt_demo.c
//  c-ray
//
//  Numeric companion for scripts/scenes/focus_tilt/focus_tilt.json.
//  Rebuilds that camera (FOV 40, 320x240, focalDistance 6, fstops 4,
//  focusTilt [15, -10]) and reports how tightly aperture rays converge on
//  the tilted focus plane, instead of relying on the rendered image alone.
//
//  Build the same way sanity/baseline.c is built (see run.sh).
//
#include <math.h>
#include <stdio.h>

#include "datatypes/camera.h"
#include "renderer/samplers/sampler.h"

static struct vector plane_hit(struct lightRay ray, struct vector P, struct vector N) {
	float t = vec_dot(vec_sub(P, ray.start), N) / vec_dot(ray.direction, N);
	return alongRay(&ray, t);
}

int main(void) {
	struct camera lens = {
		.FOV = 40.0f,
		.width = 320,
		.height = 240,
		.focus_distance = 6.0f,
		.fstops = 2.0f,
		.focus_tilt_x = 15.0f,
		.focus_tilt_y = -10.0f,
		.forward = { 0.0f, 0.0f, 1.0f },
		.right = { 1.0f, 0.0f, 0.0f },
		.up = { 0.0f, 1.0f, 0.0f },
		.is_blender = false,
	};
	cam_recompute_optics(&lens);
	cam_update_pose(&lens, &lens.orientation, &lens.position);

	struct camera pinhole = lens;
	pinhole.aperture = 0.0f;

	const struct vector P = vec_scale(lens.forward, lens.focus_distance);
	const struct vector N = cam_focus_plane_normal(&lens);

	struct sampler *a = sampler_new();
	struct sampler *b = sampler_new();

	double worst_convergence = 0.0;
	double worst_residual = 0.0;
	const int samples = 32;
	// Spread the probe across the frame, including the corners.
	const int px[] = { 0, 80, 160, 239, 319 };
	const int py[] = { 0, 60, 119, 180, 239 };
	for (size_t xi = 0; xi < sizeof(px) / sizeof(px[0]); ++xi) {
		for (size_t yi = 0; yi < sizeof(py) / sizeof(py[0]); ++yi) {
			for (int s = 0; s < samples; ++s) {
				const uint32_t pixel = (uint32_t)(px[xi] * 240 + py[yi]);
				sampler_init(a, Random, s, samples, pixel);
				sampler_init(b, Random, s, samples, pixel);
				struct lightRay center = cam_get_ray(&pinhole, px[xi], py[yi], a);
				struct lightRay apert = cam_get_ray(&lens, px[xi], py[yi], b);
				struct vector target = plane_hit(center, P, N);
				struct vector hit = plane_hit(apert, P, N);
				double drift = vec_length(vec_sub(hit, target));
				double residual = fabs(vec_dot(vec_sub(hit, P), N));
				if (drift > worst_convergence) worst_convergence = drift;
				if (residual > worst_residual) worst_residual = residual;
				if (!isfinite(hit.x) || !isfinite(hit.y) || !isfinite(hit.z)) {
					printf("non-finite focus hit at (%d,%d) sample %d\n", px[xi], py[yi], s);
					return 1;
				}
			}
		}
	}

	printf("FOCUS_TILT demo tilt=[15,-10] deg focalDistance=6 fstops=%.6g aperture=%.6g\n",
		   (double)lens.fstops, (double)lens.aperture);
	printf("  focus plane normal = (%.6f, %.6f, %.6f)\n", (double)N.x, (double)N.y, (double)N.z);
	printf("  max focus convergence error (lens vs pinhole) = %.8g\n", worst_convergence);
	printf("  max focus plane residual                      = %.8g\n", worst_residual);

	sampler_destroy(a);
	sampler_destroy(b);
	return (worst_convergence < 2.0e-5 && worst_residual < 1.0e-5) ? 0 : 1;
}
