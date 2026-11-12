#include "SphereBvh.h"

#include <algorithm>
#include <cmath>

#if defined(__EMSCRIPTEN__)
// MathSimd.h has no SIMD implementation for Emscripten (CPU_CAN_DO_SIMD is 0).
// Provide the same 4-lane semantics with scalar lanes so the leaf code stays
// structured as a four-wide operation with explicit lane masks; the wasm build
// executes the lanes one at a time.
struct float4
{
    float v[4];
    float4() {}
    explicit float4(float x) { v[0] = v[1] = v[2] = v[3] = x; }
    explicit float4(const float* p) { v[0] = p[0]; v[1] = p[1]; v[2] = p[2]; v[3] = p[3]; }
    float4(float x, float y, float z, float w) { v[0] = x; v[1] = y; v[2] = z; v[3] = w; }
    float getX() const { return v[0]; }
    float getY() const { return v[1]; }
    float getZ() const { return v[2]; }
    float getW() const { return v[3]; }
};
typedef float4 bool4;
inline float4 operator+(float4 a, float4 b) { return float4(a.v[0]+b.v[0], a.v[1]+b.v[1], a.v[2]+b.v[2], a.v[3]+b.v[3]); }
inline float4 operator-(float4 a, float4 b) { return float4(a.v[0]-b.v[0], a.v[1]-b.v[1], a.v[2]-b.v[2], a.v[3]-b.v[3]); }
inline float4 operator*(float4 a, float4 b) { return float4(a.v[0]*b.v[0], a.v[1]*b.v[1], a.v[2]*b.v[2], a.v[3]*b.v[3]); }
inline bool4 operator==(float4 a, float4 b) { return bool4(a.v[0]==b.v[0], a.v[1]==b.v[1], a.v[2]==b.v[2], a.v[3]==b.v[3]); }
inline bool4 operator!=(float4 a, float4 b) { return bool4(a.v[0]!=b.v[0], a.v[1]!=b.v[1], a.v[2]!=b.v[2], a.v[3]!=b.v[3]); }
inline bool4 operator>(float4 a, float4 b) { return bool4(a.v[0]>b.v[0], a.v[1]>b.v[1], a.v[2]>b.v[2], a.v[3]>b.v[3]); }
inline bool4 operator<(float4 a, float4 b) { return bool4(a.v[0]<b.v[0], a.v[1]<b.v[1], a.v[2]<b.v[2], a.v[3]<b.v[3]); }
inline bool4 operator&(bool4 a, bool4 b) { return bool4(a.v[0]&&b.v[0] ? 1.0f:0.0f, a.v[1]&&b.v[1] ? 1.0f:0.0f, a.v[2]&&b.v[2] ? 1.0f:0.0f, a.v[3]&&b.v[3] ? 1.0f:0.0f); }
inline bool4 operator|(bool4 a, bool4 b) { return bool4(a.v[0]||b.v[0] ? 1.0f:0.0f, a.v[1]||b.v[1] ? 1.0f:0.0f, a.v[2]||b.v[2] ? 1.0f:0.0f, a.v[3]||b.v[3] ? 1.0f:0.0f); }
inline unsigned mask(float4 a) { return (a.v[0]!=0.0f ? 1u:0u) | (a.v[1]!=0.0f ? 2u:0u) | (a.v[2]!=0.0f ? 4u:0u) | (a.v[3]!=0.0f ? 8u:0u); }
inline bool any(bool4 v) { return mask(v) != 0; }
inline float4 sqrtf(float4 v) { return float4(std::sqrt(v.v[0]), std::sqrt(v.v[1]), std::sqrt(v.v[2]), std::sqrt(v.v[3])); }
inline float4 select(float4 a, float4 b, bool4 cond)
{
    return float4(cond.v[0]!=0.0f ? b.v[0]:a.v[0], cond.v[1]!=0.0f ? b.v[1]:a.v[1],
                  cond.v[2]!=0.0f ? b.v[2]:a.v[2], cond.v[3]!=0.0f ? b.v[3]:a.v[3]);
}
#endif

