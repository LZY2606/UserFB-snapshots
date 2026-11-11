//
//  camera.c
//  c-ray
//
//  Created by Valtteri Koskivuori on 02/03/2015.
//  Copyright © 2015-2025 Valtteri Koskivuori. All rights reserved.
//

#include "../../includes.h"
#include "camera.h"

#include <common/transforms.h>
#include <common/vector.h>
#include <renderer/samplers/vec.h>

// Legal tilt range, degrees, both axes exclusive.
#define FOCUS_TILT_LIMIT 80.0f

void cam_recompute_optics(struct camera *cam) {
	if (!cam) return;
	cam->aspect_ratio = (float)cam->width / (float)cam->height;
	cam->sensor_size.x = 2.0f * tanf(deg_to_rad(cam->FOV) / 2.0f);
	cam->sensor_size.y = cam->sensor_size.x / cam->aspect_ratio;
	//FIXME: This still assumes a 35mm sensor, instead of using the computed
	//sensor width value from above. Just preserving this so existing configs
	//work, but do look into a better way to do this here!
	const float sensor_width_35mm = 0.036f;
	cam->focal_length = 0.5f * sensor_width_35mm / deg_to_rad(0.5f * cam->FOV);
	if (cam->fstops != 0.0f) cam->aperture = 0.5f * (cam->focal_length / cam->fstops);
}

void recomputeComposite(struct camera *cam) {
	struct transform transforms[2];
	if (cam->path) {
		struct vector positionAtT = spline_at(cam->path, cam->time);
		transforms[0] = tform_new_translate(positionAtT.x, positionAtT.y, positionAtT.z);
	} else {
		transforms[0] = tform_new_translate(cam->is_blender ? cam->position.x : -cam->position.x, cam->position.y, cam->position.z);
	}
	transforms[1] = tform_new_rot(cam->orientation.roll, cam->orientation.pitch, cam->orientation.yaw);

	struct transform composite = { .A = mat_id() };
	for (int i = 0; i < 2; ++i) {
		composite.A = mat_mul(composite.A, transforms[i].A);
	}

	composite.Ainv = mat_invert(composite.A);
	cam->composite = composite;
}

void cam_update_pose(struct camera *cam, const struct euler_angles *orientation, const struct vector *pos) {
	if (orientation) cam->orientation = *orientation;
	if (pos) cam->position = *pos;
	if (orientation || pos) recomputeComposite(cam);
}

struct vector cam_focus_plane_normal(const struct camera *cam) {
	const struct vector tilted = vec_add(
		cam->forward,
		vec_add(
			vec_scale(cam->right, tanf(deg_to_rad(cam->focus_tilt_x))),
			vec_scale(cam->up, tanf(deg_to_rad(cam->focus_tilt_y)))
		)
	);
	return vec_normalize(tilted);
}

bool cam_validate_focus(const struct camera *cam) {
	if (!cam) return false;
	if (!isfinite(cam->focus_tilt_x) || !isfinite(cam->focus_tilt_y)) return false;
	if (fabsf(cam->focus_tilt_x) >= FOCUS_TILT_LIMIT ||
		fabsf(cam->focus_tilt_y) >= FOCUS_TILT_LIMIT) return false;
	// Zero tilt keeps the historical camera behavior exactly, including
	// focus_distance values that would otherwise be meaningless.
	if (cam->focus_tilt_x == 0.0f && cam->focus_tilt_y == 0.0f) return true;
	if (!isfinite(cam->focus_distance) || cam->focus_distance <= 0.0f) return false;
	if (cam->width <= 0 || cam->height <= 0) return false;
	if (!isfinite(cam->sensor_size.x) || !isfinite(cam->sensor_size.y)) return false;

	const struct vector normal = cam_focus_plane_normal(cam);
	if (!isfinite(normal.x) || !isfinite(normal.y) || !isfinite(normal.z)) return false;

	// Include the full [-1,1] pixel jitter envelope, so the extrema sit half
	// a pixel beyond the nominal sensor corners.
	const float half_x = 0.5f * cam->sensor_size.x * (float)(cam->width + 1) / (float)cam->width;
	const float half_y = 0.5f * cam->sensor_size.y * (float)(cam->height + 1) / (float)cam->height;
	const float plane_num = cam->focus_distance * vec_dot(cam->forward, normal);
	if (!isfinite(plane_num) || plane_num <= 0.0f) return false;

	for (int sx = -1; sx <= 1; sx += 2) {
		for (int sy = -1; sy <= 1; sy += 2) {
			const struct vector corner = vec_add(
				cam->forward,
				vec_add(
					vec_scale(cam->right, sx * half_x),
					vec_scale(cam->up, sy * half_y)
				)
			);
			const float denom = vec_dot(corner, normal);
			// Every image ray, jitter included, has to meet the focus plane
			// strictly in front of the camera. A non-positive denominator
			// means the plane is parallel to or facing away from the ray.
			if (!(denom > 0.0f)) return false;
			const float t = plane_num / denom;
			if (!isfinite(t) || t <= 0.0f) return false;
		}
	}
	return true;
}

