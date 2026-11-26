#include "Config.h"
#include "Test.h"
#include "Maths.h"
#include <algorithm>
#if CPU_CAN_DO_THREADS
#include "enkiTS/TaskScheduler_c.h"
#endif
#include <atomic>

// 46 spheres (2 emissive) when enabled; 9 spheres (1 emissive) when disabled
#define DO_BIG_SCENE 1

static Sphere s_Spheres[] =
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
const int kSphereCount = sizeof(s_Spheres) / sizeof(s_Spheres[0]);

static SpheresSoA s_SpheresSoA(kSphereCount);

struct Material
{
    // NOTE: CoatedLambert is appended on purpose so the numeric values of the
    // pre-existing Lambert/Metal/Dielectric types (and the Material field layout)
    // stay unchanged. It is only implemented for the CPU path.
    enum Type { Lambert, Metal, Dielectric, CoatedLambert };
    Type type;
    float3pack albedo;
    float3pack emissive;
    float roughness;
    float ri;
};

// What kind of lobe a Scatter call sampled. This drives whether the emission of
// the next hit has already been accounted for (diffuse + next-event estimation)
// or must be accumulated (perfect specular reflection).
enum ScatterEvent
{
    kScatterNone = 0,
    kScatterDiffuse = 1,
    kScatterSpecular = 2
};

// ---- CoatedLambert (smooth clear coat over a Lambert base) -----------------
// Thin-coat model: no refraction displacement and no internal multi-bounce.
// ri is the coat index of refraction (ri >= 1); albedo is the base colour.

static inline float CoatF0(float ri)
{
    float r = (ri - 1.0f) / (ri + 1.0f);
    return r * r;
}

// Schlick Fresnel. The ri == 1 case is an explicit, global exception: F is
// identically zero so the material collapses to a plain Lambert diffuse.
// The same exception must be used by path sampling, sphere-light direct
// sampling and any independent integration.
static inline float CoatF(float cosine, float ri)
{
    if (ri <= 1.0f) // explicit ri == 1 exception (also guards ri < 1)
        return 0.0f;
    float c = cosine;
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;
    float F0 = CoatF0(ri);
    float t = 1.0f - c;
    float t5 = t * t * t * t * t;
    return F0 + (1.0f - F0) * t5;
}

// Orient the shading normal so it faces the direction the ray came from
// (wo = -r_in.dir). Afterwards dot(n, wo) >= 0.
static inline float3 CoatOrientedNormal(const float3& geometricNormal, const Ray& r_in)
{
    float3 wo = -r_in.dir;
    return dot(geometricNormal, wo) < 0.0f ? -geometricNormal : geometricNormal;
}

// Diffuse BSDF: albedo/pi * (1-F(N.wo)) * (1-F(N.wi)), zero below the hemisphere.
static inline float3 CoatDiffuseBSDF(const float3& albedo, float ri,
                                     const float3& n, const float3& wo, const float3& wi)
{
    float NoL = dot(n, wi);
    if (NoL <= 0.0f)
        return float3(0, 0, 0);
    float NoV = dot(n, wo);
    if (NoV < 0.0f) NoV = 0.0f;
    float Fo = CoatF(NoV, ri);
    float Fi = CoatF(NoL, ri);
    return albedo * ((1.0f - Fo) * (1.0f - Fi) / kPI);
}

