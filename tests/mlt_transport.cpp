/*
 * Tests for the primary sample space Metropolis renderer (src/mlt.hxx) and
 * the shared single-path evaluation in src/pathtracer.hxx.
 *
 * Three registered cases are driven by task_support/driver.cpp:
 *   replay    - the deterministic, stream-driven single path routine
 *   chain     - hand-computed Metropolis chain mechanics and transactions
 *   transport - a real occlusion/specular/small-light render vs. path tracing
 *
 * The file intentionally has no main(): driver.cpp supplies it.
 */

#include <iostream>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <string>
#include <limits>

#include "check.hpp"
#include "scene.hxx"
#include "framebuffer.hxx"
#include "pathtracer.hxx"
#include "mlt.hxx"

namespace
{

bool Approx(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) <= eps * (1.f + std::fabs(a) + std::fabs(b));
}

bool AllFinite(const Framebuffer& fb)
{
    for(int y = 0; y < fb.Height(); ++y)
        for(int x = 0; x < fb.Width(); ++x)
        {
            const Vec3f& p = fb.Pixel(x, y);
            if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                return false;
        }
    return true;
}

// Hand-computable two-segment scalar luminance function over the first
// primary sample, with a configurable path length (number of consumed
// primary dimensions) so short/long path switching can be exercised.
class ScriptedEvaluator : public IPathEvaluator
{
public:

    ScriptedEvaluator(int aDims, int aResX, int aResY)
        : mDims(aDims), mResX(aResX), mResY(aResY), mLastDimsUsed(0)
    {}

    virtual void Evaluate(
        PrimarySampleVector& aStream,
        Vec2f&               oRaster,
        Vec3f&               oColor,
        float&               oLuminance)
    {
        float x = 0.5f, y = 0.5f;
        for(int i = 0; i < mDims; ++i)
        {
            const float v = aStream.Next1D();
            if(i == 0) x = v;
            if(i == 1) y = v;
        }
        mLastDimsUsed = aStream.DimsUsed();

        // Two brightness regions, split at x = 0.5.
        const float lum = x < 0.5f ? 2.f : 8.f;
        oLuminance = lum;
        oColor     = Vec3f(lum); // Luminance(Vec3f(lum)) == lum
        oRaster    = Vec2f(x * mResX, y * mResY);
    }

    int LastDimsUsed() const { return mLastDimsUsed; }

private:

    int mDims;
    int mResX;
    int mResY;
    int mLastDimsUsed;
};

// Fully scripted evaluator: consumes a fixed number of dimensions and
// returns a fixed luminance/color/raster, so chain arithmetic can be
// computed by hand regardless of the mutated sample values.
class FixedEvaluator : public IPathEvaluator
{
public:

    FixedEvaluator(int aDims, float aLuminance, const Vec2f& aRaster)
        : mDims(aDims), mLuminance(aLuminance), mRaster(aRaster)
    {}

    virtual void Evaluate(
        PrimarySampleVector& aStream,
        Vec2f&               oRaster,
        Vec3f&               oColor,
        float&               oLuminance)
    {
        for(int i = 0; i < mDims; ++i)
            (void)aStream.Next1D();
        oLuminance = mLuminance;
        oColor     = Vec3f(mLuminance);
        oRaster    = mRaster;
    }

private:

    int   mDims;
    float mLuminance;
    Vec2f mRaster;
};

// Test-only access to the protected MLT chain machinery, driven by the
// scripted evaluator instead of the real camera path.
class TestHarness : public MLTRenderer
{
public:

    TestHarness(const Scene& aScene, const MLTOptions& aOptions)
        : MLTRenderer(aScene, aOptions, 1234)
    {}

    using MLTRenderer::RunOneProposal;

    void SeedState(
        const std::vector<float>& aValues,
        const Vec2f&              aRaster,
        const Vec3f&              aColor,
        const float               aLuminance)
    {
        mState.Assign(aValues);
        mCurrentRaster    = aRaster;
        mCurrentColor     = aColor;
        mCurrentLuminance = aLuminance;
    }

    void SetNormalization(float b) { mStats.Normalization = b; }

    PrimarySampleVector& MutableState() { return mState; }
    MLTStats& MutableStats() { return mStats; }

    void SnapshotFramebuffer(Framebuffer& oFb)
    {
        // No completed iterations, so GetFramebuffer returns raw sums.
        GetFramebuffer(oFb);
    }
};

