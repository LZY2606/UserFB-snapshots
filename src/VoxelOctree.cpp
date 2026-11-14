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

#include "VoxelOctree.hpp"
#include "VoxelData.hpp"
#include "Debug.hpp"
#include "Util.hpp"

#include "third-party/lz4.h"

#include <algorithm>
#include <iostream>
#include <stdio.h>
#include <cmath>
#include <functional>
#include <unordered_map>
#include <vector>

static const uint32 BitCount[] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

static const size_t CompressionBlockSize = 64*1024*1024;

VoxelOctree::VoxelOctree(const char *path) : _voxels(0) {
    FILE *fp = fopen(path, "rb");

    if (fp) {

        fread(_center.a, sizeof(float), 3, fp);
        fread(&_octreeSize, sizeof(uint64), 1, fp);

        _octree.reset(new uint32[_octreeSize]);

        std::unique_ptr<char[]> buffer(new char[LZ4_compressBound(CompressionBlockSize)]);
        char *dst = reinterpret_cast<char *>(_octree.get());

        LZ4_streamDecode_t *stream = LZ4_createStreamDecode();
        LZ4_setStreamDecode(stream, dst, 0);

        uint64 compressedSize = 0;
        for (uint64 offset = 0; offset < _octreeSize*sizeof(uint32); offset += CompressionBlockSize) {
            uint64 compSize;
            fread(&compSize, sizeof(uint64), 1, fp);
            fread(buffer.get(), sizeof(char), size_t(compSize), fp);

            int outSize = std::min<int>(_octreeSize*sizeof(uint32) - offset, CompressionBlockSize);
            LZ4_decompress_fast_continue(stream, buffer.get(), dst + offset, outSize);
            compressedSize += compSize + 8;
        }
        LZ4_freeStreamDecode(stream);

        fclose(fp);

        std::cout << "Octree size: " << prettyPrintMemory(_octreeSize*sizeof(uint32))
                  << " Compressed size: " << prettyPrintMemory(compressedSize) << std::endl;
    }
}

void VoxelOctree::save(const char *path) {
    FILE *fp = fopen(path, "wb");

    if (fp) {
        fwrite(_center.a, sizeof(float), 3, fp);
        fwrite(&_octreeSize, sizeof(uint64), 1, fp);

        LZ4_stream_t *stream = LZ4_createStream();
        LZ4_resetStream(stream);

        std::unique_ptr<char[]> buffer(new char[LZ4_compressBound(CompressionBlockSize)]);
        const char *src = reinterpret_cast<char *>(_octree.get());

        uint64 compressedSize = 0;
        for (uint64 offset = 0; offset < _octreeSize*sizeof(uint32); offset += CompressionBlockSize) {
            int outSize = int(std::min(_octreeSize*sizeof(uint32) - offset, uint64(CompressionBlockSize)));
            uint64 compSize = LZ4_compress_continue(stream, src + offset, buffer.get(), outSize);

            fwrite(&compSize, sizeof(uint64), 1, fp);
            fwrite(buffer.get(), sizeof(char), size_t(compSize), fp);

            compressedSize += compSize + 8;
        }

        LZ4_freeStream(stream);

        fclose(fp);

        std::cout << "Octree size: " << prettyPrintMemory(_octreeSize*sizeof(uint32))
                  << " Compressed size: " << prettyPrintMemory(compressedSize) << std::endl;
    }
}

VoxelOctree::VoxelOctree(VoxelData *voxels)
: _voxels(voxels)
{
    std::unique_ptr<ChunkedAllocator<uint32>> octreeAllocator(new ChunkedAllocator<uint32>());
    octreeAllocator->pushBack(0);

    buildOctree(*octreeAllocator, 0, 0, 0, _voxels->sideLength(), 0);
    (*octreeAllocator)[0] |= 1 << 18;

    _octreeSize = octreeAllocator->size() + octreeAllocator->insertionCount();
    _octree = octreeAllocator->finalize();
    _center = _voxels->getCenter();
}

