/*
 * Target checks for the LTC rectangular area light feature.
 *
 * Groups:
 *  - lookup:   LTC table layout, axes (index a + t*64), column-major matrix
 *              orientation and bilinear interpolation of tabMinv/tabMagnitude.
 *  - integral: clipped spherical-polygon integrals (diffuse cosine lobe and
 *              transformed LTC density) against an independent deterministic
 *              rectangular area quadrature. Scenarios: crossing the horizon,
 *              grazing, backface, rotated/translated.
 *  - material: real Renderer::draw draws of the pbr shading component with
 *              synthetic geometry and in-memory textures: normal mapping,
 *              metallic/dielectric, roughness, light accumulation, float HDR.
 *
 * Reference integration: midpoint quadrature over the light rectangle,
 * resolution 1024x1024 samples (REF_RES). Refinement to 4096x4096 changes
 * results by <= 3e-6 in all scenarios below, so the quadrature error is
 * far below the comparison tolerance REF_TOL = 1e-3. The reference
 * integrates the cosine lobe and the LTC density
 *   D(w) = Do(Minv*w/|Minv*w|) * |det Minv| / |Minv*w|^3, Do = max(z,0)/pi
 * over the light area; it does not use the spherical-edge integration
 * under test.
 */

#include "task_support/check.h"

#include "apps/11_physically_based/pbr_shading.h"

#include <renderer.h>
#include <sampler.h>
#include <math/matrix4.h>

#include <cmath>
#include <vector>
#include <tuple>

namespace
{

constexpr double PI_D = 3.14159265358979323846;

/* quadrature resolution and comparison tolerance (see file header) */
constexpr int REF_RES = 1024;
constexpr double REF_TOL = 1e-3;

bool near(double a, double b, double tol)
{
    return std::abs(a - b) <= tol;
}

/* ---------------- double precision helpers for the reference ---------------- */

struct DVec3
{
    double x, y, z;
};

DVec3 operator-(const DVec3& a, const DVec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
DVec3 operator+(const DVec3& a, const DVec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
DVec3 operator*(const DVec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const DVec3& a, const DVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
DVec3 cross(const DVec3& a, const DVec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(const DVec3& a) { return std::sqrt(dot(a, a)); }
DVec3 norm(const DVec3& a) { return a * (1.0 / len(a)); }
DVec3 dv(const Vec3& v) { return {v.x, v.y, v.z}; }

/*
 * Independent reference: deterministic midpoint quadrature over the light
 * rectangle. The density matrix is assembled directly from the raw table
 * data (documented layout: column-major m[9], row index a + t*64), not from
 * the component's lookup result.
 */
void reference_integrals(const pbr::RectLight& light, const Vec3& P,
                         const Vec3& N, const Vec3& V, float roughness,
                         double& out_diffuse, double& out_specular)
{
    const DVec3 n = norm(dv(N));
    const DVec3 v = norm(dv(V));

    /* local frame: view direction projected on the tangent plane */
    DVec3 t, b;
    const DVec3 proj = v - n * dot(n, v);
    if(dot(proj, proj) > 1e-10)
        t = norm(proj);
    else
    {
        const DVec3 up = std::abs(n.z) < 0.999 ? DVec3{0, 0, 1} : DVec3{1, 0, 0};
        t = norm(cross(up, n));
    }
    b = cross(n, t);

    /* bilinear interpolation of the raw tabMinv entries (column-major) */
    const float u = std::clamp(roughness, 0.0f, 1.0f) * 63.0f;
    const float w_ = std::sqrt(1.0f - std::clamp(static_cast<float>(dot(n, v)), 0.0f, 1.0f)) * 63.0f;
    const int a0 = std::min(static_cast<int>(u), 62);
    const int t0 = std::min(static_cast<int>(w_), 62);
    const float fa = u - static_cast<float>(a0);
    const float ft = w_ - static_cast<float>(t0);

    double A[3][3];
    for(int k = 0; k < 9; k++)
    {
        const int i00 = a0 + t0 * 64, i10 = a0 + 1 + t0 * 64;
        const int i01 = a0 + (t0 + 1) * 64, i11 = a0 + 1 + (t0 + 1) * 64;
        const double m0 = pbr::ltc_table::tabMinv[i00].m[k] + (pbr::ltc_table::tabMinv[i10].m[k] - pbr::ltc_table::tabMinv[i00].m[k]) * fa;
        const double m1 = pbr::ltc_table::tabMinv[i01].m[k] + (pbr::ltc_table::tabMinv[i11].m[k] - pbr::ltc_table::tabMinv[i01].m[k]) * fa;
        A[k % 3][k / 3] = m0 + (m1 - m0) * ft;
    }
    const double detA = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1])
                      - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0])
                      + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);

    const DVec3 c0 = dv(light.corners[0]);
    const DVec3 e1 = dv(light.corners[1]) - c0;
    const DVec3 e2 = dv(light.corners[3]) - c0;
    const DVec3 nl = cross(e1, e2);
    const double nll = len(nl);
    const double dA = nll / (static_cast<double>(REF_RES) * REF_RES);
    const DVec3 Pd = dv(P);

