/*
Regression tests for the parallel raw -> SVO build path
(VoxelOctree(rawPath, cacheBytes, workers, stats)). Registered with the
task driver via task_cases/task_case_count; no main() here.
*/

#include "task_support/check.h"

#include "VoxelData.hpp"
#include "VoxelOctree.hpp"
#include "Util.hpp"
#include "math/Vec3.hpp"
#include "thread/ThreadUtils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void ensurePool()
{
    if (!ThreadUtils::pool)
        ThreadUtils::startThreads(2);
}

struct Scene {
    int w, h, d;
    std::vector<uint32> voxels; // x + w*(y + h*z), 0 = empty

    void init(int W, int H, int D)
    {
        w = W; h = H; d = D;
        voxels.assign(size_t(W)*size_t(H)*size_t(D), 0);
    }

    uint32 &at(int x, int y, int z)
    {
        return voxels[size_t(x) + size_t(w)*(size_t(y) + size_t(h)*size_t(z))];
    }

    uint32 at(int x, int y, int z) const
    {
        return voxels[size_t(x) + size_t(w)*(size_t(y) + size_t(h)*size_t(z))];
    }

    int side() const
    {
        int m = std::max(w, std::max(h, d));
        int s = 1;
        while (s < m)
            s *= 2;
        return s;
    }
};

bool writeRaw(const std::string &path, const Scene &sc)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp)
        return false;
    bool ok = fwrite(&sc.w, 4, 1, fp) == 1 &&
              fwrite(&sc.h, 4, 1, fp) == 1 &&
              fwrite(&sc.d, 4, 1, fp) == 1 &&
              fwrite(sc.voxels.data(), 4, sc.voxels.size(), fp) == sc.voxels.size();
    fclose(fp);
    return ok;
}

std::vector<char> readFile(const std::string &path)
{
    std::vector<char> data;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
        return data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        data.insert(data.end(), buf, buf + n);
    fclose(fp);
    return data;
}

bool treesEqual(const VoxelOctree &a, const VoxelOctree &b)
{
    if (a.descriptorCount() != b.descriptorCount())
        return false;
    for (uint64 i = 0; i < a.descriptorCount(); ++i)
        if (a.descriptor(i) != b.descriptor(i))
            return false;
    return true;
}

// Ray space of the octree: the padded volume spans [1, 2) on each axis, so
// voxel coordinate v maps to 1 + v/sideLength.
Vec3 voxelCenter(const Scene &sc, int x, int y, int z)
{
    float s = float(sc.side());
    return Vec3(1.0f + (x + 0.5f)/s, 1.0f + (y + 0.5f)/s, 1.0f + (z + 0.5f)/s);
}

// Independent reference: DDA march through the dense voxel grid, returning
// the first occupied voxel and the parametric entry distance. Uses the same
// ray parametrization as VoxelOctree::raymarch, so t values compare directly.
bool referenceHit(const Scene &sc, Vec3 o, Vec3 d, uint32 &mat, float &tOut)
{
    int S = sc.side();
    double q0[3] = {(o.x - 1.0)*S, (o.y - 1.0)*S, (o.z - 1.0)*S};
    double dq[3] = {d.x*S, d.y*S, d.z*S};
    int dim[3] = {sc.w, sc.h, sc.d};

    double tMin = -1e300, tMax = 1e300;
    for (int a = 0; a < 3; ++a) {
        if (std::fabs(dq[a]) < 1e-12) {
            if (q0[a] < 0.0 || q0[a] >= dim[a])
                return false;
        } else {
            double t0 = (0.0 - q0[a])/dq[a];
            double t1 = (dim[a] - q0[a])/dq[a];
            if (t0 > t1)
                std::swap(t0, t1);
            tMin = std::max(tMin, t0);
            tMax = std::min(tMax, t1);
        }
    }
    if (tMin > tMax || tMax < 0.0)
        return false;

    double t = std::max(tMin, 0.0);
    int cell[3], step[3];
    double next[3], delta[3];
    for (int a = 0; a < 3; ++a) {
        double p = q0[a] + t*dq[a];
        if (p < 0.0)
            p = 0.0;
        if (p >= dim[a])
            p = std::nextafter(double(dim[a]), 0.0);
        cell[a] = int(std::floor(p));
        if (dq[a] > 0.0) {
            step[a] = 1;
            next[a] = t + (cell[a] + 1 - p)/dq[a];
            delta[a] = 1.0/dq[a];
        } else if (dq[a] < 0.0) {
            step[a] = -1;
            next[a] = t + (cell[a] - p)/dq[a];
            delta[a] = -1.0/dq[a];
        } else {
            step[a] = 0;
            next[a] = 1e300;
            delta[a] = 1e300;
        }
    }

    while (t <= tMax) {
        if (cell[0] >= 0 && cell[0] < sc.w &&
            cell[1] >= 0 && cell[1] < sc.h &&
            cell[2] >= 0 && cell[2] < sc.d) {
            uint32 v = sc.at(cell[0], cell[1], cell[2]);
            if (v) {
                mat = v;
                tOut = float(t);
                return true;
            }
        }
        int a = 0;
        if (next[1] < next[a])
            a = 1;
        if (next[2] < next[a])
            a = 2;
        t = next[a];
        next[a] += delta[a];
        cell[a] += step[a];
    }
    return false;
}

