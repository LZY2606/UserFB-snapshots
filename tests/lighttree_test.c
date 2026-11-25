/*
 * Deterministic LightTree tests.  The runner supplies main() and collects the
 * cases exported below.
 */

#include "task_support/check.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "calc.h"
#include "material.h"
#include "object.h"
#include "render.h"
#include "task_support/production.h"

static struct Object *make_sphere(const v3 position, const float radius, struct Material *material)
{
	struct Object *object = sphere_new(position, radius);
	object_init(object, material, 0.0001f, 1u, OBJECT_SPHERE);
	object->object_data->postinit(object);
	return object;
}

static void zero3(v3 x)
{
	x[0] = x[1] = x[2] = 0.f;
}

struct one_sphere_scene {
	struct Material receiver;
	struct Texture *texture;
};

static int setup_one_sphere(struct one_sphere_scene *scene, const v3 receiver_color)
{
	v3 zero;
	zero3(zero);
	memset(scene, 0, sizeof(*scene));
	scene->texture = texture_uniform_new(receiver_color);
	material_init(&scene->receiver, 1, zero, zero, zero, zero, zero, 2.f, 1.f, scene->texture);

	num_objects = 1;
	num_emittant_objects = 0;
	num_unbound_objects = 0;
	objects_init();
	objects[0] = make_sphere((v3){ 0.f, 0.f, 0.f }, 1.f, &scene->receiver);
	accel_init();
	return 1;
}

static void teardown_one_sphere(struct one_sphere_scene *scene)
{
	render_clear_point_lights();
	accel_deinit();
	objects_deinit();
	free(scene->texture);
	num_objects = 0;
	num_emittant_objects = 0;
	num_unbound_objects = 0;
}

struct two_sphere_scene {
	struct Material receiver;
	struct Material blocker;
	struct Texture *receiver_texture;
	struct Texture *blocker_texture;
};

static int setup_two_spheres(struct two_sphere_scene *scene,
	const v3 receiver_color,
	const v3 blocker_position,
	const float blocker_radius,
	const v3 blocker_transmission)
{
	v3 zero;
	zero3(zero);
	memset(scene, 0, sizeof(*scene));
	scene->receiver_texture = texture_uniform_new(receiver_color);
	scene->blocker_texture = texture_uniform_new(zero);
	material_init(&scene->receiver, 1, zero, zero, zero, zero, zero, 2.f, 1.f,
		scene->receiver_texture);
	material_init(&scene->blocker, 2, zero, zero, zero, blocker_transmission, zero, 2.f, 1.f,
		scene->blocker_texture);

	num_objects = 2;
	num_emittant_objects = 0;
	num_unbound_objects = 0;
	objects_init();
	objects[0] = make_sphere((v3){ 0.f, 0.f, 0.f }, 1.f, &scene->receiver);
	objects[1] = make_sphere(blocker_position, blocker_radius, &scene->blocker);
	accel_init();
	return 1;
}

static void teardown_two_spheres(struct two_sphere_scene *scene)
{
	render_clear_point_lights();
	accel_deinit();
	objects_deinit();
	free(scene->receiver_texture);
	free(scene->blocker_texture);
	num_objects = 0;
	num_emittant_objects = 0;
	num_unbound_objects = 0;
}

struct reference_result {
	double color[3];
	size_t shadow_queries;
};

/* Independent per-light implementation.  It deliberately lives in the test
 * rather than calling any LightTree traversal helper. */
static void reference_integrate(const PointLight *lights,
	const size_t count,
	const v3 point,
	const v3 normal,
	const v3 diffuse,
	const double offset,
	struct reference_result *reference)
{
	size_t i, c;