    double diffuse = 0.0, specular = 0.0;
    for(int i = 0; i < REF_RES; i++)
    {
        for(int j = 0; j < REF_RES; j++)
        {
            const DVec3 Q = c0 + e1 * ((i + 0.5) / REF_RES) + e2 * ((j + 0.5) / REF_RES);
            const DVec3 d = Q - Pd;
            const double r = len(d);
            const DVec3 w = d * (1.0 / r);

            /* solid angle element (unsigned: light emits from both sides here) */
            const double dw = std::abs(dot(nl, d)) / (nll * r) / (r * r) * dA;

            const double cosR = dot(n, w);
            if(cosR > 0.0)
                diffuse += cosR / PI_D * dw;

            const double wl[3] = {dot(w, t), dot(w, b), dot(w, n)};
            const double tw[3] = {
                A[0][0] * wl[0] + A[0][1] * wl[1] + A[0][2] * wl[2],
                A[1][0] * wl[0] + A[1][1] * wl[1] + A[1][2] * wl[2],
                A[2][0] * wl[0] + A[2][1] * wl[1] + A[2][2] * wl[2]};
            const double tl = std::sqrt(tw[0] * tw[0] + tw[1] * tw[1] + tw[2] * tw[2]);
            const double zo = tw[2] / tl;
            if(zo > 0.0)
                specular += zo / PI_D * std::abs(detA) / (tl * tl * tl) * dw;
        }
    }

    out_diffuse = diffuse;
    out_specular = specular;
}

/* evaluates the component exactly the way pbr::rect_lighting does */
pbr::LightSample component_sample(const pbr::RectLight& light, const Vec3& P,
                                  const Vec3& N, const Vec3& V, float roughness)
{
    const Vec3 n = normalize(N);
    const Vec3 v = normalize(V);
    const pbr::LtcTerms terms = pbr::ltc_terms(roughness, dot(n, v));
    return pbr::evaluate_rect_light(light, P, n, v, terms);
}

/* ------------------------------- lookup group ------------------------------- */

int case_lookup_table_layout()
{
    using namespace pbr::ltc_table;

    TASK_CHECK(size == 64);

    for(int i = 0; i < 64 * 64; i++)
    {
        for(int k = 0; k < 9; k++)
        {
            TASK_CHECK(std::isfinite(tabM[i].m[k]));
            TASK_CHECK(std::isfinite(tabMinv[i].m[k]));
        }
        /* the fitted lobe decouples the y axis: column-major elements 1,3,5,7 vanish */
        for(int k : {1, 3, 5, 7})
        {
            TASK_CHECK(tabM[i].m[k] == 0.0);
            TASK_CHECK(tabMinv[i].m[k] == 0.0);
        }
        TASK_CHECK(std::isfinite(tabMagnitude[i]));
        TASK_CHECK(tabMagnitude[i] > 0.0f && tabMagnitude[i] <= 1.0f);
    }

    /* matrix orientation: tabMinv is the inverse of tabM under the documented
       column-major layout (checked on a well-conditioned interior entry) */
    {
        const int i = 32 + 32 * 64;
        for(int r = 0; r < 3; r++)
        {
            for(int c = 0; c < 3; c++)
            {
                double s = 0.0;
                for(int k = 0; k < 3; k++)
                    s += tabMinv[i].m[k * 3 + r] * tabM[i].m[c * 3 + k];
                TASK_CHECK(near(s, r == c ? 1.0 : 0.0, 1e-5));
            }
        }
    }

    /* a axis (index +1) is roughness: the lobe width m00 grows along it */
    for(int a = 1; a < 64; a++)
        TASK_CHECK(tabM[a + 32 * 64].m[0] > tabM[a - 1 + 32 * 64].m[0]);

    /* t axis (index +64) is the view angle: symmetric lobe at t == 0,
       strongly skewed at grazing t == 63 */
    for(int a = 0; a < 64; a++)
    {
        TASK_CHECK(tabM[a].m[2] == 0.0);
        TASK_CHECK(tabM[a].m[6] == 0.0);
    }
    TASK_CHECK(tabM[32 + 63 * 64].m[6] < -0.5);

    return 0;
}

int case_lookup_grid_points()
{
    using namespace pbr::ltc_table;

    /* exact grid points must reproduce table entries; index is a + t*64 with
       u = roughness*63 and v = sqrt(1 - clamp(NdotV,0,1))*63 */
    struct GridPoint { float roughness; float ndotv; int index; };
    const GridPoint points[] = {
        {0.0f,         1.0f, 0},          /* a = 0,  t = 0  */
        {2.0f / 63.0f, 1.0f, 2},          /* a = 2,  t = 0  */
        {0.0f,         1.0f - (5.0f / 63.0f) * (5.0f / 63.0f), 5 * 64}, /* a = 0, t = 5 */
        {1.0f,         1.0f, 63},         /* a = 63, t = 0  */
        {1.0f,         0.0f, 63 + 63 * 64}, /* a = 63, t = 63 */
        {40.0f / 63.0f, 1.0f - (17.0f / 63.0f) * (17.0f / 63.0f), 40 + 17 * 64},
    };

    for(const auto& gp : points)
    {
        const pbr::LtcTerms terms = pbr::ltc_terms(gp.roughness, gp.ndotv);
        for(int r = 0; r < 3; r++)
        {
            for(int c = 0; c < 3; c++)
            {
                const double expected = tabMinv[gp.index].m[c * 3 + r];
                const double tol = 1e-4 * std::max(1.0, std::abs(expected));
                TASK_CHECK(near(terms.minv(r, c), expected, tol));
            }
        }
        TASK_CHECK(near(terms.magnitude, tabMagnitude[gp.index], 1e-4));
    }

    return 0;
}

