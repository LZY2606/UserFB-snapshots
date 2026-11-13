/*
 * Thin lens transport contract tests.
 *
 * Four registered cases: camera, density, transport, compatibility.
 * The tests drive the real Camera / PathTracer / VertexCM code paths; the
 * ThinLensTestAccess friend adapter only exposes intermediates of the actual
 * integrator, it never reimplements one.
 */

#include <cmath>
#include <vector>
#include <string>

#include "check.h"
#include "camera.hxx"
#include "scene.hxx"
#include "framebuffer.hxx"
#include "pathtracer.hxx"
#include "vertexcm.hxx"
#include "rng.hxx"

// Friend adapter: reads the real VertexCM / Framebuffer internals. Declared
// at global scope so it matches the friend declarations in the headers.
class ThinLensTestAccess
{
public:

    typedef VertexCM::SubPathState SubPathState;

    static void SetPathCounts(
        VertexCM &aVcm,
        float aLightSubPathCount,
        float aMergeRadius)
    {
        aVcm.mLightSubPathCount = aLightSubPathCount;
        const float eta = PI_F * Sqr(aMergeRadius) * aLightSubPathCount;
        aVcm.mMisVmWeightFactor = aVcm.mUseVM ? aVcm.Mis(eta) : 0.f;
        aVcm.mMisVcWeightFactor = aVcm.mUseVC ? aVcm.Mis(1.f / eta) : 0.f;
    }

    static Vec2f GenerateCameraSample(
        VertexCM &aVcm,
        int aPixelIndex,
        SubPathState &oState,
        CameraRaySample &oSample)
    {
        return aVcm.GenerateCameraSample(aPixelIndex, oState, &oSample);
    }

    static void ConnectToCamera(
        VertexCM &aVcm,
        const VertexCM::SubPathState &aLightState,
        const Vec3f &aHitpoint,
        const BSDF<true> &aBsdf,
        CameraConnection &oCam,
        float &oImageToSurface,
        float &oWLight,
        float &oMisWeight,
        Vec3f &oContrib,
        bool &oOccluded,
        bool &oConnected)
    {
        aVcm.ConnectToCamera(aLightState, aHitpoint, aBsdf,
            &oCam, &oImageToSurface, &oWLight, &oMisWeight,
            &oContrib, &oOccluded, &oConnected);
    }

    static float NextFloat(VertexCM &aVcm)
    {
        return aVcm.mRng.GetFloat();
    }

    static float MisVmFactor(const VertexCM &aVcm)
    {
        return aVcm.mMisVmWeightFactor;
    }

    static Vec3f FbPixel(const Framebuffer &aFb, int x, int y)
    {
        return aFb.mColor[x + y * aFb.mResX];
    }

    static Vec3f ReadPixel(const VertexCM &aVcm, int x, int y)
    {
        return FbPixel(aVcm.mFramebuffer, x, y);
    }
};

namespace
{

bool Near(float a, float b, float tol = 1e-4f)
{
    return std::fabs(a - b) <= tol * std::max(1.f, std::fabs(b));
}

bool NearV(const Vec3f &a, const Vec3f &b, float tol = 1e-4f)
{
    return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol);
}

bool NearV2(const Vec2f &a, const Vec2f &b, float tol = 1e-4f)
{
    return Near(a.x, b.x, tol) && Near(a.y, b.y, tol);
}

typedef ThinLensTestAccess Access;

////////////////////////////////////////////////////////////////////////////////
// Geometry used by the transport test: a diffuse wall that records every
// occlusion query it receives.
////////////////////////////////////////////////////////////////////////////////

struct RecordingGeometry : public AbstractGeometry
{
    RecordingGeometry() : mBlock(false) {}

    struct Record
    {
        Vec3f org;
        Vec3f dir;
        float tmax;
    };

    virtual bool Intersect(const Ray &aRay, Isect &oResult) const
    {
        return mWall.Intersect(aRay, oResult);
    }

    virtual bool IntersectP(const Ray &aRay, Isect &oResult) const
    {
        Record rec;
        rec.org  = aRay.org;
        rec.dir  = aRay.dir;
        rec.tmax = oResult.dist;
        mRecords.push_back(rec);
        return mBlock;
    }

    virtual void GrowBBox(Vec3f &aoMin, Vec3f &aoMax)
    {
        mWall.GrowBBox(aoMin, aoMax);
    }

    Triangle mWall;
    bool mBlock;
    mutable std::vector<Record> mRecords;
};

