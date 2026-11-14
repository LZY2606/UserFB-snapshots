// Offline structural compression (VoxelOctree::compact) tests.
//
// Every scene is generated as a small raw voxel file and built through the
// real VoxelData/VoxelOctree pipeline. An independent sparse occupancy/
// material reference is kept alongside, and exact rayScale=0 raymarch results
// (hit, parameter t, material) are checked against per-voxel ray/box
// intersection - never merely against the pre-compaction tree.

#include "task_support/check.h"

#include "VoxelData.hpp"
#include "VoxelOctree.hpp"
#include "Util.hpp"
#include "thread/ThreadUtils.hpp"
#include "thread/ThreadPool.hpp"
#include "third-party/lz4.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// test infrastructure

void shutdownThreads() {
    delete ThreadUtils::pool;
    ThreadUtils::pool = 0;
}

void ensureThreads() {
    static bool registered = false;
    if (!ThreadUtils::pool) {
        ThreadUtils::startThreads(2);
        if (!registered) {
            std::atexit(shutdownThreads);
            registered = true;
        }
    }
}

int popCount8(uint32 mask) {
    int count = 0;
    for (int i = 0; i < 8; i++)
        if (mask & (128 >> i))
            count++;
    return count;
}

// Sparse occupancy/material reference. The octree maps voxel (x,y,z) of a
// side-length-S grid to the box [1+x/S, 1+(x+1)/S]^3 in raymarch space.
struct Scene {
    int side;
    std::map<uint64, uint32> voxels; // key = (z<<40)|(y<<20)|x

    void set(int x, int y, int z, uint32 material) {
        voxels[(uint64(z) << 40) | (uint64(y) << 20) | uint64(x)] = material;
    }

    void writeRaw(const char *path) const {
        FILE *fp = std::fopen(path, "wb");
        int dim[3] = {side, side, side};
        std::fwrite(dim, sizeof(int), 3, fp);
        std::vector<uint32> buf(size_t(side)*side*side, 0);
        for (std::map<uint64, uint32>::const_iterator it = voxels.begin(); it != voxels.end(); ++it) {
            int x = int(it->first & 0xFFFFF);
            int y = int((it->first >> 20) & 0xFFFFF);
            int z = int((it->first >> 40) & 0xFFFFF);
            buf[size_t(x) + size_t(side)*(size_t(y) + size_t(side)*size_t(z))] = it->second;
        }
        std::fwrite(buf.data(), sizeof(uint32), buf.size(), fp);
        std::fclose(fp);
    }
};

struct RefHit {
    bool hit;
    double t;
    uint32 material;
};

// Independent reference: ray/box intersection against every occupied voxel.
RefHit referenceRay(const Scene &scene, const Vec3 &o, const Vec3 &d) {
    RefHit best;
    best.hit = false;
    best.t = 1e30;
    best.material = 0;
    double invS = 1.0/double(scene.side);
    for (std::map<uint64, uint32>::const_iterator it = scene.voxels.begin(); it != scene.voxels.end(); ++it) {
        int coord[3] = {
            int(it->first & 0xFFFFF),
            int((it->first >> 20) & 0xFFFFF),
            int((it->first >> 40) & 0xFFFFF)
        };
        double tEnter = -1e30, tExit = 1e30;
        for (int a = 0; a < 3; a++) {
            double mn = 1.0 + coord[a]*invS;
            double mx = mn + invS;
            double ta = (mn - o.a[a])/d.a[a];
            double tb = (mx - o.a[a])/d.a[a];
            if (ta > tb)
                std::swap(ta, tb);
            if (ta > tEnter) tEnter = ta;
            if (tb < tExit)  tExit  = tb;
        }
        if (tEnter <= tExit && tExit > 0.0 && tEnter < best.t) {
            best.hit = true;
            best.t = tEnter;
            best.material = it->second;
        }
    }
    return best;
}

const double kTol = 1e-4;

bool rayMatches(VoxelOctree &tree, const Scene &scene, const Vec3 &o, const Vec3 &d) {
    RefHit ref = referenceRay(scene, o, d);
    uint32 material = 0;
    float t = -1.0f;
    bool hit = tree.raymarch(o, d, 0.0f, material, t);
    if (hit != ref.hit)
        return false;
    if (!hit)
        return true;
    if (material != ref.material)
        return false;
    return std::fabs(double(t) - ref.t) <= kTol;
}

