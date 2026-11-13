/*
 * skin_bvh_example - minimal cgltf_skin_bvh usage demo.
 *
 * Usage: skin_bvh_example <file.gltf>
 *
 * Loads the given glTF file (buffers loaded, validated), builds a skinned BVH
 * over the default scene and casts the ray O = (0.25, 0.25, -2), D = (0, 0, 1)
 * against the file's static pose. For the bundled scene.gltf the closest hit
 * is node 0, triangle 0 at t = 2, in which case SKIN_EXAMPLE_HIT is printed.
 *
 * Build (from the repository root):
 *   cc -std=c99 -O2 -I. task_support/cgltf_impl.c examples/cgltf_skin_bvh.c \
 *      examples/skin_bvh_example.c -lm -o skin_bvh_example
 */
#include "cgltf.h"
#include "cgltf_skin_bvh.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		fprintf(stderr, "usage: %s <file.gltf>\n", argv[0]);
		return 2;
	}

	cgltf_options options;
	memset(&options, 0, sizeof(options));
	cgltf_data* data = NULL;
	if (cgltf_parse_file(&options, argv[1], &data) != cgltf_result_success)
	{
		fprintf(stderr, "failed to parse %s\n", argv[1]);
		return 1;
	}
	if (cgltf_load_buffers(&options, data, argv[1]) != cgltf_result_success)
	{
		fprintf(stderr, "failed to load buffers\n");
		cgltf_free(data);
		return 1;
	}
	if (cgltf_validate(data) != cgltf_result_success)
	{
		fprintf(stderr, "failed to validate\n");
		cgltf_free(data);
		return 1;
	}
	if (!data->scene)
	{
		fprintf(stderr, "no default scene\n");
		cgltf_free(data);
		return 1;
	}

	cgltf_skin_bvh* bvh = NULL;
	cgltf_skin_bvh_result res = cgltf_skin_bvh_create(&bvh, data, data->scene);
	if (res != cgltf_skin_bvh_result_success)
	{
		fprintf(stderr, "cgltf_skin_bvh_create failed: %d\n", (int)res);
		cgltf_free(data);
		return 1;
	}

	const float origin[3] = {0.25f, 0.25f, -2.0f};
	const float direction[3] = {0.0f, 0.0f, 1.0f};
	cgltf_skin_bvh_hit hit;
	cgltf_skin_bvh_stats stats;
	res = cgltf_skin_bvh_raycast(bvh, origin, direction, 0.0f, 1.0e30f, &hit, &stats);
	if (res == cgltf_skin_bvh_result_success &&
		hit.node_index == 0 && hit.triangle_index == 0 &&
		fabsf(hit.t - 2.0f) <= 1.0e-5f * (1.0f + fabsf(hit.t)))
	{
		printf("SKIN_EXAMPLE_HIT\n");
	}
	else if (res == cgltf_skin_bvh_result_success)
	{
		printf("unexpected hit: node %zu triangle %zu t %g\n",
			(size_t)hit.node_index, (size_t)hit.triangle_index, (double)hit.t);
	}
	else
	{
		printf("no hit (result %d)\n", (int)res);
	}

	cgltf_skin_bvh_destroy(bvh);
	cgltf_free(data);
	return 0;
}
