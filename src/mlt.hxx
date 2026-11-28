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

#ifndef __MLT_HXX__
#define __MLT_HXX__

#include <vector>
#include <cmath>
#include <stdexcept>
#include "renderer.hxx"
#include "pathtracer.hxx"
#include "rng.hxx"
#include "utils.hxx"

//////////////////////////////////////////////////////////////////////////
// User-tunable parameters of the primary sample space MLT renderer.
struct MLTOptions
{
    MLTOptions(
        int   aBootstrapSamples   = 100000,
        float aLargeStepProbability = 0.1f,
        float aSmallStepWidth       = 0.02f)
        : BootstrapSamples(aBootstrapSamples),
          LargeStepProbability(aLargeStepProbability),
          SmallStepWidth(aSmallStepWidth)
    {}

    //! Number of independent uniform paths used to estimate the normalization
    int   BootstrapSamples;
    //! Probability per proposal of taking a global (large) step, in (0,1]
    float LargeStepProbability;
    //! Half-width of the symmetric small-step window, in (0,0.5]
    float SmallStepWidth;
};

//////////////////////////////////////////////////////////////////////////
// A vector of primary samples (one 1D uniform per path-sampling dimension).
//
// Dimensions are lazily extended and lazily mutated: the shared path
// evaluation simply asks for the next dimension, and only the dimensions it
// actually reads are touched by a small step.  A dimension that exists from
// an earlier, longer path but is not used keeps its value and generation,
// and therefore its correct uniform marginal; a brand new dimension is
// materialized as an independent uniform.
//
// Proposals are transactions.  BeginProposal() snapshots the whole state;
// a later Commit() publishes it while Rollback() restores every value, its
// generation and removes dimensions that were first touched by the rejected
// proposal.  Neither path ever rewinds the external mutation source.
class PrimarySampleVector : public IUniformStream
{
public:

    PrimarySampleVector()
        : mCursor(0), mProposing(false), mLargeStep(false),
          mWidth(0.f), mpMutSource(NULL),
          mGlobalGeneration(0), mProposedGeneration(0), mBackupGeneration(0)
    {}

    // IUniformStream: hand out primary samples in path execution order.
    // Reading a dimension is what lazily applies the current small step.
    virtual float Next1D()
    {
        const int index = mCursor++;
        Touch(index);
        return mValues[index];
    }

    virtual int DimsUsed() const { return int(mCursor); }

    int Size() const { return int(mValues.size()); }

    float ValueAt(int aIndex) const { return mValues[aIndex]; }

    // Replay a known vector of uniforms (used to seed a chain from a
    // bootstrap path).  All dimensions share the current generation.
    void Assign(const std::vector<float>& aValues)
    {
        mValues = aValues;
        mGenerations.assign(aValues.size(), mGlobalGeneration);
        mCursor = 0;
    }

    const std::vector<float>& Values() const { return mValues; }

    uint GlobalGeneration() const { return mGlobalGeneration; }
    uint GenerationAt(int aIndex) const { return mGenerations[aIndex]; }

    // Opens a mutation transaction.  A large step re-samples every existing
    // dimension uniformly up front; a small step touches nothing here and
    // instead adds an independent offset, uniform on [-aWidth,aWidth], to
    // each dimension as it is read, wrapping mod 1.  All mutation uniforms
    // come from aMutSource, which the caller owns and which keeps advancing
    // even if the proposal is later rejected.
    void BeginProposal(
        const bool      aLargeStep,
        IUniformStream& aMutSource,
        const float     aWidth)
    {
        mBackupValues     = mValues;
        mBackupGens       = mGenerations;
        mBackupGeneration = mGlobalGeneration;
        mProposing        = true;
        mLargeStep        = aLargeStep;
        mWidth            = aWidth;
        mpMutSource       = &aMutSource;
        mProposedGeneration = mGlobalGeneration + 1;
        mCursor           = 0;

        if(aLargeStep)
        {
            // Large step: the whole vector is re-sampled uniformly.
            for(size_t i = 0; i < mValues.size(); ++i)
            {
                mValues[i] = aMutSource.Next1D();
                mGenerations[i] = mProposedGeneration;
            }
        }
    }

    // Publishes the proposal and advances the generation.
    void Commit()
    {
        mProposing = false;
        mGlobalGeneration = mProposedGeneration;
        DiscardBackup();
    }

