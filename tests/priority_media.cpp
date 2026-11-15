// Priority medium regression tests.
//
// Registered through task_support/check.h in three groups:
//   membership - active set, owner rules, tangency, crossing ordering
//   interfaces - independent Snell/Fresnel checks, reflection/TIR ownership
//   transport  - analytic RGB absorption, hidden boundary invariance, an
//                end-to-end two-eta trace into different emissive balls,
//                and a small PPM produced through DrawTest
//
// Every geometric reference below is an independent 2D cross-section solver
// rather than a call into the production medium helpers.

#include "task_support/check.h"
#include "Test.h"
#include "Medium.h"
#include "Maths.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <string>
#include <algorithm>

using namespace medium;

namespace {

struct Circle2
{
    float cx, cy, r;
    int id;
    int priority;
    float eta;
    float sigma[3];
};

struct Cross2
{
    float t;
    int id;
    bool enter;
};

// Independent 2D ray/circle solver; exact tangents (discriminant == 0) are
// deliberately excluded, matching the "tangency does not enter" rule.
std::vector<Cross2> Solve2D(float ox, float oy, float dx, float dy,
                            const std::vector<Circle2>& circles)
{
    std::vector<Cross2> out;
    for (const Circle2& c : circles)
    {
        float fx = ox - c.cx, fy = oy - c.cy;
        float b = fx * dx + fy * dy;
        float cc = fx * fx + fy * fy - c.r * c.r;
        float disc = b * b - cc;
        if (disc <= 0.0f)
            continue;
        float s = sqrtf(disc);
        float t0 = -b - s, t1 = -b + s;
        if (t0 > 0.0f) out.push_back({t0, c.id, true});
        if (t1 > 0.0f) out.push_back({t1, c.id, false});
    }
    std::sort(out.begin(), out.end(), [](const Cross2& a, const Cross2& b) { return a.t < b.t; });
    return out;
}

bool Inside2(const Circle2& c, float x, float y)
{
    float dx = x - c.cx, dy = y - c.cy;
    return dx * dx + dy * dy < c.r * c.r;
}

const Circle2& CircleById(const std::vector<Circle2>& ref, int id)
{
    return *std::find_if(ref.begin(), ref.end(),
                         [id](const Circle2& q) { return q.id == id; });
}

int RefOwner(const std::vector<int>& active, const std::vector<Circle2>& ref)
{
    int owner = -1;
    for (int id : active)
    {
        const Circle2& c = CircleById(ref, id);
        if (owner == -1 || c.priority > CircleById(ref, owner).priority ||
            (c.priority == CircleById(ref, owner).priority && id < owner))
            owner = id;
    }
    return owner;
}

// Independent unpolarised Fresnel, written from the s/p amplitude equations.
float RefFresnel(float cosI, float etaI, float etaT)
{
    cosI = std::fmin(1.0f, std::fmax(-1.0f, cosI));
    if (cosI < 0.0f) { cosI = -cosI; std::swap(etaI, etaT); }
    float sinT2 = (etaI / etaT) * (etaI / etaT) * (1.0f - cosI * cosI);
    if (sinT2 >= 1.0f) return 1.0f;
    float cosT = sqrtf(1.0f - sinT2);
    float rs = (etaI * cosI - etaT * cosT) / (etaI * cosI + etaT * cosT);
    float rp = (etaI * cosT - etaT * cosI) / (etaI * cosT + etaT * cosI);
    return 0.5f * (rs * rs + rp * rp);
}

// Independent vector Snell law; normal points against the incoming ray.
bool RefSnell(const float3& d, const float3& n, float etaI, float etaT, float3& outT)
{
    float eta = etaI / etaT;
    float cosI = -dot(d, n);
    float sinT2 = eta * eta * (1.0f - cosI * cosI);
    if (sinT2 >= 1.0f) return false;
    float cosT = sqrtf(1.0f - sinT2);
    outT = normalize(eta * d + (eta * cosI - cosT) * n);
    return true;
}

struct SceneBuilder
{
    std::vector<Sphere> spheres;
    std::vector<SceneMaterialDesc> mats;
    MediumConfig cfg;

    int Add(float x, float y, float z, float r, int type, int priority,
            float eta, const float3& sigma,
            const float3& albedo = float3(0, 0, 0),
            const float3& emissive = float3(0, 0, 0), float roughness = 0.0f)
    {
        int id = (int)spheres.size();
        spheres.push_back(Sphere(float3(x, y, z), r));
        mats.push_back({type, float3pack(albedo), float3pack(emissive), roughness, eta});
        MediumSphere m;
        m.priority = priority;
        m.eta = eta;
        m.sigma = float3pack(sigma);
        cfg.spheres.push_back(m);
        return id;
    }
    bool Install() { return InstallScene(spheres, mats, cfg); }
};

struct ProductionGuard
{
    ProductionGuard() { InitializeTest(); }
    ~ProductionGuard()
    {
        ResetToDefaultScene();
        ShutdownTest();
    }
};

void PrintVec(const char* tag, const float3& v)
{
    std::printf("  %s=(%.6f, %.6f, %.6f)\n", tag, v.getX(), v.getY(), v.getZ());
}

float MaxDiff(const float3& a, const float3& b)
{
    return std::fmax(std::fmax(std::fabs(a.getX() - b.getX()),
                               std::fabs(a.getY() - b.getY())),
                     std::fabs(a.getZ() - b.getZ()));
}

// Walks the production NextSegment helper over a whole ray, collecting the
// sequence of real (owner-changing or opaque) surface events.
struct Event
{
    int id;
    int ownerBefore;
    int ownerAfter;
    bool entering;
    float distance;
};
std::vector<Event> WalkProduction(const Ray& ray)
{
    std::vector<Event> events;
    Ray r = ray;
    std::vector<int> active = InitialActive(r);
    for (;;)
    {
        SegmentResult seg;
        int hidden = 0;
        bool hit = NextSegment(r, active, 0.001f, 1.0e7f, seg, hidden);
        if (!hit)
            break;
        events.push_back({seg.hitID, seg.ownerBefore, seg.ownerAfter, seg.entering, seg.distance});
        active = seg.activeAtSurface;
        r = Ray(seg.hit.pos, ray.dir); // geometry-only continuation for membership checks
        if (events.size() > 64) break;
    }
    return events;
}

} // namespace

