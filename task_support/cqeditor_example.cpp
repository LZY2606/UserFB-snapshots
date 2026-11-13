/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Example: complex Constant-Q coefficient editing with CQEditor.

    Builds a short synthetic PCM segment, analyses it into per-frame,
    per-band complex Constant-Q coefficients, attenuates one band,
    solves the whole-segment regularised inverse problem, and writes
    the edited real PCM back out. All error returns are checked.

    Compile with the same recipe as the other task_support programs,
    e.g.:
      source task_support/build.sh
      build_task task_support/cqeditor_example.cpp /tmp/cqeditor_example
*/

#include "dsp/chromagram/CQEditor.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main()
{
    CQConfig config;
    config.FS = 8000.0;
    config.min = 500.0;
    config.max = 1600.0;
    config.BPO = 12;
    config.CQThresh = 0.005;

    const int hop = 128;
    const int maxLength = 512;

    // Construction prepares the sparse Constant-Q kernel, the FFT
    // plans and every workspace; later analyze()/edit() calls never
    // allocate.
    CQEditor editor(config, hop, maxLength);

    const int length = 401; // hop does not have to divide this
    std::vector<double> x(length);
    for (int i = 0; i < length; ++i) {
        x[i] = 0.4 * std::sin(2.0 * M_PI * 600.0 * i / config.FS)
             + 0.3 * std::cos(2.0 * M_PI * 1100.0 * i / config.FS);
    }

    const int frames = editor.frameCount(length);
    const int K = editor.getK();
    const long long coeffs = editor.coefficientCount(length);

    std::vector<double> cRe(coeffs), cIm(coeffs);
    if (!editor.analyze(x.data(), length, cRe.data(), cIm.data())) {
        std::fprintf(stderr, "analyze rejected the input\n");
        return 1;
    }

    // Pick the strongest band of the first frame and attenuate it by
    // 12 dB in every frame.
    int band = 0;
    double best = -1.0;
    for (int k = 0; k < K; ++k) {
        double mag = std::hypot(cRe[k], cIm[k]);
        if (mag > best) { best = mag; band = k; }
    }
    for (int f = 0; f < frames; ++f) {
        size_t idx = (size_t)f * K + band;
        cRe[idx] *= 0.25;
        cIm[idx] *= 0.25;
    }

    std::vector<double> y(length);
    int iters = 0;
    double residual = 0.0;
    bool converged = false;

    const double lambda = 1e-3;     // allowed range [1e-4, 10]
    const int maxIters = length + 50;
    const double tol = 1e-11;       // must be strictly positive

    if (!editor.edit(x.data(), length, cRe.data(), cIm.data(),
                     lambda, maxIters, tol,
                     y.data(), &iters, &residual, &converged)) {
        std::fprintf(stderr,
                     "edit rejected an argument (size, finite-ness, "
                     "lambda or threshold)\n");
        return 1;
    }

    if (!converged) {
        std::fprintf(stderr,
                     "did not converge in %d iterations; final "
                     "relative residual %.6g\n", iters, residual);
        return 2;
    }

    double peak = 0.0;
    for (int i = 0; i < length; ++i) {
        peak = std::fmax(peak, std::fabs(y[i]));
    }

    std::printf("CQEDITOR_EXAMPLE frames=%d bands=%d iters=%d "
                "residual=%.3g outputPeak=%.6g attenuatedBand=%d\n",
                frames, K, iters, residual, peak, band);
    return 0;
}