// Compares the octree against the independent voxel-box reference for one
// ray at rayScale = 0: hit/miss, full 32 bit material and distance.
bool checkRay(VoxelOctree &tree, const Scene &sc, Vec3 o, Vec3 dir)
{
    uint32 refMat = 0;
    float refT = 0.0f;
    bool ref = referenceHit(sc, o, dir, refMat, refT);

    uint32 mat = 0;
    float t = 0.0f;
    bool hit = tree.raymarch(o, dir, 0.0f, mat, t);

    if (hit != ref)
        return false;
    if (hit && (mat != refMat || std::fabs(t - refT) > 1e-3f))
        return false;
    return true;
}

// ---------------------------------------------------------------- readers

// Non power-of-two, non-cube volume with a small cache: windowed reads swap
// in and out, and the tail blocks have odd depth (1 and 3) on y and z.
int case_windowed_tail_reads()
{
    ensurePool();

    Scene sc;
    sc.init(13, 11, 9);
    sc.at( 0,  0, 0) = 0x80000001u;
    sc.at(12, 10, 8) = 0xFEDCBA98u; // inside the odd tail slice of every axis
    sc.at( 5,  3, 8) = compressMaterial(Vec3(0, 0, 1), 0.45f); // odd z tail layer
    sc.at(12,  5, 2) = compressMaterial(Vec3(-1, 0, 0), 0.7f);
    sc.at( 3, 10, 4) = compressMaterial(Vec3(0, 1, 0), 0.25f);
    sc.at( 6,  5, 4) = 0x12345678u;

    std::string path = ".task-build/pb_tail.raw";
    TASK_CHECK(writeRaw(path, sc));

    BuildStats stats;
    VoxelOctree parallel(path.c_str(), 2048, 2, &stats);

    VoxelData serialData(path.c_str(), 2048);
    VoxelOctree serial(&serialData);

    TASK_CHECK(stats.topLutBuilds == 1);
    TASK_CHECK(treesEqual(parallel, serial));

    // Center uses the real (non-cube) dimensions over the padded side length
    TASK_CHECK(std::fabs(parallel.center().x - 13.0f/32.0f) < 1e-6f);
    TASK_CHECK(std::fabs(parallel.center().y - 11.0f/32.0f) < 1e-6f);
    TASK_CHECK(std::fabs(parallel.center().z -  9.0f/32.0f) < 1e-6f);

    // Voxels in the odd tail slices must be present in both builds
    TASK_CHECK(checkRay(parallel, sc, Vec3(0.5f, 1.0f + 10.5f/16.0f, 1.0f + 8.5f/16.0f),
            Vec3(1.0f, 0.001f, 0.0021f))); // hits (12,10,8)
    TASK_CHECK(checkRay(parallel, sc, Vec3(1.0f + 5.5f/16.0f, 1.0f + 3.5f/16.0f, 0.5f),
            Vec3(0.001f, 0.0013f, 1.0f))); // hits (5,3,8)
    TASK_CHECK(checkRay(parallel, sc, Vec3(0.5f, 1.0f + 0.5f/16.0f, 1.0f + 0.5f/16.0f),
            Vec3(1.0f, 0.001f, 0.001f))); // hits (0,0,0)

    // The serial path must see the odd tail layer voxels as well
    uint32 mat = 0;
    float t = 0.0f;
    TASK_CHECK(serial.raymarch(Vec3(0.5f, 1.0f + 10.5f/16.0f, 1.0f + 8.5f/16.0f),
            Vec3(1.0f, 0.001f, 0.0021f), 0.0f, mat, t));
    TASK_CHECK(mat == 0xFEDCBA98u);
    TASK_CHECK(serial.raymarch(Vec3(1.0f + 5.5f/16.0f, 1.0f + 3.5f/16.0f, 0.5f),
            Vec3(0.001f, 0.0013f, 1.0f), 0.0f, mat, t));
    TASK_CHECK(mat == compressMaterial(Vec3(0, 0, 1), 0.45f));

    // Empty row through the volume
    TASK_CHECK(checkRay(parallel, sc, Vec3(0.5f, 1.0f + 1.5f/16.0f, 1.0f + 1.5f/16.0f),
            Vec3(1.0f, 0.0013f, 0.0011f)));
    return 0;
}

