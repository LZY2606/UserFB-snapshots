// Regression tests for history-aware volume compaction and for raycasting with
// a cached RaycastContext.
//
// Cases are registered in 'task_cases' and run by task_support/driver.c. Each
// group keeps its own independent reference of the expected voxel contents so
// that the assertions do not depend on the implementation being tested.

#include "task_support/check.h"

#include "storage.h"
#include "raytracing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include <unistd.h>

using namespace Cubiquity;
using namespace Cubiquity::Internals;

namespace
{
	typedef std::array<i32, 3> Coord;
	typedef std::map<Coord, MaterialId> Scene;

	////////////////////////////////////////////////////////////////////////////
	// Independent scene helpers. 'Scene' is the reference contents for a
	// historical state; edits are applied to it as they are applied to the
	// Volume, so later assertions compare against this rather than against data
	// produced by the code under test.
	////////////////////////////////////////////////////////////////////////////

	void writeScene(Volume& volume, const Scene& scene)
	{
		for (const auto& entry : scene) {
			volume.setVoxel(entry.first[0], entry.first[1], entry.first[2], entry.second);
		}
	}

	bool sceneMatches(const Volume& volume, const Scene& scene)
	{
		for (const auto& entry : scene) {
			if (volume.voxel(entry.first[0], entry.first[1], entry.first[2]) != entry.second) {
				return false;
			}
		}
		return true;
	}

	// A sparse scene which covers four different octants of the root node, so
	// that ray traversal sees multiple non-empty branches. The pairs of
	// adjacent voxels avoid degenerate (single-child) chains.
	Scene baseScene()
	{
		return Scene{
			{ {  1,  2,  3 }, 7 }, { {  3,  2,  3 },  8 },
			{ { -3, -2, -1 }, 9 }, { { -2, -2, -1 }, 10 },
			{ {  5, -2,  4 }, 11}, { {  6, -2,  4 }, 12 },
			{ { -4,  3, -6 }, 13}, { { -3,  3, -6 }, 14 },
			{ {  1, -4, 10 }, 15}, { {  2, -4, 10 }, 16 },
			{ { 20, 20, 20 }, 17}, { { 20, 20, 21 }, 18 },
		};
	}

	Scene withEdits(Scene scene)
	{
		scene[Coord{ { 2, 3, 4 } }] = 21;
		scene[Coord{ { 3, 2, 3 } }] = 23;
		scene[Coord{ { -5, -4, -3 } }] = 22;
		return scene;
	}

	////////////////////////////////////////////////////////////////////////////
	// Node graph helpers. These inspect the real NodeStore rather than any
	// shadow representation.
	////////////////////////////////////////////////////////////////////////////

	bool isNodeIndexValid(const NodeStore& nodes, u32 index)
	{
		if (isMaterialNode(index)) { return true; }
		return index < nodes.unsharedNodesEnd();
	}

	// Collects all non-material node indices reachable from a root.
	bool collectReachable(const NodeStore& nodes, u32 root, std::unordered_set<u32>& reachable)
	{
		if (!isNodeIndexValid(nodes, root)) { return false; }
		if (isMaterialNode(root)) { return true; }

		std::vector<u32> pending = { root };
		while (!pending.empty()) {
			const u32 index = pending.back();
			pending.pop_back();
			if (!reachable.insert(index).second) { continue; }
			for (const u32 child : nodes[index]) {
				if (!isNodeIndexValid(nodes, child)) { return false; }
				if (!isMaterialNode(child)) { pending.push_back(child); }
			}
		}
		return true;
	}

	////////////////////////////////////////////////////////////////////////////
	// Volume history driver. Keeps an independent reference scene for every
	// recorded state and can restore the volume to any of them.
	////////////////////////////////////////////////////////////////////////////

	struct HistoryDriver
	{
		Volume& volume;
		std::vector<Scene> scenes; // Parallel to the volume's checkpoint history.
		int current = 0;

		explicit HistoryDriver(Volume& vol) : volume(vol)
		{
			scenes.push_back(Scene{});
		}

		void checkpoint()
		{
			volume.checkpoint();
			scenes.resize(current + 1);
			scenes.push_back(scenes[current]);
			current++;
		}

		void setVoxel(i32 x, i32 y, i32 z, MaterialId material)
		{
			volume.setVoxel(x, y, z, material);
			// Edits belong to the current state until a checkpoint is taken.
			if (material == 0) { scenes[current].erase(Coord{ { x, y, z } }); }
			else { scenes[current][Coord{ { x, y, z } }] = material; }
		}

		bool undo()
		{
			const bool changed = current > 0;
			volume.undo();
			if (changed) { current--; }
			return changed;
		}

		bool redo()
		{
			const bool changed = current + 1 < static_cast<int>(scenes.size());
			volume.redo();
			if (changed) { current++; }
			return changed;
		}

		bool verify() const
		{
			return sceneMatches(volume, scenes[current]);
		}

		// Verifies every state which the driver still considers redoable.
		bool verifyAllReachable()
		{
			const int saved = current;
			for (int i = 0; i < static_cast<int>(scenes.size()); i++) {
				while (current > i) { volume.undo(); current--; }
				while (current < i) { volume.redo(); current++; }
				if (!sceneMatches(volume, scenes[i])) { return false; }
			}
			while (current > saved) { volume.undo(); current--; }
			while (current < saved) { volume.redo(); current++; }
			return sceneMatches(volume, scenes[saved]);
		}
	};

	////////////////////////////////////////////////////////////////////////////
	// Reference raytracing. Intersects the ray with one axis-aligned box per
	// occupied voxel (a voxel occupies [p-0.5, p+0.5] on each axis).
	////////////////////////////////////////////////////////////////////////////

	struct ReferenceHit
	{
		bool hit = false;
		double distance = HUGE_VAL;
		MaterialId material = 0;
	};

	ReferenceHit referenceIntersect(const Scene& scene, Ray3f ray)
	{
		ReferenceHit best;
		for (const auto& entry : scene) {
			const Coord& p = entry.first;
			const MaterialId material = entry.second;
			if (material == 0) { continue; }

			float lo[3] = { p[0] - 0.5f, p[1] - 0.5f, p[2] - 0.5f };
			float hi[3] = { p[0] + 0.5f, p[1] + 0.5f, p[2] + 0.5f };
			const float o[3] = { ray.mOrigin[0], ray.mOrigin[1], ray.mOrigin[2] };
			const float d[3] = { ray.mDir[0], ray.mDir[1], ray.mDir[2] };

			float tMin = -1.0e30f;
			float tMax = 1.0e30f;
			bool miss = false;
			for (int axis = 0; axis < 3; axis++) {
				if (std::abs(d[axis]) < 1.0e-12f) {
					if ((o[axis] < lo[axis]) || (o[axis] > hi[axis])) { miss = true; break; }
				}
				else {
					float t0 = (lo[axis] - o[axis]) / d[axis];
					float t1 = (hi[axis] - o[axis]) / d[axis];
					if (t0 > t1) { std::swap(t0, t1); }
					tMin = std::max(tMin, t0);
					tMax = std::min(tMax, t1);
				}
			}

			if (miss || (tMax < tMin) || (tMax <= 0.0f)) { continue; }
			const double t = (tMin > 0.0f) ? tMin : 0.0;
			if (t < best.distance) {
				best.hit = true;
				best.distance = t;
				best.material = material;
			}
		}
		return best;
	}

