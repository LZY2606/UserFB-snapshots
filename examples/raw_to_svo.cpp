/*
Windowless usage example for the parallel raw -> SVO build path. No SDL, no
viewer: converts a raw voxel volume directly into a compressed .oct file.

Usage:
    raw_to_svo input.raw output.oct [cacheBytes] [workers]

Raw input layout: three int32 dimensions (width, height, depth), followed by
width*height*depth uint32 materials (0 = empty), x varying fastest.

Build (from the repository root):
    c++ -std=c++11 -O2 -pthread -Isrc examples/raw_to_svo.cpp \
        src/RawOctreeBuilder.cpp src/VoxelOctree.cpp src/VoxelData.cpp \
        src/Util.cpp src/Debug.cpp src/PlyLoader.cpp \
        src/thread/ThreadPool.cpp src/thread/ThreadUtils.cpp \
        src/third-party/lz4.c src/third-party/plyfile.c \
        src/third-party/tribox3.c -o raw_to_svo
*/

#include "VoxelOctree.hpp"

#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s input.raw output.oct [cacheBytes] [workers]\n", argv[0]);
        return 1;
    }

    size_t cacheBytes = argc > 3 ? size_t(std::strtoull(argv[3], 0, 0)) : size_t(64)*1024*1024;
    unsigned workers = argc > 4 ? unsigned(std::strtoul(argv[4], 0, 0)) : 4;

    BuildStats stats;
    VoxelOctree tree(argv[1], cacheBytes, workers, &stats);
    tree.save(argv[2]);

    std::printf("Occupancy prescans: %llu, workers used: %llu, peak input cache: %llu bytes\n",
            (unsigned long long)stats.topLutBuilds,
            (unsigned long long)stats.workersUsed,
            (unsigned long long)stats.peakInputCacheBytes);
    return 0;
}
