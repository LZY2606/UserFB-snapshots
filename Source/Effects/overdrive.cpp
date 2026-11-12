#include "dsp.h"
#include "overdrive.h"
#include <algorithm>
#include <cmath>

namespace daisysp
{
namespace
{
// Antiderivative of the SoftClip curve, evaluated in double precision so
// that the interval average stays accurate even for very short segments.
// For |z| <= 3, SoftClip(z) = z(z^2+27)/(9z^2+27), whose antiderivative is
//   z^2/18 + (4/3) ln(z^2 + 3).
// The saturated branches (SoftClip = +/-1) are matched at z = +/-3.
constexpr double kClipKink = 0.5 + (4.0 / 3.0) * 2.4849066497880004; // value at |z| = 3
constexpr double kClipSat  = kClipKink - 3.0;

double SoftClipIntegral(double z)
{
    if(z < -3.0)
    {
        return -z + kClipSat;
    }
    if(z > 3.0)
    {
        return z + kClipSat;
    }
    return z * z / 18.0 + (4.0 / 3.0) * std::log(z * z + 3.0);
}
} // namespace

void Overdrive::Init()
{
    SetDrive(.5f);
    antialias_ = false;
    hist_valid_ = false;
    hist_       = 0.0;
}

float Overdrive::Pointwise(float in) const
{
    float pre = pre_gain_ * in;
    return SoftClip(pre) * post_gain_;
}

float Overdrive::Process(float in)
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

    // Both endpoints use the current parameters: the full curve is
    // pre_gain -> SoftClip -> post_gain. With z = pre_gain * x the
    // substitution absorbs the interval length and gain into (zb - za),
    // so zero gain shows up as coincident mapped endpoints.
    const double g    = static_cast<double>(pre_gain_);
    const double post = static_cast<double>(post_gain_);
    const double za   = g * a;
    const double zb   = g * b;
    const double dz   = zb - za;

    if(dz == 0.0)
    {
        // Zero pre-gain makes the curve constant over any interval, so the
        // interval average equals the pointwise value at either endpoint.
        hist_ = b;
        return Pointwise(in);
    }

    const double out = post * (SoftClipIntegral(zb) - SoftClipIntegral(za)) / dz;

    hist_ = b;
    return static_cast<float>(out);
}

void Overdrive::SetDrive(float drive)
{
    drive  = fclamp(drive, 0.f, 1.f);
    drive_ = 2.f * drive;

    const float drive_2    = drive_ * drive_;
    const float pre_gain_a = drive_ * 0.5f;
    const float pre_gain_b = drive_2 * drive_2 * drive_ * 24.0f;
    pre_gain_              = pre_gain_a + (pre_gain_b - pre_gain_a) * drive_2;

    const float drive_squashed = drive_ * (2.0f - drive_);
    post_gain_ = 1.0f / SoftClip(0.33f + drive_squashed * (pre_gain_ - 0.33f));
}

void Overdrive::SetAntialias(bool antialias)
{
    if(antialias != antialias_)
    {
        hist_valid_ = false;
        antialias_  = antialias;
    }
}

void Overdrive::Reset()
{
    hist_valid_ = false;
}

} // namespace daisysp
