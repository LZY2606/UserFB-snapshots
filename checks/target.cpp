#include "../task_support/check.h"

#include "../apps/11_physically_based/pbr_material.h"

#include <renderer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace raw_ltc
{
struct mat33
{
    double m[9];
};
#include "../task_data/ltc.inc"
} // namespace raw_ltc

using pbr::AreaLight;
using pbr::Basis;
using pbr::Vec3d;
using FloatFB = Framebuffer<RGBAF, Depth>;

namespace
{

constexpr int kAreaIntegrationResolution = 160; // 160*160 deterministic midpoint samples
constexpr double kDiffuseReferenceAbsTolerance = 0.012;
constexpr double kDiffuseReferenceRelTolerance = 0.035;
constexpr double kLtcReferenceAbsTolerance = 0.025;
constexpr double kLtcReferenceRelTolerance = 0.070;

bool nearly(double a, double b, double absEps = 1.0e-8, double relEps = 1.0e-7)
{
    return std::abs(a - b) <= absEps + relEps * std::max(std::abs(a), std::abs(b));
}

double expectedTableValue(int k, double roughness, double ndv)
{
    const double a = std::clamp(roughness, 0.0, 1.0) * 63.0;
    const double t = std::sqrt(std::max(0.0, 1.0 - std::clamp(ndv, 0.0, 1.0))) * 63.0;
    const int a0 = std::min(62, static_cast<int>(std::floor(a)));
    const int t0 = std::min(62, static_cast<int>(std::floor(t)));
    const int a1 = a0 + 1;
    const int t1 = t0 + 1;
    const double fa = a - a0;
    const double ft = t - t0;

    auto at = [&](int ai, int ti) { return raw_ltc::tabMinv[ai + ti * 64].m[k]; };
    const double c0 = at(a0, t0) * (1.0 - fa) + at(a1, t0) * fa;
    const double c1 = at(a0, t1) * (1.0 - fa) + at(a1, t1) * fa;
    return c0 * (1.0 - ft) + c1 * ft;
}

double expectedMagnitude(double roughness, double ndv)
{
    const double a = std::clamp(roughness, 0.0, 1.0) * 63.0;
    const double t = std::sqrt(std::max(0.0, 1.0 - std::clamp(ndv, 0.0, 1.0))) * 63.0;
    const int a0 = std::min(62, static_cast<int>(std::floor(a)));
    const int t0 = std::min(62, static_cast<int>(std::floor(t)));
    const double fa = a - static_cast<double>(a0);
    const double ft = t - static_cast<double>(t0);
    auto at = [&](int ai, int ti) { return raw_ltc::tabMagnitude[ai + ti * 64]; };
    const double c0 = at(a0, t0) * (1.0 - fa) + at(a0 + 1, t0) * fa;
    const double c1 = at(a0, t0 + 1) * (1.0 - fa) + at(a0 + 1, t0 + 1) * fa;
    return c0 * (1.0 - ft) + c1 * ft;
}

Vec3d rotateAxis(const Vec3d& p, const Vec3d& axis_, double angle)
{
    const Vec3d a = pbr::normalize(axis_);
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return p * c + pbr::cross(a, p) * s + a * pbr::dot(a, p) * (1.0 - c);
}

struct Scenario
{
    std::array<Vec3d, 4> corners;
    Vec3d receiver;
    Vec3d normal;
    Vec3d view;
    double roughness;
};

std::vector<Scenario> referenceScenarios()
{
    std::vector<Scenario> scenarios;

    /* Tilted parallelogram crossing N.z=0. Its horizon-crossing edges also
       change x, so merely clamping negative corner z values is not equivalent
       to intersecting the edges with z=0. */
    {
        const Vec3d p0{-2.0, 1.0, -1.0};
        const Vec3d e1{3.0, 0.0, 0.8};
        const Vec3d e3{1.0, 0.0, 1.6};
        scenarios.push_back({
            {p0, p0 + e1, p0 + e1 + e3, p0 + e3},
            Vec3d{0.0, 0.0, 0.0},
            Vec3d{0.0, 0.0, 1.0},
            pbr::normalize(Vec3d{0.4, 0.15, 1.0}),
            0.85
        });
    }

    /* Long, low rectangle close to the shading normal's horizon. */
    scenarios.push_back({
        {Vec3d{-2.2, 3.2, 0.22}, Vec3d{2.2, 3.2, 0.22},
         Vec3d{2.2, 6.8, 0.22}, Vec3d{-2.2, 6.8, 0.22}},
        Vec3d{0.0, 0.0, 0.0},
        Vec3d{0.0, 0.0, 1.0},
        pbr::normalize(Vec3d{0.0, 0.95, 0.31}),
        0.90
    });

    /* Deterministic rotation and translation of the cross-horizon scene. */
    {
        const Scenario base = scenarios.front();
        const Vec3d axis = pbr::normalize(Vec3d{0.2, 0.9, 0.3});
        const Vec3d translation{3.0, -2.0, 1.5};
        Scenario moved = base;
        for (Vec3d& p : moved.corners)
            p = rotateAxis(p, axis, 0.73) + translation;
        moved.receiver = rotateAxis(base.receiver, axis, 0.73) + translation;
        moved.normal = pbr::normalize(rotateAxis(base.normal, axis, 0.73));
        moved.view = pbr::normalize(rotateAxis(base.view, axis, 0.73));
        moved.roughness = 0.85;
        scenarios.push_back(moved);
    }

    /* Entirely behind the shading hemisphere. */
    scenarios.push_back({
        {Vec3d{-1.0, 1.0, -1.4}, Vec3d{1.0, 1.0, -1.4},
         Vec3d{1.0, 3.0, -1.4}, Vec3d{-1.0, 3.0, -1.4}},
        Vec3d{0.0, 0.0, 0.0},
        Vec3d{0.0, 0.0, 1.0},
        Vec3d{0.0, 0.0, 1.0},
        0.8
    });

    return scenarios;
}

/* Independent midpoint quadrature over the rectangular area domain. */
double referenceDiffuseArea(const Scenario& s, int resolution)
{
    const Vec3d e1 = s.corners[1] - s.corners[0];
    const Vec3d e3 = s.corners[3] - s.corners[0];
    const Vec3d lightNormal = pbr::normalize(pbr::cross(e1, e3));
    const double area = pbr::length(pbr::cross(e1, e3));
    const double cellArea = area / static_cast<double>(resolution * resolution);

    double result = 0.0;
    for (int iy = 0; iy < resolution; ++iy)
    {
        const double v = (iy + 0.5) / resolution;
        for (int ix = 0; ix < resolution; ++ix)
        {
            const double u = (ix + 0.5) / resolution;
            const Vec3d p = s.corners[0] + e1 * u + e3 * v;
            const Vec3d delta = p - s.receiver;
            const double r2 = pbr::dot(delta, delta);
            const Vec3d omega = delta / std::sqrt(r2);

            const double receiverCos = std::max(0.0, pbr::dot(s.normal, omega));
            const double sourceCos = std::abs(pbr::dot(lightNormal, omega));
            const double solidAngle = sourceCos * cellArea / r2;
            result += receiverCos * solidAngle / pbr::PI;
        }
    }
    return result;
}

/* Area reference for the transformed LTC density. The radial linear-map
   Jacobian is |det A| / |A*omega|^3. */
double referenceLtcArea(const Scenario& s, int resolution)
{
    const auto table = pbr::sampleLtcTable(s.roughness, pbr::dot(s.normal, s.view));
    const Basis basis = pbr::makeBasis(s.normal, s.view);
    const double det = pbr::determinantColumnMajor(table.minv);

    const Vec3d e1 = s.corners[1] - s.corners[0];
    const Vec3d e3 = s.corners[3] - s.corners[0];
    const Vec3d lightNormal = pbr::normalize(pbr::cross(e1, e3));
    const double area = pbr::length(pbr::cross(e1, e3));
    const double cellArea = area / static_cast<double>(resolution * resolution);

    double result = 0.0;
    for (int iy = 0; iy < resolution; ++iy)
    {
        const double v = (iy + 0.5) / resolution;
        for (int ix = 0; ix < resolution; ++ix)
        {
            const double u = (ix + 0.5) / resolution;
            const Vec3d p = s.corners[0] + e1 * u + e3 * v;
            const Vec3d delta = p - s.receiver;
            const double r2 = pbr::dot(delta, delta);
            const double r = std::sqrt(r2);
            const Vec3d omega = delta / r;

            const Vec3d localOmega = pbr::toLocal(basis, omega);
            const Vec3d transformed = pbr::multiplyColumnMajor(table.minv, localOmega);
            const double transformedLength = pbr::length(transformed);
            const double transformedCos = std::max(0.0, transformed.z / transformedLength);
            const double jacobian =
                std::abs(det) / (transformedLength * transformedLength * transformedLength);

            const double sourceCos = std::abs(pbr::dot(lightNormal, omega));
            const double solidAngle = sourceCos * cellArea / r2;
            result += transformedCos * jacobian * solidAngle / pbr::PI;
        }
    }
    return result;
}

AreaLight twoSidedLight(const Scenario& s)
{
    AreaLight light;
    light.corners = s.corners;
    light.twoSided = true;
    light.radiance = Vec3d{1.0, 1.0, 1.0};
    return light;
}

BufferIndexed<pbr::Vertex, unsigned int> makeSyntheticQuad()
{
    BufferIndexed<pbr::Vertex, unsigned int> buffer;
    buffer.vertices = {
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{ 1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}
    };
    buffer.indices = {0, 1, 2, 0, 2, 3};
    return buffer;
}

void setupCamera(pbr::Uniforms& uniforms)
{
    uniforms.proj = Mat4::perspective(radians(60.0f), 1.0f, 0.1f, 10.0f);
    uniforms.view = Mat4::translation(Vec3{0.0f, 0.0f, -3.0f});
    uniforms.model = Mat4::identity();
    uniforms.viewPos = Vec3{0.0f, 0.0f, 3.0f};
}

void renderFloatQuad(
    Renderer& renderer,
    Program<pbr::Vertex, pbr::Varying, pbr::Uniforms, FloatFB>& program,
    const BufferIndexed<pbr::Vertex, unsigned int>& buffer,
    FloatFB& fb)
{
    fb.target<0>().fill(RGBAF(0.0f, 0.0f, 0.0f, 1.0f));
    fb.clear();
    renderer.draw(program, buffer, fb);
}

Vec3 metallicDirect(
    const Vec3& world,
    const Vec3& viewPosition,
    const Vec3& normal,
    const Vec3& baseColor,
    float metallic,
    float roughness,
    std::span<const AreaLight> lights)
{
    const Vec3 dielectricF0{0.04f, 0.04f, 0.04f};
    const Vec3 f0 = mix(dielectricF0, baseColor, metallic);
    const Vec3 diffuseColor = baseColor * (1.0f - dielectricF0.x) * (1.0f - metallic);
    return pbr::shadeDirectLights(
        world, viewPosition, normal, diffuseColor, f0, roughness, lights);
}

struct DrawError
{
    int badPixels = 0;
    float maxError = 0.0f;
    Vec3 meanColor{0.0f, 0.0f, 0.0f};
    int sampled = 0;
};

DrawError measureMaterialDraw(
    const FloatFB& fb,
    int width,
    const pbr::Uniforms& uniforms,
    const Vec3& baseColor,
    float metallic,
    float roughness,
    int normalR,
    int normalG,
    int normalB)
{
    DrawError report;
    const float planeDistance = 3.0f;
    const float planeScale = planeDistance * std::tan(radians(60.0f) * 0.5f);

    for (int y = 7; y < width - 7; y += 2)
    {
        for (int x = 7; x < width - 7; x += 2)
        {
            if (x == y)
                continue; // exact shared diagonal can have an edge-only gap
            const RGBAF pixel = fb.target<0>()(x, y);
            const float ndcX = 2.0f * (x + 0.5f) / width - 1.0f;
            const float ndcY = 2.0f * (y + 0.5f) / width - 1.0f;
            const Vec3 world{ndcX * planeScale, ndcY * planeScale, 0.0f};

            Vec3 tangentNormal{
                normalR / 255.0f * 2.0f - 1.0f,
                normalG / 255.0f * 2.0f - 1.0f,
                normalB / 255.0f * 2.0f - 1.0f
            };
            tangentNormal = normalize(tangentNormal);
            const Vec3 normal = normalize(Mat3::identity() * tangentNormal);

            const Vec3 expected = metallicDirect(
                world, uniforms.viewPos, normal, baseColor, metallic, roughness, uniforms.lights);

            for (int k = 0; k < 3; ++k)
            {
                const float actualValue = reinterpret_cast<const float*>(&pixel.x)[k];
                const float expectedValue = reinterpret_cast<const float*>(&expected.x)[k];
                const float error = std::abs(actualValue - expectedValue);
                const float tolerance = 0.025f + 0.04f * std::max(expectedValue, actualValue);
                report.maxError = std::max(report.maxError, error);
                if (!(error <= tolerance))
                    ++report.badPixels;
                reinterpret_cast<float*>(&report.meanColor.x)[k] += actualValue;
            }
            ++report.sampled;
        }
    }

    report.meanColor = report.meanColor / static_cast<float>(report.sampled);
    return report;
}

Vec3 regionMean(const FloatFB& fb, int center, int radius)
{
    Vec3 sum{0.0f, 0.0f, 0.0f};
    int count = 0;
    for (int y = center - radius; y <= center + radius; ++y)
    {
        for (int x = center - radius; x <= center + radius; ++x)
        {
            const RGBAF c = fb.target<0>()(x, y);
            sum = sum + Vec3(c.x, c.y, c.z);
            ++count;
        }
    }
    return sum / static_cast<float>(count);
}

} // namespace

