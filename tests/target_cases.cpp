// Target checks for the CPU-only CoatedLambert material.
//
// This file #includes the CPU tracer implementation (same trick as
// support/baseline.cpp) so the checks can exercise the real Trace/Scatter code
// paths and the internal coating helpers. Everything here uses only the C++
// standard library.
#include "test_api.h"

#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

#include "Cpp/Source/Test.cpp"

namespace
{

typedef float3 V3;

inline float MaxAbs3(const V3& a, const V3& b)
{
    float d = fmaxf(fabsf(a.getX() - b.getX()), fabsf(a.getY() - b.getY()));
    return fmaxf(d, fabsf(a.getZ() - b.getZ()));
}

// Maximum per-channel relative difference (with an absolute floor).
inline float MaxRel3(const V3& got, const V3& want, float absFloor = 1e-4f)
{
    float worst = 0.0f;
    float g[3] = { got.getX(), got.getY(), got.getZ() };
    float w[3] = { want.getX(), want.getY(), want.getZ() };
    for (int c = 0; c < 3; ++c)
    {
        float denom = fmaxf(fabsf(w[c]), absFloor);
        worst = fmaxf(worst, fabsf(g[c] - w[c]) / denom);
    }
    return worst;
}

// Independent reference Fresnel, written from the spec rather than calling CoatF.
float RefF(float c, float ri)
{
    if (ri <= 1.0f) return 0.0f; // explicit ri == 1 exception
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;
    float r0 = (ri - 1.0f) / (ri + 1.0f);
    float F0 = r0 * r0;
    float t = 1.0f - c;
    return F0 + (1.0f - F0) * t * t * t * t * t;
}

// Composite Simpson integral of (1-F(u))*u over [a,b], independent quadrature.
float IntegrateG(float ri, float a, float b, int steps = 20000)
{
    if (steps & 1) ++steps;
    float h = (b - a) / steps;
    auto g = [&](float u) { return (1.0f - RefF(u, ri)) * u; };
    float sum = g(a) + g(b);
    for (int i = 1; i < steps; ++i)
    {
        float u = a + h * i;
        sum += (i & 1) ? 4.0f * g(u) : 2.0f * g(u);
    }
    return sum * h / 3.0f;
}

// Replace the whole global scene with far-away harmless spheres.
void ResetCustomScene()
{
    for (int i = 0; i < kSphereCount; ++i)
    {
        // Far beyond kMaxT so these can never be hit by a test ray.
        s_Spheres[i].center = float3pack(0.0f, -1.0e8f - i * 1000.0f, 0.0f);
        s_Spheres[i].radius = 1.0f;
        s_Spheres[i].invRadius = 1.0f;
        s_SphereMats[i].type = Material::Lambert;
        s_SphereMats[i].albedo = float3pack(0, 0, 0);
        s_SphereMats[i].emissive = float3pack(0, 0, 0);
        s_SphereMats[i].roughness = 0.0f;
        s_SphereMats[i].ri = 0.0f;
    }
    s_EmissiveSphereCount = 0;
}

void SetSphere(int idx, const V3& center, float radius, const Material& m)
{
    s_Spheres[idx].center = float3pack(center.getX(), center.getY(), center.getZ());
    s_Spheres[idx].radius = radius;
    s_Spheres[idx].invRadius = 1.0f / radius;
    s_SphereMats[idx] = m;
}

Material MakeMat(Material::Type t, const V3& albedo, const V3& emissive, float ri)
{
    Material m;
    m.type = t;
    m.albedo = float3pack(albedo.getX(), albedo.getY(), albedo.getZ());
    m.emissive = float3pack(emissive.getX(), emissive.getY(), emissive.getZ());
    m.roughness = 0.0f;
    m.ri = ri;
    return m;
}

// Camera ray along -Z at offset b in X against a unit sphere at the origin.
// For that hit N.wo == sqrt(1-b^2) (1 = head-on, small = grazing).
Ray OffsetRay(float b)
{
    return Ray(V3(b, 0.0f, 3.0f), V3(0, 0, -1));
}

bool HitFirst(const Ray& r, Hit& rec, int& id)
{
    id = HitSpheres(r, s_SpheresSoA, kMinT, kMaxT, rec);
    return id >= 0;
}

void WritePPM(const char* path, int W, int H, const float* buf, int strideFloats)
{
    std::FILE* f = std::fopen(path, "wb");
    if (!f) { std::printf("WARN cannot write %s\n", path); return; }
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    const float exposure = 1.0f;
    for (int i = 0; i < W * H; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            float v = buf[i * strideFloats + c];
            if (!(v >= 0.0f) || !std::isfinite(v)) v = 0.0f;
            v = 1.0f - expf(-exposure * v);
            v = powf(fminf(1.0f, fmaxf(0.0f, v)), 1.0f / 2.2f);
            unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
            std::fputc(b, f);
        }
    }
    std::fclose(f);
    std::printf("PPM wrote %s (%dx%d)\n", path, W, H);
}

} // namespace