// Small scene verified against an independent voxel-grid raycast: multiple
// directions, empty regions and distinct full 32 bit materials.
int case_reference_raymarch()
{
    Scene sc;
    sc.init(16, 12, 10);
    sc.at( 2,  3, 4) = compressMaterial(Vec3( 1,  0,  0), 0.6f);
    sc.at( 9,  3, 4) = compressMaterial(Vec3( 0,  1,  0), 0.8f);
    sc.at(15, 11, 9) = 0xF0E1D2C3u;
    sc.at( 0,  0, 0) = 0x80000001u;
    sc.at( 7,  6, 5) = 0x12345678u;
    sc.at( 5,  0, 9) = compressMaterial(Vec3(0, 0, -1), 0.3f);
    sc.at(14,  1, 1) = compressMaterial(Vec3(0, -1, 0), 0.95f);

    std::string path = ".task-build/pb_ref.raw";
    TASK_CHECK(writeRaw(path, sc));

    BuildStats stats;
    VoxelOctree tree(path.c_str(), 4096, 3, &stats);
    TASK_CHECK(stats.topLutBuilds == 1);

    const int vx[] = {2, 9, 15, 0, 7, 5, 14};
    const int vy[] = {3, 3, 11, 0, 6, 0,  1};
    const int vz[] = {4, 4,  9, 0, 5, 9,  1};
    const Vec3 dirs[] = {
        Vec3( 1.0f,  0.21f,  0.37f), Vec3(-1.0f,  0.13f,  0.29f),
        Vec3( 0.17f, 1.0f,   0.23f), Vec3( 0.31f, -1.0f,  0.11f),
        Vec3( 0.19f,  0.27f,  1.0f), Vec3( 0.23f,  0.15f, -1.0f),
        Vec3( 1.0f,   1.0f,   1.0f), Vec3(-1.0f,  0.7f,   1.3f)
    };

    bool ok = true;
    for (int v = 0; v < 7; ++v) {
        Vec3 c = voxelCenter(sc, vx[v], vy[v], vz[v]);
        for (int k = 0; k < 8; ++k)
            ok = checkRay(tree, sc, c - dirs[k]*1.3f, dirs[k]) && ok;
    }
    TASK_CHECK(ok);

    // Baseline-style axis rays with exact distances
    uint32 mat = 0;
    float t = 0.0f;
    TASK_CHECK(tree.raymarch(Vec3(0, 1.21875f, 1.28125f), Vec3(1, 0.001f, 0.001f), 0.0f, mat, t));
    TASK_CHECK(mat == compressMaterial(Vec3(1, 0, 0), 0.6f));
    TASK_CHECK(std::fabs(t - 1.125f) < 1e-4f);
    TASK_CHECK(tree.raymarch(Vec3(3, 1.21875f, 1.28125f), Vec3(-1, 0.001f, 0.001f), 0.0f, mat, t));
    TASK_CHECK(mat == compressMaterial(Vec3(0, 1, 0), 0.8f));
    TASK_CHECK(std::fabs(t - 1.375f) < 1e-4f);

    // Empty regions and rays pointing away from the volume
    TASK_CHECK(checkRay(tree, sc, Vec3(0.5f, 1.0f + 8.5f/16.0f, 1.0f + 3.5f/16.0f),
            Vec3(1.0f, 0.0013f, 0.0017f))); // empty y=8 row
    TASK_CHECK(checkRay(tree, sc, Vec3(1.0f + 4.5f/16.0f, 1.0f + 8.5f/16.0f, 0.5f),
            Vec3(0.0011f, 0.0013f, 1.0f))); // empty x=4 column
    TASK_CHECK(checkRay(tree, sc, Vec3(0.5f, 0.5f, 0.5f), Vec3(-1.0f, -0.3f, -0.2f)));
    return 0;
}