	memset(reference, 0, sizeof(*reference));
	for (i = 0; i < count; ++i) {
		v3 delta;
		v3 wi;
		v3 transmission = { 1.f, 1.f, 1.f };
		struct Ray ray;
		float distance;

		sub3v(lights[i].position, point, delta);
		distance = mag3(delta);
		if (!(distance > 0.f))
			continue;

		mul3s(delta, 1.f / distance, wi);
		assign3(ray.point, point);
		assign3(ray.direction, wi);
		++reference->shadow_queries;
		if (is_light_blocked(&ray, distance, transmission, NULL))
			continue;

		for (c = 0; c < 3; ++c) {
			double trans = (double)transmission[c];
			double dx = (double)lights[i].position[0] - (double)point[0];
			double dy = (double)lights[i].position[1] - (double)point[1];
			double dz = (double)lights[i].position[2] - (double)point[2];
			double distance_sq = dx * dx + dy * dy + dz * dz;
			double n_dot_wi = (double)normal[0] * (double)wi[0]
				+ (double)normal[1] * (double)wi[1]
				+ (double)normal[2] * (double)wi[2];

			trans = fmax(0.0, fmin(1.0, trans));
			n_dot_wi = fmax(0.0, n_dot_wi);
			reference->color[c] += (double)diffuse[c] * (double)lights[i].intensity[c]
				* n_dot_wi / (offset + distance_sq) * trans;
		}
	}
}

static int interval_matches_reference(const LightResult *result,
	const struct reference_result *reference,
	const double abs_tol,
	const double rel_tol)
{
	int c;

	for (c = 0; c < 3; ++c) {
		const double exact = reference->color[c];
		const double rounding = 2e-6 * fmax(1.0, fabs(exact));
		const double endpoint_budget = abs_tol + rel_tol * result->lower[c] + rounding;

		if (!isfinite(result->color[c]) || !isfinite(result->lower[c]) || !isfinite(result->upper[c]))
			return 0;
		if (!(result->lower[c] <= result->upper[c] + rounding))
			return 0;
		if (result->lower[c] > exact + rounding || result->upper[c] < exact - rounding)
			return 0;
		if (result->color[c] < result->lower[c] - rounding
			|| result->color[c] > result->upper[c] + rounding)
			return 0;
		if (result->color[c] - result->lower[c] > endpoint_budget)
			return 0;
		if (result->upper[c] - result->color[c] > endpoint_budget)
			return 0;
		if (fabs(result->color[c] - exact) > abs_tol + rel_tol * fabs(exact) + rounding)
			return 0;
	}
	return 1;
}

static int test_empty_tree(void)
{
	LightTree *tree = lighttree_build(NULL, 0u);
	LightResult result;
	static const v3 point = { 0.f, 0.f, 1.f };
	static const v3 normal = { 0.f, 0.f, 1.f };
	static const v3 diffuse = { 1.f, 1.f, 1.f };
	int c;

	TASK_CHECK(tree != NULL);
	lighttree_eval(tree, point, normal, diffuse, 1.0, 0.01, 0.01, &result);
	for (c = 0; c < 3; ++c) {
		TASK_CHECK(result.color[c] == 0.0);
		TASK_CHECK(result.lower[c] == 0.0);
		TASK_CHECK(result.upper[c] == 0.0);
	}
	TASK_CHECK(result.bound_evals == 0u);
	TASK_CHECK(result.leaf_evals == 0u);
	TASK_CHECK(result.shadow_queries == 0u);
	lighttree_free(tree);
	lighttree_free(NULL);
	return 0;
}

