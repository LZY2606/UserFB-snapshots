// Regression tests for the optional interval-averaging (antialias) modes
// of Overdrive and Wavefolder.
//
// Expected values are produced by an independent double-precision
// reference implemented entirely in this file: the transfer curves are
// re-derived here and interval averages are obtained from high-accuracy
// piecewise Gauss-Legendre integration. No production transfer function
// is called to generate expected values, and no two production objects
// are compared against each other.
#include "harness.h"
#include "Source/Effects/overdrive.h"
#include "Source/Effects/wavefolder.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <vector>

namespace
{
constexpr double PI = 3.14159265358979323846;

// ---------------------------------------------------------------------
// Independent double-precision transfer curves (not production code)
// ---------------------------------------------------------------------
namespace ref
{
    double softclip(double z)
    {
        if(z < -3.0)
            return -1.0;
        if(z > 3.0)
            return 1.0;
        return z * (27.0 + z * z) / (27.0 + 9.0 * z * z);
    }

    // Independent re-derivation of the drive -> gain mapping.
    void overdrive_gains(double drive, double& pre_gain, double& post_gain)
    {
        if(drive < 0.0)
            drive = 0.0;
        if(drive > 1.0)
            drive = 1.0;
        const double d  = 2.0 * drive;
        const double d2 = d * d;
        const double a  = d * 0.5;
        const double b  = d2 * d2 * d * 24.0;
        pre_gain        = a + (b - a) * d2;
        const double sq = d * (2.0 - d);
        post_gain       = 1.0 / softclip(0.33 + sq * (pre_gain - 0.33));
    }

    double overdrive_point(double x, double pre, double post)
    {
        return softclip(pre * x) * post;
    }

    double triangle(double y)
    {
        const double ft = std::floor((y + 1.0) * 0.5);
        const long   q  = static_cast<long>(ft);
        const double sgn = (q % 2 == 0) ? 1.0 : -1.0;
        return sgn * (y - 2.0 * ft);
    }

    double folder_point(double x, double gain, double offset)
    {
        return triangle(gain * (x + offset));
    }

    // Float-precision replicas of the legacy pointwise implementation.
    // The legacy curve classifies fold points with floorf in single
    // precision, so near a fold it can sit on a different branch than the
    // double curve even though the transfer itself is unchanged. Pointwise
    // identity is therefore checked against this independent replica,
    // while the interval-average accuracy is checked in double.
    float softclip_f(float z)
    {
        if(z < -3.0f)
            return -1.0f;
        if(z > 3.0f)
            return 1.0f;
        return z * (27.0f + z * z) / (27.0f + 9.0f * z * z);
    }

    void overdrive_gains_f(float drive, float& pre_gain, float& post_gain)
    {
        if(drive < 0.0f)
            drive = 0.0f;
        if(drive > 1.0f)
            drive = 1.0f;
        const float d  = 2.0f * drive;
        const float d2 = d * d;
        const float a  = d * 0.5f;
        const float b  = d2 * d2 * d * 24.0f;
        pre_gain       = a + (b - a) * d2;
        const float sq = d * (2.0f - d);
        post_gain      = 1.0f / softclip_f(0.33f + sq * (pre_gain - 0.33f));
    }

    float overdrive_point_f(float x, float pre, float post)
    {
        return softclip_f(pre * x) * post;
    }