// Empty volume, single voxel volume and virtual padding handling.
int case_edge_scenes()
{
    ensurePool();

    Scene empty;
    empty.init(8, 8, 8);
    std::string emptyPath = ".task-build/pb_empty.raw";
    TASK_CHECK(writeRaw(emptyPath, empty));

    BuildStats st;
    VoxelOctree t0(emptyPath.c_str(), 4096, 2, &st);
    TASK_CHECK(t0.descriptorCount() == 1);
    TASK_CHECK(t0.descriptor(0) == 0x40000u);
    TASK_CHECK(st.topLutBuilds == 1);
    TASK_CHECK(st.workersUsed == 0);
    TASK_CHECK(st.peakInputCacheBytes > 0 && st.peakInputCacheBytes <= 4096);

    uint32 mat = 0;
    float t = 0.0f;
    TASK_CHECK(!t0.raymarch(Vec3(0.5f, 1.5f, 1.5f), Vec3(1.0f, 0.1f, 0.2f), 0.0f, mat, t));

    VoxelData emptyData(emptyPath.c_str(), 4096);
    VoxelOctree emptySerial(&emptyData);
    TASK_CHECK(treesEqual(t0, emptySerial));

    // Single voxel in the far corner of a 5^3 volume (virtual padding to 8)
    Scene one;
    one.init(5, 5, 5);
    one.at(4, 4, 4) = 0xCAFEBABEu;
    std::string onePath = ".task-build/pb_single.raw";
    TASK_CHECK(writeRaw(onePath, one));

    VoxelOctree t1(onePath.c_str(), 4096, 2);
    VoxelData oneData(onePath.c_str(), 4096);
    VoxelOctree oneSerial(&oneData);
    TASK_CHECK(treesEqual(t1, oneSerial));
    TASK_CHECK(std::fabs(t1.center().x - 2.5f/8.0f) < 1e-6f);
    TASK_CHECK(checkRay(t1, one, Vec3(0.5f, 1.0f + 4.5f/8.0f, 1.0f + 4.5f/8.0f),
            Vec3(1.0f, 0.001f, 0.001f)));
    TASK_CHECK(t1.raymarch(Vec3(0.5f, 1.0f + 4.5f/8.0f, 1.0f + 4.5f/8.0f),
            Vec3(1.0f, 0.001f, 0.001f), 0.0f, mat, t));
    TASK_CHECK(mat == 0xCAFEBABEu);
    return 0;
}

// ---------------------------------------------------------------- packing