// ===========================================================================
// membership
// ===========================================================================

static int MembershipOverlapAndOrder()
{
    ProductionGuard guard;
    // Two overlapping dielectric spheres (A id 0 low priority, B id 1 high
    // priority) plus a contained sphere and an opaque one, all in the z=0 plane.
    SceneBuilder b;
    int idA = b.Add(0.0f, 0.0f, 0.0f, 2.0f, 2, 1, 1.5f, float3(0, 0, 0));
    int idB = b.Add(1.5f, 0.0f, 0.0f, 2.0f, 2, 2, 1.2f, float3(0, 0, 0));
    int idC = b.Add(0.3f, 0.3f, 0.0f, 0.4f, 2, 3, 1.1f, float3(0, 0, 0));
    int idO = b.Add(-3.0f, 0.0f, 0.0f, 0.5f, 0, 0, 1.0f, float3(0, 0, 0), float3(0.8f, 0.8f, 0.8f));
    TASK_CHECK(b.Install());

    std::vector<Circle2> ref =
    {
        {0.0f, 0.0f, 2.0f, idA, 1, 1.5f, {0, 0, 0}},
        {1.5f, 0.0f, 2.0f, idB, 2, 1.2f, {0, 0, 0}},
        {0.3f, 0.3f, 0.4f, idC, 3, 1.1f, {0, 0, 0}},
        {-3.0f, 0.0f, 0.5f, idO, 0, 1.0f, {0, 0, 0}},
    };

    // Ray crossing the full lens: outside -> A -> A+B -> B -> outside, i.e.
    // A in, B in, A out, B out must appear in exactly that order.
    const float ox = -2.6f, oy = 0.2f, dx = 1.0f, dy = 0.0f;
    std::vector<Cross2> refCross = Solve2D(ox, oy, dx, dy, ref);
    Ray ray(float3(ox, oy, 0), float3(dx, dy, 0));
    std::vector<Crossing> prod = EnumerateCrossings(ray, 0.001f, 1.0e7f);
    TASK_CHECK(prod.size() == refCross.size());
    for (size_t i = 0; i < refCross.size(); ++i)
    {
        std::printf("membership crossing[%zu]: prod t=%.6f id=%d enter=%d | ref t=%.6f id=%d enter=%d | err=%.2e\n",
                    i, prod[i].t, prod[i].id, (int)prod[i].entering,
                    refCross[i].t, refCross[i].id, (int)refCross[i].enter,
                    std::fabs(prod[i].t - refCross[i].t));
        TASK_CHECK(prod[i].id == refCross[i].id);
        TASK_CHECK(prod[i].entering == refCross[i].enter);
        TASK_CHECK(std::fabs(prod[i].t - refCross[i].t) < 5.0e-5f);
    }
    // Restricted to the two overlapping spheres: A in, B in, A out, B out.
    std::vector<Cross2> lens;
    for (const Cross2& c : refCross)
        if (c.id == idA || c.id == idB)
            lens.push_back(c);
    TASK_CHECK(lens.size() == 4);
    TASK_CHECK(lens[0].id == idA && lens[0].enter);
    TASK_CHECK(lens[1].id == idB && lens[1].enter);
    TASK_CHECK(lens[2].id == idA && !lens[2].enter);
    TASK_CHECK(lens[3].id == idB && !lens[3].enter);
    // The opaque sphere appears first because the ray starts inside it.
    TASK_CHECK(refCross.front().id == idO && !refCross.front().enter);

    // Active set from an interior start, including camera-inside-several case.
    const float probes[][2] = {{0.5f, 0.2f}, {1.6f, 0.1f}, {0.3f, 0.35f}, {-2.7f, 0.0f}};
    for (auto& p : probes)
    {
        Ray pr(float3(p[0], p[1], 0), float3(1, 0, 0));
        std::vector<int> active = InitialActive(pr);
        std::vector<int> expected;
        for (const Circle2& c : ref)
            if (c.id != idO && Inside2(c, p[0], p[1]))
                expected.push_back(c.id);
        std::sort(active.begin(), active.end());
        std::sort(expected.begin(), expected.end());
        std::printf("membership probe(%.2f,%.2f): active=%zu expected=%zu\n",
                    p[0], p[1], active.size(), expected.size());
        TASK_CHECK(active == expected);
        TASK_CHECK(OwnerOf(active) == RefOwner(active, ref));
    }

    // A point exactly on a surface is not inside; an exact tangent does not cross.
    float3 onSurface(2.0f, 0.0f, 0.0f);
    TASK_CHECK(!PointInside(idA, onSurface));
    Ray tangent(float3(0.0f, 2.0f, 0.0f), float3(1, 0, 0)); // tangent to A at (0,2,0)
    TASK_CHECK(EnumerateCrossings(tangent, 0.001f, 1.0e7f).empty());
    return 0;
}