// Rays enter from all eight diagonal directions; origins sit well outside the
// [1,2]^3 root box, and small jitters keep rays away from coplanar/edge cases.
bool rayMatchesEightDirections(VoxelOctree &tree, const Scene &scene, double cx, double cy, double cz) {
    static const double jit[8][2] = {
        {0.00, 0.00}, {0.013, 0.007}, {-0.011, 0.009}, {0.007, -0.013},
        {0.017, 0.011}, {-0.007, -0.009}, {0.003, 0.019}, {-0.017, 0.003}
    };
    for (int i = 0; i < 8; i++) {
        double sx = (i & 1) ? 1.0 : -1.0;
        double sy = (i & 2) ? 1.0 : -1.0;
        double sz = (i & 4) ? 1.0 : -1.0;
        Vec3 d(float(sx*1.0), float(sy*(0.131 + jit[i][0])), float(sz*(0.073 + jit[i][1])));
        Vec3 o(float(cx - d.x*3.5), float(cy - d.y*3.5), float(cz - d.z*3.5));
        if (!rayMatches(tree, scene, o, d))
            return false;
    }
    return true;
}

std::unique_ptr<VoxelOctree> buildTree(const Scene &scene, const char *rawPath, size_t mem) {
    scene.writeRaw(rawPath);
    VoxelData voxels(rawPath, mem);
    return std::unique_ptr<VoxelOctree>(new VoxelOctree(&voxels));
}

// Save and decompress back to raw words (the file layout is the original one:
// center, word count, then LZ4 blocks), giving tests full access to the
// descriptor bytes without touching VoxelOctree internals.
std::vector<uint32> saveAndReadWords(VoxelOctree &tree, const char *path) {
    tree.save(path);
    FILE *fp = std::fopen(path, "rb");
    if (!fp)
        return std::vector<uint32>();
    float center[3];
    uint64 size = 0;
    std::fread(center, sizeof(float), 3, fp);
    std::fread(&size, sizeof(uint64), 1, fp);
    std::vector<uint32> words((size_t)size);
    std::vector<char> buffer(LZ4_compressBound(64*1024*1024));
    char *dst = reinterpret_cast<char *>(words.data());
    LZ4_streamDecode_t *stream = LZ4_createStreamDecode();
    LZ4_setStreamDecode(stream, dst, 0);
    for (uint64 offset = 0; offset < size*sizeof(uint32); offset += 64*1024*1024) {
        uint64 compSize = 0;
        std::fread(&compSize, sizeof(uint64), 1, fp);
        std::fread(buffer.data(), sizeof(char), size_t(compSize), fp);
        int outSize = int(std::min<uint64>(size*sizeof(uint32) - offset, 64*1024*1024));
        LZ4_decompress_fast_continue(stream, buffer.data(), dst + offset, outSize);
    }
    LZ4_freeStreamDecode(stream);
    std::fclose(fp);
    return words;
}

// Structural validator: walks the descriptor graph from word 0 and checks the
// childMask/leafMask semantics, the relative addresses, the 0x10000 group
// span (paired records) and the 0x20000 extended addresses against the actual
// layout, and that every word is covered exactly once. Shared subtrees are
// reached through several descriptor copies; each stored child block is
// validated on first encounter and skipped afterwards.
struct LayoutInfo {
    bool ok;
    uint64 blocks;   // reachable non-empty child blocks after the root
    uint64 refs;     // references from reachable descriptors to those blocks
    uint64 longDescriptors; // descriptors with 0x10000 (paired child records)
    uint64 farDescriptors;  // descriptors with 0x20000 (extended address)
};

