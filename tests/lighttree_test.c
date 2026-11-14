/*
 * Task tests for the deterministic hierarchical point-light integrator.
 *
 * Every numerical claim is checked against an independent per-light reference
 * that drives the production shadow path (is_light_blocked with a NULL emitter)
 * directly: the tree and the reference never share aggregation code.  Real
 * sphere geometry provides partial occlusion and colored transmission.
 */

#include "../task_support/check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "accel.h"
#include "calc.h"
#include "lighttree.h"
#include "material.h"
#include "object.h"
#include "render.h"
#include "../task_support/production.h"

#define LT_TEST_CHANNELS 3
#define TEST_EPS 1e-12

enum SceneKind {
	SCENE_RECEIVER,
	SCENE_FILTER,
	SCENE_OCCLUDER,
	SCENE_FILTER_OCCLUDER,
	SCENE_EMITTER,
};

static struct Material test_materials[4];
static struct Texture *test_textures[4];
static struct ObjectVTable emitter_vtable;

static int near_value(const double got, const double want, const double slack)
{
	return fabs(got - want) <= 2e-6 * fmax(1., fmax(fabs(got), fabs(want))) + slack;
}

static struct Object *make_sphere(const v3 position, const float radius, struct Material *material)
{
	struct Object *object = sphere_new(position, radius);
	object_init(object, material, 0.0001f, 1, OBJECT_SPHERE);
	object->object_data->postinit(object);
	return object;
}

static void fixed_light_point(const struct Object *object, const v3 point, v3 out)
{
	(void)object;
	(void)point;
	assign3(out, (v3){ 0.f, 0.f, 5.f });
}

static void scene_setup(const enum SceneKind kind)
{
	const v3 zero = { 0.f, 0.f, 0.f };
	size_t num_extra = 0;
	bool has_filter = false;
	bool has_occluder = false;
	bool has_emitter = false;

	switch (kind) {
	case SCENE_FILTER:
		has_filter = true;
		num_extra = 1;
		break;
	case SCENE_OCCLUDER:
		has_occluder = true;
		num_extra = 1;
		break;
	case SCENE_FILTER_OCCLUDER:
		has_filter = true;
		has_occluder = true;
		num_extra = 2;
		break;
	case SCENE_EMITTER:
		has_filter = true;
		has_emitter = true;
		num_extra = 2;
		break;
	case SCENE_RECEIVER:
		break;
	}

	const size_t object_count = 1 + num_extra;
	test_textures[0] = texture_uniform_new((v3){ .5f, .25f, 1.f });
	test_textures[1] = texture_uniform_new(zero);
	material_init(&test_materials[0], 1, zero, zero, zero, zero, zero, 2.f, 1.f, test_textures[0]);
	material_init(&test_materials[1], 2, zero, zero, zero, zero, zero, 2.f, 1.f, test_textures[1]);
	material_init(&test_materials[2], 3, zero, zero, zero, (v3){ .5f, .25f, .75f }, zero, 2.f, 1.f, test_textures[1]);
	material_init(&test_materials[3], 4, zero, zero, zero, zero, (v3){ 8.f, 4.f, 2.f }, 2.f, 1.f, test_textures[1]);

	num_objects = object_count;
	num_emittant_objects = has_emitter ? 1 : 0;
	num_unbound_objects = 0;
	objects_init();

	size_t slot = 0;
	objects[slot++] = make_sphere((v3){ 0.f, 0.f, 0.f }, 1.f, &test_materials[0]);
	if (has_filter)
		objects[slot++] = make_sphere((v3){ 0.f, 0.f, 3.f }, .25f, &test_materials[2]);
	if (has_occluder)
		objects[slot++] = make_sphere((v3){ 0.f, 0.f, 2.5f }, .4f, &test_materials[1]);
	if (has_emitter) {
		struct Object *emitter = make_sphere((v3){ 0.f, 0.f, 5.f }, .1f, &test_materials[3]);
		emitter_vtable = *emitter->object_data;
		emitter_vtable.get_light_point = fixed_light_point;
		emitter->object_data = &emitter_vtable;
		objects[slot++] = emitter;
		emittant_objects[0] = emitter;
	}
	accel_init();
}