static int test_bounds_reference_duplicates_and_balance(void)
{
	struct one_sphere_scene scene;
	static const v3 receiver_color = { 1.f, 1.f, 1.f };
	static const v3 point = { 0.f, 0.f, 1.f };
	static const v3 normal = { 0.f, 0.f, 1.f };
	static const v3 diffuse = { 0.375f, 0.75f, 1.f };
	PointLight lights[9];
	LightTree *tree;
	LightResult exact_result, approximate_result, repeated_result;
	struct reference_result reference;
	const double offset = 0.75;
	const double abs_tol = 1e-5;
	const double rel_tol = 1e-4;
	size_t i;
	int c;

	TASK_CHECK(setup_one_sphere(&scene, receiver_color));

	lights[0] = (PointLight){ .position = { 0.f, 0.f, 5.f }, .intensity = { 8.f, 0.002f, 0.5f } };
	lights[1] = (PointLight){ .position = { 0.f, 0.f, 5.f }, .intensity = { 2.f, 16.f, 1e-7f } };
	lights[2] = (PointLight){ .position = { 0.f, 0.f, 5.f }, .intensity = { 1e-6f, 0.001f, 32.f } };
	lights[3] = (PointLight){ .position = { 3.f, -1.f, 4.f }, .intensity = { 0.25f, 7.f, 0.03f } };
	lights[4] = (PointLight){ .position = { -4.f, 2.f, 8.f }, .intensity = { 33.f, 1e-5f, 2.f } };
	lights[5] = (PointLight){ .position = { 1.f, 1.f, 2.f }, .intensity = { 0.01f, 0.02f, 0.03f } };
	lights[6] = (PointLight){ .position = { -2.f, -3.f, 6.f }, .intensity = { 9.f, 8.f, 7.f } };
	lights[7] = (PointLight){ .position = { 50.f, 40.f, 30.f }, .intensity = { 1e-4f, 1e5f, 0.1f } };
	lights[8] = (PointLight){ .position = { 0.f, 0.f, 1.f }, .intensity = { 100.f, 100.f, 100.f } };

	tree = lighttree_build(lights, arrlen(lights));
	TASK_CHECK(tree != NULL);

	reference_integrate(lights, arrlen(lights), point, normal, diffuse, offset, &reference);
	lighttree_eval(tree, point, normal, diffuse, offset, 0.0, 0.0, &exact_result);
	TASK_CHECK(exact_result.bound_evals == 2u * arrlen(lights) - 1u);
	TASK_CHECK(exact_result.leaf_evals == arrlen(lights));
	TASK_CHECK(exact_result.shadow_queries == reference.shadow_queries);
	TASK_CHECK(interval_matches_reference(&exact_result, &reference, 0.0, 0.0));
	for (c = 0; c < 3; ++c)
		TASK_CHECK(fabs(exact_result.upper[c] - exact_result.lower[c])
			<= 2e-6 * fmax(1.0, fabs(reference.color[c])));

	lighttree_eval(tree, point, normal, diffuse, offset, abs_tol, rel_tol, &approximate_result);
	TASK_CHECK(interval_matches_reference(&approximate_result, &reference, abs_tol, rel_tol));
	TASK_CHECK(approximate_result.leaf_evals <= arrlen(lights));
	TASK_CHECK(approximate_result.shadow_queries <= approximate_result.leaf_evals);

	lighttree_eval(tree, point, normal, diffuse, offset, abs_tol, rel_tol, &repeated_result);
	TASK_CHECK(memcmp(&approximate_result, &repeated_result, sizeof(LightResult)) == 0);

	/* The snapshot must not alias or retain the caller's array. */
	for (i = 0; i < arrlen(lights); ++i) {
		lights[i].position[0] = 12345.f;
		lights[i].intensity[0] = 0.f;
	}
	lighttree_eval(tree, point, normal, diffuse, offset, 0.0, 0.0, &repeated_result);
	TASK_CHECK(interval_matches_reference(&repeated_result, &reference, 0.0, 0.0));

	lighttree_free(tree);
	teardown_one_sphere(&scene);
	return 0;
}

