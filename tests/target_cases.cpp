// CPU-side checks for the CoatedLambert material.
//
// The production code lives in Cpp/Source/Test.cpp; including it here (the
// same way support/baseline.cpp does) lets the tests exercise the real
// Scatter/Trace implementation rather than a reimplementation.

#include "test_api.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Cpp/Source/Test.cpp"

namespace
{

struct SceneItem
{
    float3 center;
    float radius;
    Material mat;
};

static Material LambertMat(float3 albedo, float3 emissive = float3(0,0,0))
{
    return Material{ Material::Lambert, albedo, emissive, 0.0f, 0.0f };
}

static void SetScene(const std::vector<SceneItem>& items, int width = 64, int height = 48)
{
    // Keep the arrays a valid "only the listed objects exist" scene.
    for (int i = 0; i < kSphereCount; ++i)
    {
        s_Spheres[i] = Sphere(float3(100000.0f, 100000.0f, 100000.0f), 1.0f);
        s_SphereMats[i] = LambertMat(float3(0,0,0));
    }
    const int n = std::min<int>((int)items.size(), kSphereCount);
    for (int i = 0; i < n; ++i)
    {
        s_Spheres[i] = Sphere(items[i].center, items[i].radius);
        s_SphereMats[i] = items[i].mat;
    }
    UpdateTest(0, 0, width, height, 0);
}

// Independent reference implementation of the model in the task statement.
// It deliberately does not call the production helpers.
static double RefFresnel(double c, double ri)
{
    if (ri <= 1.0)
        return 0.0;
    c = std::max(0.0, std::min(1.0, c));
    double f0 = (ri - 1.0) / (ri + 1.0);
    f0 *= f0;
    double m = 1.0 - c;
    return f0 + (1.0 - f0) * std::pow(m, 5.0);
}

// Numerically integrate the directional-hemispherical reflectance of the
// coated Lambert model for one colour channel:
//   F(wo) + integral_hemisphere albedo/pi * (1-F(wo)) * (1-F(wi)) * cos(wi) dwi
// using midpoint quadrature over theta and phi.
static double RefReflectance(double albedo, double ri, double cosWo, int nTheta = 192, int nPhi = 128)
{
    const double pi = 3.14159265358979323846;
    const double dTheta = (pi * 0.5) / nTheta;
    const double dPhi = (2.0 * pi) / nPhi;
    const double Fwo = RefFresnel(cosWo, ri);
    double integral = 0.0;
    for (int i = 0; i < nTheta; ++i)
    {
        double theta = (i + 0.5) * dTheta;
        double c = std::cos(theta);
        double s = std::sin(theta);
        for (int j = 0; j < nPhi; ++j)
        {
            integral += (albedo / pi) * (1.0 - Fwo) * (1.0 - RefFresnel(c, ri)) * c * s;
        }
    }
    integral *= dTheta * dPhi;
    return Fwo + integral;
}

// Average throughput returned by the real Scatter implementation.  The
// specular event contributes throughput 1 and the diffuse event contributes
// albedo*(1-F(wi)); their mixture expectation is exactly the reflectance
// above.
static float3 AverageScatterThroughput(const Material& mat, float3 n, float3 wo, int samples, uint32_t seed)
{
    Hit rec;
    rec.pos = float3(0, 0, 0);
    rec.normal = n;
    rec.t = 1.0f;
    Ray r_in(rec.pos, normalize(-wo));
    double sx = 0.0, sy = 0.0, sz = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        uint32_t state = (uint32_t)(i * 747796405u + seed) | 1u;
        float3 attenuation;
        float3 lightE;
        Ray scattered;
        int rayCount = 0;
        Scatter(mat, r_in, rec, attenuation, scattered, lightE, rayCount, state, nullptr);
        sx += attenuation.getX();
        sy += attenuation.getY();
        sz += attenuation.getZ();
    }
    return float3((float)(sx / samples), (float)(sy / samples), (float)(sz / samples));
}

