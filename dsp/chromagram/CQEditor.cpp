/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    QM DSP Library

    Centre for Digital Music, Queen Mary, University of London.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "CQEditor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

CQEditor::CQEditor(CQConfig config, int hop, int maxLength)
{
    if (!(std::isfinite(config.FS) && config.FS > 0.0) ||
        !(std::isfinite(config.min) && config.min > 0.0) ||
        !(std::isfinite(config.max) && config.max > config.min) ||
        config.BPO < 1 ||
        !std::isfinite(config.CQThresh) || config.CQThresh < 0.0) {
        throw std::invalid_argument
            ("CQEditor: invalid CQConfig");
    }
    if (maxLength < 1) {
        throw std::invalid_argument
            ("CQEditor: maxLength must be at least 1");
    }

    m_cq = new ConstantQ(config);
    m_cq->sparsekernel();

    m_N = m_cq->getFFTLength();
    m_K = m_cq->getK();

    if (hop < 1 || hop > m_N) {
        delete m_cq;
        throw std::invalid_argument
            ("CQEditor: hop must satisfy 1 <= hop <= FFT length");
    }
    m_hop = hop;
    m_maxLength = maxLength;
    m_maxFrames = frameCount(m_maxLength);

    m_fft = new FFT(m_N);
    m_window = new Window<double>(HammingWindow, m_N);
    // Cache the window once: Window::getWindowData() allocates, and
    // analyze()/edit() must not touch the heap.
    m_windowCoeffs = m_window->getWindowData();

    m_frameRe.assign(m_N, 0.0);
    m_frameIm.assign(m_N, 0.0);
    m_cqRe.assign(m_K, 0.0);
    m_cqIm.assign(m_K, 0.0);
    m_r.assign(m_maxLength, 0.0);
    m_p.assign(m_maxLength, 0.0);
    m_ap.assign(m_maxLength, 0.0);
    m_tmpCoeffRe.assign((size_t)m_maxFrames * m_K, 0.0);
    m_tmpCoeffIm.assign((size_t)m_maxFrames * m_K, 0.0);
}

CQEditor::~CQEditor()
{
    delete m_window;
    delete m_fft;
    delete m_cq;
}

int CQEditor::frameCount(int length) const
{
    if (length <= 0) return 0;
    // Frame centres are 0, hop, 2*hop, ..., all strictly less than
    // the segment length.
    return (length - 1) / m_hop + 1;
}

long long CQEditor::coefficientCount(int length) const
{
    return (long long)frameCount(length) * m_K;
}

bool CQEditor::validLength(int length) const
{
    return length >= 1 && length <= m_maxLength;
}

bool CQEditor::finiteAll(const double *p, long long n) const
{
    for (long long i = 0; i < n; ++i) {
        if (!std::isfinite(p[i])) return false;
    }
    return true;
}

void CQEditor::forward(const double *x, int length,
                       double *cRe, double *cIm)
{
    const int half = m_N / 2;
    const int frames = frameCount(length);

    for (int f = 0; f < frames; ++f) {
        const int centre = f * m_hop;

        // Take the N points centred on the frame centre, zero
        // padding outside the segment, exactly as Chromagram does for
        // a complete frame.
        for (int i = 0; i < m_N; ++i) {
            const int g = centre - half + i;
            m_frameRe[i] = (g >= 0 && g < length) ? x[g] : 0.0;
            m_frameIm[i] = 0.0;
        }

        // Same Hamming window as Chromagram, applied in place.
        m_window->cut(m_frameRe.data());

        // Same half-frame fftshift before the full FFT.
        for (int i = 0; i < half; ++i) {
            double tmp = m_frameRe[i];
            m_frameRe[i] = m_frameRe[i + half];
            m_frameRe[i + half] = tmp;
        }

        m_fft->process(false,
                       m_frameRe.data(), m_frameIm.data(),
                       m_frameRe.data(), m_frameIm.data());

        // Existing sparse-kernel path: thresholding, reversed
        // frequency index, skipped DC and FFT scaling all live inside
        // this call. All K bands are kept, complex and unfolded.
        m_cq->process(m_frameRe.data(), m_frameIm.data(),
                      m_cqRe.data(), m_cqIm.data());

        double *outRe = cRe + (size_t)f * m_K;
        double *outIm = cIm + (size_t)f * m_K;
        for (int k = 0; k < m_K; ++k) {
            outRe[k] = m_cqRe[k];
            outIm[k] = m_cqIm[k];
        }
    }
}

