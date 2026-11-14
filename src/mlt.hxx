/*
 * Copyright (C) 2012, Tomas Davidovic (http://www.davidovic.cz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * (The above is MIT License: http://en.wikipedia.org/wiki/MIT_License)
 */

// Path-space evaluation shared with the PathTracer plus a primary sample
// space Metropolis light transport renderer (PSSMLT, Kelemen-style).
//
// The camera path evaluation is the exact PathTracer loop (camera ray,
// emission hit, next event estimation with MIS, BSDF continuation, Russian
// roulette) with every random decision taken from a sequential stream of
// uniform numbers, so that one primary sample vector always maps to the same
// raster position and linear RGB result.  No already generated image is ever
// resampled: the Metropolis chain mutates primary samples only.

#ifndef __MLT_HXX__
#define __MLT_HXX__

#include <vector>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <random>
#include <algorithm>
#include "renderer.hxx"
#include "bsdf.hxx"
#include "rng.hxx"
#include "utils.hxx"

//////////////////////////////////////////////////////////////////////////
// Sequential streams of uniform random numbers in [0,1).
//
// Every random number consumed by a camera path must come through one of
// these.  This is what guarantees that a fixed primary sample vector replays
// deterministically: renderers never touch a hidden RNG inside the path code.

class UniformStream
{
public:
    virtual ~UniformStream() {}
    virtual float GetFloat() = 0;

    Vec2f GetVec2f()
    {
        const float a = GetFloat();
        const float b = GetFloat();
        return Vec2f(a, b);
    }

    Vec3f GetVec3f()
    {
        const float a = GetFloat();
        const float b = GetFloat();
        const float c = GetFloat();
        return Vec3f(a, b, c);
    }
};

// Adapts the project Rng (used by the original PathTracer).
class RngUniformStream : public UniformStream
{
public:
    explicit RngUniformStream(Rng& aRng) : mRng(aRng) {}
    virtual float GetFloat() { return mRng.GetFloat(); }

private:
    Rng& mRng;
};

// Replays an explicit sequence of uniforms.  Running past the end throws,
// so a short path followed by a long path can never silently overrun.
class SequenceUniformStream : public UniformStream
{
public:
    explicit SequenceUniformStream(const std::vector<float>& aSamples)
        : mSamples(aSamples), mIndex(0)
    {}

    virtual float GetFloat()
    {
        if(mIndex >= mSamples.size())
            throw std::out_of_range("SequenceUniformStream: sample vector exhausted");
        return mSamples[mIndex++];
    }

    size_t Position() const { return mIndex; }

private:
    const std::vector<float>& mSamples;
    size_t                    mIndex;
};

//////////////////////////////////////////////////////////////////////////
// Result of evaluating one camera path.

struct PathResult
{
    Vec3f mColor;      //!< linear radiance estimate, same units as PT output
    Vec2f mRaster;     //!< sub-pixel raster position the camera ray used
    int   mDimsUsed;   //!< number of primary samples consumed (incl. raster)
};

// MIS weight for 2 pdfs (balance heuristic), identical to PathTracer's Mis2.
inline float MltMis2(float aSamplePdf, float aOtherPdf)
{
    return aSamplePdf / (aSamplePdf + aOtherPdf);
}

