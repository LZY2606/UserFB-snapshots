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

#include "RawOctreeBuilder.hpp"

#include "VoxelOctree.hpp"
#include "ChunkedAllocator.hpp"
#include "Util.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <stdio.h>

namespace {

uint64 countCellsInHierarchicalGrid(int numLevels)
{
    uint64 result = 0;
    uint64 size = 1;
    for (int i = 0; i < numLevels; ++i) {
        result += size;
        size *= 8;
    }
    return result;
}

void buildHierarchicalGridLinks(uint8 *base, int numLevels, std::vector<uint8 *> &dst)
{
    size_t offset = 0;
    size_t size = 1;
    for (int i = 0; i < numLevels; ++i) {
        dst.push_back(base + offset);
        offset += size;
        size *= 8;
    }
}

inline size_t lutIdx(int l, int x, int y, int z)
{
    return size_t(x) + (size_t(y) << l) + (size_t(z) << 2*l);
}

void upsampleLutLevel(const std::vector<uint8 *> &table, int l)
{
    int size = 2 << l;
    for (int z = 0; z < size; z += 2)
        for (int y = 0; y < size; y += 2)
            for (int x = 0; x < size; x += 2) {
                int value =
                    table[l + 1][lutIdx(l + 1, x + 0, y + 0, z + 0)] |
                    table[l + 1][lutIdx(l + 1, x + 1, y + 0, z + 0)] |
                    table[l + 1][lutIdx(l + 1, x + 0, y + 1, z + 0)] |
                    table[l + 1][lutIdx(l + 1, x + 1, y + 1, z + 0)] |
                    table[l + 1][lutIdx(l + 1, x + 0, y + 0, z + 1)] |
                    table[l + 1][lutIdx(l + 1, x + 1, y + 0, z + 1)] |
                    table[l + 1][lutIdx(l + 1, x + 0, y + 1, z + 1)] |
                    table[l + 1][lutIdx(l + 1, x + 1, y + 1, z + 1)];

                table[l][lutIdx(l, x >> 1, y >> 1, z >> 1)] = (value != 0);
            }
}

void seekFile(FILE *fp, uint64 offset)
{
#ifdef _MSC_VER
    _fseeki64(fp, offset, SEEK_SET);
#elif __APPLE__
    fseeko(fp, offset, SEEK_SET);
#else
    fseeko64(fp, offset, SEEK_SET);
#endif
}

// Reads a w*h*d block of uint32 voxels starting at (x, y, z) from a raw file
// with the three-int32 dimension header. dst is indexed x-fastest and must
// have room for w*h*d elements.
void readBlock(FILE *fp, int dataW, int dataH, uint32 *dst,
        int x, int y, int z, int w, int h, int d)
{
    uint64 zStride = uint64(dataW)*uint64(dataH);
    uint64 yStride = uint64(dataW);
    uint64 baseOffset = (uint64(x) + uint64(y)*yStride + uint64(z)*zStride)*sizeof(uint32)
            + 3*sizeof(uint32);

    for (int vz = 0; vz < d; ++vz) {
        uint64 offset = baseOffset;
        for (int vy = 0; vy < h; ++vy) {
            seekFile(fp, offset);
            if (fread(dst + (size_t(vy) + size_t(vz)*size_t(h))*size_t(w), sizeof(uint32), w, fp) != size_t(w))
                throw std::runtime_error("Unexpected end of raw voxel file");
            offset += yStride*sizeof(uint32);
        }
        baseOffset += zStride*sizeof(uint32);
    }
}

// Descriptor words and deferred insertions produced for one subtree. All
// indices are relative to the subtree root descriptor, so the result can be
// spliced into the final tree at any base index.
struct SubtreeDescriptors {
    std::vector<uint32> words;
    std::vector<std::pair<uint64, uint32>> insertions;

    uint64 size() const { return uint64(words.size()); }
    uint64 insertionCount() const { return uint64(insertions.size()); }

    void pushBack(uint32 value) { words.push_back(value); }
    uint32 &operator[](uint64 i) { return words[size_t(i)]; }
    void insert(uint64 index, uint32 value) { insertions.push_back(std::make_pair(index, value)); }
};

class RawParallelBuilder {
    // Sanity cap on the number of spawned threads; the "workers" argument
    // only bounds how many subtree builders may run concurrently.
    static const unsigned MaxWorkerThreads = 256;
    enum { NilTask = 0xFFFFFFFFu };

