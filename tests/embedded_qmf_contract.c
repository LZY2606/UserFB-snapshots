/*
 * embedded_qmf_contract.c - C regression contract for the embedded G.722 paths.
 *
 * This file keeps "is the bit stream right" apart from "is the audio right".
 * Nothing here compares a decoded signal against the original PCM: the codec is
 * lossy, so a round-trip equality check could only ever be a smoke test.  Every
 * expected value below is built independently instead:
 *
 *   - the transmit reference applies its own direct-convolution QMF to the
 *     signal in natural time order, then runs two separate, continuously
 *     running ITU-test-mode ADPCM encoders (one low band, one high band);
 *   - the receive reference takes the reconstructed sub-bands from a separate,
 *     continuously running ITU-test-mode decoder and applies its own causal QMF
 *     convolution to produce the expected PCM.
 *
 * The ITU test mode only supplies the ADPCM half of the model.  It never
 * supplies the expected result of the band-split filters.
 *
 * Provenance of every reference and the gaps in the older Python round-trip
 * tests are documented in tests/embedded_qmf_contract.md.
 */

#include "task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "g722_private.h"
#include "g722_common.h"
#include "g722_encoder.h"
#include "g722_decoder.h"

/* ------------------------------------------------------------------------- *
 * Limits, signals and normative tables.
 * ------------------------------------------------------------------------- */

enum
{
    SEG_PAIRS   = 400,             /* 16 kHz sample pairs = one codeword each */
    SEG_SAMPLES = 2 * SEG_PAIRS,
    TAIL_PAIRS  = 96,              /* silence spliced after SEG_PAIRS */
    REF_MAX     = 1024,
    PACK_BYTES  = 1024
};

/* 12-tap quadrature mirror filter, ITU-T G.722 (the same normative values as
 * qmf_coeffs[] in the library).  Repeated here so the reference does not read
 * a library table. */
static const int QMF_C[12] =
{
    3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11
};

/* Low-band inverse quantizer tables from ITU-T G.722.  WM4 serves 48 kbit/s,
 * WM5 56 kbit/s, WM6 64 kbit/s.  The library's qm4/qm5/qm6 hold the same
 * normative constants. */
static const int WM4[16] =
{
         0, -20456, -12896,  -8968,
     -6288,  -4240,  -2584,  -1200,
     20456,  12896,   8968,   6288,
      4240,   2584,   1200,      0
};

static const int WM5[32] =
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

static const int WM6[64] =
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

static const int WM2[4] = { -7408, -1616, 7408, 1616 };

/* Test vectors.  All are low amplitude so the fixed-point quantizers never
 * saturate and the comparisons stay sensitive to ordering and phase.
 *
 *  - two-tone: one low tone and one high tone, asymmetric amplitudes;
 *  - impulses: alternating-sign pulses placed on even or odd sample positions;
 *  - signed: a deterministic sequence with alternating sign.
 */
static void gen_two_tone(int16_t *pcm, int samples)
{
    int i;

    for (i = 0; i < samples; i++)
    {
        pcm[i] = (int16_t) (int) (640.0*sin(2.0*M_PI*617.0*i/16000.0 + 0.31)
                                + 210.0*cos(2.0*M_PI*5311.0*i/16000.0 - 0.77));
    }
}

static void gen_impulses(int16_t *pcm, int samples, int first)
{
    static const int pos[4] = {20, 60, 100, 140};
    static const int amp[4] = {2600, -1900, 1100, -700};
    int i;

    memset(pcm, 0, (size_t) samples*sizeof(pcm[0]));
    for (i = 0; i < 4; i++)
    {
        if (first + 2*pos[i] < samples)
            pcm[first + 2*pos[i]] = (int16_t) amp[i];
    }
}

static void gen_signed(int16_t *pcm, int samples)
{
    int i;

    for (i = 0; i < samples; i++)
    {
        int v = (int) ((unsigned) (i*40503u + 12345u) % 8192u) - 4096;
        pcm[i] = (int16_t) ((i & 1) ? v : -v);
    }
}

/* ------------------------------------------------------------------------- *
 * Fixed-point helpers: 64-bit accumulation, explicit floor division matching
 * the arithmetic shift used by the implementation, then output limiting.
 * ------------------------------------------------------------------------- */

static long long floor_div_pow2(long long value, int shift)
{
    if (value >= 0)
        return value >> shift;
    return -(((-value) + ((1LL << shift) - 1)) >> shift);
}

static int16_t limit16(long long value)
{
    if (value > INT16_MAX)
        return INT16_MAX;
    if (value < INT16_MIN)
        return INT16_MIN;
    return (int16_t) value;
}

/* ------------------------------------------------------------------------- *
 * Transmit-side QMF reference.
 *
 * For pair n, the odd-phase samples are pcm[2n + 1 - 2j] and the even-phase
 * samples are pcm[2n - 22 + 2j], for j = 0..11.  Samples before the start of
 * the stream are zero.
 *
 *   low  = limit((sum_j C[j]*odd_j + sum_j C[j]*even_j) >> 14)
 *   high = limit((sum_j C[j]*odd_j - sum_j C[j]*even_j) >> 14)
 *
 * This is a plain causal convolution in natural time order, written out here
 * instead of copying the library's two-sample buffer shuffle loop.  A wrong
 * direction or ordering in the library therefore shows up as a mismatch
 * instead of being mirrored by the reference.
 * ------------------------------------------------------------------------- */
static void analysis_reference(const int16_t *pcm, int pairs, int *low, int *high)
{
    int n;
    int j;

    for (n = 0; n < pairs; n++)
    {
        long long odd = 0;
        long long even = 0;

        for (j = 0; j < 12; j++)
        {
            int k_odd = 2*n + 1 - 2*j;
            int k_even = 2*n - 22 + 2*j;

            if (k_odd >= 0)
                odd += (long long) QMF_C[j]*pcm[k_odd];
            if (k_even >= 0)
                even += (long long) QMF_C[j]*pcm[k_even];
        }
        low[n]  = limit16(floor_div_pow2(odd + even, 14));
        high[n] = limit16(floor_div_pow2(odd - even, 14));
    }
}

/* Deliberately broken variant used to prove the phase checks are sensitive:
 * one branch walks its coefficients in the opposite direction.  This is the
 * shape of the "reversed coefficient direction" degradation. */
static void analysis_reference_reversed(const int16_t *pcm, int pairs, int *low, int *high)
{
    int n;
    int j;

    for (n = 0; n < pairs; n++)
    {
        long long odd = 0;
        long long even = 0;

        for (j = 0; j < 12; j++)
        {
            int k_odd = 2*n + 1 - 2*j;
            int k_even = 2*n - 22 + 2*j;

            if (k_odd >= 0)
                odd += (long long) QMF_C[j]*pcm[k_odd];
            if (k_even >= 0)
                even += (long long) QMF_C[11 - j]*pcm[k_even];
        }
        low[n]  = limit16(floor_div_pow2(odd + even, 14));
        high[n] = limit16(floor_div_pow2(odd - even, 14));
    }
}

/* ------------------------------------------------------------------------- *
 * Transmit reference: independent QMF, then two continuously running ADPCM
 * encoders in ITU test mode.  Test mode treats one input sample as one
 * sub-band sample and emits the quantizer index in the top two bits of a
 * 64 kbit/s word.  The two encoders keep entirely separate band histories.
 * ------------------------------------------------------------------------- */
struct transmit_ref
{
    G722_ENC_CTX *low_enc;
    G722_ENC_CTX *high_enc;
    int           raw_low[REF_MAX];
    int           raw_high[REF_MAX];
    int16_t       low_in[REF_MAX];
    int16_t       high_in[REF_MAX];
    unsigned char low_code[REF_MAX];
    unsigned char high_code[REF_MAX];
};

static void transmit_ref_init(struct transmit_ref *ref)
{
    memset(ref, 0, sizeof(*ref));
    ref->low_enc = g722_encoder_new(64000, 0);
    ref->high_enc = g722_encoder_new(64000, 0);
    if (ref->low_enc)
        ref->low_enc->itu_test_mode = TRUE;
    if (ref->high_enc)
        ref->high_enc->itu_test_mode = TRUE;
}

static void transmit_ref_free(struct transmit_ref *ref)
{
    if (ref->low_enc)
        g722_encoder_destroy(ref->low_enc);
    if (ref->high_enc)
        g722_encoder_destroy(ref->high_enc);
    ref->low_enc = NULL;
    ref->high_enc = NULL;
}

