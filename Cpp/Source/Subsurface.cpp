#include "Subsurface.h"
#include <cmath>

static float Comp(const float3& v, int c)
{
    return c == 0 ? v.getX() : (c == 1 ? v.getY() : v.getZ());
}

bool SubsurfaceParamsValid(const SubsurfaceParams& p)
{
    for (int c = 0; c < 3; ++c)
    {
        float rho = Comp(p.rho, c);
        float d = Comp(p.d, c);
        if (!std::isfinite(rho) || rho < 0.0f)
            return false;
        if (!std::isfinite(d) || d <= 0.0f)
            return false;
    }
    return true;
}

float SubsurfaceKernel(float d, float r)
{
    // S_d(r) = 0.5*exp(-r^2/(2 d^2))/(2 pi d^2) + 0.5*exp(-r^2/(18 d^2))/(18 pi d^2)
    float d2 = d * d;
    float g1 = expf(-r * r / (2.0f * d2)) / (2.0f * kPI * d2);
    float g2 = expf(-r * r / (18.0f * d2)) / (18.0f * kPI * d2);
    return 0.5f * (g1 + g2);
}

void SubsurfaceFrame(const float3& outwardNormal, float3& outT1, float3& outT2)
{
    float3 a = fabsf(outwardNormal.getX()) > 0.9f ? float3(0, 1, 0) : float3(1, 0, 0);
    outT1 = normalize(cross(outwardNormal, a));
    outT2 = cross(outwardNormal, outT1);
}

SubsurfaceSample SubsurfaceSampleRelocation(
    const float3& sphereCenter, float sphereRadius, int sphereId,
    const SubsurfaceParams& params,
    const float3& xo, const float3& outwardNormal,
    uint32_t& rngState)
{
    SubsurfaceSample out;
    out.valid = false;
    out.pos = xo;
    out.normal = outwardNormal;
    out.sphereId = sphereId;

    // projection axis: normal with prob 1/2, tangents with 1/4 each
    float3 t1, t2;
    SubsurfaceFrame(outwardNormal, t1, t2);
    float uAxis = RandomFloat01(rngState);
    float3 axis;
    if (uAxis < 0.5f)
        axis = outwardNormal;
    else if (uAxis < 0.75f)
        axis = t1;
    else
        axis = t2;

    // color channel, then scale d or 3d, all uniform
    int channel = (int)(RandomFloat01(rngState) * 3.0f);
    if (channel > 2)
        channel = 2;
    float dc = Comp(params.d, channel);
    float sigma = (RandomFloat01(rngState) < 0.5f) ? dc : 3.0f * dc;

    // 2D Gaussian on the disk: Rayleigh radius, uniform azimuth
    float u1 = RandomFloat01(rngState);
    float radius = sigma * sqrtf(-2.0f * logf(1.0f - u1));
    float phi = 2.0f * kPI * RandomFloat01(rngState);
    float3 u, v;
    SubsurfaceFrame(axis, u, v);
    float3 p = xo + u * (cosf(phi) * radius) + v * (sinf(phi) * radius);

    // infinite line through p parallel to the axis vs. the current sphere only
    float3 oc = p - sphereCenter;
    float b = dot(oc, axis);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float discr = b * b - c;
    if (discr <= 0.0f)
        return out; // miss (or tangent, a zero-measure event): zero contribution, no resampling
    float sq = sqrtf(discr);
    float t = (RandomFloat01(rngState) < 0.5f) ? (-b - sq) : (-b + sq);
    out.pos = p + axis * t;
    out.normal = (out.pos - sphereCenter) * (1.0f / sphereRadius);
    out.valid = true;
    return out;
}

float SubsurfaceRelocationPdf(
    const float3& sphereCenter, float sphereRadius,
    const SubsurfaceParams& params,
    const float3& xo, const float3& outwardNormal,
    const float3& xi)
{
    float3 t1, t2;
    SubsurfaceFrame(outwardNormal, t1, t2);
    float3 axes[3] = { outwardNormal, t1, t2 };
    const float axisProb[3] = { 0.5f, 0.25f, 0.25f };
    float3 ni = (xi - sphereCenter) * (1.0f / sphereRadius);
    float3 dff = xi - xo;
    float eps = 1.0e-6f * sphereRadius * sphereRadius;
    float pdf = 0.0f;
    for (int j = 0; j < 3; ++j)
    {
        float3 a = axes[j];
        // this axis' own line through xi: foot point on the disk plane at xo
        float3 perp = dff - a * dot(dff, a);
        float3 lp = xo + perp;
        // candidate count of THAT line, recomputed per axis
        float3 oc = lp - sphereCenter;
        float b = dot(oc, a);
        float c = dot(oc, oc) - sphereRadius * sphereRadius;
        float discr = b * b - c;
        int count = (discr > eps) ? 2 : (discr > -eps ? 1 : 0);
        if (count == 0)
            continue;
        float r = length(perp);
        float sk = 0.0f;
        for (int ch = 0; ch < 3; ++ch)
            sk += SubsurfaceKernel(Comp(params.d, ch), r);
        sk *= 1.0f / 3.0f; // uniform channel choice; the d/3d choice is inside S_d
        pdf += axisProb[j] * sk * fabsf(dot(ni, a)) / (float)count;
    }
    return pdf;
}

float3 SubsurfaceWeight(const SubsurfaceParams& params, const float3& xo, const float3& xi, float areaPdf)
{
    if (!(areaPdf > 0.0f))
        return float3(0, 0, 0);
    float chord = length(xi - xo); // Euclidean distance
    float inv = 1.0f / areaPdf;
    return float3(
        params.rho.getX() * SubsurfaceKernel(params.d.getX(), chord) * inv,
        params.rho.getY() * SubsurfaceKernel(params.d.getY(), chord) * inv,
        params.rho.getZ() * SubsurfaceKernel(params.d.getZ(), chord) * inv);
}