int case_lookup_bilinear()
{
    using namespace pbr::ltc_table;

    /* halfway between grid points: elementwise average of the tabMinv neighbors */
    {
        const pbr::LtcTerms terms = pbr::ltc_terms(2.5f / 63.0f, 1.0f);
        for(int k = 0; k < 9; k++)
        {
            const double expected = 0.5 * (tabMinv[2].m[k] + tabMinv[3].m[k]);
            const double tol = 1e-4 * std::max(1.0, std::abs(expected));
            TASK_CHECK(near(terms.minv[k / 3][k % 3], expected, tol));
        }
        TASK_CHECK(near(terms.magnitude, 0.5 * (tabMagnitude[2] + tabMagnitude[3]), 1e-4));
    }

    /* halfway in both axes: average of the four surrounding entries */
    {
        const float ndotv = 1.0f - (5.5f / 63.0f) * (5.5f / 63.0f);
        const pbr::LtcTerms terms = pbr::ltc_terms(2.5f / 63.0f, ndotv);
        for(int k = 0; k < 9; k++)
        {
            const double expected = 0.25 * (tabMinv[2 + 5 * 64].m[k] + tabMinv[3 + 5 * 64].m[k]
                                          + tabMinv[2 + 6 * 64].m[k] + tabMinv[3 + 6 * 64].m[k]);
            const double tol = 1e-4 * std::max(1.0, std::abs(expected));
            TASK_CHECK(near(terms.minv[k / 3][k % 3], expected, tol));
        }
    }

    /* the lookup interpolates tabMinv, it must not interpolate tabM and invert:
       inverse(average of tabM neighbors) differs measurably from the average
       of tabMinv neighbors */
    {
        const pbr::LtcTerms terms = pbr::ltc_terms(2.5f / 63.0f, 1.0f);
        Mat3 avg_m;
        for(int k = 0; k < 9; k++)
            avg_m[k / 3][k % 3] = static_cast<float>(0.5 * (tabM[2].m[k] + tabM[3].m[k]));
        const Mat3 inv_avg_m = inverse(avg_m);
        TASK_CHECK(std::abs(terms.minv(0, 0) - inv_avg_m(0, 0)) > 0.05f * std::abs(terms.minv(0, 0)));
    }

    return 0;
}

/* ------------------------------ integral group ------------------------------ */

pbr::RectLight make_light(const Vec3& c0, const Vec3& c1, const Vec3& c2, const Vec3& c3, bool two_sided)
{
    pbr::RectLight light;
    light.corners[0] = c0;
    light.corners[1] = c1;
    light.corners[2] = c2;
    light.corners[3] = c3;
    light.radiance = Vec3(1.0f, 1.0f, 1.0f);
    light.two_sided = two_sided;
    return light;
}

int case_integral_frontal()
{
    /* light parallel above the receiver, double-sided */
    const pbr::RectLight light = make_light({-1.0f, -1.0f, 1.5f}, {1.0f, -1.0f, 1.5f},
                                            {1.0f, 1.0f, 1.5f}, {-1.0f, 1.0f, 1.5f}, true);

    /* off-axis view */
    {
        const Vec3 P(0, 0, 0), N(0, 0, 1), V = normalize(Vec3(0.4f, 0.0f, 1.0f));
        const auto s = component_sample(light, P, N, V, 0.35f);
        double rd, rs;
        reference_integrals(light, P, N, V, 0.35f, rd, rs);
        TASK_CHECK(near(s.diffuse, rd, REF_TOL));
        TASK_CHECK(near(s.specular, rs, REF_TOL));
    }

    /* head-on view (deterministic fallback tangent basis) */
    {
        const Vec3 P(0, 0, 0), N(0, 0, 1), V(0, 0, 1);
        const auto s = component_sample(light, P, N, V, 0.4f);
        double rd, rs;
        reference_integrals(light, P, N, V, 0.4f, rd, rs);
        TASK_CHECK(near(s.diffuse, rd, REF_TOL));
        TASK_CHECK(near(s.specular, rs, REF_TOL));
    }

    return 0;
}

int case_integral_crossing_horizon()
{
    /* vertical light straddling the receiver's tangent plane z = 0 */
    const pbr::RectLight light = make_light({-0.8f, 1.0f, -0.5f}, {0.8f, 1.0f, -0.5f},
                                            {0.8f, 1.0f, 0.7f}, {-0.8f, 1.0f, 0.7f}, true);
    const Vec3 P(0, 0, 0), N(0, 0, 1), V = normalize(Vec3(0.0f, -0.3f, 1.0f));

    const auto s = component_sample(light, P, N, V, 0.5f);
    double rd, rs;
    reference_integrals(light, P, N, V, 0.5f, rd, rs);
    TASK_CHECK(near(s.diffuse, rd, REF_TOL));
    TASK_CHECK(near(s.specular, rs, REF_TOL));

    /* light fully below the horizon contributes exactly zero */
    const pbr::RectLight below = make_light({-0.8f, 1.0f, -2.0f}, {0.8f, 1.0f, -2.0f},
                                            {0.8f, 1.0f, -1.0f}, {-0.8f, 1.0f, -1.0f}, true);
    const auto sb = component_sample(below, P, N, V, 0.5f);
    TASK_CHECK(sb.diffuse == 0.0f);
    TASK_CHECK(sb.specular == 0.0f);

    return 0;
}

int case_integral_grazing()
{
    /* grazing view direction, light nearly in the horizon plane */
    const pbr::RectLight light = make_light({-1.0f, -1.0f, 0.05f}, {1.0f, -1.0f, 0.05f},
                                            {1.0f, 1.0f, 0.05f}, {-1.0f, 1.0f, 0.05f}, true);
    const Vec3 P(0, 0, 0), N(0, 0, 1), V = normalize(Vec3(1.0f, 0.0f, 0.05f));

    const auto s = component_sample(light, P, N, V, 0.5f);
    double rd, rs;
    reference_integrals(light, P, N, V, 0.5f, rd, rs);
    TASK_CHECK(near(s.diffuse, rd, REF_TOL));
    TASK_CHECK(near(s.specular, rs, REF_TOL));

    /* grazing view with a sharp lobe (roughness 0.08): a legal non-degenerate
       rectangle must not produce NaN; the huge light covers the hemisphere,
       so the diffuse integral approaches 1 */
    const pbr::RectLight wide = make_light({-2.0f, -2.0f, 0.02f}, {2.0f, -2.0f, 0.02f},
                                           {2.0f, 2.0f, 0.02f}, {-2.0f, 2.0f, 0.02f}, true);
    const Vec3 Vg = normalize(Vec3(1.0f, 0.0f, 0.03f));
    const auto sg = component_sample(wide, P, N, Vg, 0.08f);
    TASK_CHECK(std::isfinite(sg.diffuse));
    TASK_CHECK(std::isfinite(sg.specular));
    TASK_CHECK(sg.diffuse >= 0.0f && sg.diffuse <= 1.0f);
    TASK_CHECK(sg.specular >= 0.0f && sg.specular <= 1.0f);
    TASK_CHECK(near(sg.diffuse, 1.0, 1e-3));

    return 0;
}

