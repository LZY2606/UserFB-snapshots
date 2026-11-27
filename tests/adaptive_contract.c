/*
 * Contract tests for the adaptive stages of libfreeaptx.
 *
 * Three groups are implemented behind task_group():
 *
 *   dither     - codeword history, per-subband dither, dither parity,
 *                codeword pack/unpack for aptX and aptX HD field plans,
 *                and the 8-beat sync parity cycle.
 *   predictor  - inverse quantization, factor_select / quantization factor
 *                adaptation, pole and zero weights, reconstructed and
 *                predicted samples, beat by beat per subband.
 *   stream     - public aptx_encode/aptx_decode on sustained stereo 24-bit
 *                audio for aptX and aptX HD, with the internal predictor
 *                state of both endpoints compared against the independent
 *                reference model after every emitted codeword.
 *
 * The reference model in this file is maintained independently from the
 * production implementation:
 *   - it keeps its own state (never copied out of struct aptx_context),
 *   - every rounding is expressed with wide-integer quotient/remainder,
 *   - 32-bit truncation is explicit two's complement,
 *   - the residual history is a plain newest-first array, not the
 *     production ring buffer,
 *   - no production rounding, clipping, dither, invert-quantize or
 *     prediction helper is called to compute an expectation,
 *   - stream expectations are driven by codewords parsed from the bytes
 *     actually emitted, unpacked per an explicit field plan, never by
 *     re-encoding.
 *
 * Only the format constant tables are read from the production source,
 * which is included below so the tests can observe the static stages.
 * The production sources and the public API are unchanged: a normal build
 * gains no callback, state or allocation.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "check.h"

#include "../freeaptx.c"

/* ------------------------------------------------------------------ */
/* assertion helper                                                    */

static char check_msg[192];

static void check_eq(int64_t got, int64_t want, const char *what,
                     long i0, long i1, long i2)
{
    if (got != want) {
        snprintf(check_msg, sizeof(check_msg),
                 "%s got=%lld want=%lld at (%ld,%ld,%ld)",
                 what, (long long)got, (long long)want, i0, i1, i2);
        TASK_CHECK(0, check_msg);
    } else {
        TASK_CHECK(1, what);
    }
}

/* ------------------------------------------------------------------ */
/* independent fixed-point primitives                                  */
/*                                                                     */
/* Wide integers, explicit quotient/remainder and explicit two's       */
/* complement truncation.  None of these call into the production      */
/* rounding or clipping helpers.                                       */

static int64_t ref_floor_div(int64_t a, int64_t b) /* b > 0 */
{
    int64_t q = a / b;
    if (a % b != 0 && a < 0)
        q--;
    return q;
}

static int64_t ref_mod(int64_t a, int64_t b) /* b > 0, result in [0,b) */
{
    int64_t r = a % b;
    if (r < 0)
        r += b;
    return r;
}

/* rounded right shift: add half, floor-divide, then tie-correct when the
 * discarded bits are exactly the rounding constant */
static int64_t ref_rshift(int64_t value, unsigned shift)
{
    const int64_t half = (int64_t)1 << (shift - 1);
    int64_t q = ref_floor_div(value + half, (int64_t)1 << shift);
    return q - (ref_mod(value, (int64_t)1 << (shift + 1)) == half);
}

/* explicit two's complement truncation to a signed 32-bit value */
static int32_t ref_trunc32(int64_t v)
{
    uint64_t u = (uint64_t)v & UINT64_C(0xFFFFFFFF);
    if (u >= UINT64_C(0x80000000))
        return (int32_t)((int64_t)u - INT64_C(0x100000000));
    return (int32_t)u;
}

/* explicit two's complement read of an unsigned 32-bit value */
static int32_t ref_s32(uint32_t u)
{
    if (u >= UINT32_C(0x80000000))
        return (int32_t)((int64_t)u - INT64_C(0x100000000));
    return (int32_t)u;
}

static int32_t ref_clip_p2(int64_t a, unsigned p)
{
    const int64_t max = ((int64_t)1 << p) - 1;
    const int64_t min = -((int64_t)1 << p);
    if (a > max) return (int32_t)max;
    if (a < min) return (int32_t)min;
    return (int32_t)a;
}

static int32_t ref_clip(int64_t a, int32_t amin, int32_t amax)
{
    if (a < amin) return amin;
    if (a > amax) return amax;
    return (int32_t)a;
}

static int32_t ref_sign_extend(uint32_t v, unsigned bits)
{
    if (v & (UINT32_C(1) << (bits - 1)))
        return (int32_t)((int64_t)v - ((int64_t)1 << bits));
    return (int32_t)v;
}

/* ------------------------------------------------------------------ */
/* reference model state                                               */

#define REF_MAX_ORDER 24

struct ref_subband {
    int32_t quantization_factor;             /* factor for the NEXT symbol */
    int32_t factor_select;
    int32_t reconstructed_difference;
    int32_t prev_sign[2];
    int32_t s_weight[2];                     /* pole weights */
    int32_t d_weight[REF_MAX_ORDER];         /* zero weights */
    int32_t hist[REF_MAX_ORDER + 1];         /* plain order: hist[0] newest */
    int32_t previous_reconstructed_sample;
    int32_t predicted_difference;
    int32_t predicted_sample;
};