namespace
{
    const int kLeafSize = 4;
    // Bounds are expanded slightly when testing, so that floating point
    // rounding of the ray/slab arithmetic can never prune away a real hit.
    // Expansion only creates false positives (extra traversal), never false
    // negatives. The slab arithmetic itself is done in double for robustness
    // on large scenes and near-parallel rays.
    inline double BoundPad(double lo, double hi)
    {
        double m = std::max(1.0, std::max(std::fabs(lo), std::fabs(hi)));
        return 1e-5 * m;
    }

    inline void Splats(const Ray& r, float4& ox, float4& oy, float4& oz, float4& dx, float4& dy, float4& dz)
    {
        ox = float4(r.orig.getX()); oy = float4(r.orig.getY()); oz = float4(r.orig.getZ());
        dx = float4(r.dir.getX()); dy = float4(r.dir.getY()); dz = float4(r.dir.getZ());
    }
}

SphereBvh::SphereBvh()
    : m_sphereCount(0), m_leafCount(0), m_root(-1)
{
}

SphereBvh::~SphereBvh()
{
}

void SphereBvh::Clear()
{
    m_nodes.clear();
    m_buildIds.clear();
    m_leafIds.clear();
    m_leafCenterX.clear();
    m_leafCenterY.clear();
    m_leafCenterZ.clear();
    m_leafSqRadius.clear();
    m_leafInvRadius.clear();
    m_leafValid.clear();
    m_leafCounts.clear();
    m_sphereCount = 0;
    m_leafCount = 0;
    m_root = -1;
}

void SphereBvh::Build(const Sphere* spheres, int count)
{
    Clear();
    if (count <= 0 || spheres == 0)
        return;
    m_sphereCount = count;
    m_buildIds.resize(count);
    for (int i = 0; i < count; ++i)
        m_buildIds[i] = i;
    m_nodes.reserve(2 * count / kLeafSize + 8);
    m_root = BuildNode(0, count, spheres);
}

void SphereBvh::Refit(const Sphere* spheres, int count)
{
    if (count != m_sphereCount || count <= 0 || spheres == 0)
    {
        Build(spheres, count);
        return;
    }
    if (m_root < 0)
        return;
    RefitNode(m_root, spheres);
}