void MakeProbeScene(Scene &oScene, RecordingGeometry *&oGeom, Camera *&oCam)
{
    Material mat;
    mat.mDiffuseReflectance = Vec3f(0.8f);
    oScene.mMaterials.push_back(mat);

    // Wall at z = 4, normal towards the camera (-z), so both a camera ray and
    // an incoming light sub-path see its front face.
    RecordingGeometry *geom = new RecordingGeometry;
    geom->mWall = Triangle(
        Vec3f(-2.f, -2.f, 4.f),
        Vec3f(-2.f,  2.f, 4.f),
        Vec3f( 2.f, -2.f, 4.f), 0);
    oScene.mGeometry = geom;
    oScene.BuildSceneSphere();

    oScene.mCamera.Setup(
        Vec3f(0.f, 0.f, 0.f),
        Vec3f(0.f, 0.f, 1.f),
        Vec3f(0.f, 1.f, 0.f),
        Vec2f(16.f, 16.f), 45.f);

    oGeom = geom;
    oCam  = &oScene.mCamera;
}

////////////////////////////////////////////////////////////////////////////////
// camera
////////////////////////////////////////////////////////////////////////////////

void TestCamera()
{
    Camera cam;
    cam.Setup(Vec3f(0.f, 0.f, 0.f), Vec3f(0.f, 0.f, 1.f),
        Vec3f(0.f, 1.f, 0.f), Vec2f(16.f, 16.f), 45.f);

    // Setup defaults to a pinhole.
    UNIT_CHECK(!cam.HasLens());

    const Vec2f raster(7.3f, 9.1f);
    const Ray pinhole = cam.GenerateRay(raster);
    const CameraRaySample ps = cam.SampleRay(raster, Vec2f(0.7f, 0.2f));

    // Pinhole SampleRay is component-wise identical to GenerateRay.
    UNIT_CHECK(ps.ray.org.x == pinhole.org.x && ps.ray.org.y == pinhole.org.y &&
               ps.ray.org.z == pinhole.org.z);
    UNIT_CHECK(ps.ray.dir.x == pinhole.dir.x && ps.ray.dir.y == pinhole.dir.y &&
               ps.ray.dir.z == pinhole.dir.z);
    UNIT_CHECK(ps.ray.tmin == pinhole.tmin);
    UNIT_CHECK(ps.pdfLensA == 1.f && ps.response == 1.f);

    // Pinhole pdfW matches the legacy image-plane factor.
    const float cosF = Dot(cam.mForward, pinhole.dir);
    const float legacyPdfW =
        Sqr(cam.mImagePlaneDist / cosF) / cosF;
    UNIT_CHECK(Near(ps.pdfW, legacyPdfW));

    // Validation: non-finite arguments rejected.
    UNIT_CHECK(!cam.SetLens(std::sqrt(-1.f), 3.f, 0.f));
    UNIT_CHECK(!cam.SetLens(0.2f, 1.f / 0.f, 0.f));
    UNIT_CHECK(!cam.SetLens(0.2f, 3.f, std::sqrt(-1.f)));

    // Range checks.
    UNIT_CHECK(!cam.SetLens(-0.1f, 3.f, 0.f));
    UNIT_CHECK(!cam.SetLens(0.2f, 0.f, 0.f));
    UNIT_CHECK(!cam.SetLens(0.2f, -1.f, 0.f));
    UNIT_CHECK(!cam.SetLens(0.2f, 3.f, -0.01f));
    UNIT_CHECK(!cam.SetLens(0.2f, 3.f, 1.01f));

    // Non-square resolution rejects a finite-area pupil.
    Camera wide;
    wide.Setup(Vec3f(0.f), Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 1.f, 0.f),
        Vec2f(16.f, 8.f), 45.f);
    UNIT_CHECK(!wide.SetLens(0.2f, 3.f, 0.5f));
    UNIT_CHECK(!wide.HasLens());
    // A zero radius stays valid on a non-square resolution.
    UNIT_CHECK(wide.SetLens(0.f, 3.f, 0.5f));
    UNIT_CHECK(!wide.HasLens());

    // Overflowing normalization constant is rejected as a non-finite density.
    UNIT_CHECK(!cam.SetLens(1e30f, 3.f, 1.f));
    UNIT_CHECK(!cam.HasLens());

    // Valid lens.
    const float R = 0.3f, focus = 4.f, k = 0.6f;
    UNIT_CHECK(cam.SetLens(R, focus, k));
    UNIT_CHECK(cam.HasLens());

    // Azimuth basis follows the raster axes: u.y = 0 sits on raster +x and a
    // quarter turn reaches raster +y (verified against RasterToWorld itself).
    {
        Camera edge;
        edge.Setup(Vec3f(0.f), Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 1.f, 0.f),
            Vec2f(10.f, 10.f), 45.f);
        UNIT_CHECK(edge.SetLens(0.2f, 3.f, 0.4f));

        const Vec3f rasterAxisX =
            Normalize(edge.RasterToWorld(Vec2f(6.f, 5.f)) -
                      edge.RasterToWorld(Vec2f(4.f, 5.f)));
        const Vec3f rasterAxisY =
            Normalize(edge.RasterToWorld(Vec2f(5.f, 6.f)) -
                      edge.RasterToWorld(Vec2f(5.f, 4.f)));

        const CameraRaySample ax = edge.SampleRay(Vec2f(5.f, 5.f), Vec2f(0.5f, 0.f));
        const CameraRaySample ay = edge.SampleRay(Vec2f(5.f, 5.f), Vec2f(0.5f, 0.25f));
        const Vec3f lensDirX = Normalize(ax.ray.org - edge.mPosition);
        const Vec3f lensDirY = Normalize(ay.ray.org - edge.mPosition);

        // phi = 0 aligns with raster +x; a quarter turn with raster +y.
        UNIT_CHECK(NearV(lensDirX, rasterAxisX, 1e-5f));
        UNIT_CHECK(NearV(lensDirY, rasterAxisY, 1e-5f));
        // Equal radial parameter means equal radius.
        UNIT_CHECK(Near((ax.ray.org - edge.mPosition).Length(),
                        (ay.ray.org - edge.mPosition).Length(), 1e-6f));
    }

    // Failed calls preserve the previous configuration.
    UNIT_CHECK(!cam.SetLens(0.2f, -1.f, 0.5f));
    UNIT_CHECK(cam.HasLens());

    // Forward sample geometry: lens point on the disk and ray through the
    // pinhole focus-plane intersection.
    const CameraRaySample ls = cam.SampleRay(raster, Vec2f(0.71f, 0.23f));
    const Vec3f lensOffset = ls.ray.org - cam.mPosition;
    UNIT_CHECK(Near(Dot(lensOffset, cam.mForward), 0.f, 1e-5f));
    UNIT_CHECK(lensOffset.LenSqr() <= Sqr(R) + 1e-6f);
    UNIT_CHECK(ls.ray.org.LenSqr() > 0.f); // off-center sample

    const Vec3f pinholeFocus = cam.mPosition + pinhole.dir *
        (focus / Dot(cam.mForward, pinhole.dir));
    const float tFocus = focus / Dot(cam.mForward, ls.ray.dir);
    const Vec3f lensFocus = ls.ray.org + ls.ray.dir * tFocus;
    UNIT_CHECK(NearV(lensFocus, pinholeFocus, 1e-4f));

    // Forward density / response bookkeeping.
    const float t = lensOffset.LenSqr() / Sqr(R);
    const float mean = 1.f - 0.5f * k;
    const float transmission = 1.f - k * t;
    UNIT_CHECK(Near(ls.response, transmission / mean));
    UNIT_CHECK(Near(ls.pdfLensA, transmission / (PI_F * Sqr(R) * mean)));
    UNIT_CHECK(Near(ls.pdfLensA, ls.response / (PI_F * Sqr(R))));
    UNIT_CHECK(Near(ls.pdfW,
        Sqr(cam.mImagePlaneDist / Dot(cam.mForward, ls.ray.dir)) /
        Dot(cam.mForward, ls.ray.dir)));

    // Failed reverse connections leave the output untouched.
    CameraConnection conn;
    conn.raster = Vec2f(-123.f, -456.f);
    conn.pdfLensA = -7.f;
    const CameraConnection sentinel = conn;

    // Point behind the camera.
    const Vec3f behind(0.f, 0.f, -1.f);
    UNIT_CHECK(!cam.SampleConnection(behind, Vec2f(0.5f, 0.5f), conn));
    UNIT_CHECK(conn.raster.x == sentinel.raster.x &&
               conn.raster.y == sentinel.raster.y &&
               conn.pdfLensA == sentinel.pdfLensA);

    // Point in front but projecting outside the raster.
    const Vec3f offscreen(50.f, 50.f, 2.f);
    UNIT_CHECK(!cam.SampleConnection(offscreen, Vec2f(0.5f, 0.5f), conn));
    UNIT_CHECK(conn.raster.x == sentinel.raster.x &&
               conn.pdfLensA == sentinel.pdfLensA);

    // Valid reverse connection: uniform lens density, correct geometry.
    const Vec3f point(0.2f, -0.1f, 2.5f);
    CameraConnection rc;
    UNIT_CHECK(cam.SampleConnection(point, Vec2f(0.64f, 0.31f), rc));
    UNIT_CHECK(Near(rc.pdfLensA, 1.f / (PI_F * Sqr(R))));
    UNIT_CHECK(Near(rc.distance, (rc.lensPoint - point).Length()));
    UNIT_CHECK(NearV(rc.directionToCamera,
        Normalize(rc.lensPoint - point)));
    UNIT_CHECK(Dot(cam.mForward, -rc.directionToCamera) > 0.f);
    UNIT_CHECK(cam.CheckRaster(rc.raster));

    // Setup resets the lens back to pinhole.
    cam.Setup(Vec3f(0.f), Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 1.f, 0.f),
        Vec2f(16.f, 16.f), 45.f);
    UNIT_CHECK(!cam.HasLens());
}

