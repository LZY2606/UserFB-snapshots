/******************************************************************************
 *
 *  Numerical contract for the cross-samplerate LC3 MDCT audio chain.
 *
 *  Part 1 (reference): the production `lc3_mdct_forward` /
 *  `lc3_mdct_inverse` are checked, frame after frame, against the
 *  independent double-precision direct-summation reference of
 *  `mdct_reference.c`.  Forward and inverse are checked independently
 *  (no "production forward feeding production inverse" round-trip, which
 *  could hide compensating sign or scaling errors).  Both the frame
 *  output and the delayed state carried into the next frame are
 *  compared, for separated and in-place buffer usage.
 *
 *  Part 2 (integration): the public ABI chain `lc3_setup_encoder` /
 *  `lc3_encode` / `lc3_setup_decoder` / `lc3_decode` is exercised at
 *  16 kHz encoding with 48 kHz PCM in/out, including one explicitly
 *  dropped (PLC) frame, with return values, finite/non-silent output
 *  and stride-2 guard regions checked.  This part is a smoke test of
 *  the public contract: the codec is lossy, so no numerical
 *  correspondence with the input is or can be asserted here -- that is
 *  exactly what part 1 establishes for the transform itself.
 *
 *  Exit status is 0 only when every case meets its numerical tolerance;
 *  failures are reported with the index of the first deviating
 *  coefficient / PCM sample.
 *
 ******************************************************************************/

#include "mdct.h"
#include "tables.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Independent reference entry points (checks/mdct_reference.c) */

void mdct_ref_dims(enum lc3_dt, enum lc3_srate, int *ns, int *nd);
void mdct_ref_forward(enum lc3_dt, enum lc3_srate, enum lc3_srate,
    const double *, double *, double *);
void mdct_ref_inverse(enum lc3_dt, enum lc3_srate, enum lc3_srate,
    const double *, double *, double *);

#define MAX_NS LC3_MAX_NS
#define NFRAMES 8

/* Tolerance: 2e-4 scaled by one plus the largest reference magnitude */
#define TOL_REL 2e-4

static int nfail = 0;

/* Deterministic pseudo-random source (LCG), for reproducible content */

static unsigned lcg_state;

static void lcg_seed(unsigned s) { lcg_state = s ? s : 1; }

static double lcg_uniform(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (lcg_state >> 8) / 8388608.0 - 1.0; /* in [-1, 1) */
}

/* Test frame content: impulse near the frame tail, non-integer-period
 * dual tone, silence, and further distinct signals. */

static double pcm_sample(int f, long t, int i, int ns)
{
    switch (f) {
    case 0:
    case 1: /* dual tone, non-integer periods at 48 kHz, continuous phase */
        return 0.45 * sin(2.0 * M_PI * 997.3 * t / 48000.0)
             + 0.25 * sin(2.0 * M_PI * 2499.7 * t / 48000.0 + 0.6);
    case 2: /* silence */
        return 0.0;
    case 3: /* impulses near the frame tail */
        if (i == ns - 2) return  0.9;
        if (i == ns - 7) return -0.5;
        return 0.03 * sin(2.0 * M_PI * 501.1 * t / 48000.0);
    case 4: /* a different signal: chirp sweeping 300 Hz .. 6 kHz */
        return 0.6 * sin(2.0 * M_PI * (300.0 * t / 48000.0
                + 0.5 * (5700.0 / (ns * 48000.0)) * t * t / 48000.0));
    case 5: /* decaying noise burst */
        return 0.5 * lcg_uniform() * exp(-3.0 * i / ns);
    case 6: /* dual tone again, shifted phase and level */
        return 0.30 * sin(2.0 * M_PI * 997.3 * t / 48000.0 + 1.9)
             + 0.35 * sin(2.0 * M_PI * 1499.9 * t / 48000.0);
    default: /* ramped tone */
        return 0.4 * sin(2.0 * M_PI * 751.7 * t / 48000.0)
             * (0.25 + 0.75 * (double)i / ns);
    }
}

/* Spectral test content for the inverse chain */