/* Build the whole sub-band pair from the signal, in natural time order. */
static int transmit_ref_prepare(struct transmit_ref *ref, const int16_t *pcm, int pairs)
{
    int n;

    if (pairs <= 0 || pairs > REF_MAX)
        return -1;
    analysis_reference(pcm, pairs, ref->raw_low, ref->raw_high);
    for (n = 0; n < pairs; n++)
    {
        ref->low_in[n]  = (int16_t) (ref->raw_low[n] << 1);
        ref->high_in[n] = (int16_t) (ref->raw_high[n] << 1);
    }
    return 0;
}

/* Encode one block through the same two encoder instances.  Blocks are
 * concatenated, so the ADPCM prediction and scale state runs continuously. */
static int transmit_ref_encode(struct transmit_ref *ref, int start, int pairs)
{
    if (pairs <= 0 || start < 0 || start + pairs > REF_MAX)
        return -1;
    if (g722_encode(ref->low_enc, ref->low_in + start, pairs, ref->low_code + start) != pairs)
        return -1;
    if (g722_encode(ref->high_enc, ref->high_in + start, pairs, ref->high_code + start) != pairs)
        return -1;
    return 0;
}

/* Combine the low six bits of the low-band encoder with the high two bits of
 * the high-band encoder into the expected 64 kbit/s codeword. */
static unsigned expected_code(const struct transmit_ref *ref, int n)
{
    return (unsigned) (ref->low_code[n] & 0x3F)
         | ((unsigned) (ref->high_code[n] >> 6) << 6);
}

/* ------------------------------------------------------------------------- *
 * Receive-side QMF reference.
 *
 * The library writes pair n into x[2n] = rlow[n] + rhigh[n] and
 * x[2n + 1] = rlow[n] - rhigh[n].  Tracking that gives the plain causal
 * convolution below.  Note the mirrored coefficient order of the two output
 * phases:
 *     out[2n]     = limit((sum_j C[j]    * x[2n - j])     >> 11)
 *     out[2n + 1] = limit((sum_j C[j]    * x[2n + 1 - 11 + j]) >> 11)
 * ------------------------------------------------------------------------- */
static void synthesis_reference(const int *rlow, const int *rhigh, int pairs, int16_t *pcm)
{
    int n;
    int j;

    for (n = 0; n < pairs; n++)
    {
        long long phase0 = 0;
        long long phase1 = 0;

        for (j = 0; j < 12; j++)
        {
            int k0 = n - j;
            int k1 = n - 11 + j;

            if (k0 >= 0)
                phase0 += (long long) QMF_C[j]*(rlow[k0] - rhigh[k0]);
            if (k1 >= 0)
                phase1 += (long long) QMF_C[j]*(rlow[k1] + rhigh[k1]);
        }
        pcm[2*n]     = limit16(floor_div_pow2(phase0, 11));
        pcm[2*n + 1] = limit16(floor_div_pow2(phase1, 11));
    }
}

/* ------------------------------------------------------------------------- *
 * Bit-stream helpers used by the packing group.
 * ------------------------------------------------------------------------- */

/* Pack the low `bits` bits of each code LSB-first, as the G.722 bit stream
 * definition requires. */
static int pack_lsb_first(const unsigned char *code, int count, int bits, unsigned char *out)
{
    int i;
    int b;
    int bit = 0;

    memset(out, 0, (size_t) ((count*bits + 7)/8));
    for (i = 0; i < count; i++)
    {
        for (b = 0; b < bits; b++)
        {
            if (code[i] & (1u << b))
                out[bit >> 3] |= (unsigned char) (1u << (bit & 7));
            bit++;
        }
    }
    return (bit + 7)/8;
}

/* Unpack complete codewords LSB-first from a byte buffer.  `available` bits
 * are present; codewords are returned only when all of their bits are there,
 * so trailing padding in the last byte is never promoted to a codeword. */
static int unpack_lsb_first(const unsigned char *in, int available_bits, int bits,
                            unsigned char *code, int max_codes)
{
    int bit;
    int count = 0;

    for (bit = 0; bit + bits <= available_bits; bit += bits)
    {
        unsigned value = 0;
        int b;

        for (b = 0; b < bits; b++)
        {
            if (in[(bit + b) >> 3] & (1u << ((bit + b) & 7)))
                value |= 1u << b;
        }
        if (count < max_codes)
            code[count] = (unsigned char) value;
        count++;
    }
    return count;
}

/* Read `nbits` bits LSB-first starting at `start_bit`. */
static unsigned read_bits_lsb_first(const unsigned char *in, int start_bit, int nbits)
{
    unsigned value = 0;
    int b;

    for (b = 0; b < nbits; b++)
    {
        int bit = start_bit + b;

        if (in[bit >> 3] & (1u << (bit & 7)))
            value |= 1u << b;
    }
    return value;
}

/* ------------------------------------------------------------------------- *
 * Field-wise state comparison.  Only meaningful fields are compared: scalars
 * for the predictors and scales, element-wise for the histories.  There is
 * deliberately no memcmp of the whole structure, because it contains padding.
 * ------------------------------------------------------------------------- */