int case_integral_backface_and_winding()
{
    const Vec3 P(0, 0, 0), N(0, 0, 1), V = normalize(Vec3(0.2f, 0.1f, 1.0f));
    const float roughness = 0.45f;

    /* this winding makes cross(p1-p0, p3-p0) point away from the receiver */
    const pbr::RectLight away = make_light({-1.0f, -1.0f, 1.5f}, {1.0f, -1.0f, 1.5f},
                                           {1.0f, 1.0f, 1.5f}, {-1.0f, 1.0f, 1.5f}, false);

    /* single-sided, receiver not on the emitting side: exactly zero */
    const auto s_away = component_sample(away, P, N, V, roughness);
    TASK_CHECK(s_away.diffuse == 0.0f);
    TASK_CHECK(s_away.specular == 0.0f);

    /* reversed winding emits toward the receiver and matches the reference */
    const pbr::RectLight toward = make_light({-1.0f, -1.0f, 1.5f}, {-1.0f, 1.0f, 1.5f},
                                             {1.0f, 1.0f, 1.5f}, {1.0f, -1.0f, 1.5f}, false);
    const auto s_toward = component_sample(toward, P, N, V, roughness);
    TASK_CHECK(s_toward.diffuse > 0.0f);
    TASK_CHECK(s_toward.specular > 0.0f);
    double rd, rs;
    reference_integrals(toward, P, N, V, roughness, rd, rs);
    TASK_CHECK(near(s_toward.diffuse, rd, REF_TOL));
    TASK_CHECK(near(s_toward.specular, rs, REF_TOL));

    /* double-sided: result does not depend on the corner winding */
    pbr::RectLight double_sided = away;
    double_sided.two_sided = true;
    pbr::RectLight double_sided_rev = toward;
    double_sided_rev.two_sided = true;
    const auto sd = component_sample(double_sided, P, N, V, roughness);
    const auto sdr = component_sample(double_sided_rev, P, N, V, roughness);
    TASK_CHECK(near(sd.diffuse, sdr.diffuse, 1e-6));
    TASK_CHECK(near(sd.specular, sdr.specular, 1e-6));

    /* front-facing single-sided equals double-sided for the same geometry */
    TASK_CHECK(near(s_toward.diffuse, sd.diffuse, 1e-6));
    TASK_CHECK(near(s_toward.specular, sd.specular, 1e-6));

    return 0;
}

int case_integral_rotated_translated()
{
    /* arbitrarily rotated and translated light, off-axis receiver */
    const Mat3 R = Mat3::rotation(0.7f, normalize(Vec3(1.0f, 2.0f, 3.0f)));
    const Vec3 offset(1.5f, -0.5f, 2.0f);
    const Vec3 base[4] = {{-0.9f, -0.7f, 0.0f}, {0.9f, -0.7f, 0.0f},
                          {0.9f, 0.7f, 0.0f}, {-0.9f, 0.7f, 0.0f}};
    pbr::RectLight light;
    for(int i = 0; i < 4; i++)
        light.corners[i] = R * base[i] + offset;
    light.radiance = Vec3(1.0f, 1.0f, 1.0f);
    light.two_sided = true;

    const Vec3 P(0.3f, -0.2f, 0.1f);
    const Vec3 N = normalize(Vec3(0.2f, 0.5f, 1.0f));
    const Vec3 V = normalize(Vec3(0.7f, 1.5f, 3.8f));

    const auto s = component_sample(light, P, N, V, 0.6f);
    double rd, rs;
    reference_integrals(light, P, N, V, 0.6f, rd, rs);
    TASK_CHECK(near(s.diffuse, rd, REF_TOL));
    TASK_CHECK(near(s.specular, rs, REF_TOL));

    return 0;
}

int case_integral_hemisphere_coverage()
{
    /* a huge light above the receiver covers (nearly) the whole hemisphere:
       both the cosine lobe and any normalized LTC lobe integrate to ~1 */
    const float S = 200.0f;
    const pbr::RectLight light = make_light({-S, -S, 0.5f}, {S, -S, 0.5f},
                                            {S, S, 0.5f}, {-S, S, 0.5f}, true);
    const Vec3 P(0, 0, 0), N(0, 0, 1), V = normalize(Vec3(0.1f, 0.0f, 1.0f));

    const auto s = component_sample(light, P, N, V, 0.5f);
    TASK_CHECK(near(s.diffuse, 1.0, 2e-3));
    TASK_CHECK(near(s.specular, 1.0, 2e-3));

    return 0;
}

/* ------------------------------ material group ------------------------------ */

constexpr unsigned kWidth = 64;
constexpr unsigned kHeight = 64;

struct QuadVertex
{
    Vec3 position;
    Vec3 normal;
    Vec2 texcoord;
    Vec4 tangent;
};

struct QuadVarying
{
    Vec4 position;
    Vec3 world_position;
    Vec2 uv;
    Mat3 TBN;

    VARYING(position, world_position, uv, TBN);
};

struct QuadUniforms
{
    Mat4 model;
    Mat4 view;
    Mat4 proj;

    Vec3 viewPos;

    struct
    {
        Sampler<RGBA8> albedo;
        Sampler<RGBA8> metallic_roughness;
        Sampler<RGBA8> normal;
    } material;