uint64 VoxelOctree::buildOctree(ChunkedAllocator<uint32> &allocator, int x, int y, int z, int size, uint64 descriptorIndex) {
    _voxels->prepareDataAccess(x, y, z, size);

    int halfSize = size >> 1;

    int posX[] = {x + halfSize, x, x + halfSize, x, x + halfSize, x, x + halfSize, x};
    int posY[] = {y + halfSize, y + halfSize, y, y, y + halfSize, y + halfSize, y, y};
    int posZ[] = {z + halfSize, z + halfSize, z + halfSize, z + halfSize, z, z, z, z};

    uint64 childOffset = uint64(allocator.size()) - descriptorIndex;

    int childCount = 0;
    int childIndices[8];
    uint32 childMask = 0;
    for (int i = 0; i < 8; i++) {
        if (_voxels->cubeContainsVoxelsDestructive(posX[i], posY[i], posZ[i], halfSize)) {
            childMask |= 128 >> i;
            childIndices[childCount++] = i;
        }
    }

    bool hasLargeChildren = false;
    uint32 leafMask;
    if (halfSize == 1) {
        leafMask = 0;

        for (int i = 0; i < childCount; i++) {
            int idx = childIndices[childCount - i - 1];
            allocator.pushBack(_voxels->getVoxelDestructive(posX[idx], posY[idx], posZ[idx]));
        }
    } else {
        leafMask = childMask;
        for (int i = 0; i < childCount; i++)
            allocator.pushBack(0);

        uint64 grandChildOffsets[8];
        uint64 delta = 0;
        uint64 insertionCount = allocator.insertionCount();
        for (int i = 0; i < childCount; i++) {
            int idx = childIndices[childCount - i - 1];
            grandChildOffsets[i] = delta + buildOctree(allocator, posX[idx], posY[idx], posZ[idx],
                halfSize, descriptorIndex + childOffset + i);
            delta += allocator.insertionCount() - insertionCount;
            insertionCount = allocator.insertionCount();
            if (grandChildOffsets[i] > 0x3FFF)
                hasLargeChildren = true;
        }

        for (int i = 0; i < childCount; i++) {
            uint64 childIndex = descriptorIndex + childOffset + i;
            uint64 offset = grandChildOffsets[i];
            if (hasLargeChildren) {
                offset += childCount - i;
                allocator.insert(childIndex + 1, uint32(offset));
                allocator[childIndex] |= 0x20000;
                offset >>= 32;
            }
            allocator[childIndex] |= uint32(offset << 18);
        }
    }

    allocator[descriptorIndex] = (childMask << 8) | leafMask;
    if (hasLargeChildren)
        allocator[descriptorIndex] |= 0x10000;

    return childOffset;
}

