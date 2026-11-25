/*
 * Deterministic hierarchical point-light integration.
 *
 * The public interval contract and traversal strategy are described in
 * task_support/README.md.
 */

#ifndef __LIGHTTREE_H__
#define __LIGHTTREE_H__

#include <stddef.h>

#include "type.h"

struct LightTree;
typedef struct LightTree LightTree;

struct PointLight {
	v3 position;
	v3 intensity;
};
typedef struct PointLight PointLight;

struct LightResult {
	double color[3];
	double lower[3];
	double upper[3];
	size_t bound_evals;
	size_t leaf_evals;
	size_t shadow_queries;
};
typedef struct LightResult LightResult;

/* Returns an empty, non-NULL tree for count == 0. Returns NULL for invalid input. */
LightTree *lighttree_build(const PointLight *lights, size_t count);
void lighttree_free(LightTree *tree);

void lighttree_eval(const LightTree *tree,
	const v3 point,
	const v3 normal,
	const v3 diffuse,
	double offset,
	double abs_tol,
	double rel_tol,
	LightResult *result);

#endif /* __LIGHTTREE_H__ */
