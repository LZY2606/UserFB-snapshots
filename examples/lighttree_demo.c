/*
 * Offline demonstration of the deterministic hierarchical point-light channel.
 *
 * Builds a small physical scene (a receiver sphere and a colored transparent
 * filter), installs a few hundred point lights in a read-only LightTree and
 * runs the same shading query twice: once with a loose absolute error budget
 * and once at zero tolerance.  It prints the per-channel interval, the midpoint
 * estimate and the truthful work counters, showing how the checked error bound
 * controls how many leaves and shadow rays are actually evaluated.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "accel.h"
#include "calc.h"
#include "lighttree.h"
#include "material.h"
#include "object.h"

#define DEMO_DIM 16
#define DEMO_DISTANT (DEMO_DIM * DEMO_DIM)
#define DEMO_TOTAL (DEMO_DISTANT + 2)

static struct Object *make_sphere(const v3 position, const float radius, struct Material *material)
{
	struct Object *object = sphere_new(position, radius);
	object_init(object, material, 0.0001f, 1, OBJECT_SPHERE);
	object->object_data->postinit(object);
	return object;
}

static void print_result(const char *label, const struct LightResult *result)
{
	static const char channel_name[3] = { 'R', 'G', 'B' };
	printf("%s\n", label);
	for (size_t c = 0; c < 3; c++) {
		printf("  %c color=%.8g  interval=[%.8g, %.8g]  width=%.3g\n", channel_name[c], result->color[c], result->lower[c], result->upper[c], result->upper[c] - result->lower[c]);
	}
	printf("  work: bound_evals=%zu leaf_evals=%zu shadow_queries=%zu (of %u lights)\n", result->bound_evals, result->leaf_evals, result->shadow_queries, (unsigned)DEMO_TOTAL);
}

int main(void)
{
	/* Physical scene: a receiver sphere and a colored transparent filter. */
	const v3 zero = { 0.f, 0.f, 0.f };
	struct Texture *receiver_texture = texture_uniform_new((v3){ .8f, .8f, .8f });
	struct Texture *black_texture = texture_uniform_new(zero);
	struct Material receiver, filter;
	material_init(&receiver, 1, zero, zero, zero, zero, zero, 2.f, 1.f, receiver_texture);
	material_init(&filter, 2, zero, zero, zero, (v3){ .5f, .25f, .75f }, zero, 2.f, 1.f, black_texture);

	num_objects = 2;
	num_emittant_objects = 0;
	num_unbound_objects = 0;
	objects_init();
	objects[0] = make_sphere((v3){ 0.f, 0.f, 0.f }, 1.f, &receiver);
	objects[1] = make_sphere((v3){ 0.f, 0.f, 3.f }, .25f, &filter);
	accel_init();

	/* A regular field of dim distant lights plus two bright nearby ones. */
	struct PointLight *lights = malloc(sizeof(struct PointLight) * DEMO_TOTAL);
	for (size_t i = 0; i < DEMO_DISTANT; i++) {
		const size_t ix = i % DEMO_DIM;
		const size_t iy = i / DEMO_DIM;
		lights[i].position[0] = ((float)ix - (DEMO_DIM - 1) * .5f) * .4f;
		lights[i].position[1] = ((float)iy - (DEMO_DIM - 1) * .5f) * .4f;
		lights[i].position[2] = 30.f;
		lights[i].intensity[0] = 2e-4f;
		lights[i].intensity[1] = 1e-4f;
		lights[i].intensity[2] = 3e-4f;
	}
	/* This ray passes through the colored filter: transmission (.5,.25,.75). */
	assign3(lights[DEMO_DISTANT].position, (v3){ 0.f, 0.f, 5.f });
	assign3(lights[DEMO_DISTANT].intensity, (v3){ 40.f, 40.f, 40.f });
	/* This one has a clear line of sight. */
	assign3(lights[DEMO_DISTANT + 1].position, (v3){ 6.f, 0.f, 3.f });
	assign3(lights[DEMO_DISTANT + 1].intensity, (v3){ 20.f, 10.f, 5.f });

	LightTree *tree = lighttree_build(lights, DEMO_TOTAL);
	const v3 point = { 0.f, 0.f, 1.f };
	const v3 normal = { 0.f, 0.f, 1.f };
	const v3 diffuse = { .8f, .8f, .8f };
	const double offset = 1.;

	struct LightResult approximate, exact, repeated;
	lighttree_eval(tree, point, normal, diffuse, offset, 1e-3, 0., &approximate);
	lighttree_eval(tree, point, normal, diffuse, offset, 0., 0., &exact);
	lighttree_eval(tree, point, normal, diffuse, offset, 1e-3, 0., &repeated);

	print_result("loose budget (abs_tol=1e-3):", &approximate);
	print_result("zero tolerance (exact leaves):", &exact);

	printf("determinism: repeated loose query %s\n",
		approximate.color[0] == repeated.color[0] && approximate.leaf_evals == repeated.leaf_evals && approximate.shadow_queries == repeated.shadow_queries ? "matches byte-for-byte" : "MISMATCH");
	printf("midpoint inside interval on every channel: %s\n",
		(approximate.color[0] >= approximate.lower[0] && approximate.color[0] <= approximate.upper[0]
			&& approximate.color[1] >= approximate.lower[1] && approximate.color[1] <= approximate.upper[1]
			&& approximate.color[2] >= approximate.lower[2] && approximate.color[2] <= approximate.upper[2])
			? "yes"
			: "no");

	lighttree_free(tree);
	free(lights);
	accel_deinit();
	objects_deinit();
	free(receiver_texture);
	free(black_texture);
	return 0;
}