static void scene_teardown(void)
{
	accel_deinit();
	objects_deinit();
	for (size_t i = 0; i < sizeof(test_textures) / sizeof(test_textures[0]); i++) {
		free(test_textures[i]);
		test_textures[i] = NULL;
	}
}

/* Independent per-light reference: iterates the caller's lights one by one and
 * drives the same production shadow path as the tree. */
struct Reference {
	double value[LT_TEST_CHANNELS];
	size_t leaf_evals;
	size_t shadow_queries;
};

static void reference_eval(const struct PointLight *lights, const size_t count, const v3 point, const v3 normal, const v3 diffuse, const double offset, struct Reference *ref)
{
	memset(ref, 0, sizeof(struct Reference));
	for (size_t i = 0; i < count; i++) {
		ref->leaf_evals++;
		double delta[LT_TEST_CHANNELS];
		double dist2 = 0.;
		for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
			delta[c] = (double)lights[i].position[c] - (double)point[c];
			dist2 += delta[c] * delta[c];
		}
		if (dist2 == 0.)
			continue;

		const double dist = sqrt(dist2);
		double cosine = 0.;
		for (size_t c = 0; c < LT_TEST_CHANNELS; c++)
			cosine += (double)normal[c] * delta[c] / dist;
		if (cosine <= 0.)
			continue;

		struct Ray ray;
		for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
			ray.point[c] = point[c];
			ray.direction[c] = (float)(delta[c] / dist);
		}
		norm3(ray.direction);

		v3 transmission = { 1.f, 1.f, 1.f };
		const bool blocked = is_light_blocked(&ray, (float)dist, transmission, NULL);
		ref->shadow_queries++;
		if (blocked)
			continue;

		const double geometry = cosine / (offset + dist2);
		for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
			double t = (double)transmission[c];
			if (t < 0.)
				t = 0.;
			else if (t > 1.)
				t = 1.;
			ref->value[c] += (double)diffuse[c] * (double)lights[i].intensity[c] * geometry * t;
		}
	}
}

/*
 * Degraded variant of the transport: it still walks the real shadow path but
 * discards the colored transmission returned through the intensity argument,
 * treating every non-opaque segment as perfectly clear.
 */
static double degraded_channel_eval(const struct PointLight *lights, const size_t count, const v3 point, const v3 normal, const v3 diffuse, const double offset, const size_t channel)
{
	double sum = 0.;
	for (size_t i = 0; i < count; i++) {
		double delta[LT_TEST_CHANNELS];
		double dist2 = 0.;
		for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
			delta[c] = (double)lights[i].position[c] - (double)point[c];
			dist2 += delta[c] * delta[c];
		}
		if (dist2 == 0.)
			continue;
		const double dist = sqrt(dist2);
		double cosine = 0.;
		for (size_t c = 0; c < LT_TEST_CHANNELS; c++)
			cosine += (double)normal[c] * delta[c] / dist;
		if (cosine <= 0.)
			continue;

		struct Ray ray;
		for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
			ray.point[c] = point[c];
			ray.direction[c] = (float)(delta[c] / dist);
		}
		norm3(ray.direction);
		v3 transmission = { 1.f, 1.f, 1.f };
		if (is_light_blocked(&ray, (float)dist, transmission, NULL))
			continue; /* opaque: still zero */
		/* BUG UNDER TEST: colored transmission is dropped on purpose. */
		sum += (double)diffuse[channel] * (double)lights[i].intensity[channel] * cosine / (offset + dist2);
	}
	return sum;
}

static int check_interval_contains(const struct LightResult *result, const struct Reference *ref, const double abs_tol, const double rel_tol)
{
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
		const double exact = ref->value[c];
		const double rounding = 2e-6 * fmax(1., exact);
		TASK_CHECK(result->lower[c] >= -TEST_EPS);
		TASK_CHECK(result->upper[c] >= result->lower[c] - TEST_EPS);
		/* The interval must enclose the exact sum. */
		TASK_CHECK(result->lower[c] <= exact + rounding);
		TASK_CHECK(result->upper[c] >= exact - rounding);
		/* The estimate sits inside the interval. */
		TASK_CHECK(result->color[c] >= result->lower[c] - TEST_EPS);
		TASK_CHECK(result->color[c] <= result->upper[c] + TEST_EPS);
		/* Global (not per-node) error budget on both ends. */
		const double budget = abs_tol + rel_tol * result->lower[c] + rounding;
		TASK_CHECK(result->color[c] - result->lower[c] <= budget + TEST_EPS);
		TASK_CHECK(result->upper[c] - result->color[c] <= budget + TEST_EPS);
	}
	return 0;
}