static int MembershipTieAndHidden()
{
    ProductionGuard guard;
    SceneBuilder b;
    int id0 = b.Add(0.0f, 0.0f, 0.0f, 1.5f, 2, 5, 1.3f, float3(0, 0, 0)); // equal priority
    int id1 = b.Add(1.0f, 0.0f, 0.0f, 1.5f, 2, 5, 1.4f, float3(0, 0, 0)); // equal priority
    TASK_CHECK(b.Install());

    std::vector<int> both = {id0, id1};
    TASK_CHECK(OwnerOf(both) == id0); // tie -> smaller original id
    std::vector<int> second = {id1};
    TASK_CHECK(OwnerOf(second) == id1);

    // Ray through the overlap: entering id1 while id0 stays owner is hidden.
    Ray ray(float3(-1.4f, 0.0f, 0), float3(1, 0, 0));
    std::vector<Event> events = WalkProduction(ray);
    int hiddenTotal = 0;
    SegmentResult seg;
    NextSegment(ray, InitialActive(ray), 0.001f, 1.0e7f, seg, hiddenTotal);
    std::printf("tie first real segment: id=%d owner %d->%d hidden=%d\n",
                seg.hitID, seg.ownerBefore, seg.ownerAfter, hiddenTotal);
    TASK_CHECK(hiddenTotal == 1);
    TASK_CHECK(seg.ownerBefore == id0);
    TASK_CHECK(seg.ownerAfter == id1); // leaving id0, only id1 remains
    TASK_CHECK(events.size() >= 1);
    return 0;
}

// ===========================================================================
// interfaces
// ===========================================================================

static int InterfaceFresnelSnell()
{
    ProductionGuard guard;
    SceneBuilder b;
    b.Add(0, 0, 0, 1.0f, 2, 1, 1.5f, float3(0, 0, 0));
    TASK_CHECK(b.Install());

    const float etas[][2] = {{1.0f, 1.0f}, {1.0f, 1.5f}, {1.5f, 1.0f}, {1.33f, 1.5f}};
    for (auto& pair : etas)
    {
        for (int deg = 0; deg <= 89; deg += 7)
        {
            float ang = deg * kPI / 180.0f;
            float cosI = cosf(ang);
            float got = Fresnel(cosI, pair[0], pair[1]);
            float ref = RefFresnel(cosI, pair[0], pair[1]);
            if (deg == 0 || std::fabs(got - ref) > 1.0e-5f)
                std::printf("fresnel ei=%.2f et=%.2f deg=%d: got=%.6f ref=%.6f err=%.2e\n",
                            pair[0], pair[1], deg, got, ref, std::fabs(got - ref));
            TASK_CHECK(std::fabs(got - ref) < 2.0e-5f);
        }
    }
    // Equal eta on both sides: Fresnel is exactly zero.
    TASK_CHECK(Fresnel(0.7f, 1.3f, 1.3f) == 0.0f);
    // Grazing incidence approaches one (at cosI=1e-3 it is ~0.994).
    TASK_CHECK(Fresnel(0.001f, 1.0f, 1.5f) > 0.99f);
    TASK_CHECK(Fresnel(1.0e-5f, 1.0f, 1.5f) > 0.9999f);

    // Snell directions against the independent formula, including equal eta.
    float3 n(0, 0, 1);
    for (int deg = 5; deg <= 80; deg += 15)
    {
        float ang = deg * kPI / 180.0f;
        float3 d(sinf(ang), 0, -cosf(ang));
        float3 got, ref;
        bool okGot = RefractDir(d, n, 1.0f, 1.5f, got);
        bool okRef = RefSnell(d, n, 1.0f, 1.5f, ref);
        TASK_CHECK(okGot && okRef);
        std::printf("snell deg=%d err=%.2e\n", deg, MaxDiff(got, ref));
        TASK_CHECK(MaxDiff(got, ref) < 2.0e-5f);
    }
    float3 dEq(0.5f, 0, -0.8660254f);
    float3 tEq;
    TASK_CHECK(RefractDir(dEq, n, 1.2f, 1.2f, tEq));
    TASK_CHECK(MaxDiff(tEq, normalize(dEq)) < 2.0e-6f);

    // Total internal reflection: dense -> rare beyond the critical angle.
    float3 dTir(0.9f, 0, -0.4358899f); // ~64 degrees, beyond arcsin(1/1.5)=41.8
    float3 tTir;
    TASK_CHECK(!RefractDir(dTir, n, 1.5f, 1.0f, tTir));
    TASK_CHECK(Fresnel(-dot(dTir, n), 1.5f, 1.0f) == 1.0f);
    return 0;
}

