#include "Config.h"
#include "Test.h"
#include "Maths.h"
#include "SphereBvh.h"
#include <algorithm>
#include <vector>
#if CPU_CAN_DO_THREADS
#include "enkiTS/TaskScheduler_c.h"
#endif
#include <atomic>

// 46 spheres (2 emissive) when enabled; 9 spheres (1 emissive) when disabled
#define DO_BIG_SCENE 1

static Sphere s_BuiltinSpheres[] =
{
    {float3(0,-100.5,-1), 100},
    {float3(2,0,-1), 0.5f},
    {float3(0,0,-1), 0.5f},
    {float3(-2,0,-1), 0.5f},
    {float3(2,0,1), 0.5f},
    {float3(0,0,1), 0.5f},
    {float3(-2,0,1), 0.5f},
    {float3(0.5f,1,0.5f), 0.5f},
    {float3(-1.5f,1.5f,0.f), 0.3f},
#if DO_BIG_SCENE
    {float3(4,0,-3), 0.5f}, {float3(3,0,-3), 0.5f}, {float3(2,0,-3), 0.5f}, {float3(1,0,-3), 0.5f}, {float3(0,0,-3), 0.5f}, {float3(-1,0,-3), 0.5f}, {float3(-2,0,-3), 0.5f}, {float3(-3,0,-3), 0.5f}, {float3(-4,0,-3), 0.5f},
    {float3(4,0,-4), 0.5f}, {float3(3,0,-4), 0.5f}, {float3(2,0,-4), 0.5f}, {float3(1,0,-4), 0.5f}, {float3(0,0,-4), 0.5f}, {float3(-1,0,-4), 0.5f}, {float3(-2,0,-4), 0.5f}, {float3(-3,0,-4), 0.5f}, {float3(-4,0,-4), 0.5f},
    {float3(4,0,-5), 0.5f}, {float3(3,0,-5), 0.5f}, {float3(2,0,-5), 0.5f}, {float3(1,0,-5), 0.5f}, {float3(0,0,-5), 0.5f}, {float3(-1,0,-5), 0.5f}, {float3(-2,0,-5), 0.5f}, {float3(-3,0,-5), 0.5f}, {float3(-4,0,-5), 0.5f},
    {float3(4,0,-6), 0.5f}, {float3(3,0,-6), 0.5f}, {float3(2,0,-6), 0.5f}, {float3(1,0,-6), 0.5f}, {float3(0,0,-6), 0.5f}, {float3(-1,0,-6), 0.5f}, {float3(-2,0,-6), 0.5f}, {float3(-3,0,-6), 0.5f}, {float3(-4,0,-6), 0.5f},
    {float3(1.5f,1.5f,-2), 0.3f},
#endif // #if DO_BIG_SCENE
};
const int kSphereCount = sizeof(s_BuiltinSpheres) / sizeof(s_BuiltinSpheres[0]);

