/*
 * lighttree.c - Deterministic bounding hierarchy for many point lights.
 *
 * Each query owns all traversal state.  The immutable tree may therefore be
 * shared by concurrent callers; visibility queries themselves use the
 * repository's existing global acceleration structures.
 */

#include "lighttree.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "calc.h"
#include "object.h"

/* Provided by render.c; declared locally to avoid exposing this test-only seam
 * in the public render header. */
bool is_light_blocked(const struct Ray *ray, float distance, v3 light_intensity, const struct Object *emittant_object);

struct LightNode {
	double bmin[3];
	double bmax[3];
	long double power[3]; /* Sum of RGB intensities. */
	size_t first;
	size_t count;
	int left;
	int right;
};

struct LightTree {
	size_t count;
	PointLight *lights; /* Sorted snapshot owned by the tree. */
	size_t node_count;
	struct LightNode *nodes;
};

struct BoundHeapEntry {
	size_t node;
	long double key;
};

struct BoundHeap {
	struct BoundHeapEntry *items;
	size_t len;
};

struct EvalState {
	size_t bound_evals;
	size_t leaf_evals;
	size_t shadow_queries;

	long double lower[3];
	long double lower_comp[3];
	long double unresolved[3];

	long double (*upper)[3];
	unsigned char *active;
	struct BoundHeap heaps[3];
};

static int finite_nonnegative(const PointLight *lights, const size_t count)
{
	size_t i, c;

	if (count && !lights)
		return 0;
	for (i = 0; i < count; ++i) {
		for (c = 0; c < 3; ++c) {
			if (!isfinite(lights[i].position[c]))
				return 0;
			if (!isfinite(lights[i].intensity[c]) || lights[i].intensity[c] < 0.f)
				return 0;
		}
	}
	return 1;
}

static void swap_lights(PointLight *a, PointLight *b)
{
	PointLight tmp = *a;
	*a = *b;
	*b = tmp;
}

/* Deterministic total order.  Ties are broken on all remaining coordinates and
 * intensities so duplicate positions still produce a stable balanced split. */
static int light_less(const PointLight *a, const PointLight *b, const int axis)
{
	int k;

	if (a->position[axis] < b->position[axis])
		return 1;
	if (a->position[axis] > b->position[axis])
		return 0;

	for (k = 0; k < 3; ++k) {
		if (a->position[k] < b->position[k])
			return 1;
		if (a->position[k] > b->position[k])
			return 0;
	}
	for (k = 0; k < 3; ++k) {
		if (a->intensity[k] < b->intensity[k])
			return 1;
		if (a->intensity[k] > b->intensity[k])
			return 0;
	}
	return 0;
}

static void heap_sort_sift_down(PointLight *lights, const size_t start, const size_t end, const int axis)
{
	size_t root = start;

	while (root * 2u + 1u <= end) {
		size_t child = root * 2u + 1u;
		size_t chosen = root;

		if (light_less(&lights[chosen], &lights[child], axis))
			chosen = child;
		if (child + 1u <= end && light_less(&lights[chosen], &lights[child + 1u], axis))
			chosen = child + 1u;
		if (chosen == root)
			return;
		swap_lights(&lights[root], &lights[chosen]);
		root = chosen;
	}
}

static void sort_range(PointLight *lights, const size_t first, const size_t count, const int axis)
{
	PointLight *base = lights + first;
	size_t end = count - 1u;
	long long start;

	if (count < 2u)
		return;

	start = (long long)((count - 2u) / 2u);
	while (start >= 0) {
		heap_sort_sift_down(base, (size_t)start, end, axis);
		--start;
	}

	while (end > 0u) {
		swap_lights(&base[0], &base[end]);
		--end;
		heap_sort_sift_down(base, 0, end, axis);
	}
}