    Vec3 ambient; /* stands in for the precomputed environment ambient */

    std::vector<pbr::RectLight> lights;
};

using HDRFramebuffer = Framebuffer<RGBAF, Depth>;
using QuadProgram = Program<QuadVertex, QuadVarying, QuadUniforms, HDRFramebuffer>;

/* the production component used inside a real Renderer::draw fragment callback */
void setup_quad_program(QuadProgram& program)
{
    program.onVertex([](const QuadUniforms& uniform, const QuadVertex& in, QuadVarying& out)
    {
        out.world_position = Vec3(uniform.model * Vec4(in.position, 1.0f));
        out.position = uniform.proj * uniform.view * uniform.model * Vec4(in.position, 1.0f);
        out.uv = in.texcoord;

        auto model = Mat3(uniform.model);
        auto tangent = Vec3(in.tangent);
        auto bitangent = cross(in.normal, tangent) * in.tangent.w;
        out.TBN = Mat3(model * tangent, model * bitangent, model * in.normal);
    });

    program.onFragment([](const QuadUniforms& uniform, const QuadVarying& in, HDRFramebuffer::TargetFragments& out)
    {
        auto n = normalize(in.TBN * normalize(Vec3(texture(uniform.material.normal, in.uv) / 255.0f) * 2.0f - 1.0f));
        Vec3 v = normalize(uniform.viewPos - in.world_position);

        Vec3 albedo = Vec3(texture(uniform.material.albedo, in.uv) / 255.0f);
        auto metal_rough = texture(uniform.material.metallic_roughness, in.uv) / 255.0f;

        auto material = pbr::make_material(albedo, metal_rough.z, metal_rough.y);

        Vec3 color = pbr::shade(material, in.world_position, n, v, uniform.ambient,
                                uniform.lights.data(), uniform.lights.size());

        std::get<0>(out) = Vec4(color, 1.0f);
    });
}

void setup_camera(QuadUniforms& uniforms)
{
    uniforms.model = Mat4::identity();
    uniforms.view = Mat4::translation(Vec3(0.0f, 0.0f, -3.0f));
    uniforms.proj = Mat4::perspective(radians(45.0f), static_cast<float>(kWidth) / kHeight, 1.0f, 10.0f);
    uniforms.viewPos = Vec3(0.0f, 0.0f, 3.0f);
}

BufferIndexed<QuadVertex, unsigned> make_quad_indexed()
{
    BufferIndexed<QuadVertex, unsigned> quad;
    quad.vertices = {
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
    };
    quad.indices = {0, 1, 2, 0, 2, 3};
    return quad;
}

Buffer<QuadVertex> make_quad_expanded()
{
    const auto indexed = make_quad_indexed();
    Buffer<QuadVertex> quad;
    for(unsigned i : indexed.indices)
        quad.vertices.push_back(indexed.vertices[i]);
    return quad;
}

/* analytic world position of a pixel center on the z = 0 quad */
Vec3 quad_world_at(int px, int py)
{
    const float tan_half = std::tan(0.5f * radians(45.0f));
    const float ndc_x = 2.0f * (px + 0.5f) / kWidth - 1.0f;
    const float ndc_y = 2.0f * (py + 0.5f) / kHeight - 1.0f;
    return Vec3(ndc_x * 3.0f * tan_half * (static_cast<float>(kWidth) / kHeight),
                ndc_y * 3.0f * tan_half, 0.0f);
}

Texture<RGBA8> constant_texture(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    Texture<RGBA8> tex(2, 2);
    tex.fill(RGBA8(r, g, b, 255));
    return tex;
}

/* expected shading at a world position, replicating the fragment callback
   (TBN is the identity for the quad: normal (0,0,1), tangent (1,0,0,1)) */
Vec3 expected_shading(const QuadUniforms& uniforms,
                      const RGBA8& albedo_bytes, const RGBA8& mr_bytes, const RGBA8& normal_bytes,
                      const Vec3& world)
{
    const Vec3 n = normalize(normalize(Vec3(normal_bytes) / 255.0f * 2.0f - 1.0f));
    const Vec3 v = normalize(uniforms.viewPos - world);
    const auto material = pbr::make_material(Vec3(albedo_bytes) / 255.0f,
                                             mr_bytes.z / 255.0f, mr_bytes.y / 255.0f);
    return pbr::shade(material, world, n, v, uniforms.ambient,
                      uniforms.lights.data(), uniforms.lights.size());
}

bool check_pixel(HDRFramebuffer& fb, int px, int py, const Vec3& expected, float tol)
{
    const RGBAF c = fb.target<0>()(px, py);
    return near(c.x, expected.x, tol) && near(c.y, expected.y, tol) && near(c.z, expected.z, tol);
}

/* single-sided key light above the quad, front face (cross(p1-p0, p3-p0) = -z)
   turned toward the quad */
pbr::RectLight key_light()
{
    pbr::RectLight light;
    light.corners[0] = {-1.0f, -1.0f, 2.0f};
    light.corners[1] = {-1.0f,  1.0f, 2.0f};
    light.corners[2] = { 1.0f,  1.0f, 2.0f};
    light.corners[3] = { 1.0f, -1.0f, 2.0f};
    light.radiance = {8.0f, 6.0f, 4.0f};
    light.two_sided = false;
    return light;
}

pbr::RectLight fill_light()
{
    pbr::RectLight light;
    light.corners[0] = {0.5f, -2.0f, 1.0f};
    light.corners[1] = {2.5f, -2.0f, 1.0f};
    light.corners[2] = {2.5f, -0.5f, 1.0f};
    light.corners[3] = {0.5f, -0.5f, 1.0f};
    light.radiance = {0.5f, 1.5f, 3.0f};
    light.two_sided = true;
    return light;
}