static int InterfaceReflectionOwnership()
{
    ProductionGuard guard;
    // B (id1, high priority, center (1.5,0)) overlaps A (id0, center origin).
    // The ray starts inside both and reaches B's exit while still inside A,
    // so the first real interface is owner B -> owner A.
    SceneBuilder b;
    int idA = b.Add(0.0f, 0.0f, 0.0f, 2.0f, 2, 1, 1.0f, float3(0, 0, 0));
    int idB = b.Add(1.5f, 0.0f, 0.0f, 2.0f, 2, 2, 1.5f, float3(0, 0, 0));
    TASK_CHECK(b.Install());

    Ray ray(float3(-0.3f, 0.3f, 0), normalize(float3(0.3f, -1.0f, 0)));
    std::vector<int> active0 = InitialActive(ray);
    TASK_CHECK(std::find(active0.begin(), active0.end(), idA) != active0.end());
    TASK_CHECK(std::find(active0.begin(), active0.end(), idB) != active0.end());

    int hidden = 0;
    SegmentResult seg;
    TASK_CHECK(NextSegment(ray, active0, 0.001f, 1.0e7f, seg, hidden));
    TASK_CHECK(seg.hitID == idB && seg.ownerBefore == idB && seg.ownerAfter == idA);
    PrintVec("interface normalAgainst", seg.normalAgainst);

    // Total internal reflection path: refraction must fail and the reflected
    // ray must keep the full pre-crossing membership (owner stays B).
    float3 refr;
    TASK_CHECK(!RefractDir(ray.dir, seg.normalAgainst, seg.etaBefore, seg.etaAfter, refr));
    float3 refl = normalize(reflect(ray.dir, seg.normalAgainst));
    // Nudge off the surface exactly like the production kMinT self-hit guard.
    Ray reflected(seg.hit.pos + refl * 0.002f, refl);
    std::vector<int> afterRefl = InitialActive(reflected);
    std::printf("TIR reflected point owner: %d (expected %d)\n", OwnerOf(afterRefl), idB);
    TASK_CHECK(OwnerOf(afterRefl) == idB);
    TASK_CHECK(afterRefl == active0);

    // Ordinary Fresnel reflection at a refract-able interface also preserves
    // the set: swap the etas so the B -> A crossing goes rare -> dense, find a
    // fixed seed whose single draw reflects, and verify the owner never leaves B.
    SceneBuilder b2;
    b2.Add(0.0f, 0.0f, 0.0f, 2.0f, 2, 1, 1.5f, float3(0, 0, 0));
    b2.Add(1.5f, 0.0f, 0.0f, 2.0f, 2, 2, 1.0f, float3(0, 0, 0));
    TASK_CHECK(b2.Install());
    SegmentResult seg2;
    int hidden2 = 0;
    TASK_CHECK(NextSegment(ray, InitialActive(ray), 0.001f, 1.0e7f, seg2, hidden2));
    float fresnel = Fresnel(-dot(ray.dir, seg2.normalAgainst), seg2.etaBefore, seg2.etaAfter);
    uint32_t reflSeed = 0;
    for (uint32_t cand = 1; cand < 10000 && reflSeed == 0; ++cand)
    {
        uint32_t probeSeed = cand * 2654435761u | 1u;
        if (RandomFloat01(probeSeed) < fresnel)
            reflSeed = cand * 2654435761u | 1u; // fresh copy; the draw above is a probe only
    }
    TASK_CHECK(reflSeed != 0);
    int rays = 0;
    std::vector<int> ownerPath;
    TraceDebugStats stats;
    float3 radiance = TraceMedium(ray, 0, rays, reflSeed, &stats, &ownerPath);
    PrintVec("reflection trace radiance", radiance);
    TASK_CHECK(!ownerPath.empty());
    TASK_CHECK(ownerPath.front() == idB);
    TASK_CHECK(stats.scatterEvents >= 1);

    // Equal eta on both sides: Fresnel is zero so every stream transmits with
    // an unchanged direction, yet the identity (and therefore later
    // absorption) must still switch from B to A at the boundary.
    SceneBuilder b3;
    int idA3 = b3.Add(0.0f, 0.0f, 0.0f, 2.0f, 2, 1, 1.3f, float3(0.4f, 0.2f, 0.1f));
    int idB3 = b3.Add(1.5f, 0.0f, 0.0f, 2.0f, 2, 2, 1.3f, float3(0.05f, 0.5f, 0.05f));
    b3.cfg.sky = float3pack(0.2f, 0.2f, 0.2f);
    TASK_CHECK(b3.Install());
    SegmentResult seg3;
    int hidden3 = 0;
    TASK_CHECK(NextSegment(ray, InitialActive(ray), 0.001f, 1.0e7f, seg3, hidden3));
    TASK_CHECK(seg3.ownerBefore == idB3 && seg3.ownerAfter == idA3);
    TASK_CHECK(seg3.etaBefore == seg3.etaAfter);
    TASK_CHECK(Fresnel(-dot(ray.dir, seg3.normalAgainst), seg3.etaBefore, seg3.etaAfter) == 0.0f);
    std::vector<int> eqPath;
    uint32_t eqSeed = 555;
    int eqRays = 0;
    float3 eqRadiance = TraceMedium(ray, 0, eqRays, eqSeed, nullptr, &eqPath);
    std::printf("equal-eta owner path:");
    for (int id : eqPath) std::printf(" %d", id);
    std::printf("\n");
    TASK_CHECK(eqPath.size() >= 2 && eqPath[0] == idB3 && eqPath[1] == idA3);
    PrintVec("equal-eta radiance (finite, owner switches B->A)", eqRadiance);
    TASK_CHECK(std::isfinite(eqRadiance.getX()) && std::isfinite(eqRadiance.getY()) &&
               std::isfinite(eqRadiance.getZ()));

    // Per-segment transmittance against an independent 2D piecewise integral.
    // The second segment lies entirely inside A after the handoff, so its
    // sigma must be A's even though both media share eta 1.3.
    std::vector<Circle2> eqRef =
    {
        {0.0f, 0.0f, 2.0f, idA3, 1, 1.3f, {0.4f, 0.2f, 0.1f}},
        {1.5f, 0.0f, 2.0f, idB3, 2, 1.3f, {0.05f, 0.5f, 0.05f}},
    };
    Ray walk = ray;
    std::vector<int> walkActive = InitialActive(walk);
    int segIndex = 0;
    for (;;)
    {
        SegmentResult seg;
        int step = 0;
        bool hit = NextSegment(walk, walkActive, 0.001f, 1.0e7f, seg, step);
        if (!hit) break;
        std::vector<Cross2> cs = Solve2D(walk.orig.getX(), walk.orig.getY(),
                                         walk.dir.getX(), walk.dir.getY(), eqRef);
        std::vector<int> refActive = walkActive;
        float refLast = 0.0f, tau[3] = {0, 0, 0};
        for (const Cross2& c : cs)
        {
            if (c.t > seg.hit.t) break;
            int owner = RefOwner(refActive, eqRef);
            if (owner >= 0)
                for (int k = 0; k < 3; ++k)
                    tau[k] += CircleById(eqRef, owner).sigma[k] * (c.t - refLast);
            if (c.enter) refActive.push_back(c.id);
            else refActive.erase(std::remove(refActive.begin(), refActive.end(), c.id), refActive.end());
            refLast = c.t;
        }
        float3 refTrans(expf(-tau[0]), expf(-tau[1]), expf(-tau[2]));
        std::printf("equal-eta segment %d owner=%d trans err=%.2e\n",
                    segIndex, seg.ownerBefore, MaxDiff(seg.transmittance, refTrans));
        TASK_CHECK(MaxDiff(seg.transmittance, refTrans) < 3.0e-5f);
        if (segIndex == 1)
            TASK_CHECK(seg.ownerBefore == idA3); // absorption here is A's, not B's
        walkActive = seg.activeAtSurface;
        walk = Ray(seg.hit.pos, ray.dir);
        if (++segIndex > 8) break;
    }
    TASK_CHECK(segIndex >= 2);
    return 0;
}

