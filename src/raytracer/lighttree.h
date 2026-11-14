/*
 * Deterministic hierarchical point-light integrator.
 *
 * The tree is a read-only snapshot of the caller's point lights.  Each query
 * maintains a per-channel interval over the exact diffuse direct-light sum and
 * refines spatial clusters until every channel meets a single global error
 * budget expressed in absolute and relative tolerances.  No random numbers are
 * consumed: leaves are resolved through the production shadow path only.
 **/

#ifndef __LIGHTTREE_H__
#define __LIGHTTREE_H__

#include <stddef.h>

#include "type.h"

struct PointLight {
	v3 position;
	v3 intensity;
};

struct LightResult {
	double color[3];
	double lower[3];
	double upper[3];
	size_t bound_evals;
	size_t leaf_evals;
	size_t shadow_queries;
};

typedef struct LightTree LightTree;

/*
 * Build a read-only snapshot of the lights.  Positions must be finite and
 * intensities must be finite and non-negative; invalid input aborts the
 * process like the rest of the engine.  An empty set yields a non-NULL tree
 * whose queries return all-zero results.
 */
LightTree *lighttree_build(const struct PointLight *lights, size_t count);

void lighttree_free(LightTree *tree);

/*
 * Integrate diffuse direct lighting at a shading point.
 *
 * point:     shading point
 * normal:    unit surface normal
 * diffuse:   surface albedo, components in [0,1]
 * offset:    strictly positive denominator offset (kept independent of the
 *            legacy light attenuation offset)
 * abs_tol, rel_tol: non-negative per-channel global error tolerances
 * result:    interval, estimate and truthful work counters
 *
 * The exact reference per light and channel c is
 *   diffuse[c] * intensity[c] * max(0, n.wi) / (offset + |point-light|^2)
 * multiplied by the colored transmission of the production shadow path
 * is_light_blocked(ray, distance, intensity, NULL).  A light coincident with
 * the shading point contributes zero.
 */
void lighttree_eval(const LightTree *tree, const v3 point, const v3 normal, const v3 diffuse, double offset, double abs_tol, double rel_tol, struct LightResult *result);

#endif /* __LIGHTTREE_H__ */