/* ------------------------------- lookup -------------------------------- */

int lookup_bilinear_axes(void)
{
    TASK_CHECK(raw_ltc::size == 64);

    constexpr double roughness = 0.42;
    constexpr double viewParameter = 0.37;
    const double ndv = 1.0 - viewParameter * viewParameter;

    const pbr::LtcTableSample sample = pbr::sampleLtcTable(roughness, ndv);
    TASK_CHECK(sample.roughnessIndex == 26);
    TASK_CHECK(sample.viewIndex == 23);
    TASK_CHECK(nearly(sample.roughnessWeight, 0.46, 1.0e-12, 1.0e-12));
    TASK_CHECK(nearly(sample.viewWeight, 0.31, 1.0e-12, 1.0e-12));

    for (int k = 0; k < 9; ++k)
    {
        const double expected = expectedTableValue(k, roughness, ndv);
        TASK_CHECK(nearly(sample.minv[k], expected, 1.0e-5, 1.0e-6));
    }
    TASK_CHECK(nearly(sample.magnitude, expectedMagnitude(roughness, ndv), 1.0e-6, 1.0e-6));

    /* Axis orientation: index is roughnessIndex + viewIndex*64. */
    const pbr::LtcTableSample headOnSmooth = pbr::sampleLtcTable(0.0, 1.0);
    const pbr::LtcTableSample headOnRough = pbr::sampleLtcTable(1.0, 1.0);
    TASK_CHECK(headOnSmooth.roughnessIndex == 0);
    TASK_CHECK(headOnSmooth.viewIndex == 0);
    TASK_CHECK(headOnSmooth.minv[0] > 40000.0);
    TASK_CHECK(headOnRough.minv[0] < 1.5);

    const pbr::LtcTableSample grazingSmooth = pbr::sampleLtcTable(0.0, 0.0);
    TASK_CHECK(grazingSmooth.viewIndex == 62);
    TASK_CHECK(nearly(grazingSmooth.viewWeight, 1.0, 1.0e-12, 1.0e-12));
    TASK_CHECK(grazingSmooth.minv[6] > 100.0);

    return 0;
}

