/*
Copyright (c) 2013 Benedikt Bitterli

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#ifndef RAWOCTREEBUILDER_HPP_
#define RAWOCTREEBUILDER_HPP_

#include "math/Vec3.hpp"

#include "IntTypes.hpp"

#include <cstddef>
#include <memory>

struct BuildStats;

// Builds a VoxelOctree descriptor array directly from a raw voxel file
// (three int32 dimensions followed by uint32 materials, 0 = empty, x varying
// fastest) using up to `workers` concurrent subtree builder threads.
//
// The shared occupancy top LUT is built by a single pre-scan of the file and
// is only read afterwards; each worker gets its own low LUT, destructive
// voxel window and file read position. cacheBytes bounds the simultaneously
// live input cache arrays (top LUT + low LUTs + voxel windows); the final
// tree and descriptor assembly space are not counted. The input file is only
// read, never modified.
//
// Throws std::invalid_argument if workers == 0 or cacheBytes cannot hold the
// shared occupancy index plus at least one worker window, and
// std::runtime_error on I/O failures.
std::unique_ptr<uint32[]> buildOctreeFromRaw(const char *rawPath, size_t cacheBytes,
        unsigned workers, uint64 &octreeSize, Vec3 &center, BuildStats *stats);

#endif /* RAWOCTREEBUILDER_HPP_ */
