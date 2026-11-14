#include "Config.h"
#include "Test.h"
#include "Maths.h"
#include "Subsurface.h"
#include <algorithm>
#include <vector>
#include <cmath>
#if CPU_CAN_DO_THREADS
#include "enkiTS/TaskScheduler_c.h"
#endif
#include <atomic>

// 46 spheres (2 emissive) when enabled; 9 spheres (1 emissive) when disabled
#define DO_BIG_SCENE 1

struct Material
{
    enum Type { Lambert, Metal, Dielectric, Subsurface };
    Type type;
    float3pack albedo;
    float3pack emissive;
    float roughness;
    float ri;
};

// CPU scene storage. The default scene (below) is built on first use and can
// be replaced at runtime through SubsurfaceSetScene; all of it is
// renderer-owned. The GPU scene description layout (Sphere/Material/Camera
// blobs handed out by GetSceneDesc) is unchanged.
static std::vector<Sphere> s_Spheres;
static std::vector<Material> s_SphereMats;
// Subsurface parameters, stored separately per sphere index; only meaningful
// for spheres whose material type is Material::Subsurface.
static std::vector<SubsurfaceParams> s_SubsurfaceParams;
static SpheresSoA* s_SpheresSoA = nullptr;
static std::vector<int> s_EmissiveSpheres;

static void BuildDefaultScene()
{
    s_Spheres =
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
    s_SphereMats =
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
    SubsurfaceParams none;
    none.rho = float3(0, 0, 0);
    none.d = float3(1, 1, 1);
    s_SubsurfaceParams.assign(s_Spheres.size(), none);
}

// Rebuilds derived scene data (SoA copy for HitSpheres, emissive list).
static void RebuildSceneData()
{
    delete s_SpheresSoA;
    s_SpheresSoA = new SpheresSoA((int)s_Spheres.size());
    s_EmissiveSpheres.clear();
    for (int i = 0; i < (int)s_Spheres.size(); ++i)
    {
        Sphere& s = s_Spheres[i];
        s.UpdateDerivedData();
        s_SpheresSoA->centerX[i] = s.center.x;
        s_SpheresSoA->centerY[i] = s.center.y;
        s_SpheresSoA->centerZ[i] = s.center.z;
        s_SpheresSoA->sqRadius[i] = s.radius * s.radius;
        s_SpheresSoA->invRadius[i] = s.invRadius;

        // Remember IDs of emissive spheres (light sources)
        const Material& smat = s_SphereMats[i];
        if (smat.emissive.x > 0 || smat.emissive.y > 0 || smat.emissive.z > 0)
        {
            s_EmissiveSpheres.push_back(i);
        }
    }
}

static void EnsureScene()
{
    if (s_Spheres.empty())
    {
        BuildDefaultScene();
        RebuildSceneData();
    }
}

static Camera s_Cam;

// Camera parameters used by UpdateTest; defaults reproduce the built-in view.
static float3 s_CamLookFrom(0, 2, 3);
static float3 s_CamLookAt(0, 0, 0);
static float3 s_CamVUp(0, 1, 0);
static float s_CamVfov = 60.0f;
static float s_CamAperture = 0.1f * 0.2f; // historical default incl. DO_BIG_SCENE factor
static float s_CamFocusDist = 3.0f;

// Sky: default is the original gradient; SubsurfaceSetSky switches to constant.
static bool s_UseConstantSky = false;
static float3 s_ConstantSky(0, 0, 0);

const float kMinT = 0.001f;
const float kMaxT = 1.0e7f;
const int kMaxDepth = 10;


bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outID)
{
    outID = HitSpheres(r, *s_SpheresSoA, tMin, tMax, outHit);
    return outID != -1;
}