int lookup_matrix_column_major(void)
{
    /* Half-way between view rows 32 and 33, roughness column zero. */
    const double q = 32.5 / 63.0;
    const double ndv = 1.0 - q * q;
    const pbr::LtcTableSample sample = pbr::sampleLtcTable(0.0, ndv);

    TASK_CHECK(sample.roughnessIndex == 0);
    TASK_CHECK(sample.viewIndex == 32);
    TASK_CHECK(nearly(sample.viewWeight, 0.5, 1.0e-12, 1.0e-12));

    const Vec3d xColumn = pbr::multiplyColumnMajor(sample.minv, Vec3d{1, 0, 0});
    const Vec3d yColumn = pbr::multiplyColumnMajor(sample.minv, Vec3d{0, 1, 0});
    const Vec3d zColumn = pbr::multiplyColumnMajor(sample.minv, Vec3d{0, 0, 1});

    TASK_CHECK(nearly(xColumn.x, sample.minv[0], 1.0e-8, 1.0e-7));
    TASK_CHECK(nearly(xColumn.y, sample.minv[1], 1.0e-8, 1.0e-7));
    TASK_CHECK(nearly(xColumn.z, sample.minv[2], 1.0e-8, 1.0e-7));
    TASK_CHECK(nearly(yColumn.x, sample.minv[3], 1.0e-8, 1.0e-7));
    TASK_CHECK(nearly(yColumn.y, sample.minv[4], 1.0e-8, 1.0e-7));
    TASK_CHECK(nearly(yColumn.z, sample.minv[5], 1.0e-8, 1.0e-7));
    TASK_CHECK(nearly(zColumn.x, sample.minv[6], 1.0e-8, 1.0e-7));
    TASK_CHECK(nearly(zColumn.y, sample.minv[7], 1.0e-8, 1.0e-7));
    TASK_CHECK(nearly(zColumn.z, sample.minv[8], 1.0e-8, 1.0e-7));

    /* This fitted cell is strongly asymmetric; transposing it would move the
       large z-column term to row 2 and fail the sign/size checks. */
    TASK_CHECK(sample.minv[6] > 1000.0);
    TASK_CHECK(sample.minv[2] < 0.0);
    TASK_CHECK(zColumn.x > 1000.0);
    TASK_CHECK(std::abs(zColumn.z - sample.minv[8]) < 1.0e-6);

    return 0;
}

/* ------------------------------ integral ------------------------------- */

int integral_diffuse_area_reference(void)
{
    const std::vector<Scenario> scenarios = referenceScenarios();
    for (std::size_t i = 0; i < scenarios.size(); ++i)
    {
        const Scenario& s = scenarios[i];
        const AreaLight light = twoSidedLight(s);
        const auto got = pbr::computeLightIntegrals(
            light, s.receiver, s.normal, s.view, s.roughness);
        const double reference = referenceDiffuseArea(s, kAreaIntegrationResolution);
        const double tolerance =
            kDiffuseReferenceAbsTolerance +
            kDiffuseReferenceRelTolerance * std::max(got.diffuse, reference);
        const bool ok = std::isfinite(got.diffuse) &&
                        std::abs(got.diffuse - reference) <= tolerance;
        if (!ok)
            std::fprintf(stderr, "diffuse scenario %zu edge=%.9f area=%.9f tol=%.5f\n",
                          i, got.diffuse, reference, tolerance);
        TASK_CHECK(ok);
    }
    return 0;
}