    std::string _path;

    int _dataW, _dataH, _dataD;
    int _virtualW, _virtualH, _virtualD;
    int _highestVirtualBit;
    int _sideLength;

    int _lowLutLevels;
    int _topLutLevels;
    unsigned _partitionWorkers;
    uint64 _topLutBytes;
    uint64 _lowLutBytes;
    uint64 _windowVoxelBytes;

    // Shared, read-only after the pre-scan
    std::unique_ptr<uint8[]> _topLut;
    std::vector<uint8 *> _topTable;

    struct BlockTask { int x, y, z; };
    std::vector<BlockTask> _tasks;
    std::vector<SubtreeDescriptors> _subtrees;
    std::vector<uint32> _blockToTask;
    int _blockGridDim;

    std::atomic<uint32> _workersUsed;

    uint64 _octreeSize;
    std::unique_ptr<uint32[]> _octree;
    Vec3 _center;
    uint64 _peakInputCacheBytes;

    int windowSize() const { return 1 << _lowLutLevels; }

    // Per-worker state: independent read position, destructive voxel cache
    // and low LUT. Nothing in here is shared between workers.
    struct WorkerState {
        FILE *fp;
        std::unique_ptr<uint32[]> voxels;
        std::unique_ptr<uint8[]> lowLut;
        std::vector<uint8 *> lowTable;
        int bufX, bufY, bufZ;
        int bufW, bufH, bufD;

        WorkerState() : fp(0), bufX(0), bufY(0), bufZ(0), bufW(0), bufH(0), bufD(0) {}
    };

    uint32 getVoxel(const WorkerState &ws, int x, int y, int z) const
    {
        if (x >= _dataW || y >= _dataH || z >= _dataD)
            return 0;
        size_t idx = size_t(x - ws.bufX)
                + size_t(ws.bufW)*(size_t(y - ws.bufY) + size_t(ws.bufH)*size_t(z - ws.bufZ));
        return ws.voxels[idx];
    }

    uint32 getVoxelDestructive(WorkerState &ws, int x, int y, int z) const
    {
        if (x >= _dataW || y >= _dataH || z >= _dataD)
            return 0;
        size_t idx = size_t(x - ws.bufX)
                + size_t(ws.bufW)*(size_t(y - ws.bufY) + size_t(ws.bufH)*size_t(z - ws.bufZ));
        uint32 value = ws.voxels[idx];
        if (value)
            ws.voxels[idx] = 0;
        return value;
    }

    // Occupancy query for cubes strictly smaller than the cache window, using
    // the worker-local low LUT and voxel cache. Mirrors
    // VoxelData::cubeContainsVoxelsDestructive.
    bool cubeContainsVoxelsDestructive(WorkerState &ws, int x, int y, int z, int size) const
    {
        if (x >= _dataW || y >= _dataH || z >= _dataD)
            return false;
        if (size == 1)
            return getVoxel(ws, x, y, z) != 0;

        int bit = findHighestBit(uint32(size));
        int level = _lowLutLevels - bit;
        uint8 &cell = ws.lowTable[level][lutIdx(level,
                (x - ws.bufX) >> bit, (y - ws.bufY) >> bit, (z - ws.bufZ) >> bit)];
        uint8 value = cell;
        if (value)
            cell = 0;
        return value != 0;
    }

    // Builds the worker-local low LUT for the currently cached window. The
    // depth loop covers the full buffered depth, including odd-sized tail
    // blocks of non power-of-two volumes.
    void buildLowLut(WorkerState &ws) const
    {
        int leafLevel = _lowLutLevels - 1;
        for (int z = 0; z < ws.bufD; ++z)
            for (int y = 0; y < ws.bufH; ++y)
                for (int x = 0; x < ws.bufW; ++x)
                    if (ws.voxels[size_t(x) + size_t(ws.bufW)*(size_t(y) + size_t(ws.bufH)*size_t(z))])
                        ws.lowTable[leafLevel][lutIdx(leafLevel, x >> 1, y >> 1, z >> 1)] = 1;

        for (int l = _lowLutLevels - 2; l >= 0; --l)
            upsampleLutLevel(ws.lowTable, l);
    }