void CQEditor::adjoint(const double *cRe, const double *cIm, int length,
                       double *out)
{
    const int half = m_N / 2;
    const int frames = frameCount(length);

    std::fill(out, out + length, 0.0);

    for (int f = 0; f < frames; ++f) {
        const int centre = f * m_hop;

        std::fill(m_frameRe.begin(), m_frameRe.end(), 0.0);
        std::fill(m_frameIm.begin(), m_frameIm.end(), 0.0);

        const double *inRe = cRe + (size_t)f * m_K;
        const double *inIm = cIm + (size_t)f * m_K;
        for (int k = 0; k < m_K; ++k) {
            m_cqRe[k] = inRe[k];
            m_cqIm[k] = inIm[k];
        }

        // Undo the sparse kernel product, including its frequency
        // index reversal and DC skip. Accumulates into the frame
        // frequency buffers.
        m_cq->processAdjoint(m_cqRe.data(), m_cqIm.data(),
                             m_frameRe.data(), m_frameIm.data());

        // Inverse full FFT. The library's forward FFT is unscaled
        // while its inverse carries 1/N, so the true adjoint of the
        // forward transform is N times the library inverse.
        m_fft->process(true,
                       m_frameRe.data(), m_frameIm.data(),
                       m_frameRe.data(), m_frameIm.data());
        for (int i = 0; i < m_N; ++i) {
            m_frameRe[i] *= m_N;
            m_frameIm[i] *= m_N;
        }

        // Undo the half-frame fftshift (the swap is self-inverse).
        for (int i = 0; i < half; ++i) {
            double tmp = m_frameRe[i];
            m_frameRe[i] = m_frameRe[i + half];
            m_frameRe[i + half] = tmp;
            tmp = m_frameIm[i];
            m_frameIm[i] = m_frameIm[i + half];
            m_frameIm[i + half] = tmp;
        }

        // Undo the analysis window and scatter the overlapping frame
        // back to its original sample positions, summing
        // contributions. The adjoint of a real operator is real; the
        // imaginary part only carries round-off and is discarded.
        for (int i = 0; i < m_N; ++i) {
            const int g = centre - half + i;
            if (g >= 0 && g < length) {
                out[g] += m_frameRe[i] * m_windowCoeffs[i];
            }
        }
    }
}

void CQEditor::normalOp(const double *in, int length, double lambda,
                        double *out)
{
    forward(in, length, m_tmpCoeffRe.data(), m_tmpCoeffIm.data());
    adjoint(m_tmpCoeffRe.data(), m_tmpCoeffIm.data(), length, out);
    for (int i = 0; i < length; ++i) {
        out[i] += lambda * in[i];
    }
}

bool CQEditor::analyze(const double *x, int length,
                       double *cRe, double *cIm)
{
    if (!x || !cRe || !cIm || !validLength(length)) return false;
    if (!finiteAll(x, length)) return false;

    forward(x, length, cRe, cIm);
    return true;
}

bool CQEditor::edit(const double *x, int length,
                    const double *cRe, const double *cIm,
                    double lambda, int maxIters, double tol,
                    double *y,
                    int *itersOut, double *residualOut,
                    bool *converged)
{
    if (!x || !cRe || !cIm || !y || !itersOut ||
        !residualOut || !converged) {
        return false;
    }
    if (!validLength(length)) return false;
    if (!finiteAll(x, length)) return false;
    const long long nCoeff = coefficientCount(length);
    if (!finiteAll(cRe, nCoeff) || !finiteAll(cIm, nCoeff)) return false;
    if (!std::isfinite(lambda) || lambda < 1e-4 || lambda > 10.0) {
        return false;
    }
    if (maxIters < 1) return false;
    if (!std::isfinite(tol) || tol <= 0.0) return false;

    // Right-hand side b = A* C + lambda x, built from the actual
    // adjoint path rather than any cached result.
    adjoint(cRe, cIm, length, m_r.data());
    for (int i = 0; i < length; ++i) {
        m_r[i] += lambda * x[i];
    }

    double normB2 = 0.0;
    for (int i = 0; i < length; ++i) normB2 += m_r[i] * m_r[i];

    if (normB2 == 0.0) {
        std::fill(y, y + length, 0.0);
        *itersOut = 0;
        *residualOut = 0.0;
        *converged = true;
        return true;
    }

    const double normB = std::sqrt(normB2);
    const double denomScale = std::max(normB, 1e-30);

    // Conjugate gradients for the SPD normal-equation matrix
    // H = A* A + lambda I. Only A and A* are ever applied.
    std::fill(y, y + length, 0.0);
    std::copy(m_r.begin(), m_r.begin() + length, m_p.begin());

    double rDotOld = normB2;
    int iters = 0;
    bool ok = false;
    double finalRelative = 0.0;

    while (iters < maxIters) {
        normalOp(m_p.data(), length, lambda, m_ap.data());

        double pAp = 0.0;
        for (int i = 0; i < length; ++i) pAp += m_p[i] * m_ap[i];
        if (!(pAp > 0.0) || !std::isfinite(pAp)) {
            // Loss of positive-definiteness through round-off: stop
            // and report honestly rather than returning a fake solve.
            finalRelative = std::sqrt(rDotOld) / denomScale;
            ok = false;
            break;
        }

        const double alpha = rDotOld / pAp;
        for (int i = 0; i < length; ++i) {
            y[i] += alpha * m_p[i];
            m_r[i] -= alpha * m_ap[i];
        }
        ++iters;

        double rDotNew = 0.0;
        for (int i = 0; i < length; ++i) rDotNew += m_r[i] * m_r[i];
        finalRelative = std::sqrt(rDotNew) / denomScale;

        if (finalRelative <= tol) {
            ok = true;
            break;
        }

        const double beta = rDotNew / rDotOld;
        for (int i = 0; i < length; ++i) {
            m_p[i] = m_r[i] + beta * m_p[i];
        }
        rDotOld = rDotNew;
    }

    *itersOut = iters;
    *residualOut = finalRelative;
    *converged = ok;
    return true;
}