int integral_ltc_density_area_reference(void)
{
    const std::vector<Scenario> scenarios = referenceScenarios();
    for (std::size_t i = 0; i < scenarios.size(); ++i)
    {
        const Scenario& s = scenarios[i];
        const AreaLight light = twoSidedLight(s);
        const auto got = pbr::computeLightIntegrals(
            light, s.receiver, s.normal, s.view, s.roughness);
        const double reference = referenceLtcArea(s, kAreaIntegrationResolution);
        const double tolerance =
            kLtcReferenceAbsTolerance +
            kLtcReferenceRelTolerance * std::max(got.specular, reference);
        const bool ok = std::isfinite(got.specular) &&
                        std::abs(got.specular - reference) <= tolerance;
        if (!ok)
            std::fprintf(stderr, "ltc scenario %zu edge=%.9f area=%.9f tol=%.5f\n",
                          i, got.specular, reference, tolerance);
        TASK_CHECK(ok);
    }
    return 0;
}

int integral_horizon_winding_sides(void)
{
    const Scenario crossScene = referenceScenarios().front();
    AreaLight light = twoSidedLight(crossScene);
    const auto forward = pbr::computeLightIntegrals(
        light, crossScene.receiver, crossScene.normal, crossScene.view, 0.85);

    AreaLight reversed = light;
    std::reverse(reversed.corners.begin(), reversed.corners.end());
    const auto backward = pbr::computeLightIntegrals(
        reversed, crossScene.receiver, crossScene.normal, crossScene.view, 0.85);

    TASK_CHECK(nearly(forward.diffuse, backward.diffuse, 1.0e-10, 1.0e-8));
    TASK_CHECK(nearly(forward.specular, backward.specular, 1.0e-10, 1.0e-8));

    /* Analytic true horizon intersections for the tilted parallelogram. */
    const auto clipped = pbr::clipPolygonToPositiveZ(std::span<const Vec3d>(crossScene.corners));
    TASK_CHECK(clipped.count >= 3);
    bool foundIntersectionA = false;
    bool foundIntersectionB = false;
    for (int i = 0; i < clipped.count; ++i)
    {
        const Vec3d& p = clipped.vertices[i];
        if (std::abs(p.z) < 1.0e-9)
        {
            if (std::abs(p.x - 1.125) < 1.0e-7 && std::abs(p.y - 1.0) < 1.0e-9)
                foundIntersectionA = true;
            if (std::abs(p.x + 1.375) < 1.0e-7 && std::abs(p.y - 1.0) < 1.0e-9)
                foundIntersectionB = true;
        }
    }
    TASK_CHECK(foundIntersectionA);
    TASK_CHECK(foundIntersectionB);

    /* Front face normal cross(e1,e3) = -z; receiver is on that -z side. */
    AreaLight front;
    front.corners = {
        Vec3d{-1, -1, 1}, Vec3d{-1, 1, 1},
        Vec3d{1, 1, 1}, Vec3d{1, -1, 1}
    };
    front.twoSided = false;
    const auto frontHit = pbr::computeLightIntegrals(
        front, Vec3d{0, 0, 0}, Vec3d{0, 0, 1}, Vec3d{0, 0, 1}, 0.7);
    TASK_CHECK(frontHit.frontFacing);
    TASK_CHECK(frontHit.diffuse > 0.10);
    TASK_CHECK(frontHit.specular >= 0.0);

    AreaLight back = front;
    std::reverse(back.corners.begin(), back.corners.end());
    back.twoSided = false;
    const auto backHit = pbr::computeLightIntegrals(
        back, Vec3d{0, 0, 0}, Vec3d{0, 0, 1}, Vec3d{0, 0, 1}, 0.7);
    TASK_CHECK(!backHit.frontFacing);
    TASK_CHECK(backHit.diffuse == 0.0);
    TASK_CHECK(backHit.specular == 0.0);

    /* Strictly below the local horizon. */
    const std::array<Vec3d, 4> below = {
        Vec3d{-1, 1, -0.2}, Vec3d{1, 1, -0.2},
        Vec3d{1, 3, -0.2}, Vec3d{-1, 3, -0.2}
    };
    TASK_CHECK(pbr::sphericalPolygonIntegral(below) == 0.0);

    return 0;
}

int integral_degenerate_and_grazing(void)
{
    const std::array<Vec3d, 4> diamond = {
        Vec3d{1, 0, 1}, Vec3d{0, 1, 1},
        Vec3d{-1, 0, 1}, Vec3d{0, -1, 1}
    };
    const double normalIntegral = pbr::sphericalPolygonIntegral(diamond);
    TASK_CHECK(std::isfinite(normalIntegral));
    TASK_CHECK(normalIntegral > 0.1);

    const std::array<Vec3d, 4> duplicate = {
        diamond[0], diamond[0], diamond[1], diamond[2]
    };
    const std::array<Vec3d, 3> triangle = {diamond[0], diamond[1], diamond[2]};
    TASK_CHECK(nearly(
        pbr::sphericalPolygonIntegral(duplicate),
        pbr::sphericalPolygonIntegral(triangle),
        1.0e-12, 1.0e-10));

    const std::array<Vec3d, 4> coincident = {
        Vec3d{1, 0, 1}, Vec3d{1, 0, 1}, Vec3d{1, 0, 1}, Vec3d{1, 0, 1}
    };
    TASK_CHECK(pbr::sphericalPolygonIntegral(coincident) == 0.0);

    const std::array<Vec3d, 3> antipodalHorizon = {
        Vec3d{1, 0, 0}, Vec3d{-1, 0, 0}, Vec3d{0, 1, 1}
    };
    TASK_CHECK(std::isfinite(pbr::sphericalPolygonIntegral(antipodalHorizon)));

    /* Legal, non-degenerate rectangle at an extreme grazing configuration. */
    Scenario grazing;
    grazing.corners = {
        Vec3d{-5.0, 20.0, 0.05}, Vec3d{5.0, 20.0, 0.05},
        Vec3d{5.0, 40.0, 0.05}, Vec3d{-5.0, 40.0, 0.05}
    };
    grazing.receiver = Vec3d{0, 0, 0};
    grazing.normal = Vec3d{0, 0, 1};
    grazing.view = pbr::normalize(Vec3d{0, 0.995, 0.1});
    grazing.roughness = 0.0;
    AreaLight light = twoSidedLight(grazing);
    const auto extreme = pbr::computeLightIntegrals(
        light, grazing.receiver, grazing.normal, grazing.view, grazing.roughness);
    TASK_CHECK(std::isfinite(extreme.diffuse));
    TASK_CHECK(std::isfinite(extreme.specular));
    TASK_CHECK(extreme.diffuse >= 0.0 && extreme.specular >= 0.0);

    return 0;
}

