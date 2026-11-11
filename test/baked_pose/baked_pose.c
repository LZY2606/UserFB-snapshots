// Player-style regression for animation baking.
//
// For each test scene we bake the animation through the public entry points
// (ufbx_load_file -> ufbx_bake_anim -> ufbx_evaluate_baked_vec3/quat),
// reconstruct world-space poses the way a player would -- compose the baked
// local TRS along the plain parent/child matrix chain, keeping the original
// local pose for nodes that are not part of the bake -- and cross-check the
// result against an independent ufbx_evaluate_scene() at the same time.
//
// All sample times are kept as double throughout: rounding to frame numbers
// or float would collapse adjacent-double step keys and hide real bugs.
//
// On success each file reports the baked node coverage, the maximum
// normalized position error and the maximum normalized rotation basis
// vector error. On mismatch the program prints
//   FAIL file=<path> phase=<phase> node=<name> typed=<id> time=<t> kind=<k> err=<e> limit=<l>
// and exits with status 1.

#include "ufbx.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Normalized world-coordinate error limit: errors are divided by
// max(1, max |reference coordinate|) before comparing against this.
#define POSE_TOLERANCE 2e-3
// The instantaneous jump sample is exact keyframe data and must match tightly.
#define STEP_TOLERANCE 1e-9

static const char *g_file = "?";
static const char *g_phase = "?";

static void fail_num(const char *node_name, uint32_t typed_id, double time, const char *kind, double err, double limit)
{
	fprintf(stderr, "FAIL file=%s phase=%s node=%s typed=%u time=%.17g kind=%s err=%.9g limit=%.9g\n",
		g_file, g_phase, node_name, typed_id, time, kind, err, limit);
	exit(1);
}

static void fail_internal(const char *msg)
{
	fprintf(stderr, "ERROR file=%s phase=%s: %s\n", g_file, g_phase, msg);
	exit(2);
}

// -- Small deterministic time list ----------------------------------------

typedef struct {
	double *data;
	size_t count;
	size_t capacity;
} time_list;

static void push_time(time_list *list, double time)
{
	if (list->count == list->capacity) {
		list->capacity = list->capacity ? list->capacity * 2 : 64;
		list->data = (double *)realloc(list->data, list->capacity * sizeof(double));
		if (!list->data) fail_internal("out of memory");
	}
	list->data[list->count++] = time;
}

static int cmp_double(const void *pa, const void *pb)
{
	double a = *(const double *)pa, b = *(const double *)pb;
	return (a > b) - (a < b);
}

static void sort_unique_times(time_list *list)
{
	qsort(list->data, list->count, sizeof(double), &cmp_double);
	size_t dst = 0;
	for (size_t i = 0; i < list->count; i++) {
		if (dst > 0 && list->data[i] == list->data[dst - 1]) continue;
		list->data[dst++] = list->data[i];
	}
	list->count = dst;
}

// Animation endpoints, every baked keyframe time, at least twenty
// deterministic inter-frame times, plus caller supplied extras.
static void build_times(time_list *list, ufbx_scene *scene, ufbx_baked_anim *bake, const double *extra, size_t num_extra)
{
	double begin = scene->anim->time_begin;
	double end = scene->anim->time_end;
	push_time(list, begin);
	push_time(list, end);

	for (size_t i = 0; i < bake->nodes.count; i++) {
		ufbx_baked_node *bn = &bake->nodes.data[i];
		for (size_t k = 0; k < bn->translation_keys.count; k++) push_time(list, bn->translation_keys.data[k].time);
		for (size_t k = 0; k < bn->rotation_keys.count; k++) push_time(list, bn->rotation_keys.data[k].time);
		for (size_t k = 0; k < bn->scale_keys.count; k++) push_time(list, bn->scale_keys.data[k].time);
	}

	// 24 deterministic inter-frame times over the playback range ...
	for (int k = 0; k < 24; k++) {
		push_time(list, begin + (end - begin) * (double)(2 * k + 1) / 48.0);
	}
	// ... and 24 more concentrated on the keyed range if it is tighter.
	double key_begin = bake->key_time_min;
	double key_end = bake->key_time_max;
	if (key_begin > begin || key_end < end) {
		for (int k = 0; k < 24; k++) {
			push_time(list, key_begin + (key_end - key_begin) * (double)(2 * k + 1) / 48.0);
		}
	}

	for (size_t i = 0; i < num_extra; i++) push_time(list, extra[i]);

	sort_unique_times(list);
}