static void make_basic_lights(struct PointLight *lights, const size_t count)
{
	for (size_t i = 0; i < count; i++) {
		const float angle = (float)(i + 1) * 0.7f;
		const float radius = 3.f + (float)(i % 5);
		lights[i].position[0] = radius * cosf(angle);
		lights[i].position[1] = radius * sinf(angle) * .5f;
		lights[i].position[2] = 4.f + (float)(i % 3);
		lights[i].intensity[0] = 1.f + (float)(i % 4);
		lights[i].intensity[1] = .5f + (float)(i % 7) * .1f;
		lights[i].intensity[2] = 2.f - (float)(i % 3) * .25f;
	}
}

static int bounds_empty(void)
{
	struct LightResult result;
	LightTree *tree = lighttree_build(NULL, 0);
	TASK_CHECK(tree != NULL);
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	lighttree_eval(tree, point, normal, diffuse, 1., 1., 1., &result);
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
		TASK_CHECK(result.color[c] == 0.);
		TASK_CHECK(result.lower[c] == 0.);
		TASK_CHECK(result.upper[c] == 0.);
	}
	TASK_CHECK(result.bound_evals == 0);
	TASK_CHECK(result.leaf_evals == 0);
	TASK_CHECK(result.shadow_queries == 0);
	lighttree_free(tree);
	lighttree_free(NULL); /* must be a safe no-op */
	return 0;
}

static int bounds_zero_tolerance(void)
{
	scene_setup(SCENE_RECEIVER);
	enum { N = 11 };
	struct PointLight lights[N];
	make_basic_lights(lights, N);
	LightTree *tree = lighttree_build(lights, N);
	struct Reference ref;
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	reference_eval(lights, N, point, normal, diffuse, 1., &ref);

	struct LightResult result;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &result);
	if (check_interval_contains(&result, &ref, 0., 0.))
		return 1;
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
		/* Zero tolerance collapses the interval onto the exact leaves. */
		TASK_CHECK(near_value(result.lower[c], ref.value[c], TEST_EPS));
		TASK_CHECK(near_value(result.upper[c], ref.value[c], TEST_EPS));
		TASK_CHECK(near_value(result.color[c], ref.value[c], TEST_EPS));
	}
	TASK_CHECK(result.leaf_evals == N);
	TASK_CHECK(result.shadow_queries == ref.shadow_queries);
	TASK_CHECK(result.bound_evals == 2 * N - 1); /* root plus every internal split */
	lighttree_free(tree);
	scene_teardown();
	return 0;
}

static int bounds_duplicate_positions(void)
{
	scene_setup(SCENE_RECEIVER);
	struct PointLight lights[9];
	for (size_t i = 0; i < 5; i++) {
		assign3(lights[i].position, (v3){ 2.f, 0.f, 3.f });
		lights[i].intensity[0] = (float)(i + 1);
		lights[i].intensity[1] = 1.f;
		lights[i].intensity[2] = .5f;
	}
	for (size_t i = 5; i < 9; i++) {
		assign3(lights[i].position, (v3){ -2.f, 1.f, 3.f });
		lights[i].intensity[0] = 2.f;
		lights[i].intensity[1] = (float)(i - 3);
		lights[i].intensity[2] = .25f;
	}
	LightTree *tree = lighttree_build(lights, 9);
	struct Reference ref;
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	reference_eval(lights, 9, point, normal, diffuse, 1., &ref);

	struct LightResult exact, approximate;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &exact);
	lighttree_eval(tree, point, normal, diffuse, 1., 1e-4, 0., &approximate);
	if (check_interval_contains(&exact, &ref, 0., 0.))
		return 1;
	if (check_interval_contains(&approximate, &ref, 1e-4, 0.))
		return 1;
	TASK_CHECK(near_value(exact.color[0], ref.value[0], TEST_EPS));
	TASK_CHECK(approximate.leaf_evals <= 9);
	lighttree_free(tree);
	scene_teardown();
	return 0;
}

