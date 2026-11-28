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

#ifndef __PATHTRACER_HXX__
#define __PATHTRACER_HXX__

#include <vector>
#include <cmath>
#include "renderer.hxx"
#include "bsdf.hxx"
#include "rng.hxx"

//////////////////////////////////////////////////////////////////////////
// Sequential stream of 1D uniform samples in [0,1).
//
// The whole camera path evaluation (camera ray, next event estimation,
// emission hits, BSDF continuation, Russian roulette, MIS) reads every
// random number it needs through this interface.  No code path is allowed
// to touch a private RNG behind the caller's back, which is what makes the
// evaluation deterministic in a supplied primary sample vector and lets
// PathTracer and MLTRenderer share the very same path routine.
class IUniformStream
{
public:

    virtual ~IUniformStream() {}

    virtual float Next1D() = 0;

    // Number of 1D uniforms handed out so far.
    virtual int DimsUsed() const = 0;

    // Optional exact full-frame raster position.  Generic streams leave the
    // first two uniforms to be scaled by the resolution; PixelRngStream
    // overrides this to return the original PathTracer sample verbatim, so
    // the legacy renderer stays bit-for-bit identical.
    virtual bool RasterPosition(Vec2f& /*oRaster*/) const { return false; }

    Vec2f Next2D()
    {
        const float a = Next1D();
        const float b = Next1D();
        return Vec2f(a, b);
    }

    Vec3f Next3D()
    {
        const float a = Next1D();
        const float b = Next1D();
        const float c = Next1D();
        return Vec3f(a, b, c);
    }
};

// Plays back a fixed, caller-provided sequence of uniforms.  The cursor
// only moves forward, so the number of consumed dimensions is observable.
class VectorStream : public IUniformStream
{
public:

    explicit VectorStream(const std::vector<float>& aValues)
        : mValues(aValues), mCursor(0)
    {}

    virtual float Next1D()
    {
        // Paths always request dimensions through vectors sized generously
        // by the caller; running out means a bug in that sizing.
        return mValues[mCursor++];
    }

    virtual int DimsUsed() const { return int(mCursor); }

private:

    const std::vector<float>& mValues;
    size_t                    mCursor;
};

// Adapts the original PathTracer Rng to the stream interface.  The first
// two samples are the normalized full-frame raster coordinates of one
// pixel, every subsequent draw comes straight from the Rng in exactly the
// original call order.  RasterPosition() additionally hands back the exact
// original (pixel + jitter) position, preserving legacy results.
class PixelRngStream : public IUniformStream
{
public:

    PixelRngStream(
        Rng         &aRng,
        const float  aRasterU0,
        const float  aRasterU1,
        const Vec2f& aExactRaster)
        : mRng(aRng), mDim(0), mU0(aRasterU0), mU1(aRasterU1),
          mExactRaster(aExactRaster)
    {}

    virtual float Next1D()
    {
        if(mDim == 0)
        {
            mDim++;
            return mU0;
        }
        if(mDim == 1)
        {
            mDim++;
            return mU1;
        }
        mDim++;
        return mRng.GetFloat();
    }

    virtual int DimsUsed() const { return mDim; }

    virtual bool RasterPosition(Vec2f& oRaster) const
    {
        oRaster = mExactRaster;
        return true;
    }

private:

    Rng&  mRng;
    int   mDim;
    float mU0;
    float mU1;
    Vec2f mExactRaster;
};

// Records every uniform drawn from an underlying Rng, so bootstrap paths
// can be replayed later as an explicit primary sample vector.
class RecordingStream : public IUniformStream
{
public:

    explicit RecordingStream(Rng &aRng) : mRng(aRng), mCount(0)
    {}

    void Clear() { mRecorded.clear(); mCount = 0; }

    virtual float Next1D()
    {
        const float v = mRng.GetFloat();
        mRecorded.push_back(v);
        mCount++;
        return v;
    }

    const std::vector<float>& Recorded() const { return mRecorded; }

    virtual int DimsUsed() const { return mCount; }

private:

    Rng&                  mRng;
    std::vector<float>    mRecorded;
    int                   mCount;
};

// Thin adapter exposing the project Rng through the uniform stream
// interface, which lets the MLT machinery take any uniform source (the
// rendering Rng here, a fixed VectorStream in tests).
class RngStream : public IUniformStream
{
public:

    explicit RngStream(Rng &aRng) : mRng(aRng), mCount(0)
    {}

    virtual float Next1D()
    {
        mCount++;
        return mRng.GetFloat();
    }

    virtual int DimsUsed() const { return mCount; }

private:

    Rng& mRng;
    int  mCount;
};

// Result of one camera path evaluation.
struct SinglePathResult
{
    Vec2f mRaster;   //!< Full-frame raster position, i.e. (u0*width, u1*height)
    Vec3f mColor;    //!< Linear radiance estimate (RGB)
    int   mDimsUsed; //!< Number of 1D uniforms consumed from the stream
};