LayoutInfo inspectLayout(const std::vector<uint32> &w) {
    LayoutInfo info;
    info.ok = true;
    info.blocks = info.refs = info.longDescriptors = info.farDescriptors = 0;
    if (w.empty()) {
        info.ok = false;
        return info;
    }
    std::vector<uint8> mark(w.size(), 0);
    std::vector<uint8> blockSeen(w.size(), 0);
    struct Item {
        uint64 addr;
        bool parentLong;
    };
    std::vector<Item> stack;
    Item root = {0, false};
    stack.push_back(root);
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        if (it.addr >= w.size()) {
            info.ok = false;
            continue;
        }
        if (mark[it.addr])
            continue; // shared subtree copy already validated
        mark[it.addr] = 1;
        uint32 desc = w[size_t(it.addr)];
        uint32 childMask = (desc >> 8) & 0xFF;
        uint32 leafMask = desc & 0xFF;
        int recordCount = popCount8(childMask);
        if (recordCount == 0 && it.addr != 0)
            info.ok = false;
        bool longGroup = (desc & 0x10000) != 0;
        bool far = (desc & 0x20000) != 0;
        uint64 offset = desc >> 18;
        if (longGroup)
            info.longDescriptors++;
        if (far) {
            info.farDescriptors++;
            if (!it.parentLong)
                info.ok = false; // extended address without paired group
            if (it.addr + 1 >= w.size() || mark[it.addr + 1])
                info.ok = false;
            else
                mark[it.addr + 1] = 1;
            offset = (offset << 32) | uint64(w[size_t(it.addr + 1)]);
        }
        if (it.parentLong && !far)
            info.ok = false; // paired group must use extended addresses
        if (recordCount == 0)
            continue;
        info.refs++; // this descriptor references its child block
        if (offset == 0)
            info.ok = false;
        if (!far && offset > 0x3FFF)
            info.ok = false; // short address out of range
        uint64 blockAddr = it.addr + offset;
        if (blockAddr < w.size() && !blockSeen[blockAddr]) {
            blockSeen[blockAddr] = 1;
            info.blocks++;
        } else if (blockAddr >= w.size()) {
            info.ok = false;
        }
        uint64 stride = longGroup ? 2 : 1;
        int record = 0;
        for (int octant = 7; octant >= 0; octant--) {
            if (!(childMask & (128 >> octant)))
                continue;
            uint64 recAddr = blockAddr + uint64(record)*stride;
            if (recAddr >= w.size()) {
                info.ok = false;
            } else if (leafMask & (128 >> octant)) {
                // internal child: descriptor record, visited as a node later
                Item child = {recAddr, longGroup};
                stack.push_back(child);
            } else {
                // leaf record: a complete 32-bit material word
                if (!mark[recAddr]) {
                    mark[recAddr] = 1;
                    if (w[size_t(recAddr)] == 0)
                        info.ok = false;
                }
            }
            record++;
        }
    }
    for (size_t i = 0; i < w.size(); i++)
        if (!mark[i])
            info.ok = false; // gaps or unreachable words in the layout
    return info;
}

bool statsMatchLayout(const VoxelOctree::CompactStats &st, const std::vector<uint32> &w) {
    LayoutInfo info = inspectLayout(w);
    return info.ok &&
           info.blocks == st.uniqueBlocks &&
           info.refs == st.uniqueBlocks + st.sharedReferences;
}

// ---------------------------------------------------------------------------
// scenes

const uint32 kMatA = 0x81234567u;
const uint32 kMatB = 0x07654321u;
const uint32 kMatC = 0xF0E1D2C3u;

void fillBox(Scene &s, int x0, int y0, int z0, int size, uint32 material) {
    for (int z = 0; z < size; z++)
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
                s.set(x0 + x, y0 + y, z0 + z, material);
}

// ---------------------------------------------------------------------------
// sharing group

int case_repeated_modules() {
    ensureThreads();
    // Two identical, fully populated 4^3 modules in opposite root octants.
    Scene scene;
    scene.side = 8;
    fillBox(scene, 0, 0, 0, 4, kMatA);
    fillBox(scene, 4, 4, 4, 4, kMatA);
    std::unique_ptr<VoxelOctree> tree = buildTree(scene, ".task-build/t_share1.raw", 1 << 20);

    // The reference anchors absolute correctness before compaction.
    TASK_CHECK(rayMatchesEightDirections(*tree, scene, 1.0 + 5.5/8.0, 1.0 + 5.5/8.0, 1.0 + 5.5/8.0));
    TASK_CHECK(rayMatchesEightDirections(*tree, scene, 1.0 + 1.5/8.0, 1.0 + 2.5/8.0, 1.0 + 0.5/8.0));

    uint64 before = tree->wordCount();
    VoxelOctree::CompactStats st = tree->compact();
    TASK_CHECK(st.wordsBefore == before);
    TASK_CHECK(st.wordsAfter == tree->wordCount());
    // Hand-computed DAG: root block (2 records), one shared 4^3 block
    // (8 records), one shared 2^3 leaf block (8 material words).
    TASK_CHECK(before == 147);
    TASK_CHECK(st.wordsAfter == 19);
    TASK_CHECK(st.uniqueBlocks == 3);
    TASK_CHECK(st.sharedReferences == 8);

    std::vector<uint32> words = saveAndReadWords(*tree, ".task-build/t_share1.oct");
    TASK_CHECK(words.size() == tree->wordCount());
    TASK_CHECK(statsMatchLayout(st, words));

    // Identical rays after compaction, checked against the sparse reference.
    TASK_CHECK(rayMatchesEightDirections(*tree, scene, 1.0 + 5.5/8.0, 1.0 + 5.5/8.0, 1.0 + 5.5/8.0));
    TASK_CHECK(rayMatchesEightDirections(*tree, scene, 1.0 + 1.5/8.0, 1.0 + 2.5/8.0, 1.0 + 0.5/8.0));
    // Rays that miss all content.
    TASK_CHECK(rayMatches(*tree, scene, Vec3(0.3f, 0.31f, 0.32f), Vec3(-1.0f, -0.11f, -0.07f)));
    TASK_CHECK(rayMatches(*tree, scene, Vec3(1.2f, 0.2f, 1.3f), Vec3(0.2f, -1.0f, 0.1f)));

    // A second compaction must reproduce identical descriptor bytes.
    VoxelOctree::CompactStats st2 = tree->compact();
    TASK_CHECK(st2.wordsBefore == st.wordsAfter);
    TASK_CHECK(st2.wordsAfter == st.wordsAfter);
    TASK_CHECK(st2.uniqueBlocks == st.uniqueBlocks);
    TASK_CHECK(st2.sharedReferences == st.sharedReferences);
    std::vector<uint32> words2 = saveAndReadWords(*tree, ".task-build/t_share1b.oct");
    TASK_CHECK(words2 == words);
    return 0;
}