static int bounds_rgb_dynamic_range(void)
{
	scene_setup(SCENE_RECEIVER);
	enum { N = 8 };
	struct PointLight lights[N];
	for (size_t i = 0; i < N; i++) {
		lights[i].position[0] = (float)(i - 3) * 1.5f;
		lights[i].position[1] = 0.f;
		lights[i].position[2] = 6.f;
		lights[i].intensity[0] = 2e6f; /* hot red */
		lights[i].intensity[1] = 1.f; /* mid green */
		lights[i].intensity[2] = 1e-6f; /* vanishing blue */
	}
	LightTree *tree = lighttree_build(lights, N);
	struct Reference ref;
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	reference_eval(lights, N, point, normal, diffuse, 1., &ref);

	struct LightResult result;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &result);
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
		TASK_CHECK(isfinite(result.color[c]));
		TASK_CHECK(near_value(result.color[c], ref.value[c], TEST_EPS));
		TASK_CHECK(near_value(result.lower[c], ref.value[c], TEST_EPS));
		TASK_CHECK(near_value(result.upper[c], ref.value[c], TEST_EPS));
	}
	/* Channels differ by twelve orders of magnitude and must all survive. */
	TASK_CHECK(result.color[0] > 1e5);
	TASK_CHECK(result.color[2] > 0. && result.color[2] < 1e-5);
	lighttree_free(tree);
	scene_teardown();
	return 0;
}

static int bounds_many_tiny_terms(void)
{
	scene_setup(SCENE_RECEIVER);
	enum { N = 1000 };
	struct PointLight *lights = malloc(sizeof(struct PointLight) * N);
	for (size_t i = 0; i < N; i++) {
		lights[i].position[0] = (float)((i % 25) - 12);
		lights[i].position[1] = (float)((i / 25) - 20);
		lights[i].position[2] = 30.f;
		lights[i].intensity[0] = 1e-9f;
		lights[i].intensity[1] = 1e-10f;
		lights[i].intensity[2] = 1e-11f;
	}
	LightTree *tree = lighttree_build(lights, N);
	struct Reference ref;
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	reference_eval(lights, N, point, normal, diffuse, 1., &ref);

	struct LightResult result;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &result);
	if (check_interval_contains(&result, &ref, 0., 0.))
		return 1;
	TASK_CHECK(result.leaf_evals == N);
	TASK_CHECK(near_value(result.color[0], ref.value[0], 1e-12));
	lighttree_free(tree);
	free(lights);
	scene_teardown();
	return 0;
}

static int bounds_interval_tolerance(void)
{
	scene_setup(SCENE_RECEIVER);
	enum { N = 64 };
	struct PointLight lights[N];
	make_basic_lights(lights, N);
	LightTree *tree = lighttree_build(lights, N);
	struct Reference ref;
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	reference_eval(lights, N, point, normal, diffuse, 1., &ref);

	/* A loose absolute budget lets whole distant clusters be accepted. */
	const double loose_tol = 0.1;
	struct LightResult loose;
	lighttree_eval(tree, point, normal, diffuse, 1., loose_tol, 0., &loose);
	if (check_interval_contains(&loose, &ref, loose_tol, 0.))
		return 1;
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
		const double budget = loose_tol + 2e-6 * fmax(1., ref.value[c]);
		TASK_CHECK(loose.upper[c] - loose.lower[c] <= budget + TEST_EPS);
	}
	TASK_CHECK(loose.leaf_evals < N);
	TASK_CHECK(loose.shadow_queries <= loose.leaf_evals);

	/* A budget below every individual contribution must expand all leaves. */
	const double tight_tol = 1e-4;
	struct LightResult tight;
	lighttree_eval(tree, point, normal, diffuse, 1., tight_tol, 0., &tight);
	if (check_interval_contains(&tight, &ref, tight_tol, 0.))
		return 1;
	TASK_CHECK(tight.leaf_evals == N);
	TASK_CHECK(tight.shadow_queries == ref.shadow_queries);

	/* The global budget is what steers the work: tighter means more leaves. */
	TASK_CHECK(tight.leaf_evals > loose.leaf_evals);
	TASK_CHECK(tight.bound_evals >= loose.bound_evals);
	lighttree_free(tree);
	scene_teardown();
	return 0;
}