static size_t build_node(LightTree *tree, const size_t first, const size_t count)
{
	const size_t node_index = tree->node_count++;
	struct LightNode *node = &tree->nodes[node_index];
	size_t i, c;
	int axis = 0;
	double extent, best_extent = -1.0;
	size_t left_count;

	node->first = first;
	node->count = count;
	node->left = -1;
	node->right = -1;

	const PointLight *first_light = &tree->lights[first];
	for (c = 0; c < 3; ++c) {
		node->bmin[c] = (double)first_light->position[c];
		node->bmax[c] = (double)first_light->position[c];
		node->power[c] = 0.0L;
	}

	for (i = first; i < first + count; ++i) {
		const PointLight *light = &tree->lights[i];
		for (c = 0; c < 3; ++c) {
			const double x = (double)light->position[c];
			node->bmin[c] = fmin(node->bmin[c], x);
			node->bmax[c] = fmax(node->bmax[c], x);
			node->power[c] += (long double)light->intensity[c];
		}
	}

	if (count == 1u)
		return node_index;

	for (c = 0; c < 3; ++c) {
		extent = node->bmax[c] - node->bmin[c];
		if (extent > best_extent) {
			best_extent = extent;
			axis = (int)c;
		}
	}

	sort_range(tree->lights, first, count, axis);
	left_count = (count + 1u) / 2u;
	node->left = (int)build_node(tree, first, left_count);
	node->right = (int)build_node(tree, first + left_count, count - left_count);
	return node_index;
}

LightTree *lighttree_build(const PointLight *lights, const size_t count)
{
	LightTree *tree;

	if (!finite_nonnegative(lights, count))
		return NULL;

	tree = calloc(1, sizeof(*tree));
	if (!tree)
		return NULL;
	tree->count = count;

	if (count == 0u)
		return tree;

	if (count > (SIZE_MAX - 1u) / 2u) {
		free(tree);
		return NULL;
	}

	tree->lights = malloc(count * sizeof(*tree->lights));
	tree->nodes = calloc(2u * count, sizeof(*tree->nodes));
	if (!tree->lights || !tree->nodes) {
		free(tree->lights);
		free(tree->nodes);
		free(tree);
		return NULL;
	}

	memcpy(tree->lights, lights, count * sizeof(*tree->lights));
	build_node(tree, 0, count);
	return tree;
}

void lighttree_free(LightTree *tree)
{
	if (!tree)
		return;
	free(tree->lights);
	free(tree->nodes);
	free(tree);
}

static int heap_entry_better(const struct BoundHeapEntry *a, const struct BoundHeapEntry *b)
{
	if (a->key != b->key)
		return a->key > b->key;
	return a->node < b->node;
}

static void heap_push(struct BoundHeap *heap, const size_t node, const long double key, const size_t capacity)
{
	size_t i = heap->len++;

	/* The allocated capacity is one past the tree node count, so this is
	 * defensive only. */
	if (i >= capacity)
		return;

	heap->items[i].node = node;
	heap->items[i].key = key;
	while (i > 0u) {
		const size_t parent = (i - 1u) / 2u;
		if (!heap_entry_better(&heap->items[i], &heap->items[parent]))
			break;
		struct BoundHeapEntry tmp = heap->items[i];
		heap->items[i] = heap->items[parent];
		heap->items[parent] = tmp;
		i = parent;
	}
}

static struct BoundHeapEntry heap_pop(struct BoundHeap *heap)
{
	struct BoundHeapEntry top = heap->items[0];
	size_t i = 0u;

	heap->items[0] = heap->items[--heap->len];
	while (heap->len > 0u) {
		size_t left = i * 2u + 1u;
		size_t right = left + 1u;
		size_t best = i;

		if (left < heap->len && heap_entry_better(&heap->items[left], &heap->items[best]))
			best = left;
		if (right < heap->len && heap_entry_better(&heap->items[right], &heap->items[best]))
			best = right;
		if (best == i)
			break;

		struct BoundHeapEntry tmp = heap->items[i];
		heap->items[i] = heap->items[best];
		heap->items[best] = tmp;
		i = best;
	}
	return top;
}

static void add_compensated(long double *sum, long double *compensation, const long double value)
{
	const long double y = value - *compensation;
	const long double next = *sum + y;
	*compensation = (next - *sum) - y;
	*sum = next;
}

/* Conservative unoccluded upper bound.  Visibility contributes an additional
 * factor in [0,1], so it is safe to ignore here and use zero as the lower
 * bound for an unresolved cluster. */
