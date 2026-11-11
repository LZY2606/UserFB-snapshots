//
//  focus_tilt_regression.c
//  c-ray
//
//  Headless C regression for the tilted thin-lens focus plane.
//  Built and driven by tests/check_focus_tilt.sh.
//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <c-ray/c-ray.h>

#include <v.h>
#include <common/vector.h>
#include <common/transforms.h>
#include <common/cr_string.h>
#include <common/json_loader.h>

#include <datatypes/camera.h>
#include <datatypes/scene.h>
#include <renderer/renderer.h>
#include <renderer/samplers/sampler.h>
#include <renderer/samplers/vec.h>
#include <protocol/protocol.h>

static unsigned failures = 0;
static unsigned checks = 0;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("  FAIL: " __VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static struct vector vec(float x, float y, float z) {
	return (struct vector){ x, y, z };
}

static struct camera make_cam(bool blender, int width, int height, float fov,
							  float focus_distance, float aperture,
							  float tilt_x, float tilt_y) {
	struct camera cam = { 0 };
	cam.FOV = fov;
	cam.width = width;
	cam.height = height;
	cam.focus_distance = focus_distance;
	cam.aperture = aperture;
	cam.focus_tilt_x = tilt_x;
	cam.focus_tilt_y = tilt_y;
	if (blender) {
		cam.is_blender = true;
		cam.look_at = vec(0.0f, 0.0f, -1.0f);
		cam.forward = vec(0.0f, 0.0f, -1.0f);
		cam.right = vec(1.0f, 0.0f, 0.0f);
		cam.up = vec(0.0f, -1.0f, 0.0f);
	} else {
		cam.is_blender = false;
		cam.look_at = vec(0.0f, 0.0f, 1.0f);
		cam.forward = vec(0.0f, 0.0f, 1.0f);
		cam.right = vec(1.0f, 0.0f, 0.0f);
		cam.up = vec(0.0f, 1.0f, 0.0f);
	}
	cam_recompute_optics(&cam);
	cam_update_pose(&cam, &cam.orientation, &cam.position);
	return cam;
}

static const enum samplerType all_samplers[] = { Halton, Hammersley, Random };
static const char *sampler_name(enum samplerType t) {
	switch (t) {
		case Halton: return "Halton";
		case Hammersley: return "Hammersley";
		case Random: return "Random";
	}
	return "?";
}

static const int passes = 4;
static const int pixel_index = 37;

// Exact pre-feature camera formula, kept here as the behavioral reference.
// Copied verbatim from camera.c so the test does not depend on internals.
static inline float ref_sign(float v) {
	return (v >= 0.0f) ? 1.0f : -1.0f;
}

static inline float ref_triangle_distribution(float v) {
	const float orig = v * 2.0f - 1.0f;
	v = orig / sqrtf(fabsf(orig));
	v = clamp(v, -1.0f, 1.0f);
	v = v - ref_sign(orig);
	return v;
}

static struct lightRay legacy_get_ray(const struct camera *cam, int x, int y, struct sampler *sampler) {
	struct lightRay ray = { .type = rt_camera };
	const float jitter_x = ref_triangle_distribution(sampler_dimension(sampler));
	const float jitter_y = ref_triangle_distribution(sampler_dimension(sampler));
	const struct vector pix_x = vec_scale(cam->is_blender ? cam->right : vec_negate(cam->right), (cam->sensor_size.x / cam->width));
	const struct vector pix_y = vec_scale(cam->up, (cam->sensor_size.y / cam->height));
	const struct vector pix_v = vec_add(
		cam->forward,
		vec_add(
			vec_scale(pix_x, x - cam->width * 0.5f + jitter_x + 0.5f),
			vec_scale(pix_y, y - cam->height * 0.5f + jitter_y + 0.5f)
		)
	);
	ray.direction = vec_normalize(pix_v);
	struct coord random = coord_on_unit_disc(sampler);
	if (cam->aperture > 0.0f) {
		const float ft = cam->focus_distance / vec_dot(ray.direction, cam->forward);
		const struct vector focus_point = alongRay(&ray, ft);
		const struct coord lens_point = coord_scale(cam->aperture, random);
		ray.start = vec_add(ray.start, vec_add(vec_scale(cam->right, lens_point.x), vec_scale(cam->up, lens_point.y)));
		ray.direction = vec_normalize(vec_sub(focus_point, ray.start));
	}
	tform_ray(&ray, cam->composite.A);
	return ray;
}

static bool rays_equal(struct lightRay a, struct lightRay b) {
	return a.start.x == b.start.x && a.start.y == b.start.y && a.start.z == b.start.z &&
		   a.direction.x == b.direction.x && a.direction.y == b.direction.y && a.direction.z == b.direction.z;
}

// Intersect a ray with the focus plane (point P, unit normal N).
static struct vector plane_intersection(struct lightRay ray, struct vector P, struct vector N) {
	const float t = vec_dot(vec_sub(P, ray.start), N) / vec_dot(ray.direction, N);
	return alongRay(&ray, t);
}

static float point_distance(struct vector a, struct vector b) {
	return vec_length(vec_sub(a, b));
}

// 1. Zero tilt must reproduce the legacy ray component-for-component, with
//    both aperture settings, and must not draw any extra sampler dimensions.
static void test_zero_tilt_identical(void) {
	printf("zero tilt reproduces legacy rays exactly\n");
	for (size_t s = 0; s < 3; ++s) {
		for (int aperture_on = 0; aperture_on < 2; ++aperture_on) {
			struct camera cam = make_cam(false, 24, 18, 60.0f, 5.0f, aperture_on ? 0.02f : 0.0f, 0.0f, 0.0f);
			const int xs[] = { 0, 7, 23 };
			const int ys[] = { 0, 9, 17 };
			for (size_t xi = 0; xi < 3; ++xi) {
				for (size_t yi = 0; yi < 3; ++yi) {
					for (int p = 0; p < passes; ++p) {
						struct sampler *a = sampler_new();
						struct sampler *b = sampler_new();
						sampler_init(a, all_samplers[s], p, passes, pixel_index);
						sampler_init(b, all_samplers[s], p, passes, pixel_index);
						struct lightRay got = cam_get_ray(&cam, xs[xi], ys[yi], a);
						struct lightRay ref = legacy_get_ray(&cam, xs[xi], ys[yi], b);
						CHECK(rays_equal(got, ref),
							  "%s aperture=%i pixel=(%i,%i) pass=%i ray differs",
							  sampler_name(all_samplers[s]), aperture_on, xs[xi], ys[yi], p);
						// One more dimension each: the draw sequences must stay aligned.
						const float tail_a = sampler_dimension(a);
						const float tail_b = sampler_dimension(b);
						CHECK(tail_a == tail_b,
							  "%s aperture=%i pixel=(%i,%i) pass=%i sampler sequence diverged",
							  sampler_name(all_samplers[s]), aperture_on, xs[xi], ys[yi], p);
						sampler_destroy(a);
						sampler_destroy(b);
					}
				}
			}
		}
	}
}

// 2. With tilt, a pinhole ray and a lens ray for the same pixel and sample
//    index must meet at the same point of the tilted plane, even though the
//    lens sample moves the ray origin. Reports the measured numerical error.
static void test_tilt_convergence(bool blender, bool transformed) {
	printf("tilt convergence (blender=%d, transformed=%d)\n", blender, transformed);
	struct camera lens = make_cam(blender, 32, 24, 55.0f, 6.0f, 0.03f, 12.0f, -7.0f);
	struct camera pinhole = lens;
	pinhole.aperture = 0.0f;
	if (transformed) {
		struct euler_angles pose = { .roll = 0.1f, .pitch = 0.35f, .yaw = -0.22f };
		struct vector pos = vec(1.5f, -2.0f, 0.75f);
		cam_update_pose(&lens, &pose, &pos);
		cam_update_pose(&pinhole, &pose, &pos);
	}
	CHECK(cam_validate_focus(&lens), "tilted camera rejected as invalid");

	struct vector P_cam = vec_scale(lens.forward, lens.focus_distance);
	struct vector N_cam = cam_focus_plane_normal(&lens);
	struct vector P = P_cam;
	struct vector N = N_cam;
	if (transformed) {
		tform_point(&P, lens.composite.A);
		tform_vector(&N, lens.composite.A);
	}

	float worst_convergence = 0.0f;
	float worst_residual = 0.0f;
	const int xs[] = { 0, 16, 31 };
	const int ys[] = { 0, 12, 23 };
	for (size_t s = 0; s < 3; ++s) {
		for (size_t xi = 0; xi < 3; ++xi) {
			for (size_t yi = 0; yi < 3; ++yi) {
				for (int p = 0; p < passes; ++p) {
					const uint32_t pixel = (uint32_t)(pixel_index + xi * 7 + yi);
					struct sampler *sa = sampler_new();
					struct sampler *sb = sampler_new();
					sampler_init(sa, all_samplers[s], p, passes, pixel);
					sampler_init(sb, all_samplers[s], p, passes, pixel);
					struct lightRay center_ray = cam_get_ray(&pinhole, xs[xi], ys[yi], sa);
					struct lightRay lens_ray = cam_get_ray(&lens, xs[xi], ys[yi], sb);
					struct vector target = plane_intersection(center_ray, P, N);
					struct vector hit = plane_intersection(lens_ray, P, N);
					CHECK(isfinite(hit.x) && isfinite(hit.y) && isfinite(hit.z),
						  "%s non-finite plane hit", sampler_name(all_samplers[s]));
					const float residual = fabsf(vec_dot(vec_sub(hit, P), N));
					if (residual > worst_residual) worst_residual = residual;
					const float err = point_distance(hit, target);
					if (err > worst_convergence) worst_convergence = err;
					CHECK(err < 2.0e-5f,
						  "%s pixel=(%i,%i) pass=%i focus drift %.8g",
						  sampler_name(all_samplers[s]), xs[xi], ys[yi], p, (double)err);
					// Same sample index: the dimension streams must stay in lockstep.
					CHECK(sampler_dimension(sa) == sampler_dimension(sb),
						  "%s pixel=(%i,%i) sampler sequence diverged",
						  sampler_name(all_samplers[s]), xs[xi], ys[yi]);
					sampler_destroy(sa);
					sampler_destroy(sb);
				}
			}
		}
	}
	printf("  max_convergence_error=%.8g max_plane_residual=%.8g\n",
		   (double)worst_convergence, (double)worst_residual);
	CHECK(worst_residual < 1.0e-5f, "focus points leave the tilted plane");
}

// 3. Aperture zero: tilt must not perturb the pinhole ray, and the sampler
//    tail must stay aligned with the untilted camera.
static void test_pinhole_unchanged_by_tilt(void) {
	printf("pinhole rays unchanged by tilt\n");
	for (size_t s = 0; s < 3; ++s) {
		struct camera flat = make_cam(false, 16, 12, 50.0f, 4.0f, 0.0f, 0.0f, 0.0f);
		struct camera tilted = make_cam(false, 16, 12, 50.0f, 4.0f, 0.0f, 25.0f, 15.0f);
		for (int p = 0; p < passes; ++p) {
			struct sampler *a = sampler_new();
			struct sampler *b = sampler_new();
			sampler_init(a, all_samplers[s], p, passes, pixel_index);
			sampler_init(b, all_samplers[s], p, passes, pixel_index);
			struct lightRay ra = cam_get_ray(&flat, 3, 7, a);
			struct lightRay rb = cam_get_ray(&tilted, 3, 7, b);
			CHECK(rays_equal(ra, rb), "%s tilt changed the pinhole ray", sampler_name(all_samplers[s]));
			CHECK(sampler_dimension(a) == sampler_dimension(b),
				  "%s pinhole sampler sequence diverged", sampler_name(all_samplers[s]));
			sampler_destroy(a);
			sampler_destroy(b);
		}
	}
}

// 3b. With the aperture open the tilted and planar cameras must draw exactly
//     the same number of sampler dimensions per ray (jitter x/y and the disc
//     sample), so enabling tilt never shifts the path sampling sequence that
//     follows the camera ray. A whole pixel/pass sequence is compared.
static void test_sampler_stream_parity_with_aperture(void) {
	printf("sampler stream parity with aperture open\n");
	for (size_t s = 0; s < 3; ++s) {
		struct camera flat = make_cam(false, 20, 16, 55.0f, 5.0f, 0.02f, 0.0f, 0.0f);
		struct camera tilted = make_cam(false, 20, 16, 55.0f, 5.0f, 0.02f, 18.0f, -12.0f);
		struct sampler *a = sampler_new();
		struct sampler *b = sampler_new();
		for (int p = 0; p < passes; ++p) {
			sampler_init(a, all_samplers[s], p, passes, pixel_index);
			sampler_init(b, all_samplers[s], p, passes, pixel_index);
			for (int y = 0; y < tilted.height; ++y) {
				for (int x = 0; x < tilted.width; ++x) {
					cam_get_ray(&flat, x, y, a);
					cam_get_ray(&tilted, x, y, b);
					// One extra dimension after each ray: must stay identical.
					CHECK(sampler_dimension(a) == sampler_dimension(b),
						  "%s sampler diverged at (%d,%d) pass %d",
						  sampler_name(all_samplers[s]), x, y, p);
				}
			}
		}
		sampler_destroy(a);
		sampler_destroy(b);
	}
}

// 4. Legal range, near-parallel edge, single-row frame, and the various
//    invalid configurations that update/validation must reject.
static void test_validation(void) {
	printf("focus validation and edge cases\n");

	// Close to the 80 degree limit but still fully in front of the camera:
	// a narrow FOV keeps the corner rays just short of parallel.
	struct camera near_limit = make_cam(false, 64, 48, 18.0f, 5.0f, 0.02f, 79.0f, 0.0f);
	CHECK(cam_validate_focus(&near_limit), "near-parallel 79 degree tilt marked invalid");
	struct sampler *sm = sampler_new();
	sampler_init(sm, Random, 0, 1, pixel_index);
	struct lightRay corner = cam_get_ray(&near_limit, 0, 0, sm);
	CHECK(isfinite(corner.direction.x) && isfinite(corner.direction.y) && isfinite(corner.direction.z),
		  "near-limit corner ray is not finite");
	sampler_destroy(sm);

	// Widening the FOV pushes that same steep tilt past parallel at a corner.
	struct camera parallel = make_cam(false, 64, 48, 25.0f, 5.0f, 0.02f, 79.0f, 0.0f);
	CHECK(!cam_validate_focus(&parallel), "near-parallel corner ray accepted");

	// Single-row image with tilt enabled.
	struct camera single_row = make_cam(false, 64, 1, 45.0f, 3.0f, 0.02f, 20.0f, 0.0f);
	CHECK(cam_validate_focus(&single_row), "single-row tilted camera rejected");
	sm = sampler_new();
	sampler_init(sm, Halton, 0, 1, pixel_index);
	struct lightRay row_ray = cam_get_ray(&single_row, 0, 0, sm);
	CHECK(isfinite(row_ray.direction.z), "single-row ray not finite");
	sampler_destroy(sm);

	// Tilt requires a positive, finite focus distance.
	struct camera no_distance = make_cam(false, 16, 16, 50.0f, 0.0f, 0.0f, 10.0f, 0.0f);
	CHECK(!cam_validate_focus(&no_distance), "tilt with zero focus_distance accepted");
	struct camera negative_distance = make_cam(false, 16, 16, 50.0f, -1.0f, 0.0f, 0.0f, 10.0f);
	CHECK(!cam_validate_focus(&negative_distance), "tilt with negative focus_distance accepted");
	struct camera nan_distance = make_cam(false, 16, 16, 50.0f, 5.0f, 0.0f, 10.0f, 0.0f);
	nan_distance.focus_distance = NAN;
	CHECK(!cam_validate_focus(&nan_distance), "tilt with NaN focus_distance accepted");

	// Angle range: the limit itself and anything beyond it is invalid.
	const float bad_angles[] = { -80.0f, 80.0f, -81.5f, 81.5f };
	for (size_t i = 0; i < 4; ++i) {
		struct camera bad = make_cam(false, 16, 16, 50.0f, 5.0f, 0.0f, bad_angles[i], 0.0f);
		CHECK(!cam_validate_focus(&bad), "out-of-range tilt %.2f accepted", (double)bad_angles[i]);
	}

	// Zero tilt stays unconditionally valid regardless of focus distance.
	struct camera zero_tilt = make_cam(false, 16, 16, 50.0f, 0.0f, 0.0f, 0.0f, 0.0f);
	CHECK(cam_validate_focus(&zero_tilt), "zero-tilt default camera rejected");
}

// 5. Public setter/getter: non-finite values are refused and the old value
//    survives; finite but out-of-range values reach update and fail there.
static void test_public_api(struct cr_scene *scene) {
	printf("public camera parameter API\n");
	cr_camera c = cr_camera_new(scene);
	CHECK(cr_camera_set_num_pref(scene, c, cr_camera_focus_distance, 5.0), "set focus distance");
	CHECK(cr_camera_set_num_pref(scene, c, cr_camera_fov, 50.0), "set fov");
	CHECK(cr_camera_set_num_pref(scene, c, cr_camera_res_x, 32), "set res x");
	CHECK(cr_camera_set_num_pref(scene, c, cr_camera_res_y, 24), "set res y");

	CHECK(cr_camera_set_num_pref(scene, c, cr_camera_focus_tilt_x, 11.0), "set tilt x");
	CHECK(cr_camera_set_num_pref(scene, c, cr_camera_focus_tilt_y, -9.0), "set tilt y");
	CHECK(cr_camera_get_num_pref(scene, c, cr_camera_focus_tilt_x) == 11.0, "get tilt x");
	CHECK(cr_camera_get_num_pref(scene, c, cr_camera_focus_tilt_y) == -9.0, "get tilt y");
	CHECK(cr_camera_update(scene, c), "valid tilted camera update failed");

	const double non_finite[] = { (double)NAN, (double)INFINITY, -(double)INFINITY };
	for (size_t i = 0; i < 3; ++i) {
		CHECK(!cr_camera_set_num_pref(scene, c, cr_camera_focus_tilt_x, non_finite[i]),
			  "non-finite tilt x accepted");
		CHECK(cr_camera_get_num_pref(scene, c, cr_camera_focus_tilt_x) == 11.0,
			  "failed setter overwrote tilt x");
		CHECK(!cr_camera_set_num_pref(scene, c, cr_camera_focus_tilt_y, non_finite[i]),
			  "non-finite tilt y accepted");
		CHECK(cr_camera_get_num_pref(scene, c, cr_camera_focus_tilt_y) == -9.0,
			  "failed setter overwrote tilt y");
	}

	CHECK(cr_camera_set_num_pref(scene, c, cr_camera_focus_tilt_x, 80.0), "finite 80 deg stored");
	CHECK(!cr_camera_update(scene, c), "80 degree tilt update accepted");
	CHECK(cr_camera_set_num_pref(scene, c, cr_camera_focus_tilt_x, 11.0), "restore tilt x");
	CHECK(cr_camera_update(scene, c), "restored camera still invalid");
}

static const char *write_temp_json(const char *contents) {
	const char *tmpdir = getenv("TMPDIR");
	if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
	char template[512];
	snprintf(template, sizeof(template), "%s/cray-focus-tilt-XXXXXX", tmpdir);
	int fd = mkstemp(template);
	CHECK(fd >= 0, "mkstemp failed");
	if (fd < 0) return NULL;
	ssize_t len = (ssize_t)strlen(contents);
	ssize_t written = write(fd, contents, (size_t)len);
	CHECK(written == len, "short json write");
	close(fd);
	return stringCopy(template);
}

static const char *camera_json_with_tilt(const char *tilt) {
	static char buf[2048];
	snprintf(buf, sizeof(buf),
		"{"
		"\"renderer\": {\"samples\": 1, \"bounces\": 1, \"tileWidth\": 16, \"tileHeight\": 16},"
		"\"camera\": {"
		"\"FOV\": 50.0, \"focalDistance\": 5.0, \"fstops\": 8.0,"
		"\"width\": 32, \"height\": 24%s%s},"
		"\"scene\": {\"ambientColor\": {\"type\": \"background\","
		"\"down\": {\"r\": 0.1, \"g\": 0.1, \"b\": 0.1}, \"up\": {\"r\": 0.2, \"g\": 0.2, \"b\": 0.2}}}"
		"}",
		tilt ? ", \"focusTilt\":" : "",
		tilt ? tilt : "");
	return buf;
}

// 6. JSON loading through the public cr_load_json entry point.
static void test_json_loading(void) {
	printf("json focusTilt loading\n");

	// Missing key means [0, 0].
	{
		const char *path = write_temp_json(camera_json_with_tilt(NULL));
		struct cr_renderer *r = cr_new_renderer();
		CHECK(cr_load_json(r, path), "plain camera json failed to load");
		struct cr_scene *s = cr_renderer_scene_get(r);
		CHECK(cr_camera_get_num_pref(s, 0, cr_camera_focus_tilt_x) == 0.0, "default tilt x nonzero");
		CHECK(cr_camera_get_num_pref(s, 0, cr_camera_focus_tilt_y) == 0.0, "default tilt y nonzero");
		cr_destroy_renderer(r);
		remove(path);
		free((void *)path);
	}

	// Valid tilt round-trips through the real file loader.
	{
		const char *path = write_temp_json(camera_json_with_tilt("[12.5, -6.25]"));
		struct cr_renderer *r = cr_new_renderer();
		CHECK(cr_load_json(r, path), "tilted camera json failed to load");
		struct cr_scene *s = cr_renderer_scene_get(r);
		CHECK(cr_camera_get_num_pref(s, 0, cr_camera_focus_tilt_x) == 12.5, "tilt x json mismatch");
		CHECK(cr_camera_get_num_pref(s, 0, cr_camera_focus_tilt_y) == -6.25, "tilt y json mismatch");
		cr_destroy_renderer(r);
		remove(path);
		free((void *)path);
	}

	// Out-of-range / malformed tilt must fail the load and roll the camera back.
	const char *bad_payloads[] = {
		"[80, 0]", "[-81, 0]", "[10]", "[10, 0, 5]", "[10, \"x\"]", "12",
	};
	for (size_t i = 0; i < 6; ++i) {
		const char *path = write_temp_json(camera_json_with_tilt(bad_payloads[i]));
		struct cr_renderer *r = cr_new_renderer();
		CHECK(!cr_load_json(r, path), "bad focusTilt loaded: %s", bad_payloads[i]);
		CHECK(cr_scene_totals(cr_renderer_scene_get(r)).cameras == 0,
			  "failed camera parse left a camera behind: %s", bad_payloads[i]);
		cr_destroy_renderer(r);
		remove(path);
		free((void *)path);
	}

	// A scene with no camera block still fails through the historical
	// "no cameras specified" path and must not leave any camera behind.
	{
		const char *no_camera =
			"{\"renderer\": {\"samples\": 1, \"bounces\": 1},"
			"\"scene\": {\"ambientColor\": {\"type\": \"background\"}}}";
		const char *path = write_temp_json(no_camera);
		struct cr_renderer *r = cr_new_renderer();
		CHECK(!cr_load_json(r, path), "camera-less scene loaded");
		CHECK(cr_scene_totals(cr_renderer_scene_get(r)).cameras == 0,
			  "camera-less scene produced a camera");
		cr_destroy_renderer(r);
		remove(path);
		free((void *)path);
	}
}

// 7. In-memory serialize/deserialize keeps the tilt and stays stable.
static void test_serializer_roundtrip(void) {
	printf("serialize/deserialize round trip\n");
	struct cr_renderer *ext = cr_new_renderer();
	struct cr_scene *scene = cr_renderer_scene_get(ext);
	cr_camera c = cr_camera_new(scene);
	cr_camera_set_num_pref(scene, c, cr_camera_fov, 55.0);
	cr_camera_set_num_pref(scene, c, cr_camera_focus_distance, 7.0);
	cr_camera_set_num_pref(scene, c, cr_camera_fstops, 8.0);
	cr_camera_set_num_pref(scene, c, cr_camera_res_x, 40);
	cr_camera_set_num_pref(scene, c, cr_camera_res_y, 30);
	cr_camera_set_num_pref(scene, c, cr_camera_focus_tilt_x, 18.5);
	cr_camera_set_num_pref(scene, c, cr_camera_focus_tilt_y, -4.25);
	CHECK(cr_camera_update(scene, c), "serializer fixture camera invalid");

	struct renderer *r = (struct renderer *)ext;
	char *first = serialize_renderer(r);
	CHECK(first != NULL, "serialize returned NULL");
	struct renderer *restored = deserialize_renderer(first);
	CHECK(restored != NULL, "deserialize returned NULL");
	CHECK(v_arr_len(restored->scene->cameras) == 1, "camera count changed over the wire");
	struct camera *cam = &restored->scene->cameras[0];
	CHECK(cam->focus_tilt_x == 18.5f, "tilt x lost in serialization");
	CHECK(cam->focus_tilt_y == -4.25f, "tilt y lost in serialization");

	char *second = serialize_renderer(restored);
	CHECK(second != NULL && stringEquals(first, second), "serialization is not stable");
	free(first);
	free(second);
	cr_destroy_renderer(ext);
	cr_destroy_renderer((struct cr_renderer *)restored);
}

int main(void) {
	cr_log_level_set(Silent);

	test_zero_tilt_identical();
	test_tilt_convergence(false, false);
	test_tilt_convergence(true, false);
	test_tilt_convergence(false, true);
	test_pinhole_unchanged_by_tilt();
	test_sampler_stream_parity_with_aperture();
	test_validation();

	struct cr_renderer *api_renderer = cr_new_renderer();
	test_public_api(cr_renderer_scene_get(api_renderer));
	cr_destroy_renderer(api_renderer);

	test_json_loading();
	test_serializer_roundtrip();

	printf("\n%u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