static int bounds_determinism(void)
{
	scene_setup(SCENE_RECEIVER);
	enum { N = 127 };
	struct PointLight lights[N];
	make_basic_lights(lights, N);
	LightTree *a = lighttree_build(lights, N);
	LightTree *b = lighttree_build(lights, N);
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };

	struct LightResult first, second, rebuilt;
	lighttree_eval(a, point, normal, diffuse, 1., 1e-4, 1e-6, &first);
	lighttree_eval(a, point, normal, diffuse, 1., 1e-4, 1e-6, &second);
	lighttree_eval(b, point, normal, diffuse, 1., 1e-4, 1e-6, &rebuilt);
	TASK_CHECK(memcmp(&first, &second, sizeof(struct LightResult)) == 0);
	TASK_CHECK(memcmp(&first, &rebuilt, sizeof(struct LightResult)) == 0);

	/* The tree snapshots its input: mutating the caller's array is harmless. */
	struct PointLight saved = lights[3];
	lights[3].intensity[0] += 1000.f;
	struct LightResult after_mutation;
	lighttree_eval(a, point, normal, diffuse, 1., 1e-4, 1e-6, &after_mutation);
	TASK_CHECK(memcmp(&first, &after_mutation, sizeof(struct LightResult)) == 0);
	lights[3] = saved;
	lighttree_free(a);
	lighttree_free(b);
	scene_teardown();
	return 0;
}

static int bounds_back_and_coincident(void)
{
	scene_setup(SCENE_RECEIVER);
	struct PointLight lights[4];
	assign3(lights[0].position, (v3){ 0.f, 0.f, 1.f }); /* coincident: zero */
	assign3(lights[0].intensity, (v3){ 10.f, 10.f, 10.f });
	assign3(lights[1].position, (v3){ 0.f, 0.f, -1.f }); /* behind the surface */
	assign3(lights[1].intensity, (v3){ 10.f, 10.f, 10.f });
	assign3(lights[2].position, (v3){ 2.f, 0.f, 3.f }); /* valid, faces the normal */
	assign3(lights[2].intensity, (v3){ 4.f, 4.f, 4.f });
	assign3(lights[3].position, (v3){ 0.f, 3.f, 1.f }); /* edge-on, cosine 0 */
	assign3(lights[3].intensity, (v3){ 4.f, 4.f, 4.f });

	LightTree *tree = lighttree_build(lights, 4);
	struct Reference ref;
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	reference_eval(lights, 4, point, normal, diffuse, 1., &ref);
	TASK_CHECK(ref.shadow_queries == 1);
	TASK_CHECK(ref.value[0] > 0. && ref.value[1] > 0.);

	struct LightResult result;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &result);
	if (check_interval_contains(&result, &ref, 0., 0.))
		return 1;
	TASK_CHECK(result.leaf_evals == 4);
	TASK_CHECK(result.shadow_queries == 1); /* coincident/back/edge-on cost nothing */
	lighttree_free(tree);
	scene_teardown();
	return 0;
}

static int bounds_work_budget(void)
{
	scene_setup(SCENE_RECEIVER);
	enum { DIM = 64, DISTANT = DIM *DIM, TOTAL = DISTANT + 1 };
	struct PointLight *lights = malloc(sizeof(struct PointLight) * TOTAL);
	for (size_t i = 0; i < DISTANT; i++) {
		const size_t ix = i % DIM;
		const size_t iy = i / DIM;
		lights[i].position[0] = ((float)ix - 31.5f) * .15f;
		lights[i].position[1] = ((float)iy - 31.5f) * .15f;
		lights[i].position[2] = 40.f;
		/* Genuinely dim: the whole distant field adds less than the
		 * absolute budget, so the hierarchy accepts it as clusters. */
		lights[i].intensity[0] = 2e-4f;
		lights[i].intensity[1] = 1e-4f;
		lights[i].intensity[2] = 3e-4f;
	}
	/* A spatially separate, much closer and orders-of-magnitude brighter
	 * light that cannot share a cluster bound with the distant field. */
	assign3(lights[DISTANT].position, (v3){ 12.f, 0.f, 1.5f });
	assign3(lights[DISTANT].intensity, (v3){ 120.f, 60.f, 30.f });

	LightTree *tree = lighttree_build(lights, TOTAL);
	struct Reference ref;
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	reference_eval(lights, TOTAL, point, normal, diffuse, 1., &ref);

	const double abs_tol = 1e-3; /* meaningful non-zero absolute budget */
	struct LightResult result;
	lighttree_eval(tree, point, normal, diffuse, 1., abs_tol, 0., &result);
	if (check_interval_contains(&result, &ref, abs_tol, 0.))
		return 1;
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++)
		TASK_CHECK(result.upper[c] - result.lower[c] <= abs_tol + 2e-6 * fmax(1., ref.value[c]) + TEST_EPS);

	/* The honest counters must show the hierarchy did the work, not a scan:
	 * the distant field is resolved through node bounds alone. */
	TASK_CHECK(result.leaf_evals < TOTAL / 8);
	TASK_CHECK(result.shadow_queries < TOTAL / 8);
	TASK_CHECK(result.bound_evals < TOTAL / 2);
	TASK_CHECK(result.bound_evals > 0);
	TASK_CHECK(result.shadow_queries <= result.leaf_evals);
	/* Zero tolerance on the same tree still reaches every exact leaf. */
	struct LightResult exact;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &exact);
	TASK_CHECK(exact.leaf_evals == TOTAL);
	if (check_interval_contains(&exact, &ref, 0., 0.))
		return 1;
	lighttree_free(tree);
	free(lights);
	scene_teardown();
	return 0;
}