static const char *band_state_diff(const struct g722_band *a, const struct g722_band *b,
                                   int *va, int *vb)
{
    int i;

#define STATE_SCALAR(field) \
    do { if (a->field != b->field) { *va = a->field; *vb = b->field; return #field; } } while (0)
#define STATE_ARRAY(field) \
    do { \
        for (i = 0; i < (int) (sizeof(a->field)/sizeof(a->field[0])); i++) \
            if (a->field[i] != b->field[i]) { *va = a->field[i]; *vb = b->field[i]; return #field; } \
    } while (0)

    STATE_SCALAR(s);
    STATE_SCALAR(sp);
    STATE_SCALAR(sz);
    STATE_ARRAY(r);
    STATE_ARRAY(a);
    STATE_ARRAY(ap);
    STATE_ARRAY(p);
    STATE_ARRAY(d);
    STATE_ARRAY(b);
    STATE_ARRAY(bp);
    STATE_ARRAY(sg);
    STATE_SCALAR(nb);
    STATE_SCALAR(det);
#undef STATE_SCALAR
#undef STATE_ARRAY
    return NULL;
}

static void report(const char *direction, int rate, int index, const char *what,
                   long long got, long long want)
{
    fprintf(stderr, "  %s rate=%d pos=%d %s got=%lld want=%lld\n",
            direction, rate, index, what, got, want);
}

/* ------------------------------------------------------------------------- *
 * Embedded group: the one codeword stream shared by the three bit rates.
 * ------------------------------------------------------------------------- */

/* Every 64 kbit/s codeword shifted right by one bit must equal the matching
 * 56 kbit/s code, and shifted right by two bits the 48 kbit/s code.  One
 * encoder instance per rate runs across the whole segment; no per-block
 * re-creation. */
static int case_code_hierarchy(void)
{
    static int16_t pcm[SEG_SAMPLES];
    static unsigned char code64[SEG_PAIRS];
    static unsigned char code56[SEG_PAIRS];
    static unsigned char code48[SEG_PAIRS];
    G722_ENC_CTX *enc;
    int n;
    int bad = 0;

    gen_two_tone(pcm, SEG_SAMPLES);
    enc = g722_encoder_new(64000, 0);
    TASK_CHECK(enc != NULL);
    TASK_CHECK(g722_encode(enc, pcm, SEG_SAMPLES, code64) == SEG_PAIRS);
    g722_encoder_destroy(enc);

    enc = g722_encoder_new(56000, 0);
    TASK_CHECK(enc != NULL);
    TASK_CHECK(g722_encode(enc, pcm, SEG_SAMPLES, code56) == SEG_PAIRS);
    g722_encoder_destroy(enc);

    enc = g722_encoder_new(48000, 0);
    TASK_CHECK(enc != NULL);
    TASK_CHECK(g722_encode(enc, pcm, SEG_SAMPLES, code48) == SEG_PAIRS);
    g722_encoder_destroy(enc);

    for (n = 0; n < SEG_PAIRS; n++)
    {
        if (code56[n] != (code64[n] >> 1))
        {
            report("embedded/64->56", 56000, n, "codeword", code56[n], code64[n] >> 1);
            bad++;
        }
        if (code48[n] != (code64[n] >> 2))
        {
            report("embedded/64->48", 48000, n, "codeword", code48[n], code64[n] >> 2);
            bad++;
        }
    }
    TASK_CHECK(bad == 0);
    /* The shared high-band index must survive all three truncations. */
    for (n = 0; n < SEG_PAIRS; n++)
    {
        TASK_CHECK((code64[n] >> 6) == (code56[n] >> 5));
        TASK_CHECK((code64[n] >> 6) == (code48[n] >> 4));
    }
    return 0;
}

/* Same signal through all three decoders.  Audio may differ, but the four-bit
 * low-band index, the predictor state and the scale state must match. */
static int case_decoder_state_shared(void)
{
    static int16_t pcm[SEG_SAMPLES];
    static unsigned char code[3][SEG_PAIRS];
    static int16_t pcm_out[3][SEG_SAMPLES];
    G722_DEC_CTX *dec[3];
    int rates[3] = {64000, 56000, 48000};
    int n;
    int r;
    int b;
    int low_audio_diffs = 0;
    int state_diffs = 0;

    gen_two_tone(pcm, SEG_SAMPLES);
    for (r = 0; r < 3; r++)
    {
        G722_ENC_CTX *enc = g722_encoder_new(rates[r], 0);

        TASK_CHECK(enc != NULL);
        TASK_CHECK(g722_encode(enc, pcm, SEG_SAMPLES, code[r]) == SEG_PAIRS);
        g722_encoder_destroy(enc);
    }
    for (r = 0; r < 3; r++)
    {
        dec[r] = g722_decoder_new(rates[r], 0);
        TASK_CHECK(dec[r] != NULL);
        TASK_CHECK(g722_decode(dec[r], code[r], SEG_PAIRS, pcm_out[r]) == SEG_SAMPLES);
    }
    for (n = 0; n < SEG_PAIRS; n++)
    {
        unsigned index64 = (unsigned) (code[0][n] >> 2) & 0x0F;

        TASK_CHECK(index64 == ((unsigned) (code[1][n] >> 1) & 0x0F));
        TASK_CHECK(index64 == ((unsigned) code[2][n] & 0x0F));
        for (r = 1; r < 3; r++)
        {
            for (b = 0; b < 2; b++)
            {
                int va = 0;
                int vb = 0;
                const char *field = band_state_diff(&dec[0]->band[b], &dec[r]->band[b], &va, &vb);

                if (field)
                {
                    fprintf(stderr, "  embedded/state rate=%d pos=%d band=%d field=%s "
                            "got=%d want=%d\n", rates[r], n, b, field, va, vb);
                    state_diffs++;
                }
            }
        }
        /* The reconstructed low band may differ between rates, but it must
         * actually differ, otherwise the state equality above is vacuous. */
        if (pcm_out[0][2*n] != pcm_out[1][2*n] || pcm_out[0][2*n] != pcm_out[2][2*n])
            low_audio_diffs++;
    }
    TASK_CHECK(state_diffs == 0);
    TASK_CHECK(low_audio_diffs > 0);
    for (r = 0; r < 3; r++)
        g722_decoder_destroy(dec[r]);
    return 0;
}

/* Hand-made codewords in the centre of the low-band table: the refinement bit
 * changes the reconstructed value, but not the predictor history the next
 * codeword inherits.  Code 0x14 and 0x17 share low index 5, high index 0. */
static int case_refinement_state(void)
{
    static unsigned char code_a[64];
    static unsigned char code_b[64];
    static int16_t out_a[128];
    static int16_t out_b[128];
    G722_DEC_CTX *a;
    G722_DEC_CTX *b;
    int n;
    int differs = 0;

    for (n = 0; n < 64; n++)
    {
        code_a[n] = 0x14;
        code_b[n] = 0x17;
    }
    a = g722_decoder_new(64000, 0);
    b = g722_decoder_new(64000, 0);
    TASK_CHECK(a != NULL && b != NULL);
    a->itu_test_mode = TRUE;
    b->itu_test_mode = TRUE;
    TASK_CHECK(g722_decode(a, code_a, 64, out_a) == 128);
    TASK_CHECK(g722_decode(b, code_b, 64, out_b) == 128);

    for (n = 0; n < 64; n++)
    {
        if (out_a[2*n] != out_b[2*n])
            differs++;
    }
    TASK_CHECK(differs > 0);
    /* Same low-band index, so the quantizer step and the predictor trajectory
     * stay identical even though the reconstructed value moves. */
    for (n = 0; n < 64; n++)
    {
        TASK_CHECK(a->band[0].det == b->band[0].det);
        TASK_CHECK(a->band[0].nb == b->band[0].nb);
        TASK_CHECK(a->band[0].s == b->band[0].s);
        TASK_CHECK(a->band[0].a[1] == b->band[0].a[1]);
        TASK_CHECK(a->band[0].a[2] == b->band[0].a[2]);
        TASK_CHECK(a->band[0].b[1] == b->band[0].b[1]);
        TASK_CHECK(a->band[0].r[1] == b->band[0].r[1]);
        TASK_CHECK(a->band[0].d[1] == b->band[0].d[1]);
    }
    g722_decoder_destroy(a);
    g722_decoder_destroy(b);
    return 0;
}

/* Known-answer reconstruction for a codeword decoded from a zero state.  This
 * pins the quantizer tables and the rate-specific shift, which a wrongly
 * reused 64 kbit/s table at 56 kbit/s cannot satisfy. */
static int low_band_reconstruction(int rate, int code)
{
    int index;
    int table;

    switch (rate)
    {
    default:
    case 64000:
        index = code & 0x3F;
        table = WM6[index];
        break;
    case 56000:
        index = code & 0x1F;
        table = WM5[index];
        break;
    case 48000:
        index = code & 0x0F;
        table = WM4[index];
        break;
    }
    return (32*table) >> 15;
}

static int high_band_reconstruction(int rate, int code)
{
    int ihigh;

    switch (rate)
    {
    default:
    case 64000:
        ihigh = (code >> 6) & 0x03;
        break;
    case 56000:
        ihigh = (code >> 5) & 0x03;
        break;
    case 48000:
        ihigh = (code >> 4) & 0x03;
        break;
    }
    return (8*WM2[ihigh]) >> 15;
}

static int case_known_reconstruction(void)
{
    static const int rates[3] = {64000, 56000, 48000};
    int r;
    int code;

    for (r = 0; r < 3; r++)
    {
        for (code = 0; code < 256; code++)
        {
            G722_DEC_CTX *dec;
            unsigned char c = (unsigned char) code;
            int16_t out[2];
            int rlow;
            int rhigh;

            dec = g722_decoder_new(rates[r], 0);
            TASK_CHECK(dec != NULL);
            dec->itu_test_mode = TRUE;
            TASK_CHECK(g722_decode(dec, &c, 1, out) == 2);
            rlow = out[0] >> 1;
            rhigh = out[1] >> 1;
            if (rlow != low_band_reconstruction(rates[r], code))
            {
                report("embedded/known-low", rates[r], code, "rlow",
                       rlow, low_band_reconstruction(rates[r], code));
                TASK_CHECK(0);
            }
            if (rhigh != high_band_reconstruction(rates[r], code))
            {
                report("embedded/known-high", rates[r], code, "rhigh",
                       rhigh, high_band_reconstruction(rates[r], code));
                TASK_CHECK(0);
            }
            g722_decoder_destroy(dec);
        }
    }
    return 0;
}

/* The field-wise comparison must be able to tell "same" from "different".
 * Two instances fed the same codewords agree on every meaningful field, even
 * though the structures also contain padding that is never inspected.  When
 * one instance is fed a different codeword the comparison must report the
 * first differing field. */
static int case_state_field_isolation(void)
{
    static unsigned char code[32];
    static unsigned char other[32];
    static int16_t out[64];
    G722_DEC_CTX *a;
    G722_DEC_CTX *b;
    int n;
    int b_index;
    int va = 0;
    int vb = 0;
    const char *field;

    for (n = 0; n < 32; n++)
    {
        code[n] = (unsigned char) (0x10 + (n & 0x0F));
        other[n] = code[n];
    }
    a = g722_decoder_new(64000, 0);
    b = g722_decoder_new(64000, 0);
    TASK_CHECK(a != NULL && b != NULL);
    TASK_CHECK(g722_decode(a, code, 32, out) == 64);
    TASK_CHECK(g722_decode(b, other, 32, out) == 64);
    for (b_index = 0; b_index < 2; b_index++)
        TASK_CHECK(band_state_diff(&a->band[b_index], &b->band[b_index], &va, &vb) == NULL);
    TASK_CHECK(a->in_bits == b->in_bits);
    TASK_CHECK(a->in_buffer == b->in_buffer);
    for (n = 0; n < 24; n++)
        TASK_CHECK(a->x[n] == b->x[n]);
    g722_decoder_destroy(b);

    /* Now a real difference: the same first codeword vs. its opposite. */
    b = g722_decoder_new(64000, 0);
    TASK_CHECK(b != NULL);
    other[0] = 0x3F;
    other[1] = 0x3F;
    TASK_CHECK(g722_decode(b, other, 32, out) == 64);
    field = band_state_diff(&a->band[0], &b->band[0], &va, &vb);
    TASK_CHECK(field != NULL);
    g722_decoder_destroy(a);
    g722_decoder_destroy(b);
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Analysis group: transmit QMF plus per-band ADPCM.
 * ------------------------------------------------------------------------- */

/* Encode the signal with the library, in even-sample chunks, and build the
 * expected codewords from the reference QMF plus two continuously running
 * ITU-mode encoders.  The reference is prepared over the whole natural-time
 * signal, then its encoders are fed the same chunks. */
static int verify_analysis(const char *tag, const int16_t *pcm, int pairs)
{
    static const int chunk_pairs[5] = {1, 3, 11, 2, 7};
    static unsigned char code[SEG_PAIRS];
    static unsigned char ref_code[SEG_PAIRS];
    struct transmit_ref ref;
    G722_ENC_CTX *enc;
    int pos;
    int c;
    int n;
    int bad = 0;

    enc = g722_encoder_new(64000, 0);
    TASK_CHECK(enc != NULL);
    transmit_ref_init(&ref);
    TASK_CHECK(ref.low_enc != NULL && ref.high_enc != NULL);
    TASK_CHECK(transmit_ref_prepare(&ref, pcm, pairs) == 0);

    pos = 0;
    c = 0;
    while (pos < pairs)
    {
        int count = chunk_pairs[c % 5];

        if (pos + count > pairs)
            count = pairs - pos;
        TASK_CHECK(g722_encode(enc, pcm + 2*pos, 2*count, code + pos) == count);
        TASK_CHECK(transmit_ref_encode(&ref, pos, count) == 0);
        pos += count;
        c++;
    }
    g722_encoder_destroy(enc);

    for (n = 0; n < pairs; n++)
    {
        ref_code[n] = (unsigned char) expected_code(&ref, n);
        if (code[n] != ref_code[n])
        {
            fprintf(stderr, "  analysis/%s rate=64000 pos=%d codeword got=%02x want=%02x\n",
                    tag, n, code[n], ref_code[n]);
            bad++;
        }
    }
    TASK_CHECK(bad == 0);
    transmit_ref_free(&ref);
    return 0;
}

static int case_analysis_two_tone(void)
{
    static int16_t pcm[SEG_SAMPLES];

    gen_two_tone(pcm, SEG_SAMPLES);
    return verify_analysis("two-tone", pcm, SEG_PAIRS);
}

static int case_analysis_even_impulses(void)
{
    static int16_t pcm[SEG_SAMPLES];

    gen_impulses(pcm, SEG_SAMPLES, 0);
    return verify_analysis("even-impulses", pcm, 120);
}

static int case_analysis_odd_impulses(void)
{
    static int16_t pcm[SEG_SAMPLES];

    gen_impulses(pcm, SEG_SAMPLES, 1);
    return verify_analysis("odd-impulses", pcm, 120);
}

static int case_analysis_signed(void)
{
    static int16_t pcm[SEG_SAMPLES];

    gen_signed(pcm, SEG_SAMPLES);
    return verify_analysis("signed", pcm, SEG_PAIRS);
}

/* Transmit side of the silence splice.  The reference sub-band stream ends in
 * zeros; its QMF still has the last signal pairs in its tap window, so the
 * filtered low/high bands keep ringing for a few pairs and must match the
 * library encoder's own band split sample by sample.  This is the send-side
 * counterpart of the receive-side tail check; the two are never validated by
 * round-tripping one into the other. */
static int case_analysis_silence_tail(void)
{
    static int16_t pcm[2*(SEG_PAIRS + TAIL_PAIRS)];
    static unsigned char code[SEG_PAIRS + TAIL_PAIRS];
    static unsigned char ref_code[SEG_PAIRS + TAIL_PAIRS];
    struct transmit_ref ref;
    G722_ENC_CTX *enc;
    int total = SEG_PAIRS + TAIL_PAIRS;
    int n;
    int tail_ring = 0;

    gen_two_tone(pcm, 2*SEG_PAIRS);
    memset(pcm + 2*SEG_PAIRS, 0, (size_t) (2*TAIL_PAIRS)*sizeof(pcm[0]));
    enc = g722_encoder_new(64000, 0);
    TASK_CHECK(enc != NULL);
    TASK_CHECK(g722_encode(enc, pcm, 2*total, code) == total);
    g722_encoder_destroy(enc);

    transmit_ref_init(&ref);
    TASK_CHECK(ref.low_enc != NULL && ref.high_enc != NULL);
    TASK_CHECK(transmit_ref_prepare(&ref, pcm, total) == 0);
    TASK_CHECK(transmit_ref_encode(&ref, 0, total) == 0);
    for (n = 0; n < total; n++)
    {
        ref_code[n] = (unsigned char) expected_code(&ref, n);
        if (code[n] != ref_code[n])
        {
            fprintf(stderr, "  analysis/silence-tail rate=64000 pos=%d codeword "
                    "got=%02x want=%02x\n", n, code[n], ref_code[n]);
            TASK_CHECK(0);
        }
    }
    /* The filter must still be ringing just after the splice, but the QMF
     * window is only 12 pairs long, so the far tail must be quiet. */
    for (n = SEG_PAIRS; n < SEG_PAIRS + 12; n++)
    {
        if (ref.raw_low[n] != 0 || ref.raw_high[n] != 0)
            tail_ring++;
    }
    TASK_CHECK(tail_ring > 0);
    for (n = total - 8; n < total; n++)
        TASK_CHECK(ref.raw_low[n] == 0 && ref.raw_high[n] == 0);
    transmit_ref_free(&ref);
    return 0;
}

/* Scaling and ordering check on a signal that isolates one branch.  A
 * single-sample impulse at an odd position excites only the odd-phase
 * accumulator, so the reference must report low == high; the same impulse one
 * sample later excites only the even-phase accumulator, so low == -high.  The
 * magnitude of the first non-zero pair is pinned exactly against the
 * coefficient that lands on the impulse, which catches a wrong shift or scale
 * factor. */
static int case_analysis_scaling(void)
{
    static int16_t pcm[64];
    static int low[32];
    static int high[32];
    int n;
    int odd_idx = -1;
    int even_idx = -1;
    int odd_tap = -1;
    int even_tap = -1;

    memset(pcm, 0, sizeof(pcm));
    pcm[1] = 2048;
    analysis_reference(pcm, 32, low, high);
    for (n = 0; n < 32; n++)
    {
        if (low[n] != high[n])
            TASK_CHECK(0);
        if (low[n] != 0 && odd_idx < 0)
        {
            odd_idx = n;
            /* One tap reaches the impulse: k_odd = 2n + 1 - 2j = 1 => j = n. */
            odd_tap = n;
        }
    }
    TASK_CHECK(odd_idx >= 0 && odd_tap >= 0 && odd_tap < 12);
    TASK_CHECK(low[odd_idx] == limit16(floor_div_pow2((long long) QMF_C[odd_tap]*2048, 14)));

    memset(pcm, 0, sizeof(pcm));
    pcm[2] = 2048;
    analysis_reference(pcm, 32, low, high);
    for (n = 0; n < 32; n++)
    {
        if (low[n] + high[n] < -1 || low[n] + high[n] > 0)
            TASK_CHECK(0);
        if (low[n] != 0 && even_idx < 0)
        {
            even_idx = n;
            /* k_even = 2n - 22 + 2j = 2 => j = 12 - n. */
            even_tap = 12 - n;
        }
    }
    TASK_CHECK(even_idx >= 0 && even_tap >= 0 && even_tap < 12);
    TASK_CHECK(low[even_idx] == limit16(floor_div_pow2((long long) QMF_C[even_tap]*2048, 14)));
    TASK_CHECK(high[even_idx] == limit16(floor_div_pow2(-(long long) QMF_C[even_tap]*2048, 14)));
    return 0;
}

/* The two reference ADPCM encoders must keep separate band histories.  If the
 * low and high encoders were accidentally the same instance, or one was reset
 * between bands, the combined codewords would stop matching the library.  Here
 * we additionally confirm the two index streams are not identical, so the
 * concatenation step is actually doing work. */
static int case_analysis_band_isolation(void)
{
    static int16_t pcm[SEG_SAMPLES];
    static unsigned char ref_code[SEG_PAIRS];
    struct transmit_ref ref;
    int n;
    int high_diffs = 0;

    gen_two_tone(pcm, SEG_SAMPLES);
    transmit_ref_init(&ref);
    TASK_CHECK(ref.low_enc != NULL && ref.high_enc != NULL);
    TASK_CHECK(ref.low_enc != ref.high_enc);
    TASK_CHECK(transmit_ref_prepare(&ref, pcm, SEG_PAIRS) == 0);
    TASK_CHECK(transmit_ref_encode(&ref, 0, SEG_PAIRS) == 0);
    for (n = 0; n < SEG_PAIRS; n++)
    {
        ref_code[n] = (unsigned char) expected_code(&ref, n);
        if (((ref_code[n] >> 6) & 0x03) != ((ref.low_code[n] >> 6) & 0x03))
            high_diffs++;
        /* The high band is genuinely its own encoder state, not a copy. */
        TASK_CHECK(ref.high_enc->band[0].det != ref.low_enc->band[0].det
                   || ref.high_enc->band[1].det != ref.low_enc->band[1].det
                   || ref.high_enc->band[0].nb != ref.low_enc->band[0].nb);
    }
    TASK_CHECK(high_diffs > 0);
    transmit_ref_free(&ref);
    return 0;
}

/* Phase, ordering and scaling of the two analysis branches.  A pulse at an
 * odd sample position only reaches the odd-phase tap set, so low == high; a
 * pulse at an even sample position only reaches the even-phase tap set, so
 * low == -high (up to the floor rounding of the final shift).  The onset pair
 * also shifts by one between the two cases.  A reversed coefficient direction
 * destroys both relations, which is asserted here directly. */
static int case_analysis_phase(void)
{
    static int16_t even_pcm[SEG_SAMPLES];
    static int16_t odd_pcm[SEG_SAMPLES];
    static int low_even[SEG_PAIRS];
    static int high_even[SEG_PAIRS];
    static int low_odd[SEG_PAIRS];
    static int high_odd[SEG_PAIRS];
    static int low_rev[SEG_PAIRS];
    static int high_rev[SEG_PAIRS];
    int n;
    int first_even = -1;
    int first_odd = -1;
    int rev_diffs = 0;

    gen_impulses(even_pcm, SEG_SAMPLES, 0);
    gen_impulses(odd_pcm, SEG_SAMPLES, 1);
    analysis_reference(even_pcm, 120, low_even, high_even);
    analysis_reference(odd_pcm, 120, low_odd, high_odd);
    analysis_reference_reversed(even_pcm, 120, low_rev, high_rev);

    for (n = 0; n < 120; n++)
    {
        if (first_even < 0 && (low_even[n] || high_even[n]))
            first_even = n;
        if (first_odd < 0 && (low_odd[n] || high_odd[n]))
            first_odd = n;
    }
    TASK_CHECK(first_even >= 0 && first_odd >= 0);
    TASK_CHECK(first_odd == first_even + 1);

    for (n = 0; n < 120; n++)
    {
        /* Odd-position pulses: the two branches see the same value. */
        TASK_CHECK(low_odd[n] == high_odd[n]);
        /* Even-position pulses: the two branches see opposite values; the
         * floor rounding can differ by at most one. */
        TASK_CHECK(low_even[n] + high_even[n] >= -1);
        TASK_CHECK(low_even[n] + high_even[n] <= 0);
    }
    for (n = 0; n < 120; n++)
    {
        if (low_rev[n] != low_even[n] || high_rev[n] != high_even[n])
            rev_diffs++;
    }
    TASK_CHECK(rev_diffs > 0);
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Synthesis group: receive QMF on independently decoded sub-bands.
 * ------------------------------------------------------------------------- */

/* Decode the same delivered codewords with the library and with a fresh
 * ITU-test-mode decoder, run the test's own causal QMF over the reconstructed
 * sub-bands, and compare every output sample, including start-up and tail. */
static int verify_synthesis(const char *tag, const unsigned char *code, int pairs, int rate)
{
    G722_DEC_CTX *dec;
    G722_DEC_CTX *ref_dec;
    static int16_t got[SEG_SAMPLES];
    static int16_t sub[SEG_SAMPLES];
    static int16_t want[SEG_SAMPLES];
    static int rlow[SEG_PAIRS];
    static int rhigh[SEG_PAIRS];
    int n;
    int bad = 0;

    TASK_CHECK(pairs > 0 && pairs <= SEG_PAIRS);
    dec = g722_decoder_new(rate, 0);
    ref_dec = g722_decoder_new(rate, 0);
    TASK_CHECK(dec != NULL && ref_dec != NULL);
    ref_dec->itu_test_mode = TRUE;
    TASK_CHECK(g722_decode(dec, code, pairs, got) == 2*pairs);
    TASK_CHECK(g722_decode(ref_dec, code, pairs, sub) == 2*pairs);
    for (n = 0; n < pairs; n++)
    {
        rlow[n] = sub[2*n] >> 1;
        rhigh[n] = sub[2*n + 1] >> 1;
    }
    synthesis_reference(rlow, rhigh, pairs, want);
    for (n = 0; n < 2*pairs; n++)
    {
        if (got[n] != want[n])
        {
            report(tag, rate, n, "sample", got[n], want[n]);
            bad++;
            if (bad >= 8)
                break;
        }
    }
    TASK_CHECK(bad == 0);
    g722_decoder_destroy(dec);
    g722_decoder_destroy(ref_dec);
    return 0;
}

static int encode_at(int16_t *pcm, int samples, int rate, unsigned char *code)
{
    G722_ENC_CTX *enc = g722_encoder_new(rate, 0);
    int bytes;

    TASK_CHECK(enc != NULL);
    bytes = g722_encode(enc, pcm, samples, code);
    g722_encoder_destroy(enc);
    return bytes;
}

static int case_synthesis_segment(void)
{
    static int16_t pcm[SEG_SAMPLES];
    static unsigned char code[SEG_PAIRS];
    int rates[3] = {64000, 56000, 48000};
    int r;

    gen_two_tone(pcm, SEG_SAMPLES);
    for (r = 0; r < 3; r++)
    {
        TASK_CHECK(encode_at(pcm, SEG_SAMPLES, rates[r], code) == SEG_PAIRS);
        if (verify_synthesis("synthesis/segment", code, SEG_PAIRS, rates[r]) != 0)
            return 1;
    }
    return 0;
}

/* Chunked delivery must be transparent: decoding the codeword stream in
 * irregular blocks must produce exactly the same PCM as one whole-array call,
 * including the QMF state carried between calls.  This is the receive-side
 * analogue of the "one instance spans the segment" rule. */
static int case_synthesis_chunked(void)
{
    static int16_t pcm[SEG_SAMPLES];
    static unsigned char code[SEG_PAIRS];
    static int16_t whole[SEG_SAMPLES];
    static int16_t chunked[SEG_SAMPLES];
    static const int chunks[7] = {1, 3, 11, 2, 1, 7, 5};
    G722_DEC_CTX *whole_dec;
    G722_DEC_CTX *chunk_dec;
    int pos;
    int c;
    int total;

    gen_two_tone(pcm, SEG_SAMPLES);
    TASK_CHECK(encode_at(pcm, SEG_SAMPLES, 64000, code) == SEG_PAIRS);
    whole_dec = g722_decoder_new(64000, 0);
    chunk_dec = g722_decoder_new(64000, 0);
    TASK_CHECK(whole_dec != NULL && chunk_dec != NULL);
    TASK_CHECK(g722_decode(whole_dec, code, SEG_PAIRS, whole) == SEG_SAMPLES);
    pos = 0;
    c = 0;
    total = 0;
    while (pos < SEG_PAIRS)
    {
        int count = chunks[c % 7];

        if (pos + count > SEG_PAIRS)
            count = SEG_PAIRS - pos;
        total += g722_decode(chunk_dec, code + pos, count, chunked + 2*pos);
        pos += count;
        c++;
    }
    TASK_CHECK(total == SEG_SAMPLES);
    TASK_CHECK(memcmp(whole, chunked, sizeof(whole)) == 0);
    g722_decoder_destroy(whole_dec);
    g722_decoder_destroy(chunk_dec);
    return 0;
}

/* Signal followed by silence: the QMF tail must keep ringing and must match
 * sample by sample, not merely "decay to zero".  The encoder of an appended
 * run of zero samples still emits low-level codes, so the tail is expected to
 * settle towards a small residual rather than become exactly zero; the
 * substantive assertion is the sample-by-sample match, plus a monotone energy
 * decrease across the tail. */
static int case_synthesis_silence_tail(void)
{
    static int16_t pcm[2*(SEG_PAIRS + TAIL_PAIRS)];
    static unsigned char code[SEG_PAIRS + TAIL_PAIRS];
    static int16_t got[2*(SEG_PAIRS + TAIL_PAIRS)];
    static int16_t sub[2*(SEG_PAIRS + TAIL_PAIRS)];
    static int16_t want[2*(SEG_PAIRS + TAIL_PAIRS)];
    static int rlow[SEG_PAIRS + TAIL_PAIRS];
    static int rhigh[SEG_PAIRS + TAIL_PAIRS];
    G722_ENC_CTX *enc;
    G722_DEC_CTX *dec;
    G722_DEC_CTX *ref_dec;
    int total = SEG_PAIRS + TAIL_PAIRS;
    int n;
    long long early = 0;
    long long late = 0;

    gen_two_tone(pcm, 2*SEG_PAIRS);
    memset(pcm + 2*SEG_PAIRS, 0, (size_t) (2*TAIL_PAIRS)*sizeof(pcm[0]));
    enc = g722_encoder_new(64000, 0);
    TASK_CHECK(enc != NULL);
    TASK_CHECK(g722_encode(enc, pcm, 2*total, code) == total);
    g722_encoder_destroy(enc);

    dec = g722_decoder_new(64000, 0);
    ref_dec = g722_decoder_new(64000, 0);
    TASK_CHECK(dec != NULL && ref_dec != NULL);
    ref_dec->itu_test_mode = TRUE;
    TASK_CHECK(g722_decode(dec, code, total, got) == 2*total);
    TASK_CHECK(g722_decode(ref_dec, code, total, sub) == 2*total);
    for (n = 0; n < total; n++)
    {
        rlow[n] = sub[2*n] >> 1;
        rhigh[n] = sub[2*n + 1] >> 1;
    }
    synthesis_reference(rlow, rhigh, total, want);
    TASK_CHECK(memcmp(got, want, sizeof(got)) == 0);

    /* The QMF history still carries the last signal pairs into the first
     * silence pairs; the late tail must be substantially quieter. */
    for (n = 2*SEG_PAIRS; n < 2*SEG_PAIRS + 16; n++)
        early += (long long) want[n]*want[n];
    for (n = 2*total - 16; n < 2*total; n++)
        late += (long long) want[n]*want[n];
    TASK_CHECK(early > 0);
    TASK_CHECK(late < early);
    g722_decoder_destroy(dec);
    g722_decoder_destroy(ref_dec);
    return 0;
}

/* Start-up: the very first output samples must match too.  Both channels are
 * zero-initialised, so any off-by-one in history shifting shows up here. */
static int case_synthesis_startup(void)
{
    static unsigned char code[24];
    static int16_t got[48];
    static int16_t sub[48];
    static int16_t want[48];
    static int rlow[24];
    static int rhigh[24];
    G722_DEC_CTX *dec;
    G722_DEC_CTX *ref_dec;
    int n;

    for (n = 0; n < 24; n++)
        code[n] = (unsigned char) (0x0D + n);
    dec = g722_decoder_new(64000, 0);
    ref_dec = g722_decoder_new(64000, 0);
    TASK_CHECK(dec != NULL && ref_dec != NULL);
    ref_dec->itu_test_mode = TRUE;
    TASK_CHECK(g722_decode(dec, code, 24, got) == 48);
    TASK_CHECK(g722_decode(ref_dec, code, 24, sub) == 48);
    for (n = 0; n < 24; n++)
    {
        rlow[n] = sub[2*n] >> 1;
        rhigh[n] = sub[2*n + 1] >> 1;
    }
    synthesis_reference(rlow, rhigh, 24, want);
    TASK_CHECK(memcmp(got, want, sizeof(got)) == 0);
    g722_decoder_destroy(dec);
    g722_decoder_destroy(ref_dec);
    return 0;
}

/* Phase, ordering and scaling of the two receive branches.  With the low-band
 * index pinned to zero at 48 kbit/s the reconstruction stays at zero, so the
 * input to the QMF is (+rhigh, -rhigh): the two output phases get opposite
 * excitations and must not be interchangeable.  The direct impulse response of
 * the reference also shows the mirrored coefficient order: the even output
 * walks C[0..11] forward, the odd output walks it backward. */
static int case_synthesis_phase(void)
{
    static unsigned char code[64];
    static int16_t got[128];
    static int16_t sub[128];
    static int16_t want[128];
    static int rlow[64];
    static int rhigh[64];
    G722_DEC_CTX *dec;
    G722_DEC_CTX *ref_dec;
    int imp_low[32];
    int imp_high[32];
    int16_t imp_pcm[64];
    int n;
    int even_diffs = 0;

    for (n = 0; n < 64; n++)
    {
        /* 48 kbit/s: low four bits 0 (rlow == 0), high index cycles. */
        code[n] = (unsigned char) (((n % 3) + 1) << 4);
    }
    dec = g722_decoder_new(48000, 0);
    ref_dec = g722_decoder_new(48000, 0);
    TASK_CHECK(dec != NULL && ref_dec != NULL);
    ref_dec->itu_test_mode = TRUE;
    TASK_CHECK(g722_decode(dec, code, 64, got) == 128);
    TASK_CHECK(g722_decode(ref_dec, code, 64, sub) == 128);
    for (n = 0; n < 64; n++)
    {
        rlow[n] = sub[2*n] >> 1;
        rhigh[n] = sub[2*n + 1] >> 1;
        /* The premise: only the high band is excited. */
        TASK_CHECK(rlow[n] == 0);
    }
    synthesis_reference(rlow, rhigh, 64, want);
    TASK_CHECK(memcmp(got, want, sizeof(got)) == 0);
    for (n = 0; n < 64; n++)
    {
        if (got[2*n] != got[2*n + 1])
            even_diffs++;
    }
    TASK_CHECK(even_diffs > 0);

    /* Reference impulse response: the even output walks C[0..11] forward, the
     * odd output walks it backward over the same 12-pair window.  A clean
     * rlow impulse at pair 10 gives
     *     out[2*(10+j)]     = C[j] * 4096 >> 11 = 2*C[j]
     *     out[2*(10+j)+1]   = C[j] * 4096 >> 11 = 2*C[j]
     * located symmetrically around the impulse. */
    for (n = 0; n < 32; n++)
    {
        imp_low[n] = 0;
        imp_high[n] = 0;
    }
    imp_low[10] = 1 << 12;                 /* a clean rlow impulse */
    synthesis_reference(imp_low, imp_high, 32, imp_pcm);
    for (n = 0; n < 12; n++)
    {
        long long term = ((long long) QMF_C[n]*(1 << 12)) >> 11;

        TASK_CHECK(imp_pcm[2*(10 + n)] == limit16(term));
        TASK_CHECK(imp_pcm[2*(10 + 11 - n) + 1] == limit16(term));
    }
    g722_decoder_destroy(dec);
    g722_decoder_destroy(ref_dec);
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Packing group.
 * ------------------------------------------------------------------------- */

/* The packed encoder must place codewords LSB-first and carry residual bits
 * across calls.  The reference packs the same non-packed codes itself; the
 * library is fed irregular chunks that end mid-byte. */
static int case_packing_encode(void)
{
    static const int chunk_pairs[6] = {1, 3, 11, 2, 7, 5};
    static int16_t pcm[SEG_SAMPLES];
    static unsigned char packed[PACK_BYTES];
    static unsigned char plain[SEG_PAIRS];
    static unsigned char expect[PACK_BYTES];
    int rates[2] = {56000, 48000};
    int bits[2] = {7, 6};
    int r;

    gen_two_tone(pcm, SEG_SAMPLES);
    for (r = 0; r < 2; r++)
    {
        G722_ENC_CTX *pack_enc = g722_encoder_new(rates[r], G722_PACKED);
        G722_ENC_CTX *plain_enc = g722_encoder_new(rates[r], 0);
        int pos = 0;
        int c = 0;
        int bytes = 0;

        TASK_CHECK(pack_enc != NULL && plain_enc != NULL);
        while (pos < SEG_PAIRS)
        {
            int count = chunk_pairs[c % 6];
            int before = bytes;
            int total;

            if (pos + count > SEG_PAIRS)
                count = SEG_PAIRS - pos;
            TASK_CHECK(g722_encode(plain_enc, pcm + 2*pos, 2*count, plain + pos) == count);
            total = pos + count;
            TASK_CHECK(g722_encode(pack_enc, pcm + 2*pos, 2*count, packed + bytes)
                       == total*bits[r]/8 - before);
            bytes = total*bits[r]/8;
            pos = total;
            c++;
        }
        TASK_CHECK(bytes == SEG_PAIRS*bits[r]/8);
        TASK_CHECK(pack_lsb_first(plain, SEG_PAIRS, bits[r], expect) == bytes);
        if (memcmp(packed, expect, (size_t) bytes) != 0)
        {
            int i;

            for (i = 0; i < bytes && packed[i] == expect[i]; i++)
                ;
            fprintf(stderr, "  packing/encode rate=%d byte=%d got=%02x want=%02x "
                    "(code=%d bit=%d)\n", rates[r], i, packed[i], expect[i],
                    i*8/bits[r], (i*8) % bits[r]);
            TASK_CHECK(0);
        }
        g722_encoder_destroy(pack_enc);
        g722_encoder_destroy(plain_enc);
    }
    return 0;
}

/* The delivered prefix and the valid residual bits are a pure function of the
 * number of bytes fed, given the library rule "return as soon as the last byte
 * of this call is consumed".  This is checked with 1/3/11-byte splits, an
 * empty call, and splits that complete a residual codeword on the next call.
 * The existing one-byte delay is reproduced by the model, not treated as loss.
 *
 * The packed stream is the packed form of a known non-packed codeword array, so
 * the delivered prefix is also cross-checked against an independently unpacked
 * prefix and against a non-packed decoder fed the same codewords. */
static int case_packing_decode_prefix(void)
{
    static const int chunks[10] = {1, 3, 11, 1, 11, 3, 2, 5, 1, 4};
    static unsigned char stream[PACK_BYTES];
    static unsigned char plain[PACK_BYTES];
    static unsigned char unpacked[PACK_BYTES];
    static int16_t out[4*PACK_BYTES];
    static int16_t plain_out[4*PACK_BYTES];
    int rates[2] = {56000, 48000};
    int bits[2] = {7, 6};
    int r;

    for (r = 0; r < 2; r++)
    {
        G722_DEC_CTX *dec;
        G722_DEC_CTX *plain_dec;
        int bytes = 0;
        int delivered = 0;
        int k;

        /* 96 codewords exactly fill whole bytes at both rates. */
        for (k = 0; k < 96; k++)
            plain[k] = (unsigned char) (((k*13) & ((1 << bits[r]) - 1)));
        TASK_CHECK(pack_lsb_first(plain, 96, bits[r], stream) == 96*bits[r]/8);

        dec = g722_decoder_new(rates[r], G722_PACKED);
        plain_dec = g722_decoder_new(rates[r], 0);
        TASK_CHECK(dec != NULL && plain_dec != NULL);
        TASK_CHECK(g722_decode(plain_dec, plain, 96, plain_out) == 192);

        for (k = 0; k < 10; k++)
        {
            int got;
            int model_delivered;
            int residual;
            int complete;
            unsigned valid;

            got = g722_decode(dec, stream + bytes, chunks[k], out + 2*delivered);
            bytes += chunks[k];
            TASK_CHECK(got % 2 == 0);
            delivered += got/2;
            model_delivered = (8*bytes - 1)/bits[r];
            residual = 8*bytes - bits[r]*model_delivered;
            valid = dec->in_buffer & ((1u << dec->in_bits) - 1u);
            if (delivered != model_delivered || dec->in_bits != residual)
            {
                fprintf(stderr, "  packing/prefix rate=%d bytes=%d delivered=%d model=%d "
                        "in_bits=%d residual=%d\n", rates[r], bytes, delivered,
                        model_delivered, dec->in_bits, residual);
                TASK_CHECK(0);
            }
            /* Only the valid residual bits are compared; the bits above
             * in_bits are unspecified and out of contract. */
            TASK_CHECK((valid & ((1u << dec->in_bits) - 1u))
                       == read_bits_lsb_first(stream, bits[r]*model_delivered, dec->in_bits));
            /* The delivered prefix equals the same codewords fed non-packed. */
            TASK_CHECK(memcmp(out, plain_out, (size_t) 2*delivered*sizeof(out[0])) == 0);
            /* ... and equals the independently unpacked prefix.  The unpacked
             * bytes must also reproduce the original codeword array. */
            complete = unpack_lsb_first(stream, 8*bytes, bits[r], unpacked, PACK_BYTES);
            TASK_CHECK(complete == (8*bytes)/bits[r]);
            TASK_CHECK(memcmp(unpacked, plain, (size_t) model_delivered*sizeof(plain[0])) == 0);
            {
                G722_DEC_CTX *check = g722_decoder_new(rates[r], 0);
                static int16_t check_out[4*PACK_BYTES];

                TASK_CHECK(check != NULL);
                TASK_CHECK(g722_decode(check, unpacked, model_delivered, check_out)
                           == 2*model_delivered);
                TASK_CHECK(memcmp(out, check_out, (size_t) 2*delivered*sizeof(out[0])) == 0);
                g722_decoder_destroy(check);
            }
        }
        /* An empty call must not drain the cache. */
        {
            int before_bits = dec->in_bits;
            unsigned before_buffer = dec->in_buffer;
            int got = g722_decode(dec, stream, 0, out);

            TASK_CHECK(got == 0);
            TASK_CHECK(dec->in_bits == before_bits);
            TASK_CHECK(dec->in_buffer == before_buffer);
        }
        g722_decoder_destroy(dec);
        g722_decoder_destroy(plain_dec);
    }
    return 0;
}

/* A stream of complete codewords that exactly fills whole bytes must not yield
 * padding samples beyond the codewords.  The last codeword is still cached
 * after the array call and arrives on the next non-empty call. */
static int case_packing_full_words(void)
{
    static int16_t pcm[2*64];
    static unsigned char pack[PACK_BYTES];
    static unsigned char plain[64];
    static int16_t plan_out[128];
    static int16_t got[128];
    int rates[2] = {56000, 48000};
    int bits[2] = {7, 6};
    int r;

    gen_two_tone(pcm, 2*64);
    for (r = 0; r < 2; r++)
    {
        G722_ENC_CTX *plain_enc = g722_encoder_new(rates[r], 0);
        G722_ENC_CTX *pack_enc = g722_encoder_new(rates[r], G722_PACKED);
        G722_DEC_CTX *pack_dec = g722_decoder_new(rates[r], G722_PACKED);
        G722_DEC_CTX *plain_dec = g722_decoder_new(rates[r], 0);
        int bytes;
        int got_samples;
        int model_cached;
        unsigned char extra = 0x5A;
        int flushed;

        TASK_CHECK(plain_enc && pack_enc && pack_dec && plain_dec);
        TASK_CHECK(g722_encode(plain_enc, pcm, 2*64, plain) == 64);
        TASK_CHECK(g722_decode(plain_dec, plain, 64, plan_out) == 128);
        bytes = g722_encode(pack_enc, pcm, 2*64, pack);
        TASK_CHECK(bytes == 64*bits[r]/8);
        TASK_CHECK(pack_lsb_first(plain, 64, bits[r], pack + PACK_BYTES/2) == bytes);
        TASK_CHECK(memcmp(pack, pack + PACK_BYTES/2, (size_t) bytes) == 0);

        /* Model: every complete codeword is delivered except the one held when
         * the final byte was consumed. */
        model_cached = (8*bytes - 1)/bits[r];
        got_samples = g722_decode(pack_dec, pack, bytes, got);
        TASK_CHECK(got_samples == 2*model_cached);
        TASK_CHECK(memcmp(got, plan_out, (size_t) got_samples*sizeof(got[0])) == 0);
        TASK_CHECK(pack_dec->in_bits == 8*bytes - bits[r]*model_cached);

        /* The cached codeword is delivered by the next non-empty call and is
         * exactly the last complete codeword; the byte after it supplies no
         * extra sound that we count. */
        TASK_CHECK(g722_decode(pack_dec, &extra, 0, got) == 0);
        flushed = g722_decode(pack_dec, &extra, 1, got);
        TASK_CHECK(flushed >= 2);
        TASK_CHECK(got[0] == plan_out[2*(64 - 1)]);
        TASK_CHECK(got[1] == plan_out[2*(64 - 1) + 1]);

        g722_encoder_destroy(plain_enc);
        g722_encoder_destroy(pack_enc);
        g722_decoder_destroy(pack_dec);
        g722_decoder_destroy(plain_dec);
    }
    return 0;
}

/* Interleaved double instances must not share history. */
static int case_packing_interleaved(void)
{
    static int16_t pcm_a[2*64];
    static int16_t pcm_b[2*64];
    static unsigned char pack_a[PACK_BYTES];
    static unsigned char pack_b[PACK_BYTES];
    static unsigned char plain_a[64];
    static unsigned char plain_b[64];
    static int16_t want_a[128];
    static int16_t want_b[128];
    static int16_t out_a[128];
    static int16_t out_b[128];
    int rates[2] = {56000, 48000};
    int r;

    gen_two_tone(pcm_a, 2*64);
    gen_signed(pcm_b, 2*64);
    for (r = 0; r < 2; r++)
    {
        G722_ENC_CTX *ea = g722_encoder_new(rates[r], G722_PACKED);
        G722_ENC_CTX *eb = g722_encoder_new(rates[r], G722_PACKED);
        G722_DEC_CTX *da = g722_decoder_new(rates[r], G722_PACKED);
        G722_DEC_CTX *db = g722_decoder_new(rates[r], G722_PACKED);
        G722_ENC_CTX *pa = g722_encoder_new(rates[r], 0);
        G722_ENC_CTX *pb = g722_encoder_new(rates[r], 0);
        G722_DEC_CTX *ra = g722_decoder_new(rates[r], 0);
        G722_DEC_CTX *rb = g722_decoder_new(rates[r], 0);
        int produced_a = 0;
        int produced_b = 0;
        int n;

        TASK_CHECK(ea && eb && da && db && pa && pb && ra && rb);
        g722_encode(ea, pcm_a, 2*64, pack_a);
        g722_encode(eb, pcm_b, 2*64, pack_b);
        TASK_CHECK(g722_encode(pa, pcm_a, 2*64, plain_a) == 64);
        TASK_CHECK(g722_encode(pb, pcm_b, 2*64, plain_b) == 64);
        TASK_CHECK(g722_decode(ra, plain_a, 64, want_a) == 128);
        TASK_CHECK(g722_decode(rb, plain_b, 64, want_b) == 128);

        /* Alternate one-byte calls between the two packed instances. */
        for (n = 0; n < 16; n++)
        {
            produced_a += g722_decode(da, pack_a + n, 1, out_a + 2*produced_a)/2;
            produced_b += g722_decode(db, pack_b + n, 1, out_b + 2*produced_b)/2;
        }
        TASK_CHECK(produced_a == produced_b);
        TASK_CHECK(memcmp(out_a, want_a, (size_t) produced_a*2*sizeof(out_a[0])) == 0);
        TASK_CHECK(memcmp(out_b, want_b, (size_t) produced_b*2*sizeof(out_b[0])) == 0);
        TASK_CHECK(memcmp(out_a, out_b, (size_t) produced_a*2*sizeof(out_a[0])) != 0);

        g722_encoder_destroy(ea);
        g722_encoder_destroy(eb);
        g722_decoder_destroy(da);
        g722_decoder_destroy(db);
        g722_encoder_destroy(pa);
        g722_encoder_destroy(pb);
        g722_decoder_destroy(ra);
        g722_decoder_destroy(rb);
    }
    return 0;
}

/* 8 kHz mode: the transmit QMF is bypassed, only the low band runs, and the
 * high-band state stays frozen on both sides. */
static int case_8k_bypass(void)
{
    static int16_t pcm[SEG_SAMPLES];
    static int16_t pcm8[SEG_SAMPLES/2];
    static unsigned char code8[SEG_SAMPLES/2];
    static unsigned char code_itu[SEG_SAMPLES/2];
    G722_ENC_CTX *enc8;
    G722_ENC_CTX *itu;
    G722_DEC_CTX *dec8;
    G722_DEC_CTX *dec_itu;
    static int16_t out8[SEG_SAMPLES/2];
    static int16_t out_itu[SEG_SAMPLES];
    int n;

    gen_two_tone(pcm, SEG_SAMPLES);
    for (n = 0; n < SEG_SAMPLES/2; n++)
        pcm8[n] = pcm[2*n];

    enc8 = g722_encoder_new(64000, G722_SAMPLE_RATE_8000);
    itu = g722_encoder_new(64000, 0);
    TASK_CHECK(enc8 != NULL && itu != NULL);
    itu->itu_test_mode = TRUE;
    TASK_CHECK(g722_encode(enc8, pcm8, SEG_SAMPLES/2, code8) == SEG_SAMPLES/2);
    TASK_CHECK(g722_encode(itu, pcm8, SEG_SAMPLES/2, code_itu) == SEG_SAMPLES/2);
    for (n = 0; n < SEG_SAMPLES/2; n++)
    {
        if ((code8[n] & 0x3F) != (code_itu[n] & 0x3F))
        {
            report("embedded/8k-low", 64000, n, "codeword", code8[n], code_itu[n]);
            TASK_CHECK(0);
        }
        TASK_CHECK((code8[n] >> 6) == 0x03);
    }
    for (n = 0; n < 24; n++)
        TASK_CHECK(enc8->x[n] == 0);
    TASK_CHECK(enc8->band[1].det == 8);
    TASK_CHECK(enc8->band[1].s == 0);
    TASK_CHECK(enc8->band[1].nb == 0);

    dec8 = g722_decoder_new(64000, G722_SAMPLE_RATE_8000);
    dec_itu = g722_decoder_new(64000, 0);
    TASK_CHECK(dec8 != NULL && dec_itu != NULL);
    dec_itu->itu_test_mode = TRUE;
    TASK_CHECK(g722_decode(dec8, code8, SEG_SAMPLES/2, out8) == SEG_SAMPLES/2);
    TASK_CHECK(g722_decode(dec_itu, code8, SEG_SAMPLES/2, out_itu) == SEG_SAMPLES);
    for (n = 0; n < SEG_SAMPLES/2; n++)
    {
        if (out8[n] != out_itu[2*n])
        {
            report("embedded/8k-bypass", 64000, n, "sample", out8[n], out_itu[2*n]);
            TASK_CHECK(0);
        }
    }
    for (n = 0; n < 24; n++)
        TASK_CHECK(dec8->x[n] == 0);
    TASK_CHECK(dec8->band[1].det == 8);
    TASK_CHECK(dec8->band[1].s == 0);
    TASK_CHECK(dec8->band[1].nb == 0);

    g722_encoder_destroy(enc8);
    g722_encoder_destroy(itu);
    g722_decoder_destroy(dec8);
    g722_decoder_destroy(dec_itu);
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Case table.
 * ------------------------------------------------------------------------- */

const struct task_case task_cases[] =
{
    {"embedded",  "code_hierarchy",        case_code_hierarchy},
    {"embedded",  "decoder_state_shared",  case_decoder_state_shared},
    {"embedded",  "refinement_state",      case_refinement_state},
    {"embedded",  "known_reconstruction",  case_known_reconstruction},
    {"embedded",  "state_field_isolation", case_state_field_isolation},
    {"embedded",  "eight_k_bypass",        case_8k_bypass},
    {"analysis",  "two_tone",              case_analysis_two_tone},
    {"analysis",  "even_impulses",         case_analysis_even_impulses},
    {"analysis",  "odd_impulses",          case_analysis_odd_impulses},
    {"analysis",  "signed_sequence",       case_analysis_signed},
    {"analysis",  "silence_tail",          case_analysis_silence_tail},
    {"analysis",  "scaling",               case_analysis_scaling},
    {"analysis",  "band_isolation",        case_analysis_band_isolation},
    {"analysis",  "phase_order",           case_analysis_phase},
    {"synthesis", "segment",               case_synthesis_segment},
    {"synthesis", "chunked",               case_synthesis_chunked},
    {"synthesis", "silence_tail",          case_synthesis_silence_tail},
    {"synthesis", "startup",               case_synthesis_startup},
    {"synthesis", "phase_order",           case_synthesis_phase},
    {"packing",   "encode_lsb_first",      case_packing_encode},
    {"packing",   "decode_prefix",         case_packing_decode_prefix},
    {"packing",   "full_words",            case_packing_full_words},
    {"packing",   "interleaved",           case_packing_interleaved}
};

const size_t task_case_count = sizeof(task_cases)/sizeof(task_cases[0]);