static bool Scatter(const Material& mat, int id, const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered, float3& outLightE, int& inoutRayCount, uint32_t& state)
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
            const Material& smat = s_SphereMats[i];
            if (&mat == &smat)
                continue; // skip self
            const Sphere& s = s_Spheres[i];

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
            ++inoutRayCount;
            if (HitWorld(Ray(rec.pos, l), kMinT, kMaxT, lightHit, hitID) && hitID == i)
            {
                float omega = 2 * kPI * (1-cosAMax);

                float3 rdir = r_in.dir;
                AssertUnit(rdir);
                float3 nl = dot(rec.normal, rdir) < 0 ? rec.normal : -rec.normal;
                float3 smatEmissive = smat.emissive.toFloat3();
                outLightE += (matAlbedo * smatEmissive) * (std::max(0.0f, dot(l, nl)) * omega / kPI);
            }
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
    else if (mat.type == Material::Subsurface)
    {
        // Non-local diffusion: relocate the exit point to xi on the SAME
        // sphere (projected-disk sampling), then estimate E(xi) with a cosine
        // continuation and direct sphere-light sampling from xi. The jump
        // counts as one surface event (this Scatter call); the continuation
        // stays bounded by kMaxDepth as usual.
        const SubsurfaceParams& sp = s_SubsurfaceParams[id];
        const Sphere& self = s_Spheres[id];
        float3 selfC = self.center.toFloat3();
        SubsurfaceSample smp = SubsurfaceSampleRelocation(selfC, self.radius, id, sp, rec.pos, rec.normal, state);
        if (!smp.valid)
        {
            attenuation = float3(0,0,0);
            return false; // zero contribution, no resampling
        }
        float pdf = SubsurfaceRelocationPdf(selfC, self.radius, sp, rec.pos, rec.normal, smp.pos);
        float3 w = SubsurfaceWeight(sp, rec.pos, smp.pos, pdf); // 0 when pdf <= 0
        if (!(pdf > 0.0f))
        {
            attenuation = float3(0,0,0);
            return false;
        }
        attenuation = w;

        // cosine continuation at xi on the outward hemisphere
        float3 target = smp.pos + smp.normal + RandomUnitVector(state);
        scattered = Ray(smp.pos, normalize(target - smp.pos));

        // direct sphere-light sampling from xi with the same non-local
        // weight (the old albedo is NOT applied again). The shadow ray is
        // finite in effect: it only counts when the first thing hit along l
        // is the light sphere itself.
#if DO_LIGHT_SAMPLING
        for (int j = 0; j < (int)s_EmissiveSpheres.size(); ++j)
        {
            int i = s_EmissiveSpheres[j];
            if (i == id)
                continue; // skip self
            const Material& smat = s_SphereMats[i];
            const Sphere& s = s_Spheres[i];

            // create a random direction towards sphere
            // coord system for sampling: sw, su, sv
            float3 sc = s.center.toFloat3();
            float3 sw = normalize(sc - smp.pos);
            float3 su = normalize(cross(fabs(sw.getX())>0.01f ? float3(0,1,0):float3(1,0,0), sw));
            float3 sv = cross(sw, su);
            // sample sphere by solid angle
            float cosAMax = sqrtf(1.0f - s.radius*s.radius / sqLength(smp.pos-sc));
            float eps1 = RandomFloat01(state), eps2 = RandomFloat01(state);
            float cosA = 1.0f - eps1 + eps1 * cosAMax;
            float sinA = sqrtf(1.0f - cosA*cosA);
            float phi = 2 * kPI * eps2;
            float3 l = su * (cosf(phi) * sinA) + sv * (sinf(phi) * sinA) + sw * cosA;

            // shoot shadow ray
            Hit lightHit;
            int hitID;
            ++inoutRayCount;
            if (HitWorld(Ray(smp.pos, l), kMinT, kMaxT, lightHit, hitID) && hitID == i)
            {
                float omega = 2 * kPI * (1-cosAMax);
                float3 smatEmissive = smat.emissive.toFloat3();
                outLightE += (w * smatEmissive) * (std::max(0.0f, dot(l, smp.normal)) * omega / kPI);
            }
        }
#endif
        return true;
    }
    else
    {
        attenuation = float3(1,0,1);
        return false;
    }
    return true;
}