// Shared single camera path evaluation.
//
// This is the body of the original PathTracer pixel loop, unchanged in
// semantics: camera ray generation from aRaster, background/emission hits,
// next event estimation (direct light sampling with MIS), BSDF sampled
// continuation and Russian roulette, honoring the inherited path length
// limits.  All random numbers are read from aStream in execution order.
inline PathResult EvaluateCameraPath(
    const Scene&   aScene,
    uint           aMinPathLength,
    uint           aMaxPathLength,
    const Vec2f&   aRaster,
    UniformStream& aStream)
{
    // We sample lights uniformly
    const int   lightCount    = aScene.GetLightCount();
    const float lightPickProb = lightCount > 0 ? 1.f / lightCount : 0.f;

    Ray   ray = aScene.mCamera.GenerateRay(aRaster);
    Isect isect;
    isect.dist = 1e36f;

    Vec3f pathWeight(1.f);
    Vec3f color(0.f);
    uint  pathLength   = 1;
    bool  lastSpecular = true;
    float lastPdfW     = 1;

    for(;; ++pathLength)
    {
        if(!aScene.Intersect(ray, isect))
        {
            if(pathLength < aMinPathLength)
                break;

            const BackgroundLight* background = aScene.GetBackground();
            if(!background)
                break;
            // For background we cheat with the A/W suffixes,
            // and GetRadiance actually returns W instead of A
            float directPdfW;
            Vec3f contrib = background->GetRadiance(aScene.mSceneSphere,
                ray.dir, Vec3f(0), &directPdfW);
            if(contrib.IsZero())
                break;

            float misWeight = 1.f;
            if(pathLength > 1 && !lastSpecular)
            {
                misWeight = MltMis2(lastPdfW, directPdfW * lightPickProb);
            }

            color += pathWeight * misWeight * contrib;
            break;
        }

        Vec3f hitPoint = ray.org + ray.dir * isect.dist;
        isect.dist += EPS_RAY;

        BSDF<false> bsdf(ray, isect, aScene);
        if(!bsdf.IsValid())
            break;

        // directly hit some light, lights do not reflect
        if(isect.lightID >= 0)
        {
            if(pathLength < aMinPathLength)
                break;

            const AbstractLight *light = aScene.GetLightPtr(isect.lightID);
            float directPdfA;
            Vec3f contrib = light->GetRadiance(aScene.mSceneSphere,
                ray.dir, hitPoint, &directPdfA);
            if(contrib.IsZero())
                break;

            float misWeight = 1.f;
            if(pathLength > 1 && !lastSpecular)
            {
                const float directPdfW = PdfAtoW(directPdfA, isect.dist,
                    bsdf.CosThetaFix());
                misWeight = MltMis2(lastPdfW, directPdfW * lightPickProb);
            }

            color += pathWeight * misWeight * contrib;
            break;
        }

        if(pathLength >= aMaxPathLength)
            break;

        if(bsdf.ContinuationProb() == 0)
            break;

        // next event estimation
        if(lightCount > 0 && !bsdf.IsDelta() &&
            pathLength + 1 >= aMinPathLength)
        {
            int lightID = int(aStream.GetFloat() * lightCount);
            const AbstractLight *light = aScene.GetLightPtr(lightID);

            Vec3f directionToLight;
            float distance, directPdfW;
            Vec3f radiance = light->Illuminate(aScene.mSceneSphere, hitPoint,
                aStream.GetVec2f(), directionToLight, distance, directPdfW);

            if(!radiance.IsZero())
            {
                float bsdfPdfW, cosThetaOut;
                const Vec3f factor = bsdf.Evaluate(aScene,
                    directionToLight, cosThetaOut, &bsdfPdfW);

                if(!factor.IsZero())
                {
                    float weight = 1.f;
                    if(!light->IsDelta())
                    {
                        const float contProb = bsdf.ContinuationProb();
                        bsdfPdfW *= contProb;
                        weight = MltMis2(directPdfW * lightPickProb, bsdfPdfW);
                    }

                    Vec3f contrib = (weight * cosThetaOut /
                        (lightPickProb * directPdfW)) *
                        (radiance * factor);

                    if(!aScene.Occluded(hitPoint, directionToLight, distance))
                    {
                        color += pathWeight * contrib;
                    }
                }
            }
        }

        // continue random walk
        {
            Vec3f rndTriplet = aStream.GetVec3f();
            float pdf, cosThetaOut;
            uint  sampledEvent;

            Vec3f factor = bsdf.Sample(aScene, rndTriplet, ray.dir,
                pdf, cosThetaOut, &sampledEvent);

            if(factor.IsZero())
                break;

            // Russian roulette
            const float contProb = bsdf.ContinuationProb();

            lastSpecular = (sampledEvent & BSDF<true>::kSpecular) != 0;
            lastPdfW     = pdf * contProb;

            if(contProb < 1.f)
            {
                if(aStream.GetFloat() > contProb)
                {
                    break;
                }
                pdf *= contProb;
            }

            pathWeight *= factor * (cosThetaOut / pdf);
            // We offset ray origin instead of setting tmin due to numeric
            // issues in ray-sphere intersection. The isect.dist has to be
            // extended by this EPS_RAY after hitpoint is determined
            ray.org    = hitPoint + EPS_RAY * ray.dir;
            ray.tmin   = 0.f;
            isect.dist = 1e36f;
        }
    }

    PathResult result;
    result.mColor    = color;
    result.mRaster   = aRaster;
    result.mDimsUsed = 0; // filled in by callers that know the stream
    return result;
}