void clear_hdr(HDRFramebuffer& fb)
{
    fb.clear(); /* depth */
    fb.target<0>().fill(RGBAF(0.0f, 0.0f, 0.0f, 0.0f));
}

int case_material_normal_map_lit()
{
    const RGBA8 albedo_bytes(200, 60, 30, 255);
    const RGBA8 mr_bytes(0, 76, 0, 255);        /* dielectric, roughness ~0.3 */
    const RGBA8 normal_bytes(166, 99, 247, 255); /* perturbed tangent-space normal */

    auto albedo_tex = constant_texture(200, 60, 30);
    auto mr_tex = constant_texture(0, 76, 0);
    auto normal_tex = constant_texture(166, 99, 247);

    QuadProgram program;
    setup_quad_program(program);
    auto& uniforms = program.uniforms();
    setup_camera(uniforms);
    uniforms.ambient = Vec3(0.05f, 0.07f, 0.09f);
    uniforms.lights = {key_light()};
    uniforms.material.albedo = albedo_tex;
    uniforms.material.metallic_roughness = mr_tex;
    uniforms.material.normal = normal_tex;
    uniforms.material.albedo.filter = eFilter::NEAREST;
    uniforms.material.metallic_roughness.filter = eFilter::NEAREST;
    uniforms.material.normal.filter = eFilter::NEAREST;

    Renderer renderer(kWidth, kHeight);
    HDRFramebuffer fb(kWidth, kHeight);
    clear_hdr(fb);

    const auto quad = make_quad_indexed();
    renderer.draw(program, quad, fb);

    /* per-fragment shading matches the component at the analytic world position */
    const int pixels[][2] = {{33, 32}, {16, 48}, {48, 16}};
    for(const auto& p : pixels)
    {
        const Vec3 world = quad_world_at(p[0], p[1]);
        const Vec3 expected = expected_shading(uniforms, albedo_bytes, mr_bytes, normal_bytes, world);
        TASK_CHECK(check_pixel(fb, p[0], p[1], expected, 1e-3f));
    }

    /* the visible light pushes the float attachment beyond 1.0 (HDR) */
    {
        const Vec3 world = quad_world_at(33, 32);
        const Vec3 expected = expected_shading(uniforms, albedo_bytes, mr_bytes, normal_bytes, world);
        const RGBAF c = fb.target<0>()(33, 32);
        TASK_CHECK(expected.x > 1.0f);
        TASK_CHECK(c.x > 1.0f);
    }

    /* the normal map must actually change the shading: the flat-normal
       expectation differs measurably from the rendered pixels */
    const RGBA8 flat_normal(128, 128, 255, 255);
    const Vec3 world = quad_world_at(33, 32);
    const Vec3 flat_expected = expected_shading(uniforms, albedo_bytes, mr_bytes, flat_normal, world);
    const RGBAF c = fb.target<0>()(33, 32);
    TASK_CHECK(std::abs(c.x - flat_expected.x) > 1e-3f ||
               std::abs(c.y - flat_expected.y) > 1e-3f ||
               std::abs(c.z - flat_expected.z) > 1e-3f);

    return 0;
}

int case_material_metallic_roughness()
{
    const RGBA8 albedo_bytes(200, 60, 30, 255);

    /* component-level material derivation */
    {
        const Vec3 albedo = Vec3(albedo_bytes) / 255.0f;
        const auto dielectric = pbr::make_material(albedo, 0.0f, 0.5f);
        TASK_CHECK(near(dielectric.f0.x, 0.04f, 1e-6f));
        TASK_CHECK(near(dielectric.diffuse_color.x, albedo.x * 0.96f, 1e-6f));
        const auto metal = pbr::make_material(albedo, 1.0f, 0.5f);
        TASK_CHECK(near(metal.f0.x, albedo.x, 1e-6f));
        TASK_CHECK(near(metal.diffuse_color.x, 0.0f, 1e-6f));
    }

    auto albedo_tex = constant_texture(200, 60, 30);
    auto mr_dielectric = constant_texture(0, 76, 0);
    auto mr_metal = constant_texture(0, 76, 255);
    auto mr_smooth = constant_texture(0, 26, 0);
    auto mr_rough = constant_texture(0, 230, 0);
    auto normal_tex = constant_texture(128, 128, 255);

    QuadProgram program;
    setup_quad_program(program);
    auto& uniforms = program.uniforms();
    setup_camera(uniforms);
    uniforms.ambient = Vec3(0.05f, 0.07f, 0.09f);
    uniforms.lights = {key_light()};
    uniforms.material.albedo = albedo_tex;
    uniforms.material.normal = normal_tex;
    uniforms.material.albedo.filter = eFilter::NEAREST;
    uniforms.material.metallic_roughness.filter = eFilter::NEAREST;
    uniforms.material.normal.filter = eFilter::NEAREST;

    Renderer renderer(kWidth, kHeight);
    HDRFramebuffer fb(kWidth, kHeight);
    const auto quad = make_quad_indexed();
    const Vec3 world = quad_world_at(33, 32);
    const RGBA8 normal_bytes(128, 128, 255, 255);

    /* dielectric vs metallic through the real draw */
    clear_hdr(fb);
    uniforms.material.metallic_roughness = mr_dielectric;
    renderer.draw(program, quad, fb);
    TASK_CHECK(check_pixel(fb, 33, 32, expected_shading(uniforms, albedo_bytes, RGBA8(0, 76, 0, 255), normal_bytes, world), 1e-3f));
    const RGBAF dielectric_pixel = fb.target<0>()(33, 32);

    clear_hdr(fb);
    uniforms.material.metallic_roughness = mr_metal;
    renderer.draw(program, quad, fb);
    TASK_CHECK(check_pixel(fb, 33, 32, expected_shading(uniforms, albedo_bytes, RGBA8(0, 76, 255, 255), normal_bytes, world), 1e-3f));
    const RGBAF metal_pixel = fb.target<0>()(33, 32);

    /* metal tints the specular by albedo and drops the diffuse term */
    TASK_CHECK(std::abs(dielectric_pixel.x - metal_pixel.x) > 1e-3f ||
               std::abs(dielectric_pixel.y - metal_pixel.y) > 1e-3f);

    /* roughness byte reaches the LTC lookup: smooth vs rough differ and both
       match the component expectation */
    clear_hdr(fb);
    uniforms.material.metallic_roughness = mr_smooth;
    renderer.draw(program, quad, fb);
    TASK_CHECK(check_pixel(fb, 33, 32, expected_shading(uniforms, albedo_bytes, RGBA8(0, 26, 0, 255), normal_bytes, world), 1e-3f));
    const RGBAF smooth_pixel = fb.target<0>()(33, 32);

    clear_hdr(fb);
    uniforms.material.metallic_roughness = mr_rough;
    renderer.draw(program, quad, fb);
    TASK_CHECK(check_pixel(fb, 33, 32, expected_shading(uniforms, albedo_bytes, RGBA8(0, 230, 0, 255), normal_bytes, world), 1e-3f));
    const RGBAF rough_pixel = fb.target<0>()(33, 32);

    TASK_CHECK(std::abs(smooth_pixel.x - rough_pixel.x) > 1e-3f ||
               std::abs(smooth_pixel.y - rough_pixel.y) > 1e-3f ||
               std::abs(smooth_pixel.z - rough_pixel.z) > 1e-3f);

    return 0;
}