int SphereBvh::BuildNode(int begin, int end, const Sphere* spheres)
{
    int nodeIndex = (int)m_nodes.size();
    m_nodes.push_back(Node());

    // Leaf: pack the primitive ids into a separate, 4-wide SIMD friendly
    // array. Global sphere ids stay in m_leafIds; the geometry is packed
    // separately. Unused lanes are explicitly marked invalid.
    if (end - begin <= kLeafSize)
    {
        int leafIndex = m_leafCounts.size();
        int laneCount = end - begin;
        m_leafCounts.push_back(laneCount);
        m_leafIds.resize((leafIndex + 1) * kLeafSize);
        m_leafCenterX.resize((leafIndex + 1) * kLeafSize);
        m_leafCenterY.resize((leafIndex + 1) * kLeafSize);
        m_leafCenterZ.resize((leafIndex + 1) * kLeafSize);
        m_leafSqRadius.resize((leafIndex + 1) * kLeafSize);
        m_leafInvRadius.resize((leafIndex + 1) * kLeafSize);
        m_leafValid.resize((leafIndex + 1) * kLeafSize);
        float bminX = 0, bminY = 0, bminZ = 0, bmaxX = 0, bmaxY = 0, bmaxZ = 0;
        for (int lane = 0; lane < kLeafSize; ++lane)
        {
            int at = leafIndex * kLeafSize + lane;
            if (lane < laneCount)
            {
                int id = m_buildIds[begin + lane];
                const Sphere& s = spheres[id];
                float cx = s.center.x, cy = s.center.y, cz = s.center.z;
                m_leafIds[at] = id;
                m_leafCenterX[at] = cx;
                m_leafCenterY[at] = cy;
                m_leafCenterZ[at] = cz;
                m_leafSqRadius[at] = s.radius * s.radius;
                m_leafInvRadius[at] = s.invRadius;
                m_leafValid[at] = 1.0f;
                if (lane == 0)
                {
                    bminX = cx - s.radius; bminY = cy - s.radius; bminZ = cz - s.radius;
                    bmaxX = cx + s.radius; bmaxY = cy + s.radius; bmaxZ = cz + s.radius;
                }
                else
                {
                    bminX = std::min(bminX, cx - s.radius);
                    bminY = std::min(bminY, cy - s.radius);
                    bminZ = std::min(bminZ, cz - s.radius);
                    bmaxX = std::max(bmaxX, cx + s.radius);
                    bmaxY = std::max(bmaxY, cy + s.radius);
                    bmaxZ = std::max(bmaxZ, cz + s.radius);
                }
            }
            else
            {
                // Invalid lane: geometry is zeroed and the validity mask is
                // zero. It is masked off explicitly by the query; no fake
                // sphere is placed anywhere in space.
                m_leafIds[at] = -1;
                m_leafCenterX[at] = 0.0f;
                m_leafCenterY[at] = 0.0f;
                m_leafCenterZ[at] = 0.0f;
                m_leafSqRadius[at] = 0.0f;
                m_leafInvRadius[at] = 0.0f;
                m_leafValid[at] = 0.0f;
            }
        }
        m_leafCount = leafIndex + 1;
        m_nodes[nodeIndex].bmin = float3pack(bminX, bminY, bminZ);
        m_nodes[nodeIndex].bmax = float3pack(bmaxX, bmaxY, bmaxZ);
        m_nodes[nodeIndex].child0 = leafIndex;
        m_nodes[nodeIndex].child1 = -1;
        m_nodes[nodeIndex].count = laneCount;
        m_nodes[nodeIndex].pad = 0;
        return nodeIndex;
    }

    // Split by the largest centroid extent, but always at the element
    // median, so the tree stays balanced even when many centers coincide.
    float cminX = spheres[m_buildIds[begin]].center.x, cminY = spheres[m_buildIds[begin]].center.y, cminZ = spheres[m_buildIds[begin]].center.z;
    float cmaxX = cminX, cmaxY = cminY, cmaxZ = cminZ;
    for (int i = begin + 1; i < end; ++i)
    {
        const Sphere& s = spheres[m_buildIds[i]];
        cminX = std::min(cminX, s.center.x); cmaxX = std::max(cmaxX, s.center.x);
        cminY = std::min(cminY, s.center.y); cmaxY = std::max(cmaxY, s.center.y);
        cminZ = std::min(cminZ, s.center.z); cmaxZ = std::max(cmaxZ, s.center.z);
    }
    float ex = cmaxX - cminX, ey = cmaxY - cminY, ez = cmaxZ - cminZ;
    int axis = 0;
    if (ey > ex && ey >= ez) axis = 1;
    else if (ez > ex && ez > ey) axis = 2;

    const Sphere* base = spheres;
    if (axis == 0)
        std::nth_element(m_buildIds.begin() + begin, m_buildIds.begin() + (begin + end) / 2,
                         m_buildIds.begin() + end, [base](int a, int b) { return base[a].center.x < base[b].center.x; });
    else if (axis == 1)
        std::nth_element(m_buildIds.begin() + begin, m_buildIds.begin() + (begin + end) / 2,
                         m_buildIds.begin() + end, [base](int a, int b) { return base[a].center.y < base[b].center.y; });
    else
        std::nth_element(m_buildIds.begin() + begin, m_buildIds.begin() + (begin + end) / 2,
                         m_buildIds.begin() + end, [base](int a, int b) { return base[a].center.z < base[b].center.z; });

    int mid = (begin + end) / 2;
    int c0 = BuildNode(begin, mid, spheres);
    int c1 = BuildNode(mid, end, spheres);
    m_nodes[nodeIndex].bmin = float3pack(
        std::min(m_nodes[c0].bmin.x, m_nodes[c1].bmin.x),
        std::min(m_nodes[c0].bmin.y, m_nodes[c1].bmin.y),
        std::min(m_nodes[c0].bmin.z, m_nodes[c1].bmin.z));
    m_nodes[nodeIndex].bmax = float3pack(
        std::max(m_nodes[c0].bmax.x, m_nodes[c1].bmax.x),
        std::max(m_nodes[c0].bmax.y, m_nodes[c1].bmax.y),
        std::max(m_nodes[c0].bmax.z, m_nodes[c1].bmax.z));
    m_nodes[nodeIndex].child0 = c0;
    m_nodes[nodeIndex].child1 = c1;
    m_nodes[nodeIndex].count = 0;
    m_nodes[nodeIndex].pad = 0;
    return nodeIndex;
}