// Importance-sample one continuation event for the coated material.
//
// The specular lobe (weight Fo = F(N.wo)) is sampled with probability Fo and
// the diffuse lobe with probability 1-Fo (the code base's Fresnel-as-probability
// idiom, cf. Dielectric). Returned attenuation already contains the division by
// the event/pdf probability, so averaging it under constant incoming radiance
// reproduces the hemispherical integral of the model exactly:
//   specular -> exact reflect() direction, attenuation 1
//   diffuse  -> cosine-weighted hemisphere, attenuation albedo * (1 - F(N.wi))
//               (the outgoing (1-Fo) cancels with the 1-Fo event probability)
// ri == 1 takes the explicit no-coat path: always diffuse, no extra RNG draw.
static void CoatSampleEvent(const float3& albedo, float ri,
                            const float3& p, const float3& n, const float3& wo,
                            uint32_t& state,
                            Ray& outScattered, float3& outAttenuation,
                            int& outEvent)
{
    float NoV = dot(n, wo);
    if (NoV < 0.0f) NoV = 0.0f;
    if (NoV > 1.0f) NoV = 1.0f;
    float Fo = CoatF(NoV, ri); // is exactly 0 for ri == 1

    if (ri > 1.0f && RandomFloat01(state) < Fo)
    {
        // Perfect mirror reflection, exact direction.
        float3 wi = reflect(-wo, n); // == reflect(r_in.dir, n)
        outScattered = Ray(p, normalize(wi));
        outAttenuation = float3(1, 1, 1); // Fo / Fo
        outEvent = kScatterSpecular;
        return;
    }

    // Cosine-weighted hemisphere, same construction as Lambert.
    float3 dir = normalize(n + RandomUnitVector(state));
    float NoL = dot(n, dir);
    if (NoL < 0.0f) NoL = 0.0f;
    float Fi = CoatF(NoL, ri);
    outScattered = Ray(p, dir);
    outAttenuation = albedo * (1.0f - Fi);
    outEvent = kScatterDiffuse;
}

static Material s_SphereMats[kSphereCount] =
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

static int s_EmissiveSpheres[kSphereCount];
static int s_EmissiveSphereCount;

static Camera s_Cam;

const float kMinT = 0.001f;
const float kMaxT = 1.0e7f;
const int kMaxDepth = 10;


bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outID)
{
    outID = HitSpheres(r, s_SpheresSoA, tMin, tMax, outHit);
    return outID != -1;
}

// Next-event estimation for the CoatedLambert diffuse lobe against all sphere
// lights. Mirrors the Lambert sphere-cone sampler but weights the contribution
// by (1-F(N.wo))*(1-F(N.wi)). ri == 1 makes both Fresnel factors exactly 1.
// `selfMat` may be null; otherwise the matching emissive sphere is skipped.
static float3 CoatDirectLightE(const float3& albedo, float ri,
                               const float3& p, const float3& n, const float3& wo,
                               const Material* selfMat,
                               uint32_t& state, int& inoutRayCount)
{
    float3 lightE(0, 0, 0);
#if DO_LIGHT_SAMPLING
    float NoV = dot(n, wo);
    if (NoV < 0.0f) NoV = 0.0f;
    float oneMinusFo = 1.0f - CoatF(NoV, ri); // constant for this hit
    for (int j = 0; j < s_EmissiveSphereCount; ++j)
    {
        int i = s_EmissiveSpheres[j];
        const Material& smat = s_SphereMats[i];
        if (selfMat == &smat)
            continue; // skip self
        const Sphere& s = s_Spheres[i];

        // create a random direction towards sphere
        float3 sc = s.center.toFloat3();
        float3 sw = normalize(sc - p);
        float3 su = normalize(cross(fabsf(sw.getX()) > 0.01f ? float3(0, 1, 0) : float3(1, 0, 0), sw));
        float3 sv = cross(sw, su);
        // sample sphere by solid angle
        float cosAMax = sqrtf(1.0f - s.radius*s.radius / sqLength(p - sc));
        float eps1 = RandomFloat01(state), eps2 = RandomFloat01(state);
        float cosA = 1.0f - eps1 + eps1 * cosAMax;
        float sinA = sqrtf(fmaxf(0.0f, 1.0f - cosA*cosA));
        float phi = 2 * kPI * eps2;
        float3 l = su * (cosf(phi) * sinA) + sv * (sinf(phi) * sinA) + sw * cosA;

        // shoot shadow ray
        Hit lightHit;
        int hitID;
        ++inoutRayCount;
        if (HitWorld(Ray(p, l), kMinT, kMaxT, lightHit, hitID) && hitID == i)
        {
            float cosL = dot(n, l);
            if (cosL <= 0.0f)
                continue; // diffuse lobe only contributes above the normal
            if (cosL > 1.0f) cosL = 1.0f;
            float omega = 2 * kPI * (1 - cosAMax);
            float Fi = CoatF(cosL, ri);
            float3 smatEmissive = smat.emissive.toFloat3();
            // albedo/pi * (1-Fo)(1-Fi) * Le * cosL * omega
            lightE += albedo * smatEmissive *
                      (oneMinusFo * (1.0f - Fi) * cosL * omega / kPI);
        }
    }
#endif
    return lightE;
}