// Wrap a scalar into [0,1) modulo 1 (toroidal primary sample domain).
inline float WrapUnit(float aValue)
{
    return aValue - std::floor(aValue);
}

// Symmetric small-step kernel: aRnd is uniform in [0,1), the resulting
// offset is uniform in [-aWidth,aWidth], applied with modulo-1 wrap.
inline float SmallStepValue(float aCurrent, float aRnd, float aWidth)
{
    return WrapUnit(aCurrent + (2.f * aRnd - 1.f) * aWidth);
}

//////////////////////////////////////////////////////////////////////////
// Primary sample space sampler.
//
// Holds the current chain state: a vector of uniforms, one per primary
// sample dimension, together with the generation (mutation index) at which
// each dimension was last touched.  Mutations are transactional.
// BeginMutation() snapshots the whole state; RejectMutation() restores
// values, dimension count and generation stamps exactly, including
// dimensions first introduced by the rejected proposal.  The proposal RNG
// itself is never restored: it keeps advancing across accept and reject.
//
// Large steps resample every used dimension uniformly.  Small steps add an
// independent uniform offset in [-aWidth,aWidth] with modulo-1 wrap to each
// used dimension; the kernel is symmetric.  Lazily introduced dimensions are
// drawn as fresh uniforms first, so their marginal stays uniform whether the
// proposal is accepted or rejected.

class PrimarySampler : public UniformStream
{
public:
    explicit PrimarySampler(int aSeed)
        : mRng(aSeed),
          mDist(0.f, 1.f),
          mGeneration(0),
          mLargeStep(false),
          mSmallWidth(0.f)
    {}

    // Independent uniform from the proposal RNG.  Used for the large/small
    // decision, large step values, small step offsets, lazy dimensions and
    // the accept/reject draw.  Never rewound on rejection.
    float NextUniform()
    {
        return mDist(mRng);
    }

    // Snapshot the current state and open a new mutation.  The large/small
    // decision consumes one proposal RNG number here and now, so it is not
    // replayed on rejection.
    void BeginMutation(float aLargeStepProb, float aSmallStepWidth)
    {
        mBackupState      = mState;
        mBackupGeneration = mGeneration;
        ++mGeneration;
        mLargeStep  = NextUniform() < aLargeStepProb;
        mSmallWidth = aSmallStepWidth;
    }

    // Primary sample for the current mutation; one stable value per
    // dimension within the mutation.
    float NextSample()
    {
        const size_t dim = mCursor++;

        if(dim >= mState.size())
        {
            // Dimension never used before: seed it with a fresh uniform,
            // which already has the correct marginal on its own.
            const float fresh = NextUniform();
            if(mLargeStep)
            {
                mState.push_back(Dim{fresh, mGeneration});
            }
            else
            {
                // Symmetric perturbation of a uniform is still uniform.
                mState.push_back(Dim{SmallStepValue(fresh, NextUniform(),
                    mSmallWidth), mGeneration});
            }
            return mState[dim].value;
        }

        Dim& d = mState[dim];
        if(d.generation == mGeneration)
            return d.value; // already proposed in this mutation

        if(mLargeStep)
        {
            d.value = NextUniform();
        }
        else
        {
            d.value = SmallStepValue(d.value, NextUniform(), mSmallWidth);
        }
        d.generation = mGeneration;
        return d.value;
    }

    virtual float GetFloat() { return NextSample(); }

    void AcceptMutation()
    {
        // The proposed state is already in place; drop the snapshot.
        mBackupState.clear();
        mBackupGeneration = 0;
    }

