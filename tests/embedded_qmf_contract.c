/*
 * tests/embedded_qmf_contract.c - C regression contract tests for libg722.
 *
 * Splits "is the bitstream right" from "does the audio sound right":
 *  - embedded:  64/56/48 kbps embedded codeword relations and the shared
 *               low-band adaptation state across the three rates
 *  - analysis:  transmit QMF checked against an independent natural-time
 *               -order convolution plus ITU test mode encoder references
 *  - synthesis: receive QMF checked against ITU test mode decoder subbands
 *               plus an independent causal convolution
 *  - packing:   packed 48/56 kbps paths checked against an independent
 *               LSB-first bit-counting model
 *
 * References are derived from the ITU-T G.722 tables and filter structure;
 * see tests/embedded_qmf_contract.md for provenance.  Only C, libm and
 * deterministic synthetic signals are used - no audio is downloaded.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "task_support/check.h"

#include "g722_private.h"
#include "g722_encoder.h"
#include "g722_decoder.h"

#define PI 3.14159265358979323846

#define MAX_PAIRS 320

/* ------------------------------------------------------------------ */
/* Independent arithmetic: 64-bit accumulate, explicit floor, clamp   */
/* ------------------------------------------------------------------ */

static int floor_div(int32_t a, int32_t b) /* b > 0, floor semantics */
{
    int32_t q = a/b;
    if ((a%b) != 0  &&  a < 0)
        --q;
    return (int) q;
}

static int32_t floor_shift(int64_t v, int bits)
{
    if (v >= 0)
        return (int32_t) (v >> bits);
    return (int32_t) (-(((-v) + ((1LL << bits) - 1)) >> bits));
}

static int16_t clamp16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t) v;
}

/* ------------------------------------------------------------------ */
/* QMF references, written directly as convolutions over the sample   */
/* sequence in natural time order (no 24-cell shuffle loop).          */
/* ------------------------------------------------------------------ */

/* ITU-T G.722 QMF coefficients (24 taps, folded notation). */
static const int ref_qmf[12] =
{
    3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11
};

/* Coefficient applied to the sample delayed by m (m = 0 is newest). */
static int ref_qmf_tap(int m)
{
    return (m & 1)  ?  ref_qmf[(23 - m)/2]  :  ref_qmf[m/2];
}

/*
 * Transmit reference.  t is the index of the newest 16kHz sample (the odd
 * index of the current pair).  Samples before t = 0 are zero.
 */
static void ref_analysis(const int16_t *pcm, long t, int32_t *xlow, int32_t *xhigh)
{
    int64_t lo = 0;
    int64_t hi = 0;
    int m;

    for (m = 0;  m < 24;  m++)
    {
        long idx = t - m;
        int s = (idx >= 0)  ?  pcm[idx]  :  0;
        int c = ref_qmf_tap(m);
        lo += (int64_t) s*c;
        hi += (int64_t) s*((m & 1)  ?  -c  :  c);
    }
    *xlow = floor_shift(lo, 14);
    *xhigh = floor_shift(hi, 14);
}

/*
 * Receive reference.  u is the interleaved subband sequence in natural time
 * order (u[2k] = rlow+rhigh, u[2k+1] = rlow-rhigh).  Output n uses only the
 * taps whose delay parity matches n; the newest u index is 2*(n/2)+1.
 */
static int16_t ref_synthesis(const int32_t *u, long n)
{
    long t = 2*(n/2) + 1;
    int64_t acc = 0;
    int m;

    for (m = 0;  m < 24;  m++)
    {
        long idx;
        if ((m & 1) != (n & 1))
            continue;
        idx = t - m;
        if (idx < 0)
            continue;
        acc += (int64_t) u[idx]*ref_qmf_tap(m);
    }
    return clamp16(floor_shift(acc, 11));
}

/* ------------------------------------------------------------------ */
/* ITU-T G.722 quantizer tables (reference copies for hand-crafted    */
/* codewords: fresh decoder state has det = 32 low / 8 high, s = 0).  */
/* ------------------------------------------------------------------ */