    // Local clone of VoxelOctree::buildOctree operating on the worker's
    // window. Produces exactly the descriptor sequence the serial build
    // emits for this subtree; offsets relative to the subtree root are
    // translation invariant and are finalized during the merge.
    uint64 buildNode(SubtreeDescriptors &tree, WorkerState &ws,
            int x, int y, int z, int size, uint64 descriptorIndex)
    {
        int halfSize = size >> 1;

        int posX[] = {x + halfSize, x, x + halfSize, x, x + halfSize, x, x + halfSize, x};
        int posY[] = {y + halfSize, y + halfSize, y, y, y + halfSize, y + halfSize, y, y};
        int posZ[] = {z + halfSize, z + halfSize, z + halfSize, z + halfSize, z, z, z, z};

        uint64 childOffset = uint64(tree.size()) - descriptorIndex;

        int childCount = 0;
        int childIndices[8];
        uint32 childMask = 0;
        for (int i = 0; i < 8; i++) {
            if (cubeContainsVoxelsDestructive(ws, posX[i], posY[i], posZ[i], halfSize)) {
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
                tree.pushBack(getVoxelDestructive(ws, posX[idx], posY[idx], posZ[idx]));
            }
        } else {
            leafMask = childMask;
            for (int i = 0; i < childCount; i++)
                tree.pushBack(0);

            uint64 grandChildOffsets[8];
            uint64 delta = 0;
            uint64 insertionCount = tree.insertionCount();
            for (int i = 0; i < childCount; i++) {
                int idx = childIndices[childCount - i - 1];
                grandChildOffsets[i] = delta + buildNode(tree, ws, posX[idx], posY[idx], posZ[idx],
                    halfSize, descriptorIndex + childOffset + i);
                delta += tree.insertionCount() - insertionCount;
                insertionCount = tree.insertionCount();
                if (grandChildOffsets[i] > 0x3FFF)
                    hasLargeChildren = true;
            }

            for (int i = 0; i < childCount; i++) {
                uint64 childIndex = descriptorIndex + childOffset + i;
                uint64 offset = grandChildOffsets[i];
                if (hasLargeChildren) {
                    offset += childCount - i;
                    tree.insert(childIndex + 1, uint32(offset));
                    tree[childIndex] |= 0x20000;
                    offset >>= 32;
                }
                tree[childIndex] |= uint32(offset << 18);
            }
        }

        tree[descriptorIndex] = (childMask << 8) | leafMask;
        if (hasLargeChildren)
            tree[descriptorIndex] |= 0x10000;

        return childOffset;
    }

    void buildSubtreeBlock(WorkerState &ws, const BlockTask &task, SubtreeDescriptors &out)
    {
        int window = windowSize();
        int w = std::min(window, _dataW - task.x);
        int h = std::min(window, _dataH - task.y);
        int d = std::min(window, _dataD - task.z);

        readBlock(ws.fp, _dataW, _dataH, ws.voxels.get(), task.x, task.y, task.z, w, h, d);
        ws.bufX = task.x;
        ws.bufY = task.y;
        ws.bufZ = task.z;
        ws.bufW = w;
        ws.bufH = h;
        ws.bufD = d;

        std::memset(ws.lowLut.get(), 0, size_t(_lowLutBytes));
        buildLowLut(ws);

        SubtreeDescriptors tree;
        tree.pushBack(0);
        buildNode(tree, ws, task.x, task.y, task.z, window, 0);
        out.words = std::move(tree.words);
        out.insertions = std::move(tree.insertions);
    }

    void workerMain(unsigned taskBegin, unsigned taskEnd)
    {
        if (taskBegin >= taskEnd)
            return;

        WorkerState ws;
        ws.fp = fopen(_path.c_str(), "rb");
        if (!ws.fp)
            throw std::runtime_error("Failed to open raw voxel file in worker thread");

        size_t windowCells = size_t(1) << (3*_lowLutLevels);
        ws.voxels.reset(new uint32[windowCells]);
        ws.lowLut.reset(new uint8[size_t(_lowLutBytes)]);
        buildHierarchicalGridLinks(ws.lowLut.get(), _lowLutLevels, ws.lowTable);

        for (unsigned t = taskBegin; t < taskEnd; ++t)
            buildSubtreeBlock(ws, _tasks[t], _subtrees[t]);

        fclose(ws.fp);
        _workersUsed.fetch_add(1, std::memory_order_relaxed);
    }

