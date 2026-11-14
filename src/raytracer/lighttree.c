/*
 * Deterministic hierarchical point-light integrator.
 *
 * Construction builds a read-only median-split spatial tree over a snapshot of
 * the caller's lights and aggregates per-channel power at every node.  A query
 * keeps the current partition of the lights as an interval per channel:
 *
 *   - a resolved leaf contributes its exact value on both ends;
 *   - an unresolved node contributes [0, upper], where the upper bound uses the
 *     closest point of the node bounding box, the half-space support of the
 *     shading normal and the aggregated power.
 *
 * The node with the largest remaining per-channel interval is refined until
 * the single global budget (abs_tol + rel_tol * lower) is met on every channel
 * or every leaf has been evaluated exactly.  Visibility is never assumed
 * coherent: only leaves traverse the production shadow path.
 **/

#include "lighttree.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "calc.h"
#include "mem.h"
#include "object.h"

bool is_light_blocked(const struct Ray *ray, float distance, v3 light_intensity, const struct Object *emittant_object);

#define LT_CHANNELS 3
#define LT_INSERTION_CUTOFF 24
#define LT_PQ_INITIAL_CAP 64
/* Conservatively inflates analytic upper bounds to absorb floating-point
 * rounding; far below the 2e-6 margin granted to query results. */
#define LT_BOUND_SAFETY (1. + 1e-12)

struct LTNode {
	double bmin[LT_CHANNELS];
	double bmax[LT_CHANNELS];
	double power[LT_CHANNELS]; /* sum of intensities */
	size_t first;
	size_t count;
	int left; /* child node indices, -1 for leaves */
	int right;
};

struct LightTree {
	struct PointLight *lights;
	size_t *order;
	struct LTNode *nodes;
	size_t num_nodes;
	size_t num_lights;
};

struct LTEntry {
	size_t node;
	double lo[LT_CHANNELS];
	double hi[LT_CHANNELS];
};

struct LTPriorityQueue {
	struct LTEntry *entries;
	size_t count;
	size_t cap;
};

static bool finite_v3(const v3 vec)
{
	return isfinite((double)vec[0]) && isfinite((double)vec[1]) && isfinite((double)vec[2]);
}

/* Deterministic in-place quicksort with an insertion-sort cutoff. */
static double sort_key(const struct PointLight *lights, const size_t *order, const size_t idx, const int axis)
{
	return (double)lights[order[idx]].position[axis];
}

static bool sort_less(const struct PointLight *lights, const size_t *order, const size_t a, const size_t b, const int axis)
{
	const double ka = sort_key(lights, order, a, axis);
	const double kb = sort_key(lights, order, b, axis);
	if (ka != kb)
		return ka < kb;
	return order[a] < order[b];
}

static void insertion_sort(const struct PointLight *lights, size_t *order, const size_t first, const size_t count, const int axis)
{
	size_t i, j;
	for (i = first + 1; i < first + count; i++) {
		const size_t value = order[i];
		for (j = i; j > first && sort_key(lights, order, j - 1, axis) > (double)lights[value].position[axis]; j--)
			order[j] = order[j - 1];
		order[j] = value;
	}
}

static void order_sort(const struct PointLight *lights, size_t *order, size_t first, size_t count, const int axis)
{
	/* Tail-recursive Lomuto quicksort; always recurse into the smaller
	 * partition so the call depth stays logarithmic. */
	while (count > LT_INSERTION_CUTOFF) {
		const size_t lo = first;
		const size_t hi = first + count - 1;
		const size_t mid = first + count / 2;

		/* Median of three, then park the pivot at hi. */
		if (sort_less(lights, order, mid, lo, axis)) {
			size_t tmp = order[lo];
			order[lo] = order[mid];
			order[mid] = tmp;
		}
		if (sort_less(lights, order, hi, lo, axis)) {
			size_t tmp = order[lo];
			order[lo] = order[hi];
			order[hi] = tmp;
		}
		if (sort_less(lights, order, hi, mid, axis)) {
			size_t tmp = order[mid];
			order[mid] = order[hi];
			order[hi] = tmp;
		}

		/* Lomuto partition over the strict (key, original index) order. */
		size_t store = lo;
		for (size_t k = lo; k < hi; k++) {
			if (sort_less(lights, order, k, hi, axis)) {
				size_t tmp = order[store];
				order[store] = order[k];
				order[k] = tmp;
				store++;
			}
		}
		size_t tmp = order[store];
		order[store] = order[hi];
		order[hi] = tmp;

		const size_t left_count = store - lo;
		const size_t right_count = hi - store;
		if (left_count < right_count) {
			if (left_count > 1)
				order_sort(lights, order, lo, left_count, axis);
			first = store + 1;
			count = right_count;
		} else {
			if (right_count > 1)
				order_sort(lights, order, store + 1, right_count, axis);
			count = left_count;
		}
	}

	if (count > 1)
		insertion_sort(lights, order, first, count, axis);
}