// -- Vector helpers ---------------------------------------------------------

static ufbx_vec3 make_vec3(ufbx_real x, ufbx_real y, ufbx_real z)
{
	ufbx_vec3 v;
	v.x = x;
	v.y = y;
	v.z = z;
	return v;
}

static double max_abs3(ufbx_vec3 v)
{
	double m = fabs((double)v.x);
	double dy = fabs((double)v.y);
	double dz = fabs((double)v.z);
	if (dy > m) m = dy;
	if (dz > m) m = dz;
	return m;
}

static double max_diff3(ufbx_vec3 a, ufbx_vec3 b)
{
	double m = fabs((double)a.x - (double)b.x);
	double dy = fabs((double)a.y - (double)b.y);
	double dz = fabs((double)a.z - (double)b.z);
	if (dy > m) m = dy;
	if (dz > m) m = dz;
	return m;
}

// -- Pose reconstruction ----------------------------------------------------

typedef struct {
	ufbx_scene *scene;
	ufbx_baked_anim *bake;
	// Baked node per typed_id, NULL for static nodes. bake->nodes and
	// scene->nodes are *not* parallel arrays, associate by typed_id only.
	ufbx_baked_node **baked_by_typed;
	// Optional replacement rotation keys per typed_id (quat sign-flip replay).
	ufbx_baked_quat_list *rotation_override;
	ufbx_matrix *world;
	bool *world_valid;
	double max_pos_err;
	double max_rot_err;
	size_t num_samples;
} pose_check;

static void pose_check_init(pose_check *pc, ufbx_scene *scene, ufbx_baked_anim *bake)
{
	memset(pc, 0, sizeof(*pc));
	pc->scene = scene;
	pc->bake = bake;
	size_t n = scene->nodes.count;
	pc->baked_by_typed = (ufbx_baked_node **)calloc(n, sizeof(ufbx_baked_node *));
	pc->world = (ufbx_matrix *)malloc(n * sizeof(ufbx_matrix));
	pc->world_valid = (bool *)malloc(n * sizeof(bool));
	if (!pc->baked_by_typed || !pc->world || !pc->world_valid) fail_internal("out of memory");
	for (size_t i = 0; i < bake->nodes.count; i++) {
		ufbx_baked_node *bn = &bake->nodes.data[i];
		if (bn->typed_id >= n) fail_internal("baked node typed_id out of range");
		pc->baked_by_typed[bn->typed_id] = bn;
	}
}

static void pose_check_free(pose_check *pc)
{
	free(pc->baked_by_typed);
	free(pc->world);
	free(pc->world_valid);
}

// Player-side world matrix: baked local TRS composed along the plain
// parent/child chain. Nodes missing from the bake keep their original
// local pose. Geometry transforms are *not* part of this chain.
static ufbx_matrix eval_node_world(pose_check *pc, uint32_t typed_id, double time)
{
	if (pc->world_valid[typed_id]) return pc->world[typed_id];
	ufbx_node *node = pc->scene->nodes.data[typed_id];

	ufbx_transform local_transform = node->local_transform;
	ufbx_baked_node *bn = pc->baked_by_typed[typed_id];
	if (bn) {
		if (bn->translation_keys.count > 0) {
			local_transform.translation = ufbx_evaluate_baked_vec3(bn->translation_keys, time);
		}
		ufbx_baked_quat_list rotation_keys = bn->rotation_keys;
		if (pc->rotation_override && pc->rotation_override[typed_id].count > 0) {
			rotation_keys = pc->rotation_override[typed_id];
		}
		if (rotation_keys.count > 0) {
			local_transform.rotation = ufbx_evaluate_baked_quat(rotation_keys, time);
		}
		if (bn->scale_keys.count > 0) {
			local_transform.scale = ufbx_evaluate_baked_vec3(bn->scale_keys, time);
		}
	}

	ufbx_matrix local = ufbx_transform_to_matrix(&local_transform);
	ufbx_matrix world = local;
	if (node->parent) {
		ufbx_matrix parent_world = eval_node_world(pc, node->parent->typed_id, time);
		world = ufbx_matrix_mul(&parent_world, &local);
	}

	pc->world[typed_id] = world;
	pc->world_valid[typed_id] = true;
	return world;
}