static bool Scatter(const Material& mat, const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered, float3& outLightE, int& inoutRayCount, uint32_t& state, int* outEvent = 0)
{
    outLightE = float3(0,0,0);
    if (outEvent) *outEvent = kScatterNone;
    if (mat.type == Material::Lambert)
    {
        if (outEvent) *outEvent = kScatterDiffuse;
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
                outLightE += (matAlbedo * smatEmissive) * (std::max(0.0f, dot(l, nl)) * omega / kPI);
            }
        }
#endif
        return true;
    }
    else if (mat.type == Material::Metal)
    {
        if (outEvent) *outEvent = kScatterSpecular;
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
        // Both reflection and refraction keep the seen emission (old rule was
        // type != Lambert), so any non-diffuse event preserves the next light.
        if (outEvent) *outEvent = kScatterSpecular;
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
    else if (mat.type == Material::CoatedLambert)
    {
        float3 matAlbedo = mat.albedo.toFloat3();
        float ri = mat.ri;
        // Orient N to face the view direction wo = -r_in.dir (explicit model rule).
        float3 n = CoatOrientedNormal(rec.normal, r_in);
        float3 wo = -r_in.dir;

        int eventKind = kScatterNone;
        CoatSampleEvent(matAlbedo, ri, rec.pos, n, wo, state,
                        scattered, attenuation, eventKind);
        if (outEvent) *outEvent = eventKind;

        // Diffuse lobe gets explicit sphere-light NEE. It is evaluated on every
        // hit (independent of which continuation event was sampled) and carries
        // the full (1-Fo)(1-Fi) weighting.
        outLightE = CoatDirectLightE(matAlbedo, ri, rec.pos, n, wo, &mat,
                                    state, inoutRayCount);
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
        int scatterEvent = kScatterNone;
        const Material& mat = s_SphereMats[id];
        float3 matE = mat.emissive.toFloat3();
        if (depth < kMaxDepth && Scatter(mat, r, rec, attenuation, scattered, lightE, inoutRayCount, state, &scatterEvent))
        {
#if DO_LIGHT_SAMPLING
            if (!doMaterialE) matE = float3(0,0,0); // don't add material emission if told so
            // Whether the next hit's emission is accumulated is decided by the
            // sampled event, not just the material type:
            //  * diffuse events (Lambert, or the diffuse base of CoatedLambert)
            //    already did explicit sphere-light sampling, so a light hit right
            //    after must not add its emission again;
            //  * specular/transmission events (Metal, Dielectric, and the clear-coat
            //    mirror branch) must keep emitters seen in the scattered direction.
            // The fallback preserves the old type-only rule if no event was reported.
            if (scatterEvent == kScatterDiffuse)
                doMaterialE = false;
            else if (scatterEvent == kScatterSpecular)
                doMaterialE = true;
            else
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
#if DO_BIG_SCENE
    aperture *= 0.2f;
#endif

    s_EmissiveSphereCount = 0;
    for (int i = 0; i < kSphereCount; ++i)
    {
        Sphere& s = s_Spheres[i];
        s.UpdateDerivedData();
        s_SpheresSoA.centerX[i] = s.center.x;
        s_SpheresSoA.centerY[i] = s.center.y;
        s_SpheresSoA.centerZ[i] = s.center.z;
        s_SpheresSoA.sqRadius[i] = s.radius * s.radius;
        s_SpheresSoA.invRadius[i] = s.invRadius;

        // Remember IDs of emissive spheres (light sources)
        const Material& smat = s_SphereMats[i];
        if (smat.emissive.x > 0 || smat.emissive.y > 0 || smat.emissive.z > 0)
        {
            s_EmissiveSpheres[s_EmissiveSphereCount] = i;
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
    memcpy(outObjects, s_Spheres, kSphereCount * sizeof(s_Spheres[0]));
    memcpy(outMaterials, s_SphereMats, kSphereCount * sizeof(s_SphereMats[0]));
    memcpy(outCam, &s_Cam, sizeof(s_Cam));
    memcpy(outEmissives, s_EmissiveSpheres, s_EmissiveSphereCount * sizeof(s_EmissiveSpheres[0]));
    *outEmissiveCount = s_EmissiveSphereCount;
}