int case_fence_modules() {
    ensureThreads();
    // Four identical "fence" modules far apart in a 32^3 grid; each module is
    // a 4^3 checker pattern with position-dependent materials.
    Scene scene;
    scene.side = 32;
    const int pos[4][3] = {{0,0,0}, {28,28,28}, {0,28,0}, {28,0,28}};
    for (int m = 0; m < 4; m++)
        for (int z = 0; z < 4; z++)
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    if ((x + y + z) % 2 == 0)
                        scene.set(pos[m][0] + x, pos[m][1] + y, pos[m][2] + z,
                                  0x80000000u | uint32(1 + x + 4*y + 16*z));
    std::unique_ptr<VoxelOctree> tree = buildTree(scene, ".task-build/t_share2.raw", 1 << 20);

    uint64 before = tree->wordCount();
    VoxelOctree::CompactStats st = tree->compact();
    TASK_CHECK(st.wordsBefore == before);
    TASK_CHECK(st.wordsAfter == tree->wordCount());
    TASK_CHECK(st.wordsAfter < st.wordsBefore);   // real wordCount reduction
    TASK_CHECK(st.uniqueBlocks >= 4);             // root + shared module levels
    TASK_CHECK(st.sharedReferences >= 3);         // 4 uses of the same module

    std::vector<uint32> words = saveAndReadWords(*tree, ".task-build/t_share2.oct");
    TASK_CHECK(statsMatchLayout(st, words));

    // Rays from eight directions per module, crossing the empty middle.
    for (int m = 0; m < 4; m++)
        TASK_CHECK(rayMatchesEightDirections(*tree, scene,
            1.0 + (pos[m][0] + 1.5)/32.0, 1.0 + (pos[m][1] + 2.5)/32.0, 1.0 + (pos[m][2] + 0.5)/32.0));
    TASK_CHECK(rayMatches(*tree, scene, Vec3(1.5f, 0.25f, 1.5f), Vec3(0.1f, -1.0f, 0.07f)));

    VoxelOctree::CompactStats st2 = tree->compact();
    TASK_CHECK(st2.wordsAfter == st.wordsAfter);
    std::vector<uint32> words2 = saveAndReadWords(*tree, ".task-build/t_share2b.oct");
    TASK_CHECK(words2 == words);
    return 0;
}

