#include "Config.h"
#include "Test.h"
#include "Maths.h"
#include "Medium.h"
#include <algorithm>
#include <vector>
#if CPU_CAN_DO_THREADS
#include "enkiTS/TaskScheduler_c.h"
#endif
#include <atomic>

// 46 spheres (2 emissive) when the big scene is enabled; 9 spheres (1 emissive) otherwise.
#define DO_BIG_SCENE 1

static std::vector<Sphere> MakeDefaultSpheres()
{
    std::vector<Sphere> spheres =
    {
        Sphere(float3(0,-100.5,-1), 100),
        Sphere(float3(2,0,-1), 0.5f),
        Sphere(float3(0,0,-1), 0.5f),
        Sphere(float3(-2,0,-1), 0.5f),
        Sphere(float3(2,0,1), 0.5f),
        Sphere(float3(0,0,1), 0.5f),
        Sphere(float3(-2,0,1), 0.5f),
        Sphere(float3(0.5f,1,0.5f), 0.5f),
        Sphere(float3(-1.5f,1.5f,0.f), 0.3f),
    };
#if DO_BIG_SCENE
    const float xs[] = {4,3,2,1,0,-1,-2,-3,-4};
    for (int row = 0; row < 4; ++row)
        for (int x = 0; x < 9; ++x)
            spheres.push_back(Sphere(float3(float(xs[x]), 0, float(-3 - row)), 0.5f));
    spheres.push_back(Sphere(float3(1.5f,1.5f,-2), 0.3f));
#endif
    return spheres;
}

static std::vector<Sphere> s_Spheres = MakeDefaultSpheres();
static int kSphereCount = (int)s_Spheres.size();

static SpheresSoA* s_SpheresSoA = nullptr;

struct Material
{
    enum Type { Lambert, Metal, Dielectric };
    Type type;
    float3pack albedo;
    float3pack emissive;
    float roughness;
    float ri;
};

static std::vector<Material> MakeDefaultMaterials()
{
    std::vector<Material> mats =
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
    };
#if DO_BIG_SCENE
    for (int i = 1; i <= 9; ++i)
    {
        float g = i / 10.0f;
        mats.push_back({ Material::Lambert, float3(g, g, g), float3(0,0,0), 0, 0 });
    }
    for (int i = 1; i <= 9; ++i)
    {
        float g = i / 10.0f;
        mats.push_back({ Material::Metal, float3(g, g, g), float3(0,0,0), 0, 0 });
    }
    const float3 metalAlbedos[] =
    {
        float3(0.8f,0.1f,0.1f), float3(0.8f,0.5f,0.1f), float3(0.8f,0.8f,0.1f),
        float3(0.4f,0.8f,0.1f), float3(0.1f,0.8f,0.1f), float3(0.1f,0.8f,0.5f),
        float3(0.1f,0.8f,0.8f), float3(0.1f,0.1f,0.8f), float3(0.5f,0.1f,0.8f),
    };
    for (int i = 0; i < 9; ++i)
        mats.push_back({ Material::Metal, metalAlbedos[i], float3(0,0,0), 0, 0 });
    const float3 lambertAlbedos[] =
    {
        float3(0.8f,0.1f,0.1f), float3(0.8f,0.5f,0.1f), float3(0.8f,0.8f,0.1f),
        float3(0.4f,0.8f,0.1f), float3(0.1f,0.8f,0.1f), float3(0.1f,0.8f,0.5f),
        float3(0.1f,0.8f,0.8f), float3(0.1f,0.1f,0.8f), float3(0.5f,0.1f,0.8f),
    };
    for (int i = 0; i < 8; ++i)
        mats.push_back({ Material::Lambert, lambertAlbedos[i], float3(0,0,0), 0, 0 });
    mats.push_back({ Material::Metal, float3(0.5f, 0.1f, 0.8f), float3(0,0,0), 0, 0 });
    mats.push_back({ Material::Lambert, float3(0.1f, 0.2f, 0.5f), float3(3,10,20), 0, 0 });
#endif
    return mats;
}

static std::vector<Material> s_SphereMats = MakeDefaultMaterials();

static std::vector<int> s_EmissiveSpheres;
static int s_EmissiveSphereCount;

static Camera s_Cam;

const float kMinT = 0.001f;
const float kMaxT = 1.0e7f;
const int kMaxDepth = 10;


bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outID)
{
    outID = HitSpheres(r, *s_SpheresSoA, tMin, tMax, outHit);
    return outID != -1;
}


static bool Scatter(const Material& mat, const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered, float3& outLightE, int& inoutRayCount, uint32_t& state, const std::vector<int>* mediumActive = nullptr)
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
        for (int j = 0; j < s_EmissiveSphereCount; ++j)
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
                float3 lightContrib = (matAlbedo * smatEmissive) * (std::max(0.0f, dot(l, nl)) * omega / kPI);
                // In priority-medium mode an unoccluded light segment still
                // crosses media; glass boundaries keep occluding as before.
                if (mediumActive)
                    lightContrib = lightContrib * medium::ShadowAttenuation(rec.pos, l, lightHit.t, *mediumActive);
                outLightE += lightContrib;
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
    else
    {
        attenuation = float3(1,0,1);
        return false;
    }
    return true;
}