// Reference direct-lighting integral for a spherical lamp:
//   integral_cone L * albedo/pi * (1-F(wo)) * (1-F(wi)) * cos(wi) dwi
static float3 RefDirectLight(float3 recPos, float3 n, float3 wo, float3 albedo, float ri,
                             float3 lightCenter, float lightRadius, float3 lightE,
                             int nCos = 256, int nPhi = 128)
{
    const double pi = 3.14159265358979323846;
    float3 sw = normalize(lightCenter - recPos);
    float3 su = normalize(cross(fabsf(sw.getX()) > 0.01f ? float3(0,1,0) : float3(1,0,0), sw));
    float3 sv = cross(sw, su);
    float cosAMax = sqrtf(1.0f - lightRadius*lightRadius / sqLength(recPos - lightCenter));
    double Fwo = RefFresnel(dot(n, wo), ri);
    double dCos = (1.0 - cosAMax) / nCos;
    double dPhi = (2.0 * pi) / nPhi;
    float3 sum(0, 0, 0);
    for (int i = 0; i < nCos; ++i)
    {
        double cosA = cosAMax + (i + 0.5) * dCos;
        double sinA = std::sqrt(std::max(0.0, 1.0 - cosA*cosA));
        for (int j = 0; j < nPhi; ++j)
        {
            double phi = (j + 0.5) * dPhi;
            float3 l = su * (float)(std::cos(phi) * sinA) + sv * (float)(std::sin(phi) * sinA) + sw * (float)cosA;
            double cosI = dot(n, l);
            if (cosI <= 0.0)
                continue;
            double f = (1.0 - Fwo) * (1.0 - RefFresnel(cosI, ri)) * cosI;
            sum += lightE * float3((float)(albedo.getX() * f),
                                   (float)(albedo.getY() * f),
                                   (float)(albedo.getZ() * f));
        }
    }
    return sum * (float)(dCos * dPhi / pi);
}

// Average direct-light term produced by Scatter for the real scene.
static float3 AverageDirectLight(const Material& mat, float3 recPos, float3 n, float3 wo,
                                 int samples, uint32_t seed)
{
    Hit rec;
    rec.pos = recPos;
    rec.normal = n;
    rec.t = 1.0f;
    Ray r_in(rec.pos, normalize(-wo));
    double sx = 0.0, sy = 0.0, sz = 0.0;
    for (int i = 0; i < samples; ++i)
    {
        uint32_t state = (uint32_t)(i * 2891336453u + seed) | 1u;
        float3 attenuation;
        float3 lightE;
        Ray scattered;
        int rayCount = 0;
        Scatter(mat, r_in, rec, attenuation, scattered, lightE, rayCount, state, nullptr);
        sx += lightE.getX();
        sy += lightE.getY();
        sz += lightE.getZ();
    }
    return float3((float)(sx / samples), (float)(sy / samples), (float)(sz / samples));
}

static uint32_t FindSpecularState(const Material& mat, const Ray& ray, const Hit& rec, uint32_t start, uint32_t span)
{
    for (uint32_t s = start; s < start + span; ++s)
    {
        uint32_t state = s | 1u;
        float3 attenuation, lightE;
        Ray scattered;
        bool skip = false;
        int rayCount = 0;
        Scatter(mat, ray, rec, attenuation, scattered, lightE, rayCount, state, &skip);
        if (!skip && attenuation.getX() == 1.0f && attenuation.getY() == 1.0f && attenuation.getZ() == 1.0f)
            return s | 1u;
    }
    return 0;
}

static uint32_t FindDiffuseEvent(const Material& mat, const Ray& ray, const Hit& rec,
                                 uint32_t start, uint32_t span)
{
    for (uint32_t s = start; s < start + span; ++s)
    {
        uint32_t state = s | 1u;
        float3 attenuation, lightE;
        Ray scattered;
        bool skip = false;
        int rayCount = 0;
        Scatter(mat, ray, rec, attenuation, scattered, lightE, rayCount, state, &skip);
        float3 mirror = normalize(reflect(ray.dir, rec.normal));
        if (skip && dot(scattered.dir, mirror) < 0.99999f)
            return s | 1u;
    }
    return 0;
}

static uint32_t FindDiffuseHittingLamp(const Material& mat, const Ray& ray, const Hit& rec,
                                       int lampId, uint32_t start, uint32_t span)
{
    for (uint32_t s = start; s < start + span; ++s)
    {
        uint32_t state = s | 1u;
        float3 attenuation, lightE;
        Ray scattered;
        bool skip = false;
        int rayCount = 0;
        Scatter(mat, ray, rec, attenuation, scattered, lightE, rayCount, state, &skip);
        if (!skip)
            continue;
        Hit h;
        int id;
        if (HitWorld(scattered, kMinT, kMaxT, h, id) && id == lampId)
            return s | 1u;
    }
    return 0;
}

