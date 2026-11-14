// Tests for the non-local subsurface diffusion material (Cpp/Source/Subsurface.*).
//
// Build & run:  bash task_support/run.sh target
// (this also writes a small DrawTest render to .task-build/subsurface_drawtest.ppm)
//
// The cases below use fixed seeds and compare against independently integrated
// references (double-precision quadrature / independent MC), printing sample
// counts, references, tolerances and observed errors.

#include "task_support/check.h"
#include "Subsurface.h"
#include "Test.h"
#include "Maths.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace {

const double kPiD = 3.14159265358979323846;

struct d3
{
    double x, y, z;
};
static d3 make3(double x, double y, double z) { d3 v = {x, y, z}; return v; }
static d3 operator+(d3 a, d3 b) { return make3(a.x+b.x, a.y+b.y, a.z+b.z); }
static d3 operator-(d3 a, d3 b) { return make3(a.x-b.x, a.y-b.y, a.z-b.z); }
static d3 operator*(d3 a, double s) { return make3(a.x*s, a.y*s, a.z*s); }
static d3 operator*(double s, d3 a) { return make3(a.x*s, a.y*s, a.z*s); }
static d3& operator+=(d3& a, d3 b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
static d3 mul(d3 a, d3 b) { return make3(a.x*b.x, a.y*b.y, a.z*b.z); }
static double dot(d3 a, d3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static d3 cross(d3 a, d3 b) { return make3(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x); }
static double len(d3 a) { return std::sqrt(dot(a, a)); }
static d3 norm(d3 a) { return a * (1.0 / len(a)); }
static d3 f2d(const float3& v) { return make3(v.getX(), v.getY(), v.getZ()); }
static float3 d2f(d3 v) { return float3((float)v.x, (float)v.y, (float)v.z); }
static double comp(const float3& v, int c) { return c == 0 ? v.getX() : (c == 1 ? v.getY() : v.getZ()); }

// Independent (splitmix64) random stream for reference computations.
struct Rng64
{
    uint64_t s;
    explicit Rng64(uint64_t seed) : s(seed) {}
    uint64_t next()
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double next01() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
};

static d3 randomUnit(Rng64& rng)
{
    double z = 2.0 * rng.next01() - 1.0;
    double phi = 2.0 * kPiD * rng.next01();
    double r = std::sqrt(std::max(0.0, 1.0 - z*z));
    return make3(r * std::cos(phi), r * std::sin(phi), z);
}

// Rodrigues rotation of v around unit axis by angle.
static d3 rotate(d3 v, d3 axis, double angle)
{
    double c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}

// Independent re-implementation of the radial kernel S_d(r).
static double ref_kernel(double d, double r)
{
    double d2 = d * d;
    return 0.5 * std::exp(-r*r / (2.0*d2)) / (2.0*kPiD*d2)
         + 0.5 * std::exp(-r*r / (18.0*d2)) / (18.0*kPiD*d2);
}

// Candidate count of the line p + t*a against the sphere.
static int ref_line_count(d3 p, d3 a, d3 C, double R)
{
    d3 oc = p - C;
    double b = dot(oc, a);
    double c = dot(oc, oc) - R * R;
    double disc = b * b - c;
    if (disc > 1e-12) return 2;
    if (disc > -1e-12) return 1;
    return 0;
}

// Independent re-implementation of the full mixture area PDF, using the
// public SubsurfaceFrame for the projection axes.
static double ref_pdf(d3 C, double R, const double d[3], d3 xo, d3 no, d3 xi)
{
    float3 t1f, t2f;
    SubsurfaceFrame(d2f(no), t1f, t2f);
    d3 axes[3] = { no, f2d(t1f), f2d(t2f) };
    const double pa[3] = {0.5, 0.25, 0.25};
    d3 ni = norm(xi - C);
    d3 dff = xi - xo;
    double pdf = 0.0;
    for (int j = 0; j < 3; ++j)
    {
        d3 a = axes[j];
        d3 perp = dff - a * dot(dff, a);
        int n = ref_line_count(xo + perp, a, C, R);
        if (n == 0)
            continue;
        double r = len(perp);
        double sk = 0.0;
        for (int c = 0; c < 3; ++c)
            sk += ref_kernel(d[c], r);
        sk /= 3.0;
        pdf += pa[j] * sk * std::fabs(dot(ni, a)) / (double)n;
    }
    return pdf;
}

// Midpoint quadrature of f over the sphere surface: returns integral of f dA.
template <typename F>
static double sphere_quad(d3 C, double R, int nth, int nph, F&& f)
{
    double sum = 0.0;
    for (int it = 0; it < nth; ++it)
    {
        double theta = (it + 0.5) * kPiD / nth;
        double st = std::sin(theta), ct = std::cos(theta);
        for (int ip = 0; ip < nph; ++ip)
        {
            double phi = (ip + 0.5) * 2.0 * kPiD / nph;
            d3 xi = C + R * make3(st * std::cos(phi), st * std::sin(phi), ct);
            sum += f(xi) * st;
        }
    }
    return sum * R * R * (kPiD / nth) * (2.0 * kPiD / nph);
}

// Smallest ray parameter t > tMin of a ray-sphere intersection, or -1.
static double ray_sphere(d3 o, d3 dir, d3 C, double R, double tMin)
{
    d3 oc = o - C;
    double b = dot(oc, dir);
    double c = dot(oc, oc) - R * R;
    double disc = b * b - c;
    if (disc <= 0.0)
        return -1.0;
    double sq = std::sqrt(disc);
    double t = -b - sq;
    if (t <= tMin)
        t = -b + sq;
    if (t <= tMin)
        return -1.0;
    return t;
}

struct RefSphere
{
    d3 C;
    double R;
};

// Independent quadrature of the direct sphere-light irradiance factor at xi:
// E(xi) = emissive/pi * integral over the light cone of max(0,cos)*unoccluded dw.
static d3 ref_direct_E(d3 xi, d3 ni, const RefSphere& light, d3 emissive,
                       const RefSphere* blockers, int nblockers)
{
    d3 w = norm(light.C - xi);
    double d2 = dot(light.C - xi, light.C - xi);
    double cosAMax = std::sqrt(1.0 - light.R * light.R / d2);
    double aMax = std::acos(cosAMax);
    d3 up = std::fabs(w.x) > 0.9 ? make3(0, 1, 0) : make3(1, 0, 0);
    d3 u = norm(cross(w, up));
    d3 v = cross(w, u);
    const int na = 30, np = 60;
    d3 sum = make3(0, 0, 0);
    for (int ia = 0; ia < na; ++ia)
    {
        double a = (ia + 0.5) * aMax / na;
        double sa = std::sin(a), ca = std::cos(a);
        for (int ip = 0; ip < np; ++ip)
        {
            double phi = (ip + 0.5) * 2.0 * kPiD / np;
            d3 l = u * (std::cos(phi) * sa) + v * (std::sin(phi) * sa) + w * ca;
            double cosT = dot(l, ni);
            if (cosT <= 0.0)
                continue;
            double tL = ray_sphere(xi, l, light.C, light.R, 1e-3);
            if (tL < 0.0)
                continue;
            bool blocked = false;
            for (int b = 0; b < nblockers; ++b)
            {
                double tB = ray_sphere(xi, l, blockers[b].C, blockers[b].R, 1e-3);
                if (tB > 0.0 && tB < tL - 1e-6)
                {
                    blocked = true;
                    break;
                }
            }
            if (blocked)
                continue;
            sum += emissive * (cosT * sa);
        }
    }
    return sum * ((aMax / na) * (2.0 * kPiD / np) / kPiD);
}

static SubsurfaceSceneMaterial makeMat(int type, float a0, float a1, float a2,
                                       float e0, float e1, float e2, float rough, float ri)
{
    SubsurfaceSceneMaterial m;
    m.type = type;
    m.albedo[0] = a0; m.albedo[1] = a1; m.albedo[2] = a2;
    m.emissive[0] = e0; m.emissive[1] = e1; m.emissive[2] = e2;
    m.roughness = rough;
    m.ri = ri;
    return m;
}

static SubsurfaceParams makeParams(float r0, float r1, float r2, float d0, float d1, float d2)
{
    SubsurfaceParams p;
    p.rho = float3(r0, r1, r2);
    p.d = float3(d0, d1, d2);
    return p;
}

// ---------------------------------------------------------------- density

int case_kernel_radial()
{
    // kernel values vs. the independent formula
    const double ds[3] = {0.05, 0.3, 1.7};
    double maxrel = 0.0;
    int nvals = 0;
    for (int di = 0; di < 3; ++di)
    {
        double d = ds[di];
        for (int i = 0; i <= 400; ++i)
        {
            double r = 6.0 * d * i / 400.0;
            double ref = ref_kernel(d, r);
            double got = SubsurfaceKernel((float)d, (float)r);
            maxrel = std::max(maxrel, std::fabs(got - ref) / std::max(ref, 1e-30));
            ++nvals;
        }
    }
    double tolVal = 1e-3;
    std::printf("density/kernel_radial values n=%d maxrel=%.3e tol=%.1e\n", nvals, maxrel, tolVal);
    TASK_CHECK(maxrel < tolVal);

    // planar normalization of the kernel mixture: integral of S_d(r) 2 pi r dr == 1
    for (int di = 0; di < 3; ++di)
    {
        double d = ds[di];
        double rmax = 20.0 * d;
        const int n = 200000;
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            double r = (i + 0.5) * rmax / n;
            sum += ref_kernel(d, r) * 2.0 * kPiD * r;
        }
        sum *= rmax / n;
        double err = std::fabs(sum - 1.0);
        std::printf("density/kernel_radial planar_norm d=%.2f ref=1.0 err=%.3e tol=1e-5\n", d, err);
        TASK_CHECK(err < 1e-5);
    }
    return 0;
}

int case_area_pdf_quadrature()
{
    d3 C = make3(0.3, -0.2, 0.5);
    double R = 0.8;
    d3 no = norm(make3(0.35, 0.6, 1.0)); // tilted exit normal: all 3 axes mix
    d3 xo = C + R * no;
    const double dd[3] = {0.1, 0.3, 0.9};
    SubsurfaceParams params = makeParams(0.6f, 0.5f, 0.4f, 0.1f, 0.3f, 0.9f);
    float3 Cf = d2f(C), xof = d2f(xo), nof = d2f(no);

    // independent spherical quadrature of the library area PDF
    const int nth = 400, nph = 800;
    double mass = 0.0, minPdf = 1e30, maxFormulaRel = 0.0;
    int formulaChecks = 0;
    for (int it = 0; it < nth; ++it)
    {
        double theta = (it + 0.5) * kPiD / nth;
        double st = std::sin(theta), ct = std::cos(theta);
        for (int ip = 0; ip < nph; ++ip)
        {
            double phi = (ip + 0.5) * 2.0 * kPiD / nph;
            d3 xi = C + R * make3(st * std::cos(phi), st * std::sin(phi), ct);
            float pdf = SubsurfaceRelocationPdf(Cf, (float)R, params, xof, nof, d2f(xi));
            minPdf = std::min(minPdf, (double)pdf);
            mass += pdf * st;
            if ((it * 7 + ip) % 101 == 0)
            {
                double ref = ref_pdf(C, R, dd, xo, no, xi);
                if (ref > 1e-9)
                {
                    maxFormulaRel = std::max(maxFormulaRel, std::fabs(pdf - ref) / ref);
                    ++formulaChecks;
                }
            }
        }
    }
    mass *= R * R * (kPiD / nth) * (2.0 * kPiD / nph);

    // Monte-Carlo zero-contribution probability from the public sampler
    const int M = 200000;
    uint32_t state = 0x00C0FFEEu;
    int invalid = 0;
    for (int i = 0; i < M; ++i)
    {
        SubsurfaceSample s = SubsurfaceSampleRelocation(Cf, (float)R, 0, params, xof, nof, state);
        if (!s.valid)
            ++invalid;
    }
    double z = (double)invalid / M;
    double sigZ = std::sqrt(z * (1.0 - z) / M);
    double tol = 5.0 * sigZ + 5e-3;
    double err = std::fabs(mass - (1.0 - z));
    std::printf("density/area_pdf_quadrature quad_nodes=%d mc_samples=%d mass=%.6f zero_prob=%.6f "
                "ref(1-zero)=%.6f err=%.3e tol=%.3e formula_maxrel=%.3e\n",
                nth * nph, M, mass, z, 1.0 - z, err, tol, maxFormulaRel);
    TASK_CHECK(minPdf >= 0.0);
    TASK_CHECK(formulaChecks > 1000);
    TASK_CHECK(maxFormulaRel < 2e-3);
    TASK_CHECK(mass > 0.05 && mass <= 1.0 + 1e-3);
    TASK_CHECK(err < tol);
    return 0;
}

// ---------------------------------------------------------------- relocation

int case_sample_pdf_formula()
{
    d3 C = make3(1.0, -2.0, 3.0);
    double R = 1.3;
    const double dsets[3][3] = {{0.05, 0.4, 1.2}, {0.3, 0.3, 0.3}, {1.0, 0.1, 0.02}};
    Rng64 rng(0x123456789abcdefULL);
    double maxrel = 0.0;
    int n = 0, nTiny = 0;
    for (int t = 0; t < 400; ++t)
    {
        // random configuration, then rotated by a random rotation: the axis
        // mixture must still be assembled correctly in the rotated frame
        d3 no0 = randomUnit(rng);
        d3 xi0 = randomUnit(rng);
        d3 axis = randomUnit(rng);
        double ang = rng.next01() * 2.0 * kPiD;
        d3 no = rotate(no0, axis, ang);
        d3 xi = C + R * rotate(xi0, axis, ang);
        d3 xo = C + R * no;
        const double* dd = dsets[t % 3];
        SubsurfaceParams params = makeParams(0.7f, 0.5f, 0.3f, (float)dd[0], (float)dd[1], (float)dd[2]);
        float pdf = SubsurfaceRelocationPdf(d2f(C), (float)R, params, d2f(xo), d2f(no), d2f(xi));
        double ref = ref_pdf(C, R, dd, xo, no, xi);
        TASK_CHECK(pdf >= 0.0f);
        if (ref > 1e-9)
        {
            maxrel = std::max(maxrel, std::fabs(pdf - ref) / ref);
            ++n;
        }
        else
        {
            ++nTiny;
        }
    }
    std::printf("relocation/sample_pdf_formula trials=400 compared=%d tiny=%d maxrel=%.3e tol=2e-3\n",
                n, nTiny, maxrel);
    TASK_CHECK(n > 350);
    TASK_CHECK(maxrel < 2e-3);

    // weight: rho_c * S_{d_c}(euclidean chord) / pdf per channel
    d3 no = norm(make3(0.2, -0.5, 1.0));
    d3 xo = C + R * no;
    d3 xi = C + R * norm(make3(-0.4, 0.3, 1.0));
    const double dd[3] = {0.2, 0.5, 0.9};
    SubsurfaceParams params = makeParams(0.7f, 0.5f, 0.3f, 0.2f, 0.5f, 0.9f);
    float pdf = SubsurfaceRelocationPdf(d2f(C), (float)R, params, d2f(xo), d2f(no), d2f(xi));
    TASK_CHECK(pdf > 0.0f);
    float3 w = SubsurfaceWeight(params, d2f(xo), d2f(xi), pdf);
    double chord = len(xi - xo);
    double rho[3] = {0.7, 0.5, 0.3};
    double maxWRel = 0.0;
    for (int c = 0; c < 3; ++c)
    {
        double refW = rho[c] * ref_kernel(dd[c], chord) / pdf;
        maxWRel = std::max(maxWRel, std::fabs(comp(w, c) - refW) / refW);
    }
    std::printf("relocation/sample_pdf_formula weight pdf=%.6e maxrel=%.3e tol=1e-3\n", pdf, maxWRel);
    TASK_CHECK(maxWRel < 1e-3);
    return 0;
}

int case_cap_consistency()
{
    d3 C = make3(0, 0, 0);
    double R = 1.0;
    d3 rotAxis = norm(make3(1.0, 2.0, 3.0));
    double rotAng = 63.0 * kPiD / 180.0;
    d3 no = rotate(norm(make3(0.4, 0.55, 1.0)), rotAxis, rotAng); // rotated exit point
    d3 xo = C + R * no;
    d3 capd = norm(rotate(norm(make3(-0.2, 0.3, -1.0)), rotAxis, rotAng));
    double cosCap = std::cos(25.0 * kPiD / 180.0);
    SubsurfaceParams params = makeParams(0.7f, 0.5f, 0.3f, 0.15f, 0.45f, 0.9f);
    float3 Cf = d2f(C), xof = d2f(xo), nof = d2f(no);

    // reference: integral of the area PDF over the cap, by spherical quadrature
    const int nth = 600, nph = 1200;
    double ref = sphere_quad(C, R, nth, nph, [&](d3 xi) -> double {
        if (dot(norm(xi - C), capd) <= cosCap)
            return 0.0;
        return (double)SubsurfaceRelocationPdf(Cf, (float)R, params, xof, nof, d2f(xi));
    });

    // Monte-Carlo estimate from the public sampler; candidates must stay on
    // the same sphere with consistent normals and identity
    const int N = 300000;
    uint32_t state = 0x00BEEF01u;
    int hit = 0;
    double maxRadErr = 0.0, maxNrmErr = 0.0;
    for (int i = 0; i < N; ++i)
    {
        SubsurfaceSample s = SubsurfaceSampleRelocation(Cf, (float)R, 7, params, xof, nof, state);
        if (!s.valid)
            continue;
        d3 p = f2d(s.pos);
        maxRadErr = std::max(maxRadErr, std::fabs(len(p - C) - R));
        maxNrmErr = std::max(maxNrmErr, len(f2d(s.normal) - norm(p - C)));
        TASK_CHECK(s.sphereId == 7);
        if (dot(norm(p - C), capd) > cosCap)
            ++hit;
    }
    double phat = (double)hit / N;
    double sig = std::sqrt(phat * (1.0 - phat) / N);
    double tol = 5.0 * sig + 4e-3;
    double err = std::fabs(phat - ref);
    std::printf("relocation/cap_consistency N=%d quad_nodes=%d ref=%.6f est=%.6f err=%.3e tol=%.3e "
                "rad_err=%.2e nrm_err=%.2e\n",
                N, nth * nph, ref, phat, err, tol, maxRadErr, maxNrmErr);
    TASK_CHECK(ref > 1e-4);
    TASK_CHECK(maxRadErr < 1e-3);
    TASK_CHECK(maxNrmErr < 1e-3);
    TASK_CHECK(err < tol);
    return 0;
}

// ---------------------------------------------------------------- transport

int case_constant_sky_single_ray()
{
    SubsurfaceSceneSphere sph[1] = { {{0, 0, 0}, 1.0f} };
    SubsurfaceSceneMaterial mat[1] = { makeMat(3, 0, 0, 0, 0, 0, 0, 0, 1) };
    SubsurfaceParams prm[1] = { makeParams(0.8f, 0.6f, 0.4f, 0.35f, 0.2f, 0.55f) };
    TASK_CHECK(SubsurfaceSetScene(sph, mat, prm, 1));
    SubsurfaceSetSky(0.5f, 0.7f, 0.9f);

    float3 org(0, 0, 5), dir(0, 0, -1);

    // invalid parameters must be rejected BEFORE the scene is modified
    SubsurfaceParams bad[1] = { makeParams(0.8f, 0.6f, 0.4f, 0.3f, -1.0f, 0.2f) };
    uint32_t chkA = 424242u, chkB = 424242u;
    int raysA = 0, raysB = 0;
    float3 before = SubsurfaceTraceRay(org, dir, chkA, raysA);
    TASK_CHECK(!SubsurfaceSetScene(sph, mat, bad, 1));
    SubsurfaceParams nanPrm[1] = { makeParams(0.8f, 0.6f, 0.4f, 0.3f, (float)NAN, 0.2f) };
    TASK_CHECK(!SubsurfaceSetScene(sph, mat, nanPrm, 1));
    float3 after = SubsurfaceTraceRay(org, dir, chkB, raysB);
    TASK_CHECK(before.getX() == after.getX() && before.getY() == after.getY() && before.getZ() == after.getZ());

    // independent reference: Lo_c = rho_c * sky_c * integral of S_{d_c}(chord) dA
    d3 xo = make3(0, 0, 1);
    const double rhos[3] = {0.8, 0.6, 0.4};
    const double ds[3] = {0.35, 0.2, 0.55};
    const double sky[3] = {0.5, 0.7, 0.9};
    const int nth = 400, nph = 800;
    double ref[3];
    for (int c = 0; c < 3; ++c)
        ref[c] = rhos[c] * sky[c] * sphere_quad(make3(0, 0, 0), 1.0, nth, nph,
                                                [&](d3 xi) { return ref_kernel(ds[c], len(xi - xo)); });

    // real single-ray radiance through the production tracer
    const int N = 40000;
    uint32_t state = 777u;
    int rays = 0;
    double mean[3] = {0, 0, 0}, m2[3] = {0, 0, 0};
    for (int i = 0; i < N; ++i)
    {
        float3 L = SubsurfaceTraceRay(org, dir, state, rays);
        for (int c = 0; c < 3; ++c)
        {
            double v = comp(L, c);
            double delta = v - mean[c];
            mean[c] += delta / (i + 1);
            m2[c] += delta * (v - mean[c]);
        }
    }
    bool allOk = true;
    for (int c = 0; c < 3; ++c)
    {
        double sigma = std::sqrt(m2[c] / (N - 1) / N);
        double tol = 5.0 * sigma + 5e-3 * std::fabs(ref[c]) + 1e-5;
        double err = std::fabs(mean[c] - ref[c]);
        std::printf("transport/constant_sky_single_ray ch=%d N=%d ref=%.6f mean=%.6f err=%.3e tol=%.3e\n",
                    c, N, ref[c], mean[c], err, tol);
        allOk = allOk && (err < tol) && (mean[c] > 0.3 * ref[c]);
    }
    std::printf("transport/constant_sky_single_ray quad_nodes=%d rays=%d\n", nth * nph, rays);
    TASK_CHECK(allOk);
    return 0;
}

int case_backside_light_occlusion()
{
    // subsurface sphere lit by a sphere light on its far side, partially
    // occluded; sky is black so only the non-local term can light the front
    SubsurfaceSceneSphere sph[3] = {
        {{0, 0, 0}, 1.0f},          // 0: subsurface
        {{0, 0.5f, -4}, 1.0f},      // 1: light (behind)
        {{1.2f, 0.4f, -2.5f}, 0.4f} // 2: occluder
    };
    SubsurfaceSceneMaterial mats[3] = {
        makeMat(3, 0, 0, 0, 0, 0, 0, 0, 1),   // subsurface
        makeMat(0, 0, 0, 0, 4, 3, 2, 0, 1),   // light: black Lambert, emissive
        makeMat(0, 0, 0, 0, 0, 0, 0, 0, 1)    // occluder: black Lambert
    };
    SubsurfaceParams prm[3];
    prm[0] = makeParams(0.7f, 0.5f, 0.3f, 0.4f, 0.25f, 0.6f);
    prm[1] = prm[0];
    prm[2] = prm[0];
    TASK_CHECK(SubsurfaceSetScene(sph, mats, prm, 3));
    SubsurfaceSetSky(0, 0, 0);

    d3 xo = make3(0, 0, 1);
    const double ds[3] = {0.4, 0.25, 0.6};
    const double rhos[3] = {0.7, 0.5, 0.3};
    d3 emissive = make3(4, 3, 2);
    RefSphere light = { make3(0, 0.5, -4), 1.0 };
    RefSphere blockers[2] = { { make3(0, 0, 0), 1.0 }, { make3(1.2, 0.4, -2.5), 0.4 } };

    // non-locality proof: the exit point itself cannot see the light at all
    // (the whole light cone is below its tangent plane), so any radiance at
    // xo must arrive through the non-local term
    d3 Exo = ref_direct_E(xo, norm(xo), light, emissive, blockers, 2);
    std::printf("transport/backside_light_occlusion local_E_at_xo=%.3e (must be 0)\n", len(Exo));
    TASK_CHECK(len(Exo) == 0.0);

    // independent reference: Lo_c = integral rho_c S_{d_c}(chord) E_c(xi) dA,
    // with E(xi) from independent cone quadrature (occlusion included). The
    // light emission is counted exactly once: via E(xi). A tracer that also
    // added emission when the continuation ray hits the light would
    // systematically overshoot this reference.
    const int nth = 160, nph = 320;
    double ref[3] = {0, 0, 0};
    for (int it = 0; it < nth; ++it)
    {
        double theta = (it + 0.5) * kPiD / nth;
        double st = std::sin(theta), ct = std::cos(theta);
        for (int ip = 0; ip < nph; ++ip)
        {
            double phi = (ip + 0.5) * 2.0 * kPiD / nph;
            d3 xi = make3(st * std::cos(phi), st * std::sin(phi), ct);
            d3 E = ref_direct_E(xi, norm(xi), light, emissive, blockers, 2);
            double chord = len(xi - xo);
            for (int c = 0; c < 3; ++c)
                ref[c] += rhos[c] * ref_kernel(ds[c], chord) * comp(d2f(E), c) * st;
        }
    }
    double dA = 1.0 * 1.0 * (kPiD / nth) * (2.0 * kPiD / nph);
    for (int c = 0; c < 3; ++c)
        ref[c] *= dA;

    const int N = 200000;
    uint32_t state = 20261114u;
    int rays = 0;
    float3 org(0, 0, 5), dir(0, 0, -1);
    double mean[3] = {0, 0, 0}, m2[3] = {0, 0, 0};
    for (int i = 0; i < N; ++i)
    {
        float3 L = SubsurfaceTraceRay(org, dir, state, rays);
        for (int c = 0; c < 3; ++c)
        {
            double v = comp(L, c);
            double delta = v - mean[c];
            mean[c] += delta / (i + 1);
            m2[c] += delta * (v - mean[c]);
        }
    }
    bool allOk = true;
    for (int c = 0; c < 3; ++c)
    {
        double sigma = std::sqrt(m2[c] / (N - 1) / N);
        double tol = 5.0 * sigma + 0.02 * std::fabs(ref[c]) + 1e-5;
        double err = std::fabs(mean[c] - ref[c]);
        std::printf("transport/backside_light_occlusion ch=%d N=%d ref=%.6f mean=%.6f err=%.3e tol=%.3e\n",
                    c, N, ref[c], mean[c], err, tol);
        allOk = allOk && (err < tol);
    }
    std::printf("transport/backside_light_occlusion quad_nodes=%d rays=%d\n", nth * nph, rays);
    TASK_CHECK(ref[0] > 0.005); // non-trivial non-local signal
    TASK_CHECK(allOk);
    return 0;
}

int case_specular_keeps_light()
{
    // mirror sphere reflecting the camera ray straight into a light:
    // specular paths keep the emission they see
    SubsurfaceSceneSphere sph[2] = {
        {{0, 0, 0}, 1.0f},  // 0: mirror
        {{0, 0, 8}, 1.0f}   // 1: light
    };
    SubsurfaceSceneMaterial mats[2] = {
        makeMat(1, 0.6f, 0.7f, 0.8f, 0, 0, 0, 0, 1), // metal, roughness 0
        makeMat(0, 0, 0, 0, 2, 3, 4, 0, 1)           // light
    };
    TASK_CHECK(SubsurfaceSetScene(sph, mats, nullptr, 2));
    SubsurfaceSetSky(0, 0, 0);

    uint32_t state = 55u;
    int rays = 0;
    float3 L = SubsurfaceTraceRay(float3(0, 0, 5), float3(0, 0, -1), state, rays);
    // expected: albedo * emissive = (1.2, 2.1, 3.2)
    double expL[3] = {1.2, 2.1, 3.2};
    double err = std::max(std::fabs(L.getX() - expL[0]),
                          std::max(std::fabs(L.getY() - expL[1]), std::fabs(L.getZ() - expL[2])));
    std::printf("transport/specular_keeps_light ref=(1.2,2.1,3.2) got=(%.6f,%.6f,%.6f) err=%.3e tol=1e-4\n",
                L.getX(), L.getY(), L.getZ(), err);
    TASK_CHECK(err < 1e-4);
    return 0;
}

int case_drawtest_ppm()
{
    InitializeTest();

    SubsurfaceSceneSphere sph[4] = {
        {{0, -100.5f, -1}, 100.0f}, // 0: ground
        {{0, 0, 0}, 1.0f},          // 1: subsurface ball
        {{0, 1.5f, -4}, 1.0f},      // 2: light behind the ball
        {{2.2f, 0, 1}, 0.5f}        // 3: metal side ball
    };
    SubsurfaceSceneMaterial mats[4] = {
        makeMat(0, 0.7f, 0.7f, 0.7f, 0, 0, 0, 0, 1),
        makeMat(3, 0, 0, 0, 0, 0, 0, 0, 1),
        makeMat(0, 0, 0, 0, 30, 26, 22, 0, 1),
        makeMat(1, 0.5f, 0.6f, 0.8f, 0, 0, 0, 0.1f, 1)
    };
    SubsurfaceParams prm[4];
    prm[1] = makeParams(0.85f, 0.55f, 0.4f, 0.5f, 0.35f, 0.25f);
    prm[0] = prm[2] = prm[3] = prm[1];
    TASK_CHECK(SubsurfaceSetScene(sph, mats, prm, 4));
    SubsurfaceSetSky(0.08f, 0.10f, 0.14f);
    float lookFrom[3] = {0, 1.0f, 4.0f};
    float lookAt[3] = {0, 0.2f, 0};
    float up[3] = {0, 1, 0};
    SubsurfaceSetCamera(lookFrom, lookAt, up, 50.0f, 0.0f, 4.0f);

    const int w = 64, h = 48;
    UpdateTest(0, 0, w, h, 0);
    std::vector<float> img(w * h * 4, 0.0f);
    int rays = 0;
    DrawTest(0, 0, w, h, img.data(), rays, 0);
    TASK_CHECK(rays > 0);
    double sum = 0.0;
    bool finite = true;
    for (int i = 0; i < w * h; ++i)
    {
        for (int c = 0; c < 3; ++c)
        {
            float v = img[i * 4 + c];
            if (!std::isfinite(v) || v < 0.0f)
                finite = false;
            sum += v;
        }
    }
    TASK_CHECK(finite);
    TASK_CHECK(sum > 1.0);

    const char* path = ".task-build/subsurface_drawtest.ppm";
    FILE* f = std::fopen(path, "wb");
    if (!f)
    {
        path = "subsurface_drawtest.ppm";
        f = std::fopen(path, "wb");
    }
    TASK_CHECK(f != nullptr);
    if (f)
    {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int i = 0; i < w * h; ++i)
        {
            unsigned char rgb[3];
            for (int c = 0; c < 3; ++c)
            {
                float v = std::min(1.0f, std::max(0.0f, img[i * 4 + c]));
                rgb[c] = (unsigned char)(255.99f * std::pow(v, 1.0f / 2.2f));
            }
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }
    std::printf("transport/drawtest_ppm %dx%d rays=%d sum=%.3f ppm=%s "
                "(view with: open %s)\n", w, h, rays, sum, path, path);

    SubsurfaceResetScene();
    SubsurfaceResetSky();
    ShutdownTest();
    return 0;
}

} // anonymous namespace

const task_case task_cases[] = {
    { "density", "kernel_radial", case_kernel_radial },
    { "density", "area_pdf_quadrature", case_area_pdf_quadrature },
    { "relocation", "sample_pdf_formula", case_sample_pdf_formula },
    { "relocation", "cap_consistency", case_cap_consistency },
    { "transport", "constant_sky_single_ray", case_constant_sky_single_ray },
    { "transport", "backside_light_occlusion", case_backside_light_occlusion },
    { "transport", "specular_keeps_light", case_specular_keeps_light },
    { "transport", "drawtest_ppm", case_drawtest_ppm },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