static float3 Trace(const Ray& r, int depth, int& inoutRayCount, uint32_t& state, bool doMaterialE = true)
{
    if (medium::MediumEnabled())
    {
        (void)doMaterialE; // the medium tracer carries the same emission de-duplication rules
        return medium::TraceMedium(r, depth, inoutRayCount, state);
    }
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
        if (depth < kMaxDepth && Scatter(mat, r, rec, attenuation, scattered, lightE, inoutRayCount, state))
        {
#if DO_LIGHT_SAMPLING
            if (!doMaterialE) matE = float3(0,0,0); // don't add material emission if told so
            // dor Lambert materials, we just did explicit light (emissive) sampling and already
            // for their contribution, so if next ray bounce hits the light again, don't add
            // emission
            doMaterialE = (mat.type != Material::Lambert);
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
    s_SpheresSoA = new SpheresSoA(kSphereCount);
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
    delete s_SpheresSoA;
    s_SpheresSoA = nullptr;
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
    if (testFlags & kFlagAnimate)
    {
        s_Spheres[1].center.y = cosf(time) + 1.0f;
        s_Spheres[8].center.z = sinf(time) * 0.3f;
    }
    float3 lookfrom(0, 2, 3);
    float3 lookat(0, 0, 0);
    float distToFocus = 3;
#if DO_MITSUBA_COMPARE
    float aperture = 0.0f;
#else
    float aperture = 0.1f;
#endif
    if (kSphereCount > 9)
        aperture *= 0.2f;

    s_EmissiveSphereCount = 0;
    s_EmissiveSpheres.clear();
    for (int i = 0; i < kSphereCount; ++i)
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
            s_EmissiveSphereCount++;
        }
    }

    s_Cam = Camera(lookfrom, lookat, float3(0, 1, 0), 60, float(screenWidth) / float(screenHeight), aperture, distToFocus);
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
    outCount = kSphereCount;
    outObjectSize = sizeof(Sphere);
    outMaterialSize = sizeof(Material);
    outCamSize = sizeof(Camera);
}

void GetSceneDesc(void* outObjects, void* outMaterials, void* outCam, void* outEmissives, int* outEmissiveCount)
{
    memcpy(outObjects, s_Spheres.data(), kSphereCount * sizeof(s_Spheres[0]));
    memcpy(outMaterials, s_SphereMats.data(), kSphereCount * sizeof(s_SphereMats[0]));
    memcpy(outCam, &s_Cam, sizeof(s_Cam));
    memcpy(outEmissives, s_EmissiveSpheres.data(), s_EmissiveSphereCount * sizeof(s_EmissiveSpheres[0]));
    *outEmissiveCount = s_EmissiveSphereCount;
}

// ---------------------------------------------------------------------------
// Priority-medium support surface (consumed by Cpp/Source/Medium.cpp).
// The GPU layout exposed by GetSceneDesc above is unchanged; these are
// CPU-only helpers for swapping in the small-ball medium scene.
// ---------------------------------------------------------------------------

const Sphere* ProductionSpheres()
{
    return s_Spheres.data();
}

int ProductionActiveCount()
{
    return kSphereCount;
}

int ProductionMaterialType(int id)
{
    return (int)s_SphereMats[id].type;
}

float3 ProductionMaterialEmissive(int id)
{
    return s_SphereMats[id].emissive.toFloat3();
}

bool ProductionLegacyScatter(int id, const Ray& rIn, const Hit& rec, float3& attenuation,
                              Ray& scattered, float3& outLightE, int& inoutRayCount,
                              uint32_t& state, const std::vector<int>& mediumActive)
{
    const std::vector<int>* activePtr = medium::MediumEnabled() ? &mediumActive : nullptr;
    return Scatter(s_SphereMats[id], rIn, rec, attenuation, scattered, outLightE,
                   inoutRayCount, state, activePtr);
}

bool ProductionInstallScene(const std::vector<Sphere>& spheres,
                            const std::vector<medium::SceneMaterialDesc>& mats,
                            const medium::MediumConfig& cfg)
{
    if (spheres.size() != mats.size())
        return false;
    if (!medium::ValidateConfig(cfg, (int)spheres.size()))
        return false; // reject before touching the scene

    std::vector<Sphere> newSpheres = spheres;
    std::vector<Material> newMats;
    newMats.reserve(mats.size());
    for (const medium::SceneMaterialDesc& d : mats)
    {
        Material m;
        m.type = (Material::Type)d.type;
        m.albedo = d.albedo;
        m.emissive = d.emissive;
        m.roughness = d.roughness;
        m.ri = d.ri;
        newMats.push_back(m);
    }

    delete s_SpheresSoA;
    s_Spheres = std::move(newSpheres);
    s_SphereMats = std::move(newMats);
    kSphereCount = (int)s_Spheres.size();
    s_SpheresSoA = new SpheresSoA(kSphereCount);
    if (!medium::SetMediumConfig(cfg))
    {
        // Validation already passed; keep the mode consistent if this ever fails.
        return false;
    }
    return true;
}

void ProductionResetScene()
{
    medium::ClearMediumConfig();
    delete s_SpheresSoA;
    s_Spheres = MakeDefaultSpheres();
    s_SphereMats = MakeDefaultMaterials();
    kSphereCount = (int)s_Spheres.size();
    s_SpheresSoA = new SpheresSoA(kSphereCount);
}