static int visibility_partial_occlusion(void)
{
	scene_setup(SCENE_OCCLUDER);
	struct PointLight lights[6];
	for (size_t i = 0; i < 3; i++) {
		/* Thread the needle beside the opaque sphere at (0,0,2.5). */
		assign3(lights[i].position, (v3){ 1.5f + (float)i, 0.f, 4.f });
		assign3(lights[i].intensity, (v3){ 3.f, 3.f, 3.f });
	}
	for (size_t i = 3; i < 6; i++) {
		/* These directions run through the occluder. */
		assign3(lights[i].position, (v3){ (float)(i - 4) * .1f, 0.f, 4.f });
		assign3(lights[i].intensity, (v3){ 3.f, 3.f, 3.f });
	}
	LightTree *tree = lighttree_build(lights, 6);
	struct Reference ref;
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	reference_eval(lights, 6, point, normal, diffuse, 1., &ref);
	TASK_CHECK(ref.shadow_queries == 6);
	TASK_CHECK(ref.value[0] > 0.); /* some lights survive */

	struct LightResult exact, bounded;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &exact);
	lighttree_eval(tree, point, normal, diffuse, 1., 1e-4, 0., &bounded);
	if (check_interval_contains(&exact, &ref, 0., 0.))
		return 1;
	if (check_interval_contains(&bounded, &ref, 1e-4, 0.))
		return 1;
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++)
		TASK_CHECK(near_value(exact.color[c], ref.value[c], TEST_EPS));
	lighttree_free(tree);
	scene_teardown();
	return 0;
}

static int visibility_colored_transmission(void)
{
	scene_setup(SCENE_FILTER);
	struct PointLight lights[2];
	assign3(lights[0].position, (v3){ 0.f, 0.f, 5.f }); /* through the filter */
	assign3(lights[0].intensity, (v3){ 8.f, 8.f, 8.f });
	assign3(lights[1].position, (v3){ 5.f, 0.f, 3.f }); /* clear path */
	assign3(lights[1].intensity, (v3){ 8.f, 8.f, 8.f });

	LightTree *tree = lighttree_build(lights, 2);
	struct Reference ref;
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	reference_eval(lights, 2, point, normal, diffuse, 1., &ref);

	struct LightResult result;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &result);
	if (check_interval_contains(&result, &ref, 0., 0.))
		return 1;
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++)
		TASK_CHECK(near_value(result.color[c], ref.value[c], TEST_EPS));

	/* The filtered light carries the filter's per-channel kt, so its three
	 * channels cannot collapse to a scalar. */
	TASK_CHECK(near_value(result.color[1], ref.value[1], TEST_EPS));
	TASK_CHECK(fabs(result.color[0] - result.color[1]) > 1e-9);
	lighttree_free(tree);
	scene_teardown();
	return 0;
}