int case_material_lights_accumulate_hdr()
{
    auto albedo_tex = constant_texture(200, 60, 30);
    auto mr_tex = constant_texture(0, 76, 0);
    auto normal_tex = constant_texture(128, 128, 255);

    QuadProgram program;
    setup_quad_program(program);
    auto& uniforms = program.uniforms();
    setup_camera(uniforms);
    uniforms.ambient = Vec3(0.05f, 0.07f, 0.09f);
    uniforms.lights = {key_light(), fill_light()};
    uniforms.material.albedo = albedo_tex;
    uniforms.material.metallic_roughness = mr_tex;
    uniforms.material.normal = normal_tex;
    uniforms.material.albedo.filter = eFilter::NEAREST;
    uniforms.material.metallic_roughness.filter = eFilter::NEAREST;
    uniforms.material.normal.filter = eFilter::NEAREST;

    Renderer renderer(kWidth, kHeight);
    HDRFramebuffer fb(kWidth, kHeight);
    const auto quad = make_quad_indexed();
    const Vec3 world = quad_world_at(33, 32);

    clear_hdr(fb);
    renderer.draw(program, quad, fb);

    const RGBA8 albedo_bytes(200, 60, 30, 255);
    const RGBA8 mr_bytes(0, 76, 0, 255);
    const RGBA8 normal_bytes(128, 128, 255, 255);
    TASK_CHECK(check_pixel(fb, 33, 32, expected_shading(uniforms, albedo_bytes, mr_bytes, normal_bytes, world), 1e-3f));

    /* light contributions add up linearly and are not clamped to 1 */
    const Vec3 n(0.0f, 0.0f, 1.0f);
    const Vec3 v = normalize(uniforms.viewPos - world);
    const auto material = pbr::make_material(Vec3(albedo_bytes) / 255.0f, mr_bytes.z / 255.0f, mr_bytes.y / 255.0f);
    const pbr::RectLight key = key_light();
    const pbr::RectLight fill = fill_light();
    const pbr::RectLight both[2] = {key, fill};
    const Vec3 c1 = pbr::rect_lighting(material, world, n, v, &key, 1);
    const Vec3 c2 = pbr::rect_lighting(material, world, n, v, &fill, 1);
    const Vec3 c12 = pbr::rect_lighting(material, world, n, v, both, 2);
    TASK_CHECK(near(c12.x, c1.x + c2.x, 1e-5));
    TASK_CHECK(near(c12.y, c1.y + c2.y, 1e-5));
    TASK_CHECK(near(c12.z, c1.z + c2.z, 1e-5));
    TASK_CHECK(c12.x > 1.0f); /* sum exceeds 1: HDR, no clamping */

    /* without lights the old material result (ambient only) is unchanged */
    uniforms.lights.clear();
    clear_hdr(fb);
    renderer.draw(program, quad, fb);
    const RGBAF c = fb.target<0>()(33, 32);
    TASK_CHECK(c.x == uniforms.ambient.x && c.y == uniforms.ambient.y && c.z == uniforms.ambient.z);

    return 0;
}

