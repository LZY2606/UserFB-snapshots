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
    enum Type { Lambert, Metal, Dielectric, CoatedLambert };
    Type type;
    float3pack albedo;
    float3pack emissive;
    float roughness;
    float ri;
};

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

// Coated Lambert: a thin, smooth clear coat over a diffuse base.  The normal
// is always oriented towards the side the ray came from, so all cosines below
// are positive for the visible side of the surface.
static float3 OrientNormalTowardRay(float3 normal, float3 rayDir)
{
    return dot(normal, rayDir) < 0 ? normal : -normal;
}

// Fresnel reflectance of the coat for a cosine measured against the
// observer-facing normal.  ri == 1 is the explicit no-coat exception.
static float CoatedFresnel(float cosTheta, float ri)
{
    if (ri <= 1.0f)
        return 0.0f;
    float c = std::min(std::max(cosTheta, 0.0f), 1.0f);
    float f0 = (ri - 1.0f) / (ri + 1.0f);
    f0 *= f0;
    float m = 1.0f - c;
    float m2 = m * m;
    return f0 + (1.0f - f0) * (m2 * m2 * m);
}

// Diffuse part of the BSDF.  It is only non-zero above the normal.
static float3 CoatedDiffuseBSDF(float3 albedo, float ri, float cosWo, float cosWi)
{
    if (cosWi <= 0.0f)
        return float3(0, 0, 0);
    float oneMinusFwo = 1.0f - CoatedFresnel(cosWo, ri);
    float oneMinusFwi = 1.0f - CoatedFresnel(cosWi, ri);
    return albedo * (oneMinusFwo * oneMinusFwi / kPI);
}

// Cosine-weighted direction around an already oriented normal (z >= 0).
static float3 CosineHemisphere(float3 n, uint32_t& state)
{
    float u1 = RandomFloat01(state);
    float u2 = RandomFloat01(state);
    float r = sqrtf(u1);
    float phi = 2.0f * kPI * u2;
    float x = r * cosf(phi);
    float y = r * sinf(phi);
    float z = sqrtf(std::max(0.0f, 1.0f - u1));
    float3 helper = fabsf(n.getX()) > 0.1f ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(helper, n));
    float3 bitangent = cross(n, tangent);
    return normalize(tangent * x + bitangent * y + n * z);
}