// ===========================================================================
// transport
// ===========================================================================

static int TransportAnalyticAbsorption()
{
    ProductionGuard guard;
    // Two eta=1 overlapping spheres with distinct RGB sigma and priorities.
    SceneBuilder b;
    int idA = b.Add(0.0f, 0.0f, 0.0f, 2.0f, 2, 1, 1.0f, float3(0.20f, 0.40f, 0.60f));
    int idB = b.Add(1.5f, 0.0f, 0.0f, 2.0f, 2, 2, 1.0f, float3(0.80f, 0.10f, 0.05f));
    b.cfg.sky = float3pack(0.15f, 0.21f, 0.3f);
    TASK_CHECK(b.Install());

    const float ox = -2.6f, oy = 0.25f;
    std::vector<Circle2> ref =
    {
        {0.0f, 0.0f, 2.0f, idA, 1, 1.0f, {0.20f, 0.40f, 0.60f}},
        {1.5f, 0.0f, 2.0f, idB, 2, 1.0f, {0.80f, 0.10f, 0.05f}},
    };
    std::vector<Cross2> crosses = Solve2D(ox, oy, 1.0f, 0.0f, ref);

    // Analytical piecewise Beer-Lambert integral using the 2D owner rules.
    std::vector<int> active;
    float last = 0.0f;
    float tau[3] = {0, 0, 0};
    for (const Cross2& c : crosses)
    {
        int owner = RefOwner(active, ref);
        float len = c.t - last;
        if (owner >= 0)
            for (int k = 0; k < 3; ++k)
                tau[k] += ref[owner].sigma[k] * len;
        if (c.enter) active.push_back(c.id);
        else active.erase(std::remove(active.begin(), active.end(), c.id), active.end());
        last = c.t;
    }
    float3 expected(b.cfg.sky.x * expf(-tau[0]), b.cfg.sky.y * expf(-tau[1]), b.cfg.sky.z * expf(-tau[2]));

    uint32_t state = 991;
    int rayCount = 0;
    Ray ray(float3(ox, oy, 0), float3(1, 0, 0));
    float3 got = TraceMedium(ray, 0, rayCount, state);
    float err = MaxDiff(got, expected);
    PrintVec("absorption got", got);
    PrintVec("absorption ref", expected);
    std::printf("absorption max err=%.2e rayCount=%d\n", err, rayCount);
    TASK_CHECK(err < 3.0e-4f);

    // Cumulative path length: production crossing times must agree with the
    // 2D reference to within the required 5e-4 world-unit budget.
    std::vector<Crossing> prod = EnumerateCrossings(ray, 0.001f, 1.0e7f);
    TASK_CHECK(prod.size() == crosses.size());
    float lengthErr = 0.0f;
    for (size_t i = 0; i < crosses.size(); ++i)
        lengthErr = std::fmax(lengthErr, std::fabs(prod[i].t - crosses[i].t));
    std::printf("path length cumulative error=%.2e (budget 5e-4)\n", lengthErr);
    TASK_CHECK(lengthErr < 5.0e-4f);
    return 0;
}

