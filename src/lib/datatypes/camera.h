//
//  camera.h
//  c-ray
//
//  Created by Valtteri Koskivuori on 02/03/2015.
//  Copyright © 2015-2025 Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <v.h>
#include <common/vector.h>
#include <common/quaternion.h>
#include <common/transforms.h>
#include "lightray.h"
#include "spline.h"

struct camera {
	float FOV;
	float focal_length;
	float focus_distance;
	float fstops;
	float aperture;
	float aspect_ratio;
	// Focus plane tilt around the camera right/up axes, in degrees.
	// Zero reproduces the original planar focus at focus_distance.
	float focus_tilt_x;
	float focus_tilt_y;
	struct coord sensor_size;

	struct vector up;
	struct vector right;
	struct vector look_at;
	struct vector forward;

	struct transform composite;
	struct euler_angles orientation;
	struct vector position;

	struct spline *path;
	float time;

	// TODO: size_t
	int width;
	int height;

	bool is_blender;
};

struct sampler;

void cam_recompute_optics(struct camera *cam);
void cam_update_pose(struct camera *cam, const struct euler_angles *orientation, const struct vector *pos);
struct lightRay cam_get_ray(const struct camera *cam, int x, int y, struct sampler *sampler);

// Focus plane normal in the camera's own right/up/forward basis. The plane
// passes through forward*focus_distance and has this normal.
struct vector cam_focus_plane_normal(const struct camera *cam);

// Reject focus settings that can't produce a valid image: non-finite or
// out-of-range tilt angles, a non-positive focus distance with tilt enabled,
// or a focus plane parallel to / behind any image corner ray. Pixel jitter
// across the whole sensor is taken into account, not just the center ray.
bool cam_validate_focus(const struct camera *cam);