    // Occupancy query against the shared, read-only top LUT. Mirrors the top
    // LUT branch of VoxelData::cubeContainsVoxelsDestructive.
    bool topCubeOccupied(int x, int y, int z, int size) const
    {
        if (x >= _dataW || y >= _dataH || z >= _dataD)
            return false;
        int bit = findHighestBit(uint32(size));
        int level = _highestVirtualBit - bit;
        return _topTable[level][lutIdx(level, x >> bit, y >> bit, z >> bit)] != 0;
    }

    uint32 blockIndex(int x, int y, int z) const
    {
        int bx = x >> _lowLutLevels;
        int by = y >> _lowLutLevels;
        int bz = z >> _lowLutLevels;
        return _blockToTask[size_t(bx) + size_t(_blockGridDim)*(size_t(by) + size_t(_blockGridDim)*size_t(bz))];
    }

    // Serial merge pass. Replays the serial build recursion above the cache
    // window using the shared top LUT, and splices the prebuilt subtrees in
    // at window-sized nodes. Child order, relative offsets, 0x10000 group
    // spans and 0x20000 extended addresses are recomputed here exactly as
    // the serial build computes them, so the result does not depend on the
    // order in which workers finished their subtrees.
    uint64 mergeNode(ChunkedAllocator<uint32> &allocator, int x, int y, int z, int size, uint64 descriptorIndex)
    {
        if (size == windowSize()) {
            uint64 childOffset = uint64(allocator.size()) - descriptorIndex;
            uint32 taskIdx = blockIndex(x, y, z);
            if (taskIdx == NilTask) {
                // Only reachable for the root of a completely empty volume
                allocator[descriptorIndex] = 0;
                return childOffset;
            }
            const SubtreeDescriptors &sub = _subtrees[taskIdx];
            allocator[descriptorIndex] = sub.words[0];
            for (size_t i = 1; i < sub.words.size(); ++i)
                allocator.pushBack(sub.words[i]);
            // Local word 0 is the subtree root descriptor and lives in the
            // parent's child slot at descriptorIndex; the local content words
            // 1.. land at descriptorIndex + childOffset. A local insertion
            // before local word k therefore maps to descriptorIndex +
            // childOffset + k - 1 in the global pre-insertion coordinate
            // space (local insertions always have k >= 2).
            for (size_t i = 0; i < sub.insertions.size(); ++i)
                allocator.insert(size_t(descriptorIndex + childOffset + sub.insertions[i].first - 1),
                        sub.insertions[i].second);
            return childOffset;
        }

        int halfSize = size >> 1;

        int posX[] = {x + halfSize, x, x + halfSize, x, x + halfSize, x, x + halfSize, x};
        int posY[] = {y + halfSize, y + halfSize, y, y, y + halfSize, y + halfSize, y, y};
        int posZ[] = {z + halfSize, z + halfSize, z + halfSize, z + halfSize, z, z, z, z};

        uint64 childOffset = uint64(allocator.size()) - descriptorIndex;

        int childCount = 0;
        int childIndices[8];
        uint32 childMask = 0;
        for (int i = 0; i < 8; i++) {
            if (topCubeOccupied(posX[i], posY[i], posZ[i], halfSize)) {
                childMask |= 128 >> i;
                childIndices[childCount++] = i;
            }
        }

        bool hasLargeChildren = false;
        uint32 leafMask = childMask;
        for (int i = 0; i < childCount; i++)
            allocator.pushBack(0);

        uint64 grandChildOffsets[8];
        uint64 delta = 0;
        uint64 insertionCount = allocator.insertionCount();
        for (int i = 0; i < childCount; i++) {
            int idx = childIndices[childCount - i - 1];
            grandChildOffsets[i] = delta + mergeNode(allocator, posX[idx], posY[idx], posZ[idx],
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

        allocator[descriptorIndex] = (childMask << 8) | leafMask;
        if (hasLargeChildren)
            allocator[descriptorIndex] |= 0x10000;

        return childOffset;
    }

    // Single full-file occupancy pre-scan. Runs exactly once per build; the
    // resulting top LUT is read-only shared between all workers and the
    // merge pass.
    void prescan()
    {
        _topLut.reset(new uint8[size_t(_topLutBytes)]());
        buildHierarchicalGridLinks(_topLut.get(), _topLutLevels, _topTable);

        int window = windowSize();
        std::unique_ptr<uint32[]> buffer(new uint32[size_t(window)*size_t(window)*size_t(window)]);

        FILE *fp = fopen(_path.c_str(), "rb");
        if (!fp)
            throw std::runtime_error(std::string("Failed to open raw voxel file: ") + _path);

        int leafLevel = _topLutLevels - 1;
        for (int z = 0; z < _virtualD; z += window) {
            for (int y = 0; y < _virtualH; y += window) {
                for (int x = 0; x < _virtualW; x += window) {
                    int w = std::min(window, _dataW - x);
                    int h = std::min(window, _dataH - y);
                    int d = std::min(window, _dataD - z);
                    if (w <= 0 || h <= 0 || d <= 0)
                        continue;

                    readBlock(fp, _dataW, _dataH, buffer.get(), x, y, z, w, h, d);

                    size_t count = size_t(w)*size_t(h)*size_t(d);
                    for (size_t i = 0; i < count; ++i) {
                        if (buffer[i]) {
                            _topTable[leafLevel][lutIdx(leafLevel,
                                    x >> _lowLutLevels, y >> _lowLutLevels, z >> _lowLutLevels)] = 1;
                            break;
                        }
                    }
                }
            }
        }
        fclose(fp);

        for (int l = _topLutLevels - 2; l >= 0; --l)
            upsampleLutLevel(_topTable, l);
    }

    void enumerateTasks()
    {
        int leafLevel = _topLutLevels - 1;
        _blockGridDim = 1 << leafLevel;
        _blockToTask.assign(size_t(_blockGridDim)*size_t(_blockGridDim)*size_t(_blockGridDim), NilTask);

        for (int bz = 0; bz < _blockGridDim; ++bz)
            for (int by = 0; by < _blockGridDim; ++by)
                for (int bx = 0; bx < _blockGridDim; ++bx)
                    if (_topTable[leafLevel][lutIdx(leafLevel, bx, by, bz)]) {
                        _blockToTask[size_t(bx) + size_t(_blockGridDim)*(size_t(by) + size_t(_blockGridDim)*size_t(bz))] =
                                uint32(_tasks.size());
                        _tasks.push_back(BlockTask{bx << _lowLutLevels, by << _lowLutLevels, bz << _lowLutLevels});
                    }

        _subtrees.resize(_tasks.size());
    }

public:
    RawParallelBuilder(const char *path, size_t cacheBytes, unsigned workers)
    : _path(path),
      _blockGridDim(0),
      _workersUsed(0),
      _octreeSize(0),
      _peakInputCacheBytes(0)
    {
        if (workers == 0)
            throw std::invalid_argument("VoxelOctree raw build requires at least one worker");

        FILE *fp = fopen(path, "rb");
        if (!fp)
            throw std::runtime_error(std::string("Failed to open raw voxel file: ") + path);
        bool headerOk = fread(&_dataW, 4, 1, fp) == 1 &&
                        fread(&_dataH, 4, 1, fp) == 1 &&
                        fread(&_dataD, 4, 1, fp) == 1;
        fclose(fp);
        if (!headerOk)
            throw std::runtime_error(std::string("Failed to read raw voxel header: ") + path);
        if (_dataW <= 0 || _dataH <= 0 || _dataD <= 0)
            throw std::runtime_error(std::string("Invalid raw voxel dimensions in: ") + path);

        _virtualW = roundToPow2(_dataW);
        _virtualH = roundToPow2(_dataH);
        _virtualD = roundToPow2(_dataD);
        _sideLength = std::max(_virtualW, std::max(_virtualH, _virtualD));
        _highestVirtualBit = findHighestBit(uint32(_sideLength));

        // Pick the largest cache window such that the shared top LUT plus
        // one low LUT and voxel window per worker fit into cacheBytes at the
        // same time. If the budget cannot hold all requested workers even at
        // the smallest window, fewer workers are used; if it cannot hold the
        // shared index plus a single worker window, the build fails.
        uint64 budget = uint64(cacheBytes);
        unsigned maxWorkers = std::min(workers, MaxWorkerThreads);
        bool found = false;
        for (unsigned w = maxWorkers; w >= 1 && !found; --w) {
            for (int lowBit = _highestVirtualBit; lowBit >= 1; --lowBit) {
                if (lowBit >= 20)
                    continue; // A 2^20-sided window can never fit a byte budget
                uint64 topCost = countCellsInHierarchicalGrid(_highestVirtualBit - lowBit + 1);
                uint64 perWorker = countCellsInHierarchicalGrid(lowBit)
                        + ((uint64(1) << (3*lowBit))*sizeof(uint32));
                if (topCost <= budget && perWorker <= (budget - topCost)/w) {
                    _lowLutLevels = lowBit;
                    _topLutLevels = _highestVirtualBit - lowBit + 1;
                    _partitionWorkers = w;
                    _topLutBytes = topCost;
                    _lowLutBytes = countCellsInHierarchicalGrid(lowBit);
                    _windowVoxelBytes = (uint64(1) << (3*lowBit))*sizeof(uint32);
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            throw std::invalid_argument(
                    "cacheBytes is too small for the shared occupancy index plus one worker window");
    }

    void run()
    {
        prescan();
        enumerateTasks();

        unsigned activeWorkers = 0;
        if (!_tasks.empty()) {
            activeWorkers = std::min(_partitionWorkers, unsigned(_tasks.size()));

            std::vector<std::thread> threads;
            std::vector<std::exception_ptr> errors(activeWorkers);
            unsigned taskCount = unsigned(_tasks.size());
            for (unsigned w = 0; w < activeWorkers; ++w) {
                unsigned begin = taskCount*w/activeWorkers;
                unsigned end = taskCount*(w + 1)/activeWorkers;
                threads.push_back(std::thread([this, begin, end, &errors, w] {
                    try {
                        workerMain(begin, end);
                    } catch (...) {
                        errors[w] = std::current_exception();
                    }
                }));
            }
            for (size_t i = 0; i < threads.size(); ++i)
                threads[i].join();
            for (size_t i = 0; i < errors.size(); ++i)
                if (errors[i])
                    std::rethrow_exception(errors[i]);
        }

        ChunkedAllocator<uint32> allocator;
        allocator.pushBack(0);
        mergeNode(allocator, 0, 0, 0, _sideLength, 0);
        allocator[0] |= 1 << 18;

        _octreeSize = allocator.size() + allocator.insertionCount();
        _octree = allocator.finalize();
        _center = Vec3(
            _dataW*0.5f/_sideLength,
            _dataH*0.5f/_sideLength,
            _dataD*0.5f/_sideLength
        );

        // Peak live input cache arrays: the shared top LUT plus either the
        // per-worker low LUTs and voxel windows, or - for an empty volume
        // with no workers - the single pre-scan window.
        _peakInputCacheBytes = _topLutBytes + (activeWorkers
                ? uint64(activeWorkers)*(_windowVoxelBytes + _lowLutBytes)
                : _windowVoxelBytes);
    }

    uint64 octreeSize() const { return _octreeSize; }
    Vec3 center() const { return _center; }
    uint32 workersUsed() const { return _workersUsed.load(); }
    uint64 peakInputCacheBytes() const { return _peakInputCacheBytes; }

    std::unique_ptr<uint32[]> takeOctree() { return std::move(_octree); }
};

} // anonymous namespace

std::unique_ptr<uint32[]> buildOctreeFromRaw(const char *rawPath, size_t cacheBytes,
        unsigned workers, uint64 &octreeSize, Vec3 &center, BuildStats *stats)
{
    RawParallelBuilder builder(rawPath, cacheBytes, workers);
    builder.run();

    octreeSize = builder.octreeSize();
    center = builder.center();
    if (stats) {
        stats->topLutBuilds = 1; // The occupancy pre-scan runs exactly once per build
        stats->workersUsed = builder.workersUsed();
        stats->peakInputCacheBytes = builder.peakInputCacheBytes();
    }
    return builder.takeOctree();
}