int case_material_state_reset()
{
    auto albedo_a = constant_texture(200, 60, 30);
    auto mr_a = constant_texture(0, 76, 0);
    auto normal_a = constant_texture(166, 99, 247);
    auto albedo_b = constant_texture(30, 60, 200);
    auto mr_b = constant_texture(0, 150, 0);
    auto normal_b = constant_texture(128, 128, 255);

    QuadProgram program;
    setup_quad_program(program);
    auto& uniforms = program.uniforms();
    setup_camera(uniforms);
    uniforms.material.albedo.filter = eFilter::NEAREST;
    uniforms.material.metallic_roughness.filter = eFilter::NEAREST;
    uniforms.material.normal.filter = eFilter::NEAREST;

    Renderer renderer(kWidth, kHeight);
    HDRFramebuffer fb(kWidth, kHeight);
    const auto quad = make_quad_indexed();
    const Vec3 world = quad_world_at(33, 32);

    /* round A: lights + material A */
    uniforms.ambient = Vec3(0.05f, 0.07f, 0.09f);
    uniforms.lights = {key_light()};
    uniforms.material.albedo = albedo_a;
    uniforms.material.metallic_roughness = mr_a;
    uniforms.material.normal = normal_a;
    clear_hdr(fb);
    renderer.draw(program, quad, fb);
    const std::vector<RGBAF> round_a = fb.target<0>().data();
    TASK_CHECK(check_pixel(fb, 33, 32, expected_shading(uniforms, RGBA8(200, 60, 30, 255), RGBA8(0, 76, 0, 255), RGBA8(166, 99, 247, 255), world), 1e-3f));

    /* round B on the same program: different light list (empty) and material */
    uniforms.ambient = Vec3(0.11f, 0.13f, 0.17f);
    uniforms.lights = {};
    uniforms.material.albedo = albedo_b;
    uniforms.material.metallic_roughness = mr_b;
    uniforms.material.normal = normal_b;
    clear_hdr(fb);
    renderer.draw(program, quad, fb);
    TASK_CHECK(check_pixel(fb, 33, 32, expected_shading(uniforms, RGBA8(30, 60, 200, 255), RGBA8(0, 150, 0, 255), RGBA8(128, 128, 255, 255), world), 1e-3f));
    const RGBAF cb = fb.target<0>()(33, 32);
    TASK_CHECK(cb.x == uniforms.ambient.x && cb.y == uniforms.ambient.y && cb.z == uniforms.ambient.z);

    /* round A again: no state from round B may remain; identical inputs must
       reproduce the first draw exactly */
    uniforms.ambient = Vec3(0.05f, 0.07f, 0.09f);
    uniforms.lights = {key_light()};
    uniforms.material.albedo = albedo_a;
    uniforms.material.metallic_roughness = mr_a;
    uniforms.material.normal = normal_a;
    clear_hdr(fb);
    renderer.draw(program, quad, fb);
    TASK_CHECK(fb.target<0>().data() == round_a);

    return 0;
}

int case_material_indexed_and_default_target()
{
    auto albedo_tex = constant_texture(200, 60, 30);
    auto mr_tex = constant_texture(0, 76, 0);
    auto normal_tex = constant_texture(128, 128, 255);

    QuadProgram program;
    setup_quad_program(program);
    auto& uniforms = program.uniforms();
    setup_camera(uniforms);
    uniforms.ambient = Vec3(0.05f, 0.07f, 0.09f);
    uniforms.lights = {key_light()};
    uniforms.material.albedo = albedo_tex;
    uniforms.material.metallic_roughness = mr_tex;
    uniforms.material.normal = normal_tex;
    uniforms.material.albedo.filter = eFilter::NEAREST;
    uniforms.material.metallic_roughness.filter = eFilter::NEAREST;
    uniforms.material.normal.filter = eFilter::NEAREST;

    Renderer renderer(kWidth, kHeight);

    /* indexed and plain draws of the same geometry produce identical output */
    HDRFramebuffer fb_indexed(kWidth, kHeight);
    clear_hdr(fb_indexed);
    const auto quad_indexed = make_quad_indexed();
    renderer.draw(program, quad_indexed, fb_indexed);

    HDRFramebuffer fb_plain(kWidth, kHeight);
    clear_hdr(fb_plain);
    const auto quad_plain = make_quad_expanded();
    renderer.draw(program, quad_plain, fb_plain);

    TASK_CHECK(fb_indexed.target<0>().data() == fb_plain.target<0>().data());

    /* the old quantized RGBA8 target behavior is unchanged */
    struct SimpleVertex { Vec4 p; };
    struct SimpleFragment { Vec4 position; VARYING(position); };
    struct SimpleUniforms { Vec4 color; };

    Program<SimpleVertex, SimpleFragment, SimpleUniforms> simple;
    simple.onVertex([](const SimpleUniforms&, const SimpleVertex& v, SimpleFragment& f) { f.position = v.p; });
    simple.onFragment([](const SimpleUniforms& u, const SimpleFragment&, Vec4& out) { out = u.color; });
    simple.uniforms().color = Vec4(0.5f, 0.25f, 1.0f, 1.0f);

    BufferIndexed<SimpleVertex, unsigned> simple_quad;
    simple_quad.vertices = {{{-0.8f, -0.8f, 0.0f, 1.0f}}, {{0.8f, -0.8f, 0.0f, 1.0f}},
                            {{0.8f, 0.8f, 0.0f, 1.0f}}, {{-0.8f, 0.8f, 0.0f, 1.0f}}};
    simple_quad.indices = {0, 1, 2, 0, 2, 3};

    renderer.framebuffer().clear(Vec4(0, 0, 0, 1));
    renderer.draw(simple, simple_quad);

    const RGBA8 c = renderer.framebuffer().color()(33, 32);
    TASK_CHECK(c.x == 127 && c.y == 63 && c.z == 255 && c.w == 255);

    return 0;
}

} /* anonymous namespace */

const task_case task_cases[] = {
    {"lookup", "table_layout", case_lookup_table_layout},
    {"lookup", "grid_points_and_axes", case_lookup_grid_points},
    {"lookup", "bilinear", case_lookup_bilinear},

    {"integral", "frontal", case_integral_frontal},
    {"integral", "crossing_horizon", case_integral_crossing_horizon},
    {"integral", "grazing", case_integral_grazing},
    {"integral", "backface_and_winding", case_integral_backface_and_winding},
    {"integral", "rotated_translated", case_integral_rotated_translated},
    {"integral", "hemisphere_coverage", case_integral_hemisphere_coverage},

    {"material", "normal_map_lit", case_material_normal_map_lit},
    {"material", "metallic_roughness", case_material_metallic_roughness},
    {"material", "lights_accumulate_hdr", case_material_lights_accumulate_hdr},
    {"material", "state_reset", case_material_state_reset},
    {"material", "indexed_and_default_target", case_material_indexed_and_default_target},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