    void RejectMutation()
    {
        // Full rollback: values, lazily added dimensions and generation
        // stamps all return to the pre-mutation state.
        mState.swap(mBackupState);
        mGeneration    = mBackupGeneration;
        mLargeStep     = false;
        mBackupState.clear();
    }

    // Restarts the chain from an adopted state (e.g. a bootstrap path).
    void AdoptState(const std::vector<float>& aValues)
    {
        mState.resize(aValues.size());
        for(size_t i = 0; i < aValues.size(); ++i)
            mState[i] = Dim{aValues[i], mGeneration};
        mBackupState.clear();
    }

    bool WasLargeStep() const { return mLargeStep; }

    std::vector<float> Values() const
    {
        std::vector<float> out(mState.size());
        for(size_t i = 0; i < mState.size(); ++i)
            out[i] = mState[i].value;
        return out;
    }

    size_t DimensionCount() const { return mState.size(); }
    uint64_t Generation() const   { return mGeneration; }

    // Resets the per-path dimension cursor; called before each evaluation.
    void ResetCursor() { mCursor = 0; }

private:
    struct Dim
    {
        float    value;
        uint64_t generation;
    };

    std::mt19937_64                          mRng;
    std::uniform_real_distribution<float>    mDist;
    std::vector<Dim>                         mState;
    std::vector<Dim>                         mBackupState;
    uint64_t                                 mGeneration;
    uint64_t                                 mBackupGeneration;
    size_t                                   mCursor = 0;
    bool                                     mLargeStep;
    float                                    mSmallWidth;
};

//////////////////////////////////////////////////////////////////////////
// Options for the Metropolis renderer, with validated ranges.

struct MLTOptions
{
    MLTOptions()
        : BootstrapSamples(100000),
          LargeStepProbability(0.1f),
          SmallStepWidth(1.f / 64.f)
    {}

    MLTOptions(int aBootstrapSamples, float aLargeStepProbability,
        float aSmallStepWidth)
        : BootstrapSamples(aBootstrapSamples),
          LargeStepProbability(aLargeStepProbability),
          SmallStepWidth(aSmallStepWidth)
    {}

    int   BootstrapSamples;       //!< positive integer: number of bootstrap paths
    float LargeStepProbability;   //!< finite, in (0,1]: probability of a large step
    float SmallStepWidth;         //!< finite, in (0,0.5]: small step half-width
};

// Statistics accumulated over the whole life of the renderer.  The counts
// persist across RunIteration calls and are never shared between instances.
struct MLTStats
{
    MLTStats()
        : mProposals(0),
          mLargeSteps(0),
          mAccepted(0),
          mBootstrapConstant(0.f)
    {}

    // All three counters are integer event counts (unit: number of
    // proposals, dimensionless); they accumulate over the renderer's whole
    // life and are never reset between RunIteration calls or shared between
    // renderer instances.
    uint64_t mProposals;          //!< completed proposals evaluated so far (count)
    uint64_t mLargeSteps;         //!< of those, proposals that were large steps (count)
    uint64_t mAccepted;           //!< of those, proposals accepted by the MH test (count)
    // b: normalization constant, the arithmetic mean sRGB luminance
    // (Luminance of linear RGB) over the bootstrap paths.  It has the same
    // units as the path tracer's per-pixel luminance estimate; 0 for an
    // all-black bootstrap.  A finite bootstrap makes it a finite-sample,
    // not necessarily unbiased, estimate.
    float    mBootstrapConstant;
};

class MLTRenderer : public AbstractRenderer
{
public:

    MLTRenderer(
        const Scene&      aScene,
        const MLTOptions& aOptions,
        int               aSeed = 1234)
        : AbstractRenderer(aScene),
          mOptions(aOptions),
          mInitialized(false),
          mBootSampler(aSeed),
          mChain(aSeed * 7 + 13),
          mCurrentLum(0.f),
          mFixedMinPathLength(0),
          mFixedMaxPathLength(2)
    {
        if(!(mOptions.BootstrapSamples > 0))
            throw std::invalid_argument(
                "MLTOptions.BootstrapSamples must be a positive integer");
        if(!std::isfinite(mOptions.LargeStepProbability) ||
            mOptions.LargeStepProbability <= 0.f ||
            mOptions.LargeStepProbability > 1.f)
            throw std::invalid_argument(
                "MLTOptions.LargeStepProbability must be finite in (0,1]");
        if(!std::isfinite(mOptions.SmallStepWidth) ||
            mOptions.SmallStepWidth <= 0.f ||
            mOptions.SmallStepWidth > 0.5f)
            throw std::invalid_argument(
                "MLTOptions.SmallStepWidth must be finite in (0,0.5]");
    }