static int TransportHiddenInvariance()
{
    ProductionGuard guard;
    // Baseline scene: one high-priority absorbing sphere.
    SceneBuilder base;
    int idHi = base.Add(0.0f, 0.0f, 0.0f, 1.5f, 2, 2, 1.2f, float3(0.3f, 0.2f, 0.1f));
    base.cfg.sky = float3pack(0.2f, 0.2f, 0.2f);
    TASK_CHECK(base.Install());
    Ray ray(float3(-3.0f, 0.2f, 0), normalize(float3(1, -0.15f, 0)));

    uint32_t s1 = 777;
    int rc1 = 0;
    TraceDebugStats st1;
    float3 r1 = TraceMedium(ray, 0, rc1, s1, &st1, nullptr);

    // Add three low-priority dielectric spheres that are fully contained in
    // the high-priority one and straddle the ray: every one of their
    // boundaries must therefore be occluded (the high sphere always owns it).
    SceneBuilder extra;
    extra.Add(0.0f, 0.0f, 0.0f, 1.5f, 2, 2, 1.2f, float3(0.3f, 0.2f, 0.1f));
    // Ray slope is -0.15: y(x) = 0.2 - 0.15*(x + 3); centers lie on the ray,
    // and center distance + radius < 1.5 so each sphere stays inside id 0.
    extra.Add(-1.0f, -0.10f, 0.0f, 0.18f, 2, 1, 1.1f, float3(0.9f, 0.9f, 0.9f));
    extra.Add(-0.2f, -0.22f, 0.0f, 0.18f, 2, 1, 1.15f, float3(0.7f, 0.7f, 0.7f));
    extra.Add(0.6f, -0.34f, 0.0f, 0.18f, 2, 1, 1.25f, float3(0.5f, 0.5f, 0.5f));
    extra.cfg.sky = float3pack(0.2f, 0.2f, 0.2f);
    TASK_CHECK(extra.Install());

    // Sanity: the extra spheres are genuinely intersected (six extra
    // crossings) and every one of them stays hidden across the whole walk.
    std::vector<Crossing> crossings = EnumerateCrossings(ray, 0.001f, 1.0e7f);
    TASK_CHECK(crossings.size() >= 8);
    int hiddenWalk = 0, realEvents = 0;
    {
        Ray walk = ray;
        std::vector<int> active = InitialActive(walk);
        for (;;)
        {
            SegmentResult seg;
            int step = 0;
            bool hit = NextSegment(walk, active, 0.001f, 1.0e7f, seg, step);
            hiddenWalk += step;
            if (!hit) break;
            ++realEvents;
            active = seg.activeAtSurface;
            walk = Ray(seg.hit.pos, ray.dir); // advance past the real event
            if (realEvents > 64) break;
        }
    }
    std::printf("hidden walk: realEvents=%d hidden=%d\n", realEvents, hiddenWalk);
    TASK_CHECK(hiddenWalk >= 6);
    TASK_CHECK(realEvents == 2); // only entry and exit of the owning sphere

    uint32_t s2 = 777;
    int rc2 = 0;
    TraceDebugStats st2;
    float3 r2 = TraceMedium(ray, 0, rc2, s2, &st2, nullptr);
    std::printf("hidden invariance: rng %u->%u vs %u->%u, scatters %u vs %u, hidden=%u\n",
                777u, s1, 777u, s2, st1.scatterEvents, st2.scatterEvents, st2.hiddenBoundaries);
    PrintVec("radiance without occluders", r1);
    PrintVec("radiance with occluders", r2);
    TASK_CHECK(s1 == s2);                       // random stream untouched
    TASK_CHECK(st1.scatterEvents == st2.scatterEvents); // depth budget untouched
    TASK_CHECK(MaxDiff(r1, r2) < 1.0e-6f);      // and radiance identical
    TASK_CHECK(st2.hiddenBoundaries > st1.hiddenBoundaries);
    TASK_CHECK(idHi == 0);
    return 0;
}