static void compare_matrix(pose_check *pc, ufbx_node *node, double time, double tolerance,
	ufbx_matrix world, const ufbx_matrix *ref, const char *pos_kind, const char *rot_kind)
{
	// Normalization: divide by the larger of 1 and the maximum absolute
	// reference coordinate.
	double pos_scale = fmax(1.0, max_abs3(ref->cols[3]));
	double pos_err = max_diff3(world.cols[3], ref->cols[3]) / pos_scale;

	// Rotation is compared through the basis vectors of the actual composed
	// transform, so q and -q (the same pose) compare equal by construction.
	double rot_scale = 1.0;
	double rot_err = 0.0;
	for (int c = 0; c < 3; c++) {
		double s = max_abs3(ref->cols[c]);
		if (s > rot_scale) rot_scale = s;
		double e = max_diff3(world.cols[c], ref->cols[c]);
		if (e > rot_err) rot_err = e;
	}
	rot_err /= rot_scale;

	if (pos_err > pc->max_pos_err) pc->max_pos_err = pos_err;
	if (rot_err > pc->max_rot_err) pc->max_rot_err = rot_err;

	// `!(e <= limit)` so that NaN errors also fail.
	if (!(pos_err <= tolerance)) fail_num(node->name.data, node->typed_id, time, pos_kind, pos_err, tolerance);
	if (!(rot_err <= tolerance)) fail_num(node->name.data, node->typed_id, time, rot_kind, rot_err, tolerance);
}

static void compare_node(pose_check *pc, ufbx_scene *ref_scene, uint32_t typed_id, double time, double tolerance)
{
	ufbx_node *node = pc->scene->nodes.data[typed_id];
	ufbx_node *ref_node = ref_scene->nodes.data[typed_id];
	if (ref_node->typed_id != typed_id) fail_internal("typed_id association mismatch");

	ufbx_matrix world = eval_node_world(pc, typed_id, time);
	compare_matrix(pc, node, time, tolerance, world, &ref_node->node_to_world, "position", "rotation-basis");

	// geometry_to_node affects only the geometry attached to this node, it
	// must not leak into the transforms of the children above.
	if (node->attrib) {
		ufbx_matrix geo_world = ufbx_matrix_mul(&world, &node->geometry_to_node);
		compare_matrix(pc, node, time, tolerance, geo_world, &ref_node->geometry_to_world,
			"geometry-position", "geometry-basis");
	}
}

// Sample every time forwards and then again in reverse order so that
// order-dependent evaluation caches cannot mask mismatches.
static void run_times(pose_check *pc, const time_list *times, double tolerance)
{
	size_t n = times->count;
	for (int pass = 0; pass < 2; pass++) {
		for (size_t ix = 0; ix < n; ix++) {
			double time = times->data[pass == 0 ? ix : n - 1 - ix];
			ufbx_error error;
			ufbx_scene *ref = ufbx_evaluate_scene(pc->scene, pc->scene->anim, time, NULL, &error);
			if (!ref) fail_internal(error.description.data);
			memset(pc->world_valid, 0, pc->scene->nodes.count * sizeof(bool));
			for (uint32_t typed_id = 0; typed_id < pc->scene->nodes.count; typed_id++) {
				compare_node(pc, ref, typed_id, time, tolerance);
			}
			ufbx_free_scene(ref);
			pc->num_samples++;
		}
	}
}

// -- Scene / bake helpers ---------------------------------------------------

static ufbx_scene *load_scene(const char *path, const ufbx_load_opts *opts)
{
	ufbx_error error;
	ufbx_scene *scene = ufbx_load_file(path, opts, &error);
	if (!scene) fail_internal(error.description.data);
	return scene;
}

static ufbx_baked_anim *bake_scene(ufbx_scene *scene, ufbx_bake_step_handling step_handling)
{
	ufbx_bake_opts opts = { 0 };
	// Bake continuous segments at 240 Hz. `max_keyframe_segments` must be
	// raised from the default (32) as well: the source files have keys
	// seconds apart, and the per-segment segment cap would otherwise scale
	// the effective sample rate back down to ~30 Hz, leaving cubic curves
	// with a ~1e-2 linearization error.
	opts.resample_rate = 240.0;
	opts.minimum_sample_rate = 240.0;
	opts.max_keyframe_segments = 16384;
	opts.step_handling = step_handling;
	ufbx_error error;
	ufbx_baked_anim *bake = ufbx_bake_anim(scene, scene->anim, &opts, &error);
	if (!bake) fail_internal(error.description.data);
	return bake;
}