    // Restores the exact pre-proposal state, including dimensions and
    // generations, while leaving the external mutation source untouched.
    void Rollback()
    {
        mValues.swap(mBackupValues);
        mGenerations.swap(mBackupGens);
        mGlobalGeneration = mBackupGeneration;
        mProposing = false;
        mCursor = 0;
        DiscardBackup();
    }

private:

    static float WrapUnit(float aValue)
    {
        return aValue - std::floor(aValue);
    }

    void DiscardBackup()
    {
        std::vector<float>().swap(mBackupValues);
        std::vector<uint>().swap(mBackupGens);
    }

    // Ensures dimension aIndex exists and, during a small step, applies its
    // single symmetric mutation the first time this proposal reads it.
    void Touch(int aIndex)
    {
        while(int(mValues.size()) <= aIndex)
        {
            // A dimension never seen before starts as an independent
            // uniform.  During a small step it receives the same symmetric
            // perturbation as existing dimensions, which preserves its
            // uniform marginal; a rejected proposal removes it entirely.
            const float fresh = mpMutSource->Next1D();
            if(mProposing && !mLargeStep)
            {
                const float offset =
                    (2.f * mpMutSource->Next1D() - 1.f) * mWidth;
                mValues.push_back(WrapUnit(fresh + offset));
            }
            else
            {
                mValues.push_back(fresh);
            }
            mGenerations.push_back(mProposing ? mProposedGeneration
                                              : mGlobalGeneration);
        }

        // Small step on an existing dimension: mutate once per proposal,
        // detected through the stale per-dimension generation.  Large steps
        // already refreshed every dimension, and non-proposing reads are
        // plain replays.
        if(mProposing && !mLargeStep &&
           mGenerations[aIndex] != mProposedGeneration)
        {
            const float offset =
                (2.f * mpMutSource->Next1D() - 1.f) * mWidth;
            mValues[aIndex] = WrapUnit(mValues[aIndex] + offset);
            mGenerations[aIndex] = mProposedGeneration;
        }
    }

private:

    std::vector<float> mValues;       //!< Primary samples, one per dimension
    std::vector<uint>  mGenerations;  //!< Generation tag of each dimension
    int                mCursor;       //!< Next dimension the path will read
    bool               mProposing;
    bool               mLargeStep;
    float              mWidth;
    IUniformStream*    mpMutSource;
    uint               mGlobalGeneration;
    uint               mProposedGeneration;

    std::vector<float> mBackupValues;
    std::vector<uint>  mBackupGens;
    uint               mBackupGeneration;
};

//////////////////////////////////////////////////////////////////////////
// Cumulative statistics of an MLT run.
struct MLTStats
{
    MLTStats() : Proposals(0), LargeSteps(0), Accepts(0), Normalization(0.f)
    {}

    //! Number of completed mutations (proposals) over all iterations
    long long Proposals;
    //! Number of those mutations that were large steps
    long long LargeSteps;
    //! Number of accepted mutations
    long long Accepts;
    //! Bootstrap scalar b: arithmetic mean of path Luminance values; it has
    //! the same units as Luminance() and estimates the screen-space integral
    //! of radiance (finite-bootstrap bias accepted by design).
    float     Normalization;
};

//////////////////////////////////////////////////////////////////////////
// Maps a primary sample vector to one path's raster position, linear RGB
// and scalar luminance.  Production binds it to the shared camera path;
// tests bind it to hand-computed luminance functions.
class IPathEvaluator
{
public:

    virtual ~IPathEvaluator() {}

    virtual void Evaluate(
        PrimarySampleVector& aStream,
        Vec2f&               oRaster,
        Vec3f&               oColor,
        float&               oLuminance) = 0;
};

// Evaluator backed by the real PathTracer single-path routine.
class CameraPathEvaluator : public IPathEvaluator
{
public:

    CameraPathEvaluator(
        const Scene& aScene,
        const uint   aMinPathLength,
        const uint   aMaxPathLength)
        : mScene(aScene),
          mMinPathLength(aMinPathLength),
          mMaxPathLength(aMaxPathLength)
    {}

    virtual void Evaluate(
        PrimarySampleVector& aStream,
        Vec2f&               oRaster,
        Vec3f&               oColor,
        float&               oLuminance)
    {
        const SinglePathResult result = EvaluateCameraPath(
            mScene, mMinPathLength, mMaxPathLength, aStream);
        oRaster    = result.mRaster;
        oColor     = result.mColor;
        oLuminance = Luminance(result.mColor);
    }

private:

    const Scene& mScene;
    uint         mMinPathLength;
    uint         mMaxPathLength;
};

//////////////////////////////////////////////////////////////////////////
// Primary sample space Metropolis light transport renderer.
//
// It reuses PathTracer's exact single-path evaluation
// (EvaluateCameraPath): camera rays, direct lighting, emission hits, BSDF
// continuation, Russian roulette and the existing MIS are all unchanged and
// operate on the original scene and materials.  Only the source of the
// uniform random numbers differs - here it is a mutated primary sample
// vector instead of an independent per-pixel RNG.
class MLTRenderer : public AbstractRenderer
{
public:

    MLTRenderer(
        const Scene&      aScene,
        const MLTOptions& aOptions,
        int               aSeed = 1234)
        : AbstractRenderer(aScene),
          mState(),
          mCurrentColor(0.f),
          mCurrentRaster(0.f, 0.f),
          mCurrentLuminance(0.f),
          mStats(),
          mOptions(aOptions),
          mSeed(aSeed),
          mInitialized(false),
          mMutationRng()
    {
        if(!(aOptions.BootstrapSamples > 0))
        {
            throw std::invalid_argument(
                "MLTOptions.BootstrapSamples must be a positive integer");
        }
        if(!std::isfinite(aOptions.LargeStepProbability) ||
           aOptions.LargeStepProbability <= 0.f ||
           aOptions.LargeStepProbability > 1.f)
        {
            throw std::invalid_argument(
                "MLTOptions.LargeStepProbability must be a finite value in (0,1]");
        }
        if(!std::isfinite(aOptions.SmallStepWidth) ||
           aOptions.SmallStepWidth <= 0.f ||
           aOptions.SmallStepWidth > 0.5f)
        {
            throw std::invalid_argument(
                "MLTOptions.SmallStepWidth must be a finite value in (0,0.5]");
        }
    }

    //! Exposed for tests: the shared single-path entry point.
    static SinglePathResult EvaluatePath(
        const Scene&        aScene,
        const uint          aMinPathLength,
        const uint          aMaxPathLength,
        IUniformStream&     aStream)
    {
        return EvaluateCameraPath(aScene, aMinPathLength, aMaxPathLength, aStream);
    }

    //! Exposed for tests: the primary sample sampler driving the chain.
    PrimarySampleVector& State() { return mState; }

    const MLTStats& Stats() const { return mStats; }

    bool IsInitialized() const { return mInitialized; }

    // Metropolis acceptance min(1,g/f); f=0,g>0 -> 1; both zero -> 0.
    static float Acceptance(const float f, const float g)
    {
        if(f <= 0.f)
            return g > 0.f ? 1.f : 0.f;
        if(g <= 0.f)
            return 0.f;
        return std::min(1.f, g / f);
    }

    // Wrap a real number into the primary sample domain [0,1).
    static float WrapUnit(const float aValue)
    {
        return aValue - std::floor(aValue);
    }

    // One settled contribution of scale * color / luminance; the zero term
    // (zero luminance or zero scale) is exactly black.
    static Vec3f SplatContribution(
        const Vec3f& aColor,
        const float  aLuminance,
        const float  aScale)
    {
        if(aLuminance <= 0.f || aScale == 0.f)
            return Vec3f(0.f);
        return aColor * (aScale / aLuminance);
    }

    virtual void RunIteration(int /*aIteration*/)
    {
        if(!mInitialized)
            Initialize();

        RngStream mutSource(mMutationRng);
        CameraPathEvaluator evaluator(
            mScene, mMinPathLength, mMaxPathLength);

        const int resX = int(mScene.mCamera.mResolution.x);
        const int resY = int(mScene.mCamera.mResolution.y);

        for(int i = 0; i < resX * resY; ++i)
        {
            RunOneProposal(evaluator, mutSource);
        }

        mIterations++;
    }

protected:

    // State of the current chain path, exposed for subclasses (tests).
    PrimarySampleVector mState;
    Vec3f               mCurrentColor;
    Vec2f               mCurrentRaster;
    float               mCurrentLuminance;
    MLTStats            mStats;
    MLTOptions          mOptions;