static float3 Trace(const Ray& r, int depth, int& inoutRayCount, uint32_t& state, bool doMaterialE = true)
{
    Hit rec;
    int id = 0;
    ++inoutRayCount;
    if (HitWorld(r, kMinT, kMaxT, rec, id))
    {
        Ray scattered;
        float3 attenuation;
        float3 lightE;
        const Material& mat = s_SphereMats[id];
        float3 matE = mat.emissive.toFloat3();
        if (depth < kMaxDepth && Scatter(mat, id, r, rec, attenuation, scattered, lightE, inoutRayCount, state))
        {
#if DO_LIGHT_SAMPLING
            if (!doMaterialE) matE = float3(0,0,0); // don't add material emission if told so
            // for Lambert and subsurface materials, we just did explicit light (emissive)
            // sampling and already accounted for their contribution, so if the next ray
            // bounce hits the light again, don't add emission. Emission attribution
            // follows the actual scatter event; specular paths keep the light they see.
            doMaterialE = (mat.type != Material::Lambert && mat.type != Material::Subsurface);
#endif
            return matE + lightE + attenuation * Trace(scattered, depth+1, inoutRayCount, state, doMaterialE);
        }
        else
        {
            return matE;
        }
    }
    else
    {
        // sky
        if (s_UseConstantSky)
            return s_ConstantSky;
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
    g_TS = enkiNewTaskScheduler();
    enkiInitTaskScheduler(g_TS);
    #endif
}

void ShutdownTest()
{
    #if CPU_CAN_DO_THREADS
    enkiDeleteTaskScheduler(g_TS);
    #endif
}

struct JobData
{
    float time;
    int frameCount;
    int screenWidth, screenHeight;
    float* backbuffer;
    Camera* cam;
    std::atomic<int> rayCount;
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
    int rayCount = 0;
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
                col += Trace(r, 0, rayCount, state);
            }
            col *= 1.0f / float(DO_SAMPLES_PER_PIXEL);

            float3 prev(backbuffer[0], backbuffer[1], backbuffer[2]);
            col = prev * lerpFac + col * (1-lerpFac);
            col.store(backbuffer);
            backbuffer += 4;
        }
    }
    data.rayCount += rayCount;
}

void UpdateTest(float time, int frameCount, int screenWidth, int screenHeight, unsigned testFlags)
{
    EnsureScene();
    if ((testFlags & kFlagAnimate) && s_Spheres.size() > 8)
    {
        s_Spheres[1].center.y = cosf(time) + 1.0f;
        s_Spheres[8].center.z = sinf(time) * 0.3f;
    }
    RebuildSceneData();

    s_Cam = Camera(s_CamLookFrom, s_CamLookAt, s_CamVUp, s_CamVfov, float(screenWidth) / float(screenHeight), s_CamAperture, s_CamFocusDist);
}

void DrawTest(float time, int frameCount, int screenWidth, int screenHeight, float* backbuffer, int& outRayCount, unsigned testFlags)
{
    JobData args;
    args.time = time;
    args.frameCount = frameCount;
    args.screenWidth = screenWidth;
    args.screenHeight = screenHeight;
    args.backbuffer = backbuffer;
    args.cam = &s_Cam;
    args.testFlags = testFlags;
    args.rayCount = 0;

    #if CPU_CAN_DO_THREADS
    enkiTaskSet* task = enkiCreateTaskSet(g_TS, TraceRowJob);
    bool threaded = true;
    enkiAddTaskSetMinRange(g_TS, task, &args, screenHeight, threaded ? 4 : screenHeight);
    enkiWaitForTaskSet(g_TS, task);
    enkiDeleteTaskSet(g_TS, task);
    #else
    TraceRowJob(0, screenHeight, 0, &args);
    #endif

    outRayCount = args.rayCount;
}

void GetObjectCount(int& outCount, int& outObjectSize, int& outMaterialSize, int& outCamSize)
{
    EnsureScene();
    outCount = (int)s_Spheres.size();
    outObjectSize = sizeof(Sphere);
    outMaterialSize = sizeof(Material);
    outCamSize = sizeof(Camera);
}