    float folder_point_f(float x, float gain, float offset)
    {
        float y = (x + offset) * gain;
        float ft = std::floor((y + 1.0f) * 0.5f);
        float sgn = static_cast<int>(ft) % 2 == 0 ? 1.0f : -1.0f;
        return sgn * (y - 2.0f * ft);
    }
} // namespace ref

// ---------------------------------------------------------------------
// 16-point Gauss-Legendre quadrature on [lo, hi], with explicit knots
// at every piecewise boundary of the curve being integrated.
// ---------------------------------------------------------------------
const double GL_X[8] = {0.0950125098376374, 0.2816035507792589,
                        0.4580167776572274, 0.6178762444026438,
                        0.7554044083550030, 0.8656312023878318,
                        0.9445750230732326, 0.9894009349916499};
const double GL_W[8] = {0.1894506104550685, 0.1826034150449236,
                        0.1691565193950025, 0.1495959888165767,
                        0.1246289712555339, 0.0951585116824928,
                        0.0622535239386479, 0.0271524594117541};

double integrate_pieces(const std::function<double(double)>& f,
                        double lo, double hi, std::vector<double> knots)
{
    const bool reversed = hi < lo;
    if(reversed)
    {
        const double t = lo;
        lo = hi;
        hi = t;
    }
    std::vector<double> edges;
    edges.push_back(lo);
    for(double k : knots)
    {
        if(k > lo && k < hi)
            edges.push_back(k);
    }
    edges.push_back(hi);
    double acc = 0.0;
    for(size_t i = 1; i < edges.size(); ++i)
    {
        const double a = edges[i - 1], b = edges[i];
        const double mid = 0.5 * (a + b), half = 0.5 * (b - a);
        for(int j = 0; j < 8; ++j)
        {
            acc += GL_W[j] * half * f(mid + half * GL_X[j]);
            acc += GL_W[j] * half * f(mid - half * GL_X[j]);
        }
    }
    return reversed ? -acc : acc;
}

// Expected interval averages, integrating over the *mapped* interval so
// that the parameter change semantics are exercised directly.
double expected_overdrive(double a, double b, double pre, double post)
{
    const double za = pre * a, zb = pre * b;
    if(za == zb)
        return ref::overdrive_point(b, pre, post);
    const std::vector<double> knots = {-3.0, 3.0};
    return post * integrate_pieces(ref::softclip, za, zb, knots) / (zb - za);
}

double expected_folder(double a, double b, double gain, double offset)
{
    const double ya = gain * (a + offset), yb = gain * (b + offset);
    if(ya == yb)
        return ref::folder_point(b, gain, offset);
    std::vector<double> knots;
    const double        lo = std::fmin(ya, yb), hi = std::fmax(ya, yb);
    for(long k = static_cast<long>(std::floor((lo - 1.0) * 0.5));
        k <= static_cast<long>(std::ceil((hi + 1.0) * 0.5)); ++k)
    {
        const double y = 2.0 * k + 1.0; // every fold point is an odd integer
        if(y > lo && y < hi)
            knots.push_back(y);
    }
    return integrate_pieces(ref::triangle, ya, yb, knots) / (yb - ya);
}

// Deterministic LCG so the run is fully reproducible.
struct Lcg
{
    unsigned long long s = 0x9e3779b97f4a7c15ULL;
    double             unit()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    }
    double between(double lo, double hi) { return lo + (hi - lo) * unit(); }
};

// ---------------------------------------------------------------------
// Minimal radix-2 FFT for the fixed-phase spectral report.
// ---------------------------------------------------------------------
struct Cpx
{
    double re, im;
};