static int TransportEtaDivergence()
{
    ProductionGuard guard;

    // Shared geometry: B (id1) owns the start region and its exit point is
    // still inside A (id0). Emissive ball positions are derived from the
    // independent reflection and Snell directions before either scene is run.
    auto buildScene = [](float etaA, float etaB, const float3& e1Pos, const float3& e2Pos) {
        SceneBuilder b;
        b.Add(0.0f, 0.0f, 0.0f, 2.0f, 2, 1, etaA, float3(0.02f, 0.03f, 0.04f)); // id0 A, origin
        b.Add(1.5f, 0.0f, 0.0f, 2.0f, 2, 2, etaB, float3(0.05f, 0.02f, 0.01f)); // id1 B, (1.5,0)
        b.Add(e1Pos.getX(), e1Pos.getY(), e1Pos.getZ(), 0.08f, 0, 0, 1.0f, float3(0, 0, 0),
              float3(0, 0, 0), float3(12.0f, 0.0f, 0.0f)); // red ball, zero albedo
        b.Add(e2Pos.getX(), e2Pos.getY(), e2Pos.getZ(), 0.08f, 0, 0, 1.0f, float3(0, 0, 0),
              float3(0, 0, 0), float3(0.0f, 0.0f, 12.0f)); // blue ball, zero albedo
        b.cfg.sky = float3pack(0.1f, 0.1f, 0.1f);
        return b;
    };

    Ray ray(float3(-0.3f, 0.3f, 0), normalize(float3(0.3f, -1.0f, 0)));

    // Use the TIR configuration only to derive geometry (etaB 1.5 -> etaA 1.0).
    SceneBuilder probe;
    probe.Add(0.0f, 0.0f, 0.0f, 2.0f, 2, 1, 1.0f, float3(0, 0, 0));
    probe.Add(1.5f, 0.0f, 0.0f, 2.0f, 2, 2, 1.5f, float3(0, 0, 0));
    TASK_CHECK(probe.Install());
    SegmentResult seg;
    int hidden = 0;
    TASK_CHECK(NextSegment(ray, InitialActive(ray), 0.001f, 1.0e7f, seg, hidden));
    TASK_CHECK(seg.ownerBefore == 1 && seg.ownerAfter == 0);
    float3 refr;
    TASK_CHECK(!RefractDir(ray.dir, seg.normalAgainst, 1.5f, 1.0f, refr)); // TIR confirmed
    float3 reflDir = normalize(reflect(ray.dir, seg.normalAgainst));
    float3 refrDir;
    TASK_CHECK(RefSnell(ray.dir, seg.normalAgainst, 1.0f, 1.5f, refrDir)); // scene 2 bends into A

    const float ballOffset = 0.3f;
    float3 redPos = seg.hit.pos + reflDir * ballOffset;
    float3 bluePos = seg.hit.pos + refrDir * ballOffset;
    // Independent placement checks: reflected target stays inside B,
    // refracted target leaves B but remains inside A.
    TASK_CHECK(PointInside(1, redPos));
    TASK_CHECK(!PointInside(1, bluePos));
    TASK_CHECK(PointInside(0, bluePos));
    std::printf("red ball pos=(%.4f,%.4f,%.4f) blue ball pos=(%.4f,%.4f,%.4f)\n",
                redPos.getX(), redPos.getY(), redPos.getZ(),
                bluePos.getX(), bluePos.getY(), bluePos.getZ());

    // Scene 1: TIR at B -> reflected ray must reach the red ball.
    SceneBuilder s1 = buildScene(1.0f, 1.5f, redPos, bluePos);
    TASK_CHECK(s1.Install());
    uint32_t seed1 = 4242;
    int rc1 = 0;
    std::vector<int> path1;
    TraceDebugStats stats1;
    float3 c1 = TraceMedium(ray, 0, rc1, seed1, &stats1, &path1);
    PrintVec("TIR scene radiance", c1);
    std::printf("TIR owner path:");
    for (int id : path1) std::printf(" %d", id);
    std::printf("\n");
    TASK_CHECK(path1.size() >= 2 && path1[0] == 1 && path1[1] == 1); // identity stays B
    TASK_CHECK(c1.getX() > 3.0f * std::fmax(c1.getY(), c1.getZ()));

    // Scene 2: swapped etas, same geometry -> transmission into A and the
    // refracted ray reaches the blue ball. Choose a fixed seed whose single
    // interface draw is above the (small) Fresnel probability.
    SceneBuilder s2 = buildScene(1.5f, 1.0f, redPos, bluePos);
    TASK_CHECK(s2.Install());
    SegmentResult seg2;
    int hidden2 = 0;
    TASK_CHECK(NextSegment(ray, InitialActive(ray), 0.001f, 1.0e7f, seg2, hidden2));
    float fresnel = Fresnel(-dot(ray.dir, seg2.normalAgainst), seg2.etaBefore, seg2.etaAfter);
    uint32_t chosen = 0;
    for (uint32_t cand = 1; cand < 1000 && chosen == 0; ++cand)
    {
        uint32_t tmp = cand * 2654435761u | 1u;
        if (RandomFloat01(tmp) > fresnel) chosen = cand * 2654435761u | 1u;
    }
    TASK_CHECK(chosen != 0);
    int rc2 = 0;
    std::vector<int> path2;
    TraceDebugStats stats2;
    float3 c2 = TraceMedium(ray, 0, rc2, chosen, &stats2, &path2);
    PrintVec("transmission scene radiance", c2);
    std::printf("transmission owner path:");
    for (int id : path2) std::printf(" %d", id);
    std::printf("\n");
    TASK_CHECK(path2.size() >= 2 && path2[0] == 1 && path2[1] == 0); // identity switches B -> A
    TASK_CHECK(c2.getZ() > 3.0f * std::fmax(c2.getX(), c2.getY()));
    TASK_CHECK(c1.getX() > c2.getX() && c2.getZ() > c1.getZ());
    return 0;
}

static int TransportDrawTestPpm()
{
    ProductionGuard guard;
    TASK_CHECK(LoadSmallBallScene());
    const int w = 24, h = 16;
    int frameCount = 0;
    UpdateTest(0, frameCount, w, h, 0);
    std::vector<float> buf(w * h * 4, 0.0f);
    int rays = 0;
    DrawTest(0, frameCount, w, h, buf.data(), rays, 0);
    TASK_CHECK(rays > w * h);

    const char* path = "priority_media.ppm";
    FILE* f = std::fopen(path, "wb");
    TASK_CHECK(f != nullptr);
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i)
    {
        unsigned char rgb[3];
        for (int c = 0; c < 3; ++c)
        {
            float v = buf[i * 4 + c];
            v = std::fmin(1.0f, std::fmax(0.0f, sqrtf(v)));
            rgb[c] = (unsigned char)(v * 255.0f + 0.5f);
        }
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("wrote %s (%dx%d) rays=%d\n", path, w, h, rays);
    return 0;
}