struct ref_channel {
    uint32_t history;                        /* codeword history, mod 2^32 */
    int32_t dither[NB_SUBBANDS];
    int32_t dither_parity;
    int32_t prev_q[NB_SUBBANDS];             /* previous beat codewords */
    struct ref_subband sb[NB_SUBBANDS];
};

static void ref_subband_init(struct ref_subband *s)
{
    memset(s, 0, sizeof(*s));
    s->prev_sign[0] = 1;
    s->prev_sign[1] = 1;
}

static void ref_channel_init(struct ref_channel *ch)
{
    int sb;
    ch->history = 0;
    ch->dither_parity = 0;
    for (sb = 0; sb < NB_SUBBANDS; sb++) {
        ch->dither[sb] = 0;
        ch->prev_q[sb] = 0;
        ref_subband_init(&ch->sb[sb]);
    }
}

/* codeword history update + dither generation, from the independently
 * maintained previous codeword sequence stored in prev_q[] */
static void ref_update_dither(struct ref_channel *ch)
{
    uint32_t cw = (uint32_t)ref_mod(ch->prev_q[0], 4)
                | ((uint32_t)(ref_mod(ch->prev_q[1], 4) & 2) << 1)
                | ((uint32_t)ref_mod(ch->prev_q[2], 2) << 3);
    int32_t h, d;
    int64_t m;
    int sb;

    ch->history = (cw << 8) + (ch->history << 4); /* uint32_t wraps mod 2^32 */
    h = ref_s32(ch->history);

    m = (int64_t)5184443 * ref_floor_div(h, 128);
    d = ref_trunc32(m * 4 + ref_floor_div(m, (int64_t)1 << 22));
    for (sb = 0; sb < NB_SUBBANDS; sb++)
        ch->dither[sb] = ref_trunc32((int64_t)d * ((int64_t)1 << (23 - 5*sb)));
    ch->dither_parity = (int32_t)ref_mod(ref_floor_div(d, (int64_t)1 << 25), 2);
}

/* inverse quantization: current symbol uses the quantization factor that
 * was computed for it by the previous beat; the factor updated here is
 * only used by the next symbol */
static void ref_invert_quantization(struct ref_subband *s, int32_t qs,
                                    int32_t dither, const struct aptx_tables *t)
{
    int32_t idx = qs < 0 ? -qs : qs + 1;
    int32_t qr = t->quantize_intervals[idx] / 2; /* table entries positive */
    int64_t acc, fs;
    int32_t idx2, shift;

    if (qs < 0)
        qr = -qr;

    acc = (int64_t)qr * ((int64_t)1 << 32)
        + (int64_t)dither * t->invert_quantize_dither_factors[idx];
    qr = ref_clip_p2(ref_rshift(acc, 32), 23);
    s->reconstructed_difference =
        ref_trunc32(ref_floor_div((int64_t)s->quantization_factor * qr,
                                  (int64_t)1 << 19));

    fs = (int64_t)32620 * s->factor_select
       + (int64_t)t->quantize_factor_select_offset[idx] * ((int64_t)1 << 15);
    s->factor_select = ref_clip(ref_rshift(fs, 15), 0, t->factor_max);

    idx2 = (s->factor_select & 0xFF) >> 3;
    shift = (t->factor_max - s->factor_select) >> 8;
    s->quantization_factor = (quantization_factors[idx2] << 11) >> shift;
}

static void ref_prediction_filtering(struct ref_subband *s, int32_t rd, int order)
{
    int32_t reconstructed_sample =
        ref_clip_p2((int64_t)rd + s->predicted_sample, 23);
    int64_t pacc = (int64_t)s->s_weight[0] * s->previous_reconstructed_sample
                 + (int64_t)s->s_weight[1] * reconstructed_sample;
    int32_t predictor =
        ref_clip_p2(ref_trunc32(ref_floor_div(pacc, (int64_t)1 << 22)), 23);
    int32_t srd0 = ((rd > 0) - (rd < 0)) * ((int32_t)1 << 23);
    int64_t pd = 0;
    int i;

    s->previous_reconstructed_sample = reconstructed_sample;

    /* residual history in plain newest-first order, not a ring buffer */
    memmove(&s->hist[1], &s->hist[0], (size_t)order * sizeof(s->hist[0]));
    s->hist[0] = rd;

    for (i = 0; i < order; i++) {
        int32_t srd = s->hist[i+1] < 0 ? -1 : 1;
        s->d_weight[i] = ref_trunc32((int64_t)s->d_weight[i]
            - ref_rshift((int64_t)s->d_weight[i] - (int64_t)srd * srd0, 8));
        pd += (int64_t)s->hist[i] * s->d_weight[i];
    }

    s->predicted_difference =
        ref_clip_p2(ref_trunc32(ref_floor_div(pd, (int64_t)1 << 22)), 23);
    s->predicted_sample =
        ref_clip_p2((int64_t)predictor + s->predicted_difference, 23);
}