bool VoxelOctree::raymarch(const Vec3 &o, const Vec3 &d, float rayScale, uint32 &normal, float &t) {
    struct StackEntry {
        uint64 offset;
        float maxT;
    };
    StackEntry rayStack[MaxScale + 1];

    float ox = o.x, oy = o.y, oz = o.z;
    float dx = d.x, dy = d.y, dz = d.z;

    if (std::fabs(dx) < 1e-4f) dx = 1e-4f;
    if (std::fabs(dy) < 1e-4f) dy = 1e-4f;
    if (std::fabs(dz) < 1e-4f) dz = 1e-4f;

    float dTx = 1.0f/-std::fabs(dx);
    float dTy = 1.0f/-std::fabs(dy);
    float dTz = 1.0f/-std::fabs(dz);

    float bTx = dTx*ox;
    float bTy = dTy*oy;
    float bTz = dTz*oz;

    uint8 octantMask = 7;
    if (dx > 0.0f) octantMask ^= 1, bTx = 3.0f*dTx - bTx;
    if (dy > 0.0f) octantMask ^= 2, bTy = 3.0f*dTy - bTy;
    if (dz > 0.0f) octantMask ^= 4, bTz = 3.0f*dTz - bTz;

    float minT = std::max(2.0f*dTx - bTx, std::max(2.0f*dTy - bTy, 2.0f*dTz - bTz));
    float maxT = std::min(     dTx - bTx, std::min(     dTy - bTy,      dTz - bTz));
    minT = std::max(minT, 0.0f);

    uint32 current = 0;
    uint64 parent  = 0;
    int idx     = 0;
    float posX  = 1.0f;
    float posY  = 1.0f;
    float posZ  = 1.0f;
    int scale   = MaxScale - 1;

    float scaleExp2 = 0.5f;

    if (1.5f*dTx - bTx > minT) idx ^= 1, posX = 1.5f;
    if (1.5f*dTy - bTy > minT) idx ^= 2, posY = 1.5f;
    if (1.5f*dTz - bTz > minT) idx ^= 4, posZ = 1.5f;

    while (scale < MaxScale) {
        if (current == 0)
            current = _octree[parent];

        float cornerTX = posX*dTx - bTx;
        float cornerTY = posY*dTy - bTy;
        float cornerTZ = posZ*dTz - bTz;
        float maxTC = std::min(cornerTX, std::min(cornerTY, cornerTZ));

        int childShift = idx ^ octantMask;
        uint32 childMasks = current << childShift;

        if ((childMasks & 0x8000) && minT <= maxT) {
            if (maxTC*rayScale >= scaleExp2) {
                t = maxTC;
                return true;
            }

            float maxTV = std::min(maxT, maxTC);
            float half = scaleExp2*0.5f;
            float centerTX = half*dTx + cornerTX;
            float centerTY = half*dTy + cornerTY;
            float centerTZ = half*dTz + cornerTZ;

            if (minT <= maxTV) {
                uint64 childOffset = current >> 18;
                if (current & 0x20000)
                    childOffset = (childOffset << 32) | uint64(_octree[parent + 1]);

                if (!(childMasks & 0x80)) {
                    normal = _octree[childOffset + parent + BitCount[((childMasks >> (8 + childShift)) << childShift) & 127]];

                    break;
                }

                rayStack[scale].offset = parent;
                rayStack[scale].maxT = maxT;

                uint32 siblingCount = BitCount[childMasks & 127];
                parent += childOffset + siblingCount;
                if (current & 0x10000)
                    parent += siblingCount;

                idx = 0;
                scale--;
                scaleExp2 = half;

                if (centerTX > minT) idx ^= 1, posX += scaleExp2;
                if (centerTY > minT) idx ^= 2, posY += scaleExp2;
                if (centerTZ > minT) idx ^= 4, posZ += scaleExp2;

                maxT = maxTV;
                current = 0;

                continue;
            }
        }

        int stepMask = 0;
        if (cornerTX <= maxTC) stepMask ^= 1, posX -= scaleExp2;
        if (cornerTY <= maxTC) stepMask ^= 2, posY -= scaleExp2;
        if (cornerTZ <= maxTC) stepMask ^= 4, posZ -= scaleExp2;

        minT = maxTC;
        idx ^= stepMask;

        if ((idx & stepMask) != 0) {
            int differingBits = 0;
            if (stepMask & 1) differingBits |= floatBitsToUint(posX) ^ floatBitsToUint(posX + scaleExp2);
            if (stepMask & 2) differingBits |= floatBitsToUint(posY) ^ floatBitsToUint(posY + scaleExp2);
            if (stepMask & 4) differingBits |= floatBitsToUint(posZ) ^ floatBitsToUint(posZ + scaleExp2);
            scale = (floatBitsToUint((float)differingBits) >> 23) - 127;
            scaleExp2 = uintBitsToFloat((scale - MaxScale + 127) << 23);

            parent = rayStack[scale].offset;
            maxT   = rayStack[scale].maxT;

            int shX = floatBitsToUint(posX) >> scale;
            int shY = floatBitsToUint(posY) >> scale;
            int shZ = floatBitsToUint(posZ) >> scale;
            posX = uintBitsToFloat(shX << scale);
            posY = uintBitsToFloat(shY << scale);
            posZ = uintBitsToFloat(shZ << scale);
            idx = (shX & 1) | ((shY & 1) << 1) | ((shZ & 1) << 2);

            current = 0;
        }
    }

    if (scale >= MaxScale)
        return false;

    t = minT;
    return true;
}

namespace {

// One parsed descriptor of the old layout. Only records reachable from the
// root are ever visited. A node owns one child block: the contiguous group of
// child records referenced by its descriptor.
struct CompactNode {
    uint32 childMask;
    uint32 leafMask;
    uint32 depth;
    int recordCount;
    bool recordIsLeaf[8];   // per stored record, in storage (decreasing octant) order
    uint32 recordValue[8];  // complete 32-bit material for leaves
    int recordChild[8];     // parsed node index for internal children
    int canonical;
};

// Full identity of a subtree: the current depth, the occupancy of the eight
// ordered children, and the identity of every child (complete material word
// for leaves, canonical child identity otherwise). Records stay in their
// stored order, so octant positions are never sorted away.
struct SubtreeKey {
    uint32 depth;
    uint32 childMask;
    uint32 leafMask;
    uint64 rec[8]; // leaf: (material << 1) | 1, internal: canonical << 1