void SphereBvh::RefitNode(int nodeIndex, const Sphere* spheres)
{
    Node& n = m_nodes[nodeIndex];
    if (n.count > 0)
    {
        int base = n.child0 * kLeafSize;
        float bminX = 0, bminY = 0, bminZ = 0, bmaxX = 0, bmaxY = 0, bmaxZ = 0;
        bool any = false;
        for (int lane = 0; lane < kLeafSize; ++lane)
        {
            int at = base + lane;
            if (m_leafValid[at] == 0.0f)
                continue;
            int id = m_leafIds[at];
            const Sphere& s = spheres[id];
            float cx = s.center.x, cy = s.center.y, cz = s.center.z;
            m_leafCenterX[at] = cx;
            m_leafCenterY[at] = cy;
            m_leafCenterZ[at] = cz;
            m_leafSqRadius[at] = s.radius * s.radius;
            m_leafInvRadius[at] = s.invRadius;
            float lox = cx - s.radius, loy = cy - s.radius, loz = cz - s.radius;
            float hix = cx + s.radius, hiy = cy + s.radius, hiz = cz + s.radius;
            if (!any)
            {
                bminX = lox; bminY = loy; bminZ = loz;
                bmaxX = hix; bmaxY = hiy; bmaxZ = hiz;
                any = true;
            }
            else
            {
                bminX = std::min(bminX, lox); bminY = std::min(bminY, loy); bminZ = std::min(bminZ, loz);
                bmaxX = std::max(bmaxX, hix); bmaxY = std::max(bmaxY, hiy); bmaxZ = std::max(bmaxZ, hiz);
            }
        }
        n.bmin = float3pack(bminX, bminY, bminZ);
        n.bmax = float3pack(bmaxX, bmaxY, bmaxZ);
        return;
    }
    RefitNode(n.child0, spheres);
    RefitNode(n.child1, spheres);
    n.bmin = float3pack(
        std::min(m_nodes[n.child0].bmin.x, m_nodes[n.child1].bmin.x),
        std::min(m_nodes[n.child0].bmin.y, m_nodes[n.child1].bmin.y),
        std::min(m_nodes[n.child0].bmin.z, m_nodes[n.child1].bmin.z));
    n.bmax = float3pack(
        std::max(m_nodes[n.child0].bmax.x, m_nodes[n.child1].bmax.x),
        std::max(m_nodes[n.child0].bmax.y, m_nodes[n.child1].bmax.y),
        std::max(m_nodes[n.child0].bmax.z, m_nodes[n.child1].bmax.z));
}

bool SphereBvh::BoxIntersects(const Node& n, const Ray& r, float tMin, float limitT)
{
    double t0 = tMin, t1 = limitT;
    double lo[3] = { n.bmin.x, n.bmin.y, n.bmin.z };
    double hi[3] = { n.bmax.x, n.bmax.y, n.bmax.z };
    double o[3] = { r.orig.getX(), r.orig.getY(), r.orig.getZ() };
    double d[3] = { r.dir.getX(), r.dir.getY(), r.dir.getZ() };
    for (int a = 0; a < 3; ++a)
    {
        double pad = BoundPad(lo[a], hi[a]);
        double blo = lo[a] - pad, bhi = hi[a] + pad;
        if (d[a] == 0.0)
        {
            if (o[a] < blo || o[a] > bhi)
                return false;
            continue;
        }
        double inv = 1.0 / d[a];
        double ta = (blo - o[a]) * inv;
        double tb = (bhi - o[a]) * inv;
        if (ta > tb)
        {
            double tmp = ta; ta = tb; tb = tmp;
        }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1)
            return false;
    }
    return t1 > tMin;
}