////////////////////////////////////////////////////////////////////////////////
// density
////////////////////////////////////////////////////////////////////////////////

void TestDensity()
{
    Camera cam;
    cam.Setup(Vec3f(1.f, 2.f, 3.f), Normalize(Vec3f(0.2f, 0.1f, 1.f)),
        Vec3f(0.f, 1.f, 0.f), Vec2f(24.f, 24.f), 50.f);

    const float R = 0.4f, focus = 5.f, k = 0.75f;
    UNIT_CHECK(cam.SetLens(R, focus, k));
    const float mean = 1.f - 0.5f * k;
    const Vec2f raster(12.f, 12.f);

    // Empirical radial CDF of the forward strategy versus the analytic
    // marginal CDF F(t) = (t - k t^2 / 2) / M, and response averages.
    const int N = 20000;
    Rng rng(98765);
    double sumResponseReverse = 0.0;
    double sumResponseForward = 0.0;
    double sumPdfCheck = 0.0;
    const int bins = 10;
    long below[10] = {0};

    for(int i = 0; i < N; ++i)
    {
        const Vec2f u = rng.GetVec2f();
        const CameraRaySample s = cam.SampleRay(raster, u);

        const Vec3f off = s.ray.org - cam.mPosition;
        const float t = off.LenSqr() / Sqr(R);
        UNIT_CHECK(t >= 0.f && t < 1.f + 1e-5f);
        for(int b = 0; b < bins; ++b)
            if(t <= float(b + 1) / float(bins))
                ++below[b];

        sumResponseForward += s.response;
        sumPdfCheck += s.pdfLensA * PI_F * Sqr(R) / s.response; // always 1

        // Uniform reverse strategy on the same pupil.
        CameraConnection c;
        const Vec3f point = cam.mPosition + cam.mForward * 3.f;
        UNIT_CHECK(cam.SampleConnection(point, u, c));
        sumResponseReverse += c.response;
        UNIT_CHECK(Near(c.pdfLensA, 1.f / (PI_F * Sqr(R))));
    }

    for(int b = 0; b < bins; ++b)
    {
        const float q = float(b + 1) / float(bins);
        const float analytic = (q - 0.5f * k * Sqr(q)) / mean;
        const float empirical = float(below[b]) / float(N);
        UNIT_CHECK(Near(empirical, analytic, 0.03f));
    }

    // E_rev[T/M] = 1: opening the aperture does not change exposure.
    UNIT_CHECK(Near(float(sumResponseReverse / N), 1.f, 0.02f));
    UNIT_CHECK(Near(float(sumPdfCheck / N), 1.f, 1e-5f));

    // E_fwd[response] = integral (1-kt)^2 / M^2 dt = (1 - k + k^2/3) / M^2.
    const float analyticFwdMean =
        (1.f - k + Sqr(k) / 3.f) / Sqr(mean);
    UNIT_CHECK(Near(float(sumResponseForward / N), analyticFwdMean, 0.02f));

    // Solid angle Jacobian by finite differences in raster space. The measured
    // quantity is d omega / dA_raster = cos^3 / d^2, i.e. the reciprocal of
    // the solid-angle density pdfW = d^2 / cos^3.
    const Vec2f lensU(0.37f, 0.63f);
    const CameraRaySample c0 = cam.SampleRay(raster, lensU);
    const float h = 1e-2f;
    const CameraRaySample cx = cam.SampleRay(raster + Vec2f(h, 0.f), lensU);
    const CameraRaySample cy = cam.SampleRay(raster + Vec2f(0.f, h), lensU);
    // Same lens point for all three rays.
    UNIT_CHECK(NearV(c0.ray.org, cx.ray.org, 1e-6f));
    UNIT_CHECK(NearV(c0.ray.org, cy.ray.org, 1e-6f));
    const Vec3f dWdx = (cx.ray.dir - c0.ray.dir) / h;
    const Vec3f dWdy = (cy.ray.dir - c0.ray.dir) / h;
    const float jacobian = Cross(dWdx, dWdy).Length();
    UNIT_CHECK(Near(jacobian, 1.f / c0.pdfW, 0.02f));
    const float cosFd = Dot(cam.mForward, c0.ray.dir);
    UNIT_CHECK(Near(jacobian,
        cosFd * cosFd * cosFd / Sqr(cam.mImagePlaneDist), 0.02f));

    // Same check away from the image center.
    const Vec2f raster2(4.f, 18.f);
    const CameraRaySample o0 = cam.SampleRay(raster2, lensU);
    const CameraRaySample ox = cam.SampleRay(raster2 + Vec2f(h, 0.f), lensU);
    const CameraRaySample oy = cam.SampleRay(raster2 + Vec2f(0.f, h), lensU);
    const Vec3f odx = (ox.ray.dir - o0.ray.dir) / h;
    const Vec3f ody = (oy.ray.dir - o0.ray.dir) / h;
    UNIT_CHECK(Near(Cross(odx, ody).Length(), 1.f / o0.pdfW, 0.02f));

    // Forward/reverse round trip through an independent focus-plane hit.
    const Vec2f rr(5.5f, 17.2f);
    const Vec2f uf(0.42f, 0.18f);
    const CameraRaySample fs = cam.SampleRay(rr, uf);
    const Vec3f fLens = fs.ray.org;
    const float tf = focus / Dot(cam.mForward, fs.ray.dir);
    const Vec3f fFocus = fLens + fs.ray.dir * tf;

    // Independent pinhole focus intersection.
    const Ray pr = cam.GenerateRay(rr);
    const Vec3f fPin = cam.mPosition + pr.dir *
        (focus / Dot(cam.mForward, pr.dir));
    UNIT_CHECK(NearV(fFocus, fPin, 1e-4f));

    // Scene point further along the same lens ray.
    const Vec3f scenePoint = fLens + fs.ray.dir * 2.5f;
    const float tParam = (fLens - cam.mPosition).LenSqr() / Sqr(R);
    const CameraRaySample fs2 = cam.SampleRay(rr, uf);
    UNIT_CHECK(NearV(fs2.ray.org, fLens));
    CameraConnection back;
    UNIT_CHECK(cam.SampleConnection(scenePoint, Vec2f(tParam, uf.y), back));
    UNIT_CHECK(NearV(back.lensPoint, fLens, 1e-5f));
    UNIT_CHECK(NearV2(back.raster, rr, 2e-3f));
    UNIT_CHECK(Near(back.pdfW, fs.pdfW, 1e-4f));

    // k = 0: both strategies coincide, response 1 and no pi R^2 residue.
    UNIT_CHECK(cam.SetLens(R, focus, 0.f));
    const CameraRaySample k0 = cam.SampleRay(raster, Vec2f(0.3f, 0.8f));
    UNIT_CHECK(Near(k0.response, 1.f));
    UNIT_CHECK(Near(k0.pdfLensA, 1.f / (PI_F * Sqr(R))));
}

