/*
 * Tests for interpolated shading normals on triangle meshes.
 *
 * The test is organized in five sections, each ending with a marker that
 * checks/run-shading-normals.sh greps for:
 *
 *   VCM_NORMAL_INTERPOLATION_OK  barycentric interpolation, alignment,
 *                                degenerate/non-finite fallbacks, Isect reuse
 *   VCM_ADJOINT_OK               projected-measure correction for camera
 *                                and light sub-paths (diffuse and Phong)
 *   VCM_GEOMETRIC_SUPPORT_OK     no transport through the geometric back
 *                                face, safe grazing-angle termination,
 *                                delta materials keep the geometric normal
 *   VCM_PDF_CONSISTENCY_OK       Evaluate/Pdf/Sample agree, PDFs stay solid
 *                                angle densities on ns, MIS area conversions
 *                                keep using the geometric cosine
 *   VCM_SMOOTH_RENDER_OK         the real PathTracer and VertexCM render a
 *                                small smooth curved surface; smooth.pfm
 *
 * Conventions used below:
 *   wf  unit direction toward the previous path vertex (the fixed direction)
 *   wg  unit direction toward the next path vertex (the generated one)
 *   ng  unit geometric normal (triangle winding), stored in Isect.normal
 *   ns  unit shading normal (interpolated), stored in Isect.shadingNormal
 *
 * The raw BSDF fs is the distribution built along ns (frame z axis). The
 * value returned to the integrators is expressed in the projected measure
 * of the true geometric surface:
 *   camera sub-path:  fs * |wg.ns| / |wg.ng|
 *   light  sub-path:  fs * |wf.ns| / |wf.ng|
 * while the projected cosine, area/solid-angle conversions and all MIS
 * densities keep using ng, and the solid-angle PDF keeps using ns.
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "vertexcm.hxx"
#include "pathtracer.hxx"

static const float TEST_EPS = 1e-5f;

static bool VecClose(const Vec3f &a, const Vec3f &b, float eps = TEST_EPS)
{
    return std::abs(a.x - b.x) < eps &&
           std::abs(a.y - b.y) < eps &&
           std::abs(a.z - b.z) < eps;
}

static bool FloatClose(float a, float b, float eps = TEST_EPS)
{
    return std::abs(a - b) < eps;
}

// Builds an Isect on a given triangle by shooting a ray straight at a
// barycentric point along the geometric normal.
static Isect HitAt(Triangle &tri, float b0, float b1, float b2)
{
    const Vec3f q = tri.p[0] * b0 + tri.p[1] * b1 + tri.p[2] * b2;
    Ray ray(q + tri.mNormal * 3.f, -tri.mNormal, 0.f);
    Isect hit(100.f);
    bool ok = tri.Intersect(ray, hit);
    assert(ok);
    return hit;
}

// A minimal scene holding a single material, enough for direct BSDF calls.
static Scene MakeOneMaterialScene(const Material &mat)
{
    Scene sc;
    sc.mMaterials.push_back(mat);
    return sc;
}

//////////////////////////////////////////////////////////////////////////
// Section 1: interpolation, alignment, fallbacks and Isect reuse
//////////////////////////////////////////////////////////////////////////
static void TestNormalInterpolation()
{
    const Vec3f p0(0, 0, 0), p1(1, 0, 0), p2(0, 1, 0);
    const Vec3f ng(0, 0, 1);
    const Vec3f n0 = Normalize(Vec3f( 0.20f, -0.10f, 1.f));
    const Vec3f n1 = Normalize(Vec3f(-0.30f,  0.20f, 1.f));
    const Vec3f n2 = Normalize(Vec3f( 0.10f,  0.30f, 1.f));

    Triangle smooth(p0, p1, p2, n0, n1, n2, 0);

    struct Bary { float b0, b1, b2; };
    const Bary cases[] = {
        {1.f, 0.f, 0.f},          // vertex 0
        {0.f, 1.f, 0.f},          // vertex 1
        {0.f, 0.f, 1.f},          // vertex 2
        {1.f/3.f, 1.f/3.f, 1.f/3.f},
        {0.25f, 0.55f, 0.20f},
        {0.70f, 0.20f, 0.10f},
    };

    for(size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i)
    {
        const Bary &bc = cases[i];
        Isect hit = HitAt(smooth, bc.b0, bc.b1, bc.b2);

        // geometric normal keeps its meaning: unit normal from winding
        assert(VecClose(hit.normal, ng));
        assert(hit.hasShadingNormal);
        assert(FloatClose(hit.shadingNormal.Length(), 1.f, 1e-6f));

        // expected barycentric interpolation, normalized and aligned to ng
        Vec3f expected = n0 * bc.b0 + n1 * bc.b1 + n2 * bc.b2;
        expected = Normalize(expected);
        if(Dot(expected, ng) < 0.f)
            expected = -expected;

        assert(VecClose(hit.shadingNormal, expected, 1e-5f));
    }

    // alignment: a vertex normal pointing below the surface gets flipped so
    // that ns always ends up on the geometric front face
    {
        const Vec3f flipped = Normalize(Vec3f(0.1f, 0.1f, -1.f));
        Triangle flipTri(p0, p1, p2, flipped, n1, n2, 0);
        Isect hit = HitAt(flipTri, 1.f, 0.f, 0.f);
        assert(hit.hasShadingNormal);
        assert(Dot(hit.shadingNormal, ng) > 0.f);
        assert(VecClose(hit.shadingNormal, -flipped));
    }

    // degenerate (near zero) interpolated vector -> geometric normal
    {
        const Vec3f tiny(1e-9f, 0.f, 0.f);
        Triangle degen(p0, p1, p2, tiny, tiny, tiny, 0);
        Isect hit = HitAt(degen, 1.f/3.f, 1.f/3.f, 1.f/3.f);
        assert(!hit.hasShadingNormal);
        assert(VecClose(hit.shadingNormal, ng));
        assert(VecClose(hit.normal, ng));
    }

    // non-finite vertex normal -> geometric normal
    {
        const float inf = std::numeric_limits<float>::infinity();
        const Vec3f bad(inf, 0.f, 1.f);
        Triangle badTri(p0, p1, p2, bad, n1, n2, 0);
        Isect hit = HitAt(badTri, 0.5f, 0.25f, 0.25f);
        assert(!hit.hasShadingNormal);
        assert(VecClose(hit.shadingNormal, ng));
    }

    // old 4-argument triangle: no shading normal at all
    {
        Triangle flat(p0, p1, p2, 0);
        Isect hit = HitAt(flat, 0.25f, 0.5f, 0.25f);
        assert(!hit.hasShadingNormal);
        assert(VecClose(hit.shadingNormal, ng));
    }

    // Isect reuse: a smooth hit must not leave a stale shading normal on a
    // later hit against an old triangle or a sphere
    {
        Isect shared(100.f);
        Ray smoothRay(p0 * 0.25f + p1 * 0.5f + p2 * 0.25f + ng * 2.f, -ng, 0.f);
        assert(smooth.Intersect(smoothRay, shared));
        assert(shared.hasShadingNormal);

        Triangle flat(p0, p1, p2, 0);
        Ray flatRay(p0 * 0.25f + p1 * 0.5f + p2 * 0.25f + ng * 2.f, -ng, 0.f);
        shared.dist = 100.f;
        assert(flat.Intersect(flatRay, shared));
        assert(!shared.hasShadingNormal);
        assert(VecClose(shared.shadingNormal, ng));

        Sphere sphere(Vec3f(0.25f, 0.25f, 0.f), 0.2f, 0);
        Ray sphereRay(Vec3f(0.25f, 0.25f, 1.f), Vec3f(0, 0, -1), 0.f);
        shared.dist = 100.f;
        assert(sphere.Intersect(sphereRay, shared));
        assert(!shared.hasShadingNormal);
        assert(VecClose(shared.shadingNormal, shared.normal));
        assert(FloatClose(shared.shadingNormal.Length(), 1.f, 1e-6f));
    }

    std::cout << "VCM_NORMAL_INTERPOLATION_OK\n";
}

//////////////////////////////////////////////////////////////////////////
// Section 2: adjoint (projected measure) conventions
//////////////////////////////////////////////////////////////////////////

static Isect SmoothIsect(const Vec3f &ng, const Vec3f &ns, const Vec3f &wf)
{
    Isect hit;
    hit.dist    = 10.f;
    hit.matID   = 0;
    hit.lightID = -1;
    hit.normal  = ng;
    hit.shadingNormal    = ns;
    hit.hasShadingNormal = true;
    (void)wf;
    return hit;
}

static Isect FlatIsect(const Vec3f &ng)
{
    Isect hit;
    hit.dist    = 10.f;
    hit.matID   = 0;
    hit.lightID = -1;
    hit.normal  = ng;
    hit.shadingNormal    = ng;
    hit.hasShadingNormal = false;
    return hit;
}

static void TestAdjoint()
{
    const Vec3f ng(0, 0, 1);
    const Vec3f ns = Normalize(Vec3f(0.35f, 0.10f, 1.f));
    const Vec3f wf = Normalize(Vec3f(-0.10f, 0.20f, 1.f));
    const Vec3f wg = Normalize(Vec3f( 0.30f,-0.20f, 1.f));

    // preconditions: both directions accepted by ns and on the ng front side
    assert(Dot(wf, ng) > EPS_COSINE && Dot(wg, ng) > EPS_COSINE);
    assert(Dot(wf, ns) > EPS_COSINE && Dot(wg, ns) > EPS_COSINE);

    // ---- pure Lambert material ----
    Material diffuse;
    diffuse.mDiffuseReflectance = Vec3f(0.5f);
    Scene sc = MakeOneMaterialScene(diffuse);

    Ray ray(Vec3f(0.2f, 0.2f, 0.f) - wf * 2.f, -wf, 0.f);
    Isect hitS = SmoothIsect(ng, ns, wf);
    Isect hitF = FlatIsect(ng);

    BSDF<false> camS(ray, hitS, sc), camF(ray, hitF, sc);
    BSDF<true>  lgtS(ray, hitS, sc), lgtF(ray, hitF, sc);
    assert(camS.IsValid() && lgtS.IsValid() && camF.IsValid() && lgtF.IsValid());

    const float fs = 0.5f * INV_PI_F; // analytic Lambert value along ns

    float cosCam, pdfCamDir, pdfCamRev;
    const Vec3f valCamS = camS.Evaluate(sc, wg, cosCam, &pdfCamDir, &pdfCamRev);
    float cosLgt, pdfLgtDir, pdfLgtRev;
    const Vec3f valLgtS = lgtS.Evaluate(sc, wg, cosLgt, &pdfLgtDir, &pdfLgtRev);
    float cosCamF, pdfCamF;
    const Vec3f valCamF = camF.Evaluate(sc, wg, cosCamF, &pdfCamF);
    float cosLgtF, pdfLgtF;
    const Vec3f valLgtF = lgtF.Evaluate(sc, wg, cosLgtF, &pdfLgtF);

    // projected cosine is the geometric one for both path orientations
    assert(FloatClose(cosCam, std::abs(Dot(wg, ng))));
    assert(FloatClose(cosLgt, std::abs(Dot(wg, ng))));
    assert(FloatClose(cosCamF, std::abs(Dot(wg, ng))));
    assert(FloatClose(cosLgtF, std::abs(Dot(wg, ng))));

    // camera: fs * |wg.ns| / |wg.ng|
    const float camCorrect =
        std::abs(Dot(wg, ns)) / std::abs(Dot(wg, ng));
    assert(FloatClose(valCamS.x, fs * camCorrect));
    // light: fs * |wf.ns| / |wf.ng|
    const float lgtCorrect =
        std::abs(Dot(wf, ns)) / std::abs(Dot(wf, ng));
    assert(FloatClose(valLgtS.x, fs * lgtCorrect));

    // old geometry (ns == ng): correction is exactly 1, original algorithm
    assert(FloatClose(valCamF.x, fs));
    assert(FloatClose(valLgtF.x, fs));
    assert(FloatClose(camS.CosThetaFix(), Dot(-ray.dir, ng)));
    assert(FloatClose(lgtS.CosThetaFix(), Dot(-ray.dir, ng)));

    // PDFs are solid angle densities on ns, never multiplied by correction
    const float expectedPdf = std::abs(Dot(wg, ns)) * INV_PI_F;
    assert(FloatClose(pdfCamDir, expectedPdf));
    assert(FloatClose(pdfLgtDir, expectedPdf));
    assert(FloatClose(pdfCamF, std::abs(Dot(wg, ng)) * INV_PI_F));
    // tilt means value and pdf genuinely differ, proving the factor is not
    // folded into the PDF
    assert(!FloatClose(valCamS.x, pdfCamDir, 1e-3f));

    // fixed random numbers: sampled direction, pdf, factor must match the
    // analytic cosine-weighted hemisphere sample in the ns frame
    const Vec3f rnd(0.3f, 0.4f, 0.1f); // z picks the diffuse component
    float samplePdf, sampleCos;
    Vec3f sampleDir;
    const Vec3f sampleVal =
        camS.Sample(sc, rnd, sampleDir, samplePdf, sampleCos);

    Frame frameNs;
    frameNs.SetFromZ(ns);
    float rawPdf;
    const Vec3f localSample = SampleCosHemisphereW(rnd.GetXY(), &rawPdf);
    const Vec3f expectedDir = frameNs.ToWorld(localSample);
    assert(VecClose(sampleDir, expectedDir));
    assert(FloatClose(samplePdf, rawPdf));                                  // ns density
    assert(FloatClose(sampleCos, std::abs(Dot(expectedDir, ng))));         // ng cosine
    assert(FloatClose(sampleVal.x, fs *
        (std::abs(Dot(expectedDir, ns)) / std::abs(Dot(expectedDir, ng)))));

    // same sample through the light BSDF: correction uses wf instead
    float samplePdfL, sampleCosL;
    Vec3f sampleDirL;
    const Vec3f sampleValL =
        lgtS.Sample(sc, rnd, sampleDirL, samplePdfL, sampleCosL);
    assert(VecClose(sampleDirL, expectedDir));
    assert(FloatClose(samplePdfL, rawPdf));
    assert(FloatClose(sampleCosL, std::abs(Dot(expectedDir, ng))));
    assert(FloatClose(sampleValL.x, fs * lgtCorrect));

    // ---- Phong material: the glossy lobe is built along ns, so its raw
    // value rotates with ns. Evaluate it analytically in the ns frame and
    // check the same projected-measure correction is applied on top.
    Material phong;
    phong.mPhongReflectance = Vec3f(0.4f);
    phong.mPhongExponent    = 20.f;
    Scene scP = MakeOneMaterialScene(phong);

    BSDF<false> camPS(ray, hitS, scP);
    BSDF<true>  lgtPS(ray, hitS, scP);
    assert(camPS.IsValid() && lgtPS.IsValid());

    Frame fNs;
    fNs.SetFromZ(ns);
    // mLocalDirFix = frame.ToLocal(-ray.dir) and ray.dir = -wf, hence wf
    const Vec3f lFix = fNs.ToLocal(wf);
    // glossy lobe peak = reflection of wf about ns
    Vec3f wgPhong = Normalize(-wf + 2.f * Dot(wf, ns) * ns);
    assert(Dot(wgPhong, ng) > EPS_COSINE && Dot(wgPhong, ns) > EPS_COSINE);
    const Vec3f lGen = fNs.ToLocal(wgPhong);
    const Vec3f refl = ReflectLocal(lFix);
    const float dotR = std::max(0.f, Dot(refl, lGen));
    const float rho = 0.4f * (20.f + 2.f) * 0.5f * INV_PI_F;
    const float fsPhong = rho * std::pow(dotR, 20.f);
    assert(fsPhong > 0.f);

    const float camCorrectPhong =
        std::abs(Dot(wgPhong, ns)) / std::abs(Dot(wgPhong, ng));
    const float lgtCorrectPhong =
        std::abs(Dot(wf, ns)) / std::abs(Dot(wf, ng));

    float pc1, pc2;
    const Vec3f phongCam = camPS.Evaluate(scP, wgPhong, pc1);
    const Vec3f phongLgt = lgtPS.Evaluate(scP, wgPhong, pc2);
    assert(FloatClose(phongCam.x, fsPhong * camCorrectPhong));
    assert(FloatClose(phongLgt.x, fsPhong * lgtCorrectPhong));
    // projected cosine still geometric
    assert(FloatClose(pc1, std::abs(Dot(wgPhong, ng))));
    assert(FloatClose(pc2, std::abs(Dot(wgPhong, ng))));

    std::cout << "VCM_ADJOINT_OK\n";
}

//////////////////////////////////////////////////////////////////////////
// Section 3: geometric support and safe termination
//////////////////////////////////////////////////////////////////////////
static void TestGeometricSupport()
{
    const Vec3f ng(0, 0, 1);
    const Vec3f ns = Normalize(Vec3f(0.6f, 0.f, 1.f));

    Material diffuse;
    diffuse.mDiffuseReflectance = Vec3f(0.6f);
    Scene sc = MakeOneMaterialScene(diffuse);

    const Vec3f wf(0, 0, 1);
    Ray ray(Vec3f(0.3f, 0.3f, 0.f) - wf * 2.f, -wf, 0.f);
    Isect hit = SmoothIsect(ng, ns, wf);
    BSDF<false> cam(ray, hit, sc);
    BSDF<true>  lgt(ray, hit, sc);
    assert(cam.IsValid() && lgt.IsValid());

    // direction inside the shading hemisphere but on the geometric back side
    const Vec3f backDir = Normalize(Vec3f(1.f, 0.f, -0.5f));
    assert(Dot(backDir, ns) > 0.f);   // accepted by the ns lobe
    assert(Dot(backDir, ng) < 0.f);   // rejected by the true surface

    float cosBack, pdfBack;
    const Vec3f valBack = cam.Evaluate(sc, backDir, cosBack, &pdfBack);
    assert(valBack.IsZero() && pdfBack == 0.f && cosBack == 0.f);
    assert(lgt.Evaluate(sc, backDir, cosBack).IsZero());
    assert(cam.Pdf(sc, backDir) == 0.f);
    assert(lgt.Pdf(sc, backDir) == 0.f);
    assert(cam.Pdf(sc, backDir, true) == 0.f);

    // sampling: back-side samples terminate with zero contribution and zero
    // pdf; no resampling and no renormalization of the remaining density
    int terminated = 0;
    int accepted   = 0;
    for(int i = 1; i < 40; ++i)
    {
        for(int j = 1; j < 40; ++j)
        {
            const Vec3f rnd(float(i) / 40.f, float(j) / 40.f, 0.05f);
            float spdf, scos;
            Vec3f sdir;
            const Vec3f sval = cam.Sample(sc, rnd, sdir, spdf, scos);
            if(sval.IsZero())
            {
                ++terminated;
                assert(spdf == 0.f && scos == 0.f);
            }
            else
            {
                ++accepted;
                // accepted samples keep the ordinary ns solid-angle density,
                // i.e. nothing was renormalized
                assert(FloatClose(spdf,
                    std::max(0.f, Dot(sdir, ns)) * INV_PI_F, 1e-5f));
                // and they never cross the geometric surface
                assert(Dot(sdir, ng) > 0.f);
            }
        }
    }
    assert(terminated > 0 && accepted > 0);

    // grazing geometric angle below EPS_COSINE: safe zero, never a clamped
    // tiny denominator that would explode into huge energy
    const Vec3f grazing = Normalize(Vec3f(1.f, 0.f, 0.5e-6f));
    assert(Dot(grazing, ns) > 0.f);
    assert(std::abs(Dot(grazing, ng)) < EPS_COSINE);
    float cosG, pdfG;
    assert(cam.Evaluate(sc, grazing, cosG, &pdfG).IsZero());
    assert(pdfG == 0.f && cosG == 0.f);
    assert(cam.Pdf(sc, grazing) == 0.f);

    // delta material on a smooth triangle: frame stays on ng, reflection is
    // governed by the geometric normal and no correction is applied
    Material mirror;
    mirror.mMirrorReflectance = Vec3f(1.f);
    Scene scM = MakeOneMaterialScene(mirror);
    BSDF<false> camMirror(ray, hit, scM);
    assert(camMirror.IsValid() && camMirror.IsDelta());
    float mPdf, mCos;
    Vec3f mDir;
    const Vec3f mVal = camMirror.Sample(scM, Vec3f(0.2f, 0.3f, 0.9f),
        mDir, mPdf, mCos);
    assert(!mVal.IsZero());
    assert(VecClose(mDir, wf));                 // reflection about ng, not ns
    assert(FloatClose(mCos, 1.f));

    // glass keeps the geometric normal as well: with wf == ng both reflected
    // and refracted directions lie on the z axis of an ng frame, never on ns
    Material glass;
    glass.mMirrorReflectance = Vec3f(1.f);
    glass.mIOR = 1.6f;
    Scene scG = MakeOneMaterialScene(glass);
    BSDF<false> camGlass(ray, hit, scG);
    assert(camGlass.IsValid());
    Frame fNg;
    fNg.SetFromZ(ng);
    for(int k = 0; k < 4; ++k)
    {
        float gPdf, gCos;
        Vec3f gDir;
        const Vec3f gVal = camGlass.Sample(scG,
            Vec3f(0.2f, 0.3f, 0.02f + 0.3f * k), gDir, gPdf, gCos);
        assert(!gVal.IsZero());
        const Vec3f gLocal = fNg.ToLocal(gDir);
        assert(std::abs(gLocal.x) < TEST_EPS && std::abs(gLocal.y) < TEST_EPS);
    }

    std::cout << "VCM_GEOMETRIC_SUPPORT_OK\n";
}

//////////////////////////////////////////////////////////////////////////
// Section 4: Evaluate/Pdf/Sample consistency and MIS measure handling
//////////////////////////////////////////////////////////////////////////
static void TestPdfConsistency()
{
    const Vec3f ng(0, 0, 1);
    const Vec3f ns = Normalize(Vec3f(-0.25f, 0.30f, 1.f));
    Material diffuse;
    diffuse.mDiffuseReflectance = Vec3f(0.7f);
    Scene sc = MakeOneMaterialScene(diffuse);

    const Vec3f wf = Normalize(Vec3f(0.15f, -0.1f, 1.f));
    Ray ray(Vec3f(0.1f, 0.1f, 0.f) - wf * 2.f, -wf, 0.f);
    Isect hit = SmoothIsect(ng, ns, wf);
    BSDF<false> cam(ray, hit, sc);
    BSDF<true>  lgt(ray, hit, sc);

    // fixed set of directions: accepted ones, back ones, grazing ones
    std::vector<Vec3f> dirs;
    dirs.push_back(Normalize(Vec3f( 0.3f,  0.2f, 1.f)));
    dirs.push_back(Normalize(Vec3f(-0.4f,  0.5f, 0.7f)));
    dirs.push_back(Normalize(Vec3f( 0.2f, -0.3f, 0.4f)));
    dirs.push_back(Normalize(Vec3f( 1.0f,  0.0f,-0.4f)));  // geometric back
    dirs.push_back(Normalize(Vec3f( 1.0f,  0.0f, 0.4e-6f))); // grazing

    for(size_t i = 0; i < dirs.size(); ++i)
    {
        const Vec3f &wg = dirs[i];
        const bool accepted =
            Dot(wg, ns) > EPS_COSINE && Dot(wf, ns) > EPS_COSINE &&
            Dot(wg, ng) * Dot(wf, ng) > 0.f &&
            std::abs(Dot(wg, ng)) >= EPS_COSINE;

        float cosE, dirPdf, revPdf;
        const Vec3f val = cam.Evaluate(sc, wg, cosE, &dirPdf, &revPdf);
        if(accepted)
        {
            assert(!val.IsZero());
            assert(FloatClose(dirPdf, cam.Pdf(sc, wg)));
            assert(FloatClose(revPdf, cam.Pdf(sc, wg, true)));
            // light BSDF shares the same solid-angle density
            assert(FloatClose(dirPdf, lgt.Pdf(sc, wg)));
            assert(FloatClose(cosE, std::abs(Dot(wg, ng))));
        }
        else
        {
            assert(val.IsZero() && dirPdf == 0.f && revPdf == 0.f);
            assert(cam.Pdf(sc, wg) == 0.f && lgt.Pdf(sc, wg) == 0.f);
            assert(cam.Pdf(sc, wg, true) == 0.f);
        }
    }

    // sampled pdf equals an explicit Pdf() query, and both remain ns densities
    for(int i = 1; i < 30; ++i)
    {
        const Vec3f rnd(float(i) / 31.f, 0.37f, 0.05f);
        float spdf, scos;
        Vec3f sdir;
        const Vec3f sval = cam.Sample(sc, rnd, sdir, spdf, scos);
        if(sval.IsZero())
        {
            assert(cam.Pdf(sc, sdir) == 0.f);
            continue;
        }
        assert(FloatClose(spdf, cam.Pdf(sc, sdir)));
        assert(FloatClose(spdf,
            std::max(0.f, Dot(sdir, ns)) * INV_PI_F, 1e-5f));
        assert(FloatClose(scos, std::abs(Dot(sdir, ng))));
    }

    // MIS area <-> solid angle conversion uses the geometric cosine only.
    // Round trip with ng is exact; using ns would give a different density.
    const float dist = 2.5f;
    const Vec3f wg = Normalize(Vec3f(0.3f, -0.1f, 1.f));
    const float pdfW = cam.Pdf(sc, wg);
    const float cosGeo = std::abs(Dot(wg, ng));
    const float cosShd = std::abs(Dot(wg, ns));
    const float pdfA = PdfWtoA(pdfW, dist, cosGeo);
    assert(FloatClose(PdfAtoW(pdfA, dist, cosGeo), pdfW));
    const float pdfAWrong = PdfWtoA(pdfW, dist, cosShd);
    assert(std::abs(pdfAWrong - pdfA) > 1e-5f); // shading normal is not allowed here

    // the fixed-direction cosine feeding MIS in the integrators is geometric
    assert(FloatClose(cam.CosThetaFix(), Dot(wf, ng)));
    assert(FloatClose(lgt.CosThetaFix(), Dot(wf, ng)));

    std::cout << "VCM_PDF_CONSISTENCY_OK\n";
}

//////////////////////////////////////////////////////////////////////////
// Section 5: render a small smooth curved surface with the real integrators
//////////////////////////////////////////////////////////////////////////

// A flat z=0 grid whose vertex normals describe a smooth dome: the geometry
// stays planar (so ray offsets, occlusion and light area are trivially the
// geometric ones) while the shading normals vary across the surface.
// Fills an existing Scene in place because Scene owns raw geometry pointers
// and must not be copied.
static void BuildSmoothScene(
    Scene &sc,
    const Vec2f &res,
    bool smoothNormals)
{
    Material mat;
    mat.mDiffuseReflectance = Vec3f(0.7f);
    sc.mMaterials.push_back(mat);          // 0: smooth diffuse surface

    GeometryList *geom = new GeometryList;
    sc.mGeometry = geom;

    const int N = 4;
    const float extent = 1.f;
    std::vector<Vec3f> pos((N + 1) * (N + 1));
    std::vector<Vec3f> nrm((N + 1) * (N + 1));
    for(int iy = 0; iy <= N; ++iy)
    {
        for(int ix = 0; ix <= N; ++ix)
        {
            const float x = -extent + 2.f * extent * ix / N;
            const float y = -extent + 2.f * extent * iy / N;
            const int idx = iy * (N + 1) + ix;
            pos[idx] = Vec3f(x, y, 0.f);
            // dome-like smooth normal, tilted toward the grid center
            nrm[idx] = Normalize(Vec3f(-0.35f * x, -0.35f * y, 1.f));
        }
    }

    for(int iy = 0; iy < N; ++iy)
    {
        for(int ix = 0; ix < N; ++ix)
        {
            const int a = iy * (N + 1) + ix;
            const int b = a + 1;
            const int c = a + (N + 1);
            const int d = c + 1;
            if(smoothNormals)
            {
                geom->mGeometry.push_back(
                    new Triangle(pos[a], pos[b], pos[c],
                                 nrm[a], nrm[b], nrm[c], 0));
                geom->mGeometry.push_back(
                    new Triangle(pos[b], pos[d], pos[c],
                                 nrm[b], nrm[d], nrm[c], 0));
            }
            else
            {
                geom->mGeometry.push_back(new Triangle(pos[a], pos[b], pos[c], 0));
                geom->mGeometry.push_back(new Triangle(pos[b], pos[d], pos[c], 0));
            }
        }
    }

    // area light above the surface, emitting downward (normal toward -z)
    AreaLight *light = new AreaLight(
        Vec3f(-0.4f, -0.4f, 2.f),
        Vec3f( 0.4f,  0.4f, 2.f),
        Vec3f( 0.4f, -0.4f, 2.f));
    light->mIntensity = Vec3f(6.f);
    sc.mLights.push_back(light);

    sc.mCamera.Setup(
        Vec3f(0.f, -2.6f, 1.5f),
        Normalize(Vec3f(0.f, 2.6f, -1.1f)),
        Vec3f(0.f, 0.f, 1.f),
        res, 45);

    sc.BuildSceneSphere();
}

static void TestSmoothRender(const std::string &outDir)
{
    const Vec2f res(8, 8);

    // real path tracer on the smooth surface
    Scene ptScene;
    BuildSmoothScene(ptScene, res, true);
    PathTracer pt(ptScene, 1234);
    pt.mMaxPathLength = 4;
    pt.RunIteration(0);
    Framebuffer ptFb;
    pt.GetFramebuffer(ptFb);
    assert(pt.WasUsed());
    const float ptLum = ptFb.TotalLuminance();
    assert(std::isfinite(ptLum) && ptLum > 0.f);

    // real VertexCM (BPT) on the same surface
    Scene vcmScene;
    BuildSmoothScene(vcmScene, res, true);
    VertexCM vcm(vcmScene, VertexCM::kBpt, 0.003f, 0.75f, 1234);
    vcm.mMaxPathLength = 4;
    vcm.RunIteration(0);
    Framebuffer vcmFb;
    vcm.GetFramebuffer(vcmFb);
    assert(vcm.WasUsed());
    const float vcmLum = vcmFb.TotalLuminance();
    assert(std::isfinite(vcmLum) && vcmLum > 0.f);

    const std::string path = outDir + "/smooth.pfm";
    vcmFb.SavePFM(path.c_str());
    std::ifstream f(path.c_str(), std::ios::binary);
    assert(f.good());
    f.seekg(0, std::ios::end);
    assert(f.tellg() > 0);

    // flat control scene: identical geometry/light/seed, only ns == ng differs.
    // A changed image proves the shading normals drive transport (not a
    // post-process), while energy stays finite.
    Scene flatScene;
    BuildSmoothScene(flatScene, res, false);
    VertexCM flatVcm(flatScene, VertexCM::kBpt, 0.003f, 0.75f, 1234);
    flatVcm.mMaxPathLength = 4;
    flatVcm.RunIteration(0);
    Framebuffer flatFb;
    flatVcm.GetFramebuffer(flatFb);
    const float flatLum = flatFb.TotalLuminance();
    assert(std::isfinite(flatLum) && flatLum > 0.f);
    assert(std::abs(flatLum - vcmLum) > 1e-4f);

    // Geometric-cosine / MIS accounting in this render:
    //  * the grid is geometrically planar, so occlusion rays, the EPS_RAY
    //    origin offset and the AreaLight area/pdf never see a bent surface;
    //  * BSDF::CosThetaFix() returns |wf.ng|, hence PdfAtoW in the path
    //    tracer and the dVCM/dVC/dVM updates in VertexCM divide by the
    //    geometric cosine;
    //  * ConnectVertices' geometry term cosLight*cosCamera/dist^2 uses the
    //    cosines returned by Evaluate(), which are |w.ng|, while the MIS
    //    pdfs remain solid-angle densities sampled along ns;
    //  * samples pushed below the geometric tangent plane terminate with
    //    zero weight instead of being resampled, so the MIS weights stay
    //    normalized over the original domain.
    std::cout << "VCM_SMOOTH_RENDER_OK\n";
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const std::string outDir = argv[1];

    TestNormalInterpolation();
    TestAdjoint();
    TestGeometricSupport();
    TestPdfConsistency();
    TestSmoothRender(outDir);

    return 0;
}