static void report_file(const char *path, const char *bake_name, const pose_check *pc)
{
	printf("FILE %s bake=%s nodes=%zu/%zu samples=%zu max_pos_err=%.3e max_rot_err=%.3e\n",
		path, bake_name, pc->bake->nodes.count, pc->scene->nodes.count,
		pc->num_samples, pc->max_pos_err, pc->max_rot_err);
}

// -- Instantaneous jump sample ----------------------------------------------
//
// data/maya_anim_linear_7700_ascii.fbx has a stepped key at t=0.5: z jumps
// to 2 at exactly t=0.5 while the double times immediately before and after
// evaluate to 0. Verify the baked key flags, that key times are stored as
// unmodified doubles, and that public sampling agrees with the flags and
// with independent scene evaluation.

static void check_step_flags(ufbx_baked_vec3 key, ufbx_baked_key_flags flags, double z, double time)
{
	if ((key.flags & flags) != flags) {
		fail_num("pCube1", 1, time, "step-flags", (double)(key.flags ^ flags), 0.0);
	}
	if (!(fabs((double)key.value.z - z) <= STEP_TOLERANCE)) {
		fail_num("pCube1", 1, time, "step-key-value", fabs((double)key.value.z - z), STEP_TOLERANCE);
	}
}

static void check_step_bake(ufbx_scene *scene, ufbx_baked_anim *bake, ufbx_bake_step_handling step_handling)
{
	if (bake->nodes.count != 1) fail_internal("expected exactly one baked node");
	ufbx_baked_node *bn = &bake->nodes.data[0];
	ufbx_baked_vec3_list keys = bn->translation_keys;

	size_t at = SIZE_MAX;
	for (size_t i = 0; i < keys.count; i++) {
		if (keys.data[i].time == 0.5) { at = i; break; }
	}
	if (at == SIZE_MAX) fail_internal("no baked key at t=0.5");

	if (step_handling == UFBX_BAKE_STEP_HANDLING_IDENTICAL_TIME) {
		// Left limit, key and right limit share the exact same time.
		if (at + 2 >= keys.count) fail_internal("missing identical-time step keys");
		if (keys.data[at + 1].time != 0.5 || keys.data[at + 2].time != 0.5) {
			fail_internal("identical-time step keys do not share t=0.5");
		}
		check_step_flags(keys.data[at + 0], UFBX_BAKED_KEY_STEP_LEFT, 0.0, 0.5);
		check_step_flags(keys.data[at + 1], UFBX_BAKED_KEY_KEYFRAME | UFBX_BAKED_KEY_STEP_KEY, 2.0, 0.5);
		check_step_flags(keys.data[at + 2], UFBX_BAKED_KEY_STEP_RIGHT, 0.0, 0.5);
	} else {
		// The step key sits between the previous/next representable doubles.
		if (at == 0 || at + 1 >= keys.count) fail_internal("missing adjacent-double step keys");
		ufbx_baked_vec3 left = keys.data[at - 1];
		ufbx_baked_vec3 key = keys.data[at];
		ufbx_baked_vec3 right = keys.data[at + 1];
		// Times must be exact doubles: rounding to frames or float would
		// collapse all three to 0.5.
		if (left.time != nextafter(0.5, -INFINITY) || right.time != nextafter(0.5, INFINITY)) {
			fail_num("pCube1", 1, 0.5, "step-time-not-adjacent-double",
				fmax(fabs(left.time - 0.5), fabs(right.time - 0.5)), 0.0);
		}
		check_step_flags(left, UFBX_BAKED_KEY_STEP_LEFT, 0.0, left.time);
		check_step_flags(key, UFBX_BAKED_KEY_KEYFRAME | UFBX_BAKED_KEY_STEP_KEY, 2.0, key.time);
		check_step_flags(right, UFBX_BAKED_KEY_STEP_RIGHT, 0.0, right.time);
	}

	// Correspondence between the baked flags and public sampling: sampling
	// at the exact double times must reproduce the flagged values, and must
	// agree with an independent scene evaluation at the same times.
	double times[3] = { nextafter(0.5, -INFINITY), 0.5, nextafter(0.5, INFINITY) };
	double expect_z[3] = { 0.0, 2.0, 0.0 };
	for (size_t i = 0; i < 3; i++) {
		ufbx_vec3 v = ufbx_evaluate_baked_vec3(keys, times[i]);
		if (!(fabs((double)v.z - expect_z[i]) <= STEP_TOLERANCE)) {
			fail_num("pCube1", bn->typed_id, times[i], "step-sample",
				fabs((double)v.z - expect_z[i]), STEP_TOLERANCE);
		}
		ufbx_error error;
		ufbx_scene *ref = ufbx_evaluate_scene(scene, scene->anim, times[i], NULL, &error);
		if (!ref) fail_internal(error.description.data);
		double ref_z = (double)ref->nodes.data[bn->typed_id]->local_transform.translation.z;
		ufbx_free_scene(ref);
		if (!(fabs((double)v.z - ref_z) <= STEP_TOLERANCE)) {
			fail_num("pCube1", bn->typed_id, times[i], "step-vs-scene",
				fabs((double)v.z - ref_z), STEP_TOLERANCE);
		}
	}
}