static Material s_BuiltinMats[kSphereCount] =
{
    { Material::Lambert, float3(0.8f, 0.8f, 0.8f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.8f, 0.4f, 0.4f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0, 0, },
    { Material::Metal, float3(0.4f, 0.4f, 0.8f), float3(0,0,0), 0, 0 },
    { Material::Metal, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0, 0 },
    { Material::Metal, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0.2f, 0 },
    { Material::Metal, float3(0.4f, 0.8f, 0.4f), float3(0,0,0), 0.6f, 0 },
    { Material::Dielectric, float3(0.4f, 0.4f, 0.4f), float3(0,0,0), 0, 1.5f },
    { Material::Lambert, float3(0.8f, 0.6f, 0.2f), float3(30,25,15), 0, 0 },
#if DO_BIG_SCENE
    { Material::Lambert, float3(0.1f, 0.1f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.2f, 0.2f, 0.2f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.3f, 0.3f, 0.3f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.4f, 0.4f, 0.4f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.5f, 0.5f, 0.5f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.6f, 0.6f, 0.6f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.7f, 0.7f, 0.7f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.8f, 0.8f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.9f, 0.9f, 0.9f), float3(0,0,0), 0, 0, },
    { Material::Metal, float3(0.1f, 0.1f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.2f, 0.2f, 0.2f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.3f, 0.3f, 0.3f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.4f, 0.4f, 0.4f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.5f, 0.5f, 0.5f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.6f, 0.6f, 0.6f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.7f, 0.7f, 0.7f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.8f, 0.8f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.9f, 0.9f, 0.9f), float3(0,0,0), 0, 0, },
    { Material::Metal, float3(0.8f, 0.1f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.8f, 0.5f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.8f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.4f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.1f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.1f, 0.8f, 0.5f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.1f, 0.8f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.1f, 0.1f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.5f, 0.1f, 0.8f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.8f, 0.1f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.8f, 0.5f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.8f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.4f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.1f, 0.8f, 0.1f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.1f, 0.8f, 0.5f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.1f, 0.8f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Lambert, float3(0.1f, 0.1f, 0.8f), float3(0,0,0), 0, 0, }, { Material::Metal, float3(0.5f, 0.1f, 0.8f), float3(0,0,0), 0, 0, },
    { Material::Lambert, float3(0.1f, 0.2f, 0.5f), float3(3,10,20), 0, 0 },
#endif
};

// Active CPU scene. Owned here; SetCpuScene/RefitCpuScene copy into it.
static std::vector<Sphere> s_SceneSpheres;
static std::vector<Material> s_SceneMats;
static std::vector<int> s_EmissiveSpheres;
static bool s_UseBuiltinScene = true;
static bool s_SceneReady = false;

static SphereBvh s_Bvh;
// Exhaustive reference path, for small scenes. Built lazily whenever the
// reference path is enabled (its SoA is never needed while the BVH is active).
static SpheresSoA* s_ReferenceSoA = 0;
static bool s_ReferenceDirty = true;
static bool s_UseAcceleration = true;
static Camera s_Cam;

// Trace statistics for the last DrawTest. Aggregated once per frame, after all
// worker threads have finished; while tracing, each thread accumulates into
// its own local counters.
static CpuTraceStats s_LastStats = {0, 0, 0};

static void RebuildReferenceSoA()
{
    delete s_ReferenceSoA;
    s_ReferenceSoA = new SpheresSoA((int)s_SceneSpheres.size());
    for (size_t i = 0; i < s_SceneSpheres.size(); ++i)
    {
        const Sphere& s = s_SceneSpheres[i];
        s_ReferenceSoA->centerX[i] = s.center.x;
        s_ReferenceSoA->centerY[i] = s.center.y;
        s_ReferenceSoA->centerZ[i] = s.center.z;
        s_ReferenceSoA->sqRadius[i] = s.radius * s.radius;
        s_ReferenceSoA->invRadius[i] = s.invRadius;
    }
    s_ReferenceDirty = false;
}

static void MarkReferenceDirty()
{
    s_ReferenceDirty = true;
}

static const SpheresSoA& GetReferenceSoA()
{
    if (s_ReferenceDirty || !s_ReferenceSoA)
        RebuildReferenceSoA();
    return *s_ReferenceSoA;
}

static void RefreshEmissives()
{
    s_EmissiveSpheres.clear();
    for (int i = 0; i < (int)s_SceneMats.size(); ++i)
    {
        const Material& smat = s_SceneMats[i];
        if (smat.emissive.x > 0 || smat.emissive.y > 0 || smat.emissive.z > 0)
            s_EmissiveSpheres.push_back(i);
    }
}

static void UpdateDerivedData()
{
    for (size_t i = 0; i < s_SceneSpheres.size(); ++i)
        s_SceneSpheres[i].UpdateDerivedData();
}

static void ResetSceneToBuiltin()
{
    s_SceneSpheres.assign(s_BuiltinSpheres, s_BuiltinSpheres + kSphereCount);
    s_SceneMats.assign(s_BuiltinMats, s_BuiltinMats + kSphereCount);
    s_UseBuiltinScene = true;
    s_SceneReady = true;
    RefreshEmissives();
    UpdateDerivedData();
    MarkReferenceDirty();
    s_Bvh.Build(s_SceneSpheres.data(), (int)s_SceneSpheres.size());
}

static void EnsureScene()
{
    if (!s_SceneReady)
        ResetSceneToBuiltin();
}

void SetCpuScene(const Sphere* spheres, const Material* materials, int count)
{
    if (count <= 0 || spheres == 0 || materials == 0)
    {
        ClearCpuScene();
        return;
    }
    s_SceneSpheres.assign(spheres, spheres + count);
    s_SceneMats.assign(materials, materials + count);
    s_UseBuiltinScene = false;
    s_SceneReady = true;
    RefreshEmissives();
    UpdateDerivedData();
    MarkReferenceDirty();
    s_Bvh.Build(s_SceneSpheres.data(), count);
}

void ClearCpuScene()
{
    s_SceneSpheres.clear();
    s_SceneMats.clear();
    s_EmissiveSpheres.clear();
    s_UseBuiltinScene = false;
    s_SceneReady = true;
    s_Bvh.Clear();
    MarkReferenceDirty();
}

void ResetCpuScene()
{
    ResetSceneToBuiltin();
}

bool RefitCpuScene(const Sphere* spheres, int count)
{
    EnsureScene();
    if (count != (int)s_SceneSpheres.size() || spheres == 0)
        return false;
    s_SceneSpheres.assign(spheres, spheres + count);
    UpdateDerivedData();
    MarkReferenceDirty();
    // Same primitive count: only sphere data and node bounds change; the
    // topology is preserved.
    s_Bvh.Refit(s_SceneSpheres.data(), count);
    return true;
}

void SetCpuAccelerationEnabled(bool enabled)
{
    s_UseAcceleration = enabled;
}

bool GetCpuAccelerationEnabled()
{
    return s_UseAcceleration;
}

bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outID, CpuHitStats* outStats)
{
    EnsureScene();
    SphereBvh::QueryStats stats;
    if (outStats)
    {
        outStats->nodeVisits = 0;
        outStats->sphereTests = 0;
    }
    if (!s_UseAcceleration)
    {
        // Exhaustive reference over the same scene. This replaces the
        // accelerated query; it is never run after one.
        const SpheresSoA& ref = GetReferenceSoA();
        outID = HitSpheres(r, ref, tMin, tMax, outHit);
        if (outStats)
            outStats->sphereTests = ref.count;
        return outID != -1;
    }
    outID = s_Bvh.ClosestHit(r, tMin, tMax, outHit, outStats ? &stats : 0);
    if (outStats)
    {
        outStats->nodeVisits = stats.nodesVisited;
        outStats->sphereTests = stats.sphereTests;
    }
    return outID != -1;
}

bool OccludedWorld(const Ray& r, float tMin, float tMax, CpuHitStats* outStats)
{
    EnsureScene();
    SphereBvh::QueryStats stats;
    if (outStats)
    {
        outStats->nodeVisits = 0;
        outStats->sphereTests = 0;
    }
    if (!s_UseAcceleration)
    {
        const SpheresSoA& ref = GetReferenceSoA();
        Hit h;
        int id = HitSpheres(r, ref, tMin, tMax, h);
        if (outStats)
            outStats->sphereTests = ref.count;
        return id != -1;
    }
    bool hit = s_Bvh.Occluded(r, tMin, tMax, outStats ? &stats : 0);
    if (outStats)
    {
        outStats->nodeVisits = stats.nodesVisited;
        outStats->sphereTests = stats.sphereTests;
    }
    return hit;
}

void GetCpuTraceStats(CpuTraceStats& outStats)
{
    outStats = s_LastStats;
}


static bool Scatter(const Material& mat, const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered, float3& outLightE, int& inoutRayCount, CpuTraceStats& stats, uint32_t& state)
{
    outLightE = float3(0,0,0);
    if (mat.type == Material::Lambert)
    {
        // random point on unit sphere that is tangent to the hit point
        float3 target = rec.pos + rec.normal + RandomUnitVector(state);
        scattered = Ray(rec.pos, normalize(target - rec.pos));
        float3 matAlbedo = mat.albedo.toFloat3();
        attenuation = matAlbedo;

        // sample lights
#if DO_LIGHT_SAMPLING
        for (int j = 0; j < (int)s_EmissiveSpheres.size(); ++j)
        {
            int i = s_EmissiveSpheres[j];
            const Material& smat = s_SceneMats[i];
            if (&mat == &smat)
                continue; // skip self
            const Sphere& s = s_SceneSpheres[i];

            // create a random direction towards sphere
            // coord system for sampling: sw, su, sv
            float3 sc = s.center.toFloat3();
            float3 sw = normalize(sc - rec.pos);
            float3 su = normalize(cross(fabs(sw.getX())>0.01f ? float3(0,1,0):float3(1,0,0), sw));
            float3 sv = cross(sw, su);
            // sample sphere by solid angle
            float cosAMax = sqrtf(1.0f - s.radius*s.radius / sqLength(rec.pos-sc));
            float eps1 = RandomFloat01(state), eps2 = RandomFloat01(state);
            float cosA = 1.0f - eps1 + eps1 * cosAMax;
            float sinA = sqrtf(1.0f - cosA*cosA);
            float phi = 2 * kPI * eps2;
            float3 l = su * (cosf(phi) * sinA) + sv * (sinf(phi) * sinA) + sw * cosA;
            //l = normalize(l); // NOTE(fg): This is already normalized, by construction.

            // shoot shadow ray
            Hit lightHit;
            int hitID;
            CpuHitStats hitStats;
            ++inoutRayCount;
            if (HitWorld(Ray(rec.pos, l), kMinT, kMaxT, lightHit, hitID, &hitStats) && hitID == i)
            {
                float omega = 2 * kPI * (1-cosAMax);

                float3 rdir = r_in.dir;
                AssertUnit(rdir);
                float3 nl = dot(rec.normal, rdir) < 0 ? rec.normal : -rec.normal;
                float3 smatEmissive = smat.emissive.toFloat3();
                outLightE += (matAlbedo * smatEmissive) * (std::max(0.0f, dot(l, nl)) * omega / kPI);
            }
            stats.nodeVisits += hitStats.nodeVisits;
            stats.sphereTests += hitStats.sphereTests;
        }
#endif
        return true;
    }
    else if (mat.type == Material::Metal)
    {
        AssertUnit(r_in.dir); AssertUnit(rec.normal);
        float3 refl = reflect(r_in.dir, rec.normal);
        // reflected ray, and random inside of sphere based on roughness
        float roughness = mat.roughness;
#if DO_MITSUBA_COMPARE
        roughness = 0; // until we get better BRDF for metals
#endif
        scattered = Ray(rec.pos, normalize(refl + roughness*RandomInUnitSphere(state)));
        float3 matAlbedo = mat.albedo.toFloat3();
        attenuation = matAlbedo;
        return dot(scattered.dir, rec.normal) > 0;
    }
    else if (mat.type == Material::Dielectric)
    {
        AssertUnit(r_in.dir); AssertUnit(rec.normal);
        float3 outwardN;
        float3 rdir = r_in.dir;
        float3 refl = reflect(rdir, rec.normal);
        float nint;
        attenuation = float3(1,1,1);
        float3 refr;
        float reflProb;
        float cosine;
        if (dot(rdir, rec.normal) > 0)
        {
            outwardN = -rec.normal;
            nint = mat.ri;
            cosine = mat.ri * dot(rdir, rec.normal);
        }
        else
        {
            outwardN = rec.normal;
            nint = 1.0f / mat.ri;
            cosine = -dot(rdir, rec.normal);
        }
        if (refract(rdir, outwardN, nint, refr))
        {
            reflProb = schlick(cosine, mat.ri);
        }
        else
        {
            reflProb = 1;
        }
        if (RandomFloat01(state) < reflProb)
            scattered = Ray(rec.pos, normalize(refl));
        else
            scattered = Ray(rec.pos, normalize(refr));
    }
    else
    {
        attenuation = float3(1,0,1);
        return false;
    }
    return true;
}

static float3 Trace(const Ray& r, int depth, int& inoutRayCount, CpuTraceStats& stats, uint32_t& state, bool doMaterialE = true)
{
    Hit rec;
    int id = 0;
    CpuHitStats hitStats;
    ++inoutRayCount;
    if (HitWorld(r, kMinT, kMaxT, rec, id, &hitStats))
    {
        stats.nodeVisits += hitStats.nodeVisits;
        stats.sphereTests += hitStats.sphereTests;
        Ray scattered;
        float3 attenuation;
        float3 lightE;
        const Material& mat = s_SceneMats[id];
        float3 matE = mat.emissive.toFloat3();
        if (depth < kMaxDepth && Scatter(mat, r, rec, attenuation, scattered, lightE, inoutRayCount, stats, state))
        {
#if DO_LIGHT_SAMPLING
            if (!doMaterialE) matE = float3(0,0,0); // don't add material emission if told so
            // dor Lambert materials, we just did explicit light (emissive) sampling and already
            // for their contribution, so if next ray bounce hits the light again, don't add
            // emission
            doMaterialE = (mat.type != Material::Lambert);
#endif
            return matE + lightE + attenuation * Trace(scattered, depth+1, inoutRayCount, stats, state, doMaterialE);
        }
        else
        {
            return matE;
        }
    }
    else
    {
        stats.nodeVisits += hitStats.nodeVisits;
        stats.sphereTests += hitStats.sphereTests;
        // sky
#if DO_MITSUBA_COMPARE
        return float3(0.15f,0.21f,0.3f); // easier compare with Mitsuba's constant environment light
#else
        float3 unitDir = r.dir;
        float t = 0.5f*(unitDir.getY() + 1.0f);
        return ((1.0f-t)*float3(1.0f, 1.0f, 1.0f) + t*float3(0.5f, 0.7f, 1.0f)) * 0.3f;
#endif
    }
}

#if CPU_CAN_DO_THREADS
static enkiTaskScheduler* g_TS;
#endif

void InitializeTest()
{
    EnsureScene();
    #if CPU_CAN_DO_THREADS
    if (!g_TS)
    {
        g_TS = enkiNewTaskScheduler();
        enkiInitTaskScheduler(g_TS);
    }
    #endif
}

void ShutdownTest()
{
    #if CPU_CAN_DO_THREADS
    if (g_TS)
    {
        enkiDeleteTaskScheduler(g_TS);
        g_TS = 0;
    }
    #endif
}

struct JobData
{
    float time;
    int frameCount;
    int screenWidth, screenHeight;
    float* backbuffer;
    Camera* cam;
    // One slot per enki task thread (threadnum). Slots are merged after the
    // task set completes, so worker threads never share a counter.
    CpuTraceStats* threadStats;
    int threadStatsCount;
    unsigned testFlags;
};

static void TraceRowJob(uint32_t start, uint32_t end, uint32_t threadnum, void* data_)
{
    JobData& data = *(JobData*)data_;
    float* backbuffer = data.backbuffer + start * data.screenWidth * 4;
    float invWidth = 1.0f / data.screenWidth;
    float invHeight = 1.0f / data.screenHeight;
    float lerpFac = float(data.frameCount) / float(data.frameCount+1);
    if (data.testFlags & kFlagAnimate)
        lerpFac *= DO_ANIMATE_SMOOTHING;
    if (!(data.testFlags & kFlagProgressive))
        lerpFac = 0;
    CpuTraceStats stats = {0, 0, 0};
    for (uint32_t y = start; y < end; ++y)
    {
        uint32_t state = (y * 9781 + data.frameCount * 6271) | 1;
        for (int x = 0; x < data.screenWidth; ++x)
        {
            float3 col(0, 0, 0);
            for (int s = 0; s < DO_SAMPLES_PER_PIXEL; s++)
            {
                float u = float(x + RandomFloat01(state)) * invWidth;
                float v = float(y + RandomFloat01(state)) * invHeight;
                Ray r = data.cam->GetRay(u, v, state);
                int rayCount = 0;
                col += Trace(r, 0, rayCount, stats, state);
                stats.rayCount += rayCount;
            }
            col *= 1.0f / float(DO_SAMPLES_PER_PIXEL);

            float3 prev(backbuffer[0], backbuffer[1], backbuffer[2]);
            col = prev * lerpFac + col * (1-lerpFac);
            col.store(backbuffer);
            backbuffer += 4;
        }
    }
    // Write only this thread's own slot. enki guarantees a running task thread
    // reports threadnum < GetNumTaskThreads(); any non-task thread (e.g. a
    // caller that also runs part of the range) gets the extra slot, so no two
    // threads ever write the same slot. No shared counter is touched.
    uint32_t slot = threadnum;
    if (slot >= (uint32_t)data.threadStatsCount - 1)
        slot = (uint32_t)data.threadStatsCount - 1;
    data.threadStats[slot].rayCount += stats.rayCount;
    data.threadStats[slot].nodeVisits += stats.nodeVisits;
    data.threadStats[slot].sphereTests += stats.sphereTests;
}

void UpdateTest(float time, int frameCount, int screenWidth, int screenHeight, unsigned testFlags)
{
    EnsureScene();

    if ((testFlags & kFlagAnimate) && s_UseBuiltinScene)
    {
        // Animation applies to the built-in scene only; an externally supplied
        // scene is left untouched.
        s_SceneSpheres[1].center.y = cosf(time) + 1.0f;
        s_SceneSpheres[8].center.z = sinf(time) * 0.3f;
        UpdateDerivedData();
        MarkReferenceDirty();
        // Move the existing tree's bounds/data along with the spheres;
        // topology is left alone.
        s_Bvh.Refit(s_SceneSpheres.data(), (int)s_SceneSpheres.size());
    }

    float3 lookfrom(0, 2, 3);
    float3 lookat(0, 0, 0);
    float distToFocus = 3;
#if DO_MITSUBA_COMPARE
    float aperture = 0.0f;
#else
    float aperture = 0.1f;
#endif
#if DO_BIG_SCENE
    aperture *= 0.2f;
#endif

    RefreshEmissives();

    s_Cam = Camera(lookfrom, lookat, float3(0, 1, 0), 60, float(screenWidth) / float(screenHeight), aperture, distToFocus);
}

void DrawTest(float time, int frameCount, int screenWidth, int screenHeight, float* backbuffer, int& outRayCount, unsigned testFlags)
{
    EnsureScene();
    #if CPU_CAN_DO_THREADS
    // Safe to call without an explicit InitializeTest.
    if (!g_TS)
        InitializeTest();
    #endif

    JobData args;
    args.time = time;
    args.frameCount = frameCount;
    args.screenWidth = screenWidth;
    args.screenHeight = screenHeight;
    args.backbuffer = backbuffer;
    args.cam = &s_Cam;
    args.testFlags = testFlags;

    // Task threads that can run one of these jobs, plus one slot for any
    // other thread that executes the range (including a caller thread with
    // ENKI_NO_THREAD_NUM). Each slot is written by exactly one thread.
    int maxThreads = 1;
    #if CPU_CAN_DO_THREADS
    maxThreads = (int)enkiGetNumTaskThreads(g_TS);
    if (maxThreads < 1)
        maxThreads = 1;
    #endif
    maxThreads += 1; // dedicated slot for non-task threads
    CpuTraceStats threadStats[64];
    static CpuTraceStats* heapStats = 0;
    static int heapStatsCount = 0;
    CpuTraceStats* slots = threadStats;
    const int kInline = (int)(sizeof(threadStats) / sizeof(threadStats[0]));
    if (maxThreads > kInline)
    {
        if (heapStatsCount < maxThreads)
        {
            delete[] heapStats;
            heapStats = new CpuTraceStats[maxThreads];
            heapStatsCount = maxThreads;
        }
        slots = heapStats;
    }
    for (int i = 0; i < maxThreads; ++i)
    {
        slots[i].rayCount = 0;
        slots[i].nodeVisits = 0;
        slots[i].sphereTests = 0;
    }
    args.threadStats = slots;
    args.threadStatsCount = maxThreads;

    #if CPU_CAN_DO_THREADS
    enkiTaskSet* task = enkiCreateTaskSet(g_TS, TraceRowJob);
    bool threaded = true;
    enkiAddTaskSetMinRange(g_TS, task, &args, screenHeight, threaded ? 4 : screenHeight);
    enkiWaitForTaskSet(g_TS, task);
    enkiDeleteTaskSet(g_TS, task);
    #else
    TraceRowJob(0, screenHeight, 0, &args);
    #endif

    CpuTraceStats total = {0, 0, 0};
    for (int i = 0; i < maxThreads; ++i)
    {
        total.rayCount += slots[i].rayCount;
        total.nodeVisits += slots[i].nodeVisits;
        total.sphereTests += slots[i].sphereTests;
    }
    outRayCount = (int)total.rayCount;
    s_LastStats = total;
}

void GetObjectCount(int& outCount, int& outObjectSize, int& outMaterialSize, int& outCamSize)
{
    EnsureScene();
    outCount = (int)s_SceneSpheres.size();
    outObjectSize = sizeof(Sphere);
    outMaterialSize = sizeof(Material);
    outCamSize = sizeof(Camera);
}

void GetSceneDesc(void* outObjects, void* outMaterials, void* outCam, void* outEmissives, int* outEmissiveCount)
{
    EnsureScene();
    memcpy(outObjects, s_SceneSpheres.data(), s_SceneSpheres.size() * sizeof(s_SceneSpheres[0]));
    memcpy(outMaterials, s_SceneMats.data(), s_SceneMats.size() * sizeof(s_SceneMats[0]));
    memcpy(outCam, &s_Cam, sizeof(s_Cam));
    memcpy(outEmissives, s_EmissiveSpheres.data(), s_EmissiveSpheres.size() * sizeof(s_EmissiveSpheres[0]));
    *outEmissiveCount = (int)s_EmissiveSpheres.size();
}