static float MaxChannelDiff(float3 a, float3 b)
{
    return std::max(std::max(fabsf(a.getX() - b.getX()), fabsf(a.getY() - b.getY())), fabsf(a.getZ() - b.getZ()));
}

static void WritePPM(const char* path, int width, int height, const std::vector<float>& pixels)
{
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i)
    {
        unsigned char rgb[3];
        for (int c = 0; c < 3; ++c)
        {
            float v = std::max(0.0f, pixels[i*4+c]);
            v = std::pow(v, 1.0f / 2.2f); // small preview only
            v = std::min(1.0f, v);
            rgb[c] = (unsigned char)(v * 255.0f + 0.5f);
        }
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

} // namespace

void target_cases(Suite& suite)
{
    InitializeTest();
    auto require = [&](bool ok, const char* why) { suite.require(ok, why); };

    // Snapshot the shipped default scene so the tests cannot change it.
    std::vector<Sphere> savedSpheres(s_Spheres, s_Spheres + kSphereCount);
    std::vector<Material> savedMats(s_SphereMats, s_SphereMats + kSphereCount);

    suite.run("fresnel_exception_and_formula", [&]() {
        require(CoatedFresnel(0.0f, 1.0f) == 0.0f && CoatedFresnel(0.5f, 1.0f) == 0.0f &&
                CoatedFresnel(1.0f, 1.0f) == 0.0f,
                "ri == 1 must return F(c) == 0");
        require(fabsf(CoatedFresnel(1.0f, 1.5f) - 0.04f) < 1e-6f,
                "F(1) for ri=1.5 must equal F0 = 0.04");
        require(fabsf(CoatedFresnel(0.0f, 1.5f) - 1.0f) < 1e-6f,
                "F(0) for ri>1 must be 1");
        bool matches = true;
        const float testCos[] = { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f };
        for (float c : testCos)
            matches = matches && fabsf(CoatedFresnel(c, 1.8f) - (float)RefFresnel(c, 1.8)) < 2e-6f;
        require(matches, "F(c) must match F0 + (1-F0)(1-c)^5");
    });

    suite.run("diffuse_bsdf_above_normal", [&]() {
        float3 below = CoatedDiffuseBSDF(float3(0.8f,0.4f,0.2f), 1.5f, 0.7f, -0.1f);
        require(below.getX() == 0.0f && below.getY() == 0.0f && below.getZ() == 0.0f,
                "diffuse BSDF must vanish below the normal");
        float3 lambert = CoatedDiffuseBSDF(float3(0.8f,0.4f,0.2f), 1.0f, 0.7f, 0.6f);
        require(MaxChannelDiff(lambert, float3(0.8f,0.4f,0.2f) * (1.0f / kPI)) < 1e-6f,
                "ri == 1 must reduce to albedo/pi");
        float f0 = (1.5f - 1.0f) / (1.5f + 1.0f);
        f0 *= f0;
        float3 normal = CoatedDiffuseBSDF(float3(0.8f,0.4f,0.2f), 1.5f, 1.0f, 1.0f);
        require(MaxChannelDiff(normal, float3(0.8f,0.4f,0.2f) * ((1.0f-f0)*(1.0f-f0) * (1.0f / kPI))) < 1e-6f,
                "normal-incidence BSDF must include (1-F(1))^2");
        float3 oblique = CoatedDiffuseBSDF(float3(0.8f,0.4f,0.2f), 1.5f, 0.8f, 0.4f);
        float expected = (1.0f - (float)RefFresnel(0.8, 1.5)) * (1.0f - (float)RefFresnel(0.4, 1.5)) * (1.0f / kPI);
        require(MaxChannelDiff(oblique, float3(0.8f,0.4f,0.2f) * expected) < 2e-6f,
                "oblique BSDF must include both Fresnel factors");
    });

    suite.run("hemisphere_sampling_matches_integration", [&]() {
        Material coated{ Material::CoatedLambert, float3(0.8f,0.4f,0.2f), float3(0,0,0), 0.0f, 1.5f };
        SetScene({ { float3(0,0,0), 1.0f, coated } });

        const int kSamples = 300000;
        const float3 albedo(0.8f, 0.4f, 0.2f);
        const float angleCos[2] = { 1.0f, 0.5f };
        const char* angleName[2] = { "normal", "oblique" };

        for (int a = 0; a < 2; ++a)
        {
            float s = sqrtf(std::max(0.0f, 1.0f - angleCos[a]*angleCos[a]));
            float3 n(0.0f, 0.0f, 1.0f);
            float3 wo = normalize(float3(s, 0.0f, angleCos[a]));

            Material lambert{ Material::CoatedLambert, albedo, float3(0,0,0), 0.0f, 1.0f };
            float3 avg1 = AverageScatterThroughput(lambert, n, wo, kSamples, 12345u + a);
            std::printf("DEGENERATE %s avg1=(%.9f %.9f %.9f) expect=(%.9f %.9f %.9f)\n",
                        angleName[a], avg1.getX(), avg1.getY(), avg1.getZ(),
                        albedo.getX(), albedo.getY(), albedo.getZ());
            require(MaxChannelDiff(avg1, albedo) < 1e-5f,
                    "ri == 1 coated Lambert must reduce exactly to albedo");

            Material coated15{ Material::CoatedLambert, albedo, float3(0,0,0), 0.0f, 1.5f };
            float3 avg = AverageScatterThroughput(coated15, n, wo, kSamples, 98765u + a);
            float3 ref((float)RefReflectance(albedo.getX(), 1.5, angleCos[a]),
                       (float)RefReflectance(albedo.getY(), 1.5, angleCos[a]),
                       (float)RefReflectance(albedo.getZ(), 1.5, angleCos[a]));
            float diff = MaxChannelDiff(avg, ref);
            std::printf("HEMISPHERE %s avg=(%.6f %.6f %.6f) integral=(%.6f %.6f %.6f) maxdiff=%.6f\n",
                        angleName[a], avg.getX(), avg.getY(), avg.getZ(),
                        ref.getX(), ref.getY(), ref.getZ(), diff);
            require(diff < 0.003f, "sampled throughput must match independent hemisphere integral");

            // Flipping the stored normal must not change anything: the
            // material orients the normal towards the observer.
            float3 avgFlipped = AverageScatterThroughput(coated15, -n, wo, kSamples, 98765u + a);
            require(MaxChannelDiff(avgFlipped, avg) < 1e-6f,
                    "normal must be oriented towards the observer");
        }
    });

    suite.run("sphere_light_direct_and_event_flags", [&]() {
        const float3 albedo(0.8f, 0.4f, 0.2f);
        const float3 lightE(5.0f, 5.0f, 5.0f);
        Material coated15{ Material::CoatedLambert, albedo, float3(0,0,0), 0.0f, 1.5f };
        Material lamp = LambertMat(float3(0,0,0), lightE);

        const float angleCos[2] = { 1.0f, 0.5f };
        for (int a = 0; a < 2; ++a)
        {
            float s = sqrtf(std::max(0.0f, 1.0f - angleCos[a]*angleCos[a]));
            float3 n(0.0f, 0.0f, 1.0f);
            float3 wo = normalize(float3(s, 0.0f, angleCos[a]));
            float3 recPos(0.0f, 0.0f, 1.0f);
            float3 lightPos = recPos + n * 3.0f;
            SetScene({ { float3(0,0,0), 1.0f, coated15 }, { lightPos, 1.5f, lamp } });

            float3 avg = AverageDirectLight(coated15, recPos, n, wo, 120000, 555u + a);
            float3 ref = RefDirectLight(recPos, n, wo, albedo, 1.5f, lightPos, 1.5f, lightE);
            float diff = MaxChannelDiff(avg, ref);
            std::printf("DIRECT cosWo=%.1f avg=(%.6f %.6f %.6f) integral=(%.6f %.6f %.6f) maxdiff=%.6f\n",
                        angleCos[a], avg.getX(), avg.getY(), avg.getZ(),
                        ref.getX(), ref.getY(), ref.getZ(), diff);
            require(diff < 0.003f, "sphere-light direct sampling must match independent integration");
        }

        // ri == 1 must also use the Fresnel exception for sphere-light
        // direct sampling, i.e. match plain Lambert exactly.
        {
            Material coated1{ Material::CoatedLambert, albedo, float3(0,0,0), 0.0f, 1.0f };
            Material lambert = LambertMat(albedo);
            float3 n(0.0f, 0.0f, 1.0f);
            float3 wo = n;
            float3 recPos(0.0f, 0.0f, 1.0f);
            float3 lightPos = recPos + n * 3.0f;
            SetScene({ { float3(0,0,0), 1.0f, coated1 }, { lightPos, 1.5f, lamp } });
            float3 avgCoated = AverageDirectLight(coated1, recPos, n, wo, 120000, 777u);
            SetScene({ { float3(0,0,0), 1.0f, lambert }, { lightPos, 1.5f, lamp } });
            float3 avgLambert = AverageDirectLight(lambert, recPos, n, wo, 120000, 777u);
            float3 ref = RefDirectLight(recPos, n, wo, albedo, 1.0f, lightPos, 1.5f, lightE);
            float diffCoatLambert = MaxChannelDiff(avgCoated, avgLambert);
            float diffRef = MaxChannelDiff(avgCoated, ref);
            std::printf("DIRECT_RI1 coated=(%.6f %.6f %.6f) lambert=(%.6f %.6f %.6f) integral=(%.6f %.6f %.6f) maxdiff_vs_lambert=%.6f maxdiff_vs_integral=%.6f\n",
                        avgCoated.getX(), avgCoated.getY(), avgCoated.getZ(),
                        avgLambert.getX(), avgLambert.getY(), avgLambert.getZ(),
                        ref.getX(), ref.getY(), ref.getZ(), diffCoatLambert, diffRef);
            require(diffCoatLambert == 0.0f,
                    "ri == 1 direct light must match legacy Lambert exactly");
            require(diffRef < 0.003f,
                    "ri == 1 direct light must match independent integration");
        }

        // The two events must tell the caller whether the next vertex may add
        // material emission.  This is what prevents double counting.
        SetScene({ { float3(0,0,0), 1.0f, coated15 } });
        Hit rec;
        rec.pos = float3(0,0,1);
        rec.normal = float3(0,0,1);
        rec.t = 1.0f;
        Ray r_in(rec.pos, float3(0,0,-1));

        uint32_t specState = FindSpecularState(coated15, r_in, rec, 1u, 20000u);
        require(specState != 0, "could not find a specular-event random state");
        uint32_t diffState = FindDiffuseEvent(coated15, r_in, rec, 1u, 20000u);
        require(diffState != 0, "could not find a diffuse-event random state");
        {
            uint32_t st = specState;
            float3 att, le; Ray sc; bool skip = true; int rc = 0;
            Scatter(coated15, r_in, rec, att, sc, le, rc, st, &skip);
            require(!skip, "specular event must NOT suppress material emission");
        }
        {
            uint32_t st = diffState;
            float3 att, le; Ray sc; bool skip = false; int rc = 0;
            Scatter(coated15, r_in, rec, att, sc, le, rc, st, &skip);
            require(skip, "diffuse event must suppress the already-sampled lamp");
        }
        {
            Material lambert = LambertMat(albedo);
            uint32_t st = 1u; float3 att, le; Ray sc; bool skip = false; int rc = 0;
            Scatter(lambert, r_in, rec, att, sc, le, rc, st, &skip);
            require(skip, "legacy Lambert must keep suppressing material emission");
            Material metal{ Material::Metal, albedo, float3(0,0,0), 0.0f, 0.0f };
            st = 1u; skip = true;
            Scatter(metal, r_in, rec, att, sc, le, rc, st, &skip);
            require(!skip, "legacy Metal must keep material emission");
        }
    });

    suite.run("specular_sees_lamp_and_diffuse_does_not_double_count", [&]() {
        const float3 albedo(0.8f, 0.4f, 0.2f);
        const float3 lightE(5.0f, 5.0f, 5.0f);
        Material coated15{ Material::CoatedLambert, albedo, float3(0,0,0), 0.0f, 1.5f };
        Material lamp = LambertMat(float3(0,0,0), lightE);

        SetScene({ { float3(0,0,0), 1.0f, coated15 }, { float3(0,0,3), 1.5f, lamp } });

        Hit rec;
        rec.pos = float3(0,0,1);
        rec.normal = float3(0,0,1);
        rec.t = 1.0f;
        Ray cameraRay(float3(0,0,1.2f), float3(0,0,-1));

        uint32_t specState = FindSpecularState(coated15, cameraRay, rec, 1u, 20000u);
        require(specState != 0, "could not find specular state for lamp visibility test");
        {
            uint32_t st = specState;
            float3 att, le; Ray sc; bool skip = false; int rc = 0;
            Scatter(coated15, cameraRay, rec, att, sc, le, rc, st, &skip);
            float3 mirror = normalize(reflect(cameraRay.dir, rec.normal));
            require(dot(sc.dir, mirror) > 0.99999f,
                    "specular branch must use the exact mirror direction");
        }
        int rayCount = 0;
        uint32_t traceState = specState;
        float3 specResult = Trace(cameraRay, 0, rayCount, traceState);
        float specDiff = MaxChannelDiff(specResult, lightE);
        std::printf("SPECULAR_LAMP trace=(%.6f %.6f %.6f) lamp=(%.6f %.6f %.6f) maxdiff=%.6f\n",
                    specResult.getX(), specResult.getY(), specResult.getZ(),
                    lightE.getX(), lightE.getY(), lightE.getZ(), specDiff);
        require(specDiff < 1e-4f, "specular reflection must keep the lamp seen in the mirror");

        uint32_t diffState = FindDiffuseHittingLamp(coated15, cameraRay, rec, 1, 1u, 400000u);
        require(diffState != 0, "could not find diffuse state whose scattered ray hits the lamp");
        {
            uint32_t st = diffState;
            float3 att, le; Ray sc; bool skip = false; int rc = 0;
            Scatter(coated15, cameraRay, rec, att, sc, le, rc, st, &skip);
            Hit h; int id;
            require(skip && HitWorld(sc, kMinT, kMaxT, h, id) && id == 1,
                    "diffuse scattered ray must actually hit the lamp");
            uint32_t traceSt = diffState;
            int rays = 0;
            float3 diffResult = Trace(cameraRay, 0, rays, traceSt);
            float directDiff = MaxChannelDiff(diffResult, le);
            std::printf("DIFFUSE_LAMP trace=(%.6f %.6f %.6f) direct=(%.6f %.6f %.6f) maxdiff=%.6f\n",
                        diffResult.getX(), diffResult.getY(), diffResult.getZ(),
                        le.getX(), le.getY(), le.getZ(), directDiff);
            require(directDiff < 1e-5f,
                    "diffuse path must not add the lamp a second time after direct sampling");
            require(le.getX() > 0.0f, "direct lighting estimate must be non-zero");
        }
    });

    // The default scene must remain untouched and deterministic.
    suite.run("legacy_scene_random_stream_unchanged", [&]() {
        std::memcpy(s_Spheres, savedSpheres.data(), sizeof(Sphere) * kSphereCount);
        std::memcpy(s_SphereMats, savedMats.data(), sizeof(Material) * kSphereCount);
        UpdateTest(0, 0, 24, 16, 0);
        std::vector<float> pixels(24 * 16 * 4);
        int rays = 0;
        DrawTest(0, 0, 24, 16, pixels.data(), rays, 0);
        double sum = 0.0;
        for (int i = 0; i < 24 * 16; ++i)
            for (int c = 0; c < 3; ++c)
                sum += pixels[i*4+c];
        std::printf("LEGACY pixels=384 rays=%d sum=%.9f\n", rays, sum);
        require(rays == 7384, "legacy scene ray count changed");
        require(fabs(sum - 986.966923352) < 1.0, "legacy scene random stream/values changed");
    });

    // Small PPM rendered through the real DrawTest path, showing the coated
    // material with a spherical lamp.  The default scene is restored first.
    {
        std::memcpy(s_Spheres, savedSpheres.data(), sizeof(Sphere) * kSphereCount);
        std::memcpy(s_SphereMats, savedMats.data(), sizeof(Material) * kSphereCount);
        const int w = 64, h = 48;
        Material ball{ Material::CoatedLambert, float3(0.75f, 0.45f, 0.2f), float3(0,0,0), 0.0f, 1.5f };
        Material lamp = LambertMat(float3(0.1f,0.1f,0.1f), float3(30, 30, 30));
        SetScene({ { float3(0,0,0), 1.0f, ball }, { float3(2.2f, 2.8f, 0.8f), 0.35f, lamp } }, w, h);
        std::vector<float> pixels(w * h * 4);
        int rays = 0;
        DrawTest(0, 0, w, h, pixels.data(), rays, 0);
        const char* path = ".build/coated_lambert.ppm";
        WritePPM(path, w, h, pixels);
        double sum = 0.0;
        for (float v : pixels) sum += v;
        std::printf("PPM path=%s size=%dx%d rays=%d sum=%.6f\n", path, w, h, rays, sum);
        require(rays > 0 && sum > 0.0, "DrawTest PPM must contain rendered rays");
    }

    // Leave the shipped scene in place for anyone linking this test into a
    // larger application.
    std::memcpy(s_Spheres, savedSpheres.data(), sizeof(Sphere) * kSphereCount);
    std::memcpy(s_SphereMats, savedMats.data(), sizeof(Material) * kSphereCount);
    UpdateTest(0, 0, 24, 16, 0);

    ShutdownTest();
}