// -- Per-file checks ----------------------------------------------------------

static void check_linear_file(void)
{
	const char *path = "data/maya_anim_linear_7700_ascii.fbx";
	g_file = path;
	ufbx_scene *scene = load_scene(path, NULL);

	static const struct {
		ufbx_bake_step_handling step_handling;
		const char *name;
	} modes[] = {
		{ UFBX_BAKE_STEP_HANDLING_IDENTICAL_TIME, "identical-time" },
		{ UFBX_BAKE_STEP_HANDLING_ADJACENT_DOUBLE, "adjacent-double" },
	};

	for (size_t m = 0; m < sizeof(modes) / sizeof(*modes); m++) {
		g_phase = modes[m].name;
		ufbx_baked_anim *bake = bake_scene(scene, modes[m].step_handling);

		check_step_bake(scene, bake, modes[m].step_handling);

		pose_check pc;
		pose_check_init(&pc, scene, bake);

		double jump_times[3] = { nextafter(0.5, -INFINITY), 0.5, nextafter(0.5, INFINITY) };
		time_list times = { 0 };
		build_times(&times, scene, bake, jump_times, 3);
		run_times(&pc, &times, POSE_TOLERANCE);

		// The jump neighborhood is exact keyframe data: re-check it tightly.
		time_list exact = { 0 };
		for (size_t i = 0; i < 3; i++) push_time(&exact, jump_times[i]);
		run_times(&pc, &exact, STEP_TOLERANCE);

		report_file(path, modes[m].name, &pc);

		free(times.data);
		free(exact.data);
		pose_check_free(&pc);
		ufbx_free_baked_anim(bake);
	}

	ufbx_free_scene(scene);
}

static ufbx_quat quat_negate(ufbx_quat q)
{
	q.x = -q.x;
	q.y = -q.y;
	q.z = -q.z;
	q.w = -q.w;
	return q;
}

// Synthetic public-input fixture for equivalent negated quaternions.
// The existing assets bake rotation keys that are already sign-continuous,
// so they cannot catch a broken q == -q handling in interpolation. Here the
// t=1 key (90 deg about X) is stored negated -- the exact same pose -- and
// the sampled rotations are compared against independently computed basis
// vectors (rotation by t*90 deg about X, plain double trigonometry).
static void check_quat_sign_fixture(void)
{
	g_file = "<synthetic:quat-sign>";
	g_phase = "quat-sign-fixture";

	const ufbx_real half = (ufbx_real)sqrt(0.5);
	ufbx_baked_quat keys[2];
	keys[0].time = 0.0;
	keys[0].value.x = 0.0f; keys[0].value.y = 0.0f; keys[0].value.z = 0.0f; keys[0].value.w = 1.0f;
	keys[0].flags = UFBX_BAKED_KEY_KEYFRAME;
	keys[1].time = 1.0;
	keys[1].value.x = -half; keys[1].value.y = 0.0f; keys[1].value.z = 0.0f; keys[1].value.w = -half;
	keys[1].flags = UFBX_BAKED_KEY_KEYFRAME;
	ufbx_baked_quat_list list = { keys, 2 };

	static const double sample_t[3] = { 0.25, 0.5, 0.75 };
	double max_err = 0.0;
	for (size_t i = 0; i < 3; i++) {
		double time = sample_t[i];
		double angle = time * (3.14159265358979323846 / 2.0);
		double c = cos(angle), s = sin(angle);
		// Independent expectation: basis vectors of a rotation by `angle` about X.
		ufbx_vec3 expect[3] = {
			make_vec3(1, 0, 0),
			make_vec3(0, (ufbx_real)c, (ufbx_real)s),
			make_vec3(0, (ufbx_real)-s, (ufbx_real)c),
		};
		ufbx_quat q = ufbx_evaluate_baked_quat(list, time);
		for (int b = 0; b < 3; b++) {
			ufbx_vec3 got = ufbx_quat_rotate_vec3(q, make_vec3(b == 0, b == 1, b == 2));
			double err = max_diff3(got, expect[b]) / fmax(1.0, max_abs3(expect[b]));
			if (err > max_err) max_err = err;
			if (!(err <= POSE_TOLERANCE)) {
				fail_num("<fixture>", UINT32_MAX, time, "quat-sign-basis", err, POSE_TOLERANCE);
			}
		}
	}
	printf("FILE %s bake=%s nodes=1/1 samples=%d max_pos_err=%.3e max_rot_err=%.3e\n",
		g_file, g_phase, 3, 0.0, max_err);
}

