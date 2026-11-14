// Tests for the primary sample space Metropolis renderer (src/mlt.hxx).
//
// Three case groups, registered with the check.hpp CASE macro (the test
// driver in task_support/driver.cpp supplies main):
//   replay    - deterministic replay of one camera path from an explicit
//               stream of uniform primary samples
//   chain     - hand-checkable Metropolis mechanics: acceptance rule,
//               transactional reject/accept, dimension growth, wrapping,
//               dual-state settlement, option validation
//   transport - end-to-end rendering of an occluded, specular scene with a
//               small area light, compared against plain path tracing
//
// The tests write small PFM images into $TMPDIR and print every sample size
// and error metric they use.  Remember when reading the metrics:
//   * b is only an arithmetic mean over a finite bootstrap, so the MLT image
//     normalization carries bootstrap error (it is not claimed unbiased);
//   * successive MLT iterations are correlated samples of one chain, so the
//     effective sample count is lower than the raw proposal count.

#include "check.hpp"
#include "mlt.hxx"
#include "pathtracer.hxx"
#include "scene.hxx"
#include "framebuffer.hxx"

#include <random>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <limits>
#include <cstdio>

namespace
{

const float kEps = 1e-6f;

bool Close(float a, float b, float tol = 1e-5f)
{
    return std::fabs(a - b) <= tol * (1.f + std::fabs(a) + std::fabs(b));
}

bool SameColor(const Vec3f& a, const Vec3f& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Builds the shared transport scene: Cornell box with the small ceiling
// area light and the mirror + glass spheres (occlusion and specular paths).
void BuildLitScene(Scene& oScene, const Vec2i& aRes)
{
    oScene.LoadCornellBox(aRes,
        Scene::kLightCeiling | Scene::kBothSmallSpheres);
    oScene.BuildSceneSphere();
}

std::string TmpPath(const char* aName)
{
    const char* dir = std::getenv("TMPDIR");
    std::string base = (dir != nullptr && dir[0] != '\0') ? dir : "/tmp";
    base += '/';
    base += aName;
    return base;
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// replay: one primary sample vector <-> one raster and one linear RGB value

CASE(replay)
{
    const Vec2i res(12, 10);
    Scene scene;
    BuildLitScene(scene, res);

    MLTRenderer renderer(scene, MLTOptions(500, 0.3f, 0.1f), 909);
    renderer.mMaxPathLength = 4;
    renderer.mMinPathLength = 0;

    // Independent uniform stream used to build an explicit primary vector.
    std::mt19937_64 rng(2024);
    std::uniform_real_distribution<float> dist(0.f, 1.f);
    std::vector<float> samples(40);
    for(size_t i = 0; i < samples.size(); ++i)
        samples[i] = dist(rng);

    // Entry under test: full vector, raster comes from components 0 and 1.
    const PathResult a = renderer.EvaluatePath(samples);
    const PathResult b = renderer.EvaluatePath(samples);

    // Same primary vector must replay to the identical raster and linear RGB.
    CHECK(SameColor(a.mColor, b.mColor));
    CHECK(a.mRaster.x == b.mRaster.x && a.mRaster.y == b.mRaster.y);
    CHECK(a.mDimsUsed == b.mDimsUsed);
    CHECK(a.mDimsUsed >= 2 && a.mDimsUsed <= int(samples.size()));

    // Raster mapping is exactly (u0*width, u1*height) and comes first.
    CHECK(Close(a.mRaster.x, samples[0] * res.x));
    CHECK(Close(a.mRaster.y, samples[1] * res.y));

    // Independent replay through the public stream hook with its own
    // SequenceUniformStream must give the real single-path result.
    SequenceUniformStream seq(samples);
    const Vec2f raster(seq.GetFloat() * res.x, seq.GetFloat() * res.y);
    const PathResult c = MLTRenderer::EvaluatePathOnStream(
        scene, 0, 4, raster, seq);
    CHECK(Close(raster.x, a.mRaster.x));
    CHECK(Close(raster.y, a.mRaster.y));
    CHECK(SameColor(c.mColor, a.mColor));
    CHECK(int(seq.Position()) == a.mDimsUsed);

    // Touching only later dimensions leaves the raster untouched.
    std::vector<float> moved = samples;
    for(size_t i = 2; i < moved.size(); ++i)
        moved[i] = dist(rng);
    const PathResult d = renderer.EvaluatePath(moved);
    CHECK(Close(d.mRaster.x, samples[0] * res.x));
    CHECK(Close(d.mRaster.y, samples[1] * res.y));

    // A primary sampler adopted on the same vector must drive the shared
    // path evaluation through identical dimensions in identical order.
    PrimarySampler adopted(123);
    adopted.AdoptState(samples);
    adopted.ResetCursor();
    const Vec2f adoptedRaster(adopted.NextSample() * res.x,
                              adopted.NextSample() * res.y);
    const PathResult e = MLTRenderer::EvaluatePathOnStream(
        scene, 0, 4, adoptedRaster, adopted);
    CHECK(SameColor(e.mColor, a.mColor));
    CHECK(Close(adoptedRaster.x, a.mRaster.x));
    CHECK(Close(adoptedRaster.y, a.mRaster.y));
    CHECK(int(adopted.DimensionCount()) >= a.mDimsUsed);

    // A vector that runs out mid-path must throw instead of overrunning:
    // short path first, then a deliberately truncated vector.
    bool exhausted = false;
    try
    {
        std::vector<float> tiny(1, 0.5f); // raster needs two components
        renderer.EvaluatePath(tiny);
    }
    catch(const std::out_of_range&)
    {
        exhausted = true;
    }
    CHECK(exhausted);

    // The shared evaluation, driven by the project RNG, reproduces the
    // PathTracer per-pixel loop exactly (same pixel order, same RNG stream).
    const Vec2i smallRes(8, 6);
    Scene ptScene;
    BuildLitScene(ptScene, smallRes);

    PathTracer pt(ptScene, 321);
    pt.mMaxPathLength = 4;
    pt.RunIteration(0);
    Framebuffer ptFb;
    pt.GetFramebuffer(ptFb);

    Rng replayRng(321);
    RngUniformStream replayStream(replayRng);
    Framebuffer manualFb;
    manualFb.Setup(Vec2f(float(smallRes.x), float(smallRes.y)));
    for(int pix = 0; pix < smallRes.x * smallRes.y; ++pix)
    {
        const int x = pix % smallRes.x;
        const int y = pix / smallRes.x;
        const Vec2f sample = Vec2f(float(x), float(y)) +
            replayStream.GetVec2f();
        const PathResult r = MLTRenderer::EvaluatePathOnStream(
            ptScene, 0, 4, sample, replayStream);
        manualFb.AddColor(sample, r.mColor);
    }
    CHECK(manualFb.ResX() == ptFb.ResX() && manualFb.ResY() == ptFb.ResY());
    for(int i = 0; i < smallRes.x * smallRes.y; ++i)
        CHECK(SameColor(manualFb.ColorData()[i], ptFb.ColorData()[i]));
}

//////////////////////////////////////////////////////////////////////////
// chain: Metropolis mechanics with hand-computable quantities

namespace
{

// Three-valued, two-constant-segment plus dark region "luminance" of a
// primary vector, so every acceptance probability can be computed by hand.
float ToyLuminance(const std::vector<float>& aState)
{
    const float x = aState[0];
    if(x < 0.34f) return 0.f;
    if(x < 0.67f) return 1.f;
    return 4.f;
}

} // namespace

CASE(chain)
{
    // ---- acceptance rule, all boundary cases ---------------------------
    CHECK(Close(MLTRenderer::AcceptanceProbability(2.f, 1.f), 0.5f));
    CHECK(Close(MLTRenderer::AcceptanceProbability(1.f, 2.f), 1.f));
    CHECK(Close(MLTRenderer::AcceptanceProbability(4.f, 1.f), 0.25f));
    CHECK(Close(MLTRenderer::AcceptanceProbability(0.f, 3.f), 1.f));
    CHECK(Close(MLTRenderer::AcceptanceProbability(0.f, 0.f), 0.f));

    // ---- modulo-1 wrapping and the symmetric small-step kernel ----------
    CHECK(Close(WrapUnit(1.25f), 0.25f));
    CHECK(Close(WrapUnit(-0.25f), 0.75f));
    CHECK(Close(WrapUnit(0.5f), 0.5f));
    CHECK(Close(SmallStepValue(0.95f, 0.0f, 0.5f), 0.45f));  // wrap down
    CHECK(Close(SmallStepValue(0.90f, 0.9f, 0.5f), 0.30f));  // wrap up
    CHECK(Close(SmallStepValue(0.40f, 0.5f, 0.25f), 0.40f)); // zero offset

    // ---- option validation ----------------------------------------------
    Scene dummy;
    dummy.LoadCornellBox(Vec2i(4, 4),
        Scene::kLightCeiling | Scene::kBothSmallSpheres);
    auto expectThrow = [&](int aBoot, float aLarge, float aWidth)
    {
        bool threw = false;
        try
        {
            MLTRenderer r(dummy, MLTOptions(aBoot, aLarge, aWidth));
        }
        catch(const std::invalid_argument&)
        {
            threw = true;
        }
        CHECK(threw);
    };
    expectThrow(0,    0.3f, 0.1f);
    expectThrow(-7,   0.3f, 0.1f);
    expectThrow(100,  0.f,  0.1f);
    expectThrow(100, -0.1f, 0.1f);
    expectThrow(100,  1.01f, 0.1f);
    expectThrow(100,  std::numeric_limits<float>::infinity(), 0.1f);
    expectThrow(100,  std::numeric_limits<float>::quiet_NaN(), 0.1f);
    expectThrow(100,  0.3f, 0.f);
    expectThrow(100,  0.3f, 0.51f);
    expectThrow(100,  0.3f, std::numeric_limits<float>::quiet_NaN());
    // Boundary values are legal.
    MLTRenderer valid(dummy, MLTOptions(100, 1.f, 0.5f));
    (void)valid;

    // ---- rejected long path rolls back freshly grown dimensions ---------
    // Hand-driven transaction: adopt a short (2-dim) state, open a long
    // (8-dim) proposal, then reject and verify the six new dimensions and
    // all generation stamps disappear.
    {
        PrimarySampler sampler(424242);
        const std::vector<float> shortState = {0.99f, 0.2f};
        sampler.AdoptState(shortState);
        const uint64_t genBefore = sampler.Generation();

        sampler.BeginMutation(0.f, 0.5f); // guaranteed small step
        sampler.ResetCursor();
        std::vector<float> longProposal;
        for(int k = 0; k < 8; ++k)
            longProposal.push_back(sampler.NextSample());
        CHECK(sampler.DimensionCount() == 8);
        CHECK(longProposal.size() == 8);

        sampler.NextUniform(); // the accept/reject draw, always consumed
        sampler.RejectMutation();

        CHECK(sampler.DimensionCount() == 2);
        CHECK(sampler.Generation() == genBefore);
        CHECK(sampler.Values() == shortState);
    }

    // ---- wrap-around is really applied by the sampler -------------------
    // State at 0.99 with width 0.5: a non-wrapped small step stays in
    // [0.49,1), so a value in [0,0.49) can only come from a modulo-1 wrap.
    {
        int attempts = 0;
        bool wrapped = false;
        while(attempts < 50 && !wrapped)
        {
            PrimarySampler s(1000 + attempts);
            s.AdoptState(std::vector<float>{0.99f});
            s.BeginMutation(0.f, 0.5f); // guaranteed small step
            s.ResetCursor();
            const float v = s.NextSample();
            CHECK(v >= 0.f && v < 1.f);
            if(v < 0.49f)
            {
                // Recover the offset RND that the symmetric kernel used and
                // check it lies in the positive-offset (rnd > 0.5) half.
                const float rnd = v + 0.51f; // inverse of the wrap mapping
                CHECK(rnd > 0.5f && rnd < 1.f);
                CHECK(Close(v, SmallStepValue(0.99f, rnd, 0.5f)));
                wrapped = true;
            }
            s.NextUniform();           // decision draw, always consumed
            s.RejectMutation();
            CHECK(s.Values() == std::vector<float>{0.99f});
            ++attempts;
        }
        CHECK(wrapped);
    }

    // ---- dark current state: f = 0 settlement, no 0/0 -------------------
    // A chain can be initialized on a dark primary vector; the acceptance
    // rule and the two splat terms must stay finite.
    {
        const std::vector<float> darkState = {0.1f, 0.7f}; // ToyLuminance 0
        int attempts = 0;
        bool reachedBright = false;
        while(attempts < 50 && !reachedBright)
        {
            PrimarySampler dark(31 + attempts);
            dark.AdoptState(darkState);
            dark.BeginMutation(1.f, 0.1f); // force a large step
            dark.ResetCursor();
            std::vector<float> proposal;
            for(int k = 0; k < 2; ++k)
                proposal.push_back(dark.NextSample());
            const float g = ToyLuminance(proposal);
            const float a = MLTRenderer::AcceptanceProbability(0.f, g);
            if(g > 0.f)
            {
                CHECK(a == 1.f); // f == 0, g > 0 always accepts
                // Rejected term is zero by the guard; accepted term is b/g.
                const float rejectTerm = 0.f;
                const float acceptTerm = 2.5f * a / g;
                CHECK(rejectTerm == 0.f);
                CHECK(Close(acceptTerm, 2.5f / g));
                dark.NextUniform(); // decision draw (< 1, so it accepts)
                dark.AcceptMutation();
                CHECK(ToyLuminance(dark.Values()) == g);
                reachedBright = true;
            }
            else
            {
                CHECK(a == 0.f); // both zero: reject, both deposits zero
                dark.NextUniform();
                dark.RejectMutation();
                CHECK(dark.Values() == darkState);
            }
            ++attempts;
        }
        CHECK(reachedBright);
    }

    // ---- bright -> dark proposal always rejects, dual state settled -----
    {
        int attempts = 0;
        bool gotDarkProposal = false;
        while(attempts < 100 && !gotDarkProposal)
        {
            PrimarySampler s(909 + attempts);
            s.AdoptState(std::vector<float>{0.5f}); // luminance 1
            s.BeginMutation(0.f, 0.5f);
            s.ResetCursor();
            const float x = s.NextSample();
            if(ToyLuminance(std::vector<float>{x}) == 0.f)
            {
                const float a = MLTRenderer::AcceptanceProbability(1.f, 0.f);
                CHECK(a == 0.f);
                // Any decision draw loses against probability 0.
                const bool accept = s.NextUniform() < a;
                CHECK(accept == false);
                // Both terms are still settled: rejected b, accepted zero.
                const float rejectTerm = 2.5f * (1.f - a);
                const float acceptTerm = 0.f;
                CHECK(Close(rejectTerm, 2.5f));
                CHECK(acceptTerm == 0.f);
                s.RejectMutation();
                CHECK(s.Values() == std::vector<float>{0.5f});
                gotDarkProposal = true;
            }
            else
            {
                s.NextUniform();
                s.RejectMutation();
                CHECK(s.Values() == std::vector<float>{0.5f});
            }
            ++attempts;
        }
        CHECK(gotDarkProposal);
    }

    // ---- running chain: deposit conservation and short/long rollbacks ---
    // The chain only ever holds positive-luminance states (zero proposals
    // reject), so every proposal deposits exactly b in total.
    PrimarySampler sampler(424242);
    const std::vector<float> initial = {0.99f, 0.2f};
    sampler.AdoptState(initial);

    const float b = 2.5f;
    std::vector<float> current = initial;
    uint64_t acceptedSteps = 0;
    uint64_t rejectedLongRollbacks = 0;
    double depositSum = 0.0;

    const int steps = 400;
    for(int step = 0; step < steps; ++step)
    {
        const bool longPath = (step % 2 == 1); // alternate short/long paths
        const int  dims     = longPath ? 8 : 2;

        const std::vector<float> before = sampler.Values();
        const uint64_t genBefore = sampler.Generation();

        sampler.BeginMutation(0.25f, 0.5f); // width 0.5 is the allowed limit
        sampler.ResetCursor();

        std::vector<float> proposed;
        for(int k = 0; k < dims; ++k)
        {
            const float v = sampler.NextSample();
            CHECK(v >= 0.f && v < 1.f);
            proposed.push_back(v);
        }

        const float f = ToyLuminance(current);
        const float g = ToyLuminance(proposed);
        CHECK(f > 0.f); // invariant: a held chain state is never dark
        const float a = MLTRenderer::AcceptanceProbability(f, g);

        // Dual-state settlement; F/f and G/g are 1 on the constant segments.
        const float rejectDeposit = b * (1.f - a);
        const float acceptDeposit = (g > 0.f) ? b * a : 0.f;
        CHECK(Close(rejectDeposit + acceptDeposit, b, 1e-5f));
        depositSum += double(rejectDeposit) + double(acceptDeposit);

        const bool accept = sampler.NextUniform() < a;
        if(accept)
        {
            sampler.AcceptMutation();
            current = proposed;
            ++acceptedSteps;
            // Dimensions only grow: a later short path keeps the dimensions
            // a previously accepted long path introduced.
            CHECK(sampler.DimensionCount() ==
                  std::max(before.size(), proposed.size()));
        }
        else
        {
            sampler.RejectMutation();
            // Full rollback: values, dimension count and generation stamps.
            CHECK(sampler.DimensionCount() == before.size());
            CHECK(sampler.Generation() == genBefore);
            CHECK(sampler.Values() == before);
            if(longPath) ++rejectedLongRollbacks;
        }
    }
    CHECK(rejectedLongRollbacks > 0);
    CHECK(acceptedSteps > 0);
    CHECK(Close(float(depositSum), b * float(steps), 1e-2f));

    // ---- lazy dimensions keep a uniform marginal ------------------------
    // Every mutation grows the used dimension count by one, so each step
    // introduces exactly one lazily sampled dimension.  Small and large
    // steps are mixed; the collected lazy values must stay uniform.
    {
        PrimarySampler lazy(20240711);
        lazy.AdoptState(std::vector<float>());
        const int lazyMutations = 400;
        double sum = 0.0;
        float lo = 1.f, hi = 0.f;
        for(int m = 0; m < lazyMutations; ++m)
        {
            lazy.BeginMutation(0.5f, 0.25f);
            lazy.ResetCursor();
            const size_t want = size_t(m) + 1; // one more dim each time
            float newest = 0.f;
            for(size_t k = 0; k < want; ++k)
                newest = lazy.NextSample();
            lazy.NextUniform(); // decision draw
            lazy.AcceptMutation();
            sum += newest;
            lo = std::min(lo, newest);
            hi = std::max(hi, newest);
        }
        const double mean = sum / lazyMutations;
        CHECK(mean > 0.45 && mean < 0.55);
        CHECK(lo < 0.1f && hi > 0.9f);
        CHECK(lazy.DimensionCount() == size_t(lazyMutations));
    }

    // ---- proposal RNG keeps advancing through a rejection ---------------
    PrimarySampler s1(777);
    s1.AdoptState(std::vector<float>{0.4f, 0.6f});
    std::mt19937_64 refRng(777);
    std::uniform_real_distribution<float> refDist(0.f, 1.f);
    const float u0 = refDist(refRng); // large/small decision
    const float u1 = refDist(refRng); // offset of dimension 0
    const float u2 = refDist(refRng); // offset of dimension 1
    const float u3 = refDist(refRng); // accept/reject draw
    const float u4 = refDist(refRng); // next decision after the transaction
    s1.BeginMutation(0.f, 0.1f); // guaranteed small step
    s1.ResetCursor();
    CHECK(Close(s1.NextSample(), WrapUnit(0.4f + (2.f * u1 - 1.f) * 0.1f)));
    CHECK(Close(s1.NextSample(), WrapUnit(0.6f + (2.f * u2 - 1.f) * 0.1f)));
    const bool acceptDummy = s1.NextUniform() < 0.5f; // consumes u3
    CHECK(acceptDummy == (u3 < 0.5f));
    s1.RejectMutation();
    // The rejected transaction did not rewind the proposal RNG.
    CHECK(Close(s1.NextUniform(), u4));
    (void)u0;

    // ---- large step probability 1: every proposal is a global resample ---
    PrimarySampler s2(101);
    s2.AdoptState(std::vector<float>{0.1f, 0.2f, 0.3f});
    s2.BeginMutation(1.f, 0.1f);
    s2.ResetCursor();
    CHECK(s2.WasLargeStep());
    for(int k = 0; k < 3; ++k)
    {
        const float v = s2.NextSample();
        CHECK(v >= 0.f && v < 1.f);
    }
    s2.AcceptMutation();
}

//////////////////////////////////////////////////////////////////////////
// transport: end-to-end rendering versus plain path tracing

namespace
{

float MapRmse(const Framebuffer& aA, const Framebuffer& aB)
{
    const int n = aA.ResX() * aA.ResY();
    const float totalA = std::max(aA.TotalLuminance(), kEps);
    const float totalB = std::max(aB.TotalLuminance(), kEps);
    double err = 0.0;
    for(int i = 0; i < n; ++i)
    {
        const float la = Luminance(aA.ColorData()[i]) / totalA;
        const float lb = Luminance(aB.ColorData()[i]) / totalB;
        const float d = la - lb;
        err += double(d) * double(d);
    }
    return float(std::sqrt(err / n));
}

// Distribution of normalized luminance over a coarse grid of bins; binning
// is a resolution-fair spatial comparison that tolerates single-pixel chain
// noise while still detecting energy landing in the wrong image region.
std::vector<float> BinnedEnergy(const Framebuffer& aFb, int aBinsX, int aBinsY)
{
    const int n = aFb.ResX() * aFb.ResY();
    const float total = std::max(aFb.TotalLuminance(), kEps);
    std::vector<float> bins(aBinsX * aBinsY, 0.f);
    for(int y = 0; y < aFb.ResY(); ++y)
    {
        for(int x = 0; x < aFb.ResX(); ++x)
        {
            const int bx = std::min(aBinsX - 1,
                x * aBinsX / aFb.ResX());
            const int by = std::min(aBinsY - 1,
                y * aBinsY / aFb.ResY());
            bins[bx + by * aBinsX] +=
                Luminance(aFb.ColorData()[x + y * aFb.ResX()]) / total;
        }
    }
    (void)n;
    return bins;
}

float BinRmse(const Framebuffer& aA, const Framebuffer& aB)
{
    const std::vector<float> ba = BinnedEnergy(aA, 4, 4);
    const std::vector<float> bb = BinnedEnergy(aB, 4, 4);
    double err = 0.0;
    for(size_t i = 0; i < ba.size(); ++i)
    {
        const float d = ba[i] - bb[i];
        err += double(d) * double(d);
    }
    return float(std::sqrt(err / ba.size()));
}

} // namespace

CASE(transport)
{
    const Vec2i res(16, 16);
    Scene scene;
    BuildLitScene(scene, res);

    // ---- plain path tracing reference, fixed seed, many batches ---------
    const int ptIterations = 120;
    PathTracer pt(scene, 2025);
    pt.mMinPathLength = 0;
    pt.mMaxPathLength = 4;
    for(int it = 0; it < ptIterations; ++it)
        pt.RunIteration(it);
    Framebuffer ptFb;
    pt.GetFramebuffer(ptFb);
    const float ptTotal = ptFb.TotalLuminance();
    CHECK(std::isfinite(ptTotal) && ptTotal > 0.f);

    // ---- MLT, fixed seed, run in several batches ------------------------
    // One iteration is width*height proposals; enough batches give the
    // single chain time to mix across the whole image.
    const MLTOptions options(30000, 0.25f, 1.f / 16.f);
    const int mltIterations = 40;

    MLTRenderer mlt(scene, options, 2025);
    mlt.mMinPathLength = 0;
    mlt.mMaxPathLength = 4;
    for(int it = 0; it < mltIterations; ++it)
        mlt.RunIteration(it);
    Framebuffer mltFb;
    mlt.GetFramebuffer(mltFb);
    const float mltTotal = mltFb.TotalLuminance();

    const MLTStats stats = mlt.Stats();
    CHECK(stats.mProposals == uint64_t(mltIterations) * res.x * res.y);
    CHECK(stats.mLargeSteps <= stats.mProposals);
    CHECK(stats.mAccepted <= stats.mProposals);
    CHECK(std::isfinite(stats.mBootstrapConstant) &&
          stats.mBootstrapConstant > 0.f);
    CHECK(std::isfinite(mltTotal) && mltTotal > 0.f);

    const float relLumError =
        std::fabs(mltTotal - ptTotal) / std::max(ptTotal, kEps);
    const float rmse = MapRmse(mltFb, ptFb);
    const float binRmse = BinRmse(mltFb, ptFb);

    std::cout << "TRANSPORT samples: PT "
              << (ptIterations * res.x * res.y)
              << " paths, MLT " << stats.mProposals
              << " proposals (bootstrap " << options.BootstrapSamples
              << "), large steps " << stats.mLargeSteps
              << ", accepted " << stats.mAccepted << "\n";
    std::cout << "TRANSPORT b=" << stats.mBootstrapConstant
              << " PT_total_lum=" << ptTotal
              << " MLT_total_lum=" << mltTotal
              << " rel_lum_error=" << relLumError
              << " normalized_pixel_rmse=" << rmse
              << " binned_4x4_rmse=" << binRmse << "\n";

    // Exposure must not depend on resolution or proposal count: one MLT
    // iteration deposits N*b in total, matching one PT iteration of N
    // per-pixel estimates of mean luminance b.  The remaining luminance
    // error is the finite-bootstrap error of b, which is explicitly allowed.
    CHECK(relLumError < 0.35f);
    // Exact deposit bookkeeping: every proposal deposits luminance b, so the
    // iteration-averaged image totals N*b regardless of proposal count.
    const float expectedTotal = float(res.x * res.y) *
                                stats.mBootstrapConstant;
    CHECK(std::fabs(mltTotal - expectedTotal) / expectedTotal < 1e-3f);
    // The 4x4 spatial bins must agree on where the energy lands; the
    // per-pixel RMSE is reported but not hard-gated because a single chain
    // produces correlated, spiky per-pixel estimates.
    CHECK(binRmse < 0.05f);
    for(int i = 0; i < res.x * res.y; ++i)
    {
        const Vec3f& c = mltFb.ColorData()[i];
        CHECK(std::isfinite(c.x) && std::isfinite(c.y) &&
              std::isfinite(c.z));
        CHECK(c.x >= 0.f && c.y >= 0.f && c.z >= 0.f);
    }

    const std::string mltFile = TmpPath("mlt_transport.pfm");
    const std::string ptFile  = TmpPath("pt_reference.pfm");
    mltFb.SavePFM(mltFile.c_str());
    ptFb.SavePFM(ptFile.c_str());
    std::cout << "TRANSPORT PFM written: " << mltFile << " , " << ptFile
              << "\n";

    // ---- two instances with the same seed are bit-for-bit repeatable ----
    MLTRenderer mltA(scene, options, 77);
    MLTRenderer mltB(scene, options, 77);
    mltA.mMaxPathLength = 4;
    mltB.mMaxPathLength = 4;
    for(int it = 0; it < 3; ++it)
    {
        mltA.RunIteration(it);
        mltB.RunIteration(it);
    }
    Framebuffer fbA, fbB;
    mltA.GetFramebuffer(fbA);
    mltB.GetFramebuffer(fbB);
    CHECK(fbA.TotalLuminance() > 0.f);
    for(int i = 0; i < res.x * res.y; ++i)
        CHECK(SameColor(fbA.ColorData()[i], fbB.ColorData()[i]));
    CHECK(mltA.Stats().mProposals == mltB.Stats().mProposals);
    CHECK(Close(mltA.Stats().mBootstrapConstant,
                mltB.Stats().mBootstrapConstant));

    // ---- consecutive iterations keep accumulating on one persistent chain
    MLTRenderer mltC(scene, options, 77);
    mltC.mMaxPathLength = 4;
    // b is created lazily during the first iteration and then frozen.
    mltC.RunIteration(0);
    const float bAfterOne = mltC.Stats().mBootstrapConstant;
    CHECK(bAfterOne > 0.f);
    mltC.RunIteration(1);
    mltC.RunIteration(2);
    const MLTStats statsC = mltC.Stats();
    CHECK(statsC.mProposals == uint64_t(3) * res.x * res.y);
    CHECK(statsC.mBootstrapConstant == bAfterOne);
    Framebuffer fbC;
    mltC.GetFramebuffer(fbC);
    CHECK(std::isfinite(fbC.TotalLuminance()) && fbC.TotalLuminance() > 0.f);
    // Same seed + same iteration count reproduces the same image again.
    for(int i = 0; i < res.x * res.y; ++i)
        CHECK(SameColor(fbC.ColorData()[i], fbA.ColorData()[i]));

    // ---- large step probability 1: every proposal is a large step --------
    MLTOptions largeOnly(20000, 1.f, 0.25f);
    MLTRenderer mltLarge(scene, largeOnly, 4242);
    mltLarge.mMaxPathLength = 4;
    mltLarge.RunIteration(0);
    const MLTStats largeStats = mltLarge.Stats();
    CHECK(largeStats.mLargeSteps == largeStats.mProposals);
    CHECK(largeStats.mAccepted > 0);
    Framebuffer fbLarge;
    mltLarge.GetFramebuffer(fbLarge);
    CHECK(std::isfinite(fbLarge.TotalLuminance()) &&
          fbLarge.TotalLuminance() > 0.f);

    // ---- exposure is independent of resolution --------------------------
    // A different resolution, one iteration: the mean per-pixel luminance
    // must still be exactly b (image total / pixel count), so changing the
    // resolution or the per-iteration proposal count never rescales exposure.
    {
        const Vec2i res2(12, 12);
        Scene scene2;
        BuildLitScene(scene2, res2);
        MLTRenderer mlt2(scene2, MLTOptions(20000, 0.25f, 1.f / 16.f), 2025);
        mlt2.mMaxPathLength = 4;
        mlt2.RunIteration(0);
        Framebuffer fb2;
        mlt2.GetFramebuffer(fb2);
        const int n2 = res2.x * res2.y;
        const float meanPixel = fb2.TotalLuminance() / float(n2);
        CHECK(std::fabs(meanPixel - mlt2.Stats().mBootstrapConstant) /
                  std::max(mlt2.Stats().mBootstrapConstant, kEps) < 1e-3f);
        CHECK(mlt2.Stats().mProposals == uint64_t(n2));
    }

    std::cout << "TRANSPORT notes: b is the arithmetic mean luminance of a\n"
              << "  finite bootstrap (" << options.BootstrapSamples
              << " independent paths), so the image normalization carries\n"
              << "  finite-sample bootstrap error and is not unbiased; MLT\n"
              << "  iterations are states of one Markov chain, hence\n"
              << "  correlated -- the effective sample size is below the raw\n"
              << "  proposal count and convergence is read from the binned\n"
              << "  spatial error rather than per-pixel variance.\n";

    // ---- all-black scene: zero b, no division by zero, safe to continue --
    Scene black;
    black.LoadCornellBox(Vec2i(8, 8), 0); // no lights at all
    black.BuildSceneSphere();
    CHECK(black.GetLightCount() == 0);
    MLTRenderer mltBlack(black, MLTOptions(2000, 0.3f, 0.1f), 55);
    mltBlack.mMaxPathLength = 4;
    mltBlack.RunIteration(0);
    mltBlack.RunIteration(1);
    const MLTStats blackStats = mltBlack.Stats();
    CHECK(blackStats.mBootstrapConstant == 0.f);
    CHECK(blackStats.mAccepted == 0);
    CHECK(blackStats.mProposals == uint64_t(2) * 8 * 8);
    Framebuffer fbBlack;
    mltBlack.GetFramebuffer(fbBlack);
    CHECK(fbBlack.TotalLuminance() == 0.f);
    for(int i = 0; i < 8 * 8; ++i)
        CHECK(SameColor(fbBlack.ColorData()[i], Vec3f(0.f)));
}