void SetupCornell(Scene& aScene, const Vec2i& aResolution)
{
    // Ceiling area light (small light box) plus mirror and glass spheres:
    // occlusion, specular reflection/refraction and a small emitter.
    aScene.LoadCornellBox(aResolution,
        Scene::kLightCeiling | Scene::kBothSmallSpheres);
    aScene.BuildSceneSphere();
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// replay: one stream of uniforms deterministically drives one camera path.

CASE(replay)
{
    Scene scene; SetupCornell(scene, Vec2i(16, 12));
    const int resX = 16, resY = 12;
    const uint minLen = 0, maxLen = 5;

    // Independent uniform source #1: an explicit vector.
    const int kN = 4096;
    std::vector<float> values;
    values.reserve(kN);
    Rng sourceRng(7);
    for(int i = 0; i < kN; ++i)
        values.push_back(sourceRng.GetFloat());

    VectorStream streamA(values);
    const SinglePathResult rA =
        MLTRenderer::EvaluatePath(scene, minLen, maxLen, streamA);

    // The first two uniforms must be exactly the full-frame raster position.
    CHECK(Approx(rA.mRaster.x, values[0] * resX, 1e-6f));
    CHECK(Approx(rA.mRaster.y, values[1] * resY, 1e-6f));
    CHECK(rA.mDimsUsed > 2);
    CHECK(rA.mDimsUsed <= kN);
    CHECK(std::isfinite(Luminance(rA.mColor)));

    // Same primary vector through a second, independent stream instance:
    // identical raster and bit-identical linear RGB.
    VectorStream streamB(values);
    const SinglePathResult rB =
        MLTRenderer::EvaluatePath(scene, minLen, maxLen, streamB);
    CHECK(rB.mDimsUsed == rA.mDimsUsed);
    CHECK(rB.mRaster.x == rA.mRaster.x && rB.mRaster.y == rA.mRaster.y);
    CHECK(rB.mColor.x == rA.mColor.x);
    CHECK(rB.mColor.y == rA.mColor.y);
    CHECK(rB.mColor.z == rA.mColor.z);

    // A second independent stream implementation, replaying the exact same
    // uniform sequence from its own Rng, must reach the same path result.
    Rng replayRng(7);
    RecordingStream streamC(replayRng);
    const SinglePathResult rC =
        MLTRenderer::EvaluatePath(scene, minLen, maxLen, streamC);
    CHECK(streamC.DimsUsed() == rA.mDimsUsed);
    CHECK(Approx(rC.mRaster.x, rA.mRaster.x, 1e-6f));
    CHECK(Approx(rC.mRaster.y, rA.mRaster.y, 1e-6f));
    CHECK(Approx(rC.mColor.x, rA.mColor.x, 1e-6f));
    CHECK(Approx(rC.mColor.y, rA.mColor.y, 1e-6f));
    CHECK(Approx(rC.mColor.z, rA.mColor.z, 1e-6f));

    // Prefix replay: feeding exactly the consumed prefix gives the same path,
    // i.e. dimensions are consumed strictly in order with no hidden RNG use.
    std::vector<float> prefix(values.begin(),
        values.begin() + rA.mDimsUsed);
    VectorStream streamD(prefix);
    const SinglePathResult rD =
        MLTRenderer::EvaluatePath(scene, minLen, maxLen, streamD);
    CHECK(streamD.DimsUsed() == rA.mDimsUsed);
    CHECK(rD.mColor.x == rA.mColor.x);
    CHECK(rD.mColor.y == rA.mColor.y);
    CHECK(rD.mColor.z == rA.mColor.z);

    // Across many random vectors the routine stays in bounds and finite, and
    // at least some paths carry light back out of the scene.
    int lit = 0;
    Rng floodRng(31);
    for(int s = 0; s < 256; ++s)
    {
        std::vector<float> v(kN);
        for(int i = 0; i < kN; ++i) v[i] = floodRng.GetFloat();
        VectorStream vs(v);
        const SinglePathResult r =
            MLTRenderer::EvaluatePath(scene, minLen, maxLen, vs);
        CHECK(vs.DimsUsed() <= kN);
        CHECK(r.mRaster.x >= 0.f && r.mRaster.x < resX);
        CHECK(r.mRaster.y >= 0.f && r.mRaster.y < resY);
        CHECK(std::isfinite(Luminance(r.mColor)));
        if(Luminance(r.mColor) > 0.f) ++lit;
    }
    CHECK(lit > 0);
}

//////////////////////////////////////////////////////////////////////////
// chain: acceptance, symmetric proposals, wrap-around, transactions and
// dual-state settlement, all with hand-computed constants.

CASE(chain)
{
    // ---- Option validation ------------------------------------------------
    auto invalid = [](const MLTOptions& o)
    {
        Scene scene; SetupCornell(scene, Vec2i(8, 8));
        bool threw = false;
        try { MLTRenderer r(scene, o); }
        catch(const std::invalid_argument&) { threw = true; }
        return threw;
    };
    CHECK(invalid(MLTOptions(0, 0.1f, 0.02f)));
    CHECK(invalid(MLTOptions(-7, 0.1f, 0.02f)));
    CHECK(invalid(MLTOptions(100, 0.f, 0.02f)));
    CHECK(invalid(MLTOptions(100, -0.1f, 0.02f)));
    CHECK(invalid(MLTOptions(100, 1.01f, 0.02f)));
    CHECK(invalid(MLTOptions(100,
        std::numeric_limits<float>::quiet_NaN(), 0.02f)));
    CHECK(invalid(MLTOptions(100,
        std::numeric_limits<float>::infinity(), 0.02f)));
    CHECK(invalid(MLTOptions(100, 0.1f, 0.f)));
    CHECK(invalid(MLTOptions(100, 0.1f, -0.02f)));
    CHECK(invalid(MLTOptions(100, 0.1f, 0.51f)));
    CHECK(invalid(MLTOptions(100, 0.1f,
        std::numeric_limits<float>::quiet_NaN())));
    MLTOptions good(100, 1.f, 0.5f); // boundary values are legal
    Scene goodScene; SetupCornell(goodScene, Vec2i(8, 8));
    MLTRenderer goodRenderer(goodScene, good);
    CHECK(!goodRenderer.IsInitialized()); // lazy initialization

    // ---- Acceptance rate, including the zero cases ------------------------
    CHECK(MLTRenderer::Acceptance(0.f, 0.f) == 0.f);
    CHECK(MLTRenderer::Acceptance(0.f, 3.f) == 1.f);
    CHECK(Approx(MLTRenderer::Acceptance(4.f, 2.f), 0.5f));
    CHECK(MLTRenderer::Acceptance(2.f, 4.f) == 1.f);
    CHECK(MLTRenderer::Acceptance(5.f, 5.f) == 1.f);

    // ---- Wrap-around ------------------------------------------------------
    CHECK(Approx(MLTRenderer::WrapUnit(1.25f), 0.25f));
    CHECK(Approx(MLTRenderer::WrapUnit(-0.1f), 0.9f));
    CHECK(Approx(MLTRenderer::WrapUnit(0.3f), 0.3f));

    // ---- Symmetric small step applied lazily on read, with wrapping -------
    {
        PrimarySampleVector v;
        v.Assign(std::vector<float>{0.9f, 0.1f});
        const float width = 0.2f;
        // offsets: (2*0.75-1)*0.2 = +0.1 ; (2*0.25-1)*0.2 = -0.1
        std::vector<float> mut{0.75f, 0.25f};
        VectorStream src(mut);
        v.BeginProposal(false, src, width);
        // A small step touches nothing until the path reads a dimension.
        CHECK(Approx(v.ValueAt(0), 0.9f));
        CHECK(v.GenerationAt(0) == 0u && v.GlobalGeneration() == 0u);
        const float r0 = v.Next1D(); // dim 0 now receives +0.1
        CHECK(Approx(r0, 0.0f));     // 0.9 + 0.1 wraps to 0
        CHECK(Approx(v.ValueAt(0), 0.0f));
        CHECK(v.GenerationAt(0) == 1u);
        const float r1 = v.Next1D(); // dim 1 receives -0.1
        CHECK(Approx(r1, 0.0f));     // 0.1 - 0.1
        CHECK(v.GenerationAt(1) == 1u);
        v.Rollback();
        CHECK(v.Size() == 2);
        CHECK(Approx(v.ValueAt(0), 0.9f));
        CHECK(Approx(v.ValueAt(1), 0.1f));
        CHECK(v.GlobalGeneration() == 0u && v.GenerationAt(0) == 0u);
    }

    // ---- Unused dimensions keep value, generation and uniform marginal ----
    {
        PrimarySampleVector v;
        v.Assign(std::vector<float>{0.3f, 0.8f, 0.55f});
        std::vector<float> mut{0.75f}; // exactly one offset is needed
        VectorStream src(mut);
        v.BeginProposal(false, src, 0.2f);
        (void)v.Next1D(); // only dim 0 is read/mutated
        CHECK(v.GenerationAt(0) == 1u);
        CHECK(v.GenerationAt(1) == 0u && v.GenerationAt(2) == 0u);
        CHECK(Approx(v.ValueAt(1), 0.8f));
        CHECK(Approx(v.ValueAt(2), 0.55f));
        CHECK(src.DimsUsed() == 1); // one mutation uniform per used dim
        v.Rollback();
        CHECK(Approx(v.ValueAt(0), 0.3f));
        CHECK(v.Size() == 3);
    }

    // ---- Large step re-samples every dimension ----------------------------
    {
        PrimarySampleVector v;
        v.Assign(std::vector<float>{0.9f, 0.1f, 0.4f});
        std::vector<float> mut{0.11f, 0.22f, 0.33f, 0.44f /*decision*/};
        VectorStream src(mut);
        v.BeginProposal(true, src, 0.2f);
        CHECK(Approx(v.ValueAt(0), 0.11f));
        CHECK(Approx(v.ValueAt(1), 0.22f));
        CHECK(Approx(v.ValueAt(2), 0.33f));
        v.Rollback();
        CHECK(v.Size() == 3 && Approx(v.ValueAt(2), 0.4f));
    }

    // ---- Two-segment luminance function: low and high regions -------------
    {
        ScriptedEvaluator eval(2, 16, 12);
        Vec2f raster; Vec3f color; float lum;

        PrimarySampleVector low;
        low.Assign(std::vector<float>{0.2f, 0.5f}); // x < 0.5
        eval.Evaluate(low, raster, color, lum);
        CHECK(Approx(lum, 2.f) && eval.LastDimsUsed() == 2);

        PrimarySampleVector high;
        high.Assign(std::vector<float>{0.8f, 0.5f}); // x >= 0.5
        eval.Evaluate(high, raster, color, lum);
        CHECK(Approx(lum, 8.f) && eval.LastDimsUsed() == 2);
    }

    // ---- Rejected short->long switch: no pollution of accepted dims -------
    {
        Scene scene; SetupCornell(scene, Vec2i(16, 12));
        MLTOptions opts(100, 0.5f, 0.1f);
        TestHarness harness(scene, opts);
        harness.SetNormalization(4.f);
        // Current short (2-dim) state in the bright region, f = 8.
        harness.SeedState(std::vector<float>{0.9f, 0.5f},
            Vec2f(0.9f * 16, 0.5f * 12), Vec3f(8.f), 8.f);

        // A long path consumes 4 dims and lands in the dim region, g = 2.
        FixedEvaluator longEval(4, 2.f, Vec2f(1.f, 6.f));
        // Uniforms: step flag 0.9 -> small step; 2 offsets; long path adds
        // 2 dims x (fresh + offset); decision 0.9.
        std::vector<float> mut;
        for(int i = 0; i < 32; ++i) mut.push_back(0.9f);
        VectorStream src(mut);
        const int before = src.DimsUsed();
        harness.RunOneProposal(longEval, src);
        CHECK(src.DimsUsed() > before); // source advances, never rewinds
        CHECK(harness.MutableStats().Proposals == 1);
        CHECK(harness.MutableStats().Accepts == 0); // g=2 vs f=8, rejected

        PrimarySampleVector& st = harness.MutableState();
        CHECK(st.Size() == 2);                 // long dims discarded
        CHECK(Approx(st.ValueAt(0), 0.9f));    // accepted values restored
        CHECK(Approx(st.ValueAt(1), 0.5f));
        CHECK(st.GlobalGeneration() == 0u);    // generation restored

        // A following short proposal reads the untouched original dimensions.
        FixedEvaluator shortEval(2, 2.f, Vec2f(1.f, 6.f));
        std::vector<float> mut2;
        for(int i = 0; i < 16; ++i) mut2.push_back(0.1f);
        VectorStream src2(mut2); // 0.1 < 0.5 -> large step, then accept
        harness.RunOneProposal(shortEval, src2);
        CHECK(harness.MutableStats().Accepts == 1);
        CHECK(st.Size() == 2);
        CHECK(st.GlobalGeneration() == 1u);
        CHECK(st.GenerationAt(0) == 1u && st.GenerationAt(1) == 1u);
    }

    // ---- Dual-state settlement with hand-computed weights -----------------
    {
        Scene scene; SetupCornell(scene, Vec2i(16, 12));
        MLTOptions opts(100, 0.5f, 0.1f);
        TestHarness harness(scene, opts);
        const float b = 4.f;
        harness.SetNormalization(b);
        // f = 8 at pixel (14,6); proposal g = 2 lands at pixel (1,6).
        harness.SeedState(std::vector<float>{0.9f, 0.5f},
            Vec2f(14.f, 6.f), Vec3f(8.f), 8.f);
        // Fixed g = 2 at pixel (1,6) regardless of mutated sample values.
        FixedEvaluator eval(2, 2.f, Vec2f(1.f, 6.f));

        std::vector<float> mut;
        for(int i = 0; i < 16; ++i) mut.push_back(0.9f); // small, reject
        VectorStream src(mut);
        harness.RunOneProposal(eval, src);

        // a = min(1, 2/8) = 0.25.
        // Current pixel gets b*(1-a) = 3 (chromaticity is (1,1,1)).
        // Proposed pixel gets b*a = 1.
        Framebuffer fb;
        harness.SnapshotFramebuffer(fb);
        CHECK(Approx(fb.Pixel(14, 6).x, 3.f, 1e-5f));
        CHECK(Approx(fb.Pixel(1, 6).x, 1.f, 1e-5f));
        // The rejected proposal is still settled in full, and the current
        // state remains the f = 8 path.
        CHECK(Approx(harness.MutableState().ValueAt(0), 0.9f));
        CHECK(harness.MutableStats().Proposals == 1);
    }
}

//////////////////////////////////////////////////////////////////////////
// transport: real rendering with occlusion, specular surfaces and a small
// area light, compared against ordinary path tracing, plus control cases.

CASE(transport)
{
    const int W = 32, H = 32;
    const int iterations = 120;
    Scene scene; SetupCornell(scene, Vec2i(W, H));

    // Reference path tracing, independent samples per pixel.
    PathTracer pt(scene, 4242);
    pt.mMinPathLength = 0;
    pt.mMaxPathLength = 5;
    for(int i = 0; i < iterations; ++i)
        pt.RunIteration(i);
    Framebuffer fbPt;
    pt.GetFramebuffer(fbPt);
    CHECK(AllFinite(fbPt));
    const float ptLum = fbPt.TotalLuminance();
    CHECK(ptLum > 0.f);

    // MLT, run as several batches on one persistent instance.
    MLTOptions opts(20000, 0.15f, 0.02f);
    MLTRenderer mltBatch(scene, opts, 9001);
    mltBatch.mMinPathLength = 0;
    mltBatch.mMaxPathLength = 5;
    const int batches = 4, perBatch = iterations / batches;
    for(int k = 0; k < batches; ++k)
        for(int i = 0; i < perBatch; ++i)
            mltBatch.RunIteration(i);
    Framebuffer fbMltBatch;
    mltBatch.GetFramebuffer(fbMltBatch);
    CHECK(AllFinite(fbMltBatch));

    // A second, independent instance with the same seed and one continuous
    // run must reproduce the batched instance exactly: no shared state.
    MLTRenderer mltOne(scene, opts, 9001);
    mltOne.mMinPathLength = 0;
    mltOne.mMaxPathLength = 5;
    for(int i = 0; i < batches * perBatch; ++i)
        mltOne.RunIteration(i);
    Framebuffer fbMltOne;
    mltOne.GetFramebuffer(fbMltOne);
    CHECK(AllFinite(fbMltOne));
    for(int y = 0; y < H; ++y)
        for(int x = 0; x < W; ++x)
        {
            CHECK(fbMltOne.Pixel(x, y).x == fbMltBatch.Pixel(x, y).x);
            CHECK(fbMltOne.Pixel(x, y).y == fbMltBatch.Pixel(x, y).y);
            CHECK(fbMltOne.Pixel(x, y).z == fbMltBatch.Pixel(x, y).z);
        }
    CHECK(mltOne.Stats().Proposals == mltBatch.Stats().Proposals);
    CHECK(mltOne.Stats().Normalization == mltBatch.Stats().Normalization);

    const float mltLum = fbMltOne.TotalLuminance();
    CHECK(mltLum > 0.f);

    // Normalization invariant: every proposal deposits exactly b units of
    // scalar luminance in total (the (1-a)F/f and aG/g terms), so one
    // iteration of N = W*H proposals must total N*b, independent of
    // resolution.  This is what keeps exposure constant with resolution.
    {
        MLTOptions optsNorm(4000, 0.2f, 0.02f);
        MLTRenderer mltNorm(scene, optsNorm, 2468);
        mltNorm.mMaxPathLength = 5;
        mltNorm.RunIteration(0);
        Framebuffer fbNorm;
        mltNorm.GetFramebuffer(fbNorm); // one iteration: no averaging scale
        const float expected = float(W * H) * mltNorm.Stats().Normalization;
        CHECK(Approx(fbNorm.TotalLuminance(), expected, 1e-4f));
        // Equivalently, mean per-pixel luminance equals b (the exposure),
        // which is independent of pixel count.
        CHECK(Approx(fbNorm.TotalLuminance() / float(W * H),
                     mltNorm.Stats().Normalization, 1e-4f));
    }

    // Exposure must not change with resolution: a 16x16 and a 32x32 render
    // converge to the same mean per-pixel luminance (their b estimates).
    {
        Scene smallScene; SetupCornell(smallScene, Vec2i(16, 16));
        MLTOptions optsRes(6000, 0.2f, 0.02f);
        MLTRenderer r16(smallScene, optsRes, 1357);
        r16.mMaxPathLength = 5;
        r16.RunIteration(0);
        Framebuffer f16; r16.GetFramebuffer(f16);
        const float mean16 = f16.TotalLuminance() / float(16 * 16);

        MLTRenderer r32(scene, optsRes, 1357);
        r32.mMaxPathLength = 5;
        r32.RunIteration(0);
        Framebuffer f32; r32.GetFramebuffer(f32);
        const float mean32 = f32.TotalLuminance() / float(32 * 32);
        CHECK(std::fabs(mean16 - mean32) / mean32 < 0.15f);
    }

    const float lumRatio = mltLum / ptLum;
    const float lumError = std::fabs(lumRatio - 1.f);

    // Spatial distribution: normalized per-pixel mass, total variation...
    float spatialL1 = 0.f;
    int ptLit = 0, mltLit = 0;
    for(int y = 0; y < H; ++y)
        for(int x = 0; x < W; ++x)
        {
            const float p = Luminance(fbPt.Pixel(x, y)) / ptLum;
            const float m = Luminance(fbMltOne.Pixel(x, y)) / mltLum;
            spatialL1 += std::fabs(p - m);
            if(p > 1e-7f) ++ptLit;
            if(m > 1e-7f) ++mltLit;
        }
    // ...and a smoother 8x8 block comparison.
    float blockL1 = 0.f;
    for(int by = 0; by < 8; ++by)
        for(int bx = 0; bx < 8; ++bx)
        {
            float bp = 0.f, bm = 0.f;
            for(int yy = 0; yy < 4; ++yy)
                for(int xx = 0; xx < 4; ++xx)
                {
                    bp += Luminance(fbPt.Pixel(bx * 4 + xx, by * 4 + yy));
                    bm += Luminance(fbMltOne.Pixel(bx * 4 + xx, by * 4 + yy));
                }
            blockL1 += std::fabs(bp / ptLum - bm / mltLum);
        }

    std::cout << "[transport] PT samples/pixel=" << iterations
              << " MLT proposals=" << mltOne.Stats().Proposals
              << " (large=" << mltOne.Stats().LargeSteps
              << ", accepted=" << mltOne.Stats().Accepts << ")"
              << " bootstrap b=" << mltOne.Stats().Normalization << "\n";
    std::cout << "[transport] total luminance PT=" << ptLum
              << " MLT=" << mltLum
              << " ratio=" << lumRatio << " error=" << lumError << "\n";
    std::cout << "[transport] spatial L1(pixel)=" << spatialL1
              << " L1(8x8 blocks)=" << blockL1
              << " lit pixels PT=" << ptLit << " MLT=" << mltLit << "\n";

    // Loose, statistically meaningful bounds: MLT with a finite bootstrap is
    // correlated, so we compare overall energy and where it lands, not pixels.
    CHECK(lumRatio > 0.6f && lumRatio < 1.7f);
    CHECK(spatialL1 < 1.0f);
    CHECK(blockL1 < 0.7f);
    CHECK(ptLit > 0 && mltLit > 0);
    CHECK(mltLit * 2 >= ptLit && ptLit * 2 >= mltLit);

    fbPt.SavePFM("mlt_pt.pfm");
    fbMltOne.SavePFM("mlt_mlt.pfm");
    fbMltBatch.SavePFM("mlt_mlt_batches.pfm");
    std::cout << "[transport] wrote mlt_pt.pfm, mlt_mlt.pfm,"
                 " mlt_mlt_batches.pfm (small " << W << "x" << H
              << " linear PFM images)\n";
    std::cout << "[transport] note: b is a mean of 20000 bootstrap paths, so"
                 " it carries finite-sample normalization bias, and MLT"
                 " proposals within a chain are correlated (hence block-level"
                 " rather than per-pixel comparison).\n";

    // ---- Control: large step probability 1 (independent re-sampling) ------
    {
        MLTOptions independent(5000, 1.f, 0.1f);
        MLTRenderer mltIndep(scene, independent, 555);
        mltIndep.mMaxPathLength = 5;
        for(int i = 0; i < 2; ++i)
            mltIndep.RunIteration(i);
        CHECK(mltIndep.Stats().LargeSteps == mltIndep.Stats().Proposals);
        CHECK(mltIndep.Stats().Proposals == 2 * W * H);
        Framebuffer fb;
        mltIndep.GetFramebuffer(fb);
        CHECK(AllFinite(fb));
        CHECK(fb.TotalLuminance() > 0.f);
    }

    // ---- Control: all-black scene stays black, never divides by zero ------
    {
        Scene black;
        black.LoadCornellBox(Vec2i(8, 8), Scene::kDefault);
        for(size_t i = 0; i < black.mLights.size(); ++i)
            delete black.mLights[i];
        black.mLights.clear();
        black.mMaterial2Light.clear();
        delete black.mGeometry;
        black.mGeometry = new GeometryList; // no geometry, no background

        MLTOptions optsBlack(1000, 0.25f, 0.05f);
        MLTRenderer mltBlack(black, optsBlack, 321);
        mltBlack.mMaxPathLength = 4;
        CHECK(!mltBlack.IsInitialized());
        mltBlack.RunIteration(0); // bootstrap is entirely black
        CHECK(mltBlack.Stats().Normalization == 0.f);
        mltBlack.RunIteration(1); // must keep running safely
        CHECK(mltBlack.Stats().Proposals == 2 * 8 * 8);
        CHECK(mltBlack.Stats().Accepts == 0);
        Framebuffer fb;
        mltBlack.GetFramebuffer(fb);
        CHECK(AllFinite(fb));
        CHECK(fb.TotalLuminance() == 0.f);
    }

    // ---- Control: two same-seed instances are repeatable; multi-round -----
    {
        MLTOptions optsRep(8000, 0.2f, 0.03f);
        MLTRenderer r1(scene, optsRep, 777);
        MLTRenderer r2(scene, optsRep, 777);
        r1.mMaxPathLength = 5;
        r2.mMaxPathLength = 5;
        for(int i = 0; i < 3; ++i)
        {
            r1.RunIteration(i);
            r2.RunIteration(i);
        }
        Framebuffer f1, f2;
        r1.GetFramebuffer(f1);
        r2.GetFramebuffer(f2);
        CHECK(AllFinite(f1) && AllFinite(f2));
        CHECK(f1.TotalLuminance() > 0.f);
        for(int y = 0; y < H; ++y)
            for(int x = 0; x < W; ++x)
            {
                CHECK(f1.Pixel(x, y).x == f2.Pixel(x, y).x);
                CHECK(f1.Pixel(x, y).y == f2.Pixel(x, y).y);
                CHECK(f1.Pixel(x, y).z == f2.Pixel(x, y).z);
            }
        CHECK(r1.Stats().Accepts == r2.Stats().Accepts);
        CHECK(r1.Stats().LargeSteps == r2.Stats().LargeSteps);

        // A fourth round on r1 keeps accumulating on the same chain.
        const long long before = r1.Stats().Proposals;
        r1.RunIteration(3);
        CHECK(r1.Stats().Proposals == before + W * H);
    }
}