// Original Lambert event, shared by Material::Lambert and the ri == 1
// degenerate case of CoatedLambert.  Keeping one code path preserves the
// legacy Lambert calculation and random stream exactly.
static bool ScatterLambert(const Material& mat, const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered, float3& outLightE, int& inoutRayCount, uint32_t& state, bool* outSkipNextMaterialEmission)
{
    // random point on unit sphere that is tangent to the hit point
    float3 target = rec.pos + rec.normal + RandomUnitVector(state);
    scattered = Ray(rec.pos, normalize(target - rec.pos));
    float3 matAlbedo = mat.albedo.toFloat3();
    attenuation = matAlbedo;

    // sample lights
#if DO_LIGHT_SAMPLING
    if (outSkipNextMaterialEmission)
        *outSkipNextMaterialEmission = true;
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


static bool Scatter(const Material& mat, const Ray& r_in, const Hit& rec, float3& attenuation, Ray& scattered, float3& outLightE, int& inoutRayCount, uint32_t& state, bool* outSkipNextMaterialEmission = nullptr)
{
    outLightE = float3(0,0,0);
    if (outSkipNextMaterialEmission)
        *outSkipNextMaterialEmission = false;
    if (mat.type == Material::Lambert)
    {
        return ScatterLambert(mat, r_in, rec, attenuation, scattered, outLightE, inoutRayCount, state, outSkipNextMaterialEmission);
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
    else if (mat.type == Material::CoatedLambert)
    {
        AssertUnit(r_in.dir); AssertUnit(rec.normal);
        // Orient the normal towards the observer first; this is the same for
        // both events and also applies to the no-coat degenerate case.
        Hit orientedRec = rec;
        orientedRec.normal = OrientNormalTowardRay(rec.normal, r_in.dir);
        if (mat.ri <= 1.0f)
        {
            // ri == 1 is the explicit "no coat" exception: behave exactly like
            // the legacy Lambert material, including its random stream.
            return ScatterLambert(mat, r_in, orientedRec, attenuation, scattered, outLightE, inoutRayCount, state, outSkipNextMaterialEmission);
        }

        float3 n = orientedRec.normal;
        float3 wo = -r_in.dir;
        float cosWo = std::min(std::max(dot(n, wo), 0.0f), 1.0f);
        float Fwo = CoatedFresnel(cosWo, mat.ri);
        float3 matAlbedo = mat.albedo.toFloat3();

        // Two events: the coat reflects with weight F(wo), the base scatters
        // diffusely with weight (1-F(wo)).  Pick specular with probability
        // F(wo); its throughput then remains exactly F(wo)/F(wo) == 1 and the
        // diffuse throughput below carries the matching 1/(1-F(wo)).
        bool specular = RandomFloat01(state) < Fwo;
        if (specular)
        {
            // Exact mirror direction; the recursive ray is responsible for
            // any lamp that is visible in the reflection.
            scattered = Ray(rec.pos, normalize(reflect(r_in.dir, n)));
            attenuation = float3(1,1,1);
            return true;
        }

        // Cosine-sampled diffuse event.  The cosine-sampling pdf cancels
        // cos/pi, and the event probability (1-Fwo) cancels the matching
        // factor in the BSDF, leaving:
        //   BSDF * pi / (1-Fwo) == albedo * (1-Fwi)
        float3 wi = CosineHemisphere(n, state);
        scattered = Ray(rec.pos, wi);
        float cosWi = std::min(std::max(dot(n, wi), 0.0f), 1.0f);
        attenuation = CoatedDiffuseBSDF(matAlbedo, mat.ri, cosWo, cosWi) * (kPI / (1.0f - Fwo));

#if DO_LIGHT_SAMPLING
        // The diffuse event accounts for sphere lights explicitly; a later
        // hit on the same lamp must therefore not add its emission again.
        if (outSkipNextMaterialEmission)
            *outSkipNextMaterialEmission = true;
        if (Fwo < 1.0f)
        {
            for (int j = 0; j < s_EmissiveSphereCount; ++j)
            {
                int i = s_EmissiveSpheres[j];
                const Material& smat = s_SphereMats[i];
                if (&mat == &smat)
                    continue; // skip self
                const Sphere& s = s_Spheres[i];

                float3 sc = s.center.toFloat3();
                float3 sw = normalize(sc - rec.pos);
                float3 su = normalize(cross(fabs(sw.getX())>0.01f ? float3(0,1,0):float3(1,0,0), sw));
                float3 sv = cross(sw, su);
                float cosAMax = sqrtf(1.0f - s.radius*s.radius / sqLength(rec.pos-sc));
                float eps1 = RandomFloat01(state), eps2 = RandomFloat01(state);
                float cosA = 1.0f - eps1 + eps1 * cosAMax;
                float sinA = sqrtf(1.0f - cosA*cosA);
                float phi = 2 * kPI * eps2;
                float3 l = su * (cosf(phi) * sinA) + sv * (sinf(phi) * sinA) + sw * cosA;

                Hit lightHit;
                int hitID;
                ++inoutRayCount;
                if (HitWorld(Ray(rec.pos, l), kMinT, kMaxT, lightHit, hitID) && hitID == i)
                {
                    float cosWiLight = dot(n, l);
                    if (cosWiLight <= 0.0f)
                        continue; // diffuse lobe only exists above the normal
                    float omega = 2 * kPI * (1-cosAMax);
                    // Direct-light estimate for the diffuse event.  The BSDF
                    // is divided by the diffuse-event probability (1-Fwo)
                    // because that factor is already carried by the event
                    // selection.
                    float3 brdf = CoatedDiffuseBSDF(matAlbedo, mat.ri, cosWo, cosWiLight);
                    float3 smatEmissive = smat.emissive.toFloat3();
                    outLightE += brdf * (smatEmissive * (cosWiLight * omega / (1.0f - Fwo)));
                }
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
        bool skipNextMaterialEmission = false;
        const Material& mat = s_SphereMats[id];
        float3 matE = mat.emissive.toFloat3();
        if (depth < kMaxDepth && Scatter(mat, r, rec, attenuation, scattered, lightE, inoutRayCount, state, &skipNextMaterialEmission))
        {
#if DO_LIGHT_SAMPLING
            if (!doMaterialE) matE = float3(0,0,0); // don't add material emission if told so
            // Lambert materials just did explicit light (emissive) sampling and
            // already accounted for their contribution, so if the next ray
            // bounce hits the light again, don't add emission.  A coated
            // Lambert does the same only on its diffuse event; its specular
            // event must keep whatever lamp is visible in the reflection.
            doMaterialE = (mat.type != Material::Lambert) && !skipNextMaterialEmission;
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