/* ------------------------------- material ------------------------------ */

int material_shading_formula(void)
{
    AreaLight light;
    light.corners = {
        Vec3d{-1, -1, 1}, Vec3d{1, -1, 1},
        Vec3d{1, 1, 1}, Vec3d{-1, 1, 1}
    };
    light.twoSided = true;
    light.radiance = Vec3d{2.0, 3.0, 4.0};

    const Vec3 base{0.70f, 0.50f, 0.30f};
    const Vec3 diffuseColor = base * 0.96f;
    const Vec3 f0{0.04f, 0.04f, 0.04f};
    const auto integrals = pbr::computeLightIntegrals(
        light, Vec3d{0, 0, 0}, Vec3d{0, 0, 1}, Vec3d{0, 0, 1}, 0.6f);

    const Vec3 expected{
        static_cast<float>(light.radiance.x *
            (diffuseColor.x * integrals.diffuse + f0.x * integrals.magnitude * integrals.specular)),
        static_cast<float>(light.radiance.y *
            (diffuseColor.y * integrals.diffuse + f0.y * integrals.magnitude * integrals.specular)),
        static_cast<float>(light.radiance.z *
            (diffuseColor.z * integrals.diffuse + f0.z * integrals.magnitude * integrals.specular))
    };
    const std::vector<AreaLight> lights = {light};
    const Vec3 actual = metallicDirect(
        Vec3{0, 0, 0}, Vec3{0, 0, 3}, Vec3{0, 0, 1}, base, 0.0f, 0.6f, lights);

    for (int k = 0; k < 3; ++k)
        TASK_CHECK(nearly(reinterpret_cast<const float*>(&actual.x)[k],
                          reinterpret_cast<const float*>(&expected.x)[k], 1.0e-6f, 1.0e-5f));

    /* Metallic workflow: diffuse attenuation is zero and F0 is baseColor. */
    const Vec3 metalActual = metallicDirect(
        Vec3{0, 0, 0}, Vec3{0, 0, 3}, Vec3{0, 0, 1}, base, 1.0f, 0.4f, lights);
    /* Recompute integrals at the metal roughness used above. */
    const auto metalIntegrals = pbr::computeLightIntegrals(
        light, Vec3d{0, 0, 0}, Vec3d{0, 0, 1}, Vec3d{0, 0, 1}, 0.4);
    const Vec3 metalExpected{
        static_cast<float>(light.radiance.x * base.x * metalIntegrals.magnitude * metalIntegrals.specular),
        static_cast<float>(light.radiance.y * base.y * metalIntegrals.magnitude * metalIntegrals.specular),
        static_cast<float>(light.radiance.z * base.z * metalIntegrals.magnitude * metalIntegrals.specular)
    };
    for (int k = 0; k < 3; ++k)
        TASK_CHECK(nearly(reinterpret_cast<const float*>(&metalActual.x)[k],
                          reinterpret_cast<const float*>(&metalExpected.x)[k], 1.0e-6f, 1.0e-5f));

    /* Multiple lights add before quantization/clamping. */
    AreaLight second = light;
    second.radiance = Vec3d{1.5, 0.25, 0.75};
    const std::vector<AreaLight> firstSet = {light};
    const std::vector<AreaLight> secondSet = {second};
    const std::vector<AreaLight> bothSet = {light, second};
    const Vec3 firstColor = metallicDirect(
        Vec3{0, 0, 0}, Vec3{0, 0, 3}, Vec3{0, 0, 1}, base, 0.0f, 0.6f, firstSet);
    const Vec3 secondColor = metallicDirect(
        Vec3{0, 0, 0}, Vec3{0, 0, 3}, Vec3{0, 0, 1}, base, 0.0f, 0.6f, secondSet);
    const Vec3 bothColor = metallicDirect(
        Vec3{0, 0, 0}, Vec3{0, 0, 3}, Vec3{0, 0, 1}, base, 0.0f, 0.6f, bothSet);
    for (int k = 0; k < 3; ++k)
        TASK_CHECK(nearly(reinterpret_cast<const float*>(&bothColor.x)[k],
                          reinterpret_cast<const float*>(&firstColor.x)[k] +
                          reinterpret_cast<const float*>(&secondColor.x)[k],
                          1.0e-6f, 1.0e-5f));

    const std::vector<AreaLight> noLights;
    const Vec3 noLightColor = metallicDirect(
        Vec3{0, 0, 0}, Vec3{0, 0, 3}, Vec3{0, 0, 1}, base, 0.0f, 0.6f, noLights);
    TASK_CHECK(noLightColor.x == 0.0f && noLightColor.y == 0.0f && noLightColor.z == 0.0f);

    return 0;
}