static void check_layers_file(void)
{
	const char *path = "data/maya_anim_layers_over_acc_orders_7500_ascii.fbx";
	g_file = path;
	ufbx_scene *scene = load_scene(path, NULL);
	ufbx_baked_anim *bake = bake_scene(scene, UFBX_BAKE_STEP_HANDLING_ADJACENT_DOUBLE);

	time_list times = { 0 };
	build_times(&times, scene, bake, NULL, 0);

	g_phase = "layered-rotation";
	pose_check pc;
	pose_check_init(&pc, scene, bake);
	run_times(&pc, &times, POSE_TOLERANCE);
	report_file(path, g_phase, &pc);
	pose_check_free(&pc);

	// Quat sign-flip replay: negating any rotation key stores the exact same
	// pose (q == -q), so a player-side store may legitimately contain mixed
	// signs. Interpolation between the keys must still produce the same
	// world poses.
	g_phase = "quat-sign-replay";
	size_t num_nodes = scene->nodes.count;
	ufbx_baked_quat_list *flipped = (ufbx_baked_quat_list *)calloc(num_nodes, sizeof(ufbx_baked_quat_list));
	if (!flipped) fail_internal("out of memory");
	for (size_t i = 0; i < bake->nodes.count; i++) {
		ufbx_baked_node *bn = &bake->nodes.data[i];
		size_t count = bn->rotation_keys.count;
		ufbx_baked_quat *keys = (ufbx_baked_quat *)malloc(count * sizeof(ufbx_baked_quat));
		if (!keys) fail_internal("out of memory");
		for (size_t k = 0; k < count; k++) {
			keys[k] = bn->rotation_keys.data[k];
			if (k % 2 == 1) keys[k].value = quat_negate(keys[k].value);
		}
		flipped[bn->typed_id].data = keys;
		flipped[bn->typed_id].count = count;
	}

	pose_check pc_flip;
	pose_check_init(&pc_flip, scene, bake);
	pc_flip.rotation_override = flipped;
	run_times(&pc_flip, &times, POSE_TOLERANCE);
	report_file(path, g_phase, &pc_flip);
	pose_check_free(&pc_flip);

	for (size_t i = 0; i < num_nodes; i++) free(flipped[i].data);
	free(flipped);
	free(times.data);
	ufbx_free_baked_anim(bake);
	ufbx_free_scene(scene);
}

static void check_no_inherit_scale_file(void)
{
	const char *path = "data/maya_anim_no_inherit_scale_7700_ascii.fbx";
	g_file = path;

	// Use the library helper-node mode so that a player can rebuild the
	// result along the plain parent/child matrix chain; no custom FBX
	// inherit rules on the player side.
	ufbx_load_opts load_opts = { 0 };
	load_opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
	ufbx_scene *scene = load_scene(path, &load_opts);
	ufbx_baked_anim *bake = bake_scene(scene, UFBX_BAKE_STEP_HANDLING_ADJACENT_DOUBLE);

	g_phase = "helper-node-scale";
	pose_check pc;
	pose_check_init(&pc, scene, bake);

	time_list times = { 0 };
	build_times(&times, scene, bake, NULL, 0);
	run_times(&pc, &times, POSE_TOLERANCE);
	report_file(path, g_phase, &pc);

	free(times.data);
	pose_check_free(&pc);
	ufbx_free_baked_anim(bake);
	ufbx_free_scene(scene);
}

int main(void)
{
	check_linear_file();
	check_quat_sign_fixture();
	check_layers_file();
	check_no_inherit_scale_file();
	printf("PASS baked_pose: all reconstructed poses within tolerance\n");
	return 0;
}