static int build_recursive(struct LightTree *tree, const size_t first, const size_t count)
{
	const size_t node_idx = tree->num_nodes++;
	struct LTNode *node = &tree->nodes[node_idx];
	node->first = first;
	node->count = count;
	node->left = -1;
	node->right = -1;

	size_t i, c;
	for (c = 0; c < LT_CHANNELS; c++) {
		node->bmin[c] = (double)tree->lights[tree->order[first]].position[c];
		node->bmax[c] = node->bmin[c];
		node->power[c] = 0.;
	}
	for (i = first; i < first + count; i++) {
		const struct PointLight *light = &tree->lights[tree->order[i]];
		for (c = 0; c < LT_CHANNELS; c++) {
			const double p = (double)light->position[c];
			const double intensity = (double)light->intensity[c];
			if (p < node->bmin[c])
				node->bmin[c] = p;
			if (p > node->bmax[c])
				node->bmax[c] = p;
			node->power[c] += intensity;
		}
	}

	if (count == 1)
		return (int)node_idx;

	int axis = 0;
	double extent = node->bmax[0] - node->bmin[0];
	for (c = 1; c < LT_CHANNELS; c++) {
		const double axis_extent = node->bmax[c] - node->bmin[c];
		if (axis_extent > extent) {
			extent = axis_extent;
			axis = (int)c;
		}
	}

	order_sort(tree->lights, tree->order, first, count, axis);
	const size_t left_count = count / 2;
	node->left = build_recursive(tree, first, left_count);
	node->right = build_recursive(tree, first + left_count, count - left_count);
	return (int)node_idx;
}

LightTree *lighttree_build(const struct PointLight *lights, const size_t count)
{
	if (count)
		error_check(lights != NULL, "lighttree_build: lights is NULL for count [%zu].", count);

	struct LightTree *tree = safe_calloc(1, sizeof(struct LightTree));
	tree->num_lights = count;
	if (!count)
		return tree;

	tree->lights = safe_malloc(sizeof(struct PointLight) * count);
	tree->order = safe_malloc(sizeof(size_t) * count);
	tree->nodes = safe_malloc(sizeof(struct LTNode) * (2 * count - 1));

	size_t i, c;
	for (i = 0; i < count; i++) {
		error_check(finite_v3(lights[i].position), "lighttree_build: light [%zu] has a non-finite position.", i);
		error_check(finite_v3(lights[i].intensity), "lighttree_build: light [%zu] has non-finite intensity.", i);
		for (c = 0; c < LT_CHANNELS; c++)
			error_check((double)lights[i].intensity[c] >= 0., "lighttree_build: light [%zu] has negative intensity.", i);
		tree->lights[i] = lights[i];
		tree->order[i] = i;
	}

	build_recursive(tree, 0, count);
	return tree;
}

void lighttree_free(LightTree *tree)
{
	if (!tree)
		return;
	free(tree->lights);
	free(tree->order);
	free(tree->nodes);
	free(tree);
}

/* Priority queue over active nodes: largest remaining interval first, with
 * deterministic index tie-breaks. */
static double entry_gap(const struct LTEntry *entry)
{
	double gap = 0.;
	for (size_t c = 0; c < LT_CHANNELS; c++) {
		const double channel_gap = entry->hi[c] - entry->lo[c];
		if (channel_gap > gap)
			gap = channel_gap;
	}
	return gap;
}

static bool entry_higher_priority(const struct LTEntry *a, const struct LTEntry *b)
{
	const double gap_a = entry_gap(a);
	const double gap_b = entry_gap(b);
	if (gap_a != gap_b)
		return gap_a > gap_b;
	return a->node < b->node;
}

static void pq_push(struct LTPriorityQueue *pq, const struct LTEntry *entry)
{
	if (pq->count == pq->cap) {
		pq->cap *= 2;
		pq->entries = safe_realloc(pq->entries, sizeof(struct LTEntry) * pq->cap);
	}
	size_t child = pq->count++;
	pq->entries[child] = *entry;
	while (child) {
		const size_t parent = (child - 1) / 2;
		if (!entry_higher_priority(&pq->entries[child], &pq->entries[parent]))
			break;
		struct LTEntry tmp = pq->entries[parent];
		pq->entries[parent] = pq->entries[child];
		pq->entries[child] = tmp;
		child = parent;
	}
}

