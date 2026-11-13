/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Regression tests for the complex Constant-Q coefficient editor
    (CQEditor). Three groups:

      adjoint - real inner-product identity <Ax, v> = <x, A* v>
      solve   - matrix-free CG result checked against an independent
                dense elimination built from the real forward path
      editing - band attenuation, phase edits, unedited rebuild,
                silence/edge cases, workspace reuse and honest
                non-convergence reporting
*/

#include "checks.h"
#include "dsp/chromagram/CQEditor.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

// Heap-activity counters for this test binary: analyze()/edit() are
// required to run without any allocation or deallocation after the
// constructor has prepared the plan.
static unsigned long g_newCount = 0;
static unsigned long g_deleteCount = 0;

void *operator new(std::size_t n)
{
    ++g_newCount;
    void *p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void *operator new[](std::size_t n)
{
    ++g_newCount;
    void *p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void *p) noexcept
{
    if (p) { ++g_deleteCount; std::free(p); }
}
void operator delete[](void *p) noexcept
{
    if (p) { ++g_deleteCount; std::free(p); }
}
void operator delete(void *p, std::size_t) noexcept
{
    if (p) { ++g_deleteCount; std::free(p); }
}
void operator delete[](void *p, std::size_t) noexcept
{
    if (p) { ++g_deleteCount; std::free(p); }
}

namespace {

CQConfig makeConfig()
{
    // N = 512, K = 21 with the library's parameter formulas: small
    // enough for an explicit dense reference, still a real sparse
    // Constant-Q kernel.
    CQConfig cfg;
    cfg.FS = 8000.0;
    cfg.min = 500.0;
    cfg.max = 1600.0;
    cfg.BPO = 12;
    cfg.CQThresh = 0.005;
    return cfg;
}

struct Rng {
    unsigned long long s;
    explicit Rng(unsigned long long seed) : s(seed) {}
    double next() {
        // Deterministic LCG, values in [-1, 1].
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        double u = double((s >> 33) & 0x7FFFFFFFull) /
                   double(0x7FFFFFFF);
        return 2.0 * u - 1.0;
    }
};

bool allFinite(const std::vector<double> &v)
{
    for (size_t i = 0; i < v.size(); ++i) {
        if (!std::isfinite(v[i])) return false;
    }
    return true;
}

// Independent dense Gaussian elimination with partial pivoting.
std::vector<double> denseSolve(std::vector< std::vector<double> > a,
                               std::vector<double> b)
{
    const int n = (int)b.size();
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        for (int r = col + 1; r < n; ++r) {
            if (std::fabs(a[r][col]) > std::fabs(a[pivot][col])) {
                pivot = r;
            }
        }
        std::swap(a[pivot], a[col]);
        std::swap(b[pivot], b[col]);
        double d = a[col][col];
        for (int r = 0; r < n; ++r) {
            if (r == col) continue;
            double f = a[r][col] / d;
            if (f == 0.0) continue;
            for (int c = col; c < n; ++c) a[r][c] -= f * a[col][c];
            b[r] -= f * b[col];
        }
    }
    std::vector<double> y(n);
    for (int i = 0; i < n; ++i) y[i] = b[i] / a[i][i];
    return y;
}

void runAdjoint()
{
    CQConfig cfg = makeConfig();

    struct Case { int hop; int length; int trials; };
    Case cases[] = {
        { 512, 200, 3 },   // single frame, segment shorter than N
        { 128, 400, 3 },   // overlapping frames
        { 128, 1, 2 },     // shorter than one window
        { 64,  77, 2 },    // hop does not divide the length
        { 1,   50, 1 }     // minimum allowed hop
    };

    for (size_t ci = 0; ci < sizeof(cases)/sizeof(cases[0]); ++ci) {
        const int hop = cases[ci].hop;
        const int length = cases[ci].length;
        CQEditor ed(cfg, hop, 512);
        const int K = ed.getK();
        const long long M = ed.coefficientCount(length);
        TASK_CHECK(M == (long long)ed.frameCount(length) * K);

        for (int t = 0; t < cases[ci].trials; ++t) {
            Rng rng(0x9e3779b97f4a7c15ULL + ci * 131 + t * 17);

            std::vector<double> x(length);
            for (int i = 0; i < length; ++i) x[i] = rng.next();

            std::vector<double> vRe(M), vIm(M);
            for (long long m = 0; m < M; ++m) {
                // Independent real and imaginary parts: genuinely
                // non-Hermitian, non-symmetric coefficients.
                vRe[m] = rng.next();
                vIm[m] = rng.next();
            }

            std::vector<double> cRe(M), cIm(M);
            TASK_CHECK(ed.analyze(x.data(), length,
                                  cRe.data(), cIm.data()));
            TASK_CHECK(allFinite(cRe) && allFinite(cIm));

            std::vector<double> back(length);
            ed.adjoint(vRe.data(), vIm.data(), length, back.data());
            TASK_CHECK(allFinite(back));

            double lhs = 0.0; // Re <A x, v>
            for (long long m = 0; m < M; ++m) {
                lhs += cRe[m] * vRe[m] + cIm[m] * vIm[m];
            }
            double rhs = 0.0; // <x, A* v> over real PCM space
            for (int i = 0; i < length; ++i) {
                rhs += x[i] * back[i];
            }

            double denom = std::fmax(std::fmax(std::fabs(lhs),
                                               std::fabs(rhs)), 1.0);
            double rel = std::fabs(lhs - rhs) / denom;
            TASK_CHECK(rel <= 1e-10);
        }
    }
}

void runSolve()
{
    CQConfig cfg = makeConfig();
    const double lambda = 0.01;

    struct Case { int hop; int length; };
    Case cases[] = {
        { 512, 250 },  // one frame, L < N
        { 128, 300 }   // overlapping frames, hop not dividing L
    };

    for (size_t ciCase = 0;
         ciCase < sizeof(cases)/sizeof(cases[0]); ++ciCase) {
        const int hop = cases[ciCase].hop;
        const int L = cases[ciCase].length;
        CQEditor ed(cfg, hop, 512);
        const int K = ed.getK();
        const long long M = ed.coefficientCount(L);

        // Build the explicit complex analysis matrix one basis
        // vector at a time, through the real analyze() path.
        std::vector< std::vector<double> > ar(M, std::vector<double>(L));
        std::vector< std::vector<double> > ai(M, std::vector<double>(L));
        std::vector<double> basis(L, 0.0);
        std::vector<double> cr(M), ci(M);
        for (int j = 0; j < L; ++j) {
            basis[j] = 1.0;
            TASK_CHECK(ed.analyze(basis.data(), L,
                                  cr.data(), ci.data()));
            basis[j] = 0.0;
            for (long long m = 0; m < M; ++m) {
                ar[m][j] = cr[m];
                ai[m][j] = ci[m];
            }
        }

        Rng rng(0xd1b54a32d192ed03ULL + ciCase * 97);
        std::vector<double> x(L);
        for (int i = 0; i < L; ++i) x[i] = 0.5 * rng.next();
        std::vector<double> targetRe(M), targetIm(M);
        for (long long m = 0; m < M; ++m) {
            targetRe[m] = 0.7 * rng.next();
            targetIm[m] = 0.7 * rng.next();
        }

        // Dense H = A* A + lambda I and b = A* C + lambda x.
        std::vector< std::vector<double> > H(L, std::vector<double>(L, 0.0));
        std::vector<double> b(L, 0.0);
        for (int j1 = 0; j1 < L; ++j1) {
            for (int j2 = j1; j2 < L; ++j2) {
                double v = 0.0;
                for (long long m = 0; m < M; ++m) {
                    v += ar[m][j1] * ar[m][j2] +
                         ai[m][j1] * ai[m][j2];
                }
                H[j1][j2] = v;
                H[j2][j1] = v;
            }
            H[j1][j1] += lambda;
        }
        for (int j = 0; j < L; ++j) {
            double v = lambda * x[j];
            for (long long m = 0; m < M; ++m) {
                v += ar[m][j] * targetRe[m] +
                     ai[m][j] * targetIm[m];
            }
            b[j] = v;
        }

        std::vector<double> yRef = denseSolve(H, b);

        std::vector<double> y(L);
        int iters = -1;
        double residual = -1.0;
        bool converged = false;
        TASK_CHECK(ed.edit(x.data(), L,
                           targetRe.data(), targetIm.data(),
                           lambda, L + 50, 1e-12,
                           y.data(), &iters, &residual, &converged));
        TASK_CHECK(converged);
        TASK_CHECK(iters >= 1 && iters <= L + 50);
        TASK_CHECK(allFinite(y));
        TASK_CHECK(residual <= 1e-10);

        double maxDiff = 0.0;
        for (int i = 0; i < L; ++i) {
            maxDiff = std::fmax(maxDiff, std::fabs(y[i] - yRef[i]));
        }
        TASK_CHECK(maxDiff <= 1e-7);

        // Residual reported by the solver must match the residual of
        // the actual dense normal equations.
        double normB = 0.0, normR = 0.0;
        for (int i = 0; i < L; ++i) {
            normB += b[i] * b[i];
            double hiy = 0.0;
            for (int j = 0; j < L; ++j) hiy += H[i][j] * y[j];
            double r = hiy - b[i];
            normR += r * r;
        }
        normB = std::sqrt(normB);
        normR = std::sqrt(normR);
        double denseRel = normR / std::fmax(normB, 1e-30);
        TASK_CHECK(std::fabs(denseRel - residual) <=
                   1e-10 + 1e-8 * denseRel);

        // A zero right-hand side must short-circuit to zero.
        std::vector<double> zeroX(L, 0.0), zeroRe(M, 0.0), zeroIm(M, 0.0);
        std::vector<double> yZero(L, 9.0);
        iters = -1; residual = -1.0; converged = true;
        TASK_CHECK(ed.edit(zeroX.data(), L,
                           zeroRe.data(), zeroIm.data(),
                           lambda, 10, 1e-12,
                           yZero.data(), &iters, &residual,
                           &converged));
        TASK_CHECK(converged && iters == 0 && residual == 0.0);
        for (int i = 0; i < L; ++i) TASK_CHECK(yZero[i] == 0.0);
    }
}

void runEditing()
{
    CQConfig cfg = makeConfig();
    const int hop = 128;
    const int maxLen = 512;
    CQEditor ed(cfg, hop, maxLen);
    const int K = ed.getK();
    const int N = ed.getN();
    TASK_CHECK(N == 512 && K == 21);

    // Reject bad arguments before doing any work.
    {
        std::vector<double> x(16, 0.1), y(16);
        std::vector<double> cr(ed.coefficientCount(16), 0.0);
        std::vector<double> ci = cr;
        int iters; double res; bool conv;
        TASK_CHECK(!ed.analyze(0, 16, cr.data(), ci.data()));
        TASK_CHECK(!ed.analyze(x.data(), 0, cr.data(), ci.data()));
        TASK_CHECK(!ed.analyze(x.data(), maxLen + 1,
                               cr.data(), ci.data()));
        TASK_CHECK(!ed.edit(x.data(), 16, cr.data(), ci.data(),
                            0.0, 10, 1e-9, y.data(),
                            &iters, &res, &conv));
        TASK_CHECK(!ed.edit(x.data(), 16, cr.data(), ci.data(),
                            11.0, 10, 1e-9, y.data(),
                            &iters, &res, &conv));
        TASK_CHECK(!ed.edit(x.data(), 16, cr.data(), ci.data(),
                            0.01, 0, 1e-9, y.data(),
                            &iters, &res, &conv));
        TASK_CHECK(!ed.edit(x.data(), 16, cr.data(), ci.data(),
                            0.01, 10, 0.0, y.data(),
                            &iters, &res, &conv));
        TASK_CHECK(!ed.edit(x.data(), 16, cr.data(), ci.data(),
                            0.01, 10, -1.0, y.data(),
                            &iters, &res, &conv));
        std::vector<double> bad = cr;
        bad[0] = std::numeric_limits<double>::infinity();
        TASK_CHECK(!ed.edit(x.data(), 16, bad.data(), ci.data(),
                            0.01, 10, 1e-9, y.data(),
                            &iters, &res, &conv));
        bad[0] = std::numeric_limits<double>::quiet_NaN();
        TASK_CHECK(!ed.edit(x.data(), 16, bad.data(), ci.data(),
                            0.01, 10, 1e-9, y.data(),
                            &iters, &res, &conv));
        std::vector<double> badX = x;
        badX[0] = std::numeric_limits<double>::quiet_NaN();
        TASK_CHECK(!ed.analyze(badX.data(), 16,
                               cr.data(), ci.data()));
    }

    // Dual-tone segment, including a non-divisible length.
    const int L = 401;
    std::vector<double> x(L);
    for (int i = 0; i < L; ++i) {
        x[i] = 0.4 * std::sin(2.0 * M_PI * 600.0 * i / 8000.0)
             + 0.3 * std::cos(2.0 * M_PI * 1100.0 * i / 8000.0);
    }

    const long long M = ed.coefficientCount(L);
    const int frames = ed.frameCount(L);
    std::vector<double> cRe(M), cIm(M);
    TASK_CHECK(ed.analyze(x.data(), L, cRe.data(), cIm.data()));
    TASK_CHECK(frames > 1); // overlapping frames are exercised

    // Inputs must not be modified: keep copies of every argument.
    std::vector<double> xSave = x, cReSave = cRe, cImSave = cIm;

    // Unedited target C = A x must rebuild x (regularisation is on
    // ||y - x||, never lambda * y alone).
    {
        std::vector<double> y(L);
        int iters = 0; double res = 1.0; bool conv = false;
        TASK_CHECK(ed.edit(x.data(), L, cRe.data(), cIm.data(),
                           1e-4, L + 50, 1e-12,
                           y.data(), &iters, &res, &conv));
        TASK_CHECK(conv);
        TASK_CHECK(res <= 1e-10);
        double maxDiff = 0.0;
        for (int i = 0; i < L; ++i) {
            maxDiff = std::fmax(maxDiff, std::fabs(y[i] - x[i]));
        }
        TASK_CHECK(maxDiff <= 1e-7);
    }

    // Attenuate the strongest band (in the first frame) by 12 dB in
    // every frame, then check the re-analysed output follows.
    int strong = 0;
    {
        double best = -1.0;
        for (int k = 0; k < K; ++k) {
            double mag = std::hypot(cRe[k], cIm[k]);
            if (mag > best) { best = mag; strong = k; }
        }
    }
    std::vector<double> attRe = cRe, attIm = cIm;
    for (int f = 0; f < frames; ++f) {
        attRe[(size_t)f * K + strong] *= 0.25;
        attIm[(size_t)f * K + strong] *= 0.25;
    }
    std::vector<double> yAtt(L);
    {
        int iters = 0; double res = 1.0; bool conv = false;
        TASK_CHECK(ed.edit(x.data(), L, attRe.data(), attIm.data(),
                           1e-4, L + 50, 1e-11,
                           yAtt.data(), &iters, &res, &conv));
        TASK_CHECK(conv && allFinite(yAtt));
        std::vector<double> ryRe(M), ryIm(M);
        TASK_CHECK(ed.analyze(yAtt.data(), L,
                              ryRe.data(), ryIm.data()));
        double before = 0.0, after = 0.0;
        for (int f = 0; f < frames; ++f) {
            size_t idx = (size_t)f * K + strong;
            before += std::norm(std::complex<double>(cRe[idx], cIm[idx]));
            after += std::norm(std::complex<double>(ryRe[idx], ryIm[idx]));
        }
        TASK_CHECK(after < 0.5 * before);
    }

    // Phase edit: rotate every target coefficient by a fixed phase.
    std::vector<double> phRe(M), phIm(M);
    {
        const double theta = M_PI / 3.0;
        const double cs = std::cos(theta), sn = std::sin(theta);
        for (long long m = 0; m < M; ++m) {
            phRe[m] = cs * cRe[m] - sn * cIm[m];
            phIm[m] = sn * cRe[m] + cs * cIm[m];
        }
        std::vector<double> y(L);
        int iters = 0; double res = 1.0; bool conv = false;
        TASK_CHECK(ed.edit(x.data(), L, phRe.data(), phIm.data(),
                           1e-4, L + 50, 1e-11,
                           y.data(), &iters, &res, &conv));
        TASK_CHECK(conv && allFinite(y));
        double change = 0.0;
        for (int i = 0; i < L; ++i) {
            change = std::fmax(change, std::fabs(y[i] - x[i]));
        }
        TASK_CHECK(change > 1e-4);

        // The regulariser prevents an exact coefficient fit, so
        // verify the phase edit on the strongest coefficients where
        // the data term dominates: their phases must track the
        // rotated target, both in absolute terms and relative to the
        // unedited coefficients (which are a full theta away).
        std::vector<double> ryRe(M), ryIm(M);
        TASK_CHECK(ed.analyze(y.data(), L, ryRe.data(), ryIm.data()));
        std::vector<long long> order(M);
        for (long long m = 0; m < M; ++m) order[m] = m;
        std::sort(order.begin(), order.end(),
                  [&](long long a, long long b) {
                      return std::hypot(phRe[a], phIm[a]) >
                             std::hypot(phRe[b], phIm[b]);
                  });
        const int top = 20;
        double editedMean = 0.0, originalMean = 0.0;
        for (int t = 0; t < top && t < (int)M; ++t) {
            long long m = order[t];
            double targetAng = std::atan2(phIm[m], phRe[m]);
            double editedAng = std::atan2(ryIm[m], ryRe[m]);
            double origAng = std::atan2(cIm[m], cRe[m]);
            editedMean += std::fabs(std::arg(std::complex<double>(
                std::cos(editedAng - targetAng),
                std::sin(editedAng - targetAng))));
            originalMean += std::fabs(std::arg(std::complex<double>(
                std::cos(origAng - targetAng),
                std::sin(origAng - targetAng))));
        }
        editedMean /= top;
        originalMean /= top;
        TASK_CHECK(editedMean < 0.15);
        TASK_CHECK(editedMean * 5.0 < originalMean);
    }

    // A deliberately tiny iteration cap must report non-convergence,
    // never cache a "success".
    {
        std::vector<double> y(L);
        int iters = 0; double res = 0.0; bool conv = true;
        TASK_CHECK(ed.edit(x.data(), L, attRe.data(), attIm.data(),
                           1e-3, 1, 1e-13,
                           y.data(), &iters, &res, &conv));
        TASK_CHECK(!conv);
        TASK_CHECK(iters == 1);
        TASK_CHECK(res > 1e-13);
    }

    // First/last-sample pulses and silence.
    {
        std::vector<double> pulse(L, 0.0);
        pulse[0] = 1.0;
        pulse[L - 1] = 1.0;
        std::vector<double> cr2(M), ci2(M);
        TASK_CHECK(ed.analyze(pulse.data(), L, cr2.data(), ci2.data()));
        std::vector<double> y(L);
        int iters = 0; double res = 1.0; bool conv = false;
        TASK_CHECK(ed.edit(pulse.data(), L, cr2.data(), ci2.data(),
                           1e-4, L + 50, 1e-12,
                           y.data(), &iters, &res, &conv));
        TASK_CHECK(conv);
        double maxDiff = 0.0;
        for (int i = 0; i < L; ++i) {
            maxDiff = std::fmax(maxDiff, std::fabs(y[i] - pulse[i]));
        }
        TASK_CHECK(maxDiff <= 1e-7);

        // Zero target drives the pulse energy down.
        std::vector<double> zero(M, 0.0);
        TASK_CHECK(ed.edit(pulse.data(), L, zero.data(), zero.data(),
                           0.01, L + 50, 1e-12,
                           y.data(), &iters, &res, &conv));
        TASK_CHECK(conv);
        double peak = 0.0;
        for (int i = 0; i < L; ++i) peak = std::fmax(peak, std::fabs(y[i]));
        TASK_CHECK(peak < 1.0);

        // Silence in, zero target out.
        std::vector<double> silence(L, 0.0);
        TASK_CHECK(ed.edit(silence.data(), L, zero.data(), zero.data(),
                           0.01, 10, 1e-12,
                           y.data(), &iters, &res, &conv));
        TASK_CHECK(conv && iters == 0);
        for (int i = 0; i < L; ++i) TASK_CHECK(y[i] == 0.0);
    }

    // Reuse the one plan over many lengths (including one sample and
    // lengths that do not divide by the hop or end on a half window),
    // each with its own target. Unedited rebuilds must hold and the
    // original arguments must be untouched.
    const int lengths[] = { 1, 2, 3, N / 2, N / 2 + 1, 77, 257, maxLen };
    for (size_t li = 0; li < sizeof(lengths)/sizeof(lengths[0]); ++li) {
        const int Ll = lengths[li];
        std::vector<double> xl(Ll);
        Rng rng(0x1234abcdULL + Ll);
        for (int i = 0; i < Ll; ++i) xl[i] = 0.3 * rng.next();
        const long long Ml = ed.coefficientCount(Ll);
        std::vector<double> crl(Ml), cil(Ml);
        TASK_CHECK(ed.analyze(xl.data(), Ll, crl.data(), cil.data()));
        std::vector<double> xlSave = xl, crlSave = crl, cilSave = cil;
        std::vector<double> yl(Ll);
        int iters = 0; double res = 1.0; bool conv = false;
        TASK_CHECK(ed.edit(xl.data(), Ll, crl.data(), cil.data(),
                           1e-4, Ll + 50, 1e-12,
                           yl.data(), &iters, &res, &conv));
        TASK_CHECK(conv && res <= 1e-9);
        double maxDiff = 0.0;
        for (int i = 0; i < Ll; ++i) {
            maxDiff = std::fmax(maxDiff, std::fabs(yl[i] - xl[i]));
        }
        TASK_CHECK(maxDiff <= 1e-7);
        TASK_CHECK(xl == xlSave && crl == crlSave && cil == cilSave);
    }

    // The very first call's arguments must also be unchanged.
    TASK_CHECK(x == xSave && cRe == cReSave && cIm == cImSave);

    // Repeated analyze()/edit() on a prepared plan must not touch
    // the heap. All buffers used here exist before the counters are
    // snapshotted.
    {
        CQEditor ed2(cfg, hop, maxLen);
        const int Ll = 333;
        const long long Ml = ed2.coefficientCount(Ll);
        std::vector<double> xl(Ll), yl(Ll), rl(Ml), il(Ml);
        Rng rng(0x5151abcdULL);
        for (int i = 0; i < Ll; ++i) xl[i] = 0.25 * rng.next();
        TASK_CHECK(ed2.analyze(xl.data(), Ll, rl.data(), il.data()));

        const unsigned long newBefore = g_newCount;
        const unsigned long delBefore = g_deleteCount;
        TASK_CHECK(ed2.analyze(xl.data(), Ll, rl.data(), il.data()));
        int iters = 0; double res = 1.0; bool conv = false;
        TASK_CHECK(ed2.edit(xl.data(), Ll, rl.data(), il.data(),
                            0.5, 20, 1e-9, yl.data(),
                            &iters, &res, &conv));
        TASK_CHECK(g_newCount == newBefore);
        TASK_CHECK(g_deleteCount == delBefore);
    }

    // Construction must reject illegal hop and max length before
    // returning a plan.
    {
        bool threw = false;
        try { CQEditor bad(cfg, 0, maxLen); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);

        threw = false;
        try { CQEditor bad(cfg, ed.getN() + 1, maxLen); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);

        threw = false;
        try { CQEditor bad(cfg, hop, 0); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);

        CQConfig badCfg = cfg;
        badCfg.min = -1.0;
        threw = false;
        try { CQEditor bad(badCfg, hop, maxLen); }
        catch (const std::invalid_argument &) { threw = true; }
        TASK_CHECK(threw);
    }
}

} // namespace

void task_run(const char *group)
{
    std::string g(group);
    if (g == "adjoint") {
        runAdjoint();
    } else if (g == "solve") {
        runSolve();
    } else if (g == "editing") {
        runEditing();
    } else {
        std::fprintf(stderr, "unknown group %s\n", group);
        std::exit(3);
    }
}