static int test_large_abs_tolerance_work(void)
{
	enum { COUNT = 4096 };
	struct one_sphere_scene scene;
	static const v3 receiver_color = { 1.f, 1.f, 1.f };
	static const v3 point = { 0.f, 0.f, 1.f };
	static const v3 normal = { 0.f, 0.f, 1.f };
	PointLight *lights;
	LightTree *tree;
	LightResult result;
	struct reference_result reference;
	/* About 6% of the total reference value and large enough to absorb the
	 * aggregate distant cluster while still certifying the bright light. */
	const double abs_tol = 0.1;
	const double offset = 1.0;
	size_t i;

	TASK_CHECK(setup_one_sphere(&scene, receiver_color));
	lights = malloc((size_t)COUNT * sizeof(*lights));
	TASK_CHECK(lights != NULL);

	for (i = 0; i < (size_t)COUNT; ++i) {
		size_t ix = i % 64u;
		size_t iy = i / 64u;
		lights[i] = (PointLight){
			.position = { ((float)ix - 31.5f) * 0.6f, ((float)iy - 31.5f) * 0.6f, 20.f },
			.intensity = { 0.01f, 0.01f, 0.01f },
		};
	}
	/* Spatially separated near bright light. */
	lights[0] = (PointLight){ .position = { 0.f, 0.f, 3.f }, .intensity = { 8.f, 8.f, 8.f } };

	tree = lighttree_build(lights, (size_t)COUNT);
	TASK_CHECK(tree != NULL);
	reference_integrate(lights, (size_t)COUNT, point, normal, receiver_color, offset, &reference);
	lighttree_eval(tree, point, normal, receiver_color, offset, abs_tol, 0.0, &result);

	printf("LIGHTTREE_WORK n=%d bound=%zu leaf=%zu shadow=%zu ref=%.9g result=%.9g tol=%.3g\n",
		COUNT, result.bound_evals, result.leaf_evals, result.shadow_queries,
		reference.color[0], result.color[0], abs_tol);

	TASK_CHECK(interval_matches_reference(&result, &reference, abs_tol, 0.0));
	TASK_CHECK(result.leaf_evals < (size_t)COUNT / 8u);
	TASK_CHECK(result.shadow_queries < (size_t)COUNT / 8u);
	TASK_CHECK(result.bound_evals < (size_t)COUNT / 2u);
	TASK_CHECK(result.shadow_queries <= result.leaf_evals);

	lighttree_free(tree);
	free(lights);
	teardown_one_sphere(&scene);
	return 0;
}

static int test_partial_occlusion(void)
{
	struct two_sphere_scene scene;
	static const v3 receiver_color = { 1.f, 1.f, 1.f };
	static const v3 blocker_position = { 1.f, 0.f, 3.f };
	static const v3 opaque = { 0.f, 0.f, 0.f };
	static const v3 point = { 0.f, 0.f, 1.f };
	static const v3 normal = { 0.f, 0.f, 1.f };
	PointLight lights[6];
	LightTree *tree;
	LightResult zero_result, tol_result;
	struct reference_result reference;
	int c;

	TASK_CHECK(setup_two_spheres(&scene, receiver_color, blocker_position, 0.5f, opaque));

	lights[0] = (PointLight){ .position = { 2.f, 0.f, 5.f }, .intensity = { 4.f, 4.f, 4.f } };
	lights[1] = (PointLight){ .position = { 0.f, 0.f, 5.f }, .intensity = { 2.f, 3.f, 4.f } };
	lights[2] = (PointLight){ .position = { 2.1f, 0.1f, 5.1f }, .intensity = { 1.f, 1.f, 1.f } };
	lights[3] = (PointLight){ .position = { -0.2f, 0.1f, 5.f }, .intensity = { 1.f, 2.f, 3.f } };
	lights[4] = (PointLight){ .position = { 2.f, 0.2f, 5.2f }, .intensity = { 0.5f, 0.4f, 0.3f } };
	lights[5] = (PointLight){ .position = { -2.f, 1.f, 6.f }, .intensity = { 3.f, 2.f, 1.f } };

	tree = lighttree_build(lights, arrlen(lights));
	TASK_CHECK(tree != NULL);
	reference_integrate(lights, arrlen(lights), point, normal, receiver_color, 1.0, &reference);
	TASK_CHECK(reference.color[0] > 0.0);
	/* The axial light is visible while the center line of light 0 is opaque. */
	TASK_CHECK(reference.shadow_queries == arrlen(lights));

	lighttree_eval(tree, point, normal, receiver_color, 1.0, 0.0, 0.0, &zero_result);
	TASK_CHECK(zero_result.leaf_evals == arrlen(lights));
	TASK_CHECK(zero_result.shadow_queries == arrlen(lights));
	TASK_CHECK(interval_matches_reference(&zero_result, &reference, 0.0, 0.0));

	lighttree_eval(tree, point, normal, receiver_color, 1.0, 0.25, 0.0, &tol_result);
	TASK_CHECK(interval_matches_reference(&tol_result, &reference, 0.25, 0.0));
	TASK_CHECK(tol_result.leaf_evals <= arrlen(lights));

	/* A blocked-only estimate would be visibly below the mixed reference. */
	for (c = 0; c < 3; ++c)
		TASK_CHECK(zero_result.color[c] > 0.0);

	lighttree_free(tree);
	teardown_two_spheres(&scene);
	return 0;
}