static int visibility_degraded_detected(void)
{
	scene_setup(SCENE_FILTER);
	struct PointLight light;
	assign3(light.position, (v3){ 0.f, 0.f, 5.f });
	assign3(light.intensity, (v3){ 8.f, 8.f, 8.f });
	LightTree *tree = lighttree_build(&light, 1);
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };

	struct LightResult result;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &result);

	/*
	 * Run the degraded build (colored transmission dropped) and apply the
	 * same zero-tolerance per-channel content assertion used everywhere
	 * above: color[c] must equal the exact reference within the rounding
	 * margin.  The filter kt is (.5, .25, .75), so every channel is wrong
	 * and the assertion fires on all three; green is the most divergent
	 * (.25 transmission lost).
	 */
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
		const double degraded = degraded_channel_eval(&light, 1, point, normal, diffuse, 1., c);
		TASK_CHECK(!near_value(degraded, result.color[c], TEST_EPS));
		TASK_CHECK(degraded > result.color[c]); /* dropping kt <= 1 can only inflate */
	}
	lighttree_free(tree);
	scene_teardown();
	return 0;
}

static int transport_texture_attenuation(void)
{
	scene_setup(SCENE_RECEIVER);
	enum { N = 13 };
	struct PointLight lights[N];
	make_basic_lights(lights, N);
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f }; /* matches the receiver texture */

	struct Reference ref;
	reference_eval(lights, N, point, normal, diffuse, 1., &ref);
	render_set_point_lights(lights, N, 1., 0., 0.);

	struct Ray view = { { 0.f, 0.f, -1.f }, { 0.f, 0.f, 2.f } };
	v3 color = { 0.f, 0.f, 0.f };
	const float depth = cast_ray(&view, (v3){ 1.f, 1.f, 1.f }, color, 1, NULL);
	TASK_CHECK(near_value((double)depth, 1., TEST_EPS));
	/* Legacy square camera attenuation: 1 / (1 + distance)^2 = 1/4. */
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++)
		TASK_CHECK(near_value((double)color[c], ref.value[c] * .25, 1e-9));
	render_clear_point_lights();
	scene_teardown();
	return 0;
}

static int transport_replacement_and_clear(void)
{
	scene_setup(SCENE_RECEIVER);
	struct PointLight a[3], b[2];
	make_basic_lights(a, 3);
	assign3(b[0].position, (v3){ 4.f, 0.f, 3.f });
	assign3(b[0].intensity, (v3){ 20.f, 20.f, 20.f });
	assign3(b[1].position, (v3){ -4.f, 0.f, 3.f });
	assign3(b[1].intensity, (v3){ 20.f, 20.f, 20.f });

	struct Ray view = { { 0.f, 0.f, -1.f }, { 0.f, 0.f, 2.f } };
	v3 color_a = { 0.f, 0.f, 0.f };
	v3 color_b = { 0.f, 0.f, 0.f };
	v3 color_a_again = { 0.f, 0.f, 0.f };

	render_set_point_lights(a, 3, 1., 0., 0.);
	cast_ray(&view, (v3){ 1.f, 1.f, 1.f }, color_a, 1, NULL);
	render_set_point_lights(b, 2, 1., 0., 0.); /* replaces, never accumulates */
	cast_ray(&view, (v3){ 1.f, 1.f, 1.f }, color_b, 1, NULL);
	render_set_point_lights(a, 3, 1., 0., 0.);
	cast_ray(&view, (v3){ 1.f, 1.f, 1.f }, color_a_again, 1, NULL);
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++)
		TASK_CHECK(near_value((double)color_a[c], (double)color_a_again[c], TEST_EPS));
	TASK_CHECK(fabs((double)color_b[0] - (double)color_a[0]) > 1e-4);

	/* Clearing restores the all-zero legacy result (no emitters/ambient). */
	render_clear_point_lights();
	v3 color_clear = { 0.f, 0.f, 0.f };
	cast_ray(&view, (v3){ 1.f, 1.f, 1.f }, color_clear, 1, NULL);
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++)
		TASK_CHECK(color_clear[c] == 0.f);
	scene_teardown();
	return 0;
}

