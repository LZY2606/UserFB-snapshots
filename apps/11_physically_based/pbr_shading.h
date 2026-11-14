#pragma once

/*
 * PBR material shading component (window-free).
 *
 * Bundles the material model of demo_pbr.cpp (dielectric F0 = 0.04,
 * metallic albedo blend, diffuse attenuation) with direct lighting from
 * rectangular area lights via Linearly Transformed Cosines (LTC).
 *
 * The component is pure CPU shading code: it is used by the interactive
 * demo (apps/11_physically_based/demo_pbr.cpp) and by offscreen Program
 * fragment callbacks alike. No GLFW/OpenGL, no file or texture IO.
 *
 * LTC fit data: task_data/ltc.inc (fixed 64x64 tables, see
 * task_data/README.md and task_data/LTC-LICENSE for source and terms).
 */

#include <math/vector3.h>
#include <math/matrix3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pbr
{

/* fixed 64x64 LTC fit, row index is a + t*64 (see task_data/README.md) */
namespace ltc_table
{
struct mat33 { double m[9]; };
#include "../../task_data/ltc.inc"
}

/*
 * Rectangular area light with uniform emission.
 * corners: four world-space corners in boundary order.
 * radiance: linear RGB radiance.
 * two_sided: if false, the light only emits toward the side that
 *            cross(corners[1]-corners[0], corners[3]-corners[0]) points to;
 *            if true, emission does not depend on the corner winding.
 */
struct RectLight
{
    Vec3 corners[4];
    Vec3 radiance;
    bool two_sided = false;
};

/* material parameters derived from albedo / metallic / roughness */
struct Material
{
    Vec3 diffuse_color; /* albedo * (1 - 0.04) * (1 - metallic) */
    Vec3 f0;            /* mix(dielectric F0 = 0.04, albedo, metallic) */
    float roughness = 1.0f;
};

inline Material make_material(const Vec3& albedo, float metallic, float roughness)
{
    const Vec3 DIELECTRIC_F0 = {0.04f, 0.04f, 0.04f};

    Material material;
    material.f0 = mix(DIELECTRIC_F0, albedo, metallic);
    material.diffuse_color = albedo * (1.0f - DIELECTRIC_F0.x) * (1.0f - metallic);
    material.roughness = roughness;
    return material;
}

/* bilinearly interpolated inverse LTC matrix and norm magnitude */
struct LtcTerms
{
    Mat3 minv;
    float magnitude;
};

/*
 * Table lookup at u = roughness*63, v = sqrt(1 - clamp(NdotV,0,1))*63.
 * The nine column-major elements of tabMinv and tabMagnitude are bilinearly
 * interpolated separately (interpolating tabM and inverting afterwards is
 * not the same and is intentionally not done).
 */
inline LtcTerms ltc_terms(float roughness, float ndotv)
{
    const float u = std::clamp(roughness, 0.0f, 1.0f) * 63.0f;
    const float v = std::sqrt(1.0f - std::clamp(ndotv, 0.0f, 1.0f)) * 63.0f;

    const int a0 = std::min(static_cast<int>(u), 62);
    const int t0 = std::min(static_cast<int>(v), 62);
    const float fa = u - static_cast<float>(a0);
    const float ft = v - static_cast<float>(t0);

    const int i00 = (a0    ) + (t0    ) * 64;
    const int i10 = (a0 + 1) + (t0    ) * 64;
    const int i01 = (a0    ) + (t0 + 1) * 64;
    const int i11 = (a0 + 1) + (t0 + 1) * 64;

    LtcTerms terms;
    for(int k = 0; k < 9; k++)
    {
        const double m0 = ltc_table::tabMinv[i00].m[k] + (ltc_table::tabMinv[i10].m[k] - ltc_table::tabMinv[i00].m[k]) * fa;
        const double m1 = ltc_table::tabMinv[i01].m[k] + (ltc_table::tabMinv[i11].m[k] - ltc_table::tabMinv[i01].m[k]) * fa;
        terms.minv[k / 3][k % 3] = static_cast<float>(m0 + (m1 - m0) * ft);
    }

    const double g0 = ltc_table::tabMagnitude[i00] + (ltc_table::tabMagnitude[i10] - ltc_table::tabMagnitude[i00]) * fa;
    const double g1 = ltc_table::tabMagnitude[i01] + (ltc_table::tabMagnitude[i11] - ltc_table::tabMagnitude[i01]) * fa;
    terms.magnitude = static_cast<float>(g0 + (g1 - g0) * ft);

    return terms;
}

namespace detail
{

/*
 * Tangent frame for the shading point: T is the view direction projected
 * onto the tangent plane, B = cross(N, T). For head-on views the projection
 * degenerates and a deterministic fallback basis is used instead.
 */
inline void tangent_basis(const Vec3& n, const Vec3& v, Vec3& t, Vec3& b)
{
    const Vec3 proj = v - n * dot(n, v);
    if(dot(proj, proj) > 1e-10f)
    {
        t = normalize(proj);
    }
    else
    {
        const Vec3 up = std::abs(n.z) < 0.999f ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(1.0f, 0.0f, 0.0f);
        t = normalize(cross(up, n));
    }
    b = cross(n, t);
}

/* Sutherland-Hodgman clip of a convex polygon against the z >= 0 half-space */
inline int clip_polygon_hemisphere(const Vec3* in, int count, Vec3* out)
{
    int n = 0;
    for(int i = 0; i < count; i++)
    {
        const Vec3& a = in[i];
        const Vec3& b = in[(i + 1) % count];
        const bool inside_a = a.z >= 0.0f;
        const bool inside_b = b.z >= 0.0f;

        if(inside_a)
            out[n++] = a;

        if(inside_a != inside_b)
        {
            /* true edge/horizon intersection (corner z is never just clamped) */
            const float t = a.z / (a.z - b.z);
            out[n++] = a + (b - a) * t;
        }
    }
    return n;
}

/* clipped spherical-polygon integral of the transformed light polygon */
inline float integrate_polygon(const RectLight& light, const Vec3& position,
                               const Vec3& n, const Vec3& t, const Vec3& b,
                               const Mat3& minv)
{
    Vec3 polygon[4];
    for(int i = 0; i < 4; i++)
    {
        const Vec3 d = light.corners[i] - position;
        polygon[i] = minv * Vec3(dot(d, t), dot(d, b), dot(d, n));
    }

    Vec3 clipped[8];
    const int count = clip_polygon_hemisphere(polygon, 4, clipped);
    if(count < 3)
        return 0.0f; /* entirely below the horizon */

    Vec3 sum(0.0f, 0.0f, 0.0f);
    for(int i = 0; i < count; i++)
    {
        const Vec3 p0 = normalize(clipped[i]);
        const Vec3 p1 = normalize(clipped[(i + 1) % count]);

        const Vec3 c = cross(p0, p1);
        const float len = length(c);
        if(len <= 1e-7f)
            continue; /* parallel or coincident edge: continuous limit contributes nothing */

        sum += (c / len) * std::atan2(len, dot(p0, p1));
    }

    const float PI = 3.14159265358979323846f;
    return std::abs(sum.z) / (2.0f * PI);
}

} /* namespace detail */

/*
 * Geometric polygon integral of the light under the given linear transform
 * (identity for the diffuse cosine lobe, interpolated tabMinv for the
 * specular LTC lobe). Purely geometric: no single-sided emission test.
 */
inline float rect_light_integral(const RectLight& light, const Vec3& position,
                                 const Vec3& normal, const Vec3& view_dir, const Mat3& minv)
{
    const Vec3 n = normalize(normal);
    const Vec3 v = normalize(view_dir);

    Vec3 t, b;
    detail::tangent_basis(n, v, t, b);

    return detail::integrate_polygon(light, position, n, t, b, minv);
}

/* diffuse (identity) and specular (LTC) integrals of one light */
struct LightSample
{
    float diffuse;
    float specular;
};

inline LightSample evaluate_rect_light(const RectLight& light, const Vec3& position,
                                       const Vec3& normal, const Vec3& view_dir,
                                       const LtcTerms& terms)
{
    LightSample sample{0.0f, 0.0f};

    if(!light.two_sided)
    {
        /* single-sided: emits only toward cross(p1-p0, p3-p0) */
        const Vec3 front = cross(light.corners[1] - light.corners[0],
                                 light.corners[3] - light.corners[0]);
        if(dot(front, position - light.corners[0]) <= 0.0f)
            return sample;
    }

    const Vec3 n = normalize(normal);
    const Vec3 v = normalize(view_dir);

    Vec3 t, b;
    detail::tangent_basis(n, v, t, b);

    sample.diffuse  = detail::integrate_polygon(light, position, n, t, b, Mat3::identity());
    sample.specular = detail::integrate_polygon(light, position, n, t, b, terms.minv);
    return sample;
}

/*
 * Direct lighting of all rect lights: radiance * (diffuseColor*diffuseIntegral
 * + F0*magnitude*specularIntegral) per light, summed without clamping.
 */
inline Vec3 rect_lighting(const Material& material, const Vec3& position,
                          const Vec3& normal, const Vec3& view_dir,
                          const RectLight* lights, std::size_t light_count)
{
    const Vec3 n = normalize(normal);
    const Vec3 v = normalize(view_dir);

    const LtcTerms terms = ltc_terms(material.roughness, dot(n, v));

    Vec3 direct(0.0f, 0.0f, 0.0f);
    for(std::size_t i = 0; i < light_count; i++)
    {
        const LightSample s = evaluate_rect_light(lights[i], position, n, v, terms);
        direct += lights[i].radiance * (material.diffuse_color * s.diffuse
                                        + material.f0 * (terms.magnitude * s.specular));
    }
    return direct;
}

/*
 * Full material shading: precomputed ambient plus direct rect lights.
 * With no lights this returns the ambient term unchanged.
 */
inline Vec3 shade(const Material& material, const Vec3& position,
                  const Vec3& normal, const Vec3& view_dir,
                  const Vec3& ambient,
                  const RectLight* lights, std::size_t light_count)
{
    Vec3 color = ambient;
    if(lights != nullptr && light_count > 0)
        color += rect_lighting(material, position, normal, view_dir, lights, light_count);
    return color;
}

} /* namespace pbr */