static struct LTEntry pq_pop(struct LTPriorityQueue *pq)
{
	const struct LTEntry result = pq->entries[0];
	pq->entries[0] = pq->entries[--pq->count];
	size_t parent = 0;
	for (;;) {
		size_t left = 2 * parent + 1;
		if (left >= pq->count)
			break;
		size_t right = left + 1;
		size_t child = left;
		if (right < pq->count && entry_higher_priority(&pq->entries[right], &pq->entries[left]))
			child = right;
		if (!entry_higher_priority(&pq->entries[child], &pq->entries[parent]))
			break;
		struct LTEntry tmp = pq->entries[parent];
		pq->entries[parent] = pq->entries[child];
		pq->entries[child] = tmp;
		parent = child;
	}
	return result;
}

/*
 * Conservative node interval.  The lower bound is always zero because
 * visibility cannot be promoted from a representative light to a whole cluster.
 * The upper bound assumes every light is fully visible and combines:
 *   - dmin:  distance to the closest point of the bounding box,
 *   - cos:   half-space support of n over the box, normalized by dmin,
 *   - power: aggregated per-channel intensity.
 */
static void node_bound(const struct LightTree *tree, const struct LTNode *node, const double point[LT_CHANNELS], const double normal[LT_CHANNELS], const double diffuse[LT_CHANNELS], const double offset, struct LTEntry *entry)
{
	entry->node = (size_t)(node - tree->nodes);
	for (size_t c = 0; c < LT_CHANNELS; c++) {
		entry->lo[c] = 0.;
		entry->hi[c] = 0.;
	}

	double dmin2 = 0.;
	double support = 0.; /* max over the box of n . (light - point) */
	for (size_t c = 0; c < LT_CHANNELS; c++) {
		double closest;
		if (point[c] < node->bmin[c]) {
			closest = node->bmin[c];
		} else if (point[c] > node->bmax[c]) {
			closest = node->bmax[c];
		} else {
			closest = point[c];
		}
		const double delta = closest - point[c];
		dmin2 += delta * delta;
		const double support_coord = normal[c] >= 0. ? node->bmax[c] : node->bmin[c];
		support += normal[c] * (support_coord - point[c]);
	}

	double cos_max;
	if (dmin2 == 0.) {
		/* Shading point is inside the box: the cosine bound is trivially 1. */
		cos_max = 1.;
	} else if (support <= 0.) {
		/* Every light in the node lies in the back hemisphere. */
		cos_max = 0.;
	} else {
		cos_max = support / sqrt(dmin2);
		if (cos_max > 1.)
			cos_max = 1.;
	}

	const double denom = offset + dmin2;
	for (size_t c = 0; c < LT_CHANNELS; c++)
		entry->hi[c] = diffuse[c] * node->power[c] * cos_max / denom * LT_BOUND_SAFETY;
}

static void eval_leaf(const struct LightTree *tree, const struct LTNode *node, const double point[LT_CHANNELS], const double normal[LT_CHANNELS], const double diffuse[LT_CHANNELS], const double offset, struct LightResult *result, double exact[LT_CHANNELS])
{
	for (size_t i = node->first; i < node->first + node->count; i++) {
		const struct PointLight *light = &tree->lights[tree->order[i]];
		result->leaf_evals++;

		double delta[LT_CHANNELS];
		double dist2 = 0.;
		for (size_t c = 0; c < LT_CHANNELS; c++) {
			delta[c] = (double)light->position[c] - point[c];
			dist2 += delta[c] * delta[c];
		}
		if (dist2 == 0.)
			continue; /* Coincident light and shading point: zero contribution. */

		const double dist = sqrt(dist2);
		double cosine = 0.;
		for (size_t c = 0; c < LT_CHANNELS; c++)
			cosine += normal[c] * delta[c] / dist;
		if (cosine <= 0.)
			continue; /* Back-facing light: identically zero, no shadow query. */

		struct Ray ray;
		for (size_t c = 0; c < LT_CHANNELS; c++) {
			ray.point[c] = (float)point[c];
			ray.direction[c] = (float)(delta[c] / dist);
		}
		norm3(ray.direction);

		v3 transmission = { 1.f, 1.f, 1.f };
		const bool blocked = is_light_blocked(&ray, (float)dist, transmission, NULL);
		result->shadow_queries++;
		if (blocked)
			continue;

		const double geometry = cosine / (offset + dist2);
		for (size_t c = 0; c < LT_CHANNELS; c++) {
			double t = (double)transmission[c];
			if (t < 0.)
				t = 0.;
			else if (t > 1.)
				t = 1.;
			exact[c] += diffuse[c] * (double)light->intensity[c] * geometry * t;
		}
	}
}