void fft(std::vector<Cpx>& a)
{
    const int n = static_cast<int>(a.size());
    for(int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for(; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if(i < j)
            std::swap(a[i], a[j]);
    }
    for(int len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * PI / len;
        const Cpx    wr{std::cos(ang), std::sin(ang)};
        for(int i = 0; i < n; i += len)
        {
            Cpx w{1.0, 0.0};
            for(int k = 0; k < len / 2; ++k)
            {
                const Cpx u = a[i + k];
                const Cpx v{a[i + k + len / 2].re * w.re
                                - a[i + k + len / 2].im * w.im,
                            a[i + k + len / 2].re * w.im
                                + a[i + k + len / 2].im * w.re};
                a[i + k]              = {u.re + v.re, u.im + v.im};
                a[i + k + len / 2]    = {u.re - v.re, u.im - v.im};
                w                     = {w.re * wr.re - w.im * wr.im,
                                         w.re * wr.im + w.im * wr.re};
            }
        }
    }
}

// Fixed-phase high-frequency sine: integer number of cycles, so bin k0
// contains the whole fundamental and every other bin is non-harmonic.
void spectral_report(const char* name,
                     const std::vector<float>& legacy,
                     const std::vector<float>& averaged,
                     int k0)
{
    const int N = static_cast<int>(legacy.size());
    std::vector<Cpx> Fl(N), Fa(N);
    for(int i = 0; i < N; ++i)
        Fl[i] = {legacy[i], 0.0}, Fa[i] = {averaged[i], 0.0};
    fft(Fl);
    fft(Fa);

    auto energy = [](const Cpx& c) { return c.re * c.re + c.im * c.im; };
    double nonharm_l = 0.0, nonharm_a = 0.0; // full band, minus DC/fundamental
    double upper_l = 0.0, upper_a = 0.0;    // bins k0+2 .. Nyquist-1
    for(int k = 1; k < N / 2; ++k)
    {
        if(k == k0)
            continue;
        nonharm_l += energy(Fl[k]);
        nonharm_a += energy(Fa[k]);
        if(k >= k0 + 2)
        {
            upper_l += energy(Fl[k]);
            upper_a += energy(Fa[k]);
        }
    }

    const double amp_l = std::sqrt(energy(Fl[k0])) * 2.0 / N;
    const double amp_a = std::sqrt(energy(Fa[k0])) * 2.0 / N;
    const double ph_l  = std::atan2(Fl[k0].im, Fl[k0].re);
    const double ph_a  = std::atan2(Fa[k0].im, Fa[k0].re);

    const double w         = 2.0 * PI * k0 / N;
    const double sinc_pred = std::sin(0.5 * w) / (0.5 * w);
    const double amp_ratio = amp_a / amp_l;
    double       phase_diff = ph_a - ph_l;
    while(phase_diff > PI)
        phase_diff -= 2.0 * PI;
    while(phase_diff < -PI)
        phase_diff += 2.0 * PI;

    // Interval averaging of a nonlinear curve is NOT a linear boxcar of
    // the pointwise output: the fundamental amplitude can change by more
    // than sinc predicts (compare the deeply folded wavefolder below), so
    // amplitude is reported rather than checked against sinc. The half
    // sample delay, however, is a robust property: each output is the
    // curve value at the segment midpoint to first order, shifting every
    // component by -w/2 radians regardless of the nonlinearity.
    std::printf("SPECTRAL %s N=%d fundamental_bin=%d (%.4f cycles/sample)\n",
                name, N, k0, static_cast<double>(k0) / N);
    std::printf("SPECTRAL %s nonharmonic full-band energy: legacy=%.6e "
                "averaged=%.6e ratio=%.4f\n",
                name, nonharm_l, nonharm_a, nonharm_a / nonharm_l);
    std::printf("SPECTRAL %s nonharmonic upper-band bins [%d..%d]: "
                "legacy=%.6e averaged=%.6e ratio=%.4f\n",
                name, k0 + 2, N / 2 - 1, upper_l, upper_a,
                upper_a / upper_l);
    std::printf("SPECTRAL %s fundamental amp legacy=%.6f averaged=%.6f "
                "ratio=%.6f (continuous-boxcar sinc would be %.6f)\n",
                name, amp_l, amp_a, amp_ratio, sinc_pred);
    std::printf("SPECTRAL %s fundamental phase legacy=%.6f averaged=%.6f "
                "diff=%.6f rad (half-sample delay pred=%.6f)\n",
                name, ph_l, ph_a, phase_diff, -0.5 * w);

    UF_CHECK(std::isfinite(nonharm_l) && std::isfinite(nonharm_a));
    UF_CHECK(nonharm_l > 0.0 && upper_l > 0.0);
    UF_CHECK(nonharm_a < nonharm_l); // aliasing is attenuated, not removed
    UF_CHECK(upper_a < upper_l);
    UF_CHECK(amp_l > 0.0 && amp_a > 0.0 && amp_ratio < 2.0);
    UF_CHECK(std::fabs(phase_diff + 0.5 * w) < 3.0e-2);
}
} // namespace

