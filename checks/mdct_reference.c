/******************************************************************************
 *
 *  Independent numerical reference for the LC3 MDCT/IMDCT audio chain.
 *
 *  This file intentionally does NOT use any of the library transform
 *  machinery (FFT, rotations, folding window loops).  Both directions are
 *  evaluated as direct cosine summations in double precision, over an
 *  explicitly constructed 2*ns time window, with the frame-to-frame
 *  overlap state carried by the caller.  The only data taken from the
 *  library are the window constants of `lc3_mdct_win` (read-only) and the
 *  frame geometry `lc3_ns` / `lc3_nd`.
 *
 *  The full 2*ns analysis/synthesis window is rebuilt from the stored
 *  `ns + nd` coefficients: the remaining `ns - nd` trailing coefficients
 *  of the low-delay window are zero.
 *
 *  Forward (analysis), per frame of `ns` PCM samples:
 *    t[0:nd]      = delayed samples (state), t[nd:nd+ns] = new samples
 *    X[k] = sqrt(2/ns) * sum_m t[m] * W[m] * cos(pi/ns * (m + 1/2 + ns/2)
 *                                                  * (k + 1/2))
 *    kept for k < ns_dst and scaled by sqrt(ns_dst / ns) when the encoded
 *    spectrum samplerate `sr_dst` differs from the PCM samplerate `sr`.
 *
 *  Inverse (synthesis), per frame of `ns_src` spectral coefficients:
 *    coefficients at and above `ns_src` are explicitly zeroed, the
 *    cross-rate scaling sqrt(ns / ns_src) is applied, the spectrum is
 *    extended by odd symmetry X[2ns-1-k] = -X[k], and
 *    t[m] = (1/2) * sqrt(2/ns) * sum_k X[k] * cos(pi/ns * (m + 1/2 + ns/2)
 *                                                    * (k + 1/2))
 *    is windowed by the reversed window and overlap-added with the
 *    delayed tail kept from the previous frame.
 *
 *  The companion contract test (`mdct_contract.c`) declares these entry
 *  points `extern`; no library transform code is linked from this file.
 *
 ******************************************************************************/

#include "tables.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


/* Frame geometry, forwarded from the library characteristic tables */

void mdct_ref_dims(enum lc3_dt dt, enum lc3_srate sr, int *ns, int *nd)
{
    *ns = lc3_ns(dt, sr);
    *nd = lc3_nd(dt, sr);
}

/* Reconstruct the full 2*ns window from the stored coefficients */

static void ref_window(enum lc3_dt dt, enum lc3_srate sr, double *w)
{
    int ns = lc3_ns(dt, sr), nd = lc3_nd(dt, sr);
    const float *win = lc3_mdct_win[dt][sr];

    for (int m = 0; m < 2 * ns; m++)
        w[m] = (m < ns + nd) ? (double)win[m] : 0.0;
}

/* MDCT basis function, shared by both directions */

static double ref_basis(int ns, int m, int k)
{
    return cos(M_PI / ns * (m + 0.5 + ns / 2.0) * (k + 0.5));
}

void mdct_ref_forward(enum lc3_dt dt, enum lc3_srate sr, enum lc3_srate sr_dst,
    const double *x, double *d, double *y)
{
    int ns = lc3_ns(dt, sr), nd = lc3_nd(dt, sr);
    int ns_dst = lc3_ns(dt, sr_dst);

    double t[2 * LC3_MAX_NS], w[2 * LC3_MAX_NS];
    ref_window(dt, sr, w);

    /* Assemble the full 2*ns windowed frame from the overlap state
     * and the new samples, then update the delayed state. */

    for (int i = 0; i < nd; i++) t[i] = d[i];
    for (int i = 0; i < ns; i++) t[nd + i] = x[i];
    for (int i = 0; i < nd; i++) d[i] = x[ns - nd + i];

    for (int m = 0; m < 2 * ns; m++)
        t[m] *= w[m];

    /* Direct cosine summation over the windowed frame, followed by the
     * cross-rate gain applied on the kept spectrum. */

    double g = sqrt(2.0 / ns) * sqrt((double)ns_dst / ns);

    for (int k = 0; k < ns_dst; k++) {
        double s = 0.0;
        for (int m = 0; m < 2 * ns; m++)
            s += t[m] * ref_basis(ns, m, k);
        y[k] = g * s;
    }
}

void mdct_ref_inverse(enum lc3_dt dt, enum lc3_srate sr, enum lc3_srate sr_src,
    const double *x, double *d, double *y)
{
    int ns = lc3_ns(dt, sr), nd = lc3_nd(dt, sr);
    int ns_src = lc3_ns(dt, sr_src);

    double xf[2 * LC3_MAX_NS], t[2 * LC3_MAX_NS], w[2 * LC3_MAX_NS];
    ref_window(dt, sr, w);

    /* Explicitly zero the unused high coefficients, apply the cross-rate
     * gain, and extend the spectrum by odd symmetry. */

    double g = sqrt((double)ns / ns_src);

    for (int k = 0; k < ns; k++)
        xf[k] = (k < ns_src) ? g * x[k] : 0.0;
    for (int k = ns; k < 2 * ns; k++)
        xf[k] = -xf[2 * ns - 1 - k];

    /* Direct cosine summation, windowing by the reversed window. */

    for (int m = 0; m < 2 * ns; m++) {
        double s = 0.0;
        for (int k = 0; k < 2 * ns; k++)
            s += xf[k] * ref_basis(ns, m, k);
        t[m] = 0.5 * sqrt(2.0 / ns) * s * w[2 * ns - 1 - m];
    }

    /* Overlap-add with the delayed tail of the previous frame,
     * and keep the new tail for the next one. */

    for (int i = 0; i < nd; i++) y[i] = t[ns - nd + i] + d[i];
    for (int i = nd; i < ns; i++) y[i] = t[ns - nd + i];
    for (int i = 0; i < nd; i++) d[i] = t[2 * ns - nd + i];
}