// Tree large enough to exceed the 14 bit short address range: the 0x10000
// group span and 0x20000 extended address paths must be exercised, and the
// merged layout must equal the serial build bit for bit.
int case_extended_offsets()
{
    ensurePool();

    Scene sc;
    sc.init(64, 64, 64);
    for (int z = 0; z < 64; ++z)
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                if ((x*31 + y*17 + z*13) % 5 == 0)
                    // Bits 16/17 are deliberately kept clear in materials so
                    // the descriptor flag scan below is unambiguous.
                    sc.at(x, y, z) = 0x80000000u | uint32((x*3 + y*5 + z*7) & 0x7F);

    std::string path = ".task-build/pb_large.raw";
    TASK_CHECK(writeRaw(path, sc));

    BuildStats stats;
    VoxelOctree par(path.c_str(), 1 << 20, 4, &stats);

    TASK_CHECK(par.descriptorCount() > 0x3FFF);
    bool sawGroupSpan = false, sawExtended = false;
    for (uint64 i = 0; i < par.descriptorCount(); ++i) {
        uint32 d = par.descriptor(i);
        if (d & 0x10000)
            sawGroupSpan = true;
        if (d & 0x20000)
            sawExtended = true;
    }
    TASK_CHECK(sawGroupSpan);
    TASK_CHECK(sawExtended);

    VoxelData serialData(path.c_str(), 1 << 20);
    VoxelOctree ser(&serialData);
    TASK_CHECK(treesEqual(par, ser));

    // Identical layout must give identical rayScale=0 hits and distances
    int checked = 0;
    bool regionDone[4] = {false, false, false, false};
    for (int z = 0; z < 64 && checked < 4; ++z)
        for (int y = 0; y < 64 && checked < 4; ++y)
            for (int x = 0; x < 64 && checked < 4; ++x) {
                int r = (z >= 32 ? 2 : 0) + (y >= 32 ? 1 : 0);
                if (!regionDone[r] && sc.at(x, y, z)) {
                    regionDone[r] = true;
                    Vec3 dir(0.31f, 0.17f, 1.0f);
                    Vec3 o = voxelCenter(sc, x, y, z) - dir*1.3f;
                    uint32 m1 = 0, m2 = 0;
                    float t1 = 0.0f, t2 = 0.0f;
                    TASK_CHECK(par.raymarch(o, dir, 0.0f, m1, t1));
                    TASK_CHECK(ser.raymarch(o, dir, 0.0f, m2, t2));
                    TASK_CHECK(m1 == m2 && t1 == t2);
                    ++checked;
                }
            }
    TASK_CHECK(checked == 4);

    TASK_CHECK(stats.topLutBuilds == 1);
    TASK_CHECK(stats.workersUsed >= 2);
    TASK_CHECK(stats.peakInputCacheBytes > 0 && stats.peakInputCacheBytes <= uint64(1) << 20);
    return 0;
}

// Single cache windows whose subtrees individually exceed the 14 bit short
// address range: the subtrees contain their own 0x10000 group spans and
// deferred insertion words, which the merge must relocate correctly.
int case_subtree_internal_spans()
{
    ensurePool();

    Scene sc;
    sc.init(64, 64, 64);
    for (int z = 0; z < 64; ++z)
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                if ((x*31 + y*17 + z*13) % 5 == 0)
                    sc.at(x, y, z) = 0x80000000u | uint32((x*3 + y*5 + z*7) & 0x7F);

    std::string path = ".task-build/pb_deep.raw";
    TASK_CHECK(writeRaw(path, sc));

    // 2 MiB budget: the cache window becomes 64^3, so each occupied block
    // builds a subtree with tens of thousands of descriptors.
    BuildStats stats;
    VoxelOctree par(path.c_str(), 2 << 20, 4, &stats);
    TASK_CHECK(par.descriptorCount() > 0x3FFF);

    VoxelData serialData(path.c_str(), 2 << 20);
    VoxelOctree ser(&serialData);
    TASK_CHECK(treesEqual(par, ser));

    VoxelOctree par1(path.c_str(), 2 << 20, 1);
    VoxelOctree par2(path.c_str(), 2 << 20, 2);
    TASK_CHECK(treesEqual(par, par1));
    TASK_CHECK(treesEqual(par, par2));

    // Group spans appear inside individual subtrees, not only at the root
    bool sawGroupSpan = false;
    for (uint64 i = 0; i < par.descriptorCount(); ++i)
        if (par.descriptor(i) & 0x10000)
            sawGroupSpan = true;
    TASK_CHECK(sawGroupSpan);

    // rayScale=0 parity with the serial build on a diagonal ray
    uint32 m1 = 0, m2 = 0;
    float t1 = 0.0f, t2 = 0.0f;
    Vec3 o(0.31f, 1.71f, 0.83f), dir(1.0f, -0.43f, 0.77f);
    bool h1 = par.raymarch(o, dir, 0.0f, m1, t1);
    bool h2 = ser.raymarch(o, dir, 0.0f, m2, t2);
    TASK_CHECK(h1 == h2);
    TASK_CHECK(!h1 || (m1 == m2 && t1 == t2));
    TASK_CHECK(stats.topLutBuilds == 1);
    return 0;
}