static int test_backface_and_coincident(void)
{
	struct one_sphere_scene scene;
	static const v3 white = { 1.f, 1.f, 1.f };
	static const v3 point = { 0.f, 0.f, 1.f };
	static const v3 normal = { 0.f, 0.f, 1.f };
	PointLight lights[3];
	LightTree *tree;
	LightResult result;
	struct reference_result reference;

	TASK_CHECK(setup_one_sphere(&scene, white));
	lights[0] = (PointLight){ .position = { 0.f, 0.f, 1.f }, .intensity = { 9.f, 9.f, 9.f } };
	lights[1] = (PointLight){ .position = { 0.f, 0.f, -4.f }, .intensity = { 9.f, 9.f, 9.f } };
	lights[2] = (PointLight){ .position = { 0.f, 0.f, 1.f }, .intensity = { 0.f, 0.f, 0.f } };

	tree = lighttree_build(lights, arrlen(lights));
	TASK_CHECK(tree != NULL);
	reference_integrate(lights, arrlen(lights), point, normal, white, 1.0, &reference);
	lighttree_eval(tree, point, normal, white, 1.0, 0.0, 0.0, &result);
	TASK_CHECK(result.leaf_evals == arrlen(lights));
	TASK_CHECK(result.shadow_queries == reference.shadow_queries);
	TASK_CHECK(result.shadow_queries == 1u); /* only the back light has a positive segment */
	TASK_CHECK(interval_matches_reference(&result, &reference, 0.0, 0.0));
	TASK_CHECK(result.color[0] == 0.0 && result.color[1] == 0.0 && result.color[2] == 0.0);

	lighttree_free(tree);
	teardown_one_sphere(&scene);
	return 0;
}

static int test_colored_transmission(void)
{
	struct two_sphere_scene scene;
	static const v3 white = { 1.f, 1.f, 1.f };
	static const v3 filter_color = { 0.5f, 0.25f, 0.75f };
	static const v3 blocker_position = { 0.f, 0.f, 3.f };
	static const v3 point = { 0.f, 0.f, 1.f };
	static const v3 normal = { 0.f, 0.f, 1.f };
	PointLight lights[3];
	LightTree *tree;
	LightResult result;
	struct reference_result reference;
	int c;

	TASK_CHECK(setup_two_spheres(&scene, white, blocker_position, 0.25f, filter_color));

	lights[0] = (PointLight){ .position = { 0.f, 0.f, 5.f }, .intensity = { 8.f, 4.f, 2.f } };
	lights[1] = (PointLight){ .position = { 3.f, 0.f, 4.f }, .intensity = { 2.f, 4.f, 8.f } };
	lights[2] = (PointLight){ .position = { -2.f, 1.f, 5.f }, .intensity = { 1.f, 1.f, 1.f } };

	tree = lighttree_build(lights, arrlen(lights));
	TASK_CHECK(tree != NULL);
	reference_integrate(lights, arrlen(lights), point, normal, white, 1.0, &reference);
	TASK_CHECK(reference.color[0] > 0.0);
	TASK_CHECK(reference.shadow_queries == arrlen(lights));

	lighttree_eval(tree, point, normal, white, 1.0, 0.0, 0.0, &result);
	TASK_CHECK(result.leaf_evals == arrlen(lights));
	TASK_CHECK(result.shadow_queries == arrlen(lights));
	TASK_CHECK(interval_matches_reference(&result, &reference, 0.0, 0.0));

	/* Explicit content assertion which fails if transmission is forced to 1:
	 * the axial red/green/blue term must retain 0.5/0.25/0.75 filtering. */
	TASK_CHECK(fabs(result.color[0] - reference.color[0]) <= 2e-6 * fmax(1.0, reference.color[0]));
	TASK_CHECK(fabs(result.color[1] - reference.color[1]) <= 2e-6 * fmax(1.0, reference.color[1]));
	TASK_CHECK(fabs(result.color[2] - reference.color[2]) <= 2e-6 * fmax(1.0, reference.color[2]));
	for (c = 0; c < 3; ++c)
		TASK_CHECK(result.upper[c] + 2e-6 * fmax(1.0, reference.color[c]) >= reference.color[c]);

	lighttree_free(tree);
	teardown_two_spheres(&scene);
	return 0;
}