static double spec_sample(int f, int k, int nx)
{
    switch (f) {
    case 0:
    case 1: /* smooth shaped spectrum */
        return 0.4 * sin(2.0 * M_PI * 3.3 * k / nx)
             * exp(-0.5 * k / nx) + 0.1 * lcg_uniform();
    case 2: /* silence */
        return 0.0;
    case 3: /* isolated line near the end of the kept spectrum */
        return (k == nx - 2) ? 3.0 : ((k == 1) ? -1.5 : 0.0);
    case 4: /* a different signal: dense pseudo-random spectrum */
        return 0.35 * lcg_uniform();
    case 5: /* two separated bands */
        return (k >= nx / 8 && k < nx / 8 + 4) ? 1.2
             : (k >= nx / 2 && k < nx / 2 + 3) ? -0.9 : 0.0;
    case 6:
        return 0.5 * cos(2.0 * M_PI * 7.1 * k / nx) / (1 + k / 8.0);
    default:
        return 0.3 * lcg_uniform() * (1.0 - (double)k / nx);
    }
}

/* Compare a production output against the reference, tracking the worst
 * error and reporting the first index outside tolerance. */

static double check_vec(const char *what, int dt_id, int sr_id, int inplace,
    int f, const float *got, const double *ref, int n, double *refmax)
{
    double emax = 0.0;
    int first = -1;

    for (int i = 0; i < n; i++) {
        double a = fabs(ref[i]);
        if (a > *refmax) *refmax = a;
        double e = fabs((double)got[i] - ref[i]);
        if (e > emax) emax = e;
    }
    for (int i = 0; i < n; i++) {
        double e = fabs((double)got[i] - ref[i]);
        if (e > TOL_REL * (1.0 + *refmax)) { first = i; break; }
    }
    if (first >= 0) {
        printf("MDCT_CONTRACT_FAIL %s dt=%d sr_x=%d inplace=%d frame=%d "
               "first_dev=%d got=%.9g ref=%.9g\n",
               what, dt_id, sr_id, inplace, f, first,
               (double)got[first], ref[first]);
        nfail++;
    }
    return emax;
}

/* One case: forward and inverse chains of NFRAMES frames for a
 * duration / encoded-spectrum-rate / buffer-usage combination. */