static void ref_process_subband(struct ref_subband *s, int32_t qs,
                                int32_t dither, const struct aptx_tables *t)
{
    int32_t sign, same0, same1, sw1, range;
    int64_t w;

    ref_invert_quantization(s, qs, dither, t);

    sign = (s->reconstructed_difference > -s->predicted_difference)
         - (s->reconstructed_difference < -s->predicted_difference);
    same0 = sign * s->prev_sign[0];
    same1 = sign * s->prev_sign[1];
    s->prev_sign[0] = s->prev_sign[1];
    s->prev_sign[1] = sign | 1;

    sw1 = (int32_t)ref_rshift(-(int64_t)same1 * s->s_weight[1], 1);
    sw1 = ref_clip(sw1, -0x100000, 0x100000);
    sw1 = (int32_t)ref_floor_div(sw1, 16) * 16; /* clear low 4 bits, two's complement */
    sw1 *= 16;

    w = (int64_t)254 * s->s_weight[0] + (int64_t)0x800000 * same0 + sw1;
    s->s_weight[0] = ref_clip(ref_rshift(w, 8), -0x300000, 0x300000);

    range = 0x3C0000 - s->s_weight[0];
    w = (int64_t)255 * s->s_weight[1] + (int64_t)0xC00000 * same1;
    s->s_weight[1] = ref_clip(ref_rshift(w, 8), -range, range);

    ref_prediction_filtering(s, s->reconstructed_difference, t->prediction_order);
}

static void ref_process_channel(struct ref_channel *ch, int hd,
                                const int32_t q[NB_SUBBANDS])
{
    int sb;
    for (sb = 0; sb < NB_SUBBANDS; sb++)
        ref_process_subband(&ch->sb[sb], q[sb], ch->dither[sb],
                            &all_tables[hd][sb]);
    for (sb = 0; sb < NB_SUBBANDS; sb++)
        ch->prev_q[sb] = q[sb];
}

/* ------------------------------------------------------------------ */
/* explicit codeword field plans                                       */
/*                                                                     */
/* aptX packs one channel into a 16-bit word, aptX HD into a 24-bit    */
/* word.  The highest subband field drops its least significant bit    */
/* when packing and recovers it from the parity of the remaining bits  */
/* when unpacking.                                                     */

struct field_plan { unsigned shift; unsigned bits; };

static const struct field_plan ref_plan[2][NB_SUBBANDS] = {
    { { 0, 7 }, { 7, 4 }, { 11, 2 }, { 13, 3 } }, /* aptX:    16-bit word */
    { { 0, 9 }, { 9, 6 }, { 15, 4 }, { 19, 5 } }, /* aptX HD: 24-bit word */
};

static uint32_t ref_pack(const struct field_plan *plan,
                         const int32_t q[NB_SUBBANDS], int32_t dither_parity)
{
    uint32_t word = 0;
    int64_t parity = ref_mod(dither_parity, 2);
    int sb;

    for (sb = 0; sb < NB_SUBBANDS; sb++)
        parity ^= ref_mod(q[sb], 2);

    for (sb = 0; sb < NB_SUBBANDS; sb++) {
        uint32_t field = (uint32_t)ref_mod(q[sb], (int64_t)1 << plan[sb].bits);
        if (sb == NB_SUBBANDS - 1)
            field = (field & ~UINT32_C(1)) | (uint32_t)parity;
        word |= field << plan[sb].shift;
    }
    return word;
}

static void ref_unpack(const struct field_plan *plan, uint32_t word,
                       int32_t dither_parity, int32_t q[NB_SUBBANDS])
{
    int64_t parity = ref_mod(dither_parity, 2);
    int sb;

    for (sb = 0; sb < NB_SUBBANDS; sb++) {
        uint32_t field = (uint32_t)((word >> plan[sb].shift)
                                    & ((UINT32_C(1) << plan[sb].bits) - 1));
        q[sb] = ref_sign_extend(field, plan[sb].bits);
        parity ^= ref_mod(q[sb], 2);
    }
    /* recover the highest subband LSB from the parity of the rest */
    q[NB_SUBBANDS-1] = q[NB_SUBBANDS-1] - (int32_t)ref_mod(q[NB_SUBBANDS-1], 2)
                     + (int32_t)parity;
}

/* ------------------------------------------------------------------ */
/* dither group                                                        */

static int32_t dither_seq_value(int chan, int beat, int subband)
{
    static const int lim[NB_SUBBANDS] = { 63, 7, 1, 3 };
    uint32_t r;
    int32_t v;

    switch (beat % 8) {
    case 0: return 0;
    case 1: return lim[subband];
    case 2: return -lim[subband] - 1;
    case 3: return (beat + subband + chan) & 1 ? lim[subband] : -lim[subband] - 1;
    case 4: return chan ? -1 : 1;
    case 5:
        r = (uint32_t)(beat * 2246822519u) ^ (uint32_t)(chan * 3266489917u)
          ^ (uint32_t)(subband * 668265263u);
        r ^= r >> 13; r *= 29943829u; r ^= r >> 16;
        return (int32_t)(r % (uint32_t)(2 * lim[subband] + 2)) - lim[subband] - 1;
    case 6:
        v = (subband & 1) ? -2 : 2;
        if (v > lim[subband]) v = lim[subband];
        if (v < -lim[subband] - 1) v = -lim[subband] - 1;
        return v;
    default:
        return (beat & 1) ? -lim[subband] : lim[subband];
    }
}