static inline float sign(float v) {
	return (v >= 0.0f) ? 1.0f : -1.0f;
}

// Convert uniform distribution into triangle-shaped distribution
// From https://www.shadertoy.com/view/4t2SDh
static inline float triangleDistribution(float v) {
	const float orig = v * 2.0f - 1.0f;
	v = orig / sqrtf(fabsf(orig));
	v = clamp(v, -1.0f, 1.0f); // Clamping it like this might be a bit overkill.
	v = v - sign(orig);
	return v;
}

struct lightRay cam_get_ray(const struct camera *cam, int x, int y, struct sampler *sampler) {
	struct lightRay new_ray = { .type = rt_camera };

	const float jitter_x = triangleDistribution(sampler_dimension(sampler));
	const float jitter_y = triangleDistribution(sampler_dimension(sampler));

	const struct vector pix_x = vec_scale(cam->is_blender ? cam->right : vec_negate(cam->right), (cam->sensor_size.x / cam->width));
	const struct vector pix_y = vec_scale(cam->up, (cam->sensor_size.y / cam->height));
	const struct vector pix_v = vec_add(
							cam->forward,
							vec_add(
								vec_scale(pix_x, x - cam->width  * 0.5f + jitter_x + 0.5f),
								vec_scale(pix_y, y - cam->height * 0.5f + jitter_y + 0.5f)
							)
						);
	new_ray.direction = vec_normalize(pix_v);

	// Unused if aperture == 0.0, but still computed to maintain the same
	// prng sequence for both codepaths
	struct coord random = coord_on_unit_disc(sampler);

	if (cam->aperture > 0.0f) {
		struct vector focus_point;
		bool tilted = cam->focus_tilt_x != 0.0f || cam->focus_tilt_y != 0.0f;
		if (tilted) {
			// Intersect the pixel ray with the tilted focus plane, then aim
			// the lens sample at that same point. The plane passes through
			// forward*focus_distance and has the tilted normal.
			const struct vector normal = cam_focus_plane_normal(cam);
			const struct vector plane_point = vec_scale(cam->forward, cam->focus_distance);
			const float denom = vec_dot(new_ray.direction, normal);
			// Validation guarantees denom > 0 for the configured frame, but
			// guard anyway so a later resolution override can't feed NaN/Inf
			// to a render thread; fall back to the planar formula if not.
			if (denom > 0.0f) {
				const float ft = vec_dot(vec_sub(plane_point, new_ray.start), normal) / denom;
				focus_point = alongRay(&new_ray, ft);
			} else {
				const float ft = cam->focus_distance / vec_dot(new_ray.direction, cam->forward);
				focus_point = alongRay(&new_ray, ft);
			}
		} else {
			const float ft = cam->focus_distance / vec_dot(new_ray.direction, cam->forward);
			focus_point = alongRay(&new_ray, ft);
		}
		const struct coord lens_point = coord_scale(cam->aperture, random);
		new_ray.start = vec_add(new_ray.start, vec_add(vec_scale(cam->right, lens_point.x), vec_scale(cam->up, lens_point.y)));
		new_ray.direction = vec_normalize(vec_sub(focus_point, new_ray.start));
	}
	//To world space
	tform_ray(&new_ray, cam->composite.A);
	return new_ray;
}