static void compute_node_bound(const LightTree *tree,
	const size_t node_index,
	const double point[3],
	const double normal[3],
	const double diffuse[3],
	const double offset,
	struct EvalState *state,
	long double bound[3])
{
	const struct LightNode *node = &tree->nodes[node_index];
	double nearest_sq = 0.0;
	double halfspace_max = 0.0;
	double scale = 1.0;
	double normal_len_sq = 0.0;
	double geometry = 0.0;
	int c;

	++state->bound_evals;

	for (c = 0; c < 3; ++c) {
		double nearest = 0.0;
		double extreme;

		if (point[c] < node->bmin[c])
			nearest = node->bmin[c] - point[c];
		else if (point[c] > node->bmax[c])
			nearest = point[c] - node->bmax[c];
		nearest_sq += nearest * nearest;

		/* Maximize n . (q-p) over the axis interval. */
		extreme = (normal[c] >= 0.0) ? node->bmax[c] : node->bmin[c];
		halfspace_max += normal[c] * (extreme - point[c]);

		scale = fmax(scale, fabs(node->bmin[c]) + fabs(node->bmax[c]) + fabs(point[c]));
		normal_len_sq += normal[c] * normal[c];
	}

	/* Cull only when the whole box is robustly behind the normal half-space.
	 * A tangent or rounding-sensitive box keeps the generic upper bound. */
	if (halfspace_max < -1e-12 * scale) {
		bound[0] = bound[1] = bound[2] = 0.0L;
		return;
	}

	/* Deliberately shrink nearest_sq slightly so floating-point rounding can
	 * only loosen, never tighten, the geometric denominator. */
	nearest_sq = fmax(0.0, nearest_sq - 1e-12 * fmax(1.0, nearest_sq));
	geometry = sqrt(normal_len_sq) / (offset + nearest_sq);

	for (c = 0; c < 3; ++c)
		bound[c] = (long double)geometry * (long double)diffuse[c] * node->power[c];
}

static void evaluate_light(const LightTree *tree,
	const size_t light_index,
	const v3 point,
	const double normal[3],
	const double diffuse[3],
	const double offset,
	struct EvalState *state)
{
	const PointLight *light = &tree->lights[light_index];
	v3 delta;
	v3 wi;
	v3 transmission = { 1.f, 1.f, 1.f };
	struct Ray shadow_ray;
	float distance;
	int c;

	++state->leaf_evals;

	sub3v(light->position, point, delta);
	distance = mag3(delta);

	/* A zero-length segment has no well-defined wi and the reference defines
	 * the coincident contribution as zero. */
	if (!(distance > 0.f) || !isfinite(distance))
		return;

	mul3s(delta, 1.f / distance, wi);
	assign3(shadow_ray.point, point);
	assign3(shadow_ray.direction, wi);

	++state->shadow_queries;
	if (is_light_blocked(&shadow_ray, distance, transmission, NULL))
		return;

	for (c = 0; c < 3; ++c) {
		double trans = (double)transmission[c];
		double dx = (double)light->position[0] - (double)point[0];
		double dy = (double)light->position[1] - (double)point[1];
		double dz = (double)light->position[2] - (double)point[2];
		double distance_sq = dx * dx + dy * dy + dz * dz;
		double n_dot_wi;
		double contribution;

		if (!isfinite(trans))
			trans = 0.0;
		trans = fmax(0.0, fmin(1.0, trans));
#ifdef LIGHTTREE_DROP_COLORED_TRANSMISSION
		trans = 1.0;
#endif

		n_dot_wi = normal[0] * (double)wi[0] + normal[1] * (double)wi[1] + normal[2] * (double)wi[2];
		n_dot_wi = fmax(0.0, n_dot_wi);
		contribution = (double)diffuse[c] * (double)light->intensity[c] * n_dot_wi
			/ (offset + distance_sq) * trans;
		add_compensated(&state->lower[c], &state->lower_comp[c], contribution);
	}
}

static void eval_exact_node(const LightTree *tree,
	const size_t node_index,
	const v3 point,
	const double normal[3],
	const double diffuse[3],
	const double offset,
	const double p[3],
	struct EvalState *state)
{
	long double discarded_bound[3];
	const struct LightNode *node = &tree->nodes[node_index];

