#include "Medium.h"
#include "Test.h"
#include <cmath>
#include <algorithm>
#include <cstring>

// Production internals provided by Test.cpp. They are declared here instead
// of extending Test.h so the legacy test surface stays untouched.
bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outID);
const Sphere* ProductionSpheres();
int ProductionActiveCount();
int ProductionMaterialType(int id); // 0 Lambert, 1 Metal, 2 Dielectric
float3 ProductionMaterialEmissive(int id);
bool ProductionLegacyScatter(int id, const Ray& rIn, const Hit& rec, float3& attenuation,
                              Ray& scattered, float3& outLightE, int& inoutRayCount,
                              uint32_t& state, const std::vector<int>& mediumActive);
bool ProductionInstallScene(const std::vector<Sphere>& spheres,
                            const std::vector<medium::SceneMaterialDesc>& mats,
                            const medium::MediumConfig& cfg);
void ProductionResetScene();

namespace medium {

static const float kMediumMinT = 0.001f;
static const float kMediumMaxT = 1.0e7f;
static const int kMediumMaxDepth = 10;

static std::vector<MediumSphere> g_media;
static float3 g_sky(0.15f, 0.21f, 0.3f);

int MediumCount() { return (int)g_media.size(); }
bool MediumEnabled() { return !g_media.empty(); }

float3 MediumSky() { return g_sky; }

bool ValidateConfig(const MediumConfig& cfg, int expectedCount)
{
    if ((int)cfg.spheres.size() != expectedCount)
        return false;
    if (!std::isfinite(cfg.sky.x) || !std::isfinite(cfg.sky.y) || !std::isfinite(cfg.sky.z))
        return false;
    for (const MediumSphere& m : cfg.spheres)
    {
        if (!std::isfinite(m.eta) || m.eta < 1.0f)
            return false;
        float sx = m.sigma.x, sy = m.sigma.y, sz = m.sigma.z;
        if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz))
            return false;
        if (sx < 0.0f || sy < 0.0f || sz < 0.0f)
            return false;
    }
    return true;
}

bool SetMediumConfig(const MediumConfig& cfg)
{
    if (!ValidateConfig(cfg, ProductionActiveCount()))
        return false;
    g_media = cfg.spheres;
    g_sky = cfg.sky.toFloat3();
    return true;
}

void ClearMediumConfig()
{
    g_media.clear();
    g_sky = float3(0.15f, 0.21f, 0.3f);
}

float SphereEta(int id)
{
    if (id < 0 || id >= (int)g_media.size())
        return 1.0f; // air
    return g_media[id].eta;
}

float3 SphereSigma(int id)
{
    if (id < 0 || id >= (int)g_media.size())
        return float3(0, 0, 0); // air
    return g_media[id].sigma.toFloat3();
}

bool PointInside(int id, const float3& p)
{
    const Sphere& s = ProductionSpheres()[id];
    float3 c = s.center.toFloat3();
    // Strict inequality: a point on the surface is not inside, and an exact
    // tangential contact never enters the region.
    return sqLength(p - c) < s.radius * s.radius;
}

std::vector<int> InitialActive(const Ray& r)
{
    std::vector<int> active;
    int n = ProductionActiveCount();
    for (int i = 0; i < n; ++i)
    {
        // Only dielectric spheres define a medium; opaque spheres never join
        // the active set even when the origin lies inside one of them.
        if (ProductionMaterialType(i) == 2 && PointInside(i, r.orig))
            active.push_back(i);
    }
    return active;
}

int OwnerOf(const std::vector<int>& active)
{
    int owner = -1;
    for (int id : active)
    {
        if (owner == -1 || g_media[id].priority > g_media[owner].priority ||
            (g_media[id].priority == g_media[owner].priority && id < owner))
        {
            owner = id;
        }
    }
    return owner;
}

