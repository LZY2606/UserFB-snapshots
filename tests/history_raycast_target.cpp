// Regression tests for the interaction between background compaction (bake),
// undo/redo history, save/load and cached raycast contexts.
//
// Registered with the task driver via task_cases/task_case_count (no main here).
#include "task_support/check.h"

#include "storage.h"
#include "raytracing.h"

#include <cmath>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

using namespace Cubiquity;

namespace
{
	struct VoxelRef { i32 x, y, z; MaterialId mat; };

	bool voxelsMatch(const Volume& volume, const VoxelRef* refs, size_t count)
	{
		for (size_t i = 0; i < count; i++) {
			if (volume.voxel(refs[i].x, refs[i].y, refs[i].z) != refs[i].mat) {
				return false;
			}
		}
		return true;
	}

	template <size_t N>
	bool voxelsMatch(const Volume& volume, const VoxelRef (&refs)[N])
	{
		return voxelsMatch(volume, refs, N);
	}

	// Walks the real node graph from a root, verifying that every referenced
	// node index is valid, and collecting the set of reachable nodes.
	bool validateRoot(const Internals::NodeStore& nodes, u32 id, int depth, std::unordered_set<u32>& seen)
	{
		if (id < Internals::MaterialCount) { return true; }
		if (id >= nodes.unsharedNodesEnd() || depth > 32) { return false; }
		if (!seen.insert(id).second) { return true; } // Already visited (DAG)
		for (u32 child : nodes[id]) {
			if (!validateRoot(nodes, child, depth + 1, seen)) { return false; }
		}
		return true;
	}

	std::string tempPath(const char* filename)
	{
		return (std::filesystem::temp_directory_path() / filename).string();
	}

	////////////////////////////////////////////////////////////////////////////////
	// history_compaction
	////////////////////////////////////////////////////////////////////////////////

	int case_bake_preserves_full_history()
	{
		Volume v;
		v.setVoxel(1, 2, 3, 7);
		v.setVoxel(-5, -6, -7, 2);
		v.checkpoint();                       // v0
		v.setVoxel(1, 2, 3, 9);
		v.checkpoint();                       // v1
		v.setVoxel(8, 8, 8, 4);
		v.setVoxel(1, 2, 3, 0);               // erase
		// current (v2)

		const VoxelRef expectV0[] = { {1,2,3,7}, {-5,-6,-7,2}, {8,8,8,0} };
		const VoxelRef expectV1[] = { {1,2,3,9}, {-5,-6,-7,2}, {8,8,8,0} };
		const VoxelRef expectV2[] = { {1,2,3,0}, {-5,-6,-7,2}, {8,8,8,4} };

		v.bake();

		// All snapshots (and the current position) survive compaction.
		TASK_CHECK(voxelsMatch(v, expectV2));
		v.undo(); TASK_CHECK(voxelsMatch(v, expectV1));
		v.undo(); TASK_CHECK(voxelsMatch(v, expectV0));
		v.redo(); TASK_CHECK(voxelsMatch(v, expectV1));
		v.redo(); TASK_CHECK(voxelsMatch(v, expectV2));

		// Baking from a reverted state must not lose the future either.
		v.undo(); v.undo();
		v.bake();
		TASK_CHECK(voxelsMatch(v, expectV0));
		v.redo(); TASK_CHECK(voxelsMatch(v, expectV1));
		v.redo(); TASK_CHECK(voxelsMatch(v, expectV2));
		v.undo(); v.undo(); TASK_CHECK(voxelsMatch(v, expectV0));
		v.redo(); v.redo(); TASK_CHECK(voxelsMatch(v, expectV2));
		return 0;
	}

