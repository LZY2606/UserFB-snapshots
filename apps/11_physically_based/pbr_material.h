#pragma once

/*
 * Production PBR material shading and LTC rectangular area lights.
 *
 * This header has no window/GLFW/OpenGL dependency.  It is used both by the
 * interactive 11_pbr example and by offscreen Renderer::draw callbacks.
 * All LTC math is evaluated per fragment; lighting is never baked per triangle.
 */

#include <renderer.h>
#include <sampler.h>
#include <math/matrix3.h>
#include <math/matrix4.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace pbr
{

inline constexpr double PI = 3.1415926535897932384626433832795;

/* Double precision is useful around the horizon and at grazing angles. */
struct Vec3d
{
    double x = 0.0, y = 0.0, z = 0.0;

    Vec3d() = default;
    constexpr Vec3d(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    explicit Vec3d(const ::Vec3& v) : x(v.x), y(v.y), z(v.z) {}

    Vec3d operator+(const Vec3d& b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec3d operator-(const Vec3d& b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec3d operator-() const { return {-x, -y, -z}; }
    Vec3d operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3d operator/(double s) const { return {x / s, y / s, z / s}; }
    Vec3d& operator+=(const Vec3d& b) { x += b.x; y += b.y; z += b.z; return *this; }
};

inline Vec3d operator*(double s, const Vec3d& v) { return v * s; }
inline Vec3d operator*(const Vec3d& a, const Vec3d& b)
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
inline double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3d cross(const Vec3d& a, const Vec3d& b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
inline double length(const Vec3d& v) { return std::sqrt(dot(v, v)); }
inline Vec3d normalize(const Vec3d& v)
{
    const double l = length(v);
    return l > 0.0 ? v / l : Vec3d(0.0, 0.0, 1.0);
}
inline Vec3d clampComponents(const Vec3d& v, double lo, double hi)
{
    return {std::clamp(v.x, lo, hi), std::clamp(v.y, lo, hi), std::clamp(v.z, lo, hi)};
}

inline ::Vec3 toFloat(const Vec3d& v)
{
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

/*
 * A rectangular area emitter.  corners are world-space points in boundary
 * order p0,p1,p2,p3.  For a one-sided light the emitting face points along
 * cross(p1-p0, p3-p0).  radiance is linear HDR RGB and is not clamped.
 */
struct AreaLight
{
    std::array<Vec3d, 4> corners{Vec3d{-1, -1, 0}, Vec3d{1, -1, 0}, Vec3d{1, 1, 0}, Vec3d{-1, 1, 0}};
    Vec3d radiance{1.0, 1.0, 1.0};
    bool twoSided = false;
};

struct LtcTableSample
{
    /* Nine values, interpolated independently and stored column-major. */
    std::array<double, 9> minv{};
    double magnitude = 1.0;

    /* Integer lower corner and interpolation weights, exposed for checks. */
    int roughnessIndex = 0;
    int viewIndex = 0;
    double roughnessWeight = 0.0;
    double viewWeight = 0.0;
};

namespace ltc_data
{
struct Mat33
{
    float m[9]{};
};
using mat33 = Mat33;
#include "../../task_data/ltc.inc"
} // namespace ltc_data

/* Column-major M * v. */
inline Vec3d multiplyColumnMajor(const std::array<double, 9>& m, const Vec3d& v)
{
    return {m[0] * v.x + m[3] * v.y + m[6] * v.z,
            m[1] * v.x + m[4] * v.y + m[7] * v.z,
            m[2] * v.x + m[5] * v.y + m[8] * v.z};
}

inline double determinantColumnMajor(const std::array<double, 9>& m)
{
    return m[0] * (m[4] * m[8] - m[5] * m[7])
         - m[3] * (m[1] * m[8] - m[2] * m[7])
         + m[6] * (m[1] * m[5] - m[2] * m[4]);
}

/*
 * Coordinates follow the supplied fit table:
 *   a = roughness * 63
 *   t = sqrt(1 - clamp(N.V, 0, 1)) * 63
 *   index = a + t * 64.
 */
inline LtcTableSample sampleLtcTable(double roughness, double ndv)
{
    LtcTableSample out;

    const double a = std::clamp(roughness, 0.0, 1.0) * 63.0;
    const double t = std::sqrt(std::max(0.0, 1.0 - std::clamp(ndv, 0.0, 1.0))) * 63.0;

    const int a0 = std::min(62, static_cast<int>(std::floor(a)));
    const int t0 = std::min(62, static_cast<int>(std::floor(t)));
    const int a1 = a0 + 1;
    const int t1 = t0 + 1;
    const double fa = a - static_cast<double>(a0);
    const double ft = t - static_cast<double>(t0);

    out.roughnessIndex = a0;
    out.viewIndex = t0;
    out.roughnessWeight = fa;
    out.viewWeight = ft;

    const int i00 = a0 + t0 * 64;
    const int i10 = a1 + t0 * 64;
    const int i01 = a0 + t1 * 64;
    const int i11 = a1 + t1 * 64;

    for (int k = 0; k < 9; ++k)
    {
        const double c00 = ltc_data::tabMinv[i00].m[k];
        const double c10 = ltc_data::tabMinv[i10].m[k];
        const double c01 = ltc_data::tabMinv[i01].m[k];
        const double c11 = ltc_data::tabMinv[i11].m[k];

        const double c0 = c00 * (1.0 - fa) + c10 * fa;
        const double c1 = c01 * (1.0 - fa) + c11 * fa;
        out.minv[k] = c0 * (1.0 - ft) + c1 * ft;
    }

    const double m00 = ltc_data::tabMagnitude[i00];
    const double m10 = ltc_data::tabMagnitude[i10];
    const double m01 = ltc_data::tabMagnitude[i01];
    const double m11 = ltc_data::tabMagnitude[i11];
    const double m0 = m00 * (1.0 - fa) + m10 * fa;
    const double m1 = m01 * (1.0 - fa) + m11 * fa;
    out.magnitude = m0 * (1.0 - ft) + m1 * ft;

    return out;
}

struct Basis
{
    Vec3d tangent;
    Vec3d bitangent;
    Vec3d normal;
};

/* T is the projected view vector; B = cross(N, T). */
inline Basis makeBasis(const Vec3d& normal_, const Vec3d& view_)
{
    const Vec3d N = normalize(normal_);
    const Vec3d V = normalize(view_);
    const double nv = dot(N, V);

    Vec3d T = V - N * nv;
    const double len2 = dot(T, T);

    if (len2 > 1.0e-20)
    {
        T = T / std::sqrt(len2);
    }
    else
    {
        /* Deterministic orthonormal basis for the head-on singularity. */
        const Vec3d helper = std::abs(N.x) > 0.9 ? Vec3d(0.0, 1.0, 0.0) : Vec3d(1.0, 0.0, 0.0);
        T = normalize(helper - N * dot(helper, N));
    }

    Vec3d B = cross(N, T);
    /* The branch above is already orthonormal; normalize for floating drift. */
    B = normalize(B);
    return {T, B, N};
}

inline Vec3d toLocal(const Basis& basis, const Vec3d& worldVector)
{
    return {dot(worldVector, basis.tangent),
            dot(worldVector, basis.bitangent),
            dot(worldVector, basis.normal)};
}

struct ClippedPolygon
{
    std::array<Vec3d, 8> vertices{};
    int count = 0;
};

/* Sutherland-Hodgman clipping against z >= 0, including real intersections. */
inline ClippedPolygon clipPolygonToPositiveZ(std::span<const Vec3d> polygon)
{
    ClippedPolygon result;
    if (polygon.empty()) return result;

    auto add = [&](const Vec3d& p)
    {
        if (result.count < static_cast<int>(result.vertices.size()))
            result.vertices[result.count++] = p;
    };

    Vec3d previous = polygon[polygon.size() - 1];
    bool previousInside = previous.z >= 0.0;

    for (std::size_t i = 0; i < polygon.size(); ++i)
    {
        const Vec3d current = polygon[i];
        const bool currentInside = current.z >= 0.0;

        if (currentInside != previousInside)
        {
            const double denominator = current.z - previous.z;
            if (std::abs(denominator) > 1.0e-30)
            {
                double t = (-previous.z) / denominator;
                t = std::clamp(t, 0.0, 1.0);
                Vec3d intersection = previous + (current - previous) * t;
                intersection.z = 0.0; /* plane invariant, removes round-off */
                add(intersection);
            }
        }

        if (currentInside)
            add(current);

        previous = current;
        previousInside = currentInside;
    }

    return result;
}

/*
 * Directed spherical polygon integral.  Input is already in the shading local
 * frame and, for specular, already multiplied by the interpolated LTC Minv.
 */
inline double sphericalPolygonIntegral(std::span<const Vec3d> transformedPolygon)
{
    const ClippedPolygon clipped = clipPolygonToPositiveZ(transformedPolygon);
    if (clipped.count < 3) return 0.0;

    std::array<Vec3d, 8> spherical{};
    for (int i = 0; i < clipped.count; ++i)
        spherical[i] = normalize(clipped.vertices[i]);

    /*
     * Polygon orientation supplies a continuous direction for the exact
     * antiparallel-edge limit, where cross(u,v) is zero but its normalized
     * direction is not defined by that edge alone.
     */
    Vec3d polygonOrientation{0.0, 0.0, 0.0};
    for (int i = 0; i < clipped.count; ++i)
    {
        const int j = (i + 1) % clipped.count;
        polygonOrientation += cross(clipped.vertices[i], clipped.vertices[j]);
    }
    polygonOrientation = normalize(polygonOrientation);

    Vec3d directed{0.0, 0.0, 0.0};

    for (int i = 0; i < clipped.count; ++i)
    {
        const Vec3d u = spherical[i];
        const Vec3d v = spherical[(i + 1) % clipped.count];
        Vec3d c = cross(u, v);
        const double s = length(c);
        const double d = std::clamp(dot(u, v), -1.0, 1.0);
        const double theta = std::atan2(s, d);

        if (s > 0.0)
        {
            /* theta * normalize(c) = c * theta / s, with the same limit. */
            directed += c * (theta / s);
        }
        else if (d < 0.0)
        {
            /* Exact antiparallel limit, oriented by the enclosing polygon. */
            directed += polygonOrientation * PI;
        }
        /* Coincident/same-direction edge: theta is zero. */
    }

    return std::abs(directed.z) / (2.0 * PI);
}

struct LightIntegrals
{
    double diffuse = 0.0;
    double specular = 0.0;
    double magnitude = 1.0;
    LtcTableSample table{};
    Basis basis{};
    bool frontFacing = true;
};

inline Vec3d lightNormal(const AreaLight& light)
{
    return cross(light.corners[1] - light.corners[0],
                 light.corners[3] - light.corners[0]);
}

inline LightIntegrals computeLightIntegrals(
    const AreaLight& light,
    const Vec3d& receiver,
    const Vec3d& normal,
    const Vec3d& view,
    double roughness)
{
    LightIntegrals result;
    result.basis = makeBasis(normal, view);

    const Vec3d geometricNormal = lightNormal(light);
    const double side = dot(receiver - light.corners[0], geometricNormal);
    result.frontFacing = side >= 0.0;

    if (!light.twoSided && side < 0.0)
        return result;

    const double ndv = dot(result.basis.normal, normalize(view));
    result.table = sampleLtcTable(roughness, ndv);
    result.magnitude = result.table.magnitude;

    std::array<Vec3d, 4> local{};
    std::array<Vec3d, 4> specular{};
    for (int i = 0; i < 4; ++i)
    {
        const Vec3d worldVector = light.corners[i] - receiver;
        local[i] = toLocal(result.basis, worldVector);
        specular[i] = multiplyColumnMajor(result.table.minv, local[i]);
    }

    result.diffuse = sphericalPolygonIntegral(local);
    result.specular = sphericalPolygonIntegral(specular);
    return result;
}

/* Exact fixed LTC approximation requested for this component. */
inline Vec3d evaluateAreaLight(
    const AreaLight& light,
    const Vec3d& receiver,
    const Vec3d& normal,
    const Vec3d& view,
    double roughness,
    const Vec3d& diffuseColor,
    const Vec3d& f0)
{
    const LightIntegrals integrals = computeLightIntegrals(light, receiver, normal, view, roughness);
    if (!integrals.frontFacing && !light.twoSided)
        return {0.0, 0.0, 0.0};

    return light.radiance *
           (diffuseColor * integrals.diffuse +
            f0 * integrals.table.magnitude * integrals.specular);
}

/* Direct LTC contribution for renderer vector types. */
inline ::Vec3 shadeDirectLights(
    const ::Vec3& worldPosition,
    const ::Vec3& viewPosition,
    const ::Vec3& shadingNormal,
    const ::Vec3& diffuseColor,
    const ::Vec3& f0,
    float roughness,
    std::span<const AreaLight> lights)
{
    const ::Vec3 N = normalize(shadingNormal);
    const ::Vec3 V = normalize(viewPosition - worldPosition);

    Vec3d direct{0.0, 0.0, 0.0};
    for (const AreaLight& light : lights)
    {
        direct += evaluateAreaLight(
            light,
            Vec3d(worldPosition),
            Vec3d(N),
            Vec3d(V),
            static_cast<double>(roughness),
            Vec3d(diffuseColor),
            Vec3d(f0));
    }

    return toFloat(direct);
}

/* ---------- Renderer::draw integration and existing IBL material ---------- */

struct MaterialSamplers
{
    Sampler<RGBA8> albedo;
    Sampler<RGBA8> metallic_roughness;
    Sampler<RGBA8> normal;
};

struct ShadingUniforms
{
    ::Mat4 model = ::Mat4::identity();
    ::Mat4 view = ::Mat4::identity();
    ::Mat4 proj = ::Mat4::identity();

    ::Vec3 viewPos{0.0f, 0.0f, 3.0f};

    MaterialSamplers material;

    Sampler<RGBAF> irradiance;
    Sampler<RGBAF> prefilter_radiance;
    Sampler<RGBAF> brdf;

    /* Replaced as a whole per frame/draw; there is no cached light state. */
    std::vector<AreaLight> lights;
};

/* Convenience alias matching the example's original naming. */
using Uniforms = ShadingUniforms;

struct Vertex
{
    ::Vec3 position;
    ::Vec3 normal;
    ::Vec2 texcoord;
    ::Vec4 tangent;
};

struct Varying
{
    ::Vec4 position;
    ::Vec3 world_position;
    ::Vec2 uv;
    ::Mat3 TBN;

    VARYING(position, world_position, uv, TBN);
};

inline void vertexShader(const ShadingUniforms& uniform, const Vertex& in, Varying& out)
{
    out.world_position = ::Vec3(uniform.model * ::Vec4(in.position, 1.0f));
    out.position = uniform.proj * uniform.view * uniform.model * ::Vec4(in.position, 1.0f);
    out.uv = in.texcoord;

    const ::Mat3 model = ::Mat3(uniform.model);
    const ::Vec3 tangent = ::Vec3(in.tangent);
    const ::Vec3 bitangent = ::cross(in.normal, tangent) * in.tangent.w;
    out.TBN = ::Mat3(model * tangent, model * bitangent, model * in.normal);
}

/*
 * Samples the final normal-map normal and current material textures.
 * Ambient IBL is byte-for-byte the same shading expression as the original
 * example; LTC direct light is added afterwards.
 */
inline ::Vec3 shadeFragment(const ShadingUniforms& uniform, const Varying& in)
{
    const ::Vec3 n = normalize(in.TBN * normalize(::Vec3(::texture(uniform.material.normal, in.uv) / 255.0f) * 2.0f - 1.0f));
    const ::Vec3 v = normalize(uniform.viewPos - in.world_position);
    const float nv = std::max(dot(n, v), 0.0f);
    const ::Vec3 r = reflect(-v, n);

    /* material properties */
    ::Vec3 albedo = ::Vec3(::texture(uniform.material.albedo, in.uv) / 255.0f);
    const auto metal_rough = ::texture(uniform.material.metallic_roughness, in.uv) / 255.0f;
    const float metallic = metal_rough.z;
    const float roughness = metal_rough.y;

    /* determine metallic / diffuse color values */
    const ::Vec3 DIELECTRIC_F0 = {0.04f, 0.04f, 0.04f};
    ::Vec3 f0 = ::mix(DIELECTRIC_F0, albedo, metallic);
    albedo = albedo * (1.0 - DIELECTRIC_F0.x) * (1.0 - metallic);

    ::Vec3 illuminance{0.0f, 0.0f, 0.0f};

    /* Existing precomputed environment lighting. */
    if (uniform.prefilter_radiance.m_texture && uniform.irradiance.m_texture && uniform.brdf.m_texture)
    {
        ::Vec3 radiance = ::Vec3(::textureLod(uniform.prefilter_radiance, equirectangularUV(r), roughness * static_cast<float>(uniform.prefilter_radiance.m_texture->num_mipmaps())));
        ::Vec3 irradiance = ::Vec3(::texture(uniform.irradiance, equirectangularUV(n)));
        auto env_brdf = ::Vec2(::texture(uniform.brdf, ::Vec2(nv, roughness)));

        /* https://www.jcgt.org/published/0008/01/03/paper.pdf */
        ::Vec3 Fr = max(::Vec3(1.0 - roughness), f0) - f0;
        ::Vec3 k_S = f0 + Fr * pow(1.0 - nv, 5.0f);
        auto fss_ess = (k_S * env_brdf.x + ::Vec3(env_brdf.y));

        illuminance = (fss_ess * radiance + albedo * irradiance);
    }

    /* Real per-fragment LTC area lighting. */
    illuminance += shadeDirectLights(
        in.world_position,
        uniform.viewPos,
        n,
        albedo,
        f0,
        roughness,
        uniform.lights);

    return illuminance;
}

inline void fragmentToDefaultTarget(const ShadingUniforms& uniform, const Varying& in, ::Vec4& out)
{
    out = ::Vec4(shadeFragment(uniform, in), 1.0f);
}

} // namespace pbr
