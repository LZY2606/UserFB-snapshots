#include "checks.h"
#include "Test.h"
#include "SphereBvh.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_MSC_VER)
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace
{
    // Independent scalar reference intersection over the caller's original
    // arrays (no tree involved). Same arithmetic order as the SIMD leaf so the
    // float results agree bit-for-bit: strict positive discriminant, open
    // interval, earlier root first, then later root.
    struct RefHit
    {
        float t;
        int id;
        float3 pos;
        float3 normal;
    };

    bool RefHitSpheres(const std::vector<Sphere>& spheres, const Ray& r, float tMin, float tMax, RefHit& out)
    {
        bool found = false;
        float bestT = tMax;
        int bestId = -1;
        for (int i = 0; i < (int)spheres.size(); ++i)
        {
            const Sphere& s = spheres[i];
            float coX = s.center.x - r.orig.getX();
            float coY = s.center.y - r.orig.getY();
            float coZ = s.center.z - r.orig.getZ();
            float nb = coX * r.dir.getX() + coY * r.dir.getY() + coZ * r.dir.getZ();
            float c = coX * coX + coY * coY + coZ * coZ - s.radius * s.radius;
            float disc = nb * nb - c;
            if (!(disc > 0.0f))
                continue;
            float q = std::sqrt(disc);
            float t0 = nb - q;
            float t1 = nb + q;
            float t = (t0 > tMin) ? t0 : ((t1 > tMin) ? t1 : -1.0f);
            if (!(t > tMin) || !(t < tMax))
                continue;
            if (!found || t < bestT || (t == bestT && i < bestId))
            {
                found = true;
                bestT = t;
                bestId = i;
            }
        }
        if (!found)
            return false;
        float3 pos = r.pointAt(bestT);
        const Sphere& s = spheres[bestId];
        float3 ctr(s.center.x, s.center.y, s.center.z);
        out.t = bestT;
        out.id = bestId;
        out.pos = pos;
        out.normal = (pos - ctr) * s.invRadius;
        return true;
    }

    bool RefOccluded(const std::vector<Sphere>& spheres, const Ray& r, float tMin, float tMax)
    {
        for (int i = 0; i < (int)spheres.size(); ++i)
        {
            const Sphere& s = spheres[i];
            float coX = s.center.x - r.orig.getX();
            float coY = s.center.y - r.orig.getY();
            float coZ = s.center.z - r.orig.getZ();
            float nb = coX * r.dir.getX() + coY * r.dir.getY() + coZ * r.dir.getZ();
            float c = coX * coX + coY * coY + coZ * coZ - s.radius * s.radius;
            float disc = nb * nb - c;
            if (!(disc > 0.0f))
                continue;
            float q = std::sqrt(disc);
            float t0 = nb - q;
            float t1 = nb + q;
            float t = (t0 > tMin) ? t0 : ((t1 > tMin) ? t1 : -1.0f);
            if (t > tMin && t < tMax)
                return true;
        }
        return false;
    }

    struct Rng
    {
        unsigned state;
        explicit Rng(unsigned seed) : state(seed | 1) {}
        unsigned next()
        {
            unsigned x = state;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 15;
            state = x;
            return x;
        }
        float f01() { return (next() & 0xFFFFFF) / 16777216.0f; }
        float range(float a, float b) { return a + (b - a) * f01(); }
    };

    void MakeMaterials(std::vector<Material>& mats, int count)
    {
        mats.resize(count);
        for (int i = 0; i < count; ++i)
        {
            mats[i].type = (Material::Type)(i % 3);
            mats[i].albedo = float3pack(0.2f + 0.6f * float(i % 7) / 6.0f, 0.2f + 0.6f * float((i + 2) % 5) / 4.0f, 0.2f + 0.6f * float((i + 4) % 3) / 2.0f);
            mats[i].emissive = float3pack(0, 0, 0);
            mats[i].roughness = float(i % 4) * 0.2f;
            mats[i].ri = 1.5f;
        }
        // A small, stable set of lights.
        if (count > 16)
        {
            mats[3].emissive = float3pack(4, 4, 4);
            mats[9].emissive = float3pack(2, 3, 5);
            mats[15].emissive = float3pack(5, 2, 2);
        }
    }

    // 4096 disjoint spheres in 512 clusters (8 per cluster), plus materials.
    void MakeClusterScene(std::vector<Sphere>& spheres, std::vector<Material>& mats)
    {
        const int count = 4096;
        const int clustersPerAxis = 8;
        const int clusters = clustersPerAxis * clustersPerAxis * clustersPerAxis;
        const int perCluster = count / clusters;
        Rng rng(0x9e3779b9u);
        spheres.assign(count, Sphere());
        int ci = 0;
        for (int cz = 0; cz < clustersPerAxis; ++cz)
        {
            for (int cy = 0; cy < clustersPerAxis; ++cy)
            {
                for (int cx = 0; cx < clustersPerAxis; ++cx)
                {
                    float baseX = -30.0f + cx * 9.0f;
                    float baseY = -30.0f + cy * 9.0f;
                    float baseZ = -30.0f + cz * 9.0f;
                    for (int k = 0; k < perCluster; ++k)
                    {
                        // Disjoint lattice inside the cluster: radius 0.35,
                        // spacing 1 => never overlapping.
                        int ix = k % 4;
                        int iy = (k / 4) % 4;
                        int iz = k / 16;
                        float jitter = 0.05f;
                        float x = baseX + ix * 1.0f + rng.range(-jitter, jitter);
                        float y = baseY + iy * 1.0f + rng.range(-jitter, jitter);
                        float z = baseZ + iz * 1.0f + rng.range(-jitter, jitter);
                        spheres[ci].center = float3pack(x, y, z);
                        spheres[ci].radius = 0.35f;
                        spheres[ci].UpdateDerivedData();
                        ++ci;
                    }
                }
            }
        }
        assert(ci == count);
        MakeMaterials(mats, count);
    }

    // Float error bound for a root of nb - sqrt(nb^2 - c). The discriminant is
    // a subtraction of two nearly equal magnitudes for near-tangent hits, so
    // the root's rounding grows like eps * (nb^2 + |c|) / (2 * sqrt(disc)),
    // not just as eps * |t|. Compute that bound from the same (float) inputs
    // each side used. The constant is a few float epsilons of headroom for the
    // handful of roundings in the expression.
    float RootErrorBound(float nb, float c, float t)
    {
        float disc = nb * nb - c;
        if (!(disc > 0.0f))
            return 0.0f;
        float q = std::sqrt(disc);
        const double eps = 5e-7; // ~4 float epsilons
        double bound = eps * ((double)nb * nb + std::fabs((double)c)) / (2.0 * (double)q)
                     + 4.0 * eps * (std::fabs((double)t) + 1.0);
        return (float)bound;
    }

    // Sphere data as the BVH saw it (leaf copy), so the bound is computed from
    // the same float values.
    void SphereFactors(const Sphere& s, const Ray& r, float& nb, float& c)
    {
        float coX = s.center.x - r.orig.getX();
        float coY = s.center.y - r.orig.getY();
        float coZ = s.center.z - r.orig.getZ();
        nb = coX * r.dir.getX() + coY * r.dir.getY() + coZ * r.dir.getZ();
        c = coX * coX + coY * coY + coZ * coZ - s.radius * s.radius;
    }

    bool CloseRoots(const Sphere& s, const Ray& r, float a, float b)
    {
        float nb, c;
        SphereFactors(s, r, nb, c);
        float tol = RootErrorBound(nb, c, a) + RootErrorBound(nb, c, b);
        if (tol < 2e-4f * (std::fabs(b) + 1.0f))
            tol = 2e-4f * (std::fabs(b) + 1.0f);
        return std::fabs(a - b) <= tol;
    }

    bool SameHit(const Sphere& s, const Ray& r, const Hit& hit, const RefHit& ref)
    {
        float nb, c;
        SphereFactors(s, r, nb, c);
        float tol = RootErrorBound(nb, c, ref.t);
        float floorTol = 2e-4f * (std::fabs(ref.t) + 1.0f);
        if (tol < floorTol)
            tol = floorTol;
        return std::fabs(hit.t - ref.t) <= tol
            && std::fabs(hit.pos.getX() - ref.pos.getX()) <= tol
            && std::fabs(hit.pos.getY() - ref.pos.getY()) <= tol
            && std::fabs(hit.pos.getZ() - ref.pos.getZ()) <= tol
            && std::fabs(hit.normal.getX() - ref.normal.getX()) <= tol * s.invRadius
            && std::fabs(hit.normal.getY() - ref.normal.getY()) <= tol * s.invRadius
            && std::fabs(hit.normal.getZ() - ref.normal.getZ()) <= tol * s.invRadius
            && std::fabs(sqLength(hit.normal) - sqLength(ref.normal)) <= 0.05f;
    }
}