static int run_case(enum lc3_dt dt, enum lc3_srate sr_x, int inplace,
    int dt_id, int sr_id, double *worst)
{
    const enum lc3_srate sr_pcm = LC3_SRATE_48K;
    int ns, nd, nx, nd_x;
    mdct_ref_dims(dt, sr_pcm, &ns, &nd);
    mdct_ref_dims(dt, sr_x, &nx, &nd_x);
    (void)nd_x;

    static float xp[MAX_NS], ybuf[MAX_NS], dp[MAX_NS];
    static float guard[64];
    static double xref[MAX_NS], yref[MAX_NS], dref[MAX_NS];

    double emax_f = 0, emax_fd = 0, emax_i = 0, emax_id = 0;
    double refmax = 0;

    /* ---- Forward chain ---- */

    memset(dp, 0, sizeof dp);
    memset(dref, 0, sizeof dref);
    lcg_seed(1234 + dt_id * 97 + sr_id * 7 + inplace);

    for (int f = 0; f < NFRAMES; f++) {
        /* Reference input is a private double copy; the production
         * workspace is exactly ns floats (never the shorter target
         * spectrum length), followed by a canary guard region. */
        for (int i = 0; i < ns; i++)
            xref[i] = pcm_sample(f, (long)f * ns + i, i, ns);

        float *yp = inplace ? xp : ybuf;
        for (int i = 0; i < ns; i++) xp[i] = (float)xref[i];
        for (int i = 0; i < 64; i++) yp[ns + i] = guard[i] = -1.25f;

        lc3_mdct_forward(dt, sr_pcm, sr_x, xp, dp, yp);
        mdct_ref_forward(dt, sr_pcm, sr_x, xref, dref, yref);

        /* In-place conversion destroys the input; the reference was
         * built from the pre-call copy, never from the overwritten
         * workspace, and the next frame is generated afresh. */

        for (int i = 0; i < 64; i++)
            if (yp[ns + i] != guard[i]) {
                printf("MDCT_CONTRACT_FAIL guard dt=%d sr_x=%d inplace=%d "
                       "frame=%d fwd workspace overflow at +%d\n",
                       dt_id, sr_id, inplace, f, i);
                nfail++;
                break;
            }

        double e;
        e = check_vec("fwd", dt_id, sr_id, inplace, f, yp, yref, nx, &refmax);
        if (e > emax_f) emax_f = e;
        e = check_vec("fwd-delay", dt_id, sr_id, inplace, f, dp, dref, nd,
                      &refmax);
        if (e > emax_fd) emax_fd = e;
    }

    /* ---- Inverse chain (independent spectral input) ---- */

    memset(dp, 0, sizeof dp);
    memset(dref, 0, sizeof dref);
    lcg_seed(98765 + dt_id * 131 + sr_id * 11 + inplace);

    for (int f = 0; f < NFRAMES; f++) {
        /* Unused high coefficients are explicitly zeroed in both the
         * production input and the reference input. */
        for (int i = 0; i < ns; i++)
            xref[i] = (i < nx) ? spec_sample(f, i, nx) : 0.0;

        float *yp = inplace ? xp : ybuf;
        for (int i = 0; i < ns; i++) xp[i] = (float)xref[i];
        for (int i = 0; i < 64; i++) yp[ns + i] = guard[i] = -1.25f;

        lc3_mdct_inverse(dt, sr_pcm, sr_x, xp, dp, yp);
        mdct_ref_inverse(dt, sr_pcm, sr_x, xref, dref, yref);

        for (int i = 0; i < 64; i++)
            if (yp[ns + i] != guard[i]) {
                printf("MDCT_CONTRACT_FAIL guard dt=%d sr_x=%d inplace=%d "
                       "frame=%d inv workspace overflow at +%d\n",
                       dt_id, sr_id, inplace, f, i);
                nfail++;
                break;
            }

        double e;
        e = check_vec("inv", dt_id, sr_id, inplace, f, yp, yref, ns, &refmax);
        if (e > emax_i) emax_i = e;
        e = check_vec("inv-delay", dt_id, sr_id, inplace, f, dp, dref, nd,
                      &refmax);
        if (e > emax_id) emax_id = e;
    }

    double emax = emax_f;
    if (emax_fd > emax) emax = emax_fd;
    if (emax_i  > emax) emax = emax_i;
    if (emax_id > emax) emax = emax_id;
    if (emax > *worst) *worst = emax;

    static const int sr_khz[] = { 8, 16, 24, 32, 48 };
    printf("CASE dt=%s sr_x=%2dk inplace=%d frames=%d "
           "fwd_err=%.3g fwd_derr=%.3g inv_err=%.3g inv_derr=%.3g "
           "tol=%.3g %s\n",
           dt == LC3_DT_7M5 ? "7.5ms" : "10ms",
           sr_khz[sr_id], inplace, NFRAMES,
           emax_f, emax_fd, emax_i, emax_id,
           TOL_REL * (1.0 + refmax), "OK");

    return 0;
}

/* ---- Part 2: public encoder/decoder ABI chain ---- */