void lighttree_eval(const LightTree *tree, const v3 point, const v3 normal, const v3 diffuse, const double offset, const double abs_tol, const double rel_tol, struct LightResult *result)
{
	memset(result, 0, sizeof(struct LightResult));
	if (!tree || !tree->num_lights)
		return;

	error_check(isfinite(offset) && offset > 0., "lighttree_eval: offset must be strictly positive.");
	error_check(isfinite(abs_tol) && abs_tol >= 0. && isfinite(rel_tol) && rel_tol >= 0., "lighttree_eval: tolerances must be non-negative.");
	error_check(finite_v3(point) && finite_v3(normal) && finite_v3(diffuse), "lighttree_eval: inputs must be finite.");

	double p[LT_CHANNELS], n[LT_CHANNELS], d[LT_CHANNELS];
	double normal_length = 0.;
	for (size_t c = 0; c < LT_CHANNELS; c++) {
		p[c] = (double)point[c];
		n[c] = (double)normal[c];
		d[c] = (double)diffuse[c];
		normal_length += n[c] * n[c];
		error_check(d[c] >= 0. && d[c] <= 1., "lighttree_eval: diffuse components must be in [0,1].");
	}
	error_check(fabs(sqrt(normal_length) - 1.) < 1e-4, "lighttree_eval: normal must be a unit vector.");

	double lower[LT_CHANNELS] = { 0., 0., 0. };
	double upper[LT_CHANNELS] = { 0., 0., 0. };

	struct LTPriorityQueue pq;
	pq.cap = LT_PQ_INITIAL_CAP;
	pq.count = 0;
	pq.entries = safe_malloc(sizeof(struct LTEntry) * pq.cap);

	struct LTEntry root;
	node_bound(tree, &tree->nodes[0], p, n, d, offset, &root);
	result->bound_evals = 1;
	pq_push(&pq, &root);
	for (size_t c = 0; c < LT_CHANNELS; c++)
		upper[c] += root.hi[c];

	const bool exact_mode = (abs_tol == 0. && rel_tol == 0.);

	for (;;) {
		bool within_tolerance;
		if (exact_mode) {
			/* Both tolerances zero: only the exact leaf sum is
			 * admissible, so expand every cluster regardless of
			 * whether its conservative upper bound is already zero. */
			within_tolerance = (pq.count == 0);
		} else {
			within_tolerance = true;
			for (size_t c = 0; c < LT_CHANNELS; c++) {
				const double allowance = abs_tol + rel_tol * lower[c] + 2e-6 * fmax(1., lower[c]);
				if (upper[c] - lower[c] > allowance) {
					within_tolerance = false;
					break;
				}
			}
		}
		if (within_tolerance || !pq.count)
			break;

		const struct LTEntry current = pq_pop(&pq);
		const struct LTNode *node = &tree->nodes[current.node];
		for (size_t c = 0; c < LT_CHANNELS; c++)
			upper[c] -= current.hi[c] - current.lo[c];

		if (node->left < 0) {
			double exact[LT_CHANNELS] = { 0., 0., 0. };
			eval_leaf(tree, node, p, n, d, offset, result, exact);
			for (size_t c = 0; c < LT_CHANNELS; c++) {
				lower[c] += exact[c];
				upper[c] += exact[c];
			}
		} else {
			struct LTEntry child;
			node_bound(tree, &tree->nodes[node->left], p, n, d, offset, &child);
			result->bound_evals++;
			pq_push(&pq, &child);
			for (size_t c = 0; c < LT_CHANNELS; c++)
				upper[c] += child.hi[c];

			node_bound(tree, &tree->nodes[node->right], p, n, d, offset, &child);
			result->bound_evals++;
			pq_push(&pq, &child);
			for (size_t c = 0; c < LT_CHANNELS; c++)
				upper[c] += child.hi[c];
		}
	}

	for (size_t c = 0; c < LT_CHANNELS; c++) {
		/* Bound add/subtract round-trips can leave sub-ulp drift; the
		 * true interval always satisfies lower <= upper, so snap purely
		 * rounding-induced inversions before publishing. */
		if (upper[c] < lower[c])
			upper[c] = lower[c];
		result->lower[c] = lower[c];
		result->upper[c] = upper[c];
		double midpoint = (lower[c] + upper[c]) * 0.5;
		/* Clamping absorbs midpoint rounding so the estimate is always
		 * literally inside the returned interval. */
		if (midpoint < lower[c])
			midpoint = lower[c];
		else if (midpoint > upper[c])
			midpoint = upper[c];
		result->color[c] = midpoint;
	}
	free(pq.entries);
}