	int case_branch_rules_after_compaction()
	{
		Volume v;
		v.setVoxel(10, 10, 10, 5); v.checkpoint();  // v0
		v.setVoxel(10, 10, 10, 6); v.checkpoint();  // v1
		v.setVoxel(10, 10, 10, 7);                  // v2 (current)
		v.bake();

		// Undo twice then edit: by the existing branch rules the edit modifies
		// the reverted snapshot in place, and checkpointing discards the redo
		// tail (the '6' and '7' futures).
		v.undo(); v.undo();
		TASK_CHECK(v.voxel(10, 10, 10) == 5);
		v.setVoxel(10, 10, 10, 42);
		v.checkpoint();
		TASK_CHECK(v.voxel(10, 10, 10) == 42);
		v.undo(); TASK_CHECK(v.voxel(10, 10, 10) == 42); // Slot overwritten by the edit
		v.redo(); TASK_CHECK(v.voxel(10, 10, 10) == 42);
		v.redo(); TASK_CHECK(v.voxel(10, 10, 10) == 42); // Old future is gone.

		// Editing the current snapshot must not pollute the other snapshot.
		v.setVoxel(20, 20, 20, 9);
		v.undo();
		TASK_CHECK(v.voxel(10, 10, 10) == 42);
		TASK_CHECK(v.voxel(20, 20, 20) == 0);
		v.redo();
		TASK_CHECK(v.voxel(10, 10, 10) == 42);
		TASK_CHECK(v.voxel(20, 20, 20) == 9);
		return 0;
	}

	int case_sharing_and_root_reachability()
	{
		Volume v;
		v.setVoxel(100, 100, 100, 3);
		v.setVoxel(104, 100, 100, 4);
		v.setVoxel(100, 104, 100, 5);
		v.checkpoint();
		v.setVoxel(100, 100, 104, 6);
		v.checkpoint();
		v.setVoxel(108, 100, 100, 7);
		v.bake();

		const Internals::NodeStore& nodes = Internals::getNodes(v);

		// Collect the roots of all three retained snapshots.
		std::vector<u32> roots(3);
		roots[2] = v.rootNodeIndex();
		v.undo(); roots[1] = v.rootNodeIndex();
		v.undo(); roots[0] = v.rootNodeIndex();
		v.redo(); v.redo();

		// Every retained root must reference a valid graph inside the store.
		std::unordered_set<u32> unionNodes;
		size_t individualSum = 0;
		for (u32 root : roots) {
			std::unordered_set<u32> seen;
			TASK_CHECK(validateRoot(nodes, root, 0, seen));
			individualSum += seen.size();
			unionNodes.insert(seen.begin(), seen.end());
		}

		// The snapshots build on each other, so after compaction they must
		// share nodes rather than each holding a full copy of the volume.
		TASK_CHECK(unionNodes.size() < individualSum);

		// Compaction must not leave unreachable nodes behind: every node in
		// the store is reachable from at least one of the retained roots.
		TASK_CHECK(unionNodes.size() + Internals::MaterialCount == nodes.unsharedNodesEnd());

		// Identical snapshots collapse onto the exact same compressed root.
		Volume v2;
		v2.setVoxel(50, 50, 50, 8);
		v2.checkpoint();
		v2.checkpoint();
		v2.bake();
		u32 r2 = v2.rootNodeIndex();
		v2.undo(); u32 r1 = v2.rootNodeIndex();
		v2.undo(); u32 r0 = v2.rootNodeIndex();
		TASK_CHECK(r0 == r1 && r1 == r2);
		TASK_CHECK(v2.voxel(50, 50, 50) == 8);
		return 0;
	}

