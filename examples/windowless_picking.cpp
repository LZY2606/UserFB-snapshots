// Windowless continuous-picking example.
//
// Demonstrates how a long-lived RaycastContext can be reused while a volume is
// edited, checkpointed, undone/redone and compacted. This is the pattern used
// for interactive tools (e.g. a brush preview or a "which voxel is under the
// cursor" highlight) which query the volume every frame without creating a
// window or a GPU context.
//
// Build (from the repository root):
//   c++ -std=c++20 -O2 -Isrc/library examples/windowless_picking.cpp \
//       <library sources or core.a> -o windowless_picking

#include "raytracing.h"
#include "storage.h"

#include <cstdio>

using namespace Cubiquity;

// A stand-in for the camera/controller which would normally supply a ray.
static Ray3f cursorRay(float x, float y)
{
	return Ray3f(vec3f(x, y, -40.0f), vec3f(0.001f, 0.002f, 1.0f));
}

int main()
{
	Volume volume;

	// Seed a small scene (two adjacent voxels to avoid degenerate chains).
	volume.setVoxel(0, 0, 0, 3);
	volume.setVoxel(1, 0, 0, 4);
	volume.setVoxel(0, 1, 0, 5);
	volume.setVoxel(1, 1, 0, 6);
	volume.bake();

	// One context for the lifetime of the tool. It is default-constructible and
	// rebinds automatically if it is later passed a different volume.
	RaycastContext picker;

	// Simulate frames. Each frame performs a pick; every few frames the user
	// paints (edits, then checkpoints) or undoes.
	for (int frame = 0; frame < 12; frame++) {
		if (frame == 4) {
			// Paint under the cursor: edit, then record an undo step.
			volume.checkpoint();
			volume.setVoxel(0, 0, 1, 7);
		}
		if (frame == 8) {
			volume.undo();
		}
		if (frame == 10) {
			// Compaction can run in the background without invalidating the
			// context's cached subDAGs, because the volume revision changes.
			volume.bake();
		}

		const RayVolumeIntersection hit =
			intersectVolume(volume, picker, cursorRay(0.5f, 0.5f), true);

		std::printf("frame %2d: hit=%d material=%u distance=%.3f cached_preparations=%llu\n",
			frame, hit.hit, hit.material, hit.distance,
			static_cast<unsigned long long>(picker.preparationCount()));
	}

	std::printf("Total cache preparations: %llu (one per visible scene change)\n",
		static_cast<unsigned long long>(picker.preparationCount()));
	return 0;
}