static int test_cast_ray_texture_attenuation_replacement_clear(void)
{
	struct one_sphere_scene scene;
	static const v3 texture_color = { 0.5f, 0.25f, 1.f };
	static const PointLight one_light = {
		.position = { 0.f, 0.f, 5.f },
		.intensity = { 1.f, 1.f, 1.f },
	};
	v3 color_far = { 0.f, 0.f, 0.f };
	v3 color_near = { 0.f, 0.f, 0.f };
	v3 color_weighted = { 0.f, 0.f, 0.f };
	v3 color_replaced = { 0.f, 0.f, 0.f };
	v3 color_offset = { 0.f, 0.f, 0.f };
	v3 color_cleared = { 0.f, 0.f, 0.f };
	v3 kr = { 1.f, 1.f, 1.f };
	v3 kr_weight = { 0.5f, 0.25f, 0.75f };
	struct Ray view_far = { .direction = { 0.f, 0.f, -1.f }, .point = { 0.f, 0.f, 4.f } };
	struct Ray view_near = { .direction = { 0.f, 0.f, -1.f }, .point = { 0.f, 0.f, 2.f } };
	const double expected_far[3] = { 0.5 / 272.0, 0.25 / 272.0, 1.0 / 272.0 };
	size_t c;
	float depth;
	int expected_random, actual_random;

	TASK_CHECK(setup_one_sphere(&scene, texture_color));
	render_clear_point_lights();
	render_set_point_lights(&one_light, 1u, 1.0, 0.0, 0.0);

	srand(4242);
	expected_random = rand();
	srand(4242);
	depth = cast_ray(&view_far, kr, color_far, 1u, NULL);
	TASK_CHECK(fabs((double)depth - 3.0) < 1e-6);
	for (c = 0; c < 3; ++c)
		TASK_CHECK(fabs((double)color_far[c] - expected_far[c]) < 1e-6);
	actual_random = rand();
	TASK_CHECK(actual_random == expected_random);

	cast_ray(&view_near, kr, color_near, 1u, NULL);
	for (c = 0; c < 3; ++c)
		TASK_CHECK(fabs((double)color_near[c] - 4.0 * expected_far[c]) < 2e-6);

	cast_ray(&view_far, kr_weight, color_weighted, 1u, NULL);
	for (c = 0; c < 3; ++c)
		TASK_CHECK(fabs((double)color_weighted[c] - expected_far[c] * (double)kr_weight[c]) < 1e-6);

	const PointLight replacement = {
		.position = { 0.f, 0.f, 5.f },
		.intensity = { 2.f, 3.f, 4.f },
	};
	render_set_point_lights(&replacement, 1u, 1.0, 0.0, 0.0);
	cast_ray(&view_far, kr, color_replaced, 1u, NULL);
	for (c = 0; c < 3; ++c)
		TASK_CHECK(fabs((double)color_replaced[c] - expected_far[c] * (double)(c + 2u)) < 1e-6);

	render_set_point_lights(&one_light, 1u, 3.0, 0.0, 0.0);
	cast_ray(&view_far, kr, color_offset, 1u, NULL);
	for (c = 0; c < 3; ++c)
		TASK_CHECK(fabs((double)color_offset[c] - expected_far[c] * (17.0 / 19.0)) < 1e-6);

	render_clear_point_lights();
	srand(99);
	expected_random = rand();
	srand(99);
	cast_ray(&view_far, kr, color_cleared, 1u, NULL);
	for (c = 0; c < 3; ++c)
		TASK_CHECK(color_cleared[c] == 0.f);
	TASK_CHECK(rand() == expected_random);

	teardown_one_sphere(&scene);
	return 0;
}