int case_distant_clusters() {
    ensureThreads();
    // Sparse clusters at far corners of a 64^3 grid; rays cross many empty
    // children before hitting. Two cluster shapes, each used twice.
    Scene scene;
    scene.side = 64;
    const int pos[4][3] = {{0,0,0}, {56,56,56}, {0,56,56}, {56,0,0}};
    for (int m = 0; m < 4; m++) {
        int shape = m % 2;
        for (int i = 0; i < 6; i++) {
            int x = pos[m][0] + (shape ? i : (i % 3));
            int y = pos[m][1] + (shape ? 0 : (i / 3)*2);
            int z = pos[m][2] + (shape ? i : i);
            scene.set(x, y, z, 0x90000000u | uint32(i + 1));
        }
    }
    std::unique_ptr<VoxelOctree> tree = buildTree(scene, ".task-build/t_share3.raw", 1 << 20);

    uint64 before = tree->wordCount();
    VoxelOctree::CompactStats st = tree->compact();
    TASK_CHECK(st.wordsAfter < before);
    TASK_CHECK(st.sharedReferences >= 2); // each shape stored once, used twice

    std::vector<uint32> words = saveAndReadWords(*tree, ".task-build/t_share3.oct");
    TASK_CHECK(statsMatchLayout(st, words));

    for (int m = 0; m < 4; m++)
        TASK_CHECK(rayMatchesEightDirections(*tree, scene,
            1.0 + (pos[m][0] + 2.5)/64.0, 1.0 + (pos[m][1] + 1.5)/64.0, 1.0 + (pos[m][2] + 2.5)/64.0));
    TASK_CHECK(rayMatches(*tree, scene, Vec3(1.5f, 1.5f, 0.2f), Vec3(0.07f, 0.05f, -1.0f)));

    VoxelOctree::CompactStats st2 = tree->compact();
    TASK_CHECK(st2.wordsAfter == st.wordsAfter);
    std::vector<uint32> words2 = saveAndReadWords(*tree, ".task-build/t_share3b.oct");
    TASK_CHECK(words2 == words);
    return 0;
}

// ---------------------------------------------------------------------------
// materials group

int case_material_blocks_merging() {
    ensureThreads();
    // Same shape, different materials must not merge: two full 4^3 modules
    // with different materials need 5 blocks (root, 2x4^3, 2x2^3)...
    Scene sceneAB;
    sceneAB.side = 8;
    fillBox(sceneAB, 0, 0, 0, 4, kMatA);
    fillBox(sceneAB, 4, 4, 4, 4, kMatB);
    std::unique_ptr<VoxelOctree> treeAB = buildTree(sceneAB, ".task-build/t_mat1.raw", 1 << 20);
    VoxelOctree::CompactStats stAB = treeAB->compact();
    TASK_CHECK(stAB.wordsAfter == 35);
    TASK_CHECK(stAB.uniqueBlocks == 5);
    TASK_CHECK(stAB.sharedReferences == 14);
    std::vector<uint32> wordsAB = saveAndReadWords(*treeAB, ".task-build/t_mat1.oct");
    TASK_CHECK(statsMatchLayout(stAB, wordsAB));
    TASK_CHECK(rayMatchesEightDirections(*treeAB, sceneAB, 1.0 + 5.5/8.0, 1.0 + 5.5/8.0, 1.0 + 5.5/8.0));

    // ...while identical materials collapse to 3 blocks (root, 4^3, 2^3).
    Scene sceneAA;
    sceneAA.side = 8;
    fillBox(sceneAA, 0, 0, 0, 4, kMatA);
    fillBox(sceneAA, 4, 4, 4, 4, kMatA);
    std::unique_ptr<VoxelOctree> treeAA = buildTree(sceneAA, ".task-build/t_mat2.raw", 1 << 20);
    VoxelOctree::CompactStats stAA = treeAA->compact();
    TASK_CHECK(stAA.uniqueBlocks == 3);
    TASK_CHECK(stAA.wordsAfter == 19);

    // A single differing leaf inside one module must also block the merge.
    Scene sceneLeaf;
    sceneLeaf.side = 8;
    fillBox(sceneLeaf, 0, 0, 0, 4, kMatA);
    fillBox(sceneLeaf, 4, 4, 4, 4, kMatA);
    sceneLeaf.set(7, 7, 7, kMatC);
    std::unique_ptr<VoxelOctree> treeLeaf = buildTree(sceneLeaf, ".task-build/t_mat3.raw", 1 << 20);
    VoxelOctree::CompactStats stLeaf = treeLeaf->compact();
    TASK_CHECK(stLeaf.uniqueBlocks > 3);
    TASK_CHECK(stLeaf.wordsAfter > 19);
    std::vector<uint32> wordsLeaf = saveAndReadWords(*treeLeaf, ".task-build/t_mat3.oct");
    TASK_CHECK(statsMatchLayout(stLeaf, wordsLeaf));
    TASK_CHECK(rayMatchesEightDirections(*treeLeaf, sceneLeaf, 1.0 + 7.5/8.0, 1.0 + 7.5/8.0, 1.0 + 7.5/8.0));
    return 0;
}

