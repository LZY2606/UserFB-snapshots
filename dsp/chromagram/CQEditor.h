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

#ifndef QM_DSP_CQEDITOR_H
#define QM_DSP_CQEDITOR_H

#include "ConstantQ.h"
#include "dsp/transforms/FFT.h"
#include "base/Window.h"

#include <vector>

/**
 * Complex coefficient editing on top of the sparse Constant-Q
 * analysis used by Chromagram.
 *
 * The analysis operator A for a real segment x of length L is the
 * stack of windowed, fftshifted, full-length FFT frames centred at
 * sample positions 0, hop, 2*hop, ... (all strictly less than L),
 * each followed by the existing ConstantQ::process sparse-kernel
 * path. Every Constant-Q band is retained: no magnitude, no
 * normalisation and no octave folding are applied. A single plan can
 * analyse and edit segments of any length from 1 up to the maximum
 * length given at construction; no heap allocation or deallocation
 * happens during analyze() or edit().
 *
 * edit() returns the unique minimiser of
 *
 *   ||A y - C||^2 + lambda * ||y - x||^2
 *
 * (complex inner products take their real part, PCM space uses the
 * ordinary real inner product), i.e. the normal equations
 *
 *   (A* A + lambda I) y = A* C + lambda x,
 *
 * solved matrix-free with conjugate gradients using only A and A*.
 */
class CQEditor
{
public:
    /**
     * Construct a plan. The Constant-Q kernel and all workspaces are
     * prepared here (this is the only stage allowed to allocate).
     *
     * \param config   Constant-Q configuration, as for ConstantQ.
     * \param hop      frame hop in samples; 1 <= hop <= getN().
     * \param maxLength largest PCM segment length that will be
     *                 analysed or edited; must be >= 1.
     *
     * Throws std::invalid_argument for invalid arguments.
     */
    CQEditor(CQConfig config, int hop, int maxLength);
    ~CQEditor();

    int getN() const { return m_N; }
    int getK() const { return m_K; }
    int getHop() const { return m_hop; }
    int getMaxLength() const { return m_maxLength; }

    /** Number of analysis frames for a segment of length L. */
    int frameCount(int length) const;

    /** Total complex coefficients for a segment of length L. */
    long long coefficientCount(int length) const;

    /**
     * Analyse a non-empty real segment.
     *
     * \param x        length samples, all finite.
     * \param length   1 <= length <= getMaxLength().
     * \param cRe,cIm  receive frameCount(length) * getK() complex
     *                 coefficients, stored frame-major (band k of
     *                 frame f at index f*getK()+k).
     * \return true on success, false if an argument is invalid
     *         (nothing is written in that case).
     */
    bool analyze(const double *x, int length,
                 double *cRe, double *cIm);

    /**
     * Apply the adjoint A*: coefficient buffers in the same shape as
     * analyze()'s output go back to a real length-sample PCM buffer.
     * Exposed so callers (and tests) can verify the operator
     * identity; edit() builds its normal equations from this.
     */
    void adjoint(const double *cRe, const double *cIm, int length,
                 double *out);

    /**
     * Edit a segment so that its Constant-Q coefficients approach a
     * target while staying close to the original PCM.
     *
     * Solves (A* A + lambda I) y = A* C + lambda x with matrix-free
     * conjugate gradients. The right-hand side is recomputed from the
     * actual equation every call; if it is identically zero, y is set
     * to zero directly. The reported relative residual is
     *
     *   ||(A* A + lambda I) y - b|| / max(||b||, 1e-30),
     *   b = A* C + lambda x,
     *
     * evaluated from the actual normal equations. If the iteration
     * limit is reached without meeting the threshold, *converged is
     * set to false (the last iterate is still written, but never
     * reported as success).
     *
     * \param x          original segment, length samples, finite.
     * \param length     1 <= length <= getMaxLength().
     * \param cRe,cIm    target coefficients, same shape as analyze().
     * \param lambda     regularisation, 1e-4 <= lambda <= 10.
     * \param maxIters   CG iteration cap; must be > 0.
     * \param tol        relative residual threshold; must be > 0.
     * \param y          output, length samples.
     * \param itersOut   receives the number of iterations performed.
     * \param residualOut receives the final relative residual.
     * \param converged  receives whether tol was reached.
     * \return true on success, false if an argument is invalid
     *         (nothing is written in that case).
     */
    bool edit(const double *x, int length,
              const double *cRe, const double *cIm,
              double lambda, int maxIters, double tol,
              double *y,
              int *itersOut, double *residualOut, bool *converged);

private:
    CQEditor(const CQEditor &);
    CQEditor &operator=(const CQEditor &);

    bool validLength(int length) const;
    bool finiteAll(const double *p, long long n) const;

    // Whole-segment forward operator (the validated public entry is
    // analyze()).
    void forward(const double *x, int length,
                 double *cRe, double *cIm);
    // out = A* A in + lambda in
    void normalOp(const double *in, int length, double lambda,
                  double *out);

    ConstantQ *m_cq;
    FFT *m_fft;
    Window<double> *m_window;

    int m_N;
    int m_K;
    int m_hop;
    int m_maxLength;
    int m_maxFrames;

    // Preallocated workspaces, sized for the maximum length.
    std::vector<double> m_frameRe;
    std::vector<double> m_frameIm;
    std::vector<double> m_cqRe;
    std::vector<double> m_cqIm;
    std::vector<double> m_r;         // CG residual
    std::vector<double> m_p;         // CG search direction
    std::vector<double> m_ap;        // normal operator output
    std::vector<double> m_tmpCoeffRe; // maxFrames * K
    std::vector<double> m_tmpCoeffIm;
    std::vector<double> m_windowCoeffs; // cached Hamming window
};

#endif