int material_real_draw_normals_metal_roughness(void)
{
    constexpr int W = 31;
    Renderer renderer(W, W);
    FloatFB fb(W, W);
    renderer.options().culling = false;

    auto buffer = makeSyntheticQuad();
    Program<pbr::Vertex, pbr::Varying, pbr::Uniforms, FloatFB> program;
    program.onVertex(pbr::vertexShader);
    program.onFragment([](const pbr::Uniforms& uniforms, const pbr::Varying& varying,
                          FloatFB::TargetFragments& out)
    {
        std::get<0>(out) = Vec4(pbr::shadeFragment(uniforms, varying), 1.0f);
    });

    pbr::Uniforms& uniforms = program.uniforms();
    setupCamera(uniforms);

    AreaLight offAxis;
    offAxis.corners = {
        Vec3d{0.8, -0.7, 1.0}, Vec3d{2.0, -0.7, 1.0},
        Vec3d{2.0, 0.7, 1.0}, Vec3d{0.8, 0.7, 1.0}
    };
    offAxis.twoSided = true;
    offAxis.radiance = Vec3d{3.0, 2.4, 1.8};
    uniforms.lights = {offAxis};

    Texture<RGBA8> baseTexture(1, 1, RGBA8(232, 128, 48, 255));
    Texture<RGBA8> nonMetalMr(1, 1, RGBA8(0, 178, 0, 255));
    Texture<RGBA8> metalMr(1, 1, RGBA8(0, 70, 255, 255));
    Texture<RGBA8> roughLowMr(1, 1, RGBA8(0, 38, 255, 255));
    Texture<RGBA8> roughHighMr(1, 1, RGBA8(0, 230, 255, 255));
    Texture<RGBA8> flatNormal(1, 1, RGBA8(128, 128, 255, 255));
    Texture<RGBA8> tiltedNormal(1, 1, RGBA8(255, 128, 255, 255));

    uniforms.material.albedo = baseTexture;
    uniforms.material.metallic_roughness = nonMetalMr;
    uniforms.material.normal = flatNormal;
    uniforms.material.albedo.filter = eFilter::NEAREST;
    uniforms.material.metallic_roughness.filter = eFilter::NEAREST;
    uniforms.material.normal.filter = eFilter::NEAREST;

    const Vec3 baseColor{232 / 255.0f, 128 / 255.0f, 48 / 255.0f};

    renderFloatQuad(renderer, program, buffer, fb);
    DrawError flatReport = measureMaterialDraw(
        fb, W, uniforms, baseColor, 0.0f, 178 / 255.0f, 128, 128, 255);
    if (flatReport.badPixels)
        std::fprintf(stderr, "flat normal draw mismatches=%d max=%.5f\n",
                     flatReport.badPixels, flatReport.maxError);
    TASK_CHECK(flatReport.badPixels == 0);
    TASK_CHECK(flatReport.maxError < 0.08f);
    const Vec3 flatMean = regionMean(fb, W / 2, 4);

    uniforms.material.normal = tiltedNormal;
    renderFloatQuad(renderer, program, buffer, fb);
    DrawError tiltedReport = measureMaterialDraw(
        fb, W, uniforms, baseColor, 0.0f, 178 / 255.0f, 255, 128, 255);
    if (tiltedReport.badPixels)
        std::fprintf(stderr, "tilted normal draw mismatches=%d max=%.5f\n",
                     tiltedReport.badPixels, tiltedReport.maxError);
    TASK_CHECK(tiltedReport.badPixels == 0);
    const Vec3 tiltedMean = regionMean(fb, W / 2, 4);
    const float normalChange = length(tiltedMean - flatMean);
    TASK_CHECK(normalChange > 0.015f);

    uniforms.material.normal = flatNormal;
    uniforms.material.metallic_roughness = metalMr;
    renderFloatQuad(renderer, program, buffer, fb);
    DrawError metalReport = measureMaterialDraw(
        fb, W, uniforms, baseColor, 1.0f, 70 / 255.0f, 128, 128, 255);
    TASK_CHECK(metalReport.badPixels == 0);
    TASK_CHECK(metalReport.maxError < 0.08f);

    uniforms.material.metallic_roughness = roughLowMr;
    renderFloatQuad(renderer, program, buffer, fb);
    const Vec3 lowRoughMean = regionMean(fb, W / 2, 4);
    DrawError lowRoughReport = measureMaterialDraw(
        fb, W, uniforms, baseColor, 1.0f, 38 / 255.0f, 128, 128, 255);
    TASK_CHECK(lowRoughReport.badPixels == 0);

    uniforms.material.metallic_roughness = roughHighMr;
    renderFloatQuad(renderer, program, buffer, fb);
    const Vec3 highRoughMean = regionMean(fb, W / 2, 4);
    DrawError highRoughReport = measureMaterialDraw(
        fb, W, uniforms, baseColor, 1.0f, 230 / 255.0f, 128, 128, 255);
    TASK_CHECK(highRoughReport.badPixels == 0);

    const float roughnessChange = length(lowRoughMean - highRoughMean);
    if (!(roughnessChange > 0.01f))
        std::fprintf(stderr, "roughness means low=(%.4f %.4f %.4f) high=(%.4f %.4f %.4f)\n",
                     lowRoughMean.x, lowRoughMean.y, lowRoughMean.z,
                     highRoughMean.x, highRoughMean.y, highRoughMean.z);
    TASK_CHECK(roughnessChange > 0.01f);

    return 0;
}