int case_octant_positions() {
    ensureThreads();
    // Two single voxels whose subtree paths differ only in one octant
    // position: (4,4,4) sits at octant 7 on every level below the root,
    // (2,0,0) sits at octant 6 inside its 4^3 node. Octant positions are part
    // of the identity, so the 4^3 subtrees must not merge.
    Scene scene;
    scene.side = 8;
    scene.set(4, 4, 4, kMatA);
    scene.set(2, 0, 0, kMatA);
    std::unique_ptr<VoxelOctree> tree = buildTree(scene, ".task-build/t_pos1.raw", 1 << 20);
    VoxelOctree::CompactStats st = tree->compact();
    TASK_CHECK(st.uniqueBlocks == 4); // root, 2x4^3, shared 2^3 leaf block
    TASK_CHECK(st.sharedReferences == 1);
    TASK_CHECK(st.wordsAfter == 6);
    std::vector<uint32> words = saveAndReadWords(*tree, ".task-build/t_pos1.oct");
    TASK_CHECK(statsMatchLayout(st, words));
    TASK_CHECK(rayMatchesEightDirections(*tree, scene, 1.0 + 4.5/8.0, 1.0 + 4.5/8.0, 1.0 + 4.5/8.0));
    TASK_CHECK(rayMatchesEightDirections(*tree, scene, 1.0 + 2.5/8.0, 1.0 + 0.5/8.0, 1.0 + 0.5/8.0));

    // Contrast: (0,0,0) follows octant 7 on every level just like (4,4,4),
    // so there the 4^3 subtrees do merge.
    Scene aligned;
    aligned.side = 8;
    aligned.set(4, 4, 4, kMatA);
    aligned.set(0, 0, 0, kMatA);
    std::unique_ptr<VoxelOctree> tree2 = buildTree(aligned, ".task-build/t_pos2.raw", 1 << 20);
    VoxelOctree::CompactStats st2 = tree2->compact();
    TASK_CHECK(st2.uniqueBlocks == 3);
    TASK_CHECK(st2.wordsAfter == 5);
    return 0;
}

int case_levels_not_merged() {
    ensureThreads();
    // Similar content at different levels: a full 4^3 block of material A
    // (eight 2^3 children) and, elsewhere, a single full 2^3 block of the
    // same material. The 4^3 node must not merge with the 2^3 node, while
    // the identical 2^3 leaf blocks are shared across both parents.
    Scene scene;
    scene.side = 16;
    fillBox(scene, 0, 0, 0, 4, kMatA); // full 4^3
    fillBox(scene, 8, 8, 8, 2, kMatA); // full 2^3
    std::unique_ptr<VoxelOctree> tree = buildTree(scene, ".task-build/t_lvl1.raw", 1 << 20);
    VoxelOctree::CompactStats st = tree->compact();
    // root, 2x8^3 chain nodes, 4^3 node, 4^3 chain node, shared 2^3 block
    TASK_CHECK(st.uniqueBlocks == 6);
    TASK_CHECK(st.sharedReferences == 8);
    TASK_CHECK(st.wordsAfter == 22);
    std::vector<uint32> words = saveAndReadWords(*tree, ".task-build/t_lvl1.oct");
    TASK_CHECK(statsMatchLayout(st, words));
    TASK_CHECK(rayMatchesEightDirections(*tree, scene, 1.0 + 1.5/16.0, 1.0 + 2.5/16.0, 1.0 + 0.5/16.0));
    TASK_CHECK(rayMatchesEightDirections(*tree, scene, 1.0 + 8.5/16.0, 1.0 + 8.5/16.0, 1.0 + 9.5/16.0));
    return 0;
}

// ---------------------------------------------------------------------------
// addresses group