    bool operator==(const SubtreeKey &o) const {
        if (depth != o.depth || childMask != o.childMask || leafMask != o.leafMask)
            return false;
        int count = BitCount[childMask];
        for (int i = 0; i < count; i++)
            if (rec[i] != o.rec[i])
                return false;
        return true;
    }
};

struct SubtreeKeyHash {
    size_t operator()(const SubtreeKey &k) const {
        uint64 h = 0x9E3779B97F4A7C15ull;
        h = (h ^ k.depth)*0x100000001B3ull;
        h = (h ^ k.childMask)*0x100000001B3ull;
        h = (h ^ k.leafMask)*0x100000001B3ull;
        int count = BitCount[k.childMask];
        for (int i = 0; i < count; i++)
            h = (h ^ k.rec[i])*0x100000001B3ull;
        return size_t(h);
    }
};

// One unique subtree after deduplication. Its child block is stored exactly
// once; the descriptor records referencing it may appear in many parent blocks.
struct CanonicalNode {
    uint32 childMask;
    uint32 leafMask;
    int recordCount;
    bool recordIsLeaf[8];
    uint32 recordValue[8];
    int recordChild[8]; // canonical id of internal children
    uint64 blockAddr;   // assigned during layout
    bool longRecords;   // 0x10000: this node's child records come in pairs
};

} // namespace