static double BoxEntryT(const float3pack& bmin, const float3pack& bmax, const Ray& r)
{
    double t0 = 0.0, t1 = 0.0;
    bool first = true;
    double lo[3] = { bmin.x, bmin.y, bmin.z };
    double hi[3] = { bmax.x, bmax.y, bmax.z };
    double o[3] = { r.orig.getX(), r.orig.getY(), r.orig.getZ() };
    double d[3] = { r.dir.getX(), r.dir.getY(), r.dir.getZ() };
    for (int a = 0; a < 3; ++a)
    {
        if (d[a] == 0.0)
            continue;
        double inv = 1.0 / d[a];
        double ta = (lo[a] - o[a]) * inv;
        double tb = (hi[a] - o[a]) * inv;
        if (ta > tb)
        {
            double tmp = ta; ta = tb; tb = tmp;
        }
        if (first)
        {
            t0 = ta; t1 = tb; first = false;
        }
        else
        {
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
        }
    }
    return first ? 0.0 : t0;
}

bool SphereBvh::TestLeaf(int leafIndex, const Ray& r, float tMin, Candidate& best, QueryStats* stats) const
{
    int base = leafIndex * kLeafSize;
    int laneCount = m_leafCounts[leafIndex];
    if (stats)
        stats->sphereTests += laneCount;

    float4 ox, oy, oz, dx, dy, dz;
    Splats(r, ox, oy, oz, dx, dy, dz);
    float4 sCenterX(m_leafCenterX.data() + base);
    float4 sCenterY(m_leafCenterY.data() + base);
    float4 sCenterZ(m_leafCenterZ.data() + base);
    float4 sSqRadius(m_leafSqRadius.data() + base);
    float4 coX = sCenterX - ox;
    float4 coY = sCenterY - oy;
    float4 coZ = sCenterZ - oz;
    float4 nb = coX * dx + coY * dy + coZ * dz;
    float4 c = coX * coX + coY * coY + coZ * coZ - sSqRadius;
    float4 discr = nb * nb - c;
    bool4 discrPos = discr > float4(0.0f);
    // Explicit validity mask for the (up to) three unused lanes. Only these
    // four flag values decide; no sentinel geometry is used.
    bool4 laneValid = float4(m_leafValid.data() + base) != float4(0.0f);
    if (!any(discrPos & laneValid))
        return false;

    float4 discrSq = sqrtf(discr);
    float4 t0 = nb - discrSq;
    float4 t1 = nb + discrSq;
    float4 tMin4(tMin);
    float4 t = select(t1, t0, t0 > tMin4);
    float4 bestT4(best.t);
    // Equality is kept in the candidate mask so that the scalar tie-break
    // below can choose the smallest original id. Values equal to the initial
    // tMax are rejected by that scalar check (the interval is open).
    bool4 inRange = (t > tMin4) & ((t < bestT4) | (t == bestT4));
    bool4 msk = discrPos & laneValid & inRange;
    unsigned bits = mask(msk);
    if (bits == 0)
        return false;

    for (int lane = 0; lane < kLeafSize; ++lane)
    {
        if ((bits & (1u << lane)) == 0)
            continue;
        int at = base + lane;
        float tv = (lane == 0) ? t.getX() : (lane == 1) ? t.getY() : (lane == 2) ? t.getZ() : t.getW();
        int id = m_leafIds[at];
        if (tv < best.t || (tv == best.t && id < best.id))
        {
            best.t = tv;
            best.id = id;
            best.centerX = m_leafCenterX[at];
            best.centerY = m_leafCenterY[at];
            best.centerZ = m_leafCenterZ[at];
            best.invRadius = m_leafInvRadius[at];
        }
    }
    return true;
}