	// Directions avoid zero components (the ESVO traversal does not define them
	// well) and are slightly sheared so rays do not graze voxel faces.
	std::vector<Ray3f> referenceRays()
	{
		return {
			Ray3f(vec3f(-20.0f, -20.0f, -20.0f), vec3f(1.0f, 1.003f, 1.007f)),
			Ray3f(vec3f(25.0f, 25.0f, 25.0f),   vec3f(-1.0f, -1.003f, -1.007f)),
			Ray3f(vec3f(1.0f, 2.0f, -30.0f),    vec3f(0.001f, 0.002f, 1.0f)),
			Ray3f(vec3f(-30.0f, 3.0f, -6.0f),   vec3f(1.0f, 0.001f, 0.002f)),
			Ray3f(vec3f(5.0f, -30.0f, 4.0f),    vec3f(0.001f, 1.0f, 0.002f)),
			Ray3f(vec3f(-10.0f, 10.0f, 10.0f),  vec3f(1.0f, -1.003f, -1.007f)),
			Ray3f(vec3f(30.0f, 20.0f, 20.0f),   vec3f(-1.0f, -0.002f, -0.003f)),
			Ray3f(vec3f(1.0f, -4.0f, -30.0f),   vec3f(0.001f, 0.002f, 1.0f)),
			Ray3f(vec3f(-20.0f, -20.0f, -20.0f), vec3f(1.0f, 1.3f, 1.7f)),
		};
	}

	bool compareIntersection(const RayVolumeIntersection& actual, const ReferenceHit& expected)
	{
		if (actual.hit != expected.hit) { return false; }
		if (!expected.hit) { return true; }
		return (std::abs(actual.distance - expected.distance) < 1.0e-3) &&
			(actual.material == expected.material);
	}

	// Same test, but through the six-float entry point as well as the Ray3f one.
	bool checkRays(const Volume& volume, const Scene& scene)
	{
		auto subDAGs = findSubDAGs(Internals::getNodes(volume), volume.rootNodeIndex());
		for (const Ray3f& ray : referenceRays()) {
			const ReferenceHit expected = referenceIntersect(scene, ray);
			const RayVolumeIntersection actual = intersectVolume(volume, subDAGs, ray, true);
			if (!compareIntersection(actual, expected)) { return false; }

			const RayVolumeIntersection actualFloats = intersectVolume(volume, subDAGs,
				ray.mOrigin[0], ray.mOrigin[1], ray.mOrigin[2],
				ray.mDir[0], ray.mDir[1], ray.mDir[2], true);
			if (!compareIntersection(actualFloats, expected)) { return false; }
		}
		return true;
	}

	// Temporary path in the sandbox-provided temp directory.
	std::string tempPath(const char* name)
	{
		const char* dir = std::getenv("TMPDIR");
		std::string path = dir ? dir : "/tmp";
		if (!path.empty() && path.back() != '/') { path += '/'; }
		path += "cubiquity_task_";
		path += std::to_string(::getpid());
		path += "_";
		path += name;
		return path;
	}

	// Check that every node reachable from any currently retained root is valid
	// and that no unreachable nodes remain in the store after a bake.
	bool verifyStoreAfterBake(Volume& volume, HistoryDriver& history)
	{
		const NodeStore& nodes = Internals::getNodes(volume);

		std::unordered_set<u32> reachable;
		const int saved = history.current;

		for (int i = 0; i < static_cast<int>(history.scenes.size()); i++) {
			while (history.current > i) { volume.undo(); history.current--; }
			while (history.current < i) { volume.redo(); history.current++; }

			const u32 root = volume.rootNodeIndex();
			if (!isNodeIndexValid(nodes, root)) { return false; }
			if (!collectReachable(nodes, root, reachable)) { return false; }
			if (!sceneMatches(volume, history.scenes[i])) { return false; }
		}

		while (history.current > saved) { volume.undo(); history.current--; }
		while (history.current < saved) { volume.redo(); history.current++; }

		// Every non-material node in the store must be reachable from some root.
		const u32 nodeCount = nodes.unsharedNodesEnd();
		for (u32 index = nodes.sharedNodesBegin(); index < nodeCount; index++) {
			if (reachable.find(index) == reachable.end()) { return false; }
		}

		// And every shared node (including unreferenced duplicates) is accounted
		// for by the reachability set.
		return true;
	}

	////////////////////////////////////////////////////////////////////////////
	// history_compaction
	////////////////////////////////////////////////////////////////////////////