void nearest_identity(Checks& c)
{
    std::vector<Sphere> spheres;
    std::vector<Material> mats;
    MakeClusterScene(spheres, mats);

    SphereBvh bvh;
    bvh.Build(spheres.data(), (int)spheres.size());
    c.require(bvh.SphereCount() == 4096);
    c.require(bvh.LeafCount() > 0);
    c.require(bvh.NodeCount() > bvh.LeafCount());

    // Rays through the whole clustered volume; compare every field against the
    // independent scalar reference.
    Rng rng(12345u);
    int hits = 0;
    for (int i = 0; i < 2200; ++i)
    {
        float3 org(rng.range(-40, 40), rng.range(-40, 40), rng.range(-40, 40));
        float3 dir(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
        float len = sqrtf(dot(dir, dir));
        if (len < 1e-4f)
            continue;
        dir = normalize(dir);
        Ray r(org, dir);
        Hit hit;
        int id = bvh.ClosestHit(r, kMinT, kMaxT, hit, 0);
        RefHit ref;
        bool refHit = RefHitSpheres(spheres, r, kMinT, kMaxT, ref);
        c.require((id != -1) == refHit);
        if (id != -1)
        {
            ++hits;
            c.require(id == ref.id);
            c.require(SameHit(spheres[id], r, hit, ref));
        }
    }
    c.require(hits > 100);

    // Clustered rays: from far away aimed at cluster centers, so many rays
    // start almost parallel and hit the same region.
    int clusterHits = 0;
    for (int i = 0; i < 600; ++i)
    {
        float3 target(rng.range(-35, 35), rng.range(-35, 35), rng.range(-35, 35));
        float3 org = target + float3(rng.range(-0.01f, 0.01f), rng.range(-0.01f, 0.01f), -60.0f);
        float3 dir = normalize(target - org);
        Ray r(org, dir);
        Hit hit;
        int id = bvh.ClosestHit(r, kMinT, kMaxT, hit, 0);
        RefHit ref;
        bool refHit = RefHitSpheres(spheres, r, kMinT, kMaxT, ref);
        c.require((id != -1) == refHit);
        if (id != -1)
        {
            ++clusterHits;
            c.require(id == ref.id);
            c.require(SameHit(spheres[ref.id], r, hit, ref));
        }
    }
    c.require(clusterHits > 20);

    // Exact ties: two spheres at the same distance along the ray. The smaller
    // original index must win, regardless of tree layout.
    {
        std::vector<Sphere> tie(2);
        tie[0].center = float3pack(-1.0f, 0.0f, -3.0f);
        tie[0].radius = 3.0f; tie[0].UpdateDerivedData();
        tie[1].center = float3pack(1.0f, 0.0f, -3.0f);
        tie[1].radius = 3.0f; tie[1].UpdateDerivedData();
        std::vector<Sphere> tieSwapped(2);
        tieSwapped[0] = tie[1];
        tieSwapped[1] = tie[0];
        SphereBvh tb0, tb1;
        tb0.Build(tie.data(), 2);
        tb1.Build(tieSwapped.data(), 2);
        Ray r(float3(0.0f, 0.0f, 4.0f), float3(0.0f, 0.0f, -1.0f));
        Hit h0, h1;
        c.require(tb0.ClosestHit(r, kMinT, kMaxT, h0, 0) == 0);
        c.require(tb1.ClosestHit(r, kMinT, kMaxT, h1, 0) == 0);
        float expected = 7.0f - sqrtf(8.0f);
        c.require(std::fabs(h0.t - expected) < 1e-4f);
        c.require(h0.t == h1.t);
    }

    // Empty scene.
    {
        SphereBvh empty;
        empty.Build(0, 0);
        Hit h;
        Ray r(float3(0, 0, 0), float3(0, 0, -1));
        c.require(empty.ClosestHit(r, kMinT, kMaxT, h, 0) == -1);
        c.require(!empty.Occluded(r, kMinT, kMaxT, 0));
        ClearCpuScene();
        Hit ch;
        int cid = 0;
        c.require(!HitWorld(r, kMinT, kMaxT, ch, cid));
        c.require(!OccludedWorld(r, kMinT, kMaxT));
    }

    // From inside a sphere.
    {
        std::vector<Sphere> inner(1);
        inner[0].center = float3pack(0, 0, 0); inner[0].radius = 2.0f; inner[0].UpdateDerivedData();
        SphereBvh bvh2;
        bvh2.Build(inner.data(), 1);
        Hit h;
        Ray r(float3(0, 0, 0), float3(0, 0, 1));
        int id = bvh2.ClosestHit(r, kMinT, kMaxT, h, 0);
        c.require(id == 0);
        c.require(std::fabs(h.t - 2.0f) < 1e-4f);
        Ray rb(float3(0, 0, 0), float3(0, 0, -1));
        c.require(bvh2.ClosestHit(rb, kMinT, kMaxT, h, 0) == 0);
        c.require(std::fabs(h.t - 2.0f) < 1e-4f);
        // Occlusion from inside too.
        c.require(bvh2.Occluded(r, kMinT, kMaxT, 0));
    }

    // Signed zero direction components: +0 and -0 in non-primary components
    // must give identical results.
    {
        std::vector<Sphere> axis(1);
        axis[0].center = float3pack(0, 0, -2); axis[0].radius = 0.5f; axis[0].UpdateDerivedData();
        SphereBvh ab;
        ab.Build(axis.data(), 1);
        float pz = +0.0f;
        float nz = -0.0f;
        Ray r1(float3(0, 0, 0), float3(nz, nz, -1.0f));
        Ray r2(float3(0, 0, 0), float3(pz, pz, -1.0f));
        Hit h1, h2;
        int id1 = ab.ClosestHit(r1, kMinT, kMaxT, h1, 0);
        int id2 = ab.ClosestHit(r2, kMinT, kMaxT, h2, 0);
        c.require(id1 == 0 && id2 == 0);
        c.require(h1.t == h2.t);
        c.require(std::fabs(h1.t - 1.5f) < 1e-4f);
        // Outward normal points back toward the origin (+Z).
        c.require(std::fabs(h1.normal.getZ() - 1.0f) < 1e-4f);
        c.require(std::fabs(h2.normal.getZ() - 1.0f) < 1e-4f);
        c.require(ab.Occluded(r1, kMinT, kMaxT, 0) == ab.Occluded(r2, kMinT, kMaxT, 0));
    }

    // Strictly positive discriminant: a ray that exactly grazes a sphere must
    // not register a hit (discriminant == 0).
    {
        std::vector<Sphere> graze(1);
        graze[0].center = float3pack(0, 1.0f, -2.0f); graze[0].radius = 1.0f; graze[0].UpdateDerivedData();
        SphereBvh gb;
        gb.Build(graze.data(), 1);
        Ray r(float3(0, 0, 0), float3(0, 0, -1.0f));
        Hit h;
        c.require(gb.ClosestHit(r, kMinT, kMaxT, h, 0) == -1);
        c.require(!gb.Occluded(r, kMinT, kMaxT, 0));
        // A ray starting a hair closer to the sphere center does hit.
        Ray r3(float3(0, 1e-4f, 0), float3(0.0f, 0.0f, -1.0f));
        c.require(gb.ClosestHit(r3, kMinT, kMaxT, h, 0) == 0);
    }
}

void simd_leaf_tail(Checks& c)
{
    // Scene counts around the leaf size: the final leaf has empty lanes.
    for (int count = 1; count <= 11; ++count)
    {
        std::vector<Sphere> spheres(count);
        std::vector<Material> mats;
        MakeMaterials(mats, count);
        for (int i = 0; i < count; ++i)
        {
            spheres[i].center = float3pack(0.0f, 0.0f, -2.0f - i * 2.0f);
            spheres[i].radius = 0.5f;
            spheres[i].UpdateDerivedData();
        }
        SphereBvh bvh;
        bvh.Build(spheres.data(), count);

        // Every leaf must mark trailing lanes invalid explicitly, and valid
        // lanes must map to real original ids.
        for (int l = 0; l < bvh.LeafCount(); ++l)
        {
            int lanes = bvh.GetLeafLaneCount(l);
            c.require(lanes >= 1 && lanes <= 4);
            for (int lane = 0; lane < 4; ++lane)
            {
                bool valid = bvh.IsLeafLaneValid(l, lane);
                c.require(valid == (lane < lanes));
                if (valid)
                {
                    int prim = bvh.GetLeafPrimitive(l, lane);
                    c.require(prim >= 0 && prim < count);
                }
            }
        }

        // Rays along the line of centers and around the tail, compared to the
        // scalar reference.
        Rng rng(4321u + count);
        for (int i = 0; i < 200; ++i)
        {
            float z = rng.range(-1.0f, -2.0f - (count - 1) * 2.0f - 2.0f);
            float x = rng.range(-0.6f, 0.6f);
            Ray r(float3(x, 0.0f, z), float3(0.0f, 0.0f, -1.0f));
            Hit h;
            int id = bvh.ClosestHit(r, kMinT, kMaxT, h, 0);
            RefHit ref;
            bool refHit = RefHitSpheres(spheres, r, kMinT, kMaxT, ref);
            c.require((id != -1) == refHit);
            if (id != -1)
            {
                c.require(id == ref.id);
                c.require(SameHit(spheres[id], r, h, ref));
            }
            bool occ = bvh.Occluded(r, kMinT, kMaxT, 0);
            c.require(occ == RefOccluded(spheres, r, kMinT, kMaxT));
        }

        // A ray that starts past the last sphere and travels further away
        // must miss. This is exactly where a fake "far away" padding sphere
        // would be hit.
        {
            Ray r(float3(0.0f, 0.0f, -2.0f - (count - 1) * 2.0f - 10.0f), float3(0.0f, 0.0f, -1.0f));
            Hit h;
            c.require(bvh.ClosestHit(r, kMinT, kMaxT, h, 0) == -1);
            c.require(!bvh.Occluded(r, kMinT, kMaxT, 0));
        }

        // Through the CPU scene interface: a miss must not count invalid
        // lanes as sphere tests.
        SetCpuScene(spheres.data(), mats.data(), count);
        {
            Ray r(float3(0.0f, 0.0f, 30.0f), float3(0.0f, 0.0f, 1.0f));
            Hit h;
            int id = 0;
            CpuHitStats stats;
            c.require(!HitWorld(r, kMinT, kMaxT, h, id, &stats));
            c.require(stats.sphereTests == 0);
        }
        // Valid ray through all spheres: tests must be bounded by the leaf
        // count actually traversed, not by count + padding.
        {
        Ray r(float3(0.0f, 0.0f, 30.0f), float3(0.0f, 0.0f, -1.0f));
        Hit h;
        int id = 0;
        CpuHitStats stats;
        c.require(HitWorld(r, kMinT, kMaxT, h, id, &stats));
        c.require(id == 0);
        // Invalid lanes past the last real primitive must be masked out. The
        // total sphere tests can never exceed four per leaf visited.
        c.require(stats.sphereTests <= stats.nodeVisits * 4);
        c.require(stats.nodeVisits <= bvh.NodeCount());
        }
    }
    ClearCpuScene();
}

void animated_refit(Checks& c)
{
    std::vector<Sphere> spheres;
    std::vector<Material> mats;
    MakeClusterScene(spheres, mats);
    const int count = (int)spheres.size();

    SphereBvh bvh;
    bvh.Build(spheres.data(), count);
    const int nodesBefore = bvh.NodeCount();
    const int leavesBefore = bvh.LeafCount();
    std::vector<int> leafPrimsBefore(bvh.LeafCount() * 4);
    for (int l = 0; l < bvh.LeafCount(); ++l)
        for (int lane = 0; lane < 4; ++lane)
            leafPrimsBefore[l * 4 + lane] = bvh.IsLeafLaneValid(l, lane) ? bvh.GetLeafPrimitive(l, lane) : -1;

    auto checkTopologyUnchanged = [&]()
    {
        c.require(bvh.NodeCount() == nodesBefore);
        c.require(bvh.LeafCount() == leavesBefore);
        for (int l = 0; l < bvh.LeafCount(); ++l)
            for (int lane = 0; lane < 4; ++lane)
            {
                int prim = bvh.IsLeafLaneValid(l, lane) ? bvh.GetLeafPrimitive(l, lane) : -1;
                c.require(prim == leafPrimsBefore[l * 4 + lane]);
            }
    };

    // Move sphere 0 far away out of its cluster, and sphere 100 the other way.
    std::vector<Sphere> moved = spheres;
    moved[0].center = float3pack(-120.0f, 0.0f, 0.0f);
    moved[100].center = float3pack(120.0f, 0.0f, 0.0f);
    moved[0].UpdateDerivedData();
    moved[100].UpdateDerivedData();
    bvh.Refit(moved.data(), count);
    checkTopologyUnchanged();

    {
        Ray r(float3(-120.0f, 0.0f, 20.0f), float3(0.0f, 0.0f, -1.0f));
        Hit h;
        int id = bvh.ClosestHit(r, kMinT, kMaxT, h, 0);
        c.require(id == 0);
        RefHit ref;
        c.require(RefHitSpheres(moved, r, kMinT, kMaxT, ref) && ref.id == 0);
        c.require(SameHit(moved[id], r, h, ref));
    }
    {
        Ray r(float3(120.0f, 0.0f, 20.0f), float3(0.0f, 0.0f, -1.0f));
        Hit h;
        int id = bvh.ClosestHit(r, kMinT, kMaxT, h, 0);
        c.require(id == 100);
        RefHit ref;
        c.require(RefHitSpheres(moved, r, kMinT, kMaxT, ref) && ref.id == 100);
        c.require(SameHit(moved[id], r, h, ref));
    }
    // Shadow occlusion across the moved geometry, plus a clearly empty ray.
    {
        Ray r(float3(-120.0f, 0.0f, 10.0f), float3(0.0f, 0.0f, -1.0f));
        c.require(bvh.Occluded(r, kMinT, kMaxT, 0));
        c.require(RefOccluded(moved, r, kMinT, kMaxT));
        Ray miss(float3(-110.0f, 50.0f, 10.0f), float3(0.0f, 0.0f, -1.0f));
        c.require(!bvh.Occluded(miss, kMinT, kMaxT, 0));
        c.require(!RefOccluded(moved, miss, kMinT, kMaxT));
    }

    // Move the same spheres in the opposite direction; refit again.
    std::vector<Sphere> moved2 = moved;
    moved2[0].center = float3pack(0.0f, 0.0f, 60.0f);
    moved2[100].center = float3pack(0.0f, 0.0f, -60.0f);
    moved2[0].UpdateDerivedData();
    moved2[100].UpdateDerivedData();
    bvh.Refit(moved2.data(), count);
    checkTopologyUnchanged();
    {
        Ray r(float3(0.0f, 0.0f, 80.0f), float3(0.0f, 0.0f, -1.0f));
        Hit h;
        int id = bvh.ClosestHit(r, kMinT, kMaxT, h, 0);
        c.require(id == 0);
        RefHit ref;
        c.require(RefHitSpheres(moved2, r, kMinT, kMaxT, ref) && ref.id == 0);
        c.require(SameHit(moved2[id], r, h, ref));
    }
    {
        Ray r(float3(0.0f, 0.0f, -80.0f), float3(0.0f, 0.0f, 1.0f));
        Hit h;
        int id = bvh.ClosestHit(r, kMinT, kMaxT, h, 0);
        c.require(id == 100);
        RefHit ref;
        c.require(RefHitSpheres(moved2, r, kMinT, kMaxT, ref) && ref.id == 100);
        c.require(SameHit(moved2[id], r, h, ref));
    }
    {
        Ray r(float3(0.0f, 0.0f, 59.5f), float3(0.0f, 0.0f, -1.0f));
        c.require(bvh.Occluded(r, kMinT, kMaxT, 0));
        c.require(RefOccluded(moved2, r, kMinT, kMaxT));
    }

    // Full-scene equivalence after refit.
    Rng rng(777u);
    for (int i = 0; i < 400; ++i)
    {
        float3 org(rng.range(-130, 130), rng.range(-130, 130), rng.range(-130, 130));
        float3 dir(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
        float len = sqrtf(dot(dir, dir));
        if (len < 1e-4f) continue;
        dir = normalize(dir);
        Ray r(org, dir);
        Hit h;
        int id = bvh.ClosestHit(r, kMinT, kMaxT, h, 0);
        RefHit ref;
        bool refHit = RefHitSpheres(moved2, r, kMinT, kMaxT, ref);
        c.require((id != -1) == refHit);
        if (id != -1)
        {
            c.require(id == ref.id);
            c.require(SameHit(moved2[id], r, h, ref));
        }
        c.require(bvh.Occluded(r, kMinT, kMaxT, 0) == RefOccluded(moved2, r, kMinT, kMaxT));
    }

    // The Test.h refit entry point, and per-query (not shared) stats.
    c.require(!RefitCpuScene(moved2.data(), count + 1)); // count mismatch: rejected
    SetCpuScene(moved2.data(), mats.data(), count);
    c.require(RefitCpuScene(moved2.data(), count));
    {
        Ray r(float3(0.0f, 0.0f, 80.0f), float3(0.0f, 0.0f, -1.0f));
        Hit h1, h2;
        int id1 = 0, id2 = 0;
        CpuHitStats s1, s2;
        c.require(HitWorld(r, kMinT, kMaxT, h1, id1, &s1));
        c.require(HitWorld(r, kMinT, kMaxT, h2, id2, &s2));
        c.require(id1 == 0 && id2 == 0);
        c.require(s1.nodeVisits == s2.nodeVisits);
        c.require(s1.sphereTests == s2.sphereTests);
        c.require(s1.sphereTests > 0);
    }
    // Changing the count requires a rebuild, not a refit: the refit entry
    // rejects it, and setting a new scene rebuilds and reports the new ids.
    c.require(!RefitCpuScene(moved2.data(), count - 1));
    {
        std::vector<Sphere> fewer(spheres.begin(), spheres.begin() + 10);
        std::vector<Material> fewerMats = mats;
        fewerMats.resize(10);
        SetCpuScene(fewer.data(), fewerMats.data(), 10);
        Ray r(float3(0.0f, 0.0f, 200.0f), float3(0.0f, 0.0f, -1.0f));
        Hit h;
        int id = 0;
        CpuHitStats stats;
        // Only 10 original ids remain in the scene.
        c.require(HitWorld(r, kMinT, kMaxT, h, id, &stats) ? (id >= 0 && id < 10) : true);
        RefHit ref;
        c.require((id != -1) == RefHitSpheres(fewer, r, kMinT, kMaxT, ref));
        if (id != -1)
            c.require(id == ref.id);
    }
    ClearCpuScene();

    // Finite shadow intervals: a sphere just outside the given interval must
    // not shadow it, while one inside must.
    {
        std::vector<Sphere> occl(3);
        // Radius 0.25 is exact in binary and the center is at z=-1, so the
        // entry root is exactly 0.75: usable for open-interval endpoint tests.
        occl[0].center = float3pack(0, 0, -1.0f); occl[0].radius = 0.25f; occl[0].UpdateDerivedData();
        occl[1].center = float3pack(0, 0, -5.0f); occl[1].radius = 0.2f; occl[1].UpdateDerivedData();
        occl[2].center = float3pack(0, 0, -20.0f); occl[2].radius = 0.2f; occl[2].UpdateDerivedData();
        SphereBvh ob;
        ob.Build(occl.data(), 3);
        Ray r(float3(0, 0, 0), float3(0, 0, -1.0f));
        // Interval ends before the first sphere.
        c.require(!ob.Occluded(r, kMinT, 0.5f, 0));
        c.require(!RefOccluded(occl, r, kMinT, 0.5f));
        // Interval contains only the first sphere.
        c.require(ob.Occluded(r, kMinT, 2.0f, 0));
        c.require(RefOccluded(occl, r, kMinT, 2.0f));
        // Interval starts after the first sphere, before the second.
        c.require(!ob.Occluded(r, 2.0f, 4.0f, 0));
        c.require(!RefOccluded(occl, r, 2.0f, 4.0f));
        // Interval contains only the third sphere.
        c.require(ob.Occluded(r, 10.0f, 30.0f, 0));
        c.require(RefOccluded(occl, r, 10.0f, 30.0f));
        // Open endpoints: the first sphere's entry root is exactly 0.75, so
        // it does not count when the upper bound is 0.75.
        c.require(!ob.Occluded(r, kMinT, 0.75f, 0));
        c.require(!RefOccluded(occl, r, kMinT, 0.75f));
        // ...but it does when the bound is just past it.
        c.require(ob.Occluded(r, kMinT, 0.7501f, 0));
        c.require(RefOccluded(occl, r, kMinT, 0.7501f));
        // Closest-hit uses the same finite interval.
        Hit h;
        c.require(ob.ClosestHit(r, 2.0f, 4.0f, h, 0) == -1);
        c.require(ob.ClosestHit(r, 10.0f, 30.0f, h, 0) == 2);
    }

    // Concurrent traversal: queries are const, allocate nothing and touch no
    // shared state. Many threads hammer the same tree and must all see the
    // same answer (and filled-in stats must be per-query).
    {
        struct ThreadArg
        {
            const SphereBvh* bvh;
            const std::vector<Ray>* rays;
            const int* serialIds;
            int hits;
            int mismatches;
            long long tests;
        };
        const int kThreads = 8;
        ThreadArg args[kThreads];
        std::vector<Ray> concurrentRays;
        Rng rng(2468u);
        for (int i = 0; i < 300; ++i)
        {
            float3 org(rng.range(-35, 35), rng.range(-35, 35), rng.range(-35, 35));
            float3 dir(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1));
            if (sqLength(dir) < 1e-6f)
                dir = float3(0, 0, -1);
            concurrentRays.push_back(Ray(org, normalize(dir)));
        }
        // Serial answers first.
        std::vector<int> serialIds(concurrentRays.size());
        for (int i = 0; i < (int)concurrentRays.size(); ++i)
        {
            Hit h;
            serialIds[i] = bvh.ClosestHit(concurrentRays[i], kMinT, kMaxT, h, 0);
        }
        for (int t = 0; t < kThreads; ++t)
        {
            args[t].bvh = &bvh;
            args[t].rays = &concurrentRays;
            args[t].serialIds = serialIds.data();
            args[t].hits = 0;
            args[t].mismatches = 0;
            args[t].tests = 0;
        }
#if defined(_MSC_VER)
        auto worker = [](void* p) -> unsigned {
            ThreadArg* a = (ThreadArg*)p;
            for (int i = 0; i < (int)a->rays->size(); ++i)
            {
                Hit h;
                SphereBvh::QueryStats stats;
                int id = a->bvh->ClosestHit((*a->rays)[i], kMinT, kMaxT, h, &stats);
                a->hits += (id != -1);
                a->tests += stats.sphereTests;
            }
            return 0;
        };
        HANDLE handles[kThreads];
        for (int t = 0; t < kThreads; ++t)
            handles[t] = (HANDLE)_beginthreadex(0, 0, worker, &args[t], 0, 0);
        for (int t = 0; t < kThreads; ++t)
        {
            WaitForSingleObject(handles[t], INFINITE);
            CloseHandle(handles[t]);
        }
#else
        auto worker = [](void* p) -> void* {
            ThreadArg* a = (ThreadArg*)p;
            for (int i = 0; i < (int)a->rays->size(); ++i)
            {
                Hit h;
                SphereBvh::QueryStats stats;
                int id = a->bvh->ClosestHit((*a->rays)[i], kMinT, kMaxT, h, &stats);
                a->hits += (id != -1);
                a->tests += stats.sphereTests;
                if (id != -1 && id != a->serialIds[i])
                    ++a->mismatches;
            }
            return 0;
        };
        pthread_t threads[kThreads];
        for (int t = 0; t < kThreads; ++t)
            pthread_create(&threads[t], 0, worker, &args[t]);
        for (int t = 0; t < kThreads; ++t)
            pthread_join(threads[t], 0);
#endif
        for (int t = 0; t < kThreads; ++t)
        {
            c.require(args[t].hits == args[0].hits);
            c.require(args[t].tests == args[0].tests);
            c.require(args[t].mismatches == 0);
        }
    }

    // The built-in animated scene must keep the tree in sync: the built-in
    // UpdateTest path refits, and results match the exhaustive reference.
    {
        ResetCpuScene();
        // Pick sphere 1 (animated on y) and shoot at its animated location.
        float t = 1.25f;
        UpdateTest(t, 0, 16, 16, kFlagAnimate);
        int id = 0;
        Hit h;
        // At t=1.25 sphere 1 sits at y = cos(1.25)+1.
        float y = cosf(t) + 1.0f;
        Ray r(float3(2.0f, y, 2.0f), float3(0.0f, 0.0f, -1.0f));
        CpuHitStats stats;
        c.require(HitWorld(r, kMinT, kMaxT, h, id, &stats));
        c.require(id == 1);
        // The accelerated answer must agree with an exhaustive scan of the
        // same built-in arrays exported through GetSceneDesc.
        int count2 = 0, objSize = 0, matSize = 0, camSize = 0;
        GetObjectCount(count2, objSize, matSize, camSize);
        // The built-in scene has 46 spheres (DO_BIG_SCENE in Test.cpp).
        c.require(count2 == 46);
        c.require(objSize == 20 && matSize == 36 && camSize == 88);
        std::vector<Sphere> exported(count2);
        std::vector<Material> exportedMats(count2);
        Camera cam;
        std::vector<int> emissives(count2);
        int emissiveCount = 0;
        GetSceneDesc(exported.data(), exportedMats.data(), &cam, emissives.data(), &emissiveCount);
        c.require(exported[1].center.y == y);
        c.require(emissiveCount >= 1);
        RefHit ref;
        c.require(RefHitSpheres(exported, r, kMinT, kMaxT, ref));
        c.require(ref.id == id);
        c.require(std::fabs(ref.t - h.t) < 1e-4f);
        ResetCpuScene();
    }
}

void render_acceleration(Checks& c)
{
    // ---- Large-scene traversal statistics ----
    std::vector<Sphere> spheres;
    std::vector<Material> mats;
    MakeClusterScene(spheres, mats);
    const int count = (int)spheres.size();

    SphereBvh bvh;
    bvh.Build(spheres.data(), count);

    // Clustered ray set: rays aimed at cluster centers from just outside.
    std::vector<Ray> rays;
    Rng rng(555u);
    const int clustersPerAxis = 8;
    for (int i = 0; i < 2000; ++i)
    {
        int cluster = rng.next() % (clustersPerAxis * clustersPerAxis * clustersPerAxis);
        float cx = -30.0f + (cluster % clustersPerAxis) * 9.0f + 0.5f;
        float cy = -30.0f + ((cluster / clustersPerAxis) % clustersPerAxis) * 9.0f + 0.5f;
        float cz = -30.0f + (cluster / (clustersPerAxis * clustersPerAxis)) * 9.0f + 0.5f;
        rays.push_back(Ray(float3(cx + rng.range(-2, 2), cy + rng.range(-2, 2), cz + 40.0f), float3(0.0f, 0.0f, -1.0f)));
    }

    long long totalSphereTests = 0;
    long long totalAllScan = 0;
    for (int i = 0; i < (int)rays.size(); ++i)
    {
        SphereBvh::QueryStats stats;
        Hit h;
        bvh.ClosestHit(rays[i], kMinT, kMaxT, h, &stats);
        totalSphereTests += stats.sphereTests;
        totalAllScan += count;
    }
    std::printf("cluster rays: bvh sphere tests=%lld full scan=%lld ratio=%.4f\n",
                totalSphereTests, totalAllScan, (double)totalSphereTests / (double)totalAllScan);
    c.require(totalSphereTests * 8 < totalAllScan);
    c.require(totalSphereTests > 0);

    // Shadow queries only need existence; also well below full scan.
    long long occTests = 0;
    for (int i = 0; i < (int)rays.size(); ++i)
    {
        SphereBvh::QueryStats stats;
        bvh.Occluded(rays[i], kMinT, kMaxT, &stats);
        occTests += stats.sphereTests;
    }
    std::printf("cluster rays: occlusion sphere tests=%lld full scan=%lld ratio=%.4f\n",
                occTests, totalAllScan, (double)occTests / (double)totalAllScan);
    c.require(occTests * 8 < totalAllScan);

    // ---- Small scene, accelerated vs exhaustive linear RGB ----
    std::vector<Sphere> small;
    std::vector<Material> smallMats;
    {
        small.resize(24);
        MakeMaterials(smallMats, 24);
        Rng srng(9001u);
        for (int i = 0; i < 24; ++i)
        {
            small[i].center = float3pack(srng.range(-4, 4), srng.range(-1, 2), srng.range(-6, 1));
            small[i].radius = srng.range(0.2f, 0.8f);
            small[i].UpdateDerivedData();
        }
        smallMats[3].emissive = float3pack(6, 5, 4);
        smallMats[11].emissive = float3pack(3, 4, 6);
    }
    const int w = 24, h = 16;

    SetCpuScene(small.data(), smallMats.data(), (int)small.size());
    UpdateTest(0.0f, 0, w, h, kFlagProgressive);
    SetCpuAccelerationEnabled(true);
    std::vector<float> pixBvh(w * h * 4, 0.0f);
    int rayCountBvh = 0;
    DrawTest(0.0f, 0, w, h, pixBvh.data(), rayCountBvh, kFlagProgressive);
    CpuTraceStats statsBvh;
    GetCpuTraceStats(statsBvh);
    c.require(rayCountBvh > w * h * 4);
    c.require(statsBvh.rayCount == rayCountBvh);
    c.require(statsBvh.sphereTests > 0);

    SetCpuAccelerationEnabled(false);
    UpdateTest(0.0f, 0, w, h, kFlagProgressive);
    std::vector<float> pixRef(w * h * 4, 0.0f);
    int rayCountRef = 0;
    DrawTest(0.0f, 0, w, h, pixRef.data(), rayCountRef, kFlagProgressive);
    CpuTraceStats statsRef;
    GetCpuTraceStats(statsRef);
    c.require(rayCountRef == rayCountBvh);

    // Same ray sequence both times (frame 0 means no accumulation), so the
    // linear RGB results must agree.
    int differing = 0;
    for (int i = 0; i < w * h; ++i)
    {
        for (int ch = 0; ch < 3; ++ch)
        {
            float a = pixBvh[i * 4 + ch];
            float b = pixRef[i * 4 + ch];
            c.require(std::isfinite(a) && std::isfinite(b));
            c.require(a >= 0.0f && b >= 0.0f);
            if (std::fabs(a - b) > 1e-4f)
                ++differing;
        }
    }
    std::printf("small scene render: accelerated vs exhaustive differing components=%d of %d\n",
                differing, w * h * 3);
    c.require(differing == 0);

    c.require(statsBvh.sphereTests < statsRef.sphereTests);
    std::printf("small scene render: bvh sphere tests=%lld exhaustive sphere tests=%lld rays=%lld\n",
                statsBvh.sphereTests, statsRef.sphereTests, statsBvh.rayCount);

    // PPM output (linear RGB, no gamma).
    const char* names[2] = { "support_sphere_bvh_accel.ppm", "support_sphere_bvh_ref.ppm" };
    const std::vector<float>* imgs[2] = { &pixBvh, &pixRef };
    for (int k = 0; k < 2; ++k)
    {
        FILE* f = std::fopen(names[k], "wb");
        if (!f)
            continue;        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; ++i)
            for (int ch = 0; ch < 3; ++ch)
            {
                float v = (*imgs[k])[i * 4 + ch];
                v = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
                unsigned char u = (unsigned char)(v * 255.0f + 0.5f);
                std::fwrite(&u, 1, 1, f);
            }
        std::fclose(f);
    }

    SetCpuAccelerationEnabled(true);
    ClearCpuScene();

    std::printf("large scene: spheres=%d nodes=%d leaves=%d\n", count, bvh.NodeCount(), bvh.LeafCount());
}
