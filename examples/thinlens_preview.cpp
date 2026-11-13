/*
 * Thin lens depth-of-field preview.
 *
 * Builds a small procedural room (no external models) containing two small
 * spheres, one in front of and one behind the focus plane, and renders the
 * same scene with a pinhole, a small aperture and a wide apodized aperture.
 * The three images are written as PFM files for direct comparison, together
 * with their total luminance, which stays constant up to Monte Carlo noise
 * because the pupil uses a normalized area average.
 *
 * Build (NO_OMP):
 *   bash support/build.sh examples/thinlens_preview.cpp -o thinlens_preview
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "math.hxx"
#include "ray.hxx"
#include "geometry.hxx"
#include "camera.hxx"
#include "framebuffer.hxx"
#include "scene.hxx"
#include "materials.hxx"
#include "lights.hxx"
#include "vertexcm.hxx"

namespace
{

// A compact room: two small spheres flank the focus plane (at z = 4),
// the red one in front (z = 3) and the blue one behind it (z = 5).
void BuildPreviewScene(Scene &scene)
{
    Material lightMat;  // 0: ceiling light, emits through the AreaLight
    scene.mMaterials.push_back(lightMat);

    Material wallMat;   // 1: matte white walls
    wallMat.mDiffuseReflectance = Vec3f(0.75f);
    scene.mMaterials.push_back(wallMat);

    Material redMat;    // 2: sphere in front of the focus plane
    redMat.mDiffuseReflectance = Vec3f(0.75f, 0.12f, 0.12f);
    scene.mMaterials.push_back(redMat);

    Material blueMat;   // 3: sphere behind the focus plane
    blueMat.mDiffuseReflectance = Vec3f(0.12f, 0.25f, 0.75f);
    scene.mMaterials.push_back(blueMat);

    GeometryList *geom = new GeometryList;

    const float x0 = -1.6f, x1 = 1.6f;
    const float y0 = -1.3f, y1 = 1.3f;
    const float z0 =  0.2f, z1 = 7.0f;

    // Floor and ceiling (normals oriented into the room).
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x0, y0, z0), Vec3f(x1, y0, z1), Vec3f(x1, y0, z0), 1));
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x0, y0, z0), Vec3f(x0, y0, z1), Vec3f(x1, y0, z1), 1));
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x0, y1, z0), Vec3f(x1, y1, z0), Vec3f(x1, y1, z1), 1));
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x0, y1, z0), Vec3f(x1, y1, z1), Vec3f(x0, y1, z1), 1));

    // Side and back walls.
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x0, y0, z0), Vec3f(x0, y1, z0), Vec3f(x0, y1, z1), 1));
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x0, y0, z0), Vec3f(x0, y1, z1), Vec3f(x0, y0, z1), 1));
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x1, y0, z0), Vec3f(x1, y0, z1), Vec3f(x1, y1, z1), 1));
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x1, y0, z0), Vec3f(x1, y1, z1), Vec3f(x1, y1, z0), 1));
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x0, y0, z1), Vec3f(x1, y0, z1), Vec3f(x1, y1, z1), 1));
    geom->mGeometry.push_back(new Triangle(
        Vec3f(x0, y0, z1), Vec3f(x1, y1, z1), Vec3f(x0, y1, z1), 1));

    // Small objects on either side of the focus plane (focus distance 4).
    geom->mGeometry.push_back(new Sphere(Vec3f(-0.45f, -0.65f, 3.0f), 0.35f, 2));
    geom->mGeometry.push_back(new Sphere(Vec3f( 0.45f, -0.65f, 5.0f), 0.35f, 3));

    // Ceiling area light.
    const Vec3f l0(-0.35f, y1 - 1e-4f, 2.6f);
    const Vec3f l1( 0.35f, y1 - 1e-4f, 2.6f);
    const Vec3f l2(-0.35f, y1 - 1e-4f, 3.4f);
    geom->mGeometry.push_back(new Triangle(l0, l1, l2, 0));
    AreaLight *light = new AreaLight(l0, l1, l2);
    light->mIntensity = Vec3f(18.f);
    scene.mLights.resize(1);
    scene.mLights[0] = light;
    scene.mMaterial2Light[0] = 0;

    scene.mGeometry = geom;
    scene.BuildSceneSphere();

    scene.mCamera.Setup(
        Vec3f(0.f, -0.15f, 0.f),
        Vec3f(0.f, 0.f, 1.f),
        Vec3f(0.f, 1.f, 0.f),
        Vec2f(64.f, 64.f), 45.f);
}

struct Variant
{
    const char *name;
    float radius;
    float apodization;
};

} // namespace

int main(int argc, char **argv)
{
    const std::string prefix = (argc > 1) ? std::string(argv[1]) : std::string("thinlens");
    const int iterations = 24;
    const float focusDistance = 4.f;

    const Variant variants[] = {
        {"pinhole", 0.0f,  0.0f},
        {"small",   0.08f, 0.5f},
        {"wide",    0.28f, 0.5f},
    };

    for(size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); ++v)
    {
        Scene scene;
        BuildPreviewScene(scene);
        if(variants[v].radius > 0.f)
        {
            if(!scene.mCamera.SetLens(variants[v].radius,
                   focusDistance, variants[v].apodization))
            {
                std::fprintf(stderr, "lens configuration rejected: %s\n",
                    variants[v].name);
                return 1;
            }
        }

        VertexCM renderer(scene, VertexCM::kBpt, 0.003f, 0.75f, 2024);
        renderer.mMaxPathLength = 6;
        for(int i = 0; i < iterations; ++i)
            renderer.RunIteration(i);

        Framebuffer fb;
        renderer.GetFramebuffer(fb);

        const std::string filename = prefix + "_" + variants[v].name + ".pfm";
        fb.SavePFM(filename.c_str());
        std::printf("%-8s R=%-4g focus=%g  luminance=%.6f  -> %s\n",
            variants[v].name, variants[v].radius, focusDistance,
            fb.TotalLuminance(), filename.c_str());
    }

    return 0;
}