std::vector<int> CrossedSet(const std::vector<int>& active, int id, bool entering)
{
    std::vector<int> out = active;
    if (entering)
    {
        if (std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    }
    else
    {
        out.erase(std::remove(out.begin(), out.end(), id), out.end());
    }
    return out;
}

std::vector<Crossing> EnumerateCrossings(const Ray& r, float tMin, float tMax)
{
    std::vector<Crossing> crossings;
    const Sphere* spheres = ProductionSpheres();
    int n = ProductionActiveCount();
    for (int i = 0; i < n; ++i)
    {
        float3 c = spheres[i].center.toFloat3();
        float3 co = c - r.orig;
        float nb = dot(co, r.dir);
        float cVal = dot(co, co) - spheres[i].radius * spheres[i].radius;
        float disc = nb * nb - cVal;
        if (disc <= 0.0f)
            continue; // no hit, or an exact tangent: does not cross into the region
        float sq = sqrtf(disc);
        float t0 = nb - sq; // entry (ray points against outward normal there)
        float t1 = nb + sq; // exit
        if (t0 > tMin && t0 < tMax)
            crossings.push_back(Crossing{t0, i, true});
        if (t1 > tMin && t1 < tMax)
            crossings.push_back(Crossing{t1, i, false});
    }
    std::sort(crossings.begin(), crossings.end(),
              [](const Crossing& a, const Crossing& b) { return a.t < b.t; });
    return crossings;
}

bool NearestBoundary(const Ray& r, float tMin, float tMax, int& outID, bool& outEntering, float& outT)
{
    std::vector<Crossing> crossings = EnumerateCrossings(r, tMin, tMax);
    if (crossings.empty())
        return false;
    outID = crossings.front().id;
    outEntering = crossings.front().entering;
    outT = crossings.front().t;
    return true;
}

float Fresnel(float cosI, float etaI, float etaT)
{
    if (etaI == etaT)
        return 0.0f; // matched indices: no interface, direction unchanged
    cosI = std::min(1.0f, std::max(-1.0f, cosI));
    if (cosI < 0.0f)
    {
        cosI = -cosI;
        std::swap(etaI, etaT);
    }
    float sinT2 = (etaI / etaT) * (etaI / etaT) * (1.0f - cosI * cosI);
    if (sinT2 >= 1.0f)
        return 1.0f; // total internal reflection
    float cosT = sqrtf(1.0f - sinT2);
    float rS = (etaI * cosI - etaT * cosT) / (etaI * cosI + etaT * cosT);
    float rP = (etaI * cosT - etaT * cosI) / (etaI * cosT + etaT * cosI);
    return 0.5f * (rS * rS + rP * rP);
}

bool RefractDir(const float3& dirIn, const float3& normalAgainst, float etaI, float etaT, float3& outDir)
{
    // normalAgainst points against the incoming ray: dot(dirIn, n) < 0.
    float eta = etaI / etaT;
    float cosI = -dot(dirIn, normalAgainst);
    float sinT2 = eta * eta * (1.0f - cosI * cosI);
    if (sinT2 >= 1.0f)
        return false; // total internal reflection
    float cosT = sqrtf(1.0f - sinT2);
    outDir = eta * dirIn + (eta * cosI - cosT) * normalAgainst;
    return true;
}

float3 Absorption(int owner, float d)
{
    if (owner < 0 || d <= 0.0f)
        return float3(1, 1, 1);
    float3 s = SphereSigma(owner);
    return float3(expf(-s.getX() * d), expf(-s.getY() * d), expf(-s.getZ() * d));
}

// Walks every crossing from the ray origin, integrating the owning medium's
// absorption. Hidden boundaries split the optical path but never skip it.
// Opaque spheres do not participate in medium membership.
static float3 WalkTransmittance(const std::vector<int>& active0,
                                 float tEnd, std::vector<Crossing>& crossings,
                                 int& outHidden, int* outOwnerAtEnd)
{
    std::vector<int> active = active0;
    float3 trans(1, 1, 1);
    float lastT = 0.0f; // distance is measured from the ray origin, even though
                        // intersection testing starts at tMin to avoid self hits
    int hidden = 0;
    for (const Crossing& cr : crossings)
    {
        if (cr.t > tEnd)
            break;
        if (ProductionMaterialType(cr.id) != 2)
            continue; // opaque sphere: never a medium event on a light segment
        int ownerBefore = OwnerOf(active);
        trans = trans * Absorption(ownerBefore, cr.t - lastT);
        std::vector<int> after = CrossedSet(active, cr.id, cr.entering);
        int ownerAfter = OwnerOf(after);
        if (ownerAfter == ownerBefore)
            ++hidden;
        active.swap(after);
        lastT = cr.t;
    }
    int finalOwner = OwnerOf(active);
    trans = trans * Absorption(finalOwner, tEnd - lastT);
    outHidden = hidden;
    if (outOwnerAtEnd)
        *outOwnerAtEnd = finalOwner;
    return trans;
}

bool NextSegment(const Ray& r, const std::vector<int>& initialActive, float tMin, float tMax,
                 SegmentResult& outSeg, int& inoutHiddenCount)
{
    std::vector<Crossing> crossings = EnumerateCrossings(r, tMin, tMax);
    std::vector<int> active = initialActive;
    float3 trans(1, 1, 1);
    float lastT = 0.0f;
    for (const Crossing& cr : crossings)
    {
        int ownerBefore = OwnerOf(active);
        trans = trans * Absorption(ownerBefore, cr.t - lastT);
        const bool dielectric = ProductionMaterialType(cr.id) == 2;
        std::vector<int> after = dielectric ? CrossedSet(active, cr.id, cr.entering) : active;
        int ownerAfter = dielectric ? OwnerOf(after) : ownerBefore;
        lastT = cr.t;

        const Sphere& sphere = ProductionSpheres()[cr.id];
        float3 pos = r.pointAt(cr.t);
        float3 outwardN = normalize(pos - sphere.center.toFloat3());
        float3 against = dot(r.dir, outwardN) > 0.0f ? -outwardN : outwardN;

        if (dielectric && ownerAfter == ownerBefore)
        {
            // Occluded dielectric boundary: the owning medium does not change.
            // Commit the membership update, keep the direction, draw nothing,
            // and keep going without touching the random stream or depth budget.
            active.swap(after);
            ++inoutHiddenCount;
            continue;
        }

        // Either a real dielectric interface (owner changed) or an opaque
        // Lambert/Metal surface, which always ends the segment.
        outSeg.distance = cr.t;
        outSeg.transmittance = trans;
        outSeg.boundaryID = dielectric ? cr.id : -1;
        outSeg.entering = cr.entering;
        outSeg.ownerChanged = dielectric && ownerAfter != ownerBefore;
        outSeg.ownerBefore = ownerBefore;
        outSeg.ownerAfter = ownerAfter;
        outSeg.etaBefore = SphereEta(ownerBefore);
        outSeg.etaAfter = SphereEta(ownerAfter);
        outSeg.normalAgainst = against;
        outSeg.hitSurface = true;
        outSeg.hitID = cr.id;
        outSeg.hit.pos = pos;
        outSeg.hit.normal = outwardN;
        outSeg.hit.t = cr.t;
        outSeg.activeBeforeSurface = active;
        outSeg.activeAtSurface = after;
        return true;
    }

    // No owning-medium change before tMax: finish the optical path in the
    // final owner (all finite spheres have been exited by then in practice).
    int finalOwner = OwnerOf(active);
    trans = trans * Absorption(finalOwner, tMax - lastT);
    outSeg.transmittance = trans;
    outSeg.distance = tMax;
    outSeg.boundaryID = -1;
    outSeg.hitSurface = false;
    outSeg.hitID = -1;
    outSeg.ownerBefore = finalOwner;
    outSeg.ownerAfter = -1;
    outSeg.activeBeforeSurface = active;
    outSeg.activeAtSurface = active;
    return false;
}

float3 ShadowAttenuation(const float3& from, const float3& unitDir, float tEnd,
                         const std::vector<int>& activeAtOrigin)
{
    Ray r(from, unitDir);
    std::vector<Crossing> crossings = EnumerateCrossings(r, kMediumMinT, tEnd);
    int hidden = 0;
    return WalkTransmittance(activeAtOrigin, tEnd, crossings, hidden, nullptr);
}

static float3 TraceRec(const Ray& r, const std::vector<int>& active0, int depth,
                       bool doMaterialE, int& inoutRayCount, uint32_t& state,
                       TraceDebugStats* stats, std::vector<int>* ownerPath);

float3 TraceMedium(const Ray& r, int depth, int& inoutRayCount, uint32_t& state,
                   TraceDebugStats* stats, std::vector<int>* ownerPath)
{
    std::vector<int> active = InitialActive(r);
    return TraceRec(r, active, depth, true, inoutRayCount, state, stats, ownerPath);
}

static float3 TraceRec(const Ray& r, const std::vector<int>& active0, int depth,
                       bool doMaterialE, int& inoutRayCount, uint32_t& state,
                       TraceDebugStats* stats, std::vector<int>* ownerPath)
{
    ++inoutRayCount;
    SegmentResult seg;
    int hidden = 0;
    bool hasBoundary = NextSegment(r, active0, kMediumMinT, kMediumMaxT, seg, hidden);
    if (stats)
        stats->hiddenBoundaries += (unsigned)hidden;
    if (!hasBoundary)
        return seg.transmittance * MediumSky();

    if (ownerPath)
        ownerPath->push_back(seg.ownerBefore);

    const float3 trans = seg.transmittance;
    const int id = seg.hitID;
    const float3 matE = ProductionMaterialEmissive(id);
    const int matType = ProductionMaterialType(id);

    if (matType != 2) // Lambert or Metal: legacy surface behaviour, medium-filtered
    {
        Ray scattered;
        float3 attenuation;
        float3 lightE;
        if (depth < kMediumMaxDepth &&
            ProductionLegacyScatter(id, r, seg.hit, attenuation, scattered, lightE,
                                    inoutRayCount, state, seg.activeAtSurface))
        {
            if (stats)
                ++stats->scatterEvents;
            float3 e = doMaterialE ? matE : float3(0, 0, 0);
            // Original emission de-duplication: a Lambert bounce suppresses the
            // emission term on the next recursion.
            bool nextDo = (matType != 0) && doMaterialE;
            std::vector<int> nextActive = InitialActive(scattered);
            return trans * (e + lightE + attenuation *
                TraceRec(scattered, nextActive, depth + 1, nextDo, inoutRayCount, state, stats, ownerPath));
        }
        return trans * matE;
    }

    // Dielectric: the surface is an interface between the two owning media.
    if (depth >= kMediumMaxDepth)
        return trans * matE; // dielectric carries no emission

    float3 reflDir = reflect(r.dir, seg.normalAgainst);
    float3 refrDir;
    bool canRefract = RefractDir(r.dir, seg.normalAgainst, seg.etaBefore, seg.etaAfter, refrDir);
    if (stats)
        ++stats->scatterEvents;

    if (!canRefract)
    {
        // Total internal reflection: deterministic, no random number drawn,
        // and the active set present before the interface is preserved.
        Ray reflected(seg.hit.pos, normalize(reflDir));
        return trans * TraceRec(reflected, seg.activeBeforeSurface, depth + 1, doMaterialE,
                                inoutRayCount, state, stats, ownerPath);
    }

    const float cosI = -dot(r.dir, seg.normalAgainst);
    const float fresnel = Fresnel(cosI, seg.etaBefore, seg.etaAfter);
    const float xi = RandomFloat01(state); // exactly one draw at a refract-able interface
    if (xi < fresnel)
    {
        // Reflection keeps the current membership and unit weight.
        Ray reflected(seg.hit.pos, normalize(reflDir));
        return trans * TraceRec(reflected, seg.activeBeforeSurface, depth + 1, doMaterialE,
                                inoutRayCount, state, stats, ownerPath);
    }

    // Transmission commits the full post-crossing set (including any hidden
    // boundaries consumed inside the segment) and applies the radiance form
    // of the solid angle throughput factor (eta_i / eta_t)^2.
    float etaRatio = seg.etaBefore / seg.etaAfter;
    Ray transmitted(seg.hit.pos, normalize(refrDir));
    return trans * (etaRatio * etaRatio) *
           TraceRec(transmitted, seg.activeAtSurface, depth + 1, doMaterialE,
                    inoutRayCount, state, stats, ownerPath);
}

bool InstallScene(const std::vector<Sphere>& spheres,
                  const std::vector<SceneMaterialDesc>& mats,
                  const MediumConfig& cfg)
{
    return ProductionInstallScene(spheres, mats, cfg);
}

// ---------------------------------------------------------------------------
// Small-ball scene convenience setup.
// ---------------------------------------------------------------------------

bool LoadSmallBallScene()
{
    // The original nine-sphere scene (big scene disabled).
    const Sphere spheres[] =
    {
        Sphere(float3(0, -100.5f, -1), 100),
        Sphere(float3(2, 0, -1), 0.5f),
        Sphere(float3(0, 0, -1), 0.5f),
        Sphere(float3(-2, 0, -1), 0.5f),
        Sphere(float3(2, 0, 1), 0.5f),
        Sphere(float3(0, 0, 1), 0.5f),
        Sphere(float3(-2, 0, 1), 0.5f),
        Sphere(float3(0.5f, 1, 0.5f), 0.5f),
        Sphere(float3(-1.5f, 1.5f, 0.f), 0.3f),
    };
    const SceneMaterialDesc mats[] =
    {
        {0, float3pack(0.8f, 0.8f, 0.8f), float3pack(0, 0, 0), 0.0f, 0.0f},
        {0, float3pack(0.8f, 0.4f, 0.4f), float3pack(0, 0, 0), 0.0f, 0.0f},
        {0, float3pack(0.4f, 0.8f, 0.4f), float3pack(0, 0, 0), 0.0f, 0.0f},
        {1, float3pack(0.4f, 0.4f, 0.8f), float3pack(0, 0, 0), 0.0f, 0.0f},
        {1, float3pack(0.4f, 0.8f, 0.4f), float3pack(0, 0, 0), 0.0f, 0.0f},
        {1, float3pack(0.4f, 0.8f, 0.4f), float3pack(0, 0, 0), 0.2f, 0.0f},
        {1, float3pack(0.4f, 0.8f, 0.4f), float3pack(0, 0, 0), 0.6f, 0.0f},
        {2, float3pack(0.4f, 0.4f, 0.4f), float3pack(0, 0, 0), 0.0f, 1.5f},
        {0, float3pack(0.8f, 0.6f, 0.2f), float3pack(30, 25, 15), 0.0f, 0.0f},
    };
    MediumConfig cfg;
    cfg.sky = float3pack(0.15f, 0.21f, 0.3f);
    for (int i = 0; i < 9; ++i)
    {
        MediumSphere m;
        m.priority = 0;
        m.eta = 1.0f;
        m.sigma = float3pack(0, 0, 0);
        if (i == 7)
        {
            m.priority = 1;
            m.eta = 1.5f;
            m.sigma = float3pack(0.05f, 0.10f, 0.15f);
        }
        cfg.spheres.push_back(m);
    }
    std::vector<Sphere> sphereVec(spheres, spheres + 9);
    std::vector<SceneMaterialDesc> matVec(mats, mats + 9);
    return ProductionInstallScene(sphereVec, matVec, cfg);
}

void ResetToDefaultScene()
{
    ProductionResetScene();
}

} // namespace medium