    // Performs one complete proposal transaction - begin, evaluate, accept
    // or reject - using aEvaluator and the mutation uniform source.  Both
    // the current and the proposed path are always settled into the
    // framebuffer; rejection restores the full primary-sample state while
    // aMutSource keeps advancing.  Exposed for subclasses (tests).
    void RunOneProposal(
        IPathEvaluator&  aEvaluator,
        IUniformStream&  aMutSource)
    {
        const bool largeStep =
            aMutSource.Next1D() < mOptions.LargeStepProbability;
        if(largeStep)
            mStats.LargeSteps++;

        mState.BeginProposal(largeStep, aMutSource, mOptions.SmallStepWidth);

        Vec2f proposedRaster;
        Vec3f proposedColor;
        float proposedLuminance;
        aEvaluator.Evaluate(
            mState, proposedRaster, proposedColor, proposedLuminance);
        const float g = proposedLuminance;

        const float a = Acceptance(mCurrentLuminance, g);
        // The decision uniform is always consumed, so the source advances
        // identically whether the move is accepted or rejected.
        const bool accept = aMutSource.Next1D() < a;

        // Dual-state settlement happens unconditionally, even on rejection.
        mFramebuffer.AddColor(mCurrentRaster, SplatContribution(
            mCurrentColor, mCurrentLuminance,
            mStats.Normalization * (1.f - a)));
        mFramebuffer.AddColor(proposedRaster, SplatContribution(
            proposedColor, g, mStats.Normalization * a));

        mStats.Proposals++;

        if(accept)
        {
            mState.Commit();
            mCurrentLuminance = g;
            mCurrentColor     = proposedColor;
            mCurrentRaster    = proposedRaster;
            mStats.Accepts++;
        }
        else
        {
            mState.Rollback();
        }
    }

private:

    void Initialize()
    {
        // Path length limits are frozen from this point on: the caller may
        // adjust the inherited mMinPathLength/mMaxPathLength before the first
        // iteration, but bootstrap and chain share whatever is current now.
        Rng bootstrapRng(mSeed);
        Rng mutationRng(mSeed + 1);

        std::vector<std::vector<float> > bootstrapVectors;
        std::vector<float>               bootstrapLuminance;
        std::vector<Vec3f>               bootstrapColor;
        std::vector<Vec2f>               bootstrapRaster;
        bootstrapVectors.reserve(mOptions.BootstrapSamples);
        bootstrapLuminance.reserve(mOptions.BootstrapSamples);
        bootstrapColor.reserve(mOptions.BootstrapSamples);
        bootstrapRaster.reserve(mOptions.BootstrapSamples);

        float luminanceSum = 0.f;

        for(int s = 0; s < mOptions.BootstrapSamples; ++s)
        {
            RecordingStream stream(bootstrapRng);
            const SinglePathResult result = EvaluateCameraPath(
                mScene, mMinPathLength, mMaxPathLength, stream);

            const float lum = Luminance(result.mColor);
            luminanceSum += lum;

            bootstrapVectors.push_back(stream.Recorded());
            bootstrapLuminance.push_back(lum);
            bootstrapColor.push_back(result.mColor);
            bootstrapRaster.push_back(result.mRaster);
        }

        // Arithmetic mean of bootstrap path luminances.  A finite bootstrap
        // makes this a slightly biased normalizer, which the task explicitly
        // accepts; an all-black scene yields b == 0.
        const float b = luminanceSum / float(mOptions.BootstrapSamples);
        mStats.Normalization = b;

        // Pick the initial chain state with probability proportional to the
        // bootstrap luminances.  With b == 0 every weight is zero, so any
        // state (here the first) is a valid, permanently-black chain.
        int chosen = 0;
        if(luminanceSum > 0.f)
        {
            const float pick = bootstrapRng.GetFloat() * luminanceSum;
            float cumulative = 0.f;
            chosen = int(bootstrapLuminance.size()) - 1;
            for(size_t i = 0; i < bootstrapLuminance.size(); ++i)
            {
                cumulative += bootstrapLuminance[i];
                if(pick < cumulative)
                {
                    chosen = int(i);
                    break;
                }
            }
        }

        mState.Assign(bootstrapVectors[chosen]);
        mCurrentColor     = bootstrapColor[chosen];
        mCurrentRaster    = bootstrapRaster[chosen];
        mCurrentLuminance = bootstrapLuminance[chosen];

        // Bootstrap paths seed the chain only; they are never splatted.
        mMutationRng = mutationRng;
        mInitialized = true;
    }

private:

    int  mSeed;
    bool mInitialized;
    Rng  mMutationRng;
};

#endif //__MLT_HXX__