static int group_dither(void)
{
    enum { BEATS = 40, VECTORS = 128 };
    struct aptx_channel prod[NB_CHANNELS];
    struct ref_channel model[NB_CHANNELS];
    int32_t seq[NB_CHANNELS][BEATS][NB_SUBBANDS];
    int chan, beat, sb, hd, v;

    memset(prod, 0, sizeof(prod));
    for (chan = 0; chan < NB_CHANNELS; chan++)
        ref_channel_init(&model[chan]);

    /* left and right start from different histories, with high bits set
     * so the 32-bit history wraps while the sequence runs */
    prod[0].codeword_history = ref_s32(UINT32_C(0xF1234560));
    prod[1].codeword_history = ref_s32(UINT32_C(0x0ABCDEF4));
    model[0].history = UINT32_C(0xF1234560);
    model[1].history = UINT32_C(0x0ABCDEF4);

    /* independently maintained sequence of previous codewords: positive,
     * negative and zero codewords, field extremes, per-channel variation */
    for (chan = 0; chan < NB_CHANNELS; chan++)
        for (beat = 0; beat < BEATS; beat++)
            for (sb = 0; sb < NB_SUBBANDS; sb++)
                seq[chan][beat][sb] = dither_seq_value(chan, beat, sb);

    for (beat = 0; beat < BEATS; beat++) {
        for (chan = 0; chan < NB_CHANNELS; chan++) {
            for (sb = 0; sb < NB_SUBBANDS; sb++) {
                prod[chan].quantize[sb].quantized_sample = seq[chan][beat][sb];
                model[chan].prev_q[sb] = seq[chan][beat][sb];
            }
            aptx_generate_dither(&prod[chan]);
            ref_update_dither(&model[chan]);
            check_eq(prod[chan].codeword_history, ref_s32(model[chan].history),
                     "codeword history", chan, beat, -1);
            check_eq(prod[chan].dither_parity, model[chan].dither_parity,
                     "dither parity", chan, beat, -1);
            for (sb = 0; sb < NB_SUBBANDS; sb++)
                check_eq(prod[chan].dither[sb], model[chan].dither[sb],
                         "subband dither", chan, beat, sb);
        }
    }

    /* pack and unpack against the explicit field plan, both formats */
    for (hd = 0; hd < 2; hd++) {
        const struct field_plan *plan = ref_plan[hd];
        for (v = 0; v < VECTORS; v++) {
            struct aptx_channel pc;
            int32_t q[NB_SUBBANDS], qr[NB_SUBBANDS];
            int32_t dp = (v / 3) & 1;
            uint32_t word, refword;
            uint32_t r = (uint32_t)(v * 2654435761u) ^ (uint32_t)(hd * 40503u);

            memset(&pc, 0, sizeof(pc));
            for (sb = 0; sb < NB_SUBBANDS; sb++) {
                int32_t lim = ((int32_t)1 << (plan[sb].bits - 1)) - 1;
                r ^= r >> 13; r *= 2246822519u; r ^= r >> 16;
                q[sb] = (int32_t)(r % (uint32_t)(2 * lim + 1)) - lim;
                if (v % 4 == 3) /* out-of-range values exercise the field mask */
                    q[sb] += (sb & 1) ? lim + 3 : -(lim + 3);
                pc.quantize[sb].quantized_sample = q[sb];
            }
            pc.dither_parity = dp;

            word = hd ? aptxhd_pack_codeword(&pc) : aptx_pack_codeword(&pc);
            refword = ref_pack(plan, q, dp);
            check_eq(word, refword, "packed codeword", hd, v, -1);

            memset(&pc, 0, sizeof(pc));
            pc.dither_parity = dp;
            if (hd)
                aptxhd_unpack_codeword(&pc, word);
            else
                aptx_unpack_codeword(&pc, (uint16_t)word);
            ref_unpack(plan, word, dp, qr);
            for (sb = 0; sb < NB_SUBBANDS; sb++)
                check_eq(pc.quantize[sb].quantized_sample, qr[sb],
                         "unpacked codeword", hd, v, sb);
        }
    }

    /* sixteen consecutive beats of the sync parity cycle (two full turns) */
    {
        struct aptx_channel chans[NB_CHANNELS];
        uint8_t sync_idx = 0;

        memset(chans, 0, sizeof(chans));
        for (beat = 0; beat < 16; beat++) {
            int32_t parity[NB_CHANNELS];
            int32_t expected, got;

            for (chan = 0; chan < NB_CHANNELS; chan++) {
                uint32_t r = (uint32_t)(beat * 1103515245u)
                           ^ (uint32_t)(chan * 2654435761u) ^ 12345u;
                int32_t p;
                r ^= r >> 13; r *= 2246822519u; r ^= r >> 16;
                for (sb = 0; sb < NB_SUBBANDS; sb++)
                    chans[chan].quantize[sb].quantized_sample =
                        (int32_t)((r >> (3 * sb)) & 7) - 3;
                chans[chan].dither_parity = (int32_t)(r >> 13) & 1;
                p = chans[chan].dither_parity;
                for (sb = 0; sb < NB_SUBBANDS; sb++)
                    p ^= chans[chan].quantize[sb].quantized_sample;
                parity[chan] = p & 1;
            }

            expected = (parity[LEFT] ^ parity[RIGHT]) ^ (sync_idx == 7);
            got = aptx_check_parity(chans, &sync_idx);
            check_eq(got, expected, "sync parity", beat, -1, -1);
            check_eq(sync_idx, (beat + 1) & 7, "sync index", beat, -1, -1);
        }
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* predictor group                                                     */

static int32_t predictor_qs(int beat, int32_t lim, uint32_t *rs)
{
    switch ((beat / 8) % 6) {
    case 0: /* alternating signs */
        return (beat & 1) ? -((beat % 5) + 1) : (beat % 5) + 1;
    case 1: /* zeros */
        return 0;
    case 2: /* stable small, then sudden increase */
        return (beat % 16 < 12) ? 1 : lim;
    case 3: /* near the transmittable limit */
        return (beat & 1) ? lim : -lim;
    case 4: /* pseudo-random within range */
        *rs = *rs * 1664525u + 1013904223u;
        return (int32_t)(*rs % (uint32_t)(2 * lim + 1)) - lim;
    default:
        return (beat & 2) ? (2 * lim) / 3 : -(2 * lim) / 3;
    }
}

static int32_t predictor_dither(int beat, int sb, uint32_t *rs)
{
    uint32_t v;
    *rs = *rs * 1664525u + 1013904223u;
    if (beat % 16 == 0)
        return 0x7FFFFF - 131071 * sb;
    if (beat % 16 == 8)
        return -0x800000 + 131071 * sb;
    v = *rs & UINT32_C(0xFFFFFF);
    if (v & UINT32_C(0x800000))
        return (int32_t)((int64_t)v - INT64_C(0x1000000));
    return (int32_t)v;
}

static int group_predictor(void)
{
    static const int32_t qs_lim[2][NB_SUBBANDS] = {
        { 63, 7, 1, 3 },        /* aptX field ranges */
        { 255, 31, 7, 15 },     /* aptX HD field ranges */
    };
    int hd, sb, beat, i;

    for (hd = 0; hd < 2; hd++) {
        for (sb = 0; sb < NB_SUBBANDS; sb++) {
            const struct aptx_tables *t = &all_tables[hd][sb];
            const int order = t->prediction_order;
            /* longer than the prediction order, so the residual history
             * genuinely wraps more than once */
            const int beats = 2 * order + 48;
            struct aptx_invert_quantize inv;
            struct aptx_prediction pred;
            struct ref_subband m;
            uint32_t rs = UINT32_C(0x9E3779B9) ^ (uint32_t)(hd * 257 + sb * 17);

            memset(&inv, 0, sizeof(inv));
            memset(&pred, 0, sizeof(pred));
            pred.prev_sign[0] = 1;
            pred.prev_sign[1] = 1;
            ref_subband_init(&m);

            for (beat = 0; beat < beats; beat++) {
                int32_t qs = predictor_qs(beat, qs_lim[hd][sb], &rs);
                int32_t dith = predictor_dither(beat, sb, &rs);

                /* the current symbol must use the factor computed by the
                 * previous beat, not the one this beat is about to derive */
                check_eq(inv.quantization_factor, m.quantization_factor,
                         "factor used for current symbol", hd, sb, beat);

                ref_process_subband(&m, qs, dith, t);
                aptx_process_subband(&inv, &pred, qs, dith, t);

                check_eq(inv.reconstructed_difference, m.reconstructed_difference,
                         "reconstructed difference", hd, sb, beat);
                check_eq(inv.factor_select, m.factor_select,
                         "factor select", hd, sb, beat);
                check_eq(inv.quantization_factor, m.quantization_factor,
                         "next quantization factor", hd, sb, beat);
                check_eq(pred.prev_sign[0], m.prev_sign[0],
                         "previous sign 0", hd, sb, beat);
                check_eq(pred.prev_sign[1], m.prev_sign[1],
                         "previous sign 1", hd, sb, beat);
                check_eq(pred.s_weight[0], m.s_weight[0],
                         "pole weight 0", hd, sb, beat);
                check_eq(pred.s_weight[1], m.s_weight[1],
                         "pole weight 1", hd, sb, beat);
                for (i = 0; i < order; i++)
                    check_eq(pred.d_weight[i], m.d_weight[i],
                             "zero weight", hd * 8 + sb, beat, i);
                check_eq(pred.previous_reconstructed_sample,
                         m.previous_reconstructed_sample,
                         "reconstructed sample", hd, sb, beat);
                check_eq(pred.predicted_difference, m.predicted_difference,
                         "predicted difference", hd, sb, beat);
                check_eq(pred.predicted_sample, m.predicted_sample,
                         "predicted sample", hd, sb, beat);
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* stream group                                                        */

struct stream_inst {
    struct aptx_context *enc;
    struct aptx_context *dec;
    struct ref_channel model[NB_CHANNELS]; /* per-instance model history */
    int hd;
    int inst_id;
};

static int32_t stream_sample(int inst, int group, int sample, int chan)
{
    const int64_t idx = (int64_t)group * 4 + sample;
    uint32_t h = (uint32_t)(idx * UINT64_C(2654435761))
               ^ (uint32_t)(chan * 40503 + inst * 911);
    int32_t v;

    h ^= h >> 13; h *= UINT32_C(2246822519); h ^= h >> 16;

    switch ((idx / 240) % 6) {
    case 0: v = (int32_t)((int)(idx % 160) - 80) * 40000; break; /* ramp */
    case 1: v = (idx & 1) ? 3500000 : -3500000; break;           /* alternating */
    case 2:                                                     /* 24-bit noise */
        v = (int32_t)(h & UINT32_C(0xFFFFFF));
        if (v & 0x800000)
            v = (int32_t)((int64_t)v - INT64_C(0x1000000));
        break;
    case 3: v = 0; break;                                        /* silence */
    case 4: v = (idx & 2) ? 8388607 : -8388608; break;           /* near clip */
    default: v = (idx % 64 < 56) ? 1200 : 5200000; break;        /* small, then sudden */
    }
    if (chan)
        v = -v / 2 + (int32_t)(h & 63);
    return v;
}

static void stream_compare(const struct ref_channel *m,
                           const struct aptx_channel *p,
                           const int32_t q[NB_SUBBANDS],
                           int hd, long tag, long beat, long chan)
{
    int sb, i;

    check_eq(p->codeword_history, ref_s32(m->history),
             "stream codeword history", tag, beat, chan);
    check_eq(p->dither_parity, m->dither_parity,
             "stream dither parity", tag, beat, chan);
    for (sb = 0; sb < NB_SUBBANDS; sb++) {
        const struct ref_subband *ms = &m->sb[sb];
        const struct aptx_prediction *pp = &p->prediction[sb];
        const int order = all_tables[hd][sb].prediction_order;
        long slot = chan * 8 + sb;

        check_eq(p->dither[sb], m->dither[sb],
                 "stream dither", tag, beat, slot);
        check_eq(p->quantize[sb].quantized_sample, q[sb],
                 "stream quantized sample", tag, beat, slot);
        check_eq(p->invert_quantize[sb].quantization_factor, ms->quantization_factor,
                 "stream quantization factor", tag, beat, slot);
        check_eq(p->invert_quantize[sb].factor_select, ms->factor_select,
                 "stream factor select", tag, beat, slot);
        check_eq(p->invert_quantize[sb].reconstructed_difference,
                 ms->reconstructed_difference,
                 "stream reconstructed difference", tag, beat, slot);
        check_eq(pp->prev_sign[0], ms->prev_sign[0],
                 "stream previous sign 0", tag, beat, slot);
        check_eq(pp->prev_sign[1], ms->prev_sign[1],
                 "stream previous sign 1", tag, beat, slot);
        check_eq(pp->s_weight[0], ms->s_weight[0],
                 "stream pole weight 0", tag, beat, slot);
        check_eq(pp->s_weight[1], ms->s_weight[1],
                 "stream pole weight 1", tag, beat, slot);
        for (i = 0; i < order; i++)
            check_eq(pp->d_weight[i], ms->d_weight[i],
                     "stream zero weight", tag * 100 + beat, slot, i);
        check_eq(pp->previous_reconstructed_sample, ms->previous_reconstructed_sample,
                 "stream reconstructed sample", tag, beat, slot);
        check_eq(pp->predicted_difference, ms->predicted_difference,
                 "stream predicted difference", tag, beat, slot);
        check_eq(pp->predicted_sample, ms->predicted_sample,
                 "stream predicted sample", tag, beat, slot);
    }
}

/* advance the model by one beat using the codewords parsed from the bytes
 * actually emitted on the wire */
static void stream_model_beat(struct stream_inst *inst,
                              const unsigned char *code,
                              int32_t q[NB_CHANNELS][NB_SUBBANDS])
{
    int c;

    for (c = 0; c < NB_CHANNELS; c++)
        ref_update_dither(&inst->model[c]);
    for (c = 0; c < NB_CHANNELS; c++) {
        uint32_t word;
        if (inst->hd)
            word = ((uint32_t)code[3*c+0] << 16)
                 | ((uint32_t)code[3*c+1] <<  8)
                 | ((uint32_t)code[3*c+2] <<  0);
        else
            word = ((uint32_t)code[2*c+0] << 8)
                 | ((uint32_t)code[2*c+1] << 0);
        ref_unpack(ref_plan[inst->hd], word, inst->model[c].dither_parity, q[c]);
    }
    for (c = 0; c < NB_CHANNELS; c++)
        ref_process_channel(&inst->model[c], inst->hd, q[c]);
}

static void stream_run_beat(struct stream_inst *inst, int group_idx,
                            long beat_tag, long expect_dec_bytes)
{
    const int ss = inst->hd ? 6 : 4;
    unsigned char pcm[24], code[6], pout[24];
    int32_t q[NB_CHANNELS][NB_SUBBANDS];
    size_t written = 0, dwritten = 0;
    size_t consumed, dconsumed;
    int c, s;

    for (s = 0; s < 4; s++) {
        for (c = 0; c < NB_CHANNELS; c++) {
            int32_t v = stream_sample(inst->inst_id, group_idx, s, c);
            unsigned char *p = &pcm[(s * NB_CHANNELS + c) * 3];
            p[0] = (unsigned char)((uint32_t)v & 0xFF);
            p[1] = (unsigned char)(((uint32_t)v >> 8) & 0xFF);
            p[2] = (unsigned char)(((uint32_t)v >> 16) & 0xFF);
        }
    }

    consumed = aptx_encode(inst->enc, pcm, sizeof(pcm),
                           code, sizeof(code), &written);
    check_eq((int64_t)consumed, 24, "encode consumed", inst->inst_id, beat_tag, -1);
    check_eq((int64_t)written, ss, "encode written", inst->inst_id, beat_tag, -1);

    stream_model_beat(inst, code, q);
    for (c = 0; c < NB_CHANNELS; c++)
        stream_compare(&inst->model[c], &inst->enc->channels[c], q[c],
                       inst->hd, inst->inst_id * 2, beat_tag, c);

    dconsumed = aptx_decode(inst->dec, code, (size_t)ss,
                            pout, sizeof(pout), &dwritten);
    check_eq((int64_t)dconsumed, ss, "decode consumed", inst->inst_id, beat_tag, -1);
    check_eq((int64_t)dwritten, expect_dec_bytes,
             "decode written", inst->inst_id, beat_tag, -1);
    for (c = 0; c < NB_CHANNELS; c++)
        stream_compare(&inst->model[c], &inst->dec->channels[c], q[c],
                       inst->hd, inst->inst_id * 2 + 1, beat_tag, c);
}

static void stream_reset_model(struct stream_inst *inst)
{
    int c;
    for (c = 0; c < NB_CHANNELS; c++)
        ref_channel_init(&inst->model[c]);
}

static int group_stream(void)
{
    enum { BEATS = 360, REUSE_BEATS = 16 };
    struct stream_inst insts[2];
    int64_t total_dec[2] = { 0, 0 };
    int64_t total_enc[2] = { 0, 0 };
    int i, b, c, k;

    /* two interleaved instances, one aptX and one aptX HD, each with its
     * own model history */
    for (i = 0; i < 2; i++) {
        insts[i].hd = i;
        insts[i].inst_id = i;
        insts[i].enc = aptx_init(i);
        insts[i].dec = aptx_init(i);
        TASK_CHECK(insts[i].enc != NULL && insts[i].dec != NULL, "aptx_init");
        stream_reset_model(&insts[i]);
    }

    for (b = 0; b < BEATS; b++) {
        for (i = 0; i < 2; i++) {
            /* 90-sample latency: output starts on the 23rd codeword with
             * the trailing 2 samples of that group */
            long expect = b < 22 ? 0 : (b == 22 ? 12 : 24);
            stream_run_beat(&insts[i], b, b, expect);
            total_dec[i] += expect;
            total_enc[i] += insts[i].hd ? 6 : 4;
        }
    }
    for (i = 0; i < 2; i++)
        check_eq(total_dec[i], 12 + (int64_t)(BEATS - 23) * 24,
                 "90 sample latency total", i, -1, -1);

    /* finish with deliberately short output buffers, then continuation
     * calls until the encoder drains and resets itself */
    for (i = 0; i < 2; i++) {
        struct stream_inst *inst = &insts[i];
        const int ss = inst->hd ? 6 : 4;
        unsigned char fbuf[23 * 6];
        unsigned char pout[24];
        size_t fw = 0, off = 0;
        size_t dc, dw;
        int ok;

        ok = aptx_encode_finish(inst->enc, fbuf, (size_t)(2 * ss), &fw);
        check_eq(ok, 0, "finish short output 1", i, 0, -1);
        check_eq((int64_t)fw, 2 * ss, "finish short written 1", i, 0, -1);
        off += fw;

        /* the encoder is mid-stream after the short call: decode the
         * emitted codewords in lockstep so the model, the encoder and
         * the decoder all stand after the same codeword */
        for (k = 0; k < 2; k++) {
            int32_t q[NB_CHANNELS][NB_SUBBANDS];
            stream_model_beat(inst, fbuf + (size_t)k * ss, q);
            dw = 0;
            dc = aptx_decode(inst->dec, fbuf + (size_t)k * ss, (size_t)ss,
                             pout, sizeof(pout), &dw);
            check_eq((int64_t)dc, ss, "finish decode consumed", i, k, -1);
            check_eq((int64_t)dw, 24, "finish decode written", i, k, -1);
            for (c = 0; c < NB_CHANNELS; c++)
                stream_compare(&inst->model[c], &inst->dec->channels[c], q[c],
                               inst->hd, inst->inst_id * 2 + 1, 1000 + k, c);
        }
        for (c = 0; c < NB_CHANNELS; c++)
            stream_compare(&inst->model[c], &inst->enc->channels[c],
                           inst->model[c].prev_q,
                           inst->hd, inst->inst_id * 2, 1001, c);

        ok = aptx_encode_finish(inst->enc, fbuf + off, (size_t)(3 * ss), &fw);
        check_eq(ok, 0, "finish short output 2", i, 1, -1);
        check_eq((int64_t)fw, 3 * ss, "finish short written 2", i, 1, -1);
        off += fw;

        for (k = 2; k < 5; k++) {
            int32_t q[NB_CHANNELS][NB_SUBBANDS];
            stream_model_beat(inst, fbuf + (size_t)k * ss, q);
            dw = 0;
            dc = aptx_decode(inst->dec, fbuf + (size_t)k * ss, (size_t)ss,
                             pout, sizeof(pout), &dw);
            check_eq((int64_t)dc, ss, "finish decode consumed", i, k, -1);
            check_eq((int64_t)dw, 24, "finish decode written", i, k, -1);
            for (c = 0; c < NB_CHANNELS; c++)
                stream_compare(&inst->model[c], &inst->dec->channels[c], q[c],
                               inst->hd, inst->inst_id * 2 + 1, 1000 + k, c);
        }
        for (c = 0; c < NB_CHANNELS; c++)
            stream_compare(&inst->model[c], &inst->enc->channels[c],
                           inst->model[c].prev_q,
                           inst->hd, inst->inst_id * 2, 1004, c);

        ok = aptx_encode_finish(inst->enc, fbuf + off, sizeof(fbuf) - off, &fw);
        check_eq(ok, 1, "finish completion", i, 2, -1);
        check_eq((int64_t)fw, 18 * ss, "finish final written", i, 2, -1);
        off += fw;
        check_eq((int64_t)off, 23 * ss, "finish total written", i, -1, -1);
        total_enc[i] += 23 * ss;

        /* a completed finish resets the encoder for reuse */
        check_eq(inst->enc->sync_idx, 0, "finish resets sync index", i, -1, -1);
        check_eq(inst->enc->encode_remaining, 23, "finish resets latency budget", i, -1, -1);
        check_eq(inst->enc->channels[0].invert_quantize[0].quantization_factor, 0,
                 "finish resets quantization factor", i, -1, -1);
        check_eq(inst->enc->channels[0].prediction[0].prev_sign[0], 1,
                 "finish resets predictor signs", i, -1, -1);

        /* decode the rest of the latency tail codeword by codeword; the
         * model follows the parsed wire codewords, not a re-encode */
        for (k = 5; k < 23; k++) {
            int32_t q[NB_CHANNELS][NB_SUBBANDS];
            stream_model_beat(inst, fbuf + (size_t)k * ss, q);
            dw = 0;
            dc = aptx_decode(inst->dec, fbuf + (size_t)k * ss, (size_t)ss,
                             pout, sizeof(pout), &dw);
            check_eq((int64_t)dc, ss, "finish decode consumed", i, k, -1);
            check_eq((int64_t)dw, 24, "finish decode written", i, k, -1);
            for (c = 0; c < NB_CHANNELS; c++)
                stream_compare(&inst->model[c], &inst->dec->channels[c], q[c],
                               inst->hd, inst->inst_id * 2 + 1, 1000 + k, c);
        }
        total_dec[i] += 23 * 24;
    }
    for (i = 0; i < 2; i++) {
        check_eq(total_enc[i], (int64_t)(BEATS + 23) * (insts[i].hd ? 6 : 4),
                 "total encoded bytes", i, -1, -1);
        check_eq(total_dec[i], (int64_t)(BEATS + 23) * 24 - 540,
                 "total decoded bytes", i, -1, -1);
    }

    /* reuse after reset: both endpoints and the model start over */
    for (i = 0; i < 2; i++) {
        aptx_reset(insts[i].enc);
        aptx_reset(insts[i].dec);
        stream_reset_model(&insts[i]);
    }
    for (b = 0; b < REUSE_BEATS; b++)
        for (i = 0; i < 2; i++)
            stream_run_beat(&insts[i], 2000 + b, 2000 + b, 0);

    for (i = 0; i < 2; i++) {
        aptx_finish(insts[i].enc);
        aptx_finish(insts[i].dec);
    }
    return 1;
}

/* ------------------------------------------------------------------ */

int task_group(const char *group)
{
    if (strcmp(group, "dither") == 0)
        return group_dither();
    if (strcmp(group, "predictor") == 0)
        return group_predictor();
    if (strcmp(group, "stream") == 0)
        return group_stream();
    return 0;
}
