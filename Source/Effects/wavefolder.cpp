#include "wavefolder.h"
#include <math.h>
#include <cmath>

using namespace daisysp;

namespace
{
// Antiderivative of the triangular folding curve, in double precision.
// The pointwise fold is a 4-periodic, zero-mean triangular wave in y
// (y = gain * (x + offset)): it rises on [-1, 1], falls on [1, 3], and
// repeats. A zero-mean periodic function has a periodic antiderivative,
// so only the within-period coordinate (from floor()) is needed. The
// period index itself drops out, which keeps the cost constant for an
// interval that crosses arbitrarily many fold points.
double TriangleIntegral(double y)
{
    const double u = y + 1.0;
    const double n = std::floor(u * 0.25);
    double       s = u - 4.0 * n;
    if(s < 0.0)
    {
        s = 0.0;
    }
    else if(s >= 4.0)
    {
        s = std::nextafter(4.0, 0.0);
    }
    // Periodic integral of the triangle; H(0) = H(4) = 0, so the
    // branches join continuously across period boundaries.
    const double seg = s <= 2.0 ? (0.5 * s * s - s)
                                : (3.0 * s - 0.5 * s * s - 4.0);
    return seg + 0.5;
}
} // namespace

void Wavefolder::Init()
{
    gain_      = 1.0f;
    offset_    = 0.0f;
    antialias_ = false;
    hist_valid_ = false;
    hist_       = 0.0;
}

float Wavefolder::Pointwise(float in) const
{
    float ft, sgn;
    in += offset_;
    in *= gain_;
    ft  = floorf((in + 1.0f) * 0.5f);
    sgn = static_cast<int>(ft) % 2 == 0 ? 1.0f : -1.0f;
    return sgn * (in - 2.0f * ft);
}

float Wavefolder::Process(float in)
{
    if(!std::isfinite(in))
    {
        // Non-finite input: output zero, keep the previous valid input.
        return 0.0f;
    }

    if(!antialias_)
    {
        return Pointwise(in);
    }

    const double b = static_cast<double>(in);
    if(!hist_valid_)
    {
        hist_valid_ = true;
        hist_       = b;
        return Pointwise(in);
    }

    const double a     = hist_;
    const double delta = b - a;
    if(delta == 0.0)
    {
        return Pointwise(in);
    }

    // Both endpoints are interpreted through the current parameters:
    // y = gain * (x + offset), then the triangular fold. Substitution
    // absorbs the interval length and gain into (yb - ya), so negative
    // gain reverses the interval naturally and zero gain shows up as
    // coincident mapped endpoints.
    const double gain   = static_cast<double>(gain_);
    const double offset = static_cast<double>(offset_);
    const double ya     = gain * (a + offset);
    const double yb     = gain * (b + offset);
    const double dy     = yb - ya;

    if(dy == 0.0)
    {
        // Zero gain (or coincident mapped endpoints): the curve is
        // constant over the interval, so the average is the pointwise value.
        hist_ = b;
        return Pointwise(in);
    }

    const double out = (TriangleIntegral(yb) - TriangleIntegral(ya)) / dy;

    hist_ = b;
    return static_cast<float>(out);
}

void Wavefolder::SetAntialias(bool antialias)
{
    if(antialias != antialias_)
    {
        hist_valid_ = false;
        antialias_  = antialias;
    }
}

void Wavefolder::Reset()
{
    hist_valid_ = false;
}
