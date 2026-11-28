/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2026 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software to the public domain worldwide. This software is distributed *
* without any warranty.                                                                            *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/

// A windowless example of continuous editing with ray picking, as might be
// done by a voxel editor which picks the voxel under the mouse cursor every
// frame while the user paints. It demonstrates the RaycastContext class,
// which caches the data needed for raycasting and only re-extracts it when
// the volume has actually changed (including changes from undo/redo and from
// background compaction via bake()).
//
// Build (from the repository root):
//   c++ -std=c++20 -Isrc/library examples/pick_and_edit.cpp src/library/*.cpp -o pick_and_edit
// (or build the 'PickAndEditExample' target via CMake)

#include "raytracing.h"
#include "storage.h"

#include <cstdio>

int main()
{
	using namespace Cubiquity;

	Volume volume;
	RaycastContext pickContext;

	// A fixed 'camera' position. Each frame the 'cursor' ray is cast from
	// here towards the voxel which was just painted.
	const vec3 camera(-50.0f, 40.0f, 60.0f);

	std::printf("Simulated editing session (paint, pick, checkpoint, bake, undo):\n");

	// Simulate an editing session in which the user paints a wall of voxels.
	// Each 'frame' we apply an edit and then pick at the cursor position.
	for (int frame = 0; frame < 32; frame++) {

		// Apply this frame's edit (painting a vertical wall, two voxels
		// per frame so that every occupied octant keeps branching).
		const MaterialId material = MaterialId(3 + frame % 5);
		volume.setVoxel(frame, 10, 0, material);
		volume.setVoxel(frame, 11, 0, material);

		// Periodically checkpoint (for undo) and compact in the background.
		if (frame % 8 == 7) { volume.checkpoint(); }
		if (frame % 16 == 15) { volume.bake(); }

		// Pick under the cursor (aimed at the voxel just painted). The
		// context transparently re-prepares its cached data only when the
		// volume has changed since the previous pick.
		const vec3 target(float(frame), 10.0f, 0.0f);
		Ray3f cursorRay(camera, normalize(target - camera));
		RayVolumeIntersection pick = intersectVolume(volume, pickContext, cursorRay, true);
		if (pick.hit) {
			std::printf("  frame %2d: picked material %u at distance %.1f\n",
				frame, pick.material, pick.distance);
		} else {
			std::printf("  frame %2d: nothing under cursor\n", frame);
		}
	}

	// The user hits 'undo' a few times (each checkpoint covers eight frames
	// of painting). Picking still works, and the context notices each undo
	// because the volume revision changes. The cursor keeps pointing at the
	// far end of the wall, so the picked result changes as voxels are removed.
	std::printf("After undoing (cursor still aimed at the far end of the wall):\n");
	const vec3 endOfWall(28.0f, 10.0f, 0.0f);
	Ray3f cursorRay(camera, normalize(endOfWall - camera));
	for (int i = 0; i < 4; i++) {
		volume.undo();
		RayVolumeIntersection pick = intersectVolume(volume, pickContext, cursorRay, true);
		if (pick.hit) {
			std::printf("  undo %d: picked material %u at distance %.1f\n",
				i + 1, pick.material, pick.distance);
		} else {
			std::printf("  undo %d: nothing under cursor (wall was shortened)\n", i + 1);
		}
	}

	// The cache was only re-prepared once per actual change, not once per pick.
	std::printf("Total cache preparations: %llu\n",
		static_cast<unsigned long long>(pickContext.preparationCount()));

	return 0;
}