int material_draw_lights_hdr_addition_and_reset(void)
{
    constexpr int W = 27;
    Renderer renderer(W, W);
    FloatFB fb(W, W);
    renderer.options().culling = false;

    auto buffer = makeSyntheticQuad();
    Program<pbr::Vertex, pbr::Varying, pbr::Uniforms, FloatFB> program;
    program.onVertex(pbr::vertexShader);
    program.onFragment([](const pbr::Uniforms& uniforms, const pbr::Varying& varying,
                          FloatFB::TargetFragments& out)
    {
        std::get<0>(out) = Vec4(pbr::shadeFragment(uniforms, varying), 1.0f);
    });

    pbr::Uniforms& uniforms = program.uniforms();
    setupCamera(uniforms);

    Texture<RGBA8> white(1, 1, RGBA8(255, 255, 255, 255));
    Texture<RGBA8> roughMr(1, 1, RGBA8(0, 225, 0, 255));
    Texture<RGBA8> normal(1, 1, RGBA8(128, 128, 255, 255));
    uniforms.material.albedo = white;
    uniforms.material.metallic_roughness = roughMr;
    uniforms.material.normal = normal;
    uniforms.material.albedo.filter = eFilter::NEAREST;
    uniforms.material.metallic_roughness.filter = eFilter::NEAREST;
    uniforms.material.normal.filter = eFilter::NEAREST;

    AreaLight red;
    red.corners = {
        Vec3d{-1.8, -1.8, 0.8}, Vec3d{1.8, -1.8, 0.8},
        Vec3d{1.8, 1.8, 0.8}, Vec3d{-1.8, 1.8, 0.8}
    };
    red.twoSided = true;
    red.radiance = Vec3d{8.0, 0.4, 0.4};

    AreaLight green = red;
    green.radiance = Vec3d{0.4, 6.0, 1.5};

    uniforms.lights = {red};
    renderFloatQuad(renderer, program, buffer, fb);
    const Vec3 redMean = regionMean(fb, W / 2, 2);
    TASK_CHECK(redMean.x > 1.0f);

    uniforms.lights = {green};
    renderFloatQuad(renderer, program, buffer, fb);
    const Vec3 greenMean = regionMean(fb, W / 2, 2);
    TASK_CHECK(greenMean.y > 1.0f);

    uniforms.lights = {red, green};
    renderFloatQuad(renderer, program, buffer, fb);
    const Vec3 bothMean = regionMean(fb, W / 2, 2);
    TASK_CHECK(bothMean.x > 2.0f || bothMean.y > 2.0f);

    int addBad = 0;
    float addMaxError = 0.0f;
    /* Re-render single lights to buffers is unnecessary for means; compare the
       center 5x5 against direct component calls at reconstructed positions. */
    for (int y = W / 2 - 2; y <= W / 2 + 2; ++y)
    {
        for (int x = W / 2 - 2; x <= W / 2 + 2; ++x)
        {
            if (x == y)
                continue;
            const RGBAF pixel = fb.target<0>()(x, y);
            const float ndcX = 2.0f * (x + 0.5f) / W - 1.0f;
            const float ndcY = 2.0f * (y + 0.5f) / W - 1.0f;
            const float planeScale = 3.0f * std::tan(radians(60.0f) * 0.5f);
            const Vec3 world{ndcX * planeScale, ndcY * planeScale, 0.0f};
            const std::vector<AreaLight> redSet = {red};
            const std::vector<AreaLight> greenSet = {green};
            const Vec3 r = metallicDirect(
                world, uniforms.viewPos, Vec3{0, 0, 1}, Vec3{1, 1, 1},
                0.0f, 225 / 255.0f, redSet);
            const Vec3 g = metallicDirect(
                world, uniforms.viewPos, Vec3{0, 0, 1}, Vec3{1, 1, 1},
                0.0f, 225 / 255.0f, greenSet);
            for (int k = 0; k < 3; ++k)
            {
                const float actualValue = reinterpret_cast<const float*>(&pixel.x)[k];
                const float sumValue = reinterpret_cast<const float*>(&r.x)[k] +
                                       reinterpret_cast<const float*>(&g.x)[k];
                const float error = std::abs(actualValue - sumValue);
                addMaxError = std::max(addMaxError, error);
                if (error > 0.03f + 0.04f * std::max(actualValue, sumValue))
                    ++addBad;
            }
        }
    }
    TASK_CHECK(addBad == 0);
    TASK_CHECK(addMaxError < 0.12f);

    /* Replacing the list with no lights and replacing materials must not leave
       any contribution from the previous list in this persistent Program. */
    Texture<RGBA8> replacementBase(1, 1, RGBA8(13, 219, 97, 255));
    Texture<RGBA8> replacementMr(1, 1, RGBA8(0, 111, 255, 255));
    uniforms.material.albedo = replacementBase;
    uniforms.material.metallic_roughness = replacementMr;
    uniforms.lights.clear();
    renderFloatQuad(renderer, program, buffer, fb);

    int nonzeroRgb = 0;
    for (const RGBAF& pixel : fb.target<0>().data())
    {
        if (pixel.x != 0.0f || pixel.y != 0.0f || pixel.z != 0.0f)
            ++nonzeroRgb;
    }
    TASK_CHECK(nonzeroRgb == 0);

    return 0;
}

int material_visible_light_geometry(void)
{
    constexpr int W = 31;
    Renderer renderer(W, W);
    FloatFB fb(W, W);
    renderer.options().culling = false;

    auto materialBuffer = makeSyntheticQuad();
    Program<pbr::Vertex, pbr::Varying, pbr::Uniforms, FloatFB> materialProgram;
    materialProgram.onVertex(pbr::vertexShader);
    materialProgram.onFragment([](const pbr::Uniforms& uniforms, const pbr::Varying& varying,
                                  FloatFB::TargetFragments& out)
    {
        std::get<0>(out) = Vec4(pbr::shadeFragment(uniforms, varying), 1.0f);
    });
    setupCamera(materialProgram.uniforms());

    Texture<RGBA8> white(1, 1, RGBA8(255, 255, 255, 255));
    Texture<RGBA8> mr(1, 1, RGBA8(0, 200, 0, 255));
    Texture<RGBA8> normal(1, 1, RGBA8(128, 128, 255, 255));
    auto& mu = materialProgram.uniforms();
    mu.material.albedo = white;
    mu.material.metallic_roughness = mr;
    mu.material.normal = normal;

    AreaLight visible;
    visible.corners = {
        Vec3d{-0.9, 0.55, 0.5}, Vec3d{0.9, 0.55, 0.5},
        Vec3d{0.9, 1.15, 0.5}, Vec3d{-0.9, 1.15, 0.5}
    };
    visible.twoSided = true;
    visible.radiance = Vec3d{1.0, 1.0, 1.0};
    mu.lights = {visible};
    renderFloatQuad(renderer, materialProgram, materialBuffer, fb);
    TASK_CHECK(regionMean(fb, W / 2, 3).x > 0.01f);

    struct GlowUniforms
    {
        Mat4 proj;
        Mat4 view;
        Vec4 glow;
    };
    struct GlowVarying
    {
        Vec4 position;
        VARYING(position);
    };
    struct GlowVertex
    {
        Vec3 position;
    };

    Program<GlowVertex, GlowVarying, GlowUniforms, FloatFB> glowProgram;
    glowProgram.onVertex([](const GlowUniforms& u, const GlowVertex& v, GlowVarying& out)
    {
        out.position = u.proj * u.view * Vec4(v.position, 1.0f);
    });
    glowProgram.onFragment([](const GlowUniforms& u, const GlowVarying&,
                              FloatFB::TargetFragments& out)
    {
        std::get<0>(out) = u.glow;
    });
    glowProgram.uniforms().proj = mu.proj;
    glowProgram.uniforms().view = mu.view;
    glowProgram.uniforms().glow = Vec4(7.0f, 5.0f, 3.0f, 1.0f);

    BufferIndexed<GlowVertex, unsigned int> glowBuffer;
    for (const Vec3d& corner : visible.corners)
        glowBuffer.vertices.push_back({{static_cast<float>(corner.x),
                                        static_cast<float>(corner.y),
                                        static_cast<float>(corner.z)}});
    glowBuffer.indices = {0, 1, 2, 0, 2, 3};
    renderer.draw(glowProgram, glowBuffer, fb);

    int hdrGlowPixels = 0;
    for (int y = 0; y < W; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const RGBAF c = fb.target<0>()(x, y);
            if (c.x > 6.5f && c.y > 4.5f && c.z > 2.5f)
                ++hdrGlowPixels;
        }
    }
    TASK_CHECK(hdrGlowPixels >= 12);

    return 0;
}

