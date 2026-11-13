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
 * (The above copyright notice is MIT License: http://en.wikipedia.org/wiki/MIT_License)
 */

#ifndef __CAMERA_HXX__
#define __CAMERA_HXX__

#include <vector>
#include <cmath>
#include "math.hxx"
#include "ray.hxx"

////////////////////////////////////////////////////////////////////////////////
// THIN LENS CAMERA
////////////////////////////////////////////////////////////////////////////////
//
// The pinhole camera remains the default after Setup(). A thin lens is added
// with SetLens(); a zero radius keeps the pinhole behaviour byte-for-byte.
//
// Geometry
// -------
// The lens is a disk of radius R through mPosition, perpendicular to
// mForward. Its azimuthal basis (mRasterAxisX, mRasterAxisY) is taken from
// the perspective projection itself, so increasing raster x/y maps to the
// corresponding lens azimuthal direction.
//
// The focus plane passes through mPosition + focusDistance * mForward.
//
// Eye ray generation: the pinhole direction for the raster position hits the
// focus plane at point F; the actual ray goes from the sampled lens point L
// through F. All points on the focus plane therefore image sharply.
//
// Apodized pupil
// --------------
// With t = |L - mPosition|^2 / R^2 in [0,1] and k = apodization, the pupil
// transmittance is T(t) = 1 - k*t. Its area average is
//
//   M = (1/(pi R^2)) * integral_D T dA = integral_0^1 T(t) dt = 1 - k/2,
//
// because dA = pi R^2 dt for t = r^2/R^2.
//
// The forward (eye path) strategy samples the lens with area density
//
//   p_fwd(L) = T(t) / (pi R^2 M),
//
// i.e. the transmittance is importance sampled: the eye path keeps unit
// throughput and reports response = T(t)/M only for bookkeeping. The reverse
// (light path connection) strategy samples the disk uniformly,
//
//   p_rev(L) = 1 / (pi R^2),
//
// and the integrator multiplies the connection contribution by response.
// Both strategies use the normalized pupil average M, so opening the aperture
// never multiplies the exposure: radiance is only redistributed between
// pixels (defocus blur), never scaled by the pupil area.
//
// Forward radial sampling is inversion of the marginal CDF
//
//   F(t) = (t - k t^2 / 2) / M,
//
// solved in a numerically stable way that also covers k = 0:
//
//   t = 2 M u.x / (1 + sqrt(1 - 2 k M u.x)).
//
// There is no rejection sampling. u.x drives the inside-out radial cumulative
// probability, u.y drives the azimuth from raster +x towards raster +y.
//
// Solid angle Jacobian
// --------------------
// For a fixed lens point, the mapping between raster area and ray solid angle
// is (the focus distance cancels in the derivation)
//
//   d omega / dA_raster = cos^3 / d^2,
//
// where d = mImagePlaneDist and cos is the dot of the lens-to-scene ray with
// mForward. Hence pdfW = d^2 / cos^3 on both endpoints. It contains neither
// the pixel count nor the lens area density.
//
// Joint-measure MIS summary (full derivations live in vertexcm.hxx next to
// the two endpoints that consume them):
//
//   p_fwd(L) / p_rev(L) = response,      p_rev(L) / p_fwd(L) = 1/response.
//
// For k = 0, T = M = 1 so response = 1: the two lens strategies are identical
// and no pi R^2 factor survives in any MIS weight. For a pinhole (R = 0)
// pdfLensA and response are both booked as 1.
////////////////////////////////////////////////////////////////////////////////

struct CameraRaySample
{
    Ray   ray;
    float pdfLensA; // area density of the sampled lens point (1 for pinhole)
    float pdfW;     // d^2 / cos^3, per raster area, excluding the lens density
    float response; // T(t)/M (1 for pinhole or k = 0)
};

struct CameraConnection
{
    Vec2f raster;             // raster position the connection images to
    Vec3f lensPoint;          // sampled point on the lens disk
    Vec3f directionToCamera; // unit vector from the scene point to lensPoint
    float distance;           // distance from the scene point to lensPoint
    float pdfLensA;           // area density used to sample lensPoint
    float pdfW;               // d^2 / cos^3
    float response;           // T(t)/M
};

class Camera
{
public:

    Camera() :
        mHasLens(false),
        mLensRadius(0.f),
        mFocusDistance(0.f),
        mApodization(0.f),
        mApoMean(1.f)
    {}

    void Setup(
        const Vec3f &aPosition,
        const Vec3f &aForward,
        const Vec3f &aUp,
        const Vec2f &aResolution,
        float       aHorizontalFOV)
    {
        const Vec3f forward = Normalize(aForward);
        const Vec3f up      = Normalize(Cross(aUp, -forward));
        const Vec3f left    = Cross(-forward, up);

        mPosition   = aPosition;
        mForward    = forward;
        mResolution = aResolution;

        // Raster-space basis of the lens disk. These are the two in-plane
        // rotation rows of the perspective projection itself: moving a raster
        // point along +x/+y shifts its camera-space ray along these same two
        // axes, so the lens azimuth is measured directly in raster directions.
        mRasterAxisX = up;
        mRasterAxisY = -left;

        const Vec3f pos(
            Dot(up, aPosition),
            Dot(left, aPosition),
            Dot(-forward, aPosition));

        Mat4f worldToCamera = Mat4f::Identity();
        worldToCamera.SetRow(0, up,       -pos.x);
        worldToCamera.SetRow(1, left,     -pos.y);
        worldToCamera.SetRow(2, -forward, -pos.z);

        const Mat4f perspective = Mat4f::Perspective(aHorizontalFOV, 0.1f, 10000.f);
        const Mat4f worldToNScreen = perspective * worldToCamera;
        const Mat4f nscreenToWorld = Invert(worldToNScreen);

        mWorldToRaster  =
            Mat4f::Scale(Vec3f(aResolution.x * 0.5f, aResolution.y * 0.5f, 0)) *
            Mat4f::Translate(Vec3f(1.f, 1.f, 0)) * worldToNScreen;

        mRasterToWorld  = nscreenToWorld *
            Mat4f::Translate(Vec3f(-1.f, -1.f, 0)) *
            Mat4f::Scale(Vec3f(2.f / aResolution.x, 2.f / aResolution.y, 0));

        const float tanHalfAngle = std::tan(aHorizontalFOV * PI_F / 360.f);
        mImagePlaneDist = aResolution.x / (2.f * tanHalfAngle);

        // Setup always restores the pinhole configuration.
        ResetLens();
    }

    // Configures the thin lens. Returns false (keeping the previous
    // configuration) when any argument is invalid. A zero radius selects the
    // pinhole entrance, which ignores the apodization entirely.
    bool SetLens(
        float aRadius,
        float aFocusDistance,
        float aApodization)
    {
        if(!std::isfinite(aRadius) ||
           !std::isfinite(aFocusDistance) ||
           !std::isfinite(aApodization))
            return false;

        if(aRadius < 0.f || aFocusDistance <= 0.f ||
           aApodization < 0.f || aApodization > 1.f)
            return false;

        if(aRadius > 0.f)
        {
            // Square imaging resolution only for a finite-area pupil.
            if(mResolution.x != mResolution.y)
                return false;

            const float mean = 1.f - 0.5f * aApodization;
            // The normalized area density must stay finite and non-degenerate.
            const float norm = PI_F * Sqr(aRadius) * mean;
            if(mean <= 0.f || !std::isfinite(norm) || !(norm > 0.f))
                return false;
        }

        mHasLens       = aRadius > 0.f;
        mLensRadius    = aRadius;
        mFocusDistance = aFocusDistance;
        mApodization   = aApodization;
        mApoMean       = 1.f - 0.5f * aApodization;
        return true;
    }

    bool HasLens() const
    {
        return mHasLens;
    }

    int RasterToIndex(const Vec2f &aPixelCoords) const
    {
        return int(std::floor(aPixelCoords.x) + std::floor(aPixelCoords.y) * mResolution.x);
    }

    Vec2f IndexToRaster(const int &aPixelIndex) const
    {
        const float y = std::floor(aPixelIndex / mResolution.x);
        const float x = float(aPixelIndex) - y * mResolution.x;
        return Vec2f(x, y);
    }

    Vec3f RasterToWorld(const Vec2f &aRasterXY) const
    {
        return mRasterToWorld.TransformPoint(Vec3f(aRasterXY.x, aRasterXY.y, 0));
    }

    Vec2f WorldToRaster(const Vec3f &aWorldPos) const
    {
        Vec3f temp = mWorldToRaster.TransformPoint(aWorldPos);
        return Vec2f(temp.x, temp.y);
    }

    // returns false when raster position is outside screen space
    bool CheckRaster(const Vec2f &aRasterPos) const
    {
        return aRasterPos.x >= 0 && aRasterPos.y >= 0 &&
            aRasterPos.x < mResolution.x && aRasterPos.y < mResolution.y;
    }

    // Pinhole entrance, kept unchanged. GenerateRay and the R == 0 branch of
    // SampleRay produce component-wise identical rays.
    Ray GenerateRay(const Vec2f &aRasterXY) const
    {
        const Vec3f worldRaster = RasterToWorld(aRasterXY);

        Ray res;
        res.org  = mPosition;
        res.dir  = Normalize(worldRaster - mPosition);
        res.tmin = 0;
        return res;
    }

    // Generates an eye ray. The two random numbers are supplied by the caller;
    // with a pinhole they are ignored and no randomness is consumed.
    CameraRaySample SampleRay(
        const Vec2f &aRaster,
        const Vec2f &aU) const
    {
        CameraRaySample sample;

        // Pinhole direction from the camera center.
        const Vec3f pinholeDir = Normalize(RasterToWorld(aRaster) - mPosition);

        if(!mHasLens)
        {
            sample.ray.org  = mPosition;
            sample.ray.dir  = pinholeDir;
            sample.ray.tmin = 0.f;
            sample.pdfLensA = 1.f;
            sample.response = 1.f;
            sample.pdfW     = DirectionPdfW(Dot(mForward, pinholeDir));
            return sample;
        }

        // Pinhole ray intersects the focus plane.
        const float pinholeCos = Dot(mForward, pinholeDir);
        const Vec3f focusPoint = mPosition +
            pinholeDir * (mFocusDistance / pinholeCos);

        // Importance sample the apodized pupil. Invert the marginal CDF
        // F(t) = (t - k t^2/2)/M; the stable form also covers k = 0.
        const float t       = SampleForwardRadiusParam(aU.x);
        const float r       = mLensRadius * std::sqrt(t);
        const float phi     = 2.f * PI_F * aU.y;
        const Vec3f lensPt  = LensPoint(r, phi);

        sample.ray.org  = lensPt;
        sample.ray.dir  = Normalize(focusPoint - lensPt);
        sample.ray.tmin = 0.f;

        const float transmission = 1.f - mApodization * t;
        sample.response = transmission / mApoMean;
        sample.pdfLensA = transmission / (PI_F * Sqr(mLensRadius) * mApoMean);
        sample.pdfW     = DirectionPdfW(Dot(mForward, sample.ray.dir));
        return sample;
    }

    // Reverse endpoint: uniformly samples the lens disk, connects it to the
    // scene point and re-projects the focus-plane intersection back to raster.
    // On failure oOut is left untouched. Randomness is only consumed via aU.
    bool SampleConnection(
        const Vec3f         &aPoint,
        const Vec2f         &aU,
        CameraConnection    &oOut) const
    {
        Vec3f lensPt;
        float t;
        float lensPdfA;

        if(!mHasLens)
        {
            lensPt   = mPosition;
            t        = 0.f;
            lensPdfA = 1.f;
        }
        else
        {
            // Uniform disk: t = r^2/R^2 is uniformly distributed on [0,1].
            t        = aU.x;
            const float r   = mLensRadius * std::sqrt(aU.x);
            const float phi = 2.f * PI_F * aU.y;
            lensPt          = LensPoint(r, phi);
            lensPdfA        = 1.f / (PI_F * Sqr(mLensRadius));
        }

        const Vec3f toLens = lensPt - aPoint;
        const float distSqr = toLens.LenSqr();
        if(!(distSqr > 0.f) || !std::isfinite(distSqr))
            return false;

        const float distance = std::sqrt(distSqr);
        const Vec3f dirToCamera = toLens / distance;

        // Optical axis cosine of the lens-to-scene direction; rejects points
        // at or behind the lens plane.
        const float cosLens = Dot(mForward, -dirToCamera);
        if(cosLens <= 0.f)
            return false;

        Vec2f raster;
        if(!mHasLens)
        {
            raster = WorldToRaster(aPoint);
        }
        else
        {
            // Ray from the lens point through the scene point intersects the
            // focus plane; project that intersection back through the pinhole.
            const Vec3f sceneDir = -dirToCamera;
            const Vec3f focusPoint = lensPt +
                sceneDir * (mFocusDistance / cosLens);
            raster = WorldToRaster(focusPoint);
        }

        if(!CheckRaster(raster))
            return false;

        oOut.raster             = raster;
        oOut.lensPoint          = lensPt;
        oOut.directionToCamera  = dirToCamera;
        oOut.distance           = distance;
        oOut.pdfLensA           = lensPdfA;
        oOut.pdfW               = DirectionPdfW(cosLens);
        // A pinhole ignores the apodization and books response = 1.
        oOut.response           = mHasLens ?
            (1.f - mApodization * t) / mApoMean : 1.f;
        return true;
    }

public:

    Vec3f mPosition;
    Vec3f mForward;
    Vec2f mResolution;
    Mat4f mRasterToWorld;
    Mat4f mWorldToRaster;
    float mImagePlaneDist;

private:

    void ResetLens()
    {
        mHasLens       = false;
        mLensRadius    = 0.f;
        mFocusDistance = 0.f;
        mApodization   = 0.f;
        mApoMean       = 1.f;
    }

    Vec3f LensPoint(float aRadius, float aPhi) const
    {
        return mPosition + aRadius *
            (std::cos(aPhi) * mRasterAxisX + std::sin(aPhi) * mRasterAxisY);
    }

    // Inversion of the forward marginal lens CDF, stable also for k = 0.
    float SampleForwardRadiusParam(float aUx) const
    {
        const float k = mApodization;
        const float discriminant = 1.f - 2.f * k * mApoMean * aUx;
        return 2.f * mApoMean * aUx / (1.f + std::sqrt(discriminant));
    }

    float DirectionPdfW(float aCosForward) const
    {
        // d^2 / cos^3, evaluated in the same association as the legacy
        // image-plane conversion so pinhole MIS numbers stay bit-identical.
        const float imagePointToCameraDist = mImagePlaneDist / aCosForward;
        return Sqr(imagePointToCameraDist) / aCosForward;
    }

private:

    bool  mHasLens;
    float mLensRadius;
    float mFocusDistance;
    float mApodization;
    float mApoMean;      // M = 1 - k/2
    Vec3f mRasterAxisX;  // raster +x direction in world space
    Vec3f mRasterAxisY;  // raster +y direction in world space
};

#endif //__CAMERA_HXX__