	// Minimal reproduction from the task: write, checkpoint, overwrite, bake,
	// undo. Before the fix the undo still observed the newer material.
	int case_bake_preserves_undo()
	{
		Volume volume;
		volume.setVoxel(1, 2, 3, 7);
		volume.checkpoint();
		volume.setVoxel(1, 2, 3, 9);
		TASK_CHECK(volume.voxel(1, 2, 3) == 9);

		volume.undo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 7);
		volume.redo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 9);

		volume.bake();
		TASK_CHECK(volume.voxel(1, 2, 3) == 9);

		volume.undo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 7);

		// The redo version must still be alive after compaction.
		volume.redo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 9);
		return 0;
	}

	// Compacting while positioned in the past must not discard the future.
	int case_bake_at_past_position_keeps_future()
	{
		Volume volume;
		HistoryDriver history(volume);

		history.setVoxel(1, 2, 3, 7);
		history.checkpoint();
		history.setVoxel(1, 2, 3, 9);
		history.checkpoint();
		history.setVoxel(1, 2, 3, 11);
		TASK_CHECK(history.verify());

		TASK_CHECK(history.undo()); // Now at state 1 (material 9)
		TASK_CHECK(volume.voxel(1, 2, 3) == 9);

		volume.bake();
		TASK_CHECK(volume.voxel(1, 2, 3) == 9);

		TASK_CHECK(history.redo()); // Future state 2 (material 11)
		TASK_CHECK(volume.voxel(1, 2, 3) == 11);
		TASK_CHECK(history.undo());
		TASK_CHECK(history.undo()); // Back to state 0 (material 7)
		TASK_CHECK(volume.voxel(1, 2, 3) == 7);
		TASK_CHECK(!history.undo()); // No earlier state
		return 0;
	}

	// All historical states survive compaction, in both directions.
	int case_bake_preserves_all_states()
	{
		Volume volume;
		HistoryDriver history(volume);
		writeScene(volume, baseScene());
		history.scenes[0] = baseScene();

		const Scene second = withEdits(baseScene());
		for (const auto& entry : second) {
			history.setVoxel(entry.first[0], entry.first[1], entry.first[2], entry.second);
		}
		history.checkpoint();

		history.setVoxel(6, -2, 4, 0);
		history.setVoxel(-4, 3, -6, 31);
		history.checkpoint();

		TASK_CHECK(history.verifyAllReachable());

		volume.bake();
		TASK_CHECK(history.verifyAllReachable());
		TASK_CHECK(verifyStoreAfterBake(volume, history));

		// Baking again is idempotent and still preserves history.
		volume.bake();
		TASK_CHECK(history.verifyAllReachable());
		TASK_CHECK(verifyStoreAfterBake(volume, history));
		return 0;
	}

	// Versions must share subgraphs after compaction rather than each keeping a
	// private copy of the whole volume.
	int case_bake_shares_identical_subgraphs()
	{
		Volume volume;

		// Large sparse scene with two independent regions. The edit in version 2
		// touches only one voxel in one region, so version 1 and version 2 share
		// most of their node graph while both remain non-uniform roots.
		Scene scene = baseScene();
		for (i32 i = 0; i < 12; i++) {
			scene[Coord{ { 100 + i, 200, 300 } }] = static_cast<MaterialId>(1 + (i % 3));
			scene[Coord{ { -100 - i, -200, -300 } }] = static_cast<MaterialId>(4 + (i % 3));
		}
		writeScene(volume, scene);
		volume.checkpoint();

		volume.setVoxel(100, 200, 300, 42);

		volume.bake();

		const NodeStore& nodes = Internals::getNodes(volume);

		std::unordered_set<u32> reachable0, reachable1;

		volume.undo();
		// Reconstruct the exact expected state from the independent reference.
		TASK_CHECK(volume.voxel(100, 200, 300) == 1);
		TASK_CHECK(sceneMatches(volume, scene));
		TASK_CHECK(collectReachable(nodes, volume.rootNodeIndex(), reachable0));

		volume.redo();
		TASK_CHECK(volume.voxel(100, 200, 300) == 42);
		TASK_CHECK(collectReachable(nodes, volume.rootNodeIndex(), reachable1));

		// The two versions must have real nodes in common (sharing), and the
		// union must be strictly smaller than the sum of the individual counts.
		size_t sharedCount = 0;
		for (u32 index : reachable0) {
			if (reachable1.count(index)) { sharedCount++; }
		}
		TASK_CHECK(sharedCount > 0);
		TASK_CHECK(reachable0.size() + reachable1.size() - sharedCount < reachable0.size() + reachable1.size());

		// The node store is compacted: the total number of nodes equals the
		// reachable union (no leaks, no per-version dense expansion).
		std::unordered_set<u32> unionSet = reachable0;
		unionSet.insert(reachable1.begin(), reachable1.end());
		TASK_CHECK(nodes.unsharedNodesEnd() == nodes.sharedNodesBegin() + unionSet.size());
		return 0;
	}

	// Editing the current version after compaction must not pollute another
	// version (copy-on-write along the path to the edited voxel).
	int case_edit_after_bake_is_isolated()
	{
		Volume volume;
		Scene first = baseScene();
		writeScene(volume, first);
		volume.checkpoint();

		volume.setVoxel(2, 3, 4, 21);
		volume.checkpoint();
		volume.bake();

		// Go back to the first version and edit it.
		volume.undo();
		volume.undo();
		volume.setVoxel(1, 2, 3, 99);

		TASK_CHECK(volume.voxel(1, 2, 3) == 99);
		volume.redo(); // second version
		TASK_CHECK(volume.voxel(1, 2, 3) == 7);
		TASK_CHECK(volume.voxel(2, 3, 4) == 21);
		volume.redo(); // third version
		TASK_CHECK(volume.voxel(1, 2, 3) == 7);
		TASK_CHECK(volume.voxel(2, 3, 4) == 21);
		volume.undo();
		volume.undo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 99);
		return 0;
	}

	// Empty volumes and pure material roots take part in compaction.
	int case_empty_and_material_roots()
	{
		Volume volume;
		HistoryDriver history(volume);

		// An untouched new volume is empty (material 0 at the root).
		TASK_CHECK(volume.voxel(0, 0, 0) == 0);
		volume.bake();
		TASK_CHECK(volume.voxel(0, 0, 0) == 0);

		history.setVoxel(4, 5, 6, 7);
		history.checkpoint();
		history.setVoxel(4, 5, 6, 8);
		history.checkpoint();
		TASK_CHECK(history.verifyAllReachable());

		volume.bake();
		TASK_CHECK(history.verifyAllReachable());
		TASK_CHECK(verifyStoreAfterBake(volume, history));

		// Both recorded versions must remain distinct and correct. Note that
		// the first edit precedes the first checkpoint, so it belongs to the
		// oldest retained state.
		volume.undo();
		TASK_CHECK(volume.voxel(4, 5, 6) == 8);
		volume.undo();
		TASK_CHECK(volume.voxel(4, 5, 6) == 7);
		volume.redo();
		TASK_CHECK(volume.voxel(4, 5, 6) == 8);
		volume.redo();
		TASK_CHECK(volume.voxel(4, 5, 6) == 8);
		TASK_CHECK(history.verify());
		return 0;
	}

	// fill() fills the whole root; combine() merges volumes. Both must follow
	// the same branch/checkpoint rules and survive compaction.
	int case_fill_and_combine_history()
	{
		Volume volume;
		volume.setVoxel(1, 2, 3, 7);
		volume.checkpoint();          // state 0: single voxel
		volume.fill(3);
		volume.checkpoint();          // state 1: uniform material 3
		volume.setVoxel(1, 2, 3, 4);
		volume.checkpoint();          // state 2: 4 at the edited voxel

		Volume other;
		other.setVoxel(1, 2, 3, 5);
		other.setVoxel(5, 6, 7, 6);
		volume.combine(other, [](MaterialId a, MaterialId b) { return b != 0 ? b : a; });
		TASK_CHECK(volume.voxel(1, 2, 3) == 5);
		TASK_CHECK(volume.voxel(5, 6, 7) == 6);
		TASK_CHECK(volume.voxel(-10, 20, -30) == 3);
		TASK_CHECK(volume.voxel(9, 9, 9) == 3);

		volume.bake();

		// Every retained root remains reachable and the store holds no orphans.
		// (All four states are reachable: three checkpoints plus the current one.)
		{
			const NodeStore& nodes = Internals::getNodes(volume);
			const u32 savedRoot = volume.rootNodeIndex();
			std::unordered_set<u32> reachable;
			TASK_CHECK(collectReachable(nodes, savedRoot, reachable));
			volume.undo();
			TASK_CHECK(collectReachable(nodes, volume.rootNodeIndex(), reachable));
			volume.undo();
			TASK_CHECK(collectReachable(nodes, volume.rootNodeIndex(), reachable));
			volume.undo();
			TASK_CHECK(collectReachable(nodes, volume.rootNodeIndex(), reachable));
			TASK_CHECK(nodes.unsharedNodesEnd() == nodes.sharedNodesBegin() + reachable.size());
			while (volume.rootNodeIndex() != savedRoot) { volume.redo(); }
		}

		// Current state survives.
		TASK_CHECK(volume.voxel(1, 2, 3) == 5);
		TASK_CHECK(volume.voxel(5, 6, 7) == 6);
		TASK_CHECK(volume.voxel(-10, 20, -30) == 3);

		// Undo back through the pre-combine versions.
		// Undo back through the pre-combine versions.
		volume.undo();                // state 2: edited uniform volume (4)
		TASK_CHECK(volume.voxel(1, 2, 3) == 4);
		TASK_CHECK(volume.voxel(-10, 20, -30) == 3);
		volume.undo();                // state 1: uniform 3
		TASK_CHECK(volume.voxel(1, 2, 3) == 3);
		TASK_CHECK(volume.voxel(5, 6, 7) == 3);
		volume.undo();                // state 0: material 7 only
		TASK_CHECK(volume.voxel(1, 2, 3) == 7);
		TASK_CHECK(volume.voxel(-10, 20, -30) == 0);
		volume.redo();                // state 1 again
		TASK_CHECK(volume.voxel(1, 2, 3) == 3);
		volume.redo();                // state 2 again
		TASK_CHECK(volume.voxel(1, 2, 3) == 4);
		return 0;
	}

	// Deterministic pseudo-random history: many branches, edits, undo/redo,
	// compaction and further branching. The expected scene for each state is
	// maintained independently, then every retained root is checked against the
	// real node graph after compaction.
	int case_randomised_branching_history()
	{
		Volume volume;
		HistoryDriver history(volume);

		// Simple deterministic LCG so the case does not depend on <random>.
		u32 state = 123456789u;
		auto next = [&]() {
			state = state * 1664525u + 1013904223u;
			return state >> 8;
		};
		auto coord = [&](u32 value, i32 offset) -> i32 {
			return static_cast<i32>(value % 17) - 8 + offset;
		};

		for (int step = 0; step < 60; step++) {
			const u32 action = next() % 10;
			if (action < 5) {
				const i32 x = coord(next(), 0);
				const i32 y = coord(next(), 0);
				const i32 z = coord(next(), 0);
				const MaterialId material = static_cast<MaterialId>(next() % 9);
				history.setVoxel(x, y, z, material);
			}
			else if (action < 7) {
				history.checkpoint();
			}
			else if (action < 9) {
				history.undo();
			}
			else {
				history.redo();
			}
			TASK_CHECK(history.verify());
		}

		// Compact in the middle of history, verify every state, then branch.
		TASK_CHECK(history.undo());
		TASK_CHECK(history.undo());
		volume.bake();
		TASK_CHECK(history.verifyAllReachable());
		TASK_CHECK(verifyStoreAfterBake(volume, history));

		for (int step = 0; step < 20; step++) {
			const i32 x = coord(next(), 1);
			const i32 y = coord(next(), 2);
			const i32 z = coord(next(), 3);
			const MaterialId material = static_cast<MaterialId>(next() % 9);
			history.setVoxel(x, y, z, material);
			if (next() % 3 == 0) { history.checkpoint(); }
			TASK_CHECK(history.verify());
		}

		volume.bake();
		TASK_CHECK(history.verifyAllReachable());
		TASK_CHECK(verifyStoreAfterBake(volume, history));
		return 0;
	}

	// A checkpoint made after undo crops the redo history. Compacted storage
	// must respect this.
	int case_checkpoint_after_undo_crops_redo()
	{
		Volume volume;
		volume.setVoxel(1, 2, 3, 5);
		volume.checkpoint();
		volume.setVoxel(1, 2, 3, 6);
		volume.checkpoint();
		volume.setVoxel(1, 2, 3, 7);

		volume.bake();
		TASK_CHECK(volume.voxel(1, 2, 3) == 7);

		volume.undo();
		volume.undo(); // state 0, material 5
		TASK_CHECK(volume.voxel(1, 2, 3) == 5);

		volume.setVoxel(1, 2, 3, 8);
		volume.checkpoint(); // crops the redo branch
		TASK_CHECK(volume.voxel(1, 2, 3) == 8);

		volume.bake();

		TASK_CHECK(volume.voxel(1, 2, 3) == 8);
		volume.undo();
		// State 0 was edited in place before the checkpoint, so it now holds 8
		// (editing the current version is what changes it). The old 6/7 branch
		// must have been cropped, not resurrected.
		TASK_CHECK(volume.voxel(1, 2, 3) == 8);
		volume.redo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 8);
		volume.undo();
		volume.undo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 8);
		return 0;
	}

	////////////////////////////////////////////////////////////////////////////
	// copy_on_write_io
	////////////////////////////////////////////////////////////////////////////

	// Saving only writes the current scene, in the same layout, and must not
	// destroy the in-memory history.
	int case_save_preserves_history()
	{
		const std::string path = tempPath("save_history.dag");

		Volume volume;
		HistoryDriver history(volume);
		writeScene(volume, baseScene());
		history.scenes[0] = baseScene();
		history.checkpoint();
		history.setVoxel(2, 3, 4, 21);
		history.checkpoint();

		const Scene savedScene = history.scenes.back();
		volume.save(path);

		// In-memory history is untouched.
		TASK_CHECK(history.verifyAllReachable());
		TASK_CHECK(volume.voxel(2, 3, 4) == 21);

		TASK_CHECK(history.undo()); // edit belongs to the previous state too
		TASK_CHECK(history.verify());
		TASK_CHECK(volume.voxel(2, 3, 4) == 21);
		TASK_CHECK(history.undo()); // oldest state predates the edit
		TASK_CHECK(history.verify());
		TASK_CHECK(volume.voxel(2, 3, 4) == 0);
		TASK_CHECK(history.redo());
		TASK_CHECK(history.redo());

		// And the file contains the scene which was current at save time.
		Volume loaded;
		TASK_CHECK(loaded.load(path));
		TASK_CHECK(sceneMatches(loaded, savedScene));
		TASK_CHECK(loaded.voxel(2, 3, 4) == 21);

		std::filesystem::remove(path);
		return 0;
	}

	// The on-disk format is unchanged: a load of a file written by save gives
	// exactly the current scene, and save after undo writes that older state.
	int case_save_after_undo_writes_current_state()
	{
		const std::string path = tempPath("save_current.dag");

		Volume volume;
		volume.setVoxel(1, 2, 3, 7);
		volume.checkpoint();
		volume.setVoxel(1, 2, 3, 9);
		volume.checkpoint();
		volume.setVoxel(1, 2, 3, 11);

		volume.undo(); // current scene has material 9
		volume.save(path);

		Volume loaded;
		TASK_CHECK(loaded.load(path));
		TASK_CHECK(loaded.voxel(1, 2, 3) == 9);

		// The saving volume still has its full history.
		TASK_CHECK(volume.voxel(1, 2, 3) == 9);
		volume.undo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 7);
		volume.redo();
		volume.redo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 11);

		std::filesystem::remove(path);
		return 0;
	}

	// A successful load replaces both scene and history: the old states can no
	// longer be undone to.
	int case_load_replaces_history()
	{
		const std::string path = tempPath("load_replace.dag");

		Volume saved;
		saved.setVoxel(10, 10, 10, 42);
		saved.setVoxel(11, 10, 10, 43);
		saved.save(path);

		Volume volume;
		volume.setVoxel(1, 2, 3, 7);
		volume.checkpoint();
		volume.setVoxel(1, 2, 3, 9);

		TASK_CHECK(volume.load(path));

		TASK_CHECK(volume.voxel(10, 10, 10) == 42);
		TASK_CHECK(volume.voxel(11, 10, 10) == 43);
		TASK_CHECK(volume.voxel(1, 2, 3) == 0);

		// No previous state is reachable.
		volume.undo();
		TASK_CHECK(volume.voxel(10, 10, 10) == 42);
		TASK_CHECK(volume.voxel(1, 2, 3) == 0);
		TASK_CHECK(volume.voxel(11, 10, 10) == 43);

		// A checkpoint now starts a new history from the loaded scene.
		volume.checkpoint();          // state 0: loaded scene
		volume.setVoxel(10, 10, 10, 44);
		volume.checkpoint();          // state 1: material 44
		volume.setVoxel(10, 10, 10, 45);

		volume.bake();
		TASK_CHECK(volume.voxel(10, 10, 10) == 45);
		volume.undo();
		TASK_CHECK(volume.voxel(10, 10, 10) == 44);
		volume.undo();
		TASK_CHECK(volume.voxel(10, 10, 10) == 42);
		TASK_CHECK(volume.voxel(1, 2, 3) == 0);

		std::filesystem::remove(path);
		return 0;
	}

	// A load which cannot open the file leaves the existing scene and history
	// untouched.
	int case_failed_load_keeps_history()
	{
		Volume volume;
		HistoryDriver history(volume);
		writeScene(volume, baseScene());
		history.scenes[0] = baseScene();
		history.checkpoint();
		history.setVoxel(2, 3, 4, 21);
		history.checkpoint();

		const std::string missing = tempPath("does_not_exist.dag");
		std::filesystem::remove(missing);

		TASK_CHECK(!volume.load(missing));
		TASK_CHECK(history.verifyAllReachable());
		TASK_CHECK(volume.voxel(2, 3, 4) == 21);
		TASK_CHECK(history.undo());
		TASK_CHECK(history.verify());
		TASK_CHECK(volume.voxel(2, 3, 4) == 21);
		TASK_CHECK(history.undo());
		TASK_CHECK(history.verify());
		TASK_CHECK(volume.voxel(2, 3, 4) == 0);
		TASK_CHECK(history.redo());
		TASK_CHECK(history.redo());
		TASK_CHECK(history.verify());

		// A truncated file is also a failed load.
		const std::string truncated = tempPath("truncated.dag");
		{
			FILE* file = std::fopen(truncated.c_str(), "wb");
			TASK_CHECK(file != nullptr);
			const unsigned char bytes[2] = { 0, 0 };
			std::fwrite(bytes, 1, sizeof(bytes), file);
			std::fclose(file);
		}
		TASK_CHECK(!volume.load(truncated));
		TASK_CHECK(history.verify());
		TASK_CHECK(volume.voxel(2, 3, 4) == 21);

		std::filesystem::remove(truncated);
		return 0;
	}

	// Round trip every historical state through save+load, comparing against
	// the independent references, both before and after compaction.
	int case_save_load_round_trip_states()
	{
		Volume volume;
		HistoryDriver history(volume);
		writeScene(volume, baseScene());
		history.scenes[0] = baseScene();
		history.checkpoint();

		const Scene second = withEdits(baseScene());
		for (const auto& entry : second) {
			history.setVoxel(entry.first[0], entry.first[1], entry.first[2], entry.second);
		}
		history.checkpoint();
		history.setVoxel(20, 20, 20, 0);
		history.checkpoint();

		volume.bake();
		TASK_CHECK(history.verifyAllReachable());

		for (int state = 0; state < static_cast<int>(history.scenes.size()); state++) {
			while (history.current > state) { volume.undo(); history.current--; }
			while (history.current < state) { volume.redo(); history.current++; }

			const std::string path = tempPath("round_trip.dag");
			volume.save(path);

			Volume loaded;
			TASK_CHECK(loaded.load(path));
			TASK_CHECK(sceneMatches(loaded, history.scenes[state]));

			// Check unrelated coordinates are empty in the loaded volume.
			TASK_CHECK(loaded.voxel(-10, -10, -10) == 0);

			std::filesystem::remove(path);
		}
		return 0;
	}

	// Save must not shrink the in-memory reachable set (no destroying nodes
	// which a retained redo state needs).
	int case_save_retains_redo_nodes()
	{
		const std::string path = tempPath("save_redo.dag");

		Volume volume;
		volume.setVoxel(1, 2, 3, 7);
		volume.checkpoint();
		volume.setVoxel(1, 2, 3, 9);
		volume.checkpoint();
		volume.setVoxel(1, 2, 3, 11);

		volume.undo();
		volume.undo();
		volume.save(path);

		volume.redo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 9);
		volume.redo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 11);

		std::filesystem::remove(path);
		return 0;
	}

	////////////////////////////////////////////////////////////////////////////
	// raycast_epochs
	////////////////////////////////////////////////////////////////////////////

	// The context must reuse its cache across stable queries.
	int case_raycast_context_reuses_cache()
	{
		Volume volume;
		writeScene(volume, baseScene());
		volume.bake();

		RaycastContext context;
		const std::vector<Ray3f> rays = referenceRays();

		for (const Ray3f& ray : rays) {
			const ReferenceHit expected = referenceIntersect(baseScene(), ray);
			const RayVolumeIntersection actual = intersectVolume(volume, context, ray, true);
			TASK_CHECK(compareIntersection(actual, expected));
		}
		TASK_CHECK(context.preparationCount() == 1);
		TASK_CHECK(checkRays(volume, baseScene()));
		TASK_CHECK(context.preparationCount() == 1);

		// Mixing the float entry point must not refresh the cache either.
		for (const Ray3f& ray : rays) {
			const RayVolumeIntersection actual = intersectVolume(volume, context,
				ray.mOrigin[0], ray.mOrigin[1], ray.mOrigin[2],
				ray.mDir[0], ray.mDir[1], ray.mDir[2], true);
			const ReferenceHit expected = referenceIntersect(baseScene(), ray);
			TASK_CHECK(compareIntersection(actual, expected));
		}
		TASK_CHECK(context.preparationCount() == 1);
		TASK_CHECK(volume.revision() != 0);
		return 0;
	}

	// After edits the cache is refreshed exactly once, even for many edits
	// between queries, and the new scene is picked up.
	int case_raycast_context_refreshes_after_edits()
	{
		Volume volume;
		Scene scene = baseScene();
		writeScene(volume, scene);
		volume.bake();

		RaycastContext context;
		const Ray3f ray = Ray3f(vec3f(-20.0f, -20.0f, -20.0f), vec3f(1.0f, 1.003f, 1.007f));

		RayVolumeIntersection before = intersectVolume(volume, context, ray, true);
		TASK_CHECK(compareIntersection(before, referenceIntersect(scene, ray)));
		TASK_CHECK(context.preparationCount() == 1);

		// Many edits without querying in between. These are deliberately offset
		// from the ray origin so the first hit on the ray is unchanged; the test
		// is about cache invalidation, not about changing the result.
		for (i32 i = 1; i <= 64; i++) {
			const Coord coord{ { -20 + i, 30, 30 } };
			const MaterialId material = static_cast<MaterialId>(1 + (i % 5));
			volume.setVoxel(coord[0], coord[1], coord[2], material);
			scene[coord] = material;
		}

		RayVolumeIntersection after = intersectVolume(volume, context, ray, true);
		TASK_CHECK(compareIntersection(after, referenceIntersect(scene, ray)));
		TASK_CHECK(context.preparationCount() == 2);

		// Stable queries reuse the cache again.
		intersectVolume(volume, context, ray, true);
		intersectVolume(volume, context, ray, true);
		TASK_CHECK(context.preparationCount() == 2);
		return 0;
	}

	// undo/redo change the visible state, so they must invalidate the cache
	// while remaining conservative (exactly one refresh per state change seen
	// by the next query).
	int case_raycast_context_tracks_history()
	{
		Volume volume;
		Scene before = baseScene();
		writeScene(volume, before);
		volume.checkpoint();

		Scene after = before;
		after[Coord{ { -21, -21, -21 } }] = 55;
		volume.setVoxel(-21, -21, -21, 55);
		volume.bake();

		RaycastContext context;
		const Ray3f ray = Ray3f(vec3f(-25.0f, -25.0f, -25.0f), vec3f(1.0f, 1.003f, 1.007f));

		TASK_CHECK(compareIntersection(intersectVolume(volume, context, ray, true),
			referenceIntersect(after, ray)));
		TASK_CHECK(context.preparationCount() == 1);

		volume.undo();
		TASK_CHECK(compareIntersection(intersectVolume(volume, context, ray, true),
			referenceIntersect(before, ray)));
		TASK_CHECK(context.preparationCount() == 2);

		volume.redo();
		TASK_CHECK(compareIntersection(intersectVolume(volume, context, ray, true),
			referenceIntersect(after, ray)));
		TASK_CHECK(context.preparationCount() == 3);
		return 0;
	}

	// Compaction can leave the root index unchanged; only the revision can be
	// relied on to detect it. This checks the context after a bake which does
	// move data around.
	int case_raycast_context_after_bake()
	{
		Volume volume;
		const Scene scene = baseScene();
		writeScene(volume, scene);
		volume.checkpoint();
		volume.setVoxel(2, 3, 4, 21);
		volume.checkpoint();

		RaycastContext context;
		const Ray3f ray = Ray3f(vec3f(1.0f, 2.0f, -30.0f), vec3f(0.001f, 0.002f, 1.0f));

		Scene current = scene;
		current[Coord{ { 2, 3, 4 } }] = 21;

		const RayVolumeIntersection preBake = intersectVolume(volume, context, ray, true);
		TASK_CHECK(compareIntersection(preBake, referenceIntersect(current, ray)));
		const u64 preparations = context.preparationCount();

		volume.bake();
		TASK_CHECK(compareIntersection(intersectVolume(volume, context, ray, true),
			referenceIntersect(current, ray)));
		TASK_CHECK(context.preparationCount() == preparations + 1);

		// bake() is a no-op for reads if nothing changed, but it is allowed to
		// count as a preparation epoch; what matters is that queries afterwards
		// are correct and stable queries reuse the cache.
		const u64 afterBake = context.preparationCount();
		intersectVolume(volume, context, ray, true);
		TASK_CHECK(context.preparationCount() == afterBake);
		return 0;
	}

	// Two contexts are independent; switching a context to a different volume
	// rebinds and refreshes it.
	int case_raycast_context_independent_and_rebind()
	{
		Volume a;
		writeScene(a, baseScene());
		a.bake();

		Scene sceneB = baseScene();
		sceneB[Coord{ { -8, -8, -8 } }] = 77;
		Volume b;
		writeScene(b, sceneB);
		b.bake();

		RaycastContext contextA;
		RaycastContext contextB;

		const Ray3f ray = Ray3f(vec3f(-20.0f, -20.0f, -20.0f), vec3f(1.0f, 1.003f, 1.007f));
		TASK_CHECK(compareIntersection(intersectVolume(a, contextA, ray, true),
			referenceIntersect(baseScene(), ray)));
		TASK_CHECK(compareIntersection(intersectVolume(b, contextB, ray, true),
			referenceIntersect(sceneB, ray)));
		TASK_CHECK(contextA.preparationCount() == 1);
		TASK_CHECK(contextB.preparationCount() == 1);

		// Rebinding a single context to the other volume forces a refresh.
		TASK_CHECK(compareIntersection(intersectVolume(b, contextA, ray, true),
			referenceIntersect(sceneB, ray)));
		TASK_CHECK(contextA.preparationCount() == 2);

		// Switching back also refreshes (it is a different volume again).
		TASK_CHECK(compareIntersection(intersectVolume(a, contextA, ray, true),
			referenceIntersect(baseScene(), ray)));
		TASK_CHECK(contextA.preparationCount() == 3);

		// contextB was untouched by all of this.
		TASK_CHECK(contextB.preparationCount() == 1);
		return 0;
	}

	// A load replaces the scene; a context bound to the volume must observe it.
	int case_raycast_context_after_load()
	{
		const std::string path = tempPath("ray_load.dag");

		Volume saved;
		Scene savedScene{ { { -8, -8, -8 }, 77 }, { { -7, -8, -8 }, 78 } };
		writeScene(saved, savedScene);
		saved.save(path);

		Volume volume;
		writeScene(volume, baseScene());
		volume.bake();

		RaycastContext context;
		const Ray3f ray = Ray3f(vec3f(-30.0f, -8.0f, -8.0f), vec3f(1.0f, 0.002f, 0.003f));
		TASK_CHECK(compareIntersection(intersectVolume(volume, context, ray, true),
			referenceIntersect(baseScene(), ray)));
		TASK_CHECK(context.preparationCount() == 1);

		TASK_CHECK(volume.load(path));
		TASK_CHECK(compareIntersection(intersectVolume(volume, context, ray, true),
			referenceIntersect(savedScene, ray)));
		TASK_CHECK(context.preparationCount() == 2);

		std::filesystem::remove(path);
		return 0;
	}

	// A windowless "continuous picking" example: a stream of queries against a
	// volume which is being edited, checkpointed and compacted, with a single
	// long-lived context. The cache must only refresh when the scene changes.
	int case_continuous_picking_example()
	{
		Volume volume;
		Scene scene = baseScene();
		writeScene(volume, scene);
		volume.bake();

		RaycastContext picker;
		const Ray3f pickRay = Ray3f(vec3f(-20.0f, -20.0f, -20.0f), vec3f(1.0f, 1.003f, 1.007f));

		u64 expectations = 0;

		// Simulate several frames of hover (no edits): one preparation total.
		for (int frame = 0; frame < 10; frame++) {
			const RayVolumeIntersection hit = intersectVolume(volume, picker, pickRay, true);
			TASK_CHECK(compareIntersection(hit, referenceIntersect(scene, pickRay)));
		}
		TASK_CHECK(picker.preparationCount() == 1);
		expectations++;

		// Edit a voxel on the picked ray, then query repeatedly. The first query
		// after the edit refreshes; later ones reuse.
		scene[Coord{ { -10, -10, -10 } }] = 60;
		volume.setVoxel(-10, -10, -10, 60);
		for (int frame = 0; frame < 5; frame++) {
			TASK_CHECK(compareIntersection(intersectVolume(volume, picker, pickRay, true),
				referenceIntersect(scene, pickRay)));
		}
		TASK_CHECK(picker.preparationCount() == 2);

		// Checkpoint + edit + undo + query sequence: each visible state change
		// costs at most one preparation.
		volume.checkpoint();
		scene[Coord{ { -9, -9, -9 } }] = 61;
		volume.setVoxel(-9, -9, -9, 61);
		TASK_CHECK(compareIntersection(intersectVolume(volume, picker, pickRay, true),
			referenceIntersect(scene, pickRay)));
		const u64 beforeUndo = picker.preparationCount();
		TASK_CHECK(beforeUndo == 3);

		volume.undo();
		TASK_CHECK(compareIntersection(intersectVolume(volume, picker, pickRay, true),
			referenceIntersect(scene, pickRay)));
		TASK_CHECK(picker.preparationCount() == 4);

		volume.redo();
		volume.bake();
		const u64 afterBakeQuery = picker.preparationCount();
		TASK_CHECK(compareIntersection(intersectVolume(volume, picker, pickRay, true),
			referenceIntersect(scene, pickRay)));
		TASK_CHECK(picker.preparationCount() == afterBakeQuery + 1);
		TASK_CHECK(picker.preparationCount() <= 6);

		// The old manual SubDAGArray interface still produces the same result.
		const SubDAGArray manual = findSubDAGs(Internals::getNodes(volume), volume.rootNodeIndex());
		TASK_CHECK(compareIntersection(intersectVolume(volume, manual, pickRay, true),
			referenceIntersect(scene, pickRay)));

		(void)expectations;
		return 0;
	}

	// The context works for a uniformly-filled (pure material) volume, which
	// compaction can also produce for an empty/uniform root.
	int case_raycast_material_root()
	{
		Volume volume;
		volume.fill(7);
		volume.checkpoint();
		volume.setVoxel(1, 2, 3, 9);
		volume.bake();

		RaycastContext context;
		Scene scene = baseScene();
		scene.clear();
		scene[Coord{ { 1, 2, 3 } }] = 9;
		// A filled volume is not representable by our sparse reference, so just
		// probe a voxel which must be material 7 (away from the edit).
		const Ray3f ray = Ray3f(vec3f(-10.5f, -10.5f, -10.5f), vec3f(0.001f, 0.002f, 1.0f));
		const RayVolumeIntersection hit = intersectVolume(volume, context, ray, true);
		TASK_CHECK(hit.hit);
		TASK_CHECK(hit.material == 7);
		TASK_CHECK(context.preparationCount() == 1);

		volume.undo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 7);
		volume.redo();
		TASK_CHECK(volume.voxel(1, 2, 3) == 9);
		return 0;
	}

	// A sparse scene with several non-empty root branches is checked against the
	// independent voxel-box reference, before and after compaction, and after
	// history navigation. Both intersectVolume entry points are covered.
	int case_raycast_multi_branch_scene()
	{
		Volume volume;
		writeScene(volume, baseScene());

		// The manual SubDAGArray interface is checked first.
		TASK_CHECK(checkRays(volume, baseScene()));

		// Add a second non-trivial version so the compaction path has multiple
		// roots to remap.
		volume.checkpoint();
		volume.setVoxel(5, -2, 4, 0);
		volume.setVoxel(-4, 3, -6, 30);

		Scene edited = baseScene();
		edited.erase(Coord{ { 5, -2, 4 } });
		edited[Coord{ { -4, 3, -6 } }] = 30;

		RaycastContext context;
		TASK_CHECK(context.preparationCount() == 0);

		const std::vector<Ray3f> rays = referenceRays();
		for (const Ray3f& ray : rays) {
			const RayVolumeIntersection hit = intersectVolume(volume, context, ray, true);
			TASK_CHECK(compareIntersection(hit, referenceIntersect(edited, ray)));
		}
		TASK_CHECK(context.preparationCount() == 1);

		volume.bake();

		for (const Ray3f& ray : rays) {
			const RayVolumeIntersection hit = intersectVolume(volume, context, ray, true);
			TASK_CHECK(compareIntersection(hit, referenceIntersect(edited, ray)));
		}
		TASK_CHECK(context.preparationCount() == 2);
		TASK_CHECK(checkRays(volume, edited));

		volume.undo();
		for (const Ray3f& ray : rays) {
			const RayVolumeIntersection hit = intersectVolume(volume, context, ray, true);
			TASK_CHECK(compareIntersection(hit, referenceIntersect(baseScene(), ray)));
		}
		TASK_CHECK(context.preparationCount() == 3);
		return 0;
	}

	// Several edits between two queries, including checkpoint and history moves,
	// must prepare the cache only once for the next query.
	int case_raycast_single_prepare_after_burst()
	{
		Volume volume;
		Scene scene = baseScene();
		writeScene(volume, scene);
		volume.bake();

		RaycastContext context;
		const Ray3f ray = Ray3f(vec3f(-30.0f, 3.0f, -6.0f), vec3f(1.0f, 0.001f, 0.002f));
		TASK_CHECK(compareIntersection(intersectVolume(volume, context, ray, true),
			referenceIntersect(scene, ray)));
		TASK_CHECK(context.preparationCount() == 1);

		// Burst of edits, with a checkpoint and redo history movement mixed in.
		for (i32 i = 0; i < 20; i++) {
			const Coord coord{ { -4, 3 + i, -6 } };
			const MaterialId material = static_cast<MaterialId>(2 + (i % 7));
			volume.setVoxel(coord[0], coord[1], coord[2], material);
			scene[coord] = material;
		}
		volume.checkpoint();
		volume.undo();
		volume.redo();
		volume.fill(0); // clear the whole volume
		volume.undo();  // and take it back

		TASK_CHECK(compareIntersection(intersectVolume(volume, context, ray, true),
			referenceIntersect(scene, ray)));
		TASK_CHECK(context.preparationCount() == 2);

		// Real-time hover is free again.
		for (int i = 0; i < 8; i++) {
			TASK_CHECK(compareIntersection(intersectVolume(volume, context, ray, true),
				referenceIntersect(scene, ray)));
		}
		TASK_CHECK(context.preparationCount() == 2);
		return 0;
	}

	// Revision is monotonic across every public mutation entry point.
	int case_revision_monotonic()
	{
		Volume volume;
		u64 revision = volume.revision();

		volume.setVoxel(1, 2, 3, 7);
		TASK_CHECK(volume.revision() > revision);
		revision = volume.revision();

		volume.checkpoint();
		TASK_CHECK(volume.revision() > revision);
		revision = volume.revision();

		volume.undo();
		TASK_CHECK(volume.revision() > revision);
		revision = volume.revision();

		volume.redo();
		TASK_CHECK(volume.revision() > revision);
		revision = volume.revision();

		volume.bake();
		TASK_CHECK(volume.revision() > revision);
		revision = volume.revision();

		volume.fill(2);
		TASK_CHECK(volume.revision() > revision);
		revision = volume.revision();

		Volume other;
		other.setVoxel(4, 5, 6, 3);
		volume.combine(other, [](MaterialId a, MaterialId b) { return b != 0 ? b : a; });
		TASK_CHECK(volume.revision() > revision);
		revision = volume.revision();

		const std::string path = tempPath("revision.dag");
		volume.save(path);
		volume.load(path);
		TASK_CHECK(volume.revision() > revision);
		std::filesystem::remove(path);
		return 0;
	}

	// Registration table is at global scope below; the case functions are in
	// the anonymous namespace above but remain visible within this translation
	// unit.
}

