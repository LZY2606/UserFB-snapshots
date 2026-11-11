#include "hardsync_ref.h"

#include <algorithm>
#include <cmath>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace hardsync_ref {

namespace {

long long gcdll(long long a, long long b)
{
    while (b) {
        long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// In-place iterative radix-2 FFT.
void fft(std::vector<double> &re, std::vector<double> &im, bool inverse)
{
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * M_PI
                           / static_cast<double>(len);
        const double wr = std::cos(ang);
        const double wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            for (size_t k = 0; k < len / 2; ++k) {
                const size_t a = i + k;
                const size_t b = i + k + len / 2;
                const double tr = cr * re[b] - ci * im[b];
                const double ti = cr * im[b] + ci * re[b];
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
    if (inverse) {
        for (size_t i = 0; i < n; ++i) {
            re[i] /= static_cast<double>(n);
            im[i] /= static_cast<double>(n);
        }
    }
}

// Ideal continuous hard-sync waveform (2*slavePhase-1) at time t.
double continuousWave(double t, double masterHz, double slaveHz)
{
    double phi;
    if (masterHz <= 0.0) {
        phi = slaveHz * t;
    } else {
        const double cycle = std::floor(t * masterHz);
        const double segmentStart = cycle / masterHz;
        phi = slaveHz * (t - segmentStart);
    }
    phi -= std::floor(phi);
    return 2.0 * phi - 1.0;
}

} // namespace

size_t periodSamples(double sampleRate, double masterHz, double slaveHz)
{
    const long long fs = std::llround(sampleRate);
    if (std::fabs(sampleRate - static_cast<double>(fs)) > 1e-9) {
        return 0;
    }
    long long periods[2] = {0, 0};
    double hz[2] = {masterHz, slaveHz};
    for (int k = 0; k < 2; ++k) {
        if (hz[k] == 0.0) {
            periods[k] = 1; // a stationary phase is trivially periodic
            continue;
        }
        const long long h = std::llround(hz[k]);
        if (std::fabs(hz[k] - static_cast<double>(h)) > 1e-9) {
            return 0; // non-integer Hz: no grid-aligned period
        }
        const long long g = gcdll(h, fs);
        periods[k] = fs / g;
    }
    const long long g = gcdll(periods[0], periods[1]);
    const long long lcm = periods[0] / g * periods[1];
    return static_cast<size_t>(lcm);
}

void bandlimited(double sampleRate, double masterHz, double slaveHz,
                 size_t skip, size_t count, double *out)
{
    const size_t N = periodSamples(sampleRate, masterHz, slaveHz);
    // A grid-aligned period is required for the harmonic model.
    if (N == 0) {
        fineAveraged(sampleRate, masterHz, slaveHz, skip, count, 128, out);
        return;
    }

    const double T = static_cast<double>(N) / sampleRate;
    // Dense midpoint quadrature of the exact continuous waveform.
    size_t Q = 1024;
    while (Q < 16 * N) {
        Q <<= 1;
    }

    // Midpoint samples of the exact continuous waveform over one period.
    std::vector<double> re(Q, 0.0), im(Q, 0.0);
    for (size_t q = 0; q < Q; ++q) {
        const double t = (static_cast<double>(q) + 0.5) * T / Q;
        re[q] = continuousWave(t, masterHz, slaveHz);
    }
    fft(re, im, false);

    // Harmonics strictly below Nyquist: k / T < sampleRate / 2.
    const size_t kMax = (N - 1) / 2;

    // DFT coefficients with compensation for the midpoint offset.
    // X[k]/Q ~= (a_k - i b_k) e^{i phi}, so (a_k - i b_k) = (X[k]/Q) e^{-i phi}.
    std::vector<double> ak(kMax + 1, 0.0), bk(kMax + 1, 0.0);
    ak[0] = re[0] / static_cast<double>(Q);
    for (size_t k = 1; k <= kMax; ++k) {
        const double phase = M_PI * static_cast<double>(k) / Q;
        const double cr = std::cos(phase), sr = std::sin(phase);
        const double xr = 2.0 * re[k] / Q;
        const double xi = 2.0 * im[k] / Q;
        ak[k] = xr * cr + xi * sr;
        bk[k] = xr * sr - xi * cr;
    }

    // Re-synthesize at the requested output samples.  Each harmonic is
    // rotated sample by sample so the true DC and every kept harmonic
    // survive without gain/phase/delay fitting.
    for (size_t n = 0; n < count; ++n) {
        out[n] = ak[0];
    }
    for (size_t k = 1; k <= kMax; ++k) {
        const double delta = 2.0 * M_PI * static_cast<double>(k)
                             / static_cast<double>(N);
        double cr = std::cos(delta * static_cast<double>(skip));
        double sr = std::sin(delta * static_cast<double>(skip));
        const double dr = std::cos(delta), ds = std::sin(delta);
        const double a = ak[k], b = bk[k];
        for (size_t n = 0; n < count; ++n) {
            out[n] += a * cr + b * sr;
            const double ncr = cr * dr - sr * ds;
            sr = cr * ds + sr * dr;
            cr = ncr;
        }
    }
}

void naiveHardReset(double sampleRate, double masterHz, double slaveHz,
                    size_t skip, size_t count, double *out)
{
    double masterPhase = 0.0;
    double slavePhase = 0.0;
    const double dm = masterHz / sampleRate;
    const double ds = slaveHz / sampleRate;
    const size_t total = skip + count;
    for (size_t i = 0; i < total; ++i) {
        if (i >= skip) {
            out[i - skip] = 2.0 * slavePhase - 1.0;
        }
        masterPhase += dm;
        slavePhase += ds;
        if (masterPhase >= 1.0) {
            // Reset deferred to the sample boundary, no band limiting.
            masterPhase -= 1.0;
            slavePhase = 0.0;
        } else if (slavePhase >= 1.0) {
            slavePhase -= 1.0;
        }
    }
}

void fineAveraged(double sampleRate, double masterHz, double slaveHz,
                  size_t skip, size_t count, int oversample, double *out)
{
    for (size_t n = 0; n < count; ++n) {
        double acc = 0.0;
        for (int j = 0; j < oversample; ++j) {
            const double t = (static_cast<double>(skip + n)
                              + (static_cast<double>(j) + 0.5) / oversample)
                             / sampleRate;
            acc += continuousWave(t, masterHz, slaveHz);
        }
        out[n] = acc / oversample;
    }
}

} // namespace hardsync_ref