	int case_empty_and_material_roots()
	{
		Volume v;                             // Empty (root is material 0)
		v.checkpoint();                       // v0: empty
		v.fill(9);                            // Pure material root
		v.checkpoint();                       // v1: all 9
		v.setVoxel(3, 3, 3, 4);
		v.checkpoint();                       // v2: 9 with a voxel of 4
		v.fill(0);                            // v3: empty again
		v.bake();

		TASK_CHECK(v.voxel(3, 3, 3) == 0);
		v.undo(); TASK_CHECK(v.voxel(3, 3, 3) == 4 && v.voxel(4, 4, 4) == 9);
		v.undo(); TASK_CHECK(v.voxel(3, 3, 3) == 9 && v.voxel(-1000, 1000, -1000) == 9);
		v.undo(); TASK_CHECK(v.voxel(3, 3, 3) == 0);
		v.redo(); v.redo(); v.redo();
		TASK_CHECK(v.voxel(3, 3, 3) == 0);

		// Empty and pure-material scenes also round-trip through save/load.
		const std::string emptyPath = tempPath("cubiquity_test_empty.dag");
		const std::string filledPath = tempPath("cubiquity_test_filled.dag");
		v.save(emptyPath);
		Volume w;
		TASK_CHECK(w.load(emptyPath));
		TASK_CHECK(w.voxel(3, 3, 3) == 0);

		Volume v2;
		v2.fill(7);
		v2.save(filledPath);
		Volume w2;
		TASK_CHECK(w2.load(filledPath));
		TASK_CHECK(w2.voxel(11, 11, 11) == 7);

		std::filesystem::remove(emptyPath);
		std::filesystem::remove(filledPath);
		return 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// copy_on_write_io
	////////////////////////////////////////////////////////////////////////////////

	int case_save_keeps_history_and_writes_current_scene()
	{
		const std::string path = tempPath("cubiquity_test_io_scene.dag");
		const std::string freshPath = tempPath("cubiquity_test_io_fresh.dag");

		Volume v;
		v.setVoxel(1, 2, 3, 7); v.checkpoint();
		v.setVoxel(1, 2, 3, 9); v.setVoxel(4, 5, 6, 3); v.checkpoint();
		v.setVoxel(10, 10, 10, 5);
		v.save(path);

		// The implicit compaction in save() must not destroy in-memory history.
		v.undo();
		TASK_CHECK(v.voxel(1, 2, 3) == 9 && v.voxel(4, 5, 6) == 3 && v.voxel(10, 10, 10) == 0);
		v.undo();
		TASK_CHECK(v.voxel(1, 2, 3) == 7 && v.voxel(4, 5, 6) == 0);
		v.redo(); v.redo();
		TASK_CHECK(v.voxel(1, 2, 3) == 9 && v.voxel(4, 5, 6) == 3 && v.voxel(10, 10, 10) == 5);

		// The file holds only the current scene, in the original layout: it
		// must match a fresh volume containing just that scene.
		Volume fresh;
		fresh.setVoxel(1, 2, 3, 9); fresh.setVoxel(4, 5, 6, 3); fresh.setVoxel(10, 10, 10, 5);
		fresh.save(freshPath);
		TASK_CHECK(std::filesystem::file_size(path) == std::filesystem::file_size(freshPath));

		Volume w;
		TASK_CHECK(w.load(path));
		TASK_CHECK(w.voxel(1, 2, 3) == 9 && w.voxel(4, 5, 6) == 3 && w.voxel(10, 10, 10) == 5);
		TASK_CHECK(w.voxel(1, 2, 4) == 0 && w.voxel(11, 10, 10) == 0);

		std::filesystem::remove(path);
		std::filesystem::remove(freshPath);
		return 0;
	}

	int case_load_replaces_history_and_failed_load_keeps_state()
	{
		const std::string path = tempPath("cubiquity_test_io_load.dag");
		const std::string missing = tempPath("cubiquity_test_missing_12345.dag");

		Volume v;
		v.setVoxel(1, 1, 1, 5); v.checkpoint();
		v.setVoxel(2, 2, 2, 6);
		v.save(path);

		Volume w;
		TASK_CHECK(w.load(path));
		TASK_CHECK(w.voxel(1, 1, 1) == 5 && w.voxel(2, 2, 2) == 6);

		// A loaded volume starts a fresh history containing only the loaded
		// scene; there is nothing to undo back to.
		w.undo();
		TASK_CHECK(w.voxel(1, 1, 1) == 5 && w.voxel(2, 2, 2) == 6);
		w.setVoxel(3, 3, 3, 7);
		w.undo();
		TASK_CHECK(w.voxel(3, 3, 3) == 7);
		w.redo();
		TASK_CHECK(w.voxel(3, 3, 3) == 7);

		// A failed load leaves the scene (and history) untouched.
		TASK_CHECK(!w.load(missing));
		TASK_CHECK(w.voxel(3, 3, 3) == 7 && w.voxel(1, 1, 1) == 5);

		TASK_CHECK(!v.load(missing));
		v.undo();
		TASK_CHECK(v.voxel(2, 2, 2) == 0 && v.voxel(1, 1, 1) == 5);
		v.redo();
		TASK_CHECK(v.voxel(2, 2, 2) == 6);

		std::filesystem::remove(path);
		return 0;
	}

	int case_edits_are_copy_on_write()
	{
		Volume v;
		v.setVoxel(1, 1, 1, 5); v.checkpoint();
		v.setVoxel(2, 2, 2, 6); v.checkpoint();
		v.setVoxel(3, 3, 3, 7);
		v.bake();

		// Editing the current snapshot after compaction must not modify the
		// nodes shared with the other snapshots.
		v.setVoxel(4, 4, 4, 8);
		v.undo();
		TASK_CHECK(v.voxel(4, 4, 4) == 0 && v.voxel(3, 3, 3) == 0 && v.voxel(2, 2, 2) == 6);
		v.undo();
		TASK_CHECK(v.voxel(2, 2, 2) == 0 && v.voxel(1, 1, 1) == 5);
		v.redo(); v.redo();
		TASK_CHECK(v.voxel(4, 4, 4) == 8 && v.voxel(3, 3, 3) == 7);

		// combine() follows the same branch rules.
		Volume a;
		a.setVoxel(5, 5, 5, 1); a.checkpoint();
		Volume b;
		b.setVoxel(5, 5, 5, 2); b.setVoxel(6, 6, 6, 3);
		a.combine(b, [](MaterialId x, MaterialId y) { return y ? y : x; });
		TASK_CHECK(a.voxel(5, 5, 5) == 2 && a.voxel(6, 6, 6) == 3);
		a.undo();
		TASK_CHECK(a.voxel(5, 5, 5) == 1 && a.voxel(6, 6, 6) == 0);
		a.redo();
		a.bake();
		TASK_CHECK(a.voxel(5, 5, 5) == 2 && a.voxel(6, 6, 6) == 3);
		a.undo();
		TASK_CHECK(a.voxel(5, 5, 5) == 1 && a.voxel(6, 6, 6) == 0);
		return 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// raycast_epochs
	////////////////////////////////////////////////////////////////////////////////

	struct TestVoxel { i32 x, y, z; MaterialId mat; };

	// Sparse scene with voxels spread over multiple non-zero branches. Each
	// occupied octant holds at least two voxels so that every SubDAG is an
	// internal node (a SubDAG which is a single material node is outside the
	// scope of the existing ESVO raycaster).
	const TestVoxel kSceneVoxels[] = {
		{  100,  100,  100,  3 },
		{  150,  120,  110, 13 },
		{ -100,  100,  100,  4 },
		{ -150,  120,   90, 14 },
		{  100, -100, -100,  5 },
		{  140, -130, -120, 15 },
		{ -100, -100, -100,  6 },
		{ -120, -140, -110, 16 },
		{   30,   40,  -60,  7 },
		{   50,   60,  -80, 17 },
	};

	void buildScene(Volume& v)
	{
		for (const TestVoxel& tv : kSceneVoxels) {
			v.setVoxel(tv.x, tv.y, tv.z, tv.mat);
		}
	}

	// Independent slab-method intersection of a ray with a voxel box.
	double slabEntry(const TestVoxel& tv, const vec3& origin, const vec3& dir)
	{
		const i32 p[3] = { tv.x, tv.y, tv.z };
		double tEnter = -1e30, tExit = 1e30;
		for (int axis = 0; axis < 3; axis++) {
			double t0 = ((p[axis] - 0.5) - origin[axis]) / dir[axis];
			double t1 = ((p[axis] + 0.5) - origin[axis]) / dir[axis];
			if (t0 > t1) { std::swap(t0, t1); }
			tEnter = std::max(tEnter, t0);
			tExit = std::min(tExit, t1);
		}
		return tEnter < tExit ? tEnter : -1.0;
	}

	int case_hits_match_independent_reference()
	{
		Volume v;
		buildScene(v);
		v.checkpoint();
		v.setVoxel(0, 0, 0, 9); // Extra branch near the origin
		v.bake();

		RaycastContext context;

		for (const TestVoxel& tv : kSceneVoxels) {
			// Cast along +x towards the voxel (and along -x for one of them).
			for (float sign : { 1.0f, -1.0f }) {
				vec3 origin(float(tv.x) - sign * 20.0f, float(tv.y), float(tv.z));
				vec3 dir(sign, 0.01f, 0.02f);
				Ray3f ray(origin, dir);

				RayVolumeIntersection hit = intersectVolume(v, context, ray, true);
				RayVolumeIntersection ref = traceRayRef(v, ray); // Independent box traversal
				double expected = slabEntry(tv, origin, dir);

				TASK_CHECK(hit.hit && ref.hit);
				TASK_CHECK(hit.material == tv.mat);
				TASK_CHECK(ref.material == tv.mat);
				TASK_CHECK(expected > 0.0);
				TASK_CHECK(std::fabs(hit.distance - expected) < 0.05);
				TASK_CHECK(std::fabs(ref.distance - expected) < 0.05);
				TASK_CHECK(std::fabs(hit.distance - ref.distance) < 0.05);
			}
		}

		// A ray pointing away from everything misses in both implementations.
		Ray3f missRay(vec3(0.0f, 5000.0f, 0.0f), vec3(0.01f, 1.0f, 0.02f));
		TASK_CHECK(!intersectVolume(v, context, missRay, true).hit);
		TASK_CHECK(!traceRayRef(v, missRay).hit);
		return 0;
	}

	int case_context_caching_and_epochs()
	{
		Volume v;
		v.setVoxel(100, 100, 100, 3);
		v.setVoxel(150, 120, 110, 11); // Second voxel keeps the SubDAG internal
		v.setVoxel(-100, -100, -100, 4);
		v.setVoxel(-150, -120, -110, 12);

		RaycastContext context;
		Ray3f ray1(vec3(80.0f, 100.0f, 100.0f), vec3(1.0f, 0.01f, 0.02f));

		// The first query prepares the cache exactly once.
		RayVolumeIntersection hit = intersectVolume(v, context, ray1, true);
		TASK_CHECK(hit.hit && hit.material == 3);
		TASK_CHECK(context.preparationCount() == 1);

		// Queries on the unmodified volume reuse the cache (both overloads).
		intersectVolume(v, context, ray1, true);
		intersectVolume(v, context, 80.0f, 100.0f, 100.0f, 1.0f, 0.01f, 0.02f, true);
		TASK_CHECK(context.preparationCount() == 1);

		// A burst of edits is coalesced into a single re-preparation.
		v.setVoxel(50, 50, 50, 5);
		v.setVoxel(-50, 50, -50, 6);
		v.setVoxel(-70, 60, -80, 13); // Second voxel in that octant
		v.checkpoint();
		v.setVoxel(100, 100, 100, 7);
		hit = intersectVolume(v, context, ray1, true);
		TASK_CHECK(hit.hit && hit.material == 7);
		TASK_CHECK(context.preparationCount() == 2);

		// undo/redo change the visible scene, so each query re-prepares.
		v.undo();
		hit = intersectVolume(v, context, ray1, true);
		TASK_CHECK(hit.hit && hit.material == 3);
		TASK_CHECK(context.preparationCount() == 3);
		v.redo();
		hit = intersectVolume(v, context, ray1, true);
		TASK_CHECK(hit.hit && hit.material == 7);
		TASK_CHECK(context.preparationCount() == 4);

		// Compaction reshuffles node indices (possibly keeping the same root
		// index), but the revision bump still invalidates the cache.
		v.bake();
		hit = intersectVolume(v, context, ray1, true);
		TASK_CHECK(hit.hit && hit.material == 7);
		TASK_CHECK(context.preparationCount() == 5);

		// A second context maintains its own independent cache.
		RaycastContext context2;
		intersectVolume(v, context2, ray1, true);
		TASK_CHECK(context2.preparationCount() == 1);
		TASK_CHECK(context.preparationCount() == 5);

		// Switching a context to another volume rebinds it.
		Volume v2;
		v2.setVoxel(-100, -100, -100, 8);
		v2.setVoxel(-150, -120, -110, 12); // Second voxel in that octant
		Ray3f ray2(vec3(-80.0f, -100.0f, -100.0f), vec3(-1.0f, 0.01f, 0.02f));
		RayVolumeIntersection hit2 = intersectVolume(v2, context2, ray2, true);
		TASK_CHECK(hit2.hit && hit2.material == 8);
		TASK_CHECK(context2.preparationCount() == 2);
		intersectVolume(v2, context2, ray2, true);
		TASK_CHECK(context2.preparationCount() == 2);
		intersectVolume(v, context2, ray1, true); // Switch back
		TASK_CHECK(context2.preparationCount() == 3);

		// save() compacts implicitly and load() replaces the scene; both are
		// visible to the context through the revision counter.
		const std::string path = tempPath("cubiquity_test_raycast.dag");
		v.save(path);
		intersectVolume(v, context, ray1, true);
		TASK_CHECK(context.preparationCount() == 6);
		Volume v3;
		TASK_CHECK(v3.load(path));
		RayVolumeIntersection hit3 = intersectVolume(v3, context, ray1, true);
		TASK_CHECK(hit3.hit && hit3.material == 7);
		TASK_CHECK(context.preparationCount() == 7);

		std::filesystem::remove(path);
		return 0;
	}

	int case_legacy_manual_subdag_interface()
	{
		Volume v;
		v.setVoxel(1, 2, 3, 7);
		v.setVoxel(5, 6, 7, 8);         // Second voxel keeps the SubDAG internal
		v.setVoxel(-8, -9, -10, 4);
		v.setVoxel(-20, -24, -28, 12);  // Second voxel in that octant
		v.bake();

		// The old manual SubDAGArray interface still works.
		SubDAGArray subDAGs = findSubDAGs(Internals::getNodes(v), v.rootNodeIndex());
		RayVolumeIntersection hit = intersectVolume(v, subDAGs,
			-3.0f, 2.0f, 3.0f, 1.0f, 0.01f, 0.02f, true);
		TASK_CHECK(hit.hit && hit.material == 7);

		// And agrees with the context-based interface.
		RaycastContext context;
		RayVolumeIntersection hit2 = intersectVolume(v, context,
			Ray3f(vec3(-3.0f, 2.0f, 3.0f), vec3(1.0f, 0.01f, 0.02f)), true);
		TASK_CHECK(hit2.hit && hit2.material == 7);
		TASK_CHECK(std::fabs(hit.distance - hit2.distance) < 0.001);
		return 0;
	}
}

const struct task_case task_cases[] = {
	{ "history_compaction", "bake_preserves_full_history", case_bake_preserves_full_history },
	{ "history_compaction", "branch_rules_after_compaction", case_branch_rules_after_compaction },
	{ "history_compaction", "sharing_and_root_reachability", case_sharing_and_root_reachability },
	{ "history_compaction", "empty_and_material_roots", case_empty_and_material_roots },
	{ "copy_on_write_io", "save_keeps_history_and_writes_current_scene", case_save_keeps_history_and_writes_current_scene },
	{ "copy_on_write_io", "load_replaces_history_and_failed_load_keeps_state", case_load_replaces_history_and_failed_load_keeps_state },
	{ "copy_on_write_io", "edits_are_copy_on_write", case_edits_are_copy_on_write },
	{ "raycast_epochs", "hits_match_independent_reference", case_hits_match_independent_reference },
	{ "raycast_epochs", "context_caching_and_epochs", case_context_caching_and_epochs },
	{ "raycast_epochs", "legacy_manual_subdag_interface", case_legacy_manual_subdag_interface },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