static int TransportDirectLight()
{
    ProductionGuard guard;

    // Everything sits inside one eta=1 dielectric so refraction never bends
    // the path; the only medium effect is absorption of the direct light ray.
    auto build = [](bool withBlocker) {
        SceneBuilder b;
        b.Add(0.0f, 0.0f, 0.0f, 2.5f, 2, 1, 1.0f, float3(0.30f, 0.20f, 0.10f)); // id0 medium
        b.Add(-1.2f, 0.0f, 0.0f, 0.10f, 0, 0, 1.0f, float3(0, 0, 0),
              float3(0.9f, 0.9f, 0.9f));                                       // id1 Lambert
        if (withBlocker)
            b.Add(-0.2f, 0.0f, 0.0f, 0.15f, 2, 2, 1.2f, float3(0, 0, 0));       // id2 glass blocker
        b.Add(0.8f, 0.0f, 0.0f, 0.08f, 0, 0, 1.0f, float3(0, 0, 0),
              float3(0, 0, 0), float3(20.0f, 20.0f, 20.0f));                   // emissive ball
        b.cfg.sky = float3pack(0.05f, 0.05f, 0.05f);
        return b;
    };

    SceneBuilder open = build(false);
    TASK_CHECK(open.Install());

    // (a) Finite unoccluded light segment: ShadowAttenuation must match the
    // independent 2D piecewise Beer-Lambert integral.
    float3 from(-1.1f, 0.0f, 0.0f); // on the light-facing Lambert surface
    float3 to(0.8f, 0.0f, 0.0f);
    float3 dir = normalize(to - from);
    float tEnd = length(to - from);
    float3 got = ShadowAttenuation(from, dir, tEnd, InitialActive(Ray(from, dir)));
    std::vector<Circle2> ref = {{0.0f, 0.0f, 2.0f, 0, 1, 1.0f, {0.30f, 0.20f, 0.10f}}};
    std::vector<Cross2> cs = Solve2D(from.getX(), from.getY(), dir.getX(), dir.getY(), ref);
    float last = 0.0f, tau[3] = {0, 0, 0};
    for (const Cross2& c : cs)
    {
        if (c.t > tEnd) break;
        for (int k = 0; k < 3; ++k) tau[k] += ref[0].sigma[k] * (c.t - last);
        last = c.t;
    }
    for (int k = 0; k < 3; ++k) tau[k] += ref[0].sigma[k] * (tEnd - last);
    float3 expected(expf(-tau[0]), expf(-tau[1]), expf(-tau[2]));
    PrintVec("light segment attenuation got", got);
    PrintVec("light segment attenuation ref", expected);
    TASK_CHECK(MaxDiff(got, expected) < 3.0e-6f);

    // (b) A glass sphere between the Lambert surface and the light must keep
    // occluding the shadow ray (the production light sampler still rejects on
    // any HitWorld boundary; there are no refracted shadow rays). Check the
    // boundary ordering deterministically instead of relying on MC sampling.
    float3 shadowFrom = from; // production shadow rays start at the hit point
    float3 shadowDir = dir;
    float tLight = tEnd;
    // Open scene: the first boundary in range is the emissive ball itself.
    int openID = -1;
    bool openEnter = false;
    float openT = 0.0f;
    TASK_CHECK(NearestBoundary(Ray(shadowFrom, shadowDir), 0.001f, tLight, openID, openEnter, openT));
    (void)openT;
    TASK_CHECK(openID == 2 && openEnter); // id 2 is the emissive ball in the open layout

    SceneBuilder blocked = build(true);
    TASK_CHECK(blocked.Install());
    int blockID = -1;
    bool blockEnter = false;
    float blockT = 0.0f;
    TASK_CHECK(NearestBoundary(Ray(shadowFrom, shadowDir), 0.001f, tLight, blockID, blockEnter, blockT));
    TASK_CHECK(blockID == 2 && blockEnter && blockT < tLight); // glass id 2 comes first now
    std::printf("shadow blocker: id=%d t=%.4f before light t=%.4f\n", blockID, blockT, tLight);

    // Both full traces stay finite and deterministic under the fixed stream.
    Ray ray(float3(-1.5f, 0.0f, 0), float3(1, 0, 0));
    uint32_t seedOpen = 31337, seedBlocked = 31337;
    int rcOpen = 0, rcBlocked = 0;
    float3 openRad = TraceMedium(ray, 0, rcOpen, seedOpen);
    float3 blockedRad = TraceMedium(ray, 0, rcBlocked, seedBlocked);
    PrintVec("direct light unblocked", openRad);
    PrintVec("direct light glass-blocked", blockedRad);
    for (const float3& v : {openRad, blockedRad})
        TASK_CHECK(std::isfinite(v.getX()) && std::isfinite(v.getY()) && std::isfinite(v.getZ()));
    return 0;
}

static int TransportInvalidRejected()
{
    ProductionGuard guard;
    SceneBuilder b;
    b.Add(0, 0, 0, 1.0f, 2, 0, 1.5f, float3(0, 0, 0));
    TASK_CHECK(b.Install());
    int before = -1;
    TASK_CHECK(MediumEnabled());

    MediumConfig bad = b.cfg;
    bad.spheres[0].eta = 0.9f;
    TASK_CHECK(!SetMediumConfig(bad));
    bad = b.cfg;
    bad.spheres[0].sigma = float3pack(0, -1, 0);
    TASK_CHECK(!SetMediumConfig(bad));
    bad = b.cfg;
    bad.spheres[0].eta = std::numeric_limits<float>::infinity();
    TASK_CHECK(!SetMediumConfig(bad));
    MediumConfig wrongSize = b.cfg;
    wrongSize.spheres.pop_back();
    TASK_CHECK(!SetMediumConfig(wrongSize));

    // The valid configuration must still be live after the rejected attempts.
    TASK_CHECK(MediumCount() == 1);
    TASK_CHECK(std::fabs(SphereEta(0) - 1.5f) < 1.0e-6f);
    TASK_CHECK(before == -1);
    ResetToDefaultScene();
    TASK_CHECK(!MediumEnabled());
    return 0;
}

// ===========================================================================
// registration
// ===========================================================================

const struct task_case task_cases[] =
{
    {"membership", "overlap_order_and_interior_membership", MembershipOverlapAndOrder},
    {"membership", "priority_tie_and_hidden_boundary", MembershipTieAndHidden},
    {"interfaces", "fresnel_and_snell_against_reference", InterfaceFresnelSnell},
    {"interfaces", "reflection_and_tir_ownership", InterfaceReflectionOwnership},
    {"transport", "analytic_rgb_absorption", TransportAnalyticAbsorption},
    {"transport", "hidden_boundary_rng_depth_invariance", TransportHiddenInvariance},
    {"transport", "direct_light_absorption_and_glass_occlusion", TransportDirectLight},
    {"transport", "eta_divergence_reaches_different_lights", TransportEtaDivergence},
    {"transport", "draw_test_small_ppm", TransportDrawTestPpm},
    {"transport", "invalid_config_rejected_before_scene_change", TransportInvalidRejected},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