// save()/load roundtrip plus byte-identical .oct files for 1, 2 and 4
// workers on an irregular volume that spans many cache windows.
int case_save_reload()
{
    Scene sc;
    sc.init(24, 18, 20);
    for (int i = 0; i < 90; ++i)
        sc.at((i*7 + 3) % 24, (i*11 + 5) % 18, (i*13 + 2) % 20) =
                0x81000000u | (uint32(i)*2654435761u & 0xFFFFu);

    std::string path = ".task-build/pb_save.raw";
    TASK_CHECK(writeRaw(path, sc));

    VoxelOctree t1(path.c_str(), 8192, 1);
    VoxelOctree t2(path.c_str(), 8192, 2);
    VoxelOctree t4(path.c_str(), 8192, 4);
    t1.save(".task-build/pb_w1.oct");
    t2.save(".task-build/pb_w2.oct");
    t4.save(".task-build/pb_w4.oct");

    std::vector<char> f1 = readFile(".task-build/pb_w1.oct");
    std::vector<char> f2 = readFile(".task-build/pb_w2.oct");
    std::vector<char> f4 = readFile(".task-build/pb_w4.oct");
    TASK_CHECK(!f1.empty());
    TASK_CHECK(f1 == f2);
    TASK_CHECK(f1 == f4);

    VoxelOctree loaded(".task-build/pb_w4.oct");
    TASK_CHECK(treesEqual(t4, loaded));
    TASK_CHECK(std::fabs(loaded.center().x - 24.0f/64.0f) < 1e-6f);
    TASK_CHECK(std::fabs(loaded.center().y - 18.0f/64.0f) < 1e-6f);
    TASK_CHECK(std::fabs(loaded.center().z - 20.0f/64.0f) < 1e-6f);

    // The reloaded tree raymarches like the in-memory one
    uint32 m1 = 0, m2 = 0;
    float hitT1 = 0.0f, hitT2 = 0.0f;
    Vec3 o(0.5f, 1.0f + 5.5f/32.0f, 1.0f + 2.5f/32.0f);
    Vec3 dir(1.0f, 0.001f, 0.001f);
    TASK_CHECK(t4.raymarch(o, dir, 0.0f, m1, hitT1));
    TASK_CHECK(loaded.raymarch(o, dir, 0.0f, m2, hitT2));
    TASK_CHECK(m1 == m2 && hitT1 == hitT2);
    TASK_CHECK(m1 == sc.at(3, 5, 2)); // first voxel on that row
    return 0;
}

// ------------------------------------------------------------ parallelism

// Occupied blocks spread over multiple cache windows: with workers=4 the
// work must actually be distributed, the occupancy pre-scan must run once,
// and the reported cache peak must stay within the budget.
int case_stats_and_distribution()
{
    Scene sc;
    sc.init(64, 64, 64);
    for (int b = 0; b < 8; ++b) {
        int ox = (b & 1) ? 56 : 4;
        int oy = (b & 2) ? 56 : 4;
        int oz = (b & 4) ? 56 : 4;
        for (int z = 0; z < 4; ++z)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    sc.at(ox + x, oy + y, oz + z) = 0x80000000u | uint32(1 + b);
    }

    std::string path = ".task-build/pb_stats.raw";
    TASK_CHECK(writeRaw(path, sc));

    BuildStats st4;
    VoxelOctree t4(path.c_str(), 1 << 20, 4, &st4);
    TASK_CHECK(st4.topLutBuilds == 1);
    TASK_CHECK(st4.workersUsed >= 2);
    TASK_CHECK(st4.workersUsed <= 4);
    TASK_CHECK(st4.peakInputCacheBytes > 0 && st4.peakInputCacheBytes <= uint64(1) << 20);

    BuildStats st1;
    VoxelOctree t1(path.c_str(), 1 << 20, 1, &st1);
    TASK_CHECK(st1.topLutBuilds == 1);
    TASK_CHECK(st1.workersUsed == 1);
    TASK_CHECK(treesEqual(t1, t4));

    // The stats pointer is optional
    VoxelOctree t3(path.c_str(), 1 << 20, 3);
    TASK_CHECK(treesEqual(t3, t4));
    return 0;
}