	/* Count the same conservative node-bound seam used by approximate cuts,
	 * even though zero tolerance then resolves every leaf exactly. */
	compute_node_bound(tree, node_index, p, normal, diffuse, offset, state, discarded_bound);

	if (node->left < 0) {
		evaluate_light(tree, node->first, point, normal, diffuse, offset, state);
		return;
	}

	eval_exact_node(tree, (size_t)node->left, point, normal, diffuse, offset, p, state);
	eval_exact_node(tree, (size_t)node->right, point, normal, diffuse, offset, p, state);
}

static void recompute_unresolved(const LightTree *tree, struct EvalState *state)
{
	size_t node_index;
	int c;

	for (c = 0; c < 3; ++c)
		state->unresolved[c] = 0.0L;

	for (node_index = 0; node_index < tree->node_count; ++node_index) {
		if (!state->active[node_index])
			continue;
		for (c = 0; c < 3; ++c)
			state->unresolved[c] += state->upper[node_index][c];
	}
}

static int tolerances_met(const struct EvalState *state, const double abs_tol, const double rel_tol)
{
	int c;

	for (c = 0; c < 3; ++c) {
		long double tolerance = (long double)abs_tol + (long double)rel_tol * state->lower[c];
		if (state->unresolved[c] > tolerance)
			return 0;
	}
	return 1;
}

static int choose_channel(const struct EvalState *state, const double abs_tol, const double rel_tol)
{
	int c;
	int zero_channel = -1;
	int ratio_channel = -1;
	long double best_zero = -1.0L;
	long double best_ratio = -1.0L;

	for (c = 0; c < 3; ++c) {
		long double tolerance = (long double)abs_tol + (long double)rel_tol * state->lower[c];
		long double urgency;

		if (state->unresolved[c] <= tolerance)
			continue;

		if (tolerance <= 0.0L) {
			if (state->unresolved[c] > best_zero) {
				best_zero = state->unresolved[c];
				zero_channel = c;
			}
		} else {
			urgency = state->unresolved[c] / tolerance;
			if (urgency > best_ratio) {
				best_ratio = urgency;
				ratio_channel = c;
			}
		}
	}

	return zero_channel >= 0 ? zero_channel : ratio_channel;
}

static void finish_result(LightResult *result, const struct EvalState *state)
{
	int c;

	for (c = 0; c < 3; ++c) {
		long double total_upper = state->lower[c] + state->unresolved[c];
		result->lower[c] = (double)state->lower[c];
		result->color[c] = (double)state->lower[c];
		result->upper[c] = (double)total_upper;
	}
	result->bound_evals = state->bound_evals;
	result->leaf_evals = state->leaf_evals;
	result->shadow_queries = state->shadow_queries;
}