// =====================================================================
// Group 1: interval averages of the analytical curves, including every
// numerical edge case called out in the specification.
// =====================================================================
void uf_integral()
{
    Lcg rng;

    // --- Overdrive: random intervals across the whole supported range ---
    {
        daisysp::Overdrive od;
        od.Init();
        od.SetAntialias(true);
        const double drives[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
        for(double drive : drives)
        {
            od.SetDrive(static_cast<float>(drive));
            double pre, post;
            ref::overdrive_gains(drive, pre, post);
            float pre_f, post_f;
            ref::overdrive_gains_f(static_cast<float>(drive), pre_f, post_f);
            for(int it = 0; it < 60; ++it)
            {
                const float a = static_cast<float>(rng.between(-32.0, 32.0));
                const float b = static_cast<float>(rng.between(-32.0, 32.0));
                od.Reset();
                UF_CHECK(std::fabs(od.Process(a)
                                   - ref::overdrive_point_f(a, pre_f, post_f))
                         < 2e-6); // first call after reset: f(b)
                const float got = od.Process(b);
                const double want = expected_overdrive(a, b, pre, post);
                UF_CHECK(std::fabs(got - want) < 3e-5);
                UF_CHECK(std::isfinite(got));
            }
        }
    }

    // Overdrive: ~1e-7 intervals straddling the SoftClip kinks (z = +/-3)
    {
        daisysp::Overdrive od;
        od.Init();
        od.SetAntialias(true);
        od.SetDrive(1.0f);
        double pre, post;
        ref::overdrive_gains(1.0, pre, post);
        float pre_f, post_f;
        ref::overdrive_gains_f(1.0f, pre_f, post_f);
        for(int sign : {-1, 1})
        {
            const float xk = static_cast<float>(sign * 3.0 / pre);
            for(float step : {1e-7f, -1e-7f})
            {
                const float a = xk;
                const float b = xk + step;
                od.Reset();
                UF_CHECK(std::fabs(od.Process(a)
                                   - ref::overdrive_point_f(a, pre_f, post_f))
                         < 2e-6);
                const float  got  = od.Process(b);
                const double want = expected_overdrive(a, b, pre, post);
                UF_CHECK(std::fabs(got - want) < 3e-5);
                // and an interval that crosses from the smooth branch
                // into saturation
                const float c = xk + 2.0f * step;
                od.Reset();
                od.Process(xk - step);
                const float  got2  = od.Process(c);
                const double want2 = expected_overdrive(
                    static_cast<double>(xk - step), c, pre, post);
                UF_CHECK(std::fabs(got2 - want2) < 3e-5);
            }
        }
    }

    // Overdrive: a == b returns f(b); fully saturated intervals.
    {
        daisysp::Overdrive od;
        od.Init();
        od.SetAntialias(true);
        od.SetDrive(0.9f);
        double pre, post;
        ref::overdrive_gains(0.9, pre, post);
        float pre_f, post_f;
        ref::overdrive_gains_f(0.9f, pre_f, post_f);
        od.Reset();
        const float x = 0.7f;
        UF_CHECK(std::fabs(od.Process(x)
                           - ref::overdrive_point_f(x, pre_f, post_f))
                 < 2e-6);
        UF_CHECK(std::fabs(od.Process(x)
                           - ref::overdrive_point_f(x, pre_f, post_f))
                 < 2e-6); // repeated identical input
        const float sat = 20.0f;
        od.Reset();
        od.Process(sat);
        const float  got  = od.Process(31.5f);
        const double want = expected_overdrive(sat, 31.5, pre, post);
        UF_CHECK(std::fabs(got - want) < 3e-5);
        UF_CHECK(std::fabs(got - post) < 3e-5); // saturated at +post_gain
    }

    // --- Wavefolder: random intervals, gains, offsets, sign crossings ---
    {
        daisysp::Wavefolder wf;
        wf.Init();
        wf.SetAntialias(true);
        const double gains[6]   = {64.0, -64.0, 2.5, -3.75, 0.25, 0.0};
        const double offsets[3] = {-8.0, 0.0, 8.0};
        for(double gain : gains)
        {
            for(double offset : offsets)
            {
                const float gain_f   = static_cast<float>(gain);
                const float offset_f = static_cast<float>(offset);
                wf.SetGain(gain_f);
                wf.SetOffset(offset_f);
                for(int it = 0; it < 25; ++it)
                {
                    float a = static_cast<float>(rng.between(-32.0, 32.0));
                    float b = static_cast<float>(rng.between(-32.0, 32.0));
                    if(it % 3 == 0)
                        a = -a * 0.5f; // force intervals crossing zero
                    wf.Reset();
                    UF_CHECK(std::fabs(wf.Process(a)
                                       - ref::folder_point_f(
                                           a, gain_f, offset_f))
                             < 2e-6);
                    const float  got  = wf.Process(b);
                    const double want = expected_folder(a, b, gain, offset);
                    UF_CHECK(std::fabs(got - want) < 3e-5);
                    UF_CHECK(std::fabs(got) <= 1.00001f);
                }
            }
        }
    }

    // Wavefolder: full-range sweep at max gain crosses thousands of folds
    // at constant cost; negative gain reverses the mapped interval.
    {
        daisysp::Wavefolder wf;
        wf.Init();
        wf.SetAntialias(true);
        for(double gain : {64.0, -64.0})
        {
            wf.SetGain(static_cast<float>(gain));
            wf.SetOffset(-1.25f);
            wf.Reset();
            const float a = -32.0f, b = 32.0f;
            wf.Process(a);
            const float  got  = wf.Process(b);
            const double want = expected_folder(a, b, gain, -1.25);
            UF_CHECK(std::fabs(got - want) < 3e-5);
            UF_CHECK(std::fabs(got) <= 1.00001f);
        }
    }

    // Wavefolder: ~1e-7 intervals with an endpoint exactly on a fold point,
    // including a fold point at high gain and in negative territory.
    {
        daisysp::Wavefolder wf;
        wf.Init();
        wf.SetAntialias(true);
        struct Cfg
        {
            double g, o, y;
        };
        Cfg cfgs[4] = {{2.0, 0.0, 1.0}, {2.0, 0.0, -1.0},
                       {64.0, 0.0, 1.0}, {-4.0, 0.5, 3.0}};
        for(Cfg c : cfgs)
        {
            wf.SetGain(static_cast<float>(c.g));
            wf.SetOffset(static_cast<float>(c.o));
            const float xk = static_cast<float>((c.y - c.g * c.o) / c.g);
            for(float step : {1e-7f, -1e-7f})
            {
                wf.Reset();
                wf.Process(xk);
                const float  b    = xk + step;
                const float  got  = wf.Process(b);
                const double want = expected_folder(xk, b, c.g, c.o);
                UF_CHECK(std::fabs(got - want) < 3e-5);
            }
        }
    }

    // Wavefolder: zero gain is a constant curve; a == b returns f(b).
    {
        daisysp::Wavefolder wf;
        wf.Init();
        wf.SetAntialias(true);
        wf.SetGain(0.0f);
        wf.SetOffset(0.75f);
        wf.Reset();
        const float a = -1.5f, b = 2.5f;
      UF_CHECK(std::fabs(wf.Process(a) - ref::folder_point_f(a, 0.0f, 0.75f))
                 < 2e-6);
        UF_CHECK(std::fabs(wf.Process(b)
                           - ref::folder_point_f(b, 0.0f, 0.75f))
                 < 2e-6);
        wf.Reset();
        wf.Process(0.4f);
        UF_CHECK(std::fabs(wf.Process(0.4f)
                           - ref::folder_point_f(0.4f, 0.0f, 0.75f))
                 < 2e-6);
    }

    // --- NaN / infinity: zero output, history retained ---
    {
        daisysp::Overdrive od;
        od.Init();
        od.SetDrive(0.7f);
        od.SetAntialias(true);
        double pre, post;
        ref::overdrive_gains(0.7, pre, post);
        float pre_f, post_f;
        ref::overdrive_gains_f(0.7f, pre_f, post_f);
        const float nan_v = std::numeric_limits<float>::quiet_NaN();
        const float inf_v = std::numeric_limits<float>::infinity();
        UF_CHECK(od.Process(nan_v) == 0.0f); // first call invalid: no history
        const float x0 = 0.4f;
        UF_CHECK(std::fabs(od.Process(x0)
                           - ref::overdrive_point_f(x0, pre_f, post_f))
                 < 2e-6);
        UF_CHECK(od.Process(nan_v) == 0.0f);
        UF_CHECK(od.Process(inf_v) == 0.0f);
        UF_CHECK(od.Process(-inf_v) == 0.0f);
        const float x1 = 0.6f; // interval still starts at x0
        UF_CHECK(std::fabs(od.Process(x1)
                           - expected_overdrive(x0, x1, pre, post))
                 < 3e-5);

        daisysp::Wavefolder wf;
        wf.Init();
        wf.SetAntialias(true);
        wf.SetGain(3.0f);
        wf.Process(-0.5f);
        UF_CHECK(wf.Process(nan_v) == 0.0f);
        UF_CHECK(wf.Process(inf_v) == 0.0f);
        const float y1 = 0.25f;
        UF_CHECK(std::fabs(wf.Process(y1)
                           - expected_folder(-0.5, y1, 3.0, 0.0))
                 < 3e-5);
    }
}

// =====================================================================
// Group 2: parameter changes while running. Both endpoints of every
// interval must be interpreted with the parameters active at the call;
// includes zero crossings and gain sign/zero changes.
// =====================================================================
void uf_modulation()
{
    // Overdrive: drive changes do not clear the interval history.
    {
        daisysp::Overdrive od;
        od.Init();
        od.SetAntialias(true);
        const float seq_x[7]   = {0.30f, -0.55f, 0.80f, -0.10f,
                                  0.65f, -0.90f, 0.12f};
        const float seq_d[7]   = {0.20f, 0.90f, 0.05f, 1.00f,
                                  0.50f, 0.95f, 0.00f};
        float       prev       = seq_x[0];
        for(int i = 0; i < 7; ++i)
        {
            od.SetDrive(seq_d[i]);
            double pre, post;
            ref::overdrive_gains(seq_d[i], pre, post);
            float pre_f, post_f;
            ref::overdrive_gains_f(seq_d[i], pre_f, post_f);
            if(i == 0)
            {
                UF_CHECK(std::fabs(od.Process(seq_x[i])
                                   - ref::overdrive_point_f(
                                       seq_x[i], pre_f, post_f))
                         < 2e-6);
            }
            else
            {
                const float  got  = od.Process(seq_x[i]);
                const double want = expected_overdrive(
                    prev, seq_x[i], pre, post);
                UF_CHECK(std::fabs(got - want) < 3e-5);
            }
            prev = seq_x[i];
        }
    }

    // Wavefolder: gain/offset changes, sign flips and zero gain mid-run.
    {
        daisysp::Wavefolder wf;
        wf.Init();
        wf.SetAntialias(true);
        const float xs[7]     = {0.30f, -0.80f, 1.50f, 0.90f,
                                 -1.20f, 0.10f, -2.00f};
        const float gs[7]     = {2.0f, -3.0f, -3.0f, 0.0f,
                                 64.0f, -64.0f, 1.0f};
        const float os_[7]    = {0.10f, 0.10f, 2.00f, 2.00f,
                                 2.00f, -1.00f, -1.00f};
        float       prev      = xs[0];
        for(int i = 0; i < 7; ++i)
        {
            wf.SetGain(gs[i]);
            wf.SetOffset(os_[i]);
            if(i == 0)
            {
                UF_CHECK(std::fabs(wf.Process(xs[i])
                                   - ref::folder_point_f(
                                       xs[i], gs[i], os_[i]))
                         < 2e-6);
            }
            else
            {
                const float  got  = wf.Process(xs[i]);
                const double want = expected_folder(
                    prev, xs[i], gs[i], os_[i]);
                UF_CHECK(std::fabs(got - want) < 3e-5);
                UF_CHECK(std::fabs(got) <= 1.00001f);
            }
            prev = xs[i];
        }
    }

    // Fixed-phase high-frequency sine through both modules, old vs new.
    {
        const int N = 4096, k0 = 257;
        std::vector<float> in(N), od_l(N), od_a(N), wf_l(N), wf_a(N);
        daisysp::Overdrive d_legacy, d_avg;
        d_legacy.Init();
        d_avg.Init();
        d_legacy.SetDrive(0.8f);
        d_avg.SetDrive(0.8f);
        d_avg.SetAntialias(true);
        daisysp::Wavefolder f_legacy, f_avg;
        f_legacy.Init();
        f_avg.Init();
        f_legacy.SetGain(4.0f);
        f_avg.SetGain(4.0f);
        f_avg.SetAntialias(true);
        for(int i = 0; i < N; ++i)
        {
            in[i] = static_cast<float>(
                1.2 * std::sin(2.0 * PI * k0 * i / N)); // phase fixed at 0
            od_l[i] = d_legacy.Process(in[i]);
            od_a[i] = d_avg.Process(in[i]);
            wf_l[i] = f_legacy.Process(in[i]);
            wf_a[i] = f_avg.Process(in[i]);
            UF_CHECK(std::isfinite(od_a[i]) && std::isfinite(wf_a[i]));
        }
        spectral_report("overdrive_drive0.8", od_l, od_a, k0);
        spectral_report("wavefolder_gain4", wf_l, wf_a, k0);

        // Linear case: a sine that never folds (|y| < 1) makes the
        // wavefolder an identity. The interval average of a linear curve
        // is exactly the arithmetic mean of the endpoints, i.e. the
        // two-tap filter (x[n] + x[n-1]) / 2, whose response is
        // cos(w/2) * exp(-j w/2). (A continuous one-sample boxcar would
        // give sinc(w/2) instead; interval averaging uses the endpoints.)
        {
            const int M = 4096, k1 = 193;
            std::vector<float> lin_l(M), lin_a(M);
            daisysp::Wavefolder l_legacy, l_avg;
            l_legacy.Init();
            l_avg.Init();
            l_legacy.SetGain(1.0f);
            l_avg.SetGain(1.0f);
            l_avg.SetAntialias(true);
            for(int i = 0; i < M; ++i)
            {
                const float x = static_cast<float>(
                    0.5 * std::sin(2.0 * PI * k1 * i / M));
                lin_l[i] = l_legacy.Process(x);
                lin_a[i] = l_avg.Process(x);
            }
            std::vector<Cpx> Fl(M), Fa(M);
            for(int i = 0; i < M; ++i)
                Fl[i] = {lin_l[i], 0.0}, Fa[i] = {lin_a[i], 0.0};
            fft(Fl);
            fft(Fa);
            auto en = [](const Cpx& c) { return c.re * c.re + c.im * c.im; };
            const double ampL = std::sqrt(en(Fl[k1])) * 2.0 / M;
            const double ampA = std::sqrt(en(Fa[k1])) * 2.0 / M;
            const double w1   = 2.0 * PI * k1 / M;
            const double cos1 = std::cos(0.5 * w1);
            double pd = std::atan2(Fa[k1].im, Fa[k1].re)
                        - std::atan2(Fl[k1].im, Fl[k1].re);
            while(pd > PI) pd -= 2.0 * PI;
            while(pd < -PI) pd += 2.0 * PI;
            std::printf("SPECTRAL linear_midpoint N=%d bin=%d amp ratio=%.6f "
                        "(cos pred=%.6f) phase diff=%.6f (pred=%.6f)\n",
                        M, k1, ampA / ampL, cos1, pd, -0.5 * w1);
            UF_CHECK(std::fabs(ampA / ampL - cos1) < 2.0e-4);
            UF_CHECK(std::fabs(pd + 0.5 * w1) < 3.0e-2);
        }
    }
}

// =====================================================================
// Group 3: legacy default behavior, mode toggling and Reset.
// =====================================================================
void uf_compatibility()
{
    // Default mode is off and follows the old pointwise curve exactly.
    {
        daisysp::Overdrive od;
        od.Init();
        daisysp::Wavefolder wf;
        wf.Init();
        od.SetDrive(0.6f);
        wf.SetGain(2.0f);
        wf.SetOffset(0.25f);
        double pre, post;
        ref::overdrive_gains(0.6, pre, post);
        float pre_f, post_f;
        ref::overdrive_gains_f(0.6f, pre_f, post_f);
        Lcg rng;
        for(int i = 0; i < 200; ++i)
        {
            const float x = static_cast<float>(rng.between(-32.0, 32.0));
            UF_CHECK(std::fabs(od.Process(x)
                               - ref::overdrive_point_f(x, pre_f, post_f))
                     < 2e-6);
            UF_CHECK(std::fabs(wf.Process(x)
                               - ref::folder_point_f(x, 2.0f, 0.25f))
                     < 1e-7);
            UF_CHECK(std::fabs(wf.Process(x)) <= 1.00001f);
        }
        UF_CHECK(wf.Process(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
        UF_CHECK(od.Process(std::numeric_limits<float>::infinity()) == 0.0f);
    }

    // Enabling clears history: first averaged sample is f(b), and the
    // averaged output matches the interval reference thereafter.
    {
        daisysp::Wavefolder wf;
        wf.Init();
        wf.SetGain(2.5f);
        wf.Process(0.7f); // legacy history must not exist for averaging
        wf.SetAntialias(true);
        UF_CHECK(std::fabs(wf.Process(0.7f)
                           - ref::folder_point_f(0.7f, 2.5f, 0.0f))
                 < 2e-6);
        const float got = wf.Process(-0.9f);
        UF_CHECK(std::fabs(got - expected_folder(0.7, -0.9, 2.5, 0.0))
                 < 3e-5);

        // Disabling also clears history; output is pointwise again.
        wf.SetAntialias(false);
        UF_CHECK(std::fabs(wf.Process(-0.9f)
                           - ref::folder_point_f(-0.9f, 2.5f, 0.0f))
                 < 3e-7);
    }

    // Re-setting the same mode keeps history; Reset clears only history.
    {
        daisysp::Overdrive od;
        od.Init();
        od.SetDrive(0.75f);
        od.SetAntialias(true);
        double pre, post;
        ref::overdrive_gains(0.75, pre, post);
        float pre_f, post_f;
        ref::overdrive_gains_f(0.75f, pre_f, post_f);
        const float x0 = 0.2f, x1 = -0.6f, x2 = 0.4f;
        od.Process(x0);
        od.SetAntialias(true); // no toggle: history retained
        const float got = od.Process(x1);
        UF_CHECK(std::fabs(got - expected_overdrive(x0, x1, pre, post))
                 < 3e-5);

        od.Reset();
        UF_CHECK(std::fabs(od.Process(x2)
                           - ref::overdrive_point_f(x2, pre_f, post_f))
                 < 2e-6); // first after reset: f(b)
        // parameters and mode survived Reset
        const float got2 = od.Process(x1);
        UF_CHECK(std::fabs(got2 - expected_overdrive(x2, x1, pre, post))
                 < 3e-5);
    }

    // Reset on Wavefolder, including after invalid samples.
    {
        daisysp::Wavefolder wf;
        wf.Init();
        wf.SetGain(-2.0f);
        wf.SetOffset(0.5f);
        wf.SetAntialias(true);
        wf.Process(0.3f);
        wf.Process(std::numeric_limits<float>::infinity());
        wf.Reset();
        UF_CHECK(std::fabs(wf.Process(0.3f)
                           - ref::folder_point_f(0.3f, -2.0f, 0.5f))
                 < 2e-6);
        const float got = wf.Process(-0.4f);
        UF_CHECK(std::fabs(got - expected_folder(0.3, -0.4, -2.0, 0.5))
                 < 3e-5);
    }

    // Averaging genuinely changes nonlinear outputs but never the curve:
    // on a constant input both modes are identical to f(b).
    {
        daisysp::Wavefolder wf_a, wf_l;
        wf_a.Init();
        wf_l.Init();
        wf_a.SetGain(3.5f);
        wf_l.SetGain(3.5f);
        wf_a.SetAntialias(true);
        bool differs = false;
        float prev = 0.0f;
        for(int i = 0; i < 256; ++i)
        {
            const float x = static_cast<float>(
                1.5 * std::sin(2.0 * PI * 31.0 * i / 256.0));
            const float pa = wf_a.Process(x);
            const float pl = wf_l.Process(x);
            if(i > 0 && std::fabs(pa - pl) > 1e-4)
                differs = true;
            if(x == prev)
                UF_CHECK(std::fabs(pa - pl) < 1e-7);
            prev = x;
        }
        UF_CHECK(differs);
    }
}