static const int ref_qm2[4] = {-7408, -1616, 7408, 1616};
static const int ref_qm4[16] =
{
         0, -20456, -12896, -8968,
     -6288,  -4240,  -2584, -1200,
     20456,  12896,   8968,  6288,
      4240,   2584,   1200,     0
};
static const int ref_qm5[32] =
{
      -280,   -280, -23352, -17560,
    -14120, -11664,  -9752,  -8184,
     -6864,  -5712,  -4696,  -3784,
     -2960,  -2208,  -1520,   -880,
     23352,  17560,  14120,  11664,
      9752,   8184,   6864,   5712,
      4696,   3784,   2960,   2208,
      1520,    880,    280,   -280
};
static const int ref_qm6[64] =
{
      -136,   -136,   -136,   -136,
    -24808, -21904, -19008, -16704,
    -14984, -13512, -12280, -11192,
    -10232,  -9360,  -8576,  -7856,
     -7192,  -6576,  -6000,  -5456,
     -4944,  -4464,  -4008,  -3576,
     -3168,  -2776,  -2400,  -2032,
     -1688,  -1360,  -1040,   -728,
     24808,  21904,  19008,  16704,
     14984,  13512,  12280,  11192,
     10232,   9360,   8576,   7856,
      7192,   6576,   6000,   5456,
      4944,   4464,   4008,   3576,
      3168,   2776,   2400,   2032,
      1688,   1360,   1040,    728,
       432,    136,   -432,   -136
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void report_mismatch(const char *dir, int rate, long pos, const char *field,
                            long got, long want)
{
    fprintf(stderr, "MISMATCH dir=%s rate=%d pos=%ld field=%s got=%ld want=%ld\n",
            dir, rate, pos, field, got, want);
}

/* Field-by-field comparison of the meaningful band state (never a
   whole-struct memcmp: padding is not state). */
static const char *band_diff(const struct g722_band *a, const struct g722_band *b)
{
    int i;

    if (a->s != b->s) return "s";
    if (a->sp != b->sp) return "sp";
    if (a->sz != b->sz) return "sz";
    for (i = 0;  i < 3;  i++)
    {
        if (a->r[i] != b->r[i]) return "r";
        if (a->a[i] != b->a[i]) return "a";
        if (a->ap[i] != b->ap[i]) return "ap";
        if (a->p[i] != b->p[i]) return "p";
    }
    for (i = 0;  i < 7;  i++)
    {
        if (a->d[i] != b->d[i]) return "d";
        if (a->b[i] != b->b[i]) return "b";
        if (a->bp[i] != b->bp[i]) return "bp";
        if (a->sg[i] != b->sg[i]) return "sg";
    }
    if (a->nb != b->nb) return "nb";
    if (a->det != b->det) return "det";
    return NULL;
}

/* A band that was never driven must still be at its init values. */
static int band_pristine(const struct g722_band *b, int det0)
{
    int i;

    if (b->det != det0  ||  b->nb != 0  ||  b->s != 0  ||  b->sp != 0  ||  b->sz != 0)
        return 0;
    for (i = 0;  i < 3;  i++)
        if (b->r[i]  ||  b->a[i]  ||  b->ap[i]  ||  b->p[i])
            return 0;
    for (i = 0;  i < 7;  i++)
        if (b->d[i]  ||  b->b[i]  ||  b->bp[i]  ||  b->sg[i])
            return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Deterministic test signals                                         */
/* ------------------------------------------------------------------ */

#define SIG_PAIRS 160
#define TAIL_PAIRS 24
#define TOT_PAIRS (SIG_PAIRS + TAIL_PAIRS)

/* Low-amplitude asymmetric dual tone: the alternating term makes even and
   odd sample positions distinguishable.  Tail is silence. */
static void sig_dual_tone(int16_t *pcm, int sig_pairs, int total_pairs)
{
    int i;
    int n = 2*sig_pairs;

    for (i = 0;  i < n;  i++)
    {
        double v = 900.0*sin(2.0*PI*1000.0*(double) i/16000.0)
                 + 400.0*sin(2.0*PI*3300.0*(double) i/16000.0 + 0.7)
                 + ((i & 1)  ?  137.0  :  -137.0);
        pcm[i] = (int16_t) v;
    }
    for (i = n;  i < 2*total_pairs;  i++)
        pcm[i] = 0;
}

/* Deterministic sequence with both signs. */
static void sig_mixed(int16_t *pcm, int n, uint32_t seed)
{
    uint32_t g = seed;
    int i;

    for (i = 0;  i < n;  i++)
    {
        g = g*1664525u + 1013904223u;
        pcm[i] = (int16_t) ((int32_t) (g >> 18) - 8192);
    }
}

static void sig_impulse(int16_t *pcm, int n, int pos, int amp)
{
    int i;

    for (i = 0;  i < n;  i++)
        pcm[i] = 0;
    pcm[pos] = (int16_t) amp;
}

/* ------------------------------------------------------------------ */
/* Shared analysis check: library encoder vs independent convolution  */
/* reference + two continuously running ITU test mode encoders.       */
/* ------------------------------------------------------------------ */

static int analysis_check(const char *tag, const int16_t *pcm, int pairs,
                          const int32_t *xl, const int32_t *xh, int tail_pairs)
{
    static const int chunk_pairs[] = {1, 7, 2, 40, 13, 60};
    int16_t lin[MAX_PAIRS];
    int16_t hin[MAX_PAIRS];
    uint8_t cl[MAX_PAIRS];
    uint8_t ch[MAX_PAIRS];
    uint8_t real[MAX_PAIRS];
    G722_ENC_CTX *eL;
    G722_ENC_CTX *eH;
    G722_ENC_CTX *eR;
    int k;
    int off;
    int ci;
    int total;
    long tail_energy = 0;

    for (k = 0;  k < pairs;  k++)
    {
        int in_range = (xl[k] > -16384  &&  xl[k] < 16383
                        &&  xh[k] > -16384  &&  xh[k] < 16383);
        if (!in_range)
            report_mismatch("analysis", 64000, k, tag, xl[k], xh[k]);
        TASK_CHECK(in_range);
        lin[k] = (int16_t) (2*xl[k]);
        hin[k] = (int16_t) (2*xh[k]);
    }
    eL = g722_encoder_new(64000, 0);
    eH = g722_encoder_new(64000, 0);
    eR = g722_encoder_new(64000, 0);
    TASK_CHECK(eL != NULL  &&  eH != NULL  &&  eR != NULL);
    /* Two independent ITU test mode references: the low-band ADPCM history
       lives in eL, the high-band history in eH.  They are never mixed. */
    eL->itu_test_mode = 1;
    eH->itu_test_mode = 1;
    TASK_CHECK(g722_encode(eL, lin, pairs, cl) == pairs);
    TASK_CHECK(g722_encode(eH, hin, pairs, ch) == pairs);
    /* The real encoder is fed in whole sample-pair chunks, one instance
       for the whole stream. */
    off = 0;
    ci = 0;
    total = 0;
    while (off < pairs)
    {
        int cp = chunk_pairs[ci++%6];
        int n;
        if (cp > pairs - off)
            cp = pairs - off;
        n = g722_encode(eR, pcm + 2*off, 2*cp, real + total);
        TASK_CHECK(n == cp);
        total += n;
        off += cp;
    }
    TASK_CHECK(total == pairs);
    for (k = 0;  k < pairs;  k++)
    {
        int expect = ((ch[k] >> 6) << 6) | (cl[k] & 0x3F);
        if (real[k] != expect)
            report_mismatch("analysis", 64000, k, tag, real[k], expect);
        TASK_CHECK(real[k] == expect);
        if (k >= pairs - tail_pairs)
            tail_energy += labs(xl[k]) + labs(xh[k]);
    }
    /* After the signal cuts to silence the filter tail must still ring. */
    if (tail_pairs > 0)
        TASK_CHECK(tail_energy > 0);
    g722_encoder_destroy(eL);
    g722_encoder_destroy(eH);
    g722_encoder_destroy(eR);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Shared synthesis check: ITU test mode decoder provides the true    */
/* reconstructed subbands; the final PCM comes from the independent   */
/* causal convolution.                                                */
/* ------------------------------------------------------------------ */

static int synthesis_check(const char *tag, const int16_t *pcm, int pairs, int tail_pairs)
{
    uint8_t codes[MAX_PAIRS];
    int16_t sub[2*MAX_PAIRS];
    int16_t out[2*MAX_PAIRS];
    int32_t u[2*MAX_PAIRS];
    G722_ENC_CTX *e;
    G722_DEC_CTX *dI;
    G722_DEC_CTX *dR;
    int k;
    int n;
    long tail_energy = 0;

    e = g722_encoder_new(64000, 0);
    TASK_CHECK(e != NULL);
    TASK_CHECK(g722_encode(e, pcm, 2*pairs, codes) == pairs);
    dI = g722_decoder_new(64000, 0);
    TASK_CHECK(dI != NULL);
    dI->itu_test_mode = 1;
    TASK_CHECK(g722_decode(dI, codes, pairs, sub) == 2*pairs);
    for (k = 0;  k < pairs;  k++)
    {
        int rlow = sub[2*k] >> 1;    /* exact: emitted values are even */
        int rhigh = sub[2*k + 1] >> 1;
        u[2*k] = rlow + rhigh;
        u[2*k + 1] = rlow - rhigh;
    }
    dR = g722_decoder_new(64000, 0);
    TASK_CHECK(dR != NULL);
    TASK_CHECK(g722_decode(dR, codes, pairs, out) == 2*pairs);
    for (n = 0;  n < 2*pairs;  n++)
    {
        int16_t expect = ref_synthesis(u, n);
        if (out[n] != expect)
            report_mismatch("synthesis", 64000, n, tag, out[n], expect);
        TASK_CHECK(out[n] == expect);
        if (n >= 2*(pairs - tail_pairs))
            tail_energy += labs(expect);
    }
    if (tail_pairs > 0)
        TASK_CHECK(tail_energy > 0);
    g722_encoder_destroy(e);
    g722_decoder_destroy(dI);
    g722_decoder_destroy(dR);
    return 0;
}

/* ================================================================== */
/* Group: embedded                                                    */
/* ================================================================== */

/*
 * For one continuous 16kHz stream, every 64kbps codeword shifted right by
 * one/two bits must equal the 56/48kbps (non-packed) codeword.  One encoder
 * instance per rate for the whole stream, fed in whole sample-pair chunks.
 */
static int case_codeword_shift_relation(void)
{
    enum { PAIRS = 200 };
    static const int chunk_pairs[] = {1, 7, 33, 2, 100, 57};
    int16_t pcm[2*PAIRS];
    uint8_t c64[PAIRS];
    uint8_t c56[PAIRS];
    uint8_t c48[PAIRS];
    G722_ENC_CTX *e64;
    G722_ENC_CTX *e56;
    G722_ENC_CTX *e48;
    int k;
    int off;
    int ci;
    int n64 = 0;
    int n56 = 0;
    int n48 = 0;

    sig_dual_tone(pcm, 160, 160);
    sig_mixed(pcm + 320, 80, 7u);

    e64 = g722_encoder_new(64000, 0);
    e56 = g722_encoder_new(56000, 0);
    e48 = g722_encoder_new(48000, 0);
    TASK_CHECK(e64 != NULL  &&  e56 != NULL  &&  e48 != NULL);
    off = 0;
    ci = 0;
    while (off < PAIRS)
    {
        int cp = chunk_pairs[ci++%6];
        if (cp > PAIRS - off)
            cp = PAIRS - off;
        n64 += g722_encode(e64, pcm + 2*off, 2*cp, c64 + n64);
        n56 += g722_encode(e56, pcm + 2*off, 2*cp, c56 + n56);
        n48 += g722_encode(e48, pcm + 2*off, 2*cp, c48 + n48);
        off += cp;
    }
    TASK_CHECK(n64 == PAIRS  &&  n56 == PAIRS  &&  n48 == PAIRS);
    for (k = 0;  k < PAIRS;  k++)
    {
        int shift_ok = (c56[k] == (c64[k] >> 1)  &&  c48[k] == (c64[k] >> 2));
        /* The 4-bit low-band index driving adaptation is identical. */
        int idx_ok = ((c64[k] & 0x3F) >> 2) == ((c56[k] & 0x1F) >> 1)
                  && ((c64[k] & 0x3F) >> 2) == (c48[k] & 0x0F);

        if (!shift_ok)
            report_mismatch("embedded", 56000, k, "codeword_shift",
                            c64[k], ((long) c56[k] << 8) | c48[k]);
        TASK_CHECK(shift_ok);
        if (!idx_ok)
            report_mismatch("embedded", 48000, k, "low_band_index4",
                            (c64[k] & 0x3F) >> 2, c48[k] & 0x0F);
        TASK_CHECK(idx_ok);
    }
    /* Encoder adaptation state must not depend on the rate either. */
    TASK_CHECK(band_diff(&e64->band[0], &e56->band[0]) == NULL);
    TASK_CHECK(band_diff(&e64->band[0], &e48->band[0]) == NULL);
    TASK_CHECK(band_diff(&e64->band[1], &e56->band[1]) == NULL);
    TASK_CHECK(band_diff(&e64->band[1], &e48->band[1]) == NULL);
    g722_encoder_destroy(e64);
    g722_encoder_destroy(e56);
    g722_encoder_destroy(e48);
    return 0;
}

/*
 * The three decoders may output different low-band audio, but the predictor
 * and scale state of both bands must stay in lockstep, codeword by
 * codeword, across the whole stream.
 */
static int case_decoder_state_lockstep(void)
{
    enum { PAIRS = 200 };
    int16_t pcm[2*PAIRS];
    uint8_t c64[PAIRS];
    uint8_t c56[PAIRS];
    uint8_t c48[PAIRS];
    G722_ENC_CTX *e64;
    G722_ENC_CTX *e56;
    G722_ENC_CTX *e48;
    G722_DEC_CTX *d64;
    G722_DEC_CTX *d56;
    G722_DEC_CTX *d48;
    int16_t o64[2];
    int16_t o56[2];
    int16_t o48[2];
    int k;
    int differ = 0;
    const char *f;

    sig_dual_tone(pcm, 160, 160);
    sig_mixed(pcm + 320, 80, 7u);
    e64 = g722_encoder_new(64000, 0);
    e56 = g722_encoder_new(56000, 0);
    e48 = g722_encoder_new(48000, 0);
    TASK_CHECK(g722_encode(e64, pcm, 2*PAIRS, c64) == PAIRS);
    TASK_CHECK(g722_encode(e56, pcm, 2*PAIRS, c56) == PAIRS);
    TASK_CHECK(g722_encode(e48, pcm, 2*PAIRS, c48) == PAIRS);
    d64 = g722_decoder_new(64000, 0);
    d56 = g722_decoder_new(56000, 0);
    d48 = g722_decoder_new(48000, 0);
    TASK_CHECK(d64 != NULL  &&  d56 != NULL  &&  d48 != NULL);
    for (k = 0;  k < PAIRS;  k++)
    {
        TASK_CHECK(g722_decode(d64, c64 + k, 1, o64) == 2);
        TASK_CHECK(g722_decode(d56, c56 + k, 1, o56) == 2);
        TASK_CHECK(g722_decode(d48, c48 + k, 1, o48) == 2);
        f = band_diff(&d64->band[0], &d56->band[0]);
        if (f == NULL)
            f = band_diff(&d64->band[0], &d48->band[0]);
        if (f != NULL)
            fprintf(stderr, "STATE dir=embedded rate=48000 pos=%d field=band0.%s\n", k, f);
        TASK_CHECK(f == NULL);
        f = band_diff(&d64->band[1], &d56->band[1]);
        if (f == NULL)
            f = band_diff(&d64->band[1], &d48->band[1]);
        if (f != NULL)
            fprintf(stderr, "STATE dir=embedded rate=48000 pos=%d field=band1.%s\n", k, f);
        TASK_CHECK(f == NULL);
        if (o64[0] != o48[0]  ||  o64[1] != o48[1])
            differ = 1;
    }
    /* The reconstructed audio does differ across rates (otherwise the
       lockstep above would be vacuous). */
    TASK_CHECK(differ);
    g722_encoder_destroy(e64);
    g722_encoder_destroy(e56);
    g722_encoder_destroy(e48);
    g722_decoder_destroy(d64);
    g722_decoder_destroy(d56);
    g722_decoder_destroy(d48);
    return 0;
}

/*
 * Hand-crafted codewords against the ITU-T reconstruction tables.  A fresh
 * decoder has det = 32 (low) / 8 (high) and s = 0, so the first reconstructed
 * value is exactly floor(det*qm[...]/32768).  Run in ITU test mode so the
 * subband values are observable directly.
 */
static int check_recon(int rate, int code, int exp_rlow, int exp_rhigh)
{
    G722_DEC_CTX *d = g722_decoder_new(rate, 0);
    uint8_t c = (uint8_t) code;
    int16_t out[2];

    TASK_CHECK(d != NULL);
    d->itu_test_mode = 1;
    TASK_CHECK(g722_decode(d, &c, 1, out) == 2);
    if (out[0] != (int16_t) (exp_rlow << 1)  ||  out[1] != (int16_t) (exp_rhigh << 1))
        report_mismatch("embedded", rate, code, "recon_table",
                        ((long) (uint16_t) out[0] << 16) | (uint16_t) out[1],
                        ((long) (uint16_t) (exp_rlow << 1) << 16) | (uint16_t) (exp_rhigh << 1));
    TASK_CHECK(out[0] == (int16_t) (exp_rlow << 1));
    TASK_CHECK(out[1] == (int16_t) (exp_rhigh << 1));
    g722_decoder_destroy(d);
    return 0;
}

static int case_reconstruction_tables(void)
{
    static const int idx6[] = {1, 13, 32, 60};
    static const int idx5[] = {1, 9, 16, 28};
    static const int idx4[] = {1, 5, 8, 13};
    int t;

    for (t = 0;  t < 4;  t++)
    {
        int ih = t;
        /* 64 kbps: code = (ih<<6)|il6, low band reconstructs via qm6 */
        TASK_CHECK(check_recon(64000, (ih << 6) | idx6[t],
                               floor_div(32*ref_qm6[idx6[t]], 32768),
                               floor_div(8*ref_qm2[ih], 32768)) == 0);
        /* 56 kbps: code = (ih<<5)|il5, low band reconstructs via qm5 */
        TASK_CHECK(check_recon(56000, (ih << 5) | idx5[t],
                               floor_div(32*ref_qm5[idx5[t]], 32768),
                               floor_div(8*ref_qm2[ih], 32768)) == 0);
        /* 48 kbps: code = (ih<<4)|il4, low band reconstructs via qm4 */
        TASK_CHECK(check_recon(48000, (ih << 4) | idx4[t],
                               floor_div(32*ref_qm4[idx4[t]], 32768),
                               floor_div(8*ref_qm2[ih], 32768)) == 0);
    }
    return 0;
}

/*
 * The low-band refinement bits (the low two bits of the 6-bit low-band
 * index) change the reconstructed value but not the prediction history:
 * two decoders that differ only in those bits must keep identical band
 * state and identical subsequent output.
 */
static int case_refinement_bits(void)
{
    enum { PAIRS = 60, PREFIX = 40 };
    int16_t pcm[2*PAIRS];
    uint8_t codes[PAIRS];
    G722_ENC_CTX *e;
    G722_DEC_CTX *dA;
    G722_DEC_CTX *dB;
    int16_t oA[2*PAIRS];
    int16_t oB[2*PAIRS];
    uint8_t refine0 = 0x60;  /* ihigh = 1, ilow top4 = 8, refinement 0 */
    uint8_t refine3 = 0x63;  /* ihigh = 1, ilow top4 = 8, refinement 3 */
    int k;
    const char *f;

    sig_dual_tone(pcm, PAIRS, PAIRS);
    e = g722_encoder_new(64000, 0);
    TASK_CHECK(e != NULL);
    TASK_CHECK(g722_encode(e, pcm, 2*PAIRS, codes) == PAIRS);
    dA = g722_decoder_new(64000, 0);
    dB = g722_decoder_new(64000, 0);
    TASK_CHECK(dA != NULL  &&  dB != NULL);
    dA->itu_test_mode = 1;
    dB->itu_test_mode = 1;
    TASK_CHECK(g722_decode(dA, codes, PREFIX, oA) == 2*PREFIX);
    TASK_CHECK(g722_decode(dB, codes, PREFIX, oB) == 2*PREFIX);
    TASK_CHECK(dA->band[0].det > 64);
    TASK_CHECK(g722_decode(dA, &refine0, 1, oA + 2*PREFIX) == 2);
    TASK_CHECK(g722_decode(dB, &refine3, 1, oB + 2*PREFIX) == 2);
    /* ... the reconstructed value changes ... */
    TASK_CHECK(oA[2*PREFIX] != oB[2*PREFIX]);
    /* ... the prediction history does not ... */
    f = band_diff(&dA->band[0], &dB->band[0]);
    if (f != NULL)
        fprintf(stderr, "STATE dir=embedded rate=64000 pos=%d field=band0.%s\n", PREFIX, f);
    TASK_CHECK(f == NULL);
    f = band_diff(&dA->band[1], &dB->band[1]);
    if (f != NULL)
        fprintf(stderr, "STATE dir=embedded rate=64000 pos=%d field=band1.%s\n", PREFIX, f);
    TASK_CHECK(f == NULL);
    /* ... and neither does anything after it. */
    TASK_CHECK(g722_decode(dA, codes + PREFIX, PAIRS - PREFIX, oA + 2*PREFIX + 2) == 2*(PAIRS - PREFIX));
    TASK_CHECK(g722_decode(dB, codes + PREFIX, PAIRS - PREFIX, oB + 2*PREFIX + 2) == 2*(PAIRS - PREFIX));
    for (k = 2*PREFIX + 2;  k < 2*PAIRS;  k++)
        TASK_CHECK(oA[k] == oB[k]);
    TASK_CHECK(band_diff(&dA->band[0], &dB->band[0]) == NULL);
    TASK_CHECK(band_diff(&dA->band[1], &dB->band[1]) == NULL);
    g722_encoder_destroy(e);
    g722_decoder_destroy(dA);
    g722_decoder_destroy(dB);
    return 0;
}

/* ================================================================== */
/* Group: analysis                                                    */
/* ================================================================== */

/*
 * Impulses on even and odd sample positions.  Hand-computed anchors pin
 * down tap direction, phase, scaling and floor behaviour of the transmit
 * reference; the full codeword stream is then checked against it.
 */
static int case_analysis_impulse_phase(void)
{
    enum { PAIRS = 40 };
    int16_t pcm[2*PAIRS];
    int32_t xl[PAIRS];
    int32_t xh[PAIRS];
    int k;

    /* Impulse on an even sample position (index 10, pair 5). */
    sig_impulse(pcm, 2*PAIRS, 10, 6000);
    for (k = 0;  k < PAIRS;  k++)
        ref_analysis(pcm, 2L*k + 1, &xl[k], &xh[k]);
    /* pair 5, delay 1, tap -11: lo = floor(-66000/16384) = -5,
       hi = floor(+66000/16384) = 4 */
    TASK_CHECK(xl[5] == -5  &&  xh[5] == 4);
    /* pair 6, delay 3, tap +53: lo = floor(318000/16384) = 19,
       hi = floor(-318000/16384) = -20 (floor, not truncation) */
    TASK_CHECK(xl[6] == 19  &&  xh[6] == -20);
    /* pair 16, delay 23, tap +3: lo = 1, hi = floor(-18000/16384) = -2 */
    TASK_CHECK(xl[16] == 1  &&  xh[16] == -2);
    for (k = 17;  k < PAIRS;  k++)
        TASK_CHECK(xl[k] == 0  &&  xh[k] == 0);
    TASK_CHECK(analysis_check("impulse_even", pcm, PAIRS, xl, xh, 0) == 0);

    /* Impulse on an odd sample position (index 11, pair 5). */
    sig_impulse(pcm, 2*PAIRS, 11, 6000);
    for (k = 0;  k < PAIRS;  k++)
        ref_analysis(pcm, 2L*k + 1, &xl[k], &xh[k]);
    /* pair 5, delay 0, tap +3: both lanes floor(18000/16384) = 1 */
    TASK_CHECK(xl[5] == 1  &&  xh[5] == 1);
    /* pair 6, delay 2, tap -11: both lanes floor(-66000/16384) = -5 */
    TASK_CHECK(xl[6] == -5  &&  xh[6] == -5);
    /* pair 16, delay 22, tap -11: both lanes -5 */
    TASK_CHECK(xl[16] == -5  &&  xh[16] == -5);
    for (k = 17;  k < PAIRS;  k++)
        TASK_CHECK(xl[k] == 0  &&  xh[k] == 0);
    TASK_CHECK(analysis_check("impulse_odd", pcm, PAIRS, xl, xh, 0) == 0);
    return 0;
}

/* Startup, a continuous segment, and the filter tail after the signal
   cuts to silence - codeword by codeword. */
static int case_analysis_dual_tone(void)
{
    int16_t pcm[2*TOT_PAIRS];
    int32_t xl[TOT_PAIRS];
    int32_t xh[TOT_PAIRS];
    int k;

    sig_dual_tone(pcm, SIG_PAIRS, TOT_PAIRS);
    for (k = 0;  k < TOT_PAIRS;  k++)
        ref_analysis(pcm, 2L*k + 1, &xl[k], &xh[k]);
    return analysis_check("dual_tone", pcm, TOT_PAIRS, xl, xh, TAIL_PAIRS);
}

static int case_analysis_mixed_sequence(void)
{
    int16_t pcm[2*TOT_PAIRS];
    int32_t xl[TOT_PAIRS];
    int32_t xh[TOT_PAIRS];
    int k;

    sig_mixed(pcm, 2*SIG_PAIRS, 99u);
    for (k = 2*SIG_PAIRS;  k < 2*TOT_PAIRS;  k++)
        pcm[k] = 0;
    for (k = 0;  k < TOT_PAIRS;  k++)
        ref_analysis(pcm, 2L*k + 1, &xl[k], &xh[k]);
    return analysis_check("mixed", pcm, TOT_PAIRS, xl, xh, TAIL_PAIRS);
}

/*
 * 8kHz mode: the QMF is bypassed, so the low band is just the ADPCM of
 * amp>>1 (cross-checked against an ITU test mode reference fed the same
 * samples) and the high-band state must never be touched.
 */
static int case_eight_k_bypass(void)
{
    enum { N = 120 };
    int16_t pcm[N];
    int16_t o8[N];
    int16_t io[2*N];
    uint8_t c8[N];
    uint8_t ci[N];
    G722_ENC_CTX *e8;
    G722_ENC_CTX *eI;
    G722_DEC_CTX *d8;
    G722_DEC_CTX *dI;
    int k;

    sig_mixed(pcm, N, 5u);
    e8 = g722_encoder_new(64000, G722_SAMPLE_RATE_8000);
    eI = g722_encoder_new(64000, 0);
    TASK_CHECK(e8 != NULL  &&  eI != NULL);
    eI->itu_test_mode = 1;
    TASK_CHECK(g722_encode(e8, pcm, N, c8) == N);
    TASK_CHECK(g722_encode(eI, pcm, N, ci) == N);
    for (k = 0;  k < N;  k++)
    {
        int expect = 0xC0 | (ci[k] & 0x3F);
        if (c8[k] != expect)
            report_mismatch("analysis", 64000, k, "eight_k_code", c8[k], expect);
        TASK_CHECK(c8[k] == expect);
    }
    /* The low-band ADPCM state matches the ITU reference exactly. */
    TASK_CHECK(band_diff(&e8->band[0], &eI->band[0]) == NULL);
    /* The high band of the 8kHz encoder is never updated. */
    TASK_CHECK(band_pristine(&e8->band[1], 8));

    d8 = g722_decoder_new(64000, G722_SAMPLE_RATE_8000);
    dI = g722_decoder_new(64000, 0);
    TASK_CHECK(d8 != NULL  &&  dI != NULL);
    dI->itu_test_mode = 1;
    TASK_CHECK(g722_decode(d8, c8, N, o8) == N);
    TASK_CHECK(g722_decode(dI, c8, N, io) == 2*N);
    for (k = 0;  k < N;  k++)
    {
        if (o8[k] != io[2*k])
            report_mismatch("synthesis", 64000, k, "eight_k_low", o8[k], io[2*k]);
        TASK_CHECK(o8[k] == io[2*k]);
    }
    TASK_CHECK(band_diff(&d8->band[0], &dI->band[0]) == NULL);
    TASK_CHECK(band_pristine(&d8->band[1], 8));
    g722_encoder_destroy(e8);
    g722_encoder_destroy(eI);
    g722_decoder_destroy(d8);
    g722_decoder_destroy(dI);
    return 0;
}

/* ================================================================== */
/* Group: synthesis                                                   */
/* ================================================================== */

/*
 * Hand-computed anchors for the receive reference itself: which u lane
 * drives which output of the pair, the >>11 scaling, and floor (not
 * truncation) on negative values.
 */
static int case_synthesis_anchors(void)
{
    int32_t u[48];

    memset(u, 0, sizeof(u));
    /* u[1] (the rlow-rhigh lane) drives the first output of the pair:
       floor(4096*3/2048) = 6, and does not leak into the second output. */
    u[1] = 4096;
    TASK_CHECK(ref_synthesis(u, 0) == 6);
    TASK_CHECK(ref_synthesis(u, 1) == 0);
    /* u[0] (the rlow+rhigh lane) drives the second output:
       floor(4096*(-11)/2048) = -22, and not the first. */
    u[1] = 0;
    u[0] = 4096;
    TASK_CHECK(ref_synthesis(u, 0) == 0);
    TASK_CHECK(ref_synthesis(u, 1) == -22);
    /* Scaling and floor: floor(-6144/2048) = -3, floor(-3000/2048) = -2,
       floor(3000/2048) = 1. */
    u[0] = 0;
    u[1] = -2048;
    TASK_CHECK(ref_synthesis(u, 0) == -3);
    u[1] = -1000;
    TASK_CHECK(ref_synthesis(u, 0) == -2);
    u[1] = 1000;
    TASK_CHECK(ref_synthesis(u, 0) == 1);
    return 0;
}

static int case_synthesis_dual_tone(void)
{
    int16_t pcm[2*TOT_PAIRS];

    sig_dual_tone(pcm, SIG_PAIRS, TOT_PAIRS);
    return synthesis_check("dual_tone", pcm, TOT_PAIRS, TAIL_PAIRS);
}

static int case_synthesis_impulse_and_mixed(void)
{
    int16_t pcm[2*TOT_PAIRS];
    int k;

    sig_impulse(pcm, 2*TOT_PAIRS, 10, 6000);
    TASK_CHECK(synthesis_check("impulse_even", pcm, TOT_PAIRS, 0) == 0);
    sig_impulse(pcm, 2*TOT_PAIRS, 11, 6000);
    TASK_CHECK(synthesis_check("impulse_odd", pcm, TOT_PAIRS, 0) == 0);
    sig_mixed(pcm, 2*SIG_PAIRS, 99u);
    for (k = 2*SIG_PAIRS;  k < 2*TOT_PAIRS;  k++)
        pcm[k] = 0;
    TASK_CHECK(synthesis_check("mixed", pcm, TOT_PAIRS, TAIL_PAIRS) == 0);
    return 0;
}

/* ================================================================== */
/* Group: packing                                                     */
/* ================================================================== */

/* Independent LSB-first bit-packing model (encode side). */
struct enc_model
{
    uint32_t acc;
    int bits;
    uint8_t bytes[256];
    long nbytes;
};

static void model_pack(struct enc_model *m, int code, int bps)
{
    m->acc |= (uint32_t) code << m->bits;
    m->bits += bps;
    while (m->bits >= 8)
    {
        m->bytes[m->nbytes++] = (uint8_t) (m->acc & 0xFF);
        m->acc >>= 8;
        m->bits -= 8;
    }
}

/*
 * Independent delivery model (decode side).  Bit counting alone determines
 * how many codewords a fragment delivers: a byte is consumed only when the
 * cache holds fewer than bps bits, one codeword is delivered per step, and
 * the step stops the moment the fragment's last byte is used - a complete
 * codeword may stay cached until the next non-empty call.
 */
struct dec_model
{
    uint32_t acc;
    int bits;
    long delivered;
    uint8_t ideal[256];
};

static int model_deliver(struct dec_model *m, const uint8_t *bytes, int len, int bps)
{
    int j = 0;
    int produced = 0;

    while (j < len)
    {
        if (m->bits < bps)
        {
            m->acc |= (uint32_t) bytes[j++] << m->bits;
            m->bits += 8;
        }
        m->ideal[m->delivered++] = (uint8_t) (m->acc & ((1u << bps) - 1));
        m->acc >>= bps;
        m->bits -= bps;
        produced++;
    }
    return produced;
}

/* Extract n bits (LSB first) from a byte stream, for cross-checks. */
static uint32_t stream_bits(const uint8_t *bytes, long bitpos, int n)
{
    uint32_t v = 0;
    int i;

    for (i = 0;  i < n;  i++)
        v |= (uint32_t) ((bytes[(bitpos + i) >> 3] >> ((bitpos + i) & 7)) & 1) << i;
    return v;
}

/*
 * Packed encoding at 56 and 48 kbps, fed in chunks that leave residual
 * bits at the end of a call.  Emitted bytes must match the independent
 * LSB-first model of the non-packed codewords; only the valid residual
 * bits of the state are compared.  40 codewords of 7 or 6 bits fill whole
 * bytes exactly, so nothing may be left over at the end.
 */
static int case_packed_encode_bit_model(void)
{
    static const int rates[2] = {56000, 48000};
    static const int chunk_pairs[8] = {1, 3, 11, 2, 8, 7, 5, 3};
    enum { PAIRS = 40 };
    int16_t pcm[2*PAIRS];
    int r;

    sig_mixed(pcm, 2*PAIRS, 31u);
    for (r = 0;  r < 2;  r++)
    {
        int bps = (rates[r] == 56000)  ?  7  :  6;
        G722_ENC_CTX *eP = g722_encoder_new(rates[r], G722_PACKED);
        G722_ENC_CTX *eN = g722_encoder_new(rates[r], 0);
        struct enc_model m;
        uint8_t codes[PAIRS];
        int ncodes = 0;
        int off = 0;
        int ci;
        long prev_bytes;

        TASK_CHECK(eP != NULL  &&  eN != NULL);
        memset(&m, 0, sizeof(m));
        prev_bytes = 0;
        for (ci = 0;  ci < 8;  ci++)
        {
            int cp = chunk_pairs[ci];
            uint8_t bp[64];
            uint8_t bn[64];
            int nP;
            int nN;
            int i;
            long emitted;

            nP = g722_encode(eP, pcm + 2*off, 2*cp, bp);
            nN = g722_encode(eN, pcm + 2*off, 2*cp, bn);
            TASK_CHECK(nN == cp);
            for (i = 0;  i < cp;  i++)
            {
                codes[ncodes++] = bn[i];
                model_pack(&m, bn[i], bps);
            }
            emitted = m.nbytes - prev_bytes;
            if (nP != emitted  ||  memcmp(bp, m.bytes + prev_bytes, nP) != 0)
                report_mismatch("packing", rates[r], off, "packed_bytes",
                                nP > 0  ?  bp[0]  :  -1,
                                emitted > 0  ?  m.bytes[prev_bytes]  :  -1);
            TASK_CHECK(nP == emitted);
            TASK_CHECK(memcmp(bp, m.bytes + prev_bytes, nP) == 0);
            prev_bytes = m.nbytes;
            /* residual state: only the valid leftover bits are compared */
            if (eP->out_bits != m.bits  ||  (eP->out_buffer & ((1u << m.bits) - 1)) != m.acc)
                report_mismatch("packing", rates[r], off, "residual_bits",
                                eP->out_buffer & 0xFF, m.acc);
            TASK_CHECK(eP->out_bits == m.bits);
            TASK_CHECK((eP->out_buffer & ((1u << m.bits) - 1)) == m.acc);
            off += cp;
        }
        TASK_CHECK(ncodes == PAIRS);
        /* 40 codewords of 7 or 6 bits fill whole bytes exactly. */
        TASK_CHECK(m.bits == 0  &&  eP->out_bits == 0);
        /* The packed bytes unpack (LSB first) to the non-packed codewords. */
        for (ci = 0;  ci < ncodes;  ci++)
        {
            int got = (int) stream_bits(m.bytes, (long) ci*bps, bps);
            if (got != codes[ci])
                report_mismatch("packing", rates[r], ci, "lsb_first_roundtrip", got, codes[ci]);
            TASK_CHECK(got == codes[ci]);
        }
        g722_encoder_destroy(eP);
        g722_encoder_destroy(eN);
    }
    return 0;
}

/*
 * Packed decoding fed in 1, 3, 11 byte fragments.  The model's bit
 * counting decides the delivered prefix and the valid cache after every
 * fragment; the delivered PCM prefix is cross-checked against a non-packed
 * decoder.  The stream is a whole number of codewords filling whole bytes,
 * so the final codeword stays cached until a padding byte arrives; the
 * extra codeword the padding bits form is not audio and is never compared
 * as such.
 */
static int case_packed_decode_fragment_rhythm(void)
{
    static const int rates[2] = {56000, 48000};
    static const int frags[3] = {1, 3, 11};
    enum { PAIRS = 40 };
    int16_t pcm[2*PAIRS];
    int r;

    sig_mixed(pcm, 2*PAIRS, 17u);
    for (r = 0;  r < 2;  r++)
    {
        int bps = (rates[r] == 56000)  ?  7  :  6;
        G722_ENC_CTX *eN = g722_encoder_new(rates[r], 0);
        G722_ENC_CTX *eP = g722_encoder_new(rates[r], G722_PACKED);
        G722_DEC_CTX *dP = g722_decoder_new(rates[r], G722_PACKED);
        G722_DEC_CTX *dN = g722_decoder_new(rates[r], 0);
        uint8_t codes[PAIRS];
        uint8_t stream[64];
        int16_t outP[2*(PAIRS + 2)];
        struct dec_model m;
        int nbytes;
        int off = 0;
        int fi = 0;
        long got = 0;

        TASK_CHECK(eN != NULL  &&  eP != NULL  &&  dP != NULL  &&  dN != NULL);
        TASK_CHECK(g722_encode(eN, pcm, 2*PAIRS, codes) == PAIRS);
        nbytes = g722_encode(eP, pcm, 2*PAIRS, stream);
        TASK_CHECK(nbytes == PAIRS*bps/8);
        memset(&m, 0, sizeof(m));
        while (off < nbytes)
        {
            int flen = frags[fi++%3];
            int n;
            int produced;
            int rn;
            int16_t ref[2*(PAIRS + 2)];
            int16_t zout[2];

            if (flen > nbytes - off)
                flen = nbytes - off;
            /* A zero-length call must not drain the cached codeword. */
            TASK_CHECK(g722_decode(dP, stream, 0, zout) == 0);
            TASK_CHECK(dP->in_bits == m.bits
                       &&  (dP->in_buffer & ((1u << m.bits) - 1)) == m.acc);
            n = g722_decode(dP, stream + off, flen, outP + got);
            produced = model_deliver(&m, stream + off, flen, bps);
            if (n != 2*produced)
                report_mismatch("packing", rates[r], off, "delivered_prefix", n, 2L*produced);
            TASK_CHECK(n == 2*produced);
            /* Cross-check the freshly delivered PCM prefix. */
            rn = g722_decode(dN, m.ideal + m.delivered - produced, produced, ref);
            if (rn != n  ||  memcmp(ref, outP + got, n*sizeof(int16_t)) != 0)
                report_mismatch("packing", rates[r], off, "pcm_prefix", rn, n);
            TASK_CHECK(rn == n);
            TASK_CHECK(memcmp(ref, outP + got, n*sizeof(int16_t)) == 0);
            got += n;
            off += flen;
            if (dP->in_bits != m.bits  ||  (dP->in_buffer & ((1u << m.bits) - 1)) != m.acc)
                report_mismatch("packing", rates[r], off, "cache_bits",
                                dP->in_buffer & 0xFF, m.acc);
            TASK_CHECK(dP->in_bits == m.bits);
            TASK_CHECK((dP->in_buffer & ((1u << m.bits) - 1)) == m.acc);
        }
        /* Whole bytes consumed: the final codeword stays cached.  This
           existing delay is not a lost sample. */
        TASK_CHECK(m.delivered == PAIRS - 1);
        TASK_CHECK(got == 2*(PAIRS - 1));
        /* One padding byte flushes the last real codeword; the padding
           bits then form one extra codeword which is not audio. */
        {
            uint8_t pad = 0xA5;
            int n;
            int produced;
            int rn;
            int16_t ref[4];

            n = g722_decode(dP, &pad, 1, outP + got);
            produced = model_deliver(&m, &pad, 1, bps);
            TASK_CHECK(produced == 2);
            TASK_CHECK(n == 2*produced);
            TASK_CHECK(m.delivered == PAIRS + 1);
            rn = g722_decode(dN, m.ideal + PAIRS - 1, 1, ref);
            TASK_CHECK(rn == 2);
            TASK_CHECK(memcmp(ref, outP + got, 2*sizeof(int16_t)) == 0);
            TASK_CHECK(dP->in_bits == m.bits
                       &&  (dP->in_buffer & ((1u << m.bits) - 1)) == m.acc);
        }
        g722_encoder_destroy(eN);
        g722_encoder_destroy(eP);
        g722_decoder_destroy(dP);
        g722_decoder_destroy(dN);
    }
    return 0;
}

/*
 * Two packed decoder instances on interleaved fragment schedules must not
 * cross their bit-cache or ADPCM histories; each is checked against its
 * own model and its own non-packed reference decoder.
 */
static int case_packed_dual_instance(void)
{
    enum { PAIRS = 24 };
    int16_t pcmA[2*PAIRS];
    int16_t pcmB[2*PAIRS];
    uint8_t streamA[32];
    uint8_t streamB[32];
    int nbytesA;
    int nbytesB;
    G722_ENC_CTX *eA;
    G722_ENC_CTX *eB;
    G722_DEC_CTX *dA;
    G722_DEC_CTX *dB;
    G722_DEC_CTX *rA;
    G722_DEC_CTX *rB;
    struct dec_model mA;
    struct dec_model mB;
    int16_t outA[2*(PAIRS + 2)];
    int16_t outB[2*(PAIRS + 2)];
    long gotA = 0;
    long gotB = 0;
    int offA = 0;
    int offB = 0;

    sig_mixed(pcmA, 2*PAIRS, 41u);
    sig_dual_tone(pcmB, PAIRS, PAIRS);
    eA = g722_encoder_new(56000, G722_PACKED);
    eB = g722_encoder_new(56000, G722_PACKED);
    TASK_CHECK(eA != NULL  &&  eB != NULL);
    nbytesA = g722_encode(eA, pcmA, 2*PAIRS, streamA);
    nbytesB = g722_encode(eB, pcmB, 2*PAIRS, streamB);
    TASK_CHECK(nbytesA == 21  &&  nbytesB == 21);  /* 24*7/8 */
    TASK_CHECK(memcmp(streamA, streamB, 21) != 0);
    dA = g722_decoder_new(56000, G722_PACKED);
    dB = g722_decoder_new(56000, G722_PACKED);
    rA = g722_decoder_new(56000, 0);
    rB = g722_decoder_new(56000, 0);
    TASK_CHECK(dA != NULL  &&  dB != NULL  &&  rA != NULL  &&  rB != NULL);
    memset(&mA, 0, sizeof(mA));
    memset(&mB, 0, sizeof(mB));
    while (offA < nbytesA  ||  offB < nbytesB)
    {
        if (offA < nbytesA)
        {
            int flen = (offA + 2 <= nbytesA)  ?  2  :  nbytesA - offA;
            int n = g722_decode(dA, streamA + offA, flen, outA + gotA);
            int produced = model_deliver(&mA, streamA + offA, flen, 7);
            int16_t ref[16];
            int rn = g722_decode(rA, mA.ideal + mA.delivered - produced, produced, ref);

            TASK_CHECK(n == 2*produced  &&  rn == n);
            TASK_CHECK(memcmp(ref, outA + gotA, n*sizeof(int16_t)) == 0);
            gotA += n;
            offA += flen;
        }
        if (offB < nbytesB)
        {
            int flen = (offB + 3 <= nbytesB)  ?  3  :  nbytesB - offB;
            int n = g722_decode(dB, streamB + offB, flen, outB + gotB);
            int produced = model_deliver(&mB, streamB + offB, flen, 7);
            int16_t ref[16];
            int rn = g722_decode(rB, mB.ideal + mB.delivered - produced, produced, ref);

            TASK_CHECK(n == 2*produced  &&  rn == n);
            TASK_CHECK(memcmp(ref, outB + gotB, n*sizeof(int16_t)) == 0);
            gotB += n;
            offB += flen;
        }
    }
    TASK_CHECK(mA.delivered == PAIRS - 1  &&  mB.delivered == PAIRS - 1);
    /* Each instance matches its own model - no shared history. */
    TASK_CHECK(dA->in_bits == mA.bits  &&  dB->in_bits == mB.bits);
    TASK_CHECK((dA->in_buffer & ((1u << mA.bits) - 1)) == mA.acc);
    TASK_CHECK((dB->in_buffer & ((1u << mB.bits) - 1)) == mB.acc);
    TASK_CHECK(band_diff(&dA->band[0], &rA->band[0]) == NULL);
    TASK_CHECK(band_diff(&dB->band[0], &rB->band[0]) == NULL);
    g722_encoder_destroy(eA);
    g722_encoder_destroy(eB);
    g722_decoder_destroy(dA);
    g722_decoder_destroy(dB);
    g722_decoder_destroy(rA);
    g722_decoder_destroy(rB);
    return 0;
}

/* ================================================================== */

const struct task_case task_cases[] =
{
    {"embedded", "codeword_shift_relation", case_codeword_shift_relation},
    {"embedded", "decoder_state_lockstep", case_decoder_state_lockstep},
    {"embedded", "reconstruction_tables", case_reconstruction_tables},
    {"embedded", "refinement_bits", case_refinement_bits},
    {"analysis", "impulse_phase", case_analysis_impulse_phase},
    {"analysis", "dual_tone", case_analysis_dual_tone},
    {"analysis", "mixed_sequence", case_analysis_mixed_sequence},
    {"analysis", "eight_k_bypass", case_eight_k_bypass},
    {"synthesis", "reference_anchors", case_synthesis_anchors},
    {"synthesis", "dual_tone", case_synthesis_dual_tone},
    {"synthesis", "impulse_and_mixed", case_synthesis_impulse_and_mixed},
    {"packing", "encode_bit_model", case_packed_encode_bit_model},
    {"packing", "decode_fragment_rhythm", case_packed_decode_fragment_rhythm},
    {"packing", "dual_instance", case_packed_dual_instance}
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