// Mis power (1 for balance heuristic)
inline float PathMis(float aPdf)
{
    return aPdf;
}

// Mis weight for 2 pdfs
inline float PathMis2(
    float aSamplePdf,
    float aOtherPdf)
{
    return PathMis(aSamplePdf) / (PathMis(aSamplePdf) + PathMis(aOtherPdf));
}

// The single shared camera-path evaluation used by both PathTracer and
// MLTRenderer.  It is a pure function of the scene, the path length limits
// and the uniforms handed in through aStream: identical streams produce
// identical raster positions and linear RGB, and it never draws random
// numbers from anywhere else.
//
// The first two uniforms always select the full-frame raster position
// (u0*width, u1*height); all following uniforms are consumed in execution
// order, exactly as the original PathTracer loop did.
inline SinglePathResult EvaluateCameraPath(
    const Scene&   aScene,
    const uint     aMinPathLength,
    const uint     aMaxPathLength,
    IUniformStream&aStream)
{
    SinglePathResult result;
    result.mColor = Vec3f(0.f);
    result.mDimsUsed = 0;

    // We sample lights uniformly
    const int   lightCount    = aScene.GetLightCount();
    const float lightPickProb = lightCount > 0 ? 1.f / lightCount : 0.f;

    const float resX = aScene.mCamera.mResolution.x;
    const float resY = aScene.mCamera.mResolution.y;

    const float u0 = aStream.Next1D();
    const float u1 = aStream.Next1D();
    Vec2f sample(u0 * resX, u1 * resY);
    // A stream may supply the exact raster position (the legacy per-pixel
    // path does, to avoid any divide/multiply round trip); generic primary
    // sample vectors keep the canonical (u0*width, u1*height) definition.
    Vec2f exactRaster;
    if(aStream.RasterPosition(exactRaster))
        sample = exactRaster;
    result.mRaster = sample;

    Ray   ray = aScene.mCamera.GenerateRay(sample);
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
                misWeight = PathMis2(lastPdfW, directPdfW * lightPickProb);
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
                misWeight = PathMis2(lastPdfW, directPdfW * lightPickProb);
            }

            color += pathWeight * misWeight * contrib;
            break;
        }

        if(pathLength >= aMaxPathLength)
            break;

        if(bsdf.ContinuationProb() == 0)
            break;

        // next event estimation
        if(lightCount > 0 && !bsdf.IsDelta() && pathLength + 1 >= aMinPathLength)
        {
            int lightID = int(aStream.Next1D() * lightCount);
            const AbstractLight *light = aScene.GetLightPtr(lightID);

            Vec3f directionToLight;
            float distance, directPdfW;
            Vec3f radiance = light->Illuminate(aScene.mSceneSphere, hitPoint,
                aStream.Next2D(), directionToLight, distance, directPdfW);

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
                        weight = PathMis2(directPdfW * lightPickProb, bsdfPdfW);
                    }

                    Vec3f contrib = (weight * cosThetaOut / (lightPickProb * directPdfW)) *
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
            Vec3f rndTriplet = aStream.Next3D();
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
                if(aStream.Next1D() > contProb)
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

    result.mColor    = color;
    result.mDimsUsed = aStream.DimsUsed();
    return result;
}

class PathTracer : public AbstractRenderer
{
public:

    PathTracer(
        const Scene& aScene,
        int aSeed = 1234
    ) :
        AbstractRenderer(aScene), mRng(aSeed)
    {}

    virtual void RunIteration(int aIteration)
    {
        const int resX = int(mScene.mCamera.mResolution.x);
        const int resY = int(mScene.mCamera.mResolution.y);

        for(int pixID = 0; pixID < resX * resY; pixID++)
        {
            const int x = pixID % resX;
            const int y = pixID / resX;

            // Original behavior: per-pixel jitter from the default Rng, in
            // the original pixel order.  The exact (pixel + jitter) position
            // is handed to the shared routine so no divide/multiply round
            // trip changes it; the same values are also exposed as the two
            // normalized raster uniforms the generic contract relies on.
            const Vec2f jitter = mRng.GetVec2f();
            const Vec2f exactSample(float(x) + jitter.x,
                                    float(y) + jitter.y);
            const float u0 = exactSample.x / resX;
            const float u1 = exactSample.y / resY;

            PixelRngStream stream(mRng, u0, u1, exactSample);
            SinglePathResult result = EvaluateCameraPath(
                mScene, mMinPathLength, mMaxPathLength, stream);

            mFramebuffer.AddColor(result.mRaster, result.mColor);
        }

        mIterations++;
    }

private:

    Rng mRng;
};

#endif //__PATHTRACER_HXX__