int case_long_addresses() {
    ensureThreads();
    // Large but sparse: 4096 cells of 8^3 voxels on a 16^3 grid inside a
    // 256^3 volume (0.2% occupancy). 2048 distinct cell types, each used
    // twice, so compaction both shares subtrees and keeps long relative
    // addresses between levels.
    Scene scene;
    scene.side = 256;
    std::vector<double> targets;
    int cellIndex = 0;
    for (int cz = 0; cz < 16; cz++) {
        for (int cy = 0; cy < 16; cy++) {
            for (int cx = 0; cx < 16; cx++, cellIndex++) {
                int type = cellIndex/2; // adjacent cell pairs share a type
                uint32 h = uint32(type)*2654435761u;
                int bx = cx*16, by = cy*16, bz = cz*16;
                for (int o = 0; o < 8; o++) {
                    uint32 h2 = h + uint32(o)*0x9E3779B9u;
                    int x = bx + ((o & 1) ? 4 : 0) + int(h2 & 3);
                    int y = by + ((o & 2) ? 4 : 0) + int((h2 >> 4) & 3);
                    int z = bz + ((o & 4) ? 4 : 0) + int((h2 >> 8) & 3);
                    scene.set(x, y, z, 0x80000000u | uint32(type*8 + o + 1));
                    if (cellIndex % 512 == 1 && o == 3) {
                        targets.push_back(1.0 + (x + 0.5)/256.0);
                        targets.push_back(1.0 + (y + 0.5)/256.0);
                        targets.push_back(1.0 + (z + 0.5)/256.0);
                    }
                }
            }
        }
    }
    std::unique_ptr<VoxelOctree> tree = buildTree(scene, ".task-build/t_addr.raw", 1 << 24);

    // The original build must contain the long relative addresses.
    std::vector<uint32> before = saveAndReadWords(*tree, ".task-build/t_addr_pre.oct");
    LayoutInfo preInfo = inspectLayout(before);
    TASK_CHECK(preInfo.ok);
    TASK_CHECK(preInfo.longDescriptors >= 1);
    TASK_CHECK(preInfo.farDescriptors >= 1);
    TASK_CHECK(before.size() == tree->wordCount());

    VoxelOctree::CompactStats st = tree->compact();
    TASK_CHECK(st.wordsAfter == tree->wordCount());
    TASK_CHECK(st.wordsAfter < st.wordsBefore);
    TASK_CHECK(st.sharedReferences >= 2048); // every cell type used twice

    // Relayout keeps both short and long pointers, all consistent.
    std::vector<uint32> after = saveAndReadWords(*tree, ".task-build/t_addr_post.oct");
    LayoutInfo postInfo = inspectLayout(after);
    TASK_CHECK(postInfo.ok);
    TASK_CHECK(postInfo.longDescriptors >= 1);
    TASK_CHECK(postInfo.farDescriptors >= 1);
    TASK_CHECK(postInfo.blocks == st.uniqueBlocks);
    TASK_CHECK(postInfo.refs == st.uniqueBlocks + st.sharedReferences);
    TASK_CHECK(after.size() == tree->wordCount());

    // Rays from eight directions per sampled target, plus empty-space rays.
    for (size_t i = 0; i + 2 < targets.size(); i += 3)
        TASK_CHECK(rayMatchesEightDirections(*tree, scene, targets[i], targets[i + 1], targets[i + 2]));
    TASK_CHECK(rayMatches(*tree, scene, Vec3(0.2f, 1.03f, 1.04f), Vec3(1.0f, 0.11f, 0.06f)));
    TASK_CHECK(rayMatches(*tree, scene, Vec3(1.02f, 0.3f, 1.05f), Vec3(0.09f, 1.0f, 0.13f)));

    // Recompaction is a fixpoint: identical bytes and statistics.
    VoxelOctree::CompactStats st2 = tree->compact();
    TASK_CHECK(st2.wordsBefore == st.wordsAfter);
    TASK_CHECK(st2.wordsAfter == st.wordsAfter);
    TASK_CHECK(st2.uniqueBlocks == st.uniqueBlocks);
    TASK_CHECK(st2.sharedReferences == st.sharedReferences);
    std::vector<uint32> after2 = saveAndReadWords(*tree, ".task-build/t_addr_post2.oct");
    TASK_CHECK(after2 == after);
    return 0;
}

// ---------------------------------------------------------------------------
// roundtrip group