static struct ObjectVTable fixed_emitter_vtable;

static void fixed_emitter_point(const struct Object *object, const v3 point, v3 out)
{
	(void)object;
	(void)point;
	assign3(out, (v3){ 0.f, 0.f, 5.f });
}

static struct Object *baseline_sphere(const v3 position, const float radius, struct Material *material)
{
	struct Object *object = sphere_new(position, radius);
	object_init(object, material, 0.0001f, 1u, OBJECT_SPHERE);
	object->object_data->postinit(object);
	return object;
}

static int test_old_emitter_channel_control(void)
{
	struct Material receiver, filter, emitter;
	struct Texture *texture, *black;
	v3 zero = { 0.f, 0.f, 0.f };
	v3 color = { 0.f, 0.f, 0.f };
	v3 kr = { 1.f, 1.f, 1.f };
	struct Ray view = { .direction = { 0.f, 0.f, -1.f }, .point = { 0.f, 0.f, 2.f } };
	int expected_random;

	render_clear_point_lights();
	texture = texture_uniform_new((v3){ 0.5f, 0.25f, 1.f });
	black = texture_uniform_new(zero);
	material_init(&receiver, 1, zero, zero, zero, zero, zero, 2.f, 1.f, texture);
	material_init(&filter, 2, zero, zero, zero, (v3){ 0.5f, 0.25f, 0.75f }, zero, 2.f, 1.f, black);
	material_init(&emitter, 3, zero, zero, zero, zero, (v3){ 8.f, 4.f, 2.f }, 2.f, 1.f, black);

	num_objects = 3;
	num_emittant_objects = 1;
	num_unbound_objects = 0;
	objects_init();
	objects[0] = baseline_sphere((v3){ 0.f, 0.f, 0.f }, 1.f, &receiver);
	objects[1] = baseline_sphere((v3){ 0.f, 0.f, 3.f }, 0.25f, &filter);
	objects[2] = baseline_sphere((v3){ 0.f, 0.f, 5.f }, 0.1f, &emitter);
	fixed_emitter_vtable = *objects[2]->object_data;
	fixed_emitter_vtable.get_light_point = fixed_emitter_point;
	objects[2]->object_data = &fixed_emitter_vtable;
	emittant_objects[0] = objects[2];
	accel_init();

	srand(1675);
	expected_random = rand();
	srand(1675);
	float depth = cast_ray(&view, kr, color, 1u, NULL);
	TASK_CHECK(fabs((double)depth - 1.0) < 1e-6);
	TASK_CHECK(fabs((double)color[0] - 0.5 / 17.0) < 2e-6);
	TASK_CHECK(fabs((double)color[1] - 0.0625 / 17.0) < 2e-6);
	TASK_CHECK(fabs((double)color[2] - 0.375 / 17.0) < 2e-6);
	TASK_CHECK(rand() == expected_random);

	accel_deinit();
	objects_deinit();
	free(texture);
	free(black);
	num_objects = 0;
	num_emittant_objects = 0;
	num_unbound_objects = 0;
	return 0;
}

const struct task_case task_cases[] = {
	{ "bounds", "empty_tree", test_empty_tree },
	{ "bounds", "reference_duplicates_balance", test_bounds_reference_duplicates_and_balance },
	{ "bounds", "large_abs_tolerance_work", test_large_abs_tolerance_work },
	{ "visibility", "partial_occlusion", test_partial_occlusion },
	{ "visibility", "backface_and_coincident", test_backface_and_coincident },
	{ "transport", "colored_transmission", test_colored_transmission },
	{ "transport", "cast_ray_integration", test_cast_ray_texture_attenuation_replacement_clear },
	{ "transport", "old_emitter_control", test_old_emitter_channel_control },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