void GetSceneDesc(void* outObjects, void* outMaterials, void* outCam, void* outEmissives, int* outEmissiveCount)
{
    EnsureScene();
    memcpy(outObjects, s_Spheres.data(), s_Spheres.size() * sizeof(Sphere));
    memcpy(outMaterials, s_SphereMats.data(), s_SphereMats.size() * sizeof(Material));
    memcpy(outCam, &s_Cam, sizeof(s_Cam));
    memcpy(outEmissives, s_EmissiveSpheres.data(), s_EmissiveSpheres.size() * sizeof(int));
    *outEmissiveCount = (int)s_EmissiveSpheres.size();
}


// ---- CPU scene control, constant sky and single-ray radiance entry ----
// (declared in Subsurface.h; scene data is copied into renderer-owned storage)

void SubsurfaceResetScene()
{
    BuildDefaultScene();
    RebuildSceneData();
}

static bool Finite3(const float* v)
{
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

bool SubsurfaceSetScene(const SubsurfaceSceneSphere* spheres,
                        const SubsurfaceSceneMaterial* materials,
                        const SubsurfaceParams* subsurfaceParams,
                        int count)
{
    if (!spheres || !materials || count < 1 || count > 256)
        return false;
    // Validate everything BEFORE touching the renderer-owned scene.
    for (int i = 0; i < count; ++i)
    {
        const SubsurfaceSceneSphere& s = spheres[i];
        const SubsurfaceSceneMaterial& m = materials[i];
        if (!Finite3(s.center) || !std::isfinite(s.radius) || s.radius <= 0.0f)
            return false;
        if (m.type < 0 || m.type > 3)
            return false;
        if (!Finite3(m.albedo) || !Finite3(m.emissive))
            return false;
        if (!std::isfinite(m.roughness) || m.roughness < 0.0f || !std::isfinite(m.ri))
            return false;
        if (m.type == 3) // Subsurface
        {
            if (!subsurfaceParams || !SubsurfaceParamsValid(subsurfaceParams[i]))
                return false;
        }
    }
    // Commit: copy into renderer-owned storage.
    s_Spheres.resize(count);
    s_SphereMats.resize(count);
    s_SubsurfaceParams.resize(count);
    for (int i = 0; i < count; ++i)
    {
        s_Spheres[i] = Sphere(float3(spheres[i].center[0], spheres[i].center[1], spheres[i].center[2]), spheres[i].radius);
        Material& m = s_SphereMats[i];
        m.type = (Material::Type)materials[i].type;
        m.albedo = float3pack(materials[i].albedo[0], materials[i].albedo[1], materials[i].albedo[2]);
        m.emissive = float3pack(materials[i].emissive[0], materials[i].emissive[1], materials[i].emissive[2]);
        m.roughness = materials[i].roughness;
        m.ri = materials[i].ri;
        if (materials[i].type == 3)
            s_SubsurfaceParams[i] = subsurfaceParams[i];
        else
        {
            s_SubsurfaceParams[i].rho = float3(0, 0, 0);
            s_SubsurfaceParams[i].d = float3(1, 1, 1);
        }
    }
    RebuildSceneData();
    return true;
}

void SubsurfaceSetSky(float r, float g, float b)
{
    s_UseConstantSky = true;
    s_ConstantSky = float3(r, g, b);
}

void SubsurfaceResetSky()
{
    s_UseConstantSky = false;
}

void SubsurfaceSetCamera(const float* lookFrom, const float* lookAt, const float* vUp,
                         float vfovDeg, float aperture, float focusDist)
{
    s_CamLookFrom = float3(lookFrom[0], lookFrom[1], lookFrom[2]);
    s_CamLookAt = float3(lookAt[0], lookAt[1], lookAt[2]);
    s_CamVUp = float3(vUp[0], vUp[1], vUp[2]);
    s_CamVfov = vfovDeg;
    s_CamAperture = aperture;
    s_CamFocusDist = focusDist;
}

float3 SubsurfaceTraceRay(const float3& orig, const float3& dir, uint32_t& rngState, int& inoutRayCount)
{
    EnsureScene();
    return Trace(Ray(orig, dir), 0, inoutRayCount, rngState);
}