int case_save_load_recompact() {
    ensureThreads();
    // Repeated modules plus a unique staircase, saved and reloaded through
    // the original file layout and loading constructor.
    Scene scene;
    scene.side = 32;
    const int pos[3][3] = {{0,0,0}, {24,24,0}, {0,24,24}};
    for (int m = 0; m < 3; m++)
        for (int i = 0; i < 4; i++)
            scene.set(pos[m][0] + i, pos[m][1] + i, pos[m][2] + i, 0xA0000000u | uint32(i + 1));
    for (int i = 0; i < 8; i++)
        scene.set(20 + i, 4 + i, 9, 0xB0000000u | uint32(i + 1));
    std::unique_ptr<VoxelOctree> tree = buildTree(scene, ".task-build/t_rt1.raw", 1 << 20);
    Vec3 center = tree->center();

    VoxelOctree::CompactStats st = tree->compact();
    TASK_CHECK(st.wordsAfter < st.wordsBefore);
    std::vector<uint32> words = saveAndReadWords(*tree, ".task-build/t_rt1.oct");
    TASK_CHECK(statsMatchLayout(st, words));

    // The original loading constructor reads the shared layout directly.
    VoxelOctree loaded(".task-build/t_rt1.oct");
    TASK_CHECK(loaded.wordCount() == st.wordsAfter);
    TASK_CHECK(loaded.center().x == center.x && loaded.center().y == center.y && loaded.center().z == center.z);
    for (int m = 0; m < 3; m++)
        TASK_CHECK(rayMatchesEightDirections(loaded, scene,
            1.0 + (pos[m][0] + 1.5)/32.0, 1.0 + (pos[m][1] + 2.5)/32.0, 1.0 + (pos[m][2] + 3.5)/32.0));
    TASK_CHECK(rayMatchesEightDirections(loaded, scene, 1.0 + 23.5/32.0, 1.0 + 7.5/32.0, 1.0 + 9.5/32.0));

    // Compacting the loaded tree reproduces the same bytes and statistics.
    VoxelOctree::CompactStats stL = loaded.compact();
    TASK_CHECK(stL.wordsAfter == st.wordsAfter);
    TASK_CHECK(stL.uniqueBlocks == st.uniqueBlocks);
    TASK_CHECK(stL.sharedReferences == st.sharedReferences);
    std::vector<uint32> wordsL = saveAndReadWords(loaded, ".task-build/t_rt1b.oct");
    TASK_CHECK(wordsL == words);

    // And the reloaded tree still raymarches against the sparse reference.
    VoxelOctree loaded2(".task-build/t_rt1b.oct");
    TASK_CHECK(rayMatchesEightDirections(loaded2, scene, 1.0 + 1.5/32.0, 1.0 + 2.5/32.0, 1.0 + 3.5/32.0));
    return 0;
}

int case_empty_root() {
    ensureThreads();
    Scene scene;
    scene.side = 8; // no voxels at all
    std::unique_ptr<VoxelOctree> tree = buildTree(scene, ".task-build/t_empty.raw", 1 << 20);
    TASK_CHECK(tree->wordCount() == 1);

    VoxelOctree::CompactStats st = tree->compact();
    TASK_CHECK(st.wordsBefore == 1);
    TASK_CHECK(st.wordsAfter == 1);
    TASK_CHECK(st.uniqueBlocks == 0);
    TASK_CHECK(st.sharedReferences == 0);
    TASK_CHECK(tree->wordCount() == 1);

    uint32 material = 0;
    float t = -1.0f;
    TASK_CHECK(!tree->raymarch(Vec3(0.3f, 1.4f, 1.6f), Vec3(1.0f, 0.11f, 0.07f), 0.0f, material, t));

    std::vector<uint32> words = saveAndReadWords(*tree, ".task-build/t_empty.oct");
    TASK_CHECK(words.size() == 1);
    TASK_CHECK(words[0] == (1u << 18));
    LayoutInfo info = inspectLayout(words);
    TASK_CHECK(info.ok && info.blocks == 0 && info.refs == 0);

    VoxelOctree loaded(".task-build/t_empty.oct");
    TASK_CHECK(loaded.wordCount() == 1);
    TASK_CHECK(!loaded.raymarch(Vec3(0.3f, 1.4f, 1.6f), Vec3(1.0f, 0.11f, 0.07f), 0.0f, material, t));
    VoxelOctree::CompactStats stL = loaded.compact();
    TASK_CHECK(stL.wordsAfter == 1 && stL.uniqueBlocks == 0 && stL.sharedReferences == 0);

    // Idempotent on the empty root as well.
    VoxelOctree::CompactStats st2 = tree->compact();
    TASK_CHECK(st2.wordsAfter == 1 && st2.uniqueBlocks == 0 && st2.sharedReferences == 0);
    std::vector<uint32> words2 = saveAndReadWords(*tree, ".task-build/t_empty2.oct");
    TASK_CHECK(words2 == words);
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"sharing",   "repeated_modules",    case_repeated_modules},
    {"sharing",   "fence_modules",       case_fence_modules},
    {"sharing",   "distant_clusters",    case_distant_clusters},
    {"materials", "material_blocks",     case_material_blocks_merging},
    {"materials", "octant_positions",    case_octant_positions},
    {"materials", "levels_not_merged",   case_levels_not_merged},
    {"addresses", "long_addresses",      case_long_addresses},
    {"roundtrip", "save_load_recompact", case_save_load_recompact},
    {"roundtrip", "empty_root",          case_empty_root}
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