extern const struct task_case task_cases[] = {
	{ "history_compaction", "bake_preserves_undo",          case_bake_preserves_undo },
	{ "history_compaction", "bake_at_past_position",        case_bake_at_past_position_keeps_future },
	{ "history_compaction", "bake_preserves_all_states",    case_bake_preserves_all_states },
	{ "history_compaction", "bake_shares_subgraphs",        case_bake_shares_identical_subgraphs },
	{ "history_compaction", "edit_after_bake_isolated",     case_edit_after_bake_is_isolated },
	{ "history_compaction", "empty_and_material_roots",     case_empty_and_material_roots },
	{ "history_compaction", "fill_and_combine_history",     case_fill_and_combine_history },
	{ "history_compaction", "checkpoint_after_undo_crops",  case_checkpoint_after_undo_crops_redo },
	{ "history_compaction", "randomised_branching",         case_randomised_branching_history },

	{ "copy_on_write_io",   "save_preserves_history",       case_save_preserves_history },
	{ "copy_on_write_io",   "save_after_undo_writes_state", case_save_after_undo_writes_current_state },
	{ "copy_on_write_io",   "load_replaces_history",        case_load_replaces_history },
	{ "copy_on_write_io",   "failed_load_keeps_history",    case_failed_load_keeps_history },
	{ "copy_on_write_io",   "save_load_round_trip",         case_save_load_round_trip_states },
	{ "copy_on_write_io",   "save_retains_redo_nodes",      case_save_retains_redo_nodes },

	{ "raycast_epochs",     "context_reuses_cache",         case_raycast_context_reuses_cache },
	{ "raycast_epochs",     "context_refresh_after_edits",  case_raycast_context_refreshes_after_edits },
	{ "raycast_epochs",     "context_tracks_history",       case_raycast_context_tracks_history },
	{ "raycast_epochs",     "context_after_bake",           case_raycast_context_after_bake },
	{ "raycast_epochs",     "context_independent_rebind",   case_raycast_context_independent_and_rebind },
	{ "raycast_epochs",     "context_after_load",           case_raycast_context_after_load },
	{ "raycast_epochs",     "continuous_picking",           case_continuous_picking_example },
	{ "raycast_epochs",     "material_root",                case_raycast_material_root },
	{ "raycast_epochs",     "multi_branch_scene",           case_raycast_multi_branch_scene },
	{ "raycast_epochs",     "single_prepare_after_burst",   case_raycast_single_prepare_after_burst },
	{ "raycast_epochs",     "revision_monotonic",           case_revision_monotonic },
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
