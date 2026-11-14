/***************************************************************************************************
* Cubiquity - A micro-voxel engine for games and other interactive applications                    *
*                                                                                                  *
* Written in 2023 by David Williams                                                                *
*                                                                                                  *
* To the extent possible under law, the author(s) have dedicated all copyright and related and     *
* neighboring rights to this software to the public domain worldwide. This software is distributed *
* without any warranty.                                                                            *
*                                                                                                  *
* You should have received a copy of the CC0 Public Domain Dedication along with this software.    *
* If not, see http://creativecommons.org/publicdomain/zero/1.0/.                                   *
***************************************************************************************************/
#ifndef CUBIQUITY_RAYTRACING_H
#define CUBIQUITY_RAYTRACING_H

#include "geometry.h"
#include "storage.h"

#include "geometry.h"

#include <array>
#include <cassert>
#include <climits>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <algorithm>
#include <cstdint>
#include <numeric>

namespace Cubiquity
{
	using namespace Internals;

	typedef vec3f vec3;
	typedef vec3i ivec3;
	typedef vec3u uvec3;
	typedef vec3b bvec3;

	// This structure contains an explicit 'hit' member rather than relying on a sentinal value
	// such as a negative distance or a material of zero. I have found this simplifies the code
	// and we might want to skip material identification anyway (if traversal is stopped early
	// due to LOD). There is some redundancy storing both the distance and the position, but we
	// get the former basically for free as we traverse and the latter is very often useful.
	struct RayVolumeIntersection
	{
		bool hit;
		double distance;
		uint material;
		vec3 position;
		vec3 normal;
	};

	struct SubDAG
	{
		ivec3 lowerBound;
		i32 nodeHeight;

		uint padding0;
		uint nodeIndex;
		uint padding1, padding2;
	};

	typedef std::array<SubDAG, 8> SubDAGArray;

	SubDAGArray findSubDAGs(const Internals::NodeStore& nodes, u32 rootNodeIndex);

	// Caches the SubDAGArray for the volume it was last asked to trace against.
	// Callers should keep the context alive for the duration of their raycasting
	// and reuse it between queries; the cache is refreshed on the first query and
	// whenever the bound volume's revision changes. It is deliberately keyed on
	// the volume revision rather than the root index, because nodes can be
	// modified in place and compaction can leave the root index unchanged.
	//
	// The context does not extend the lifetime of the volume: the volume must
	// outlive the context's last use. It is not safe for concurrent use.
	class RaycastContext
	{
	public:
		RaycastContext() = default;

		// Number of times the cached SubDAGArray has actually been prepared.
		u64 preparationCount() const { return mPreparationCount; }

	private:
		friend RayVolumeIntersection intersectVolume(const Volume& volume, RaycastContext& context,
			float ray_orig_x, float ray_orig_y, float ray_orig_z,
			float ray_dir_x, float ray_dir_y, float ray_dir_z,
			bool computeSurfaceProperties, float maxFootprint);
		friend RayVolumeIntersection intersectVolume(const Volume& volume, RaycastContext& context,
			Ray3f ray, bool computeSurfaceProperties, float maxFootprint);

		// Ensures 'subDAGs' matches the current state of 'volume'.
		const SubDAGArray& prepare(const Volume& volume);

		const Volume* mVolume = nullptr;
		u64 mRevision = 0;
		u64 mPreparationCount = 0;
		SubDAGArray mSubDAGs{};
	};

	const float MAX_FOOTPRINT_DISABLED = -1.0f;
	RayVolumeIntersection intersectVolume(const Volume& volume, const SubDAGArray& subDAGs,
		float ray_orig_x, float ray_orig_y, float ray_orig_z,
		float ray_dir_x, float ray_dir_y, float ray_dir_z,
		bool computeSurfaceProperties, float maxFootprint = MAX_FOOTPRINT_DISABLED);
	RayVolumeIntersection intersectVolume(const Volume& volume, const SubDAGArray& subDAGs, Ray3f ray, bool computeSurfaceProperties, float maxFootprint = MAX_FOOTPRINT_DISABLED);

	// Context-based overloads. These use (and maintain) the context's cache of the
	// SubDAGArray, avoiding the need for the caller to recompute it on every query.
	RayVolumeIntersection intersectVolume(const Volume& volume, RaycastContext& context,
		float ray_orig_x, float ray_orig_y, float ray_orig_z,
		float ray_dir_x, float ray_dir_y, float ray_dir_z,
		bool computeSurfaceProperties, float maxFootprint = MAX_FOOTPRINT_DISABLED);
	RayVolumeIntersection intersectVolume(const Volume& volume, RaycastContext& context, Ray3f ray, bool computeSurfaceProperties, float maxFootprint = MAX_FOOTPRINT_DISABLED);

	RayVolumeIntersection traceRayRef(Volume& volume,
		float ray_orig_x, float ray_orig_y, float ray_orig_z,
		float ray_dir_x, float ray_dir_y, float ray_dir_z);
	RayVolumeIntersection traceRayRef(Volume& volume, Ray3f ray);
}

#endif // CUBIQUITY_RAYTRACING_H