int SphereBvh::ClosestHit(const Ray& r, float tMin, float tMax, Hit& outHit, QueryStats* outStats) const
{
    if (outStats)
    {
        outStats->nodesVisited = 0;
        outStats->sphereTests = 0;
    }
    if (m_root < 0)
        return -1;

    Candidate best;
    best.t = tMax;
    best.id = -1;
    best.centerX = best.centerY = best.centerZ = 0.0f;
    best.invRadius = 0.0f;

    int stack[128];
    int sp = 0;
    stack[sp++] = m_root;
    while (sp > 0)
    {
        int idx = stack[--sp];
        if (outStats)
            outStats->nodesVisited++;
        const Node& n = m_nodes[idx];
        if (!BoxIntersects(n, r, tMin, best.t))
            continue;
        if (n.count > 0)
        {
            TestLeaf(n.child0, r, tMin, best, outStats);
            continue;
        }
        // Visit the nearer child first; only affects performance, not results.
        float e0 = BoxEntryT(m_nodes[n.child0].bmin, m_nodes[n.child0].bmax, r);
        float e1 = BoxEntryT(m_nodes[n.child1].bmin, m_nodes[n.child1].bmax, r);
        if (e0 <= e1)
        {
            stack[sp++] = n.child1;
            stack[sp++] = n.child0;
        }
        else
        {
            stack[sp++] = n.child0;
            stack[sp++] = n.child1;
        }
    }

    if (best.id < 0)
        return -1;

    float3 pos = r.pointAt(best.t);
    outHit.pos = pos;
    outHit.normal = (pos - float3(best.centerX, best.centerY, best.centerZ)) * best.invRadius;
    outHit.t = best.t;
    return best.id;
}

bool SphereBvh::Occluded(const Ray& r, float tMin, float tMax, QueryStats* outStats) const
{
    if (outStats)
    {
        outStats->nodesVisited = 0;
        outStats->sphereTests = 0;
    }
    if (m_root < 0)
        return false;

    float4 ox, oy, oz, dx, dy, dz;
    Splats(r, ox, oy, oz, dx, dy, dz);
    float4 tMin4(tMin);
    float4 tMax4(tMax);

    int stack[128];
    int sp = 0;
    stack[sp++] = m_root;
    while (sp > 0)
    {
        int idx = stack[--sp];
        if (outStats)
            outStats->nodesVisited++;
        const Node& n = m_nodes[idx];
        if (!BoxIntersects(n, r, tMin, tMax))
            continue;
        if (n.count > 0)
        {
            int base = n.child0 * kLeafSize;
            int laneCount = m_leafCounts[n.child0];
            if (outStats)
                outStats->sphereTests += laneCount;
            float4 sCenterX(m_leafCenterX.data() + base);
            float4 sCenterY(m_leafCenterY.data() + base);
            float4 sCenterZ(m_leafCenterZ.data() + base);
            float4 sSqRadius(m_leafSqRadius.data() + base);
            float4 coX = sCenterX - ox;
            float4 coY = sCenterY - oy;
            float4 coZ = sCenterZ - oz;
            float4 nb = coX * dx + coY * dy + coZ * dz;
            float4 c = coX * coX + coY * coY + coZ * coZ - sSqRadius;
            float4 discr = nb * nb - c;
            bool4 discrPos = discr > float4(0.0f);
            bool4 laneValid = float4(m_leafValid.data() + base) != float4(0.0f);
            float4 discrSq = sqrtf(discr);
            float4 t0 = nb - discrSq;
            float4 t1 = nb + discrSq;
            float4 t = select(t1, t0, t0 > tMin4);
            if (any(discrPos & laneValid & (t > tMin4) & (t < tMax4)))
                return true;
            continue;
        }
        stack[sp++] = n.child0;
        stack[sp++] = n.child1;
    }
    return false;
}

int SphereBvh::GetLeafLaneCount(int leafIndex) const
{
    if (leafIndex < 0 || leafIndex >= (int)m_leafCounts.size())
        return 0;
    return m_leafCounts[leafIndex];
}

bool SphereBvh::IsLeafLaneValid(int leafIndex, int lane) const
{
    if (leafIndex < 0 || leafIndex >= (int)m_leafCounts.size() || lane < 0 || lane >= kLeafSize)
        return false;
    return m_leafValid[leafIndex * kLeafSize + lane] != 0.0f;
}

int SphereBvh::GetLeafPrimitive(int leafIndex, int lane) const
{
    if (leafIndex < 0 || leafIndex >= (int)m_leafCounts.size() || lane < 0 || lane >= kLeafSize)
        return -1;
    return m_leafIds[leafIndex * kLeafSize + lane];
}