int material_default_target_quantization(void)
{
    constexpr int W = 24;
    Renderer renderer(W, W);
    renderer.options().culling = false;

    Program<pbr::Vertex, pbr::Varying, pbr::Uniforms> program;
    program.onVertex(pbr::vertexShader);
    program.onFragment(pbr::fragmentToDefaultTarget);
    setupCamera(program.uniforms());

    Texture<RGBA8> white(1, 1, RGBA8(255, 255, 255, 255));
    Texture<RGBA8> roughMr(1, 1, RGBA8(0, 220, 0, 255));
    Texture<RGBA8> normal(1, 1, RGBA8(128, 128, 255, 255));
    program.uniforms().material.albedo = white;
    program.uniforms().material.metallic_roughness = roughMr;
    program.uniforms().material.normal = normal;

    AreaLight bright;
    bright.corners = {
        Vec3d{-1.5, -1.5, 1.0}, Vec3d{1.5, -1.5, 1.0},
        Vec3d{1.5, 1.5, 1.0}, Vec3d{-1.5, 1.5, 1.0}
    };
    bright.twoSided = true;
    bright.radiance = Vec3d{20.0, 0.0, 0.0};
    program.uniforms().lights = {bright};

    renderer.framebuffer().clear(Vec4(0, 0, 0, 1));
    auto buffer = makeSyntheticQuad();
    renderer.draw(program, buffer);

    int covered = 0;
    int redQuantized = 0;
    for (const RGBA8& pixel : renderer.framebuffer().color().data())
    {
        if (pixel.x > 0 || pixel.y > 0 || pixel.z > 0)
            ++covered;
        if (pixel.x == 255)
            ++redQuantized;
    }
    TASK_CHECK(covered > 100);
    TASK_CHECK(redQuantized > 20);

    return 0;
}

int material_no_lights_matches_ambient(void)
{
    pbr::Uniforms uniforms;
    uniforms.viewPos = Vec3{0.0f, 0.0f, 3.0f};
    uniforms.lights.clear();

    Texture<RGBA8> albedo(1, 1, RGBA8(201, 157, 91, 255));
    Texture<RGBA8> metallicRoughness(1, 1, RGBA8(0, 128, 37, 255));
    Texture<RGBA8> normalMap(1, 1, RGBA8(128, 128, 255, 255));
    Texture<RGBAF> irradiance(1, 1, RGBAF(0.18f, 0.27f, 0.41f, 1.0f));
    Texture<RGBAF> radiance(1, 1, RGBAF(0.55f, 0.43f, 0.31f, 1.0f));
    Texture<RGBAF> brdf(1, 1, RGBAF(0.82f, 0.64f, 0.0f, 1.0f));

    uniforms.material.albedo = albedo;
    uniforms.material.metallic_roughness = metallicRoughness;
    uniforms.material.normal = normalMap;
    uniforms.irradiance = irradiance;
    uniforms.prefilter_radiance = radiance;
    uniforms.brdf = brdf;
    uniforms.material.albedo.filter = eFilter::NEAREST;
    uniforms.material.metallic_roughness.filter = eFilter::NEAREST;
    uniforms.material.normal.filter = eFilter::NEAREST;
    uniforms.irradiance.filter = eFilter::NEAREST;
    uniforms.prefilter_radiance.filter = eFilter::NEAREST;
    uniforms.brdf.filter = eFilter::NEAREST;

    pbr::Varying varying;
    varying.world_position = Vec3{0.0f, 0.0f, 0.0f};
    varying.uv = Vec2{0.5f, 0.5f};
    varying.TBN = Mat3::identity();

    const Vec3 actual = pbr::shadeFragment(uniforms, varying);

    /* Independent copy of the original ambient-only PBR expression. */
    const Vec3 n = normalize(varying.TBN * normalize(
        Vec3(texture(uniforms.material.normal, varying.uv) / 255.0f) * 2.0f - 1.0f));
    const Vec3 v = normalize(uniforms.viewPos - varying.world_position);
    const float nv = std::max(dot(n, v), 0.0f);
    const Vec3 r = reflect(-v, n);
    Vec3 baseColor = Vec3(texture(uniforms.material.albedo, varying.uv) / 255.0f);
    const auto metalRough = texture(uniforms.material.metallic_roughness, varying.uv) / 255.0f;
    const float metallic = metalRough.z;
    const float roughness = metalRough.y;
    const Vec3 dielectricF0{0.04f, 0.04f, 0.04f};
    Vec3 f0 = mix(dielectricF0, baseColor, metallic);
    baseColor = baseColor * (1.0f - dielectricF0.x) * (1.0f - metallic);
    const Vec3 envRadiance = Vec3(textureLod(
        uniforms.prefilter_radiance, equirectangularUV(r),
        roughness * static_cast<float>(uniforms.prefilter_radiance.m_texture->num_mipmaps())));
    const Vec3 envIrradiance = Vec3(texture(uniforms.irradiance, equirectangularUV(n)));
    const Vec2 envBrdf = Vec2(texture(uniforms.brdf, Vec2(nv, roughness)));
    const Vec3 fr = max(Vec3(1.0f - roughness), f0) - f0;
    const Vec3 kS = f0 + fr * pow(1.0f - nv, 5.0f);
    const Vec3 fssEss = kS * envBrdf.x + Vec3(envBrdf.y);
    const Vec3 expected = fssEss * envRadiance + baseColor * envIrradiance;

    for (int k = 0; k < 3; ++k)
        TASK_CHECK(nearly(reinterpret_cast<const float*>(&actual.x)[k],
                          reinterpret_cast<const float*>(&expected.x)[k], 1.0e-6, 1.0e-6));

    return 0;
}

const task_case task_cases[] = {
    {"lookup", "bilinear_axes", lookup_bilinear_axes},
    {"lookup", "matrix_column_major", lookup_matrix_column_major},
    {"integral", "diffuse_area_reference", integral_diffuse_area_reference},
    {"integral", "ltc_density_area_reference", integral_ltc_density_area_reference},
    {"integral", "horizon_winding_sides", integral_horizon_winding_sides},
    {"integral", "degenerate_and_grazing", integral_degenerate_and_grazing},
    {"material", "no_lights_matches_ambient", material_no_lights_matches_ambient},
    {"material", "shading_formula", material_shading_formula},
    {"material", "real_draw_normals_metal_roughness", material_real_draw_normals_metal_roughness},
    {"material", "draw_hdr_addition_reset", material_draw_lights_hdr_addition_and_reset},
    {"material", "visible_light_geometry", material_visible_light_geometry},
    {"material", "default_target_quantization", material_default_target_quantization}
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