static int transport_legacy_emitter_additivity(void)
{
	scene_setup(SCENE_EMITTER);
	struct PointLight light;
	assign3(light.position, (v3){ 5.f, 0.f, 3.f }); /* clear of the filter sphere */
	assign3(light.intensity, (v3){ 12.f, 6.f, 3.f });
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };
	struct Reference ref;
	reference_eval(&light, 1, point, normal, diffuse, 1., &ref);

	struct Ray view = { { 0.f, 0.f, -1.f }, { 0.f, 0.f, 2.f } };

	v3 legacy_only = { 0.f, 0.f, 0.f };
	cast_ray(&view, (v3){ 1.f, 1.f, 1.f }, legacy_only, 1, NULL);
	TASK_CHECK(legacy_only[0] > 0.f); /* Phong/Blinn emitter channel alive */

	v3 both = { 0.f, 0.f, 0.f };
	render_set_point_lights(&light, 1, 1., 0., 0.);
	cast_ray(&view, (v3){ 1.f, 1.f, 1.f }, both, 1, NULL);
	render_clear_point_lights();
	for (size_t c = 0; c < LT_TEST_CHANNELS; c++) {
		const double expected = (double)legacy_only[c] + ref.value[c] * .25;
		TASK_CHECK(near_value((double)both[c], expected, 1e-9));
	}
	scene_teardown();
	return 0;
}

static int transport_random_stream_untouched(void)
{
	scene_setup(SCENE_RECEIVER);
	enum { N = 31 };
	struct PointLight lights[N];
	make_basic_lights(lights, N);
	struct Ray view = { { 0.f, 0.f, -1.f }, { 0.f, 0.f, 2.f } };

	srand(1675);
	const int expected = rand();
	srand(1675);
	render_set_point_lights(lights, N, 1., 1e-4, 1e-6);
	v3 color = { 0.f, 0.f, 0.f };
	cast_ray(&view, (v3){ 1.f, 1.f, 1.f }, color, 1, NULL);
	TASK_CHECK(rand() == expected);
	render_clear_point_lights();

	srand(1675);
	v3 cleared = { 0.f, 0.f, 0.f };
	cast_ray(&view, (v3){ 1.f, 1.f, 1.f }, cleared, 1, NULL);
	TASK_CHECK(rand() == expected);
	scene_teardown();
	return 0;
}

static int transport_point_offset_independent(void)
{
	scene_setup(SCENE_RECEIVER);
	struct PointLight light;
	assign3(light.position, (v3){ 0.f, 0.f, 4.f });
	assign3(light.intensity, (v3){ 9.f, 9.f, 9.f });
	LightTree *tree = lighttree_build(&light, 1);
	v3 point = { 0.f, 0.f, 1.f };
	v3 normal = { 0.f, 0.f, 1.f };
	v3 diffuse = { .5f, .25f, 1.f };

	struct LightResult near_offset, far_offset;
	lighttree_eval(tree, point, normal, diffuse, 1., 0., 0., &near_offset);
	lighttree_eval(tree, point, normal, diffuse, 1000., 0., 0., &far_offset);
	TASK_CHECK(near_offset.color[0] > far_offset.color[0]);

	/* Exact denominator check: d^2 = 9, so offset 1 divides by 10. */
	struct Reference ref;
	reference_eval(&light, 1, point, normal, diffuse, 1., &ref);
	TASK_CHECK(near_value(near_offset.color[0], ref.value[0], TEST_EPS));
	lighttree_free(tree);
	scene_teardown();
	return 0;
}

const struct task_case task_cases[] = {
	{ "bounds", "empty_tree", bounds_empty },
	{ "bounds", "zero_tolerance_exact", bounds_zero_tolerance },
	{ "bounds", "duplicate_positions", bounds_duplicate_positions },
	{ "bounds", "rgb_dynamic_range", bounds_rgb_dynamic_range },
	{ "bounds", "many_tiny_terms", bounds_many_tiny_terms },
	{ "bounds", "interval_tolerance", bounds_interval_tolerance },
	{ "bounds", "determinism", bounds_determinism },
	{ "bounds", "back_and_coincident", bounds_back_and_coincident },
	{ "bounds", "work_budget_4096", bounds_work_budget },
	{ "visibility", "partial_occlusion", visibility_partial_occlusion },
	{ "visibility", "colored_transmission", visibility_colored_transmission },
	{ "visibility", "degraded_transmission_detected", visibility_degraded_detected },
	{ "transport", "texture_and_camera_attenuation", transport_texture_attenuation },
	{ "transport", "set_replaces_and_clear", transport_replacement_and_clear },
	{ "transport", "legacy_emitter_additivity", transport_legacy_emitter_additivity },
	{ "transport", "random_stream_untouched", transport_random_stream_untouched },
	{ "transport", "point_offset_independent", transport_point_offset_independent },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
