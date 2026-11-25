/*
 * lighttree_demo.c - Small headless scene and deterministic LightTree query.
 *
 * Build and run from the repository root:
 *   bash task_support/run.sh target
 *   cc -std=c11 -O2 -DUNBOUND_OBJECTS -I. -Isrc/core -Isrc/raytracer \
 *      -Ilib/SimplexNoise -Ilib/cJSON examples/lighttree_demo.c \
 *      .task-build/libscene.a -lm -o .task-build/lighttree_demo
 *   .task-build/lighttree_demo
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "accel.h"
#include "lighttree.h"
#include "material.h"
#include "object.h"

static struct Object *make_sphere(const v3 position, const float radius, struct Material *material)
{
	struct Object *object = sphere_new(position, radius);
	object_init(object, material, 0.0001f, 1u, OBJECT_SPHERE);
	object->object_data->postinit(object);
	return object;
}

int main(void)
{
	enum { LIGHT_COUNT = 4096 };
	struct Material receiver_material;
	struct Material filter_material;
	struct Texture *receiver_texture;
	struct Texture *black_texture;
	PointLight *lights;
	LightTree *tree;
	LightResult result;
	v3 zero = { 0.f, 0.f, 0.f };
	v3 white = { 1.f, 1.f, 1.f };
	v3 filter_kt = { 0.5f, 0.25f, 0.75f };
	const v3 point = { 0.f, 0.f, 1.f };
	const v3 normal = { 0.f, 0.f, 1.f };
	size_t i;

	receiver_texture = texture_uniform_new(white);
	black_texture = texture_uniform_new(zero);
	material_init(&receiver_material, 1, zero, zero, zero, zero, zero, 2.f, 1.f, receiver_texture);
	material_init(&filter_material, 2, zero, zero, zero, filter_kt, zero, 2.f, 1.f, black_texture);

	num_objects = 2;
	num_emittant_objects = 0;
	num_unbound_objects = 0;
	objects_init();
	objects[0] = make_sphere((v3){ 0.f, 0.f, 0.f }, 1.f, &receiver_material);
	objects[1] = make_sphere((v3){ 0.f, 0.f, 3.f }, 0.25f, &filter_material);
	accel_init();

	lights = malloc((size_t)LIGHT_COUNT * sizeof(*lights));
	if (!lights) {
		fprintf(stderr, "allocation failed\n");
		return 1;
	}

	/* Many weak, spatially separated distant lights. */
	for (i = 0; i < (size_t)LIGHT_COUNT; ++i) {
		size_t ix = i % 64u;
		size_t iy = i / 64u;
		lights[i] = (PointLight){
			.position = {
				((float)ix - 31.5f) * 0.6f,
				((float)iy - 31.5f) * 0.6f,
				20.f,
			},
			.intensity = { 0.01f, 0.01f, 0.01f },
		};
	}

	/* One nearby bright light seen through the colored transparent sphere. */
	lights[0] = (PointLight){
		.position = { 0.f, 0.f, 5.f },
		.intensity = { 8.f, 4.f, 2.f },
	};

	tree = lighttree_build(lights, (size_t)LIGHT_COUNT);
	if (!tree) {
		fprintf(stderr, "lighttree_build failed\n");
		return 1;
	}

	lighttree_eval(tree, point, normal, white, 1.0, 0.1, 0.0, &result);

	printf("LightTree demo: %d lights, offset=1, abs_tol=0.1\n", LIGHT_COUNT);
	printf("color = [%.9f, %.9f, %.9f]\n", result.color[0], result.color[1], result.color[2]);
	printf("lower = [%.9f, %.9f, %.9f]\n", result.lower[0], result.lower[1], result.lower[2]);
	printf("upper = [%.9f, %.9f, %.9f]\n", result.upper[0], result.upper[1], result.upper[2]);
	printf("work  = bound_evals=%zu leaf_evals=%zu shadow_queries=%zu\n",
		result.bound_evals, result.leaf_evals, result.shadow_queries);
	printf("The axial bright light is filtered by kt=(0.5,0.25,0.75).\n");

	lighttree_free(tree);
	free(lights);
	accel_deinit();
	objects_deinit();
	free(receiver_texture);
	free(black_texture);
	return 0;
}