// Same input and cacheBytes: 1, 2 and 4 workers must produce byte-identical
// trees, repeated builds must not drift, and two live instances must stay
// independent.
int case_deterministic_output()
{
    ensurePool();

    Scene sc;
    sc.init(20, 14, 30);
    for (int i = 0; i < 120; ++i)
        sc.at((i*9 + 1) % 20, (i*5 + 2) % 14, (i*7 + 4) % 30) =
                0x80000000u | (uint32(i)*40503u & 0xFFFFu) | 1u;

    std::string path = ".task-build/pb_det.raw";
    TASK_CHECK(writeRaw(path, sc));

    VoxelOctree a(path.c_str(), 4096, 1);
    VoxelOctree b(path.c_str(), 4096, 2);
    VoxelOctree c(path.c_str(), 4096, 4);
    VoxelOctree c2(path.c_str(), 4096, 4);
    TASK_CHECK(treesEqual(a, b));
    TASK_CHECK(treesEqual(a, c));
    TASK_CHECK(treesEqual(c, c2));

    VoxelData serialData(path.c_str(), 4096);
    VoxelOctree ser(&serialData);
    TASK_CHECK(treesEqual(a, ser));

    // Both instances remain usable at the same time
    TASK_CHECK(checkRay(a, sc, Vec3(0.5f, 1.2f, 1.1f), Vec3(1.0f, 0.4f, 0.3f)));
    TASK_CHECK(checkRay(c2, sc, Vec3(0.5f, 1.2f, 1.1f), Vec3(1.0f, 0.4f, 0.3f)));
    TASK_CHECK(checkRay(a, sc, Vec3(1.3f, 0.6f, 1.7f), Vec3(-0.5f, 1.0f, -0.8f)));
    return 0;
}

// workers == 0 and undersized caches must fail with std::invalid_argument,
// and the builder must remain usable afterwards.
int case_invalid_arguments()
{
    Scene sc;
    sc.init(8, 8, 8);
    sc.at(3, 3, 3) = 0xDEADBEEFu;
    std::string path = ".task-build/pb_args.raw";
    TASK_CHECK(writeRaw(path, sc));

    bool threw = false;
    try {
        VoxelOctree t(path.c_str(), 4096, 0);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        VoxelOctree t(path.c_str(), 8, 1); // cannot hold the shared index + one window
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    TASK_CHECK(threw);

    threw = false;
    try {
        VoxelOctree t(".task-build/pb_missing.raw", 4096, 1);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    TASK_CHECK(threw);

    VoxelOctree ok(path.c_str(), 4096, 2);
    TASK_CHECK(ok.descriptorCount() >= 1);
    uint32 mat = 0;
    float t = 0.0f;
    TASK_CHECK(ok.raymarch(Vec3(0.5f, 1.0f + 3.5f/8.0f, 1.0f + 3.5f/8.0f),
            Vec3(1.0f, 0.001f, 0.001f), 0.0f, mat, t));
    TASK_CHECK(mat == 0xDEADBEEFu);
    return 0;
}

} // anonymous namespace

const struct task_case task_cases[] = {
    { "readers",     "windowed_tail_reads",      &case_windowed_tail_reads  },
    { "readers",     "reference_raymarch",       &case_reference_raymarch   },
    { "readers",     "edge_scenes",              &case_edge_scenes          },
    { "packing",     "extended_offsets",         &case_extended_offsets     },
    { "packing",     "subtree_internal_spans",   &case_subtree_internal_spans },
    { "packing",     "save_reload",              &case_save_reload          },
    { "parallelism", "stats_and_distribution",   &case_stats_and_distribution },
    { "parallelism", "deterministic_output",     &case_deterministic_output },
    { "parallelism", "invalid_arguments",        &case_invalid_arguments    },
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