    // Metropolis-Hastings acceptance min(1, g/f) with the explicit zero
    // rules: f == 0, g > 0 accepts with probability 1; f == g == 0 rejects.
    static float AcceptanceProbability(float aCurrentLum, float aProposalLum)
    {
        if(aCurrentLum == 0.f)
            return aProposalLum > 0.f ? 1.f : 0.f;
        return std::min(1.f, aProposalLum / aCurrentLum);
    }

    // Evaluates one path from an explicit, ordered primary sample vector.
    // Components 0,1 map to the full-frame raster (u0*width, u1*height);
    // the remaining components are consumed in path execution order.  The
    // same vector always yields the same raster and linear RGB.
    PathResult EvaluatePath(const std::vector<float>& aSamples) const
    {
        SequenceUniformStream stream(aSamples);
        const int resX = int(mScene.mCamera.mResolution.x);
        const int resY = int(mScene.mCamera.mResolution.y);

        const Vec2f raster(stream.GetFloat() * resX,
                           stream.GetFloat() * resY);

        PathResult result = EvaluateCameraPath(mScene,
            mMinPathLength, mMaxPathLength, raster, stream);
        result.mDimsUsed = int(stream.Position());
        return result;
    }

    // Public testing hook around the shared path evaluation.
    static PathResult EvaluatePathOnStream(
        const Scene&   aScene,
        uint           aMinPathLength,
        uint           aMaxPathLength,
        const Vec2f&   aRaster,
        UniformStream& aStream)
    {
        return EvaluateCameraPath(aScene, aMinPathLength, aMaxPathLength,
            aRaster, aStream);
    }

    MLTStats Stats() const { return mStats; }

    virtual void RunIteration(int /*aIteration*/)
    {
        if(!mInitialized)
            Initialize();

        const int resX = int(mScene.mCamera.mResolution.x);
        const int resY = int(mScene.mCamera.mResolution.y);
        const int proposalCount = resX * resY;
        const float b = mStats.mBootstrapConstant;
        // One iteration holds N = width*height proposals and each proposal
        // deposits total luminance b, so one iteration totals N*b, exactly
        // matching one PT iteration whose N pixels each have expected
        // luminance b.  GetFramebuffer then averages over iterations, which
        // keeps per-pixel exposure independent of resolution and of the
        // number of iterations/proposals.

        for(int i = 0; i < proposalCount; ++i)
        {
            // ---- propose (transaction begins) ----------------------------
            mChain.BeginMutation(mOptions.LargeStepProbability,
                mOptions.SmallStepWidth);
            mChain.ResetCursor();

            const Vec2f proposalRaster(mChain.NextSample() * resX,
                                       mChain.NextSample() * resY);
            const PathResult proposal = EvaluateCameraPath(mScene,
                mFixedMinPathLength, mFixedMaxPathLength,
                proposalRaster, mChain);
            const float g = Luminance(proposal.mColor);
            const float f = mCurrentLum;

            // ---- Metropolis-Hastings acceptance --------------------------
            const float a = AcceptanceProbability(f, g);

            // ---- deposit both states, even on rejection ------------------
            // Both the rejected (current) and accepted (proposal) terms are
            // settled every proposal.  Zero-luminance terms contribute zero;
            // the guards also keep the all-black case free of 0/0 divisions.
            // With f > 0 the two terms sum to b in luminance.
            if(f > 0.f)
            {
                const Vec3f rejected = mCurrentColor *
                    (b * (1.f - a) / f);
                mFramebuffer.AddColor(mCurrentRaster, rejected);
            }
            if(g > 0.f)
            {
                const Vec3f accepted = proposal.mColor * (b * a / g);
                mFramebuffer.AddColor(proposalRaster, accepted);
            }

            // ---- settle the transaction ----------------------------------
            // The decision draw always advances the proposal RNG, whether
            // the proposal is accepted or not.
            const bool accept = mChain.NextUniform() < a;
            ++mStats.mProposals;
            if(mChain.WasLargeStep())
                ++mStats.mLargeSteps;

            if(accept)
            {
                mChain.AcceptMutation();
                mCurrentColor  = proposal.mColor;
                mCurrentRaster = proposalRaster;
                mCurrentLum    = g;
                ++mStats.mAccepted;
            }
            else
            {
                mChain.RejectMutation();
            }
        }

        mIterations++;
    }

private:

    void Initialize()
    {
        // Path length limits are frozen here: callers may configure the
        // inherited members before the first iteration, but not afterwards.
        mFixedMinPathLength = mMinPathLength;
        mFixedMaxPathLength = mMaxPathLength;

        const int resX = int(mScene.mCamera.mResolution.x);
        const int resY = int(mScene.mCamera.mResolution.y);

        const int bootstrapCount = mOptions.BootstrapSamples;
        std::vector<float> bootstrapLum(bootstrapCount, 0.f);
        std::vector<std::vector<float> > bootstrapStates(bootstrapCount);

        float luminanceSum = 0.f;

        // Independent uniform primary samples: the bootstrap sampler only
        // ever takes large steps, so every path is an independent draw.
        for(int k = 0; k < bootstrapCount; ++k)
        {
            mBootSampler.BeginMutation(1.f, mOptions.SmallStepWidth);
            mBootSampler.ResetCursor();

            const Vec2f raster(mBootSampler.NextSample() * resX,
                               mBootSampler.NextSample() * resY);
            const PathResult result = EvaluateCameraPath(mScene,
                mFixedMinPathLength, mFixedMaxPathLength, raster, mBootSampler);

            const float lum = Luminance(result.mColor);
            bootstrapLum[k]    = lum;
            bootstrapStates[k] = mBootSampler.Values();
            luminanceSum      += lum;
            mBootSampler.AcceptMutation();
        }

        // Arithmetic mean; a finite bootstrap gives a finite-sample, possibly
        // biased normalization constant -- this is accepted by design.
        mStats.mBootstrapConstant = luminanceSum / bootstrapCount;

        // Choose the initial chain state with probability proportional to
        // luminance.  An all-black bootstrap keeps the chain on a zero
        // contribution state; every splat below is then zero.
        int startIndex = 0;
        if(luminanceSum > 0.f)
        {
            const float pick = mChain.NextUniform() * luminanceSum;
            float cumulative = 0.f;
            for(int k = 0; k < bootstrapCount; ++k)
            {
                cumulative += bootstrapLum[k];
                if(pick <= cumulative)
                {
                    startIndex = k;
                    break;
                }
            }
        }

        mChain.AdoptState(bootstrapStates[startIndex]);
        mChain.ResetCursor();
        // Consume the two raster dimensions through the chain first, so the
        // path's internal randoms start at dimension 2 exactly as they did
        // during bootstrap; adopted values are returned without touching the
        // proposal RNG, so this reproduces the selected bootstrap path.
        const Vec2f startRaster(mChain.NextSample() * resX,
                                mChain.NextSample() * resY);
        const PathResult startPath = EvaluateCameraPath(mScene,
            mFixedMinPathLength, mFixedMaxPathLength, startRaster, mChain);

        mCurrentColor  = startPath.mColor;
        mCurrentRaster = startRaster;
        mCurrentLum    = Luminance(startPath.mColor);

        // Bootstrap paths warm the chain only; they are never splatted.
        mInitialized = true;
    }

    MLTOptions      mOptions;
    bool            mInitialized;
    PrimarySampler  mBootSampler;   //!< independent bootstrap primary samples
    PrimarySampler  mChain;         //!< the Metropolis chain
    MLTStats        mStats;

    Vec3f           mCurrentColor;
    Vec2f           mCurrentRaster;
    float           mCurrentLum;

    uint            mFixedMinPathLength;
    uint            mFixedMaxPathLength;
};

#endif //__MLT_HXX__