void target_cases(Suite& suite)
{
    InitializeTest();
    // ------------------------------------------------------------------
    suite.run("01_fresnel_schlick_and_ri1_exception", [&]()
    {
        // ri == 1 must be identically zero across the whole cosine range.
        bool ri1Zero = true;
        for (int i = 0; i <= 100; ++i)
        {
            float c = i / 100.0f;
            if (CoatF(c, 1.0f) != 0.0f) ri1Zero = false;
        }
        suite.require(ri1Zero, "F must be identically 0 when ri == 1");

        const float ri = 1.5f;
        float F0expect = CoatF0(ri);
        suite.require(fabsf(F0expect - 0.04f) < 1e-7f, "F0 for ri=1.5 is 0.04");
        suite.require(fabsf(CoatF(1.0f, ri) - F0expect) < 1e-7f, "F(1) == F0");
        suite.require(fabsf(CoatF(0.0f, ri) - 1.0f) < 1e-6f, "F(0) == 1");

        bool schlickMatches = true;
        for (int i = 1; i < 40; ++i)
        {
            float c = i / 40.0f;
            if (fabsf(CoatF(c, ri) - RefF(c, ri)) > 1e-6f) schlickMatches = false;
        }
        suite.require(schlickMatches, "CoatF must match the specified Schlick formula");

        // Diffuse BSDF is exactly albedo/pi*(1-Fo)*(1-Fi) above the hemisphere
        // and zero below it.
        V3 alb(0.8f, 0.5f, 0.2f);
        V3 n(0, 1, 0), wo(0, 1, 0);
        V3 wi(0.3f, 0.9f, 0.0f); wi = normalize(wi);
        V3 f = CoatDiffuseBSDF(alb, ri, n, wo, wi);
        float expectScale = (1 - RefF(1, ri)) * (1 - RefF(dot(n, wi), ri)) / kPI;
        suite.require(MaxAbs3(f, alb * expectScale) < 1e-6f, "diffuse BSDF formula mismatch");
        V3 below = CoatDiffuseBSDF(alb, ri, n, wo, V3(0, -1, 0));
        suite.require(MaxAbs3(below, V3(0, 0, 0)) == 0.0f, "diffuse BSDF must vanish below normal");
    });

    // ------------------------------------------------------------------
    // Independent hemisphere integration vs the actual event sampler average,
    // at a head-on and a grazing view angle.
    suite.run("02_sampler_average_matches_hemisphere_integral", [&]()
    {
        const int N = 240000;
        const float ri = 1.5f;
        V3 albedo(0.8f, 0.45f, 0.2f);
        float K = 2.0f * IntegrateG(ri, 0.0f, 1.0f); // E_cos[1-F] factor

        float angles[2] = { 0.0f, 75.0f * kPI / 180.0f };
        const char* names[2] = { "head-on", "grazing-75deg" };
        for (int a = 0; a < 2; ++a)
        {
            V3 n(0, 1, 0);
            V3 wo(sinf(angles[a]), cosf(angles[a]), 0.0f);
            float NoV = cosf(angles[a]);
            float Fo = CoatF(NoV, ri);

            uint32_t state = 0x1234abcdu + a * 777u;
            V3 sum(0, 0, 0);
            int specSeen = 0;
            double dirErr = 0.0;
            for (int i = 0; i < N; ++i)
            {
                Ray sr; V3 atten; int ev;
                CoatSampleEvent(albedo, ri, V3(0, 0, 0), n, wo, state, sr, atten, ev);
                sum += atten;
                if (ev == kScatterSpecular)
                {
                    ++specSeen;
                    V3 expectDir = normalize(reflect(-wo, n));
                    dirErr = fmax(dirErr, MaxAbs3(sr.dir, expectDir));
                }
            }
            V3 avg = sum * (1.0f / N);
            V3 expect(
                Fo + albedo.getX() * (1.0f - Fo) * K,
                Fo + albedo.getY() * (1.0f - Fo) * K,
                Fo + albedo.getZ() * (1.0f - Fo) * K);
            float rel = MaxRel3(avg, expect, 1e-3f);
            float freq = (float)specSeen / N;
            std::printf("DIFF hemi[%-13s] avg=(%.5f %.5f %.5f) integ=(%.5f %.5f %.5f) reldiff=%.4f specFreq=%.4f F=%.4f maxMirrorErr=%.2e\n",
                        names[a], avg.getX(), avg.getY(), avg.getZ(),
                        expect.getX(), expect.getY(), expect.getZ(), rel, freq, Fo, dirErr);
            suite.require(rel < 0.03f, "sampler average must match independent hemisphere integral");
            suite.require(fabsf(freq - Fo) < 0.015f, "specular event frequency must equal F(N.wo)");
            suite.require(specSeen > 1000, "test must observe many specular events");
            suite.require(dirErr < 1e-5, "specular branch must keep the exact reflection direction");
        }

        // ri == 1 collapses to plain Lambert reflectance = albedo exactly.
        {
            V3 n(0, 1, 0), wo(0.2f, 0.98f, 0.0f); wo = normalize(wo);
            uint32_t state = 4242u;
            double sx = 0, sy = 0, sz = 0;
            int nonDiffuse = 0;
            const int M = 50000;
            for (int i = 0; i < M; ++i)
            {
                Ray sr; V3 atten; int ev;
                CoatSampleEvent(V3(0.7f, 0.6f, 0.5f), 1.0f, V3(0, 0, 0), n, wo, state, sr, atten, ev);
                if (ev != kScatterDiffuse) ++nonDiffuse;
                sx += atten.getX(); sy += atten.getY(); sz += atten.getZ();
            }
            double inv = 1.0 / M;
            float d = fmaxf(fmaxf(fabsf((float)(sx*inv) - 0.7f), fabsf((float)(sy*inv) - 0.6f)),
                            fabsf((float)(sz*inv) - 0.5f));
            std::printf("DIFF ri1-collapse maxabs=%.2e nonDiffuseEvents=%d\n", d, nonDiffuse);
            suite.require(nonDiffuse == 0, "ri=1 must never sample a specular event");
            suite.require(d < 1e-6f, "ri=1 coated material must collapse to Lambert albedo");
        }
    });

    // ------------------------------------------------------------------
    // Sphere-light direct illumination through the real NEE helper vs an
    // independent 1D integral for a cone aligned with the normal.
    suite.run("03_sphere_light_direct_matches_integration", [&]()
    {
        ResetCustomScene();
        const float D = 4.0f, R = 1.0f;
        V3 Le(9.0f, 8.0f, 7.0f);
        SetSphere(0, V3(0, D, 0), R, MakeMat(Material::Lambert, V3(0, 0, 0), Le, 0.0f));
        UpdateTest(0, 0, 16, 16, 0);

        const float cosA = sqrtf(1.0f - (R / D) * (R / D));
        const int N = 160000;
        float riList[2] = { 1.5f, 1.0f };
        for (int which = 0; which < 2; ++which)
        {
            float ri = riList[which];
            V3 albedo(0.72f, 0.5f, 0.28f);
            float viewAngles[2] = { 0.0f, 70.0f * kPI / 180.0f };
            for (int va = 0; va < 2; ++va)
            {
                V3 n(0, 1, 0);
                V3 wo(sinf(viewAngles[va]), cosf(viewAngles[va]), 0.0f);
                float NoV = cosf(viewAngles[va]);
                float Fo = CoatF(NoV, ri);

                uint32_t state = 0x5151u + which * 131u + va * 17u;
                int rays = 0;
                V3 sum(0, 0, 0);
                for (int i = 0; i < N; ++i)
                    sum += CoatDirectLightE(albedo, ri, V3(0, 0, 0), n, wo, 0, state, rays);
                V3 avg = sum * (1.0f / N);

                float gInt = IntegrateG(ri, cosA, 1.0f);
                V3 ref(2.0f * albedo.getX() * Le.getX() * (1.0f - Fo) * gInt,
                        2.0f * albedo.getY() * Le.getY() * (1.0f - Fo) * gInt,
                        2.0f * albedo.getZ() * Le.getZ() * (1.0f - Fo) * gInt);
                float rel = MaxRel3(avg, ref, 1e-3f);
                std::printf("DIFF direct ri=%.1f view=%2ddeg avg=(%.4f %.4f %.4f) ref=(%.4f %.4f %.4f) reldiff=%.4f\n",
                            ri, va == 0 ? 0 : 70, avg.getX(), avg.getY(), avg.getZ(),
                            ref.getX(), ref.getY(), ref.getZ(), rel);
                suite.require(rel < 0.04f, "sphere-light NEE average must match independent integration");

                if (ri == 1.0f && va == 0)
                {
                    // Closed form: a*Le*(R/D)^2 for an uncoated Lambert surface.
                    V3 analytic = albedo * ((R / D) * (R / D)) * Le;
                    float relA = MaxRel3(avg, analytic, 1e-3f);
                    std::printf("DIFF direct ri=1 analytic reldiff=%.4f\n", relA);
                    suite.require(relA < 0.03f, "ri=1 direct light must match a*Le*(R/D)^2");
                }
            }
        }

        // Explicit grazing < head-on diffuse direct response.
        auto directAvg = [&](float viewDeg) {
            float ri = 1.5f; V3 albedo(0.72f, 0.5f, 0.28f);
            float th = viewDeg * kPI / 180.0f;
            V3 n(0, 1, 0), wo(sinf(th), cosf(th), 0);
            uint32_t st = 999u; int rays = 0; V3 s(0, 0, 0);
            for (int i = 0; i < 120000; ++i)
                s += CoatDirectLightE(albedo, ri, V3(0, 0, 0), n, wo, 0, st, rays);
            return s * (1.0f / 120000);
        };
        float headLum = dot(directAvg(0), V3(0.2126f, 0.7152f, 0.0722f));
        float grazeLum = dot(directAvg(75), V3(0.2126f, 0.7152f, 0.0722f));
        std::printf("DIFF fresnel-ramp direct head=%.4f grazing=%.4f ratio=%.3f\n",
                    headLum, grazeLum, grazeLum / headLum);
        suite.require(grazeLum < headLum * 0.85f, "grazing diffuse direct light must be attenuated by (1-Fo)");
    });

    // ------------------------------------------------------------------
    // Specular event preserves a light seen in the mirror direction, both at
    // the emission-policy level and as an end-to-end stochastic average.
    suite.run("04_specular_sees_light", [&]()
    {
        ResetCustomScene();
        V3 Le(8.0f, 7.0f, 6.0f);
        const float ri = 1.5f;
        SetSphere(0, V3(0, 0, 0), 1.0f, MakeMat(Material::CoatedLambert, V3(0, 0, 0), V3(0, 0, 0), ri));

        // Grazing-ish hit: N.wo = 0.30 -> appreciable F.
        float c = 0.30f, b = sqrtf(1.0f - c * c);
        Ray cam = OffsetRay(b);
        UpdateTest(0, 0, 16, 16, 0); // builds SoA for sphere 0 only placement so far
        Hit rec; int id;
        bool hit = HitFirst(cam, rec, id);
        suite.require(hit && id == 0, "offset camera ray must hit coated sphere 0");
        V3 n = CoatOrientedNormal(rec.normal, cam);
        V3 wo = -cam.dir;
        float NoV = dot(n, wo);
        float Fo = CoatF(NoV, ri);
        V3 mdir = normalize(reflect(cam.dir, n));

        // Pure-emissive (black albedo) sphere exactly along the mirror direction.
        float D = 3.0f, LR = 1.0f;
        SetSphere(1, rec.pos + mdir * D, LR, MakeMat(Material::Lambert, V3(0, 0, 0), Le, 0.0f));
        UpdateTest(0, 0, 16, 16, 0);
        Hit rec2; int id2;
        suite.require(HitFirst(cam, rec2, id2) && id2 == 0, "light placement must not occlude the primary hit");

        // Policy primitive: same mirror ray, with/without next emission.
        Ray mirrorRay(rec.pos, mdir);
        int r1 = 0; uint32_t st1 = 7u;
        V3 withE = Trace(mirrorRay, 0, r1, st1, true);
        int r0 = 0; uint32_t st0 = 7u;
        V3 withoutE = Trace(mirrorRay, 0, r0, st0, false);
        float dWith = MaxRel3(withE, Le);
        float dWithout = MaxAbs3(withoutE, V3(0, 0, 0));
        std::printf("DIFF spec-policy preserve relLe=%.2e suppressAbs=%.2e with=(%.3f %.3f %.3f)\n",
                    dWith, dWithout, withE.getX(), withE.getY(), withE.getZ());
        suite.require(dWith < 1e-4f, "mirror ray with emission enabled must return the light");
        suite.require(dWithout < 1e-5f, "mirror ray with emission disabled must be suppressed to zero");

        // End-to-end average of the real Trace must be Fo * Le (albedo is black,
        // so diffuse events contribute nothing and spec events return Le).
        const int N = 80000;
        V3 sum(0, 0, 0);
        int rays = 0;
        for (int i = 0; i < N; ++i)
        {
            uint32_t st = (uint32_t)(0x9000u + i * 2654435761u) | 1u;
            sum += Trace(cam, 0, rays, st, true);
        }
        V3 avg = sum * (1.0f / N);
        V3 ref = Le * Fo;
        float rel = MaxRel3(avg, ref, 1e-3f);
        std::printf("DIFF spec-sees-light avg=(%.4f %.4f %.4f) Fo*Le=(%.4f %.4f %.4f) reldiff=%.4f (Fo=%.4f)\n",
                    avg.getX(), avg.getY(), avg.getZ(), ref.getX(), ref.getY(), ref.getZ(), rel, Fo);
        suite.require(rel < 0.08f, "average mirror light must equal F(N.wo)*Le");
    });

    // ------------------------------------------------------------------
    // Event-driven emission wiring + no double count of diffuse direct light.
    suite.run("05_diffuse_event_suppresses_relight_no_double_count", [&]()
    {
        float riCases[2] = { 1.0f, 1.5f };
        for (int rc = 0; rc < 2; ++rc)
        {
            float ri = riCases[rc];
            ResetCustomScene();
            V3 albedo(0.8f, 0.8f, 0.8f);
            SetSphere(0, V3(0, 0, 0), 1.0f, MakeMat(Material::CoatedLambert, albedo, V3(0, 0, 0), ri));
            UpdateTest(0, 0, 16, 16, 0);

            Ray cam = OffsetRay(0.35f); // slightly oblique hit
            Hit rec; int id;
            suite.require(HitFirst(cam, rec, id) && id == 0, "wiring test primary hit");
            V3 n = CoatOrientedNormal(rec.normal, cam);
            V3 wo = -cam.dir;

            // Find a seed that yields a diffuse event pointing well above N.
            uint32_t seed = 0;
            Ray dray; V3 datten; int ev = kScatterNone;
            for (uint32_t s = 1; s < 200000; ++s)
            {
                uint32_t cand = (s * 2654435761u) | 1u;
                uint32_t t = cand; // CoatSampleEvent mutates the state reference
                Ray sr; V3 atten; int e;
                CoatSampleEvent(albedo, ri, rec.pos, n, wo, t, sr, atten, e);
                if (e == kScatterDiffuse && dot(n, sr.dir) > 0.65f)
                {
                    seed = cand; dray = sr; datten = atten; ev = e; break;
                }
            }
            suite.require(seed != 0 && ev == kScatterDiffuse, "must find a usable diffuse seed");

            V3 Le(8, 8, 8);
            SetSphere(1, rec.pos + dray.dir * 3.0f, 1.0f, MakeMat(Material::Lambert, V3(0, 0, 0), Le, 0));
            UpdateTest(0, 0, 16, 16, 0);

            // Full real Trace.
            int raysFull = 0; uint32_t stFull = seed;
            V3 full = Trace(cam, 0, raysFull, stFull, true);

            // Replicate Trace's body using Scatter's reported event flag.
            Hit hc; int idc;
            suite.require(HitWorld(cam, kMinT, kMaxT, hc, idc), "replicate primary hit");
            Ray scat; V3 atten, lightE; int raysB = 0, eventFlag = -1; uint32_t stB = seed;
            bool ok = Scatter(s_SphereMats[idc], cam, hc, atten, scat, lightE, raysB, stB, &eventFlag);
            suite.require(ok && eventFlag == kScatterDiffuse, "Scatter must report diffuse event");
            suite.require(MaxAbs3(atten, datten) < 1e-6f, "Scatter attenuation must match sampled event");
            int raysSub = 0;
            V3 subCorrect = Trace(scat, 1, raysSub, stB, eventFlag == kScatterDiffuse ? false : true);
            V3 expected = lightE + atten * subCorrect; // coat material emission is zero
            float matchErr = MaxAbs3(full, expected);

            // What the OLD type-only rule would have done for CoatedLambert
            // (type != Lambert => keep next emission): double count the light.
            uint32_t stC = seed; int raysC0 = 0;
            Ray scatC; V3 attenC, lightEC; int evC = 0;
            Scatter(s_SphereMats[idc], cam, hc, attenC, scatC, lightEC, raysC0, stC, &evC);
            int raysWrong = 0;
            V3 subCounted = Trace(scatC, 1, raysWrong, stC, true);
            V3 wrong = lightEC + attenC * subCounted;
            float wrongGap = MaxAbs3(wrong, full);

            std::printf("DIFF wiring ri=%.1f matchErr=%.2e doubleCountGap=%.4f subCountedLum=%.3f\n",
                        ri, matchErr, wrongGap, dot(subCounted, V3(0.333f, 0.333f, 0.333f)));
            suite.require(matchErr < 1e-4f, "Trace must use Scatter event flag (suppress diffuse relight)");
            suite.require(wrongGap > 0.5f, "counting next emission after diffuse event must clearly double count");
            suite.require(MaxRel3(subCounted, Le) < 1e-4f, "placed diffuse-direction light must be hit exactly");
        }
    });

    // ------------------------------------------------------------------
    // ri=1 CoatedLambert is numerically identical to Lambert, plus PPM output
    // through Trace and a DrawTest smoke render.
    suite.run("06_ri1_equals_lambert_and_emit_ppm", [&]()
    {
        ResetCustomScene();
        V3 alb(0.66f, 0.42f, 0.24f);
        V3 Le(10.0f, 9.0f, 7.0f);
        SetSphere(0, V3(0, 0, 0), 1.0f, MakeMat(Material::Lambert, alb, V3(0, 0, 0), 0));
        SetSphere(1, V3(2.4f, 2.4f, 2.2f), 0.8f, MakeMat(Material::Lambert, V3(0, 0, 0), Le, 0));
        UpdateTest(0, 0, 32, 24, 0);

        Ray cam = OffsetRay(0.2f);
        int mismatches = 0;
        float worst = 0.0f;
        for (int i = 0; i < 400; ++i)
        {
            uint32_t seed = (uint32_t)(i * 40503u + 12345u) | 1u;
            int rA = 0; uint32_t sA = seed;
            V3 a = Trace(cam, 0, rA, sA, true);

            s_SphereMats[0].type = Material::CoatedLambert;
            s_SphereMats[0].ri = 1.0f;
            int rB = 0; uint32_t sB = seed;
            V3 b = Trace(cam, 0, rB, sB, true);
            s_SphereMats[0].type = Material::Lambert;
            s_SphereMats[0].ri = 0.0f;

            float d = MaxAbs3(a, b);
            worst = fmaxf(worst, d);
            if (d > 1e-5f) ++mismatches;
        }
        std::printf("DIFF lambert-equivalence worstAbs=%.2e mismatches=%d/400\n", worst, mismatches);
        suite.require(mismatches == 0, "ri=1 CoatedLambert must trace identically to Lambert");

        // ---- Small PPM via Trace: painted wood ball + sphere lamp ----
        ResetCustomScene();
        SetSphere(0, V3(0, 0.0f, -0.5f), 1.0f,
                  MakeMat(Material::CoatedLambert, V3(0.66f, 0.36f, 0.15f), V3(0, 0, 0), 1.6f));
        SetSphere(1, V3(2.0f, 3.0f, 2.2f), 1.0f,
                  MakeMat(Material::Lambert, V3(0, 0, 0), V3(13.0f, 11.5f, 8.5f), 0));
        SetSphere(2, V3(0, -1001.0f, -0.5f), 1000.0f,
                  MakeMat(Material::Lambert, V3(0.42f, 0.31f, 0.21f), V3(0, 0, 0), 0));
        UpdateTest(0, 0, 96, 72, 0);

        const int W = 96, H = 72, SPP = 96;
        V3 lo(0.15f, 0.9f, 3.6f), at(0.0f, 0.05f, -0.5f);
        V3 w = normalize(lo - at);
        V3 uu = normalize(cross(V3(0, 1, 0), w));
        V3 vv = cross(w, uu);
        float vfov = 42.0f * kPI / 180.0f;
        float hh = tanf(vfov * 0.5f), hw = hh * (float)W / H;

        std::vector<float> rgb((size_t)W * H * 3, 0.0f);
        int totalRays = 0;
        for (int y = 0; y < H; ++y)
        {
            for (int x = 0; x < W; ++x)
            {
                uint32_t state = (uint32_t)((y * 9781 + x * 6271) | 1u);
                V3 col(0, 0, 0);
                for (int s = 0; s < SPP; ++s)
                {
                    float sx = (x + RandomFloat01(state)) / W;
                    float sy = (y + RandomFloat01(state)) / H;
                    V3 dir = normalize((-w + uu * ((2 * sx - 1) * hw) + vv * ((1 - 2 * sy) * hh)));
                    Ray r(lo, dir);
                    col += Trace(r, 0, totalRays, state, true);
                }
                col *= 1.0f / SPP;
                int o = (y * W + x) * 3;
                rgb[o + 0] = col.getX();
                rgb[o + 1] = col.getY();
                rgb[o + 2] = col.getZ();
            }
        }
        WritePPM("coated_target.ppm", W, H, rgb.data(), 3);
        WritePPM(".build/coated_target.ppm", W, H, rgb.data(), 3);
        float sum = 0; int nonBlack = 0;
        for (size_t i = 0; i < rgb.size(); ++i) { sum += rgb[i]; if (rgb[i] > 0.01f) ++nonBlack; }
        std::printf("PPM trace rays=%d linearSum=%.3f nonzeroChannels=%d\n", totalRays, sum, nonBlack);
        suite.require(sum > 0.0f && nonBlack > W, "Trace PPM must contain a lit render");

        // ---- DrawTest smoke + its own small PPM ----
        s_Cam = Camera(V3(0.15f, 0.9f, 3.6f), V3(0, 0.05f, -0.5f), V3(0, 1, 0),
                       42.0f, 64.0f / 48.0f, 0.0f, 3.4f);
        std::vector<float> px((size_t)64 * 48 * 4, 0.0f);
        int drawRays = 0;
        DrawTest(0, 0, 64, 48, px.data(), drawRays, 0);
        bool finite = true; double dsum = 0;
        for (float v : px) { if (!std::isfinite(v) || v < 0) finite = false; dsum += v; }
        WritePPM("coated_drawtest.ppm", 64, 48, px.data(), 4);
        std::printf("DRAWTEST rays=%d sum=%.3f\n", drawRays, dsum);
        suite.require(finite, "DrawTest pixels must be finite and non-negative");
        suite.require(drawRays > 384, "DrawTest must trace rays");
    });

    ShutdownTest();
}