void lighttree_eval(const LightTree *tree,
	const v3 point,
	const v3 normal_value,
	const v3 diffuse_value,
	const double offset,
	double abs_tol,
	double rel_tol,
	LightResult *result)
{
	double p[3], normal[3], diffuse[3];
	struct EvalState state;
	size_t c;

	if (!result)
		return;
	memset(result, 0, sizeof(*result));

	if (!tree || tree->count == 0u || !point || !normal_value || !diffuse_value)
		return;

	if (!(abs_tol >= 0.0))
		abs_tol = 0.0;
	if (!(rel_tol >= 0.0))
		rel_tol = 0.0;

	for (c = 0; c < 3; ++c) {
		p[c] = (double)point[c];
		normal[c] = (double)normal_value[c];
		diffuse[c] = (double)diffuse_value[c];
	}

	memset(&state, 0, sizeof(state));

	/* Zero tolerance is an exact leaf expansion.  It deliberately does not
	 * hide lights behind conservative node bounds. */
	if (abs_tol == 0.0 && rel_tol == 0.0) {
		eval_exact_node(tree, 0u, point, normal, diffuse, offset, p, &state);
		finish_result(result, &state);
		return;
	}

	state.upper = calloc(tree->node_count, sizeof(*state.upper));
	state.active = calloc(tree->node_count, sizeof(*state.active));
	for (c = 0; c < 3; ++c)
		state.heaps[c].items = calloc(tree->node_count + 1u, sizeof(struct BoundHeapEntry));

	if (!state.upper || !state.active || !state.heaps[0].items || !state.heaps[1].items || !state.heaps[2].items) {
		/* Defensive fallback: never return a loose/empty answer because of
		 * transient allocation failure. */
		free(state.upper);
		free(state.active);
		for (c = 0; c < 3; ++c)
			free(state.heaps[c].items);
		memset(&state, 0, sizeof(state));
		for (size_t i = 0; i < tree->count; ++i)
			evaluate_light(tree, i, point, normal, diffuse, offset, &state);
		finish_result(result, &state);
		return;
	}

	compute_node_bound(tree, 0, p, normal, diffuse, offset, &state, state.upper[0]);
	state.active[0] = 1;
	for (c = 0; c < 3; ++c) {
		state.unresolved[c] = state.upper[0][c];
		if (state.upper[0][c] > 0.0L)
			heap_push(&state.heaps[c], 0, state.upper[0][c], tree->node_count + 1u);
	}

	size_t processed_since_recompute = 0;
	while (1) {
		int channel;

		if (tolerances_met(&state, abs_tol, rel_tol)) {
			long double fresh[3] = { 0.0L, 0.0L, 0.0L };
			size_t node_index;

			/* Verify with a direct scan so long-double cancellation in the
			 * incremental cut total cannot falsely stop early. */
			for (node_index = 0; node_index < tree->node_count; ++node_index) {
				int k;
				if (!state.active[node_index])
					continue;
				for (k = 0; k < 3; ++k)
					fresh[k] += state.upper[node_index][k];
			}
			if (tolerances_met(&(struct EvalState){ .lower = { state.lower[0], state.lower[1], state.lower[2] },
								  .unresolved = { fresh[0], fresh[1], fresh[2] } },
				    abs_tol,
				    rel_tol)) {
				for (c = 0; c < 3; ++c)
					state.unresolved[c] = fresh[c];
				break;
			}
			for (c = 0; c < 3; ++c)
				state.unresolved[c] = fresh[c];
		}

		if (processed_since_recompute >= 64u) {
			recompute_unresolved(tree, &state);
			processed_since_recompute = 0;
		}

		channel = choose_channel(&state, abs_tol, rel_tol);
		if (channel < 0) {
			recompute_unresolved(tree, &state);
			if (tolerances_met(&state, abs_tol, rel_tol))
				break;
			continue;
		}

		struct BoundHeap *heap = &state.heaps[channel];
		while (heap->len > 0 && !state.active[heap->items[0].node])
			(void)heap_pop(heap);

		if (heap->len == 0) {
			recompute_unresolved(tree, &state);
			continue;
		}

		struct BoundHeapEntry selected = heap_pop(heap);
		const size_t node_index = selected.node;
		struct LightNode *node = &tree->nodes[node_index];

		if (!state.active[node_index])
			continue;

		state.active[node_index] = 0;
		for (c = 0; c < 3; ++c)
			state.unresolved[c] -= state.upper[node_index][c];
		++processed_since_recompute;

		if (node->left < 0) {
			evaluate_light(tree, node->first, point, normal, diffuse, offset, &state);
			continue;
		}

		const int children[2] = { node->left, node->right };
		size_t child_order[2] = { 0u, 1u };
		int q;

		/* Deterministic child processing order for equal priorities. */
		if (state.upper[node->left][channel] < state.upper[node->right][channel]
			|| (state.upper[node->left][channel] == state.upper[node->right][channel]
				&& (size_t)node->left > (size_t)node->right)) {
			child_order[0] = 1u;
			child_order[1] = 0u;
		}

		for (q = 0; q < 2; ++q) {
			const size_t child_index = (size_t)children[child_order[q]];
			state.active[child_index] = 1;
			compute_node_bound(tree, child_index, p, normal, diffuse, offset, &state, state.upper[child_index]);
			for (c = 0; c < 3; ++c) {
				state.unresolved[c] += state.upper[child_index][c];
				if (state.upper[child_index][c] > 0.0L)
					heap_push(&state.heaps[c], child_index, state.upper[child_index][c],
						tree->node_count + 1u);
			}
		}
	}

	finish_result(result, &state);

	free(state.upper);
	free(state.active);
	for (c = 0; c < 3; ++c)
		free(state.heaps[c].items);
}