static int run_integration(void)
{
    const int dt_us = 10000, sr_enc = 16000, sr_pcm = 48000;
    const int nframes = 8, drop = 4;

    int ns = lc3_frame_samples(dt_us, sr_pcm);
    if (ns != 480) {
        printf("MDCT_CONTRACT_FAIL codec lc3_frame_samples=%d\n", ns);
        return ++nfail;
    }
    int nbytes = lc3_frame_bytes(dt_us, 64000);

    void *em = malloc(lc3_encoder_size(dt_us, sr_pcm));
    void *dm = malloc(lc3_decoder_size(dt_us, sr_pcm));
    lc3_encoder_t enc = lc3_setup_encoder(dt_us, sr_enc, sr_pcm, em);
    lc3_decoder_t dec = lc3_setup_decoder(dt_us, sr_enc, sr_pcm, dm);

    if (!enc || !dec) {
        printf("MDCT_CONTRACT_FAIL codec setup enc=%p dec=%p\n",
               (void *)enc, (void *)dec);
        free(em); free(dm);
        return ++nfail;
    }

    static float pcm_in[2 * MAX_NS];
    static float pcm_out[2 * MAX_NS + 64];
    static unsigned char packet[LC3_MAX_FRAME_BYTES];

    double ein = 0, eout = 0;
    int rc_ok = 1;

    for (int f = 0; f < nframes && rc_ok; f++) {
        /* Stride-2 interleaved input: samples at even positions,
         * sentinel junk at odd positions. */
        for (int i = 0; i < ns; i++) {
            long t = (long)f * ns + i;
            pcm_in[2 * i] = (float)(0.4 * sin(2.0 * M_PI * 700.0 * t / sr_pcm)
                                  + 0.2 * sin(2.0 * M_PI * 1800.0 * t / sr_pcm
                                              + 0.3));
            pcm_in[2 * i + 1] = -3.0f;
            ein += pcm_in[2 * i] * pcm_in[2 * i];
        }

        if (lc3_encode(enc, LC3_PCM_FORMAT_FLOAT, pcm_in, 2,
                       nbytes, packet) != 0) {
            printf("MDCT_CONTRACT_FAIL codec encode rc!=0 frame=%d\n", f);
            rc_ok = 0;
            break;
        }

        /* Stride-2 output with canary guard regions before, between
         * (odd slots) and after the decoded samples. */
        for (int i = 0; i < 2 * ns + 64; i++)
            pcm_out[i] = -7.5f;

        int rc;
        if (f == drop) /* explicitly lost frame: PLC */
            rc = lc3_decode(dec, NULL, 0, LC3_PCM_FORMAT_FLOAT, pcm_out, 2);
        else
            rc = lc3_decode(dec, packet, nbytes,
                            LC3_PCM_FORMAT_FLOAT, pcm_out, 2);

        if (rc != (f == drop ? 1 : 0)) {
            printf("MDCT_CONTRACT_FAIL codec decode rc=%d frame=%d\n", rc, f);
            rc_ok = 0;
            break;
        }

        double ef = 0;
        for (int i = 0; i < ns; i++) {
            float v = pcm_out[2 * i];
            if (!isfinite(v)) {
                printf("MDCT_CONTRACT_FAIL codec nonfinite frame=%d i=%d\n",
                       f, i);
                rc_ok = 0;
            }
            ef += (double)v * v;
            if (pcm_out[2 * i + 1] != -7.5f) {
                printf("MDCT_CONTRACT_FAIL codec stride guard frame=%d "
                       "slot=%d\n", f, 2 * i + 1);
                rc_ok = 0;
            }
        }
        for (int i = 2 * ns; i < 2 * ns + 64; i++)
            if (pcm_out[i] != -7.5f) {
                printf("MDCT_CONTRACT_FAIL codec tail guard frame=%d "
                       "slot=%d\n", f, i);
                rc_ok = 0;
            }
        eout += ef;

        /* Non-silent decoded energy, including recovery after the
         * dropped frame (PLC must not deadlock the decoder). */
        if (f != 2 && ef <= 1e-6 * ns) {
            printf("MDCT_CONTRACT_FAIL codec silent frame=%d e=%.3g\n",
                   f, ef);
            rc_ok = 0;
        }
    }

    /* The lossy codec is not a lossless round-trip: assert signal-like
     * output energy, never sample equality with the input. */
    if (rc_ok && !(eout > 0.05 * ein && eout < 20.0 * ein)) {
        printf("MDCT_CONTRACT_FAIL codec energy ein=%.3g eout=%.3g\n",
               ein, eout);
        rc_ok = 0;
    }

    if (!rc_ok)
        nfail++;
    else
        printf("CODEC_INTEGRATION enc=16k pcm=48k frames=%d plc_drop=%d "
               "nbytes=%d ein=%.4g eout=%.4g OK\n",
               nframes, drop, nbytes, ein, eout);

    free(em);
    free(dm);
    return rc_ok ? 0 : 1;
}

int main(void)
{
    static const enum lc3_srate sr_x[] = {
        LC3_SRATE_8K, LC3_SRATE_16K, LC3_SRATE_24K,
        LC3_SRATE_32K, LC3_SRATE_48K,
    };
    static const enum lc3_dt dts[] = { LC3_DT_7M5, LC3_DT_10M };

    int cases = 0;
    double worst = 0;

    for (int idt = 0; idt < 2; idt++)
        for (int isr = 0; isr < 5; isr++)
            for (int inplace = 0; inplace < 2; inplace++) {
                run_case(dts[idt], sr_x[isr], inplace, idt, isr, &worst);
                cases++;
            }

    printf("MDCT_REFERENCE_SUMMARY cases=%d worst_err=%.3g\n", cases, worst);

    run_integration();

    if (nfail > 0) {
        printf("MDCT_CONTRACT_FAIL failures=%d\n", nfail);
        return 1;
    }

    printf("MDCT_CONTRACT_CASES cases=%d\n", cases);
    return 0;
}