////////////////////////////////////////////////////////////////////////////////
// transport
////////////////////////////////////////////////////////////////////////////////

float TotalLuminance(const Framebuffer &aFb)
{
    Framebuffer fb = aFb;
    return fb.TotalLuminance();
}

template<typename TRenderer>
Framebuffer RunRenderer(TRenderer &aRenderer, int aIterations)
{
    aRenderer.mMaxPathLength = 6;
    for(int i = 0; i < aIterations; ++i)
        aRenderer.RunIteration(i);
    Framebuffer fb;
    aRenderer.GetFramebuffer(fb);
    return fb;
}

void TestTransport()
{
    // ----- Adapter level: real dVCM root and real connection intermediates.
    Scene scene;
    RecordingGeometry *geom = NULL;
    Camera *cam = NULL;
    MakeProbeScene(scene, geom, cam);
    const float R = 0.5f, focus = 4.f, k = 0.6f;
    UNIT_CHECK(cam->SetLens(R, focus, k));
    const float mean = 1.f - 0.5f * k;

    VertexCM bpt(scene, VertexCM::kBpt, 0.003f, 0.75f, 4242);
    const float pathCount = 256.f;
    Access::SetPathCounts(bpt, pathCount, 0.05f);

    // Real camera root with the reverse-to-forward lens measure ratio.
    Access::SubPathState eyeState;
    CameraRaySample eyeSample;
    Access::GenerateCameraSample(bpt, 100, eyeState, eyeSample);
    UNIT_CHECK(eyeState.mThroughput.IsZero() == false);
    UNIT_CHECK(eyeState.mThroughput.x == 1.f &&
               eyeState.mThroughput.y == 1.f &&
               eyeState.mThroughput.z == 1.f);
    const float expectedRoot =
        pathCount / eyeSample.pdfW / eyeSample.response;
    UNIT_CHECK(Near(eyeState.dVCM, expectedRoot, 1e-5f));
    UNIT_CHECK((eyeSample.ray.org - cam->mPosition).LenSqr() > 0.f);

    // Real light-path connection.
    const Vec3f hitpoint(0.25f, -0.15f, 4.f);
    const Vec3f lightPos(-1.f, 1.f, 2.f);
    Ray lightRay(lightPos, Normalize(hitpoint - lightPos), 0.f);
    Isect isect(1e36f);
    isect.dist = (hitpoint - lightPos).Length();
    isect.matID = 0;
    isect.lightID = -1;
    isect.normal = Vec3f(0.f, 0.f, -1.f);
    BSDF<true> bsdf(lightRay, isect, scene);
    UNIT_CHECK(bsdf.IsValid());

    Access::SubPathState lightState;
    lightState.mOrigin = hitpoint;
    lightState.mDirection = lightRay.dir;
    lightState.mThroughput = Vec3f(1.f);
    lightState.mPathLength = 2;
    lightState.mIsFiniteLight = 1;
    lightState.mSpecularPath = 0;
    lightState.dVCM = 0.37f;
    lightState.dVC  = 0.11f;
    lightState.dVM  = 0.05f;

    // Retry until the lens draw is clearly off-center.
    CameraConnection conn;
    float i2s, wLight, misWeight;
    Vec3f contrib;
    bool occluded = false, connected = false;
    int tries = 0;
    while(tries < 100)
    {
        Access::ConnectToCamera(bpt, lightState, hitpoint, bsdf,
            conn, i2s, wLight, misWeight, contrib, occluded, connected);
        ++tries;
        UNIT_CHECK(connected);
        if((conn.lensPoint - cam->mPosition).LenSqr() >= 0.25f * Sqr(R))
            break;
    }
    UNIT_CHECK(tries < 100);

    UNIT_CHECK(connected && !occluded);
    UNIT_CHECK(Near(conn.pdfLensA, 1.f / (PI_F * Sqr(R))));
    const float t = (conn.lensPoint - cam->mPosition).LenSqr() / Sqr(R);
    UNIT_CHECK(Near(conn.response, (1.f - k * t) / mean));

    // Independent competition density in the reverse measure.
    float cosToCamera, bsdfDirPdfW, bsdfRevPdfW;
    const Vec3f bsdfFactor = bsdf.Evaluate(scene,
        conn.directionToCamera, cosToCamera, &bsdfDirPdfW, &bsdfRevPdfW);
    bsdfRevPdfW *= bsdf.ContinuationProb();
    const float expI2s =
        conn.pdfW * std::fabs(cosToCamera) / Sqr(conn.distance);
    UNIT_CHECK(Near(i2s, expI2s, 1e-5f));

    const float expWLight =
        (conn.response * i2s / pathCount) *
        (Access::MisVmFactor(bpt) + lightState.dVCM +
         lightState.dVC * bsdfRevPdfW);
    UNIT_CHECK(Near(wLight, expWLight, 1e-4f));
    UNIT_CHECK(Near(misWeight, 1.f / (wLight + 1.f), 1e-5f));
    UNIT_CHECK(NearV(contrib,
        misWeight * conn.response * lightState.mThroughput *
        bsdfFactor * (i2s / pathCount), 1e-5f));

    // The occlusion ray terminates at the sampled lens point.
    UNIT_CHECK(geom->mRecords.size() >= 1);
    const RecordingGeometry::Record &rec = geom->mRecords.back();
    UNIT_CHECK(NearV(rec.dir, conn.directionToCamera, 1e-5f));
    const Vec3f rayEnd = rec.org + rec.dir * rec.tmax;
    UNIT_CHECK(NearV(rayEnd, conn.lensPoint, 5.f * EPS_RAY));
    UNIT_CHECK((conn.lensPoint - cam->mPosition).LenSqr() > 1e-6f);

    // Splatted framebuffer pixel matches the returned contribution.
    const int px = int(conn.raster.x);
    const int py = int(conn.raster.y);
    UNIT_CHECK(NearV(Access::ReadPixel(bpt, px, py), contrib, 1e-5f));

    // Blocking geometry: connection reports occlusion and nothing is added.
    geom->mBlock = true;
    const Vec3f pixelBefore = Access::ReadPixel(bpt, px, py);
    CameraConnection blocked;
    bool blockedOccluded = false, blockedConnected = true;
    Access::ConnectToCamera(bpt, lightState, hitpoint, bsdf,
        blocked, i2s, wLight, misWeight, contrib,
        blockedOccluded, blockedConnected);
    UNIT_CHECK(blockedOccluded && !blockedConnected);
    UNIT_CHECK(NearV(Access::ReadPixel(bpt, px, py), pixelBefore, 0.f));

    // ----- Full small-scene renders through all four integrators.
    Scene cornell;
    cornell.LoadCornellBox(Vec2i(16, 16),
        Scene::kLightCeiling | Scene::kBothSmallSpheres);
    cornell.BuildSceneSphere();

    const int iterations = 32;

    // Pinhole reference (also the zero-radius regression configuration).
    Framebuffer refPt, refLt, refBpt, refVcm;
    {
        PathTracer pt(cornell, 1234);
        refPt = RunRenderer(pt, iterations);
        VertexCM lt(cornell, VertexCM::kLightTrace, .003f, .75f, 1234);
        refLt = RunRenderer(lt, iterations);
        VertexCM bp(cornell, VertexCM::kBpt, .003f, .75f, 1234);
        refBpt = RunRenderer(bp, iterations);
        VertexCM vc(cornell, VertexCM::kVcm, .003f, .75f, 1234);
        refVcm = RunRenderer(vc, iterations);
    }
    UNIT_CHECK(TotalLuminance(refPt) > 0.f &&
               TotalLuminance(refLt) > 0.f &&
               TotalLuminance(refBpt) > 0.f &&
               TotalLuminance(refVcm) > 0.f);

    // R = 0 through the new entrance must reproduce the pinhole render.
    cornell.mCamera.SetLens(0.f, 4.f, 0.6f);
    {
        PathTracer pt(cornell, 1234);
        Framebuffer fb = RunRenderer(pt, iterations);
        UNIT_CHECK(Near(TotalLuminance(fb), TotalLuminance(refPt), 1e-6f));
        VertexCM bp(cornell, VertexCM::kBpt, .003f, .75f, 1234);
        fb = RunRenderer(bp, iterations);
        UNIT_CHECK(Near(TotalLuminance(fb), TotalLuminance(refBpt), 1e-6f));
    }

    // Apodized finite aperture, focus between the camera and the back wall.
    cornell.mCamera.SetLens(0.15f, 4.f, 0.6f);
    Framebuffer lensPt, lensLt, lensBpt, lensVcm;
    {
        PathTracer pt(cornell, 1234);
        lensPt = RunRenderer(pt, iterations);
        VertexCM lt(cornell, VertexCM::kLightTrace, .003f, .75f, 1234);
        lensLt = RunRenderer(lt, iterations);
        VertexCM bp(cornell, VertexCM::kBpt, .003f, .75f, 1234);
        lensBpt = RunRenderer(bp, iterations);
        VertexCM vc(cornell, VertexCM::kVcm, .003f, .75f, 1234);
        lensVcm = RunRenderer(vc, iterations);
    }

    const Framebuffer *refs[4] = {&refPt, &refLt, &refBpt, &refVcm};
    const Framebuffer *lens[4] = {&lensPt, &lensLt, &lensBpt, &lensVcm};
    for(int a = 0; a < 4; ++a)
    {
        const float t0 = TotalLuminance(*refs[a]);
        const float t1 = TotalLuminance(*lens[a]);
        UNIT_CHECK(std::isfinite(t1) && t1 > 0.f);
        // Normalized pupil: exposure stays within Monte Carlo noise.
        UNIT_CHECK(std::fabs(t1 - t0) / t0 < 0.12f);
    }

    // Defocus: a finite aperture visibly changes the image.
    int changed = 0;
    float maxDiff = 0.f;
    for(int y = 0; y < 16; ++y)
        for(int x = 0; x < 16; ++x)
        {
            const Vec3f d = Access::FbPixel(lensBpt, x, y) -
                            Access::FbPixel(refBpt, x, y);
            const float mag = std::fabs(d.x) + std::fabs(d.y) + std::fabs(d.z);
            maxDiff = std::max(maxDiff, mag);
            if(mag > 0.06f)
                ++changed;
        }
    UNIT_CHECK(changed > 8 && maxDiff > 0.12f);

    // A different aperture radius produces a different image again.
    cornell.mCamera.SetLens(0.05f, 4.f, 0.6f);
    {
        VertexCM bp(cornell, VertexCM::kBpt, .003f, .75f, 1234);
        Framebuffer small = RunRenderer(bp, iterations);
        int different = 0;
        for(int y = 0; y < 16; ++y)
            for(int x = 0; x < 16; ++x)
            {
                const Vec3f d = Access::FbPixel(small, x, y) -
                                Access::FbPixel(lensBpt, x, y);
                if(d.LenSqr() > 1e-5f)
                    ++different;
            }
        UNIT_CHECK(different > 4);
    }
}