VoxelOctree::CompactStats VoxelOctree::compact() {
    CompactStats stats;
    stats.wordsBefore = _octreeSize;
    stats.wordsAfter = _octreeSize;
    stats.uniqueBlocks = 0;
    stats.sharedReferences = 0;

    if (!_octree || _octreeSize == 0)
        return stats;

    const uint32 *old = _octree.get();

    // Parse the reachable records. Already-shared subtrees (from a previous
    // compact) are visited once per child block via this memo.
    std::vector<CompactNode> nodes;
    std::unordered_map<uint64, int> parsed;

    std::function<int(uint64, uint32)> parseNode = [&](uint64 recordAddr, uint32 depth) -> int {
        uint32 desc = old[recordAddr];
        uint64 childOffset = desc >> 18;
        if (desc & 0x20000)
            childOffset = (childOffset << 32) | uint64(old[recordAddr + 1]);
        uint64 blockAddr = recordAddr + childOffset;

        uint64 memoKey = (blockAddr << 6) | uint64(depth & 63);
        auto it = parsed.find(memoKey);
        if (it != parsed.end())
            return it->second;

        int id = int(nodes.size());
        nodes.push_back(CompactNode());
        // NOTE: recursion below may reallocate `nodes`; never hold a reference.
        nodes[id].childMask = (desc >> 8) & 0xFF;
        nodes[id].leafMask = desc & 0xFF;
        nodes[id].depth = depth;
        nodes[id].recordCount = BitCount[nodes[id].childMask];
        nodes[id].canonical = -1;

        uint64 stride = (desc & 0x10000) ? 2 : 1;
        int record = 0;
        for (int octant = 7; octant >= 0; octant--) {
            if (!(nodes[id].childMask & (128 >> octant)))
                continue;
            uint64 recAddr = blockAddr + uint64(record)*stride;
            uint32 w = old[recAddr];
            // leafMask bit set means the child is an internal node (matches
            // buildOctree and raymarch); a clear bit marks a leaf material.
            bool isLeaf = (nodes[id].leafMask & (128 >> octant)) == 0;
            nodes[id].recordIsLeaf[record] = isLeaf;
            if (isLeaf) {
                nodes[id].recordValue[record] = w;
                nodes[id].recordChild[record] = -1;
            } else {
                int childId = parseNode(recAddr, depth + 1);
                nodes[id].recordValue[record] = 0;
                nodes[id].recordChild[record] = childId;
            }
            record++;
        }

        parsed[memoKey] = id;
        return id;
    };

    int rootId = parseNode(0, 0);

    // Canonicalize bottom-up: a subtree identity can only be completed once
    // the identities of its children are known.
    std::vector<int> order(nodes.size());
    for (size_t i = 0; i < nodes.size(); i++)
        order[i] = int(i);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return nodes[a].depth > nodes[b].depth;
    });

    std::vector<CanonicalNode> canonNodes;
    std::unordered_map<SubtreeKey, int, SubtreeKeyHash> canonMap;
    for (size_t i = 0; i < order.size(); i++) {
        CompactNode &node = nodes[order[i]];
        SubtreeKey key;
        key.depth = node.depth;
        key.childMask = node.childMask;
        key.leafMask = node.leafMask;
        for (int r = 0; r < node.recordCount; r++) {
            if (node.recordIsLeaf[r])
                key.rec[r] = (uint64(node.recordValue[r]) << 1) | 1;
            else
                key.rec[r] = uint64(nodes[node.recordChild[r]].canonical) << 1;
        }
        auto it = canonMap.find(key);
        if (it != canonMap.end()) {
            node.canonical = it->second;
        } else {
            int canonId = int(canonNodes.size());
            canonNodes.push_back(CanonicalNode());
            CanonicalNode &c = canonNodes.back();
            c.childMask = node.childMask;
            c.leafMask = node.leafMask;
            c.recordCount = node.recordCount;
            c.blockAddr = 0;
            c.longRecords = false;
            for (int r = 0; r < node.recordCount; r++) {
                c.recordIsLeaf[r] = node.recordIsLeaf[r];
                c.recordValue[r] = node.recordValue[r];
                c.recordChild[r] = node.recordIsLeaf[r] ? -1 : nodes[node.recordChild[r]].canonical;
            }
            canonMap[key] = canonId;
            node.canonical = canonId;
        }
    }

    // Emit blocks breadth-first from the root so that offsets stay small and
    // every canonical node sits at a fixed depth level. Each unique child
    // block is emitted exactly once, no matter how many parents reference it.
    std::vector<int> emitOrder;
    std::vector<char> emitted(canonNodes.size(), 0);
    if (nodes[rootId].recordCount > 0) {
        emitOrder.push_back(nodes[rootId].canonical);
        emitted[nodes[rootId].canonical] = 1;
        for (size_t i = 0; i < emitOrder.size(); i++) {
            const CanonicalNode &c = canonNodes[emitOrder[i]];
            for (int r = 0; r < c.recordCount; r++) {
                if (c.recordIsLeaf[r] || emitted[c.recordChild[r]])
                    continue;
                emitted[c.recordChild[r]] = 1;
                emitOrder.push_back(c.recordChild[r]);
            }
        }
    }

    // Assign addresses. Whether a node's child records need the long (paired)
    // form depends on the final offsets, which in turn depend on record sizes,
    // so iterate to a fixpoint. Sizes and offsets only ever grow, hence this
    // terminates.
    bool changed = true;
    uint64 totalSize = 1;
    while (changed) {
        changed = false;
        uint64 addr = 1;
        for (size_t i = 0; i < emitOrder.size(); i++) {
            CanonicalNode &c = canonNodes[emitOrder[i]];
            c.blockAddr = addr;
            addr += uint64(c.recordCount)*(c.longRecords ? 2 : 1);
        }
        totalSize = addr;
        for (size_t i = 0; i < emitOrder.size(); i++) {
            CanonicalNode &c = canonNodes[emitOrder[i]];
            if (c.longRecords)
                continue;
            uint64 stride = 1;
            for (int r = 0; r < c.recordCount; r++) {
                if (c.recordIsLeaf[r])
                    continue;
                uint64 recAddr = c.blockAddr + uint64(r)*stride;
                uint64 offset = canonNodes[c.recordChild[r]].blockAddr - recAddr;
                if (offset > 0x3FFF) {
                    c.longRecords = true;
                    changed = true;
                    break;
                }
            }
        }
    }

    // Emit the new layout. The root stays in word 0 and every offset is
    // recomputed against the final addresses.
    std::unique_ptr<uint32[]> compacted(new uint32[totalSize]);
    {
        const CanonicalNode &root = canonNodes[nodes[rootId].canonical];
        uint32 desc = (root.childMask << 8) | root.leafMask | (1 << 18);
        if (root.longRecords)
            desc |= 0x10000;
        compacted[0] = desc;
    }
    uint64 totalReferences = 0;
    for (size_t i = 0; i < emitOrder.size(); i++) {
        const CanonicalNode &c = canonNodes[emitOrder[i]];
        uint64 stride = c.longRecords ? 2 : 1;
        for (int r = 0; r < c.recordCount; r++) {
            uint64 recAddr = c.blockAddr + uint64(r)*stride;
            if (c.recordIsLeaf[r]) {
                compacted[recAddr] = c.recordValue[r];
                continue;
            }
            totalReferences++;
            const CanonicalNode &child = canonNodes[c.recordChild[r]];
            uint64 offset = child.blockAddr - recAddr;
            uint32 desc = (child.childMask << 8) | child.leafMask;
            if (child.longRecords)
                desc |= 0x10000;
            if (c.longRecords) {
                desc |= 0x20000 | (uint32(offset >> 32) << 18);
                compacted[recAddr] = desc;
                compacted[recAddr + 1] = uint32(offset);
            } else {
                compacted[recAddr] = desc | (uint32(offset) << 18);
            }
        }
    }

    _octree = std::move(compacted);
    _octreeSize = totalSize;

    stats.wordsAfter = totalSize;
    stats.uniqueBlocks = emitOrder.size();
    if (!emitOrder.empty())
        totalReferences++; // the root descriptor references the first block
    stats.sharedReferences = totalReferences - stats.uniqueBlocks;
    return stats;
}