////////////////////////////////////////////////////////////////////////////////
// compatibility
////////////////////////////////////////////////////////////////////////////////

void TestCompatibility()
{
    // RNG dimensions: a pinhole camera path consumes only the pixel jitter
    // pair, the lens adds exactly one pair and nothing more.
    Scene scene;
    scene.LoadCornellBox(Vec2i(8, 8), Scene::kLightCeiling);
    scene.BuildSceneSphere();

    VertexCM pinhole(scene, VertexCM::kBpt, .003f, .75f, 777);
    Access::SetPathCounts(pinhole, 64.f, 0.05f);
    Access::SubPathState phState;
    CameraRaySample phSample;
    Access::GenerateCameraSample(pinhole, 5, phState, phSample);

    Rng expectedPinhole(777);
    expectedPinhole.GetVec2f(); // pixel jitter only
    UNIT_CHECK(Near(Access::NextFloat(pinhole), expectedPinhole.GetFloat()));

    UNIT_CHECK(scene.mCamera.SetLens(0.2f, 3.f, 0.5f));
    VertexCM lens(scene, VertexCM::kBpt, .003f, .75f, 777);
    Access::SetPathCounts(lens, 64.f, 0.05f);
    Access::SubPathState lensState;
    CameraRaySample lensSample;
    Access::GenerateCameraSample(lens, 5, lensState, lensSample);

    Rng expectedLens(777);
    expectedLens.GetVec2f(); // pixel jitter
    expectedLens.GetVec2f(); // one lens pair
    UNIT_CHECK(Near(Access::NextFloat(lens), expectedLens.GetFloat()));

    // Independent camera states: interleave changed focus, rotation and
    // position configurations without cross-contamination.
    Camera a, b;
    a.Setup(Vec3f(0.f), Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 1.f, 0.f),
        Vec2f(10.f, 10.f), 45.f);
    b.Setup(Vec3f(5.f, -2.f, 1.f), Normalize(Vec3f(1.f, 0.f, 1.f)),
        Vec3f(0.f, 1.f, 0.f), Vec2f(10.f, 10.f), 60.f);
    UNIT_CHECK(a.SetLens(0.1f, 2.f, 0.3f));
    UNIT_CHECK(b.SetLens(0.4f, 8.f, 0.9f));

    const CameraRaySample sa1 = a.SampleRay(Vec2f(5.f, 5.f), Vec2f(0.3f, 0.4f));
    const CameraRaySample sb1 = b.SampleRay(Vec2f(5.f, 5.f), Vec2f(0.3f, 0.4f));
    const CameraRaySample sa2 = a.SampleRay(Vec2f(5.f, 5.f), Vec2f(0.3f, 0.4f));
    UNIT_CHECK(NearV(sa1.ray.org, sa2.ray.org));
    UNIT_CHECK(!NearV(sa1.ray.org, sb1.ray.org));
    UNIT_CHECK(!NearV(sa1.ray.dir, sb1.ray.dir, 1e-3f));

    // Failure on one camera leaves the other one's configuration alone.
    UNIT_CHECK(!b.SetLens(-1.f, 8.f, 0.9f));
    UNIT_CHECK(b.HasLens());
    UNIT_CHECK(a.HasLens());

    // Two scenes rendered interleaved keep independent framebuffers.
    Scene s1, s2;
    s1.LoadCornellBox(Vec2i(8, 8), Scene::kLightCeiling);
    s1.BuildSceneSphere();
    s2.LoadCornellBox(Vec2i(8, 8),
        Scene::kLightCeiling | Scene::kBothSmallSpheres);
    s2.BuildSceneSphere();
    s1.mCamera.SetLens(0.12f, 3.5f, 0.4f);
    UNIT_CHECK(!s2.mCamera.HasLens());

    PathTracer p1(s1, 11);
    PathTracer p2(s2, 11);
    p1.mMaxPathLength = 4;
    p2.mMaxPathLength = 4;
    p1.RunIteration(0);
    p2.RunIteration(0);
    p1.RunIteration(1);
    Framebuffer f1, f2;
    p1.GetFramebuffer(f1);
    p2.GetFramebuffer(f2);
    UNIT_CHECK(std::isfinite(f1.TotalLuminance()) &&
               std::isfinite(f2.TotalLuminance()));
    UNIT_CHECK(s1.mCamera.HasLens() && !s2.mCamera.HasLens());

    // k = 0 leaves no pi R^2 residue in the connection competition weight.
    Camera c;
    c.Setup(Vec3f(0.f), Vec3f(0.f, 0.f, 1.f), Vec3f(0.f, 1.f, 0.f),
        Vec2f(12.f, 12.f), 45.f);
    UNIT_CHECK(c.SetLens(0.25f, 3.f, 0.f));
    CameraConnection cc;
    UNIT_CHECK(c.SampleConnection(Vec3f(0.1f, 0.1f, 2.f),
        Vec2f(0.7f, 0.2f), cc));
    UNIT_CHECK(Near(cc.response, 1.f));
    UNIT_CHECK(Near(cc.pdfLensA, 1.f / (PI_F * Sqr(0.25f))));
}

} // namespace

void register_contract()
{
    unit_add("camera", &TestCamera);
    unit_add("density", &TestDensity);
    unit_add("transport", &TestTransport);
    unit_add("compatibility", &TestCompatibility);
}
