/*
 * Adaptive-contract regression tests for libfreeaptx.
 *
 * These tests never compare the library against itself.  Every expected value
 * is produced by the independent reference model below, expressed with wide
 * integers, explicit quotient/remainder helpers and explicit two's-complement
 * truncation.  The reference never calls the production rounding, clipping,
 * dither, inverse-quantization or prediction helpers, and it stores residual
 * history in plain chronological order rather than in the production ring
 * buffer.  Only the format constant tables are shared.
 *
 * Groups:
 *   dither     codeword field plans, 32-bit codeword history, four dither
 *              subbands and parity for aptX and aptX HD, including negative
 *              codewords, 32-bit history wrap, eight-beat parity sync and
 *              independent left/right histories.
 *   predictor  inverse quantization, factor_select, the next quantization
 *              factor, both pole weights, zero weights, reconstructed samples
 *              and predictions on every beat, against a chronologically
 *              stored residual history.
 *   stream     the public aptx_encode()/aptx_decode() API on sustained 24-bit
 *              stereo audio.  The model is fed the codewords that were really
 *              emitted (never a re-encoding) and compared beat by beat with
 *              the observable encoder/decoder state.
 *
 * freeaptx.c is included so the stream group can observe static pipeline
 * state and the constant tables.  Nothing is added to a normal library build.
 *
 * TASK_CHECK reports each actual comparison.  Each group returns 1 only after
 * every comparison in it passed.  task_support/main.c provides main().
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freeaptx.h"
#include "freeaptx.c"
#include "check.h"

/* ------------------------------------------------------------------ */
/* Reference primitives                                                */
/* ------------------------------------------------------------------ */

/* Explicit two's-complement truncation to 32 bits. */
static int32_t ref_wrap32(int64_t value)
{
    uint32_t low = (uint32_t)((uint64_t)value & 0xFFFFFFFFu);
    int64_t signed_low = (int64_t)low;
    if (low & 0x80000000u)
        signed_low -= 4294967296LL;
    return (int32_t)signed_low;
}

/* Floor division. */
static int64_t ref_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0)))
        --q;
    return q;
}

/* Round-half-to-even right shift, spelled with quotient and remainder. */
static int64_t ref_round_shift(int64_t value, unsigned shift)
{
    int64_t divisor = (int64_t)1 << shift;
    int64_t q = ref_floor_div(value, divisor);
    int64_t r = value - q * divisor;
    int64_t twice = 2 * r;

    if (twice > divisor)
        ++q;
    else if (twice == divisor && (q & 1))
        ++q;
    return q;
}

static int64_t ref_floor_shift(int64_t value, unsigned shift)
{
    return ref_floor_div(value, (int64_t)1 << shift);
}

/* Clip into the symmetric 24-bit style range, after 32-bit truncation. */
static int32_t ref_clip_symmetric(int64_t value, unsigned bits)
{
    int32_t wrapped = ref_wrap32(value);
    int64_t limit = (int64_t)1 << bits;

    if (wrapped < -limit)
        return (int32_t)(-limit);
    if (wrapped > limit - 1)
        return (int32_t)(limit - 1);
    return wrapped;
}

static int32_t ref_clip_range(int64_t value, int64_t low, int64_t high)
{
    if (value < low)
        return (int32_t)low;
    if (value > high)
        return (int32_t)high;
    return (int32_t)value;
}

static int32_t ref_sign_extend(uint32_t value, unsigned bits)
{
    unsigned shift = 32 - bits;
    int32_t shifted = (int32_t)(value << shift);
    return shifted >> shift;
}

static int32_t ref_sign(int64_t value)
{
    if (value > 0)
        return 1;
    if (value < 0)
        return -1;
    return 0;
}

/* Deterministic 32-bit LCG, independent of library state. */
struct ref_rng {
    uint32_t state;
};

static uint32_t ref_rng_next(struct ref_rng *rng)
{
    rng->state = rng->state * 1103515245u + 12345u;
    return rng->state;
}

/* Uniform value in [low, high], rejection sampled so there is no bias. */
static int32_t ref_rng_range(struct ref_rng *rng, int32_t low, int32_t high)
{
    uint32_t span = (uint32_t)((int64_t)high - (int64_t)low + 1);
    uint32_t limit = 0xFFFFFFFFu - (0xFFFFFFFFu % span);
    uint32_t value;

    do {
        value = ref_rng_next(rng);
    } while (value >= limit);
    return (int32_t)((int64_t)low + (int64_t)(value % span));
}

/* ------------------------------------------------------------------ */
/* Reference dither / codeword model                                   */
/* ------------------------------------------------------------------ */

#define REF_SUBBANDS 4
#define REF_LEFT 0
#define REF_RIGHT 1

enum ref_format {
    REF_APTX = 0,
    REF_APTX_HD = 1
};

struct ref_field {
    unsigned shift;
    unsigned width;
};

/* Field plans written out independently of the production packers. */
static const struct ref_field ref_fields_aptx[REF_SUBBANDS] = {
    {  0, 7 },   /* subband 0: 7-bit two's-complement */
    {  7, 4 },   /* subband 1: 4-bit */
    { 11, 2 },   /* subband 2: 2-bit */
    { 13, 3 },   /* subband 3: 3-bit, low bit carries parity */
};

static const struct ref_field ref_fields_hd[REF_SUBBANDS] = {
    {  0, 9 },
    {  9, 6 },
    { 15, 4 },
    { 19, 5 },
};

static const struct ref_field *ref_fields(enum ref_format fmt)
{
    return fmt == REF_APTX_HD ? ref_fields_hd : ref_fields_aptx;
}

static unsigned ref_codeword_bits(enum ref_format fmt)
{
    const struct ref_field *fields = ref_fields(fmt);
    return fields[3].shift + fields[3].width;
}

struct ref_dither_channel {
    uint32_t history;         /* 32-bit codeword history, wraps naturally */
    int32_t subband[REF_SUBBANDS];
    int32_t parity;
};

static void ref_dither_reset(struct ref_dither_channel *channel)
{
    memset(channel, 0, sizeof(*channel));
}

/*
 * Advance the codeword history from the previous beat's final quantized
 * samples, then derive the four dither perturbations and the dither parity:
 *
 *   cw      = (q0 & 3) | ((q1 & 2) << 1) | ((q2 & 1) << 3)
 *   history = (cw << 8) + (history << 4)              [32-bit wrap]
 *   m       = 5184443 * floor(history_signed / 2^7)
 *   d       = wrap32(m * 4 + floor(m / 2^22))
 *   dither  = wrap32(d << (23 - 5k))
 *   parity  = (d >> 25) & 1
 */
static void ref_dither_advance(struct ref_dither_channel *channel,
                               const int32_t quantized[REF_SUBBANDS])
{
    uint32_t cw = (uint32_t)((quantized[0] & 3) << 0)
                + (uint32_t)((quantized[1] & 2) << 1)
                + (uint32_t)((quantized[2] & 1) << 3);
    int32_t signed_history;
    int64_t m;
    int32_t d;
    int subband;

    channel->history = (cw << 8) + (channel->history << 4);
    signed_history = ref_wrap32((int64_t)channel->history);
    m = 5184443LL * ref_floor_div(signed_history, 128);
    d = ref_wrap32(m * 4 + ref_floor_div(m, 4194304LL));
    for (subband = 0; subband < REF_SUBBANDS; subband++)
        channel->subband[subband] =
            ref_wrap32((int64_t)((uint32_t)d << (23 - 5 * subband)));
    channel->parity = (int32_t)(((uint32_t)d >> 25) & 1u);
}

/*
 * Independent packing following the explicit field plan.  The parity bit is
 * the XOR of the dither parity and the low bit of all four quantized samples;
 * it replaces the least significant bit of the highest subband.
 */
static uint32_t ref_pack_codeword(enum ref_format fmt,
                                  const int32_t quantized[REF_SUBBANDS],
                                  int32_t dither_parity)
{
    const struct ref_field *fields = ref_fields(fmt);
    uint32_t parity = (uint32_t)(dither_parity & 1);
    uint32_t codeword = 0;
    int subband;

    for (subband = 0; subband < REF_SUBBANDS; subband++) {
        uint32_t width_mask = ((uint32_t)1 << fields[subband].width) - 1;
        parity ^= (uint32_t)(quantized[subband] & 1);
        codeword |= ((uint32_t)quantized[subband] & width_mask)
                    << fields[subband].shift;
    }

    codeword &= ~((uint32_t)1 << fields[3].shift);
    codeword |= (parity & 1u) << fields[3].shift;
    return codeword;
}

/*
 * Sign-extend every field, then restore the highest subband's low bit from the
 * parity equation.  The dither parity must be the one generated for this beat.
 */
static void ref_unpack_codeword(enum ref_format fmt, uint32_t codeword,
                                int32_t dither_parity,
                                int32_t quantized[REF_SUBBANDS])
{
    const struct ref_field *fields = ref_fields(fmt);
    int32_t parity = dither_parity & 1;
    int subband;

    for (subband = 0; subband < REF_SUBBANDS; subband++)
        quantized[subband] = ref_sign_extend(codeword >> fields[subband].shift,
                                             fields[subband].width);

    for (subband = 0; subband < REF_SUBBANDS; subband++)
        parity ^= quantized[subband] & 1;
    quantized[3] = (quantized[3] & ~1) | (parity & 1);
}

/* Big-endian byte plan, independent of production output ordering. */
static void ref_store_codeword(enum ref_format fmt, uint32_t codeword,
                               unsigned char *output)
{
    unsigned bytes = ref_codeword_bits(fmt) / 8;
    unsigned i;

    for (i = 0; i < bytes; i++)
        output[i] = (unsigned char)((codeword >> (8 * (bytes - 1 - i))) & 0xFF);
}

static uint32_t ref_load_codeword(enum ref_format fmt,
                                  const unsigned char *input)
{
    unsigned bytes = ref_codeword_bits(fmt) / 8;
    uint32_t codeword = 0;
    unsigned i;

    for (i = 0; i < bytes; i++)
        codeword = (codeword << 8) | input[i];
    return codeword;
}

/* ------------------------------------------------------------------ */
/* Reference predictor model                                           */
/* ------------------------------------------------------------------ */

#define REF_HISTORY_MAX 25   /* longest order (LF = 24) plus one */

struct ref_predictor {
    int32_t quantization_factor;
    int32_t factor_select;
    int32_t pole_weight[2];
    int32_t zero_weight[REF_HISTORY_MAX];
    int32_t prev_sign[2];
    int32_t previous_reconstructed;
    int32_t predicted_difference;
    int32_t predicted_sample;
    /* Residual history in plain chronological order: [0] is newest. */
    int32_t residual[REF_HISTORY_MAX];
};

static void ref_predictor_reset(struct ref_predictor *pred)
{
    memset(pred, 0, sizeof(*pred));
    pred->prev_sign[0] = 1;
    pred->prev_sign[1] = 1;
}

struct ref_subband_result {
    int32_t reconstructed_difference;
    int32_t reconstructed_sample;
};

/*
 * One beat of inverse quantization and adaptive prediction.
 *
 * The reconstructed difference is computed with the quantization factor that
 * was in force at the start of the beat; the updated factor and factor_select
 * belong to the next beat.  Every rounding step and the 24-bit saturation are
 * kept separate.
 */
static void ref_predictor_step(struct ref_predictor *pred,
                               int32_t quantized_sample,
                               int32_t dither,
                               const struct aptx_tables *tables,
                               struct ref_subband_result *out)
{
    int order = tables->prediction_order;
    int32_t index = (quantized_sample ^ -(quantized_sample < 0)) + 1;
    int32_t half_interval = tables->quantize_intervals[index] / 2;
    int32_t qr;
    int32_t recon_diff;
    int32_t factor_select_product;
    int32_t factor_select_sum;
    int32_t factor_select_rounded;
    int32_t factor_index;
    int32_t factor_shift;
    int32_t sign;
    int32_t same_sign0;
    int32_t same_sign1;
    int32_t sw1;
    int32_t range;
    int32_t weight0;
    int32_t weight1;
    int32_t reconstructed_sample;
    int32_t predictor_term;
    int32_t srd0;
    int64_t predicted_difference;
    int i;

    /* --- inverse quantization, using the old quantization factor --- */
    if (quantized_sample < 0)
        half_interval = -half_interval;

    qr = ref_clip_symmetric(
            ref_round_shift((int64_t)half_interval * ((int64_t)1 << 32)
                            + (int64_t)dither
                              * (int64_t)tables->invert_quantize_dither_factors[index],
                            32),
            23);
    recon_diff = ref_wrap32(ref_floor_div((int64_t)pred->quantization_factor
                                          * (int64_t)qr, 1 << 19));

    /* --- factor_select update --- */
    factor_select_product = ref_wrap32(32620LL * (int64_t)pred->factor_select);
    factor_select_sum = ref_wrap32((int64_t)factor_select_product
                                   + (int64_t)tables->quantize_factor_select_offset[index]
                                     * 32768LL);
    factor_select_rounded = ref_wrap32(ref_round_shift(factor_select_sum, 15));
    pred->factor_select = ref_clip_range(factor_select_rounded, 0,
                                         tables->factor_max);

    /* --- next beat's quantization factor --- */
    factor_index = (pred->factor_select & 0xFF) >> 3;
    factor_shift = (tables->factor_max - pred->factor_select) >> 8;
    pred->quantization_factor =
        ref_wrap32(((int64_t)quantization_factors[factor_index] << 11)
                   >> factor_shift);

    /* --- pole-section sign adaptation --- */
    sign = ref_sign((int64_t)recon_diff + (int64_t)pred->predicted_difference);
    same_sign0 = sign * pred->prev_sign[0];
    same_sign1 = sign * pred->prev_sign[1];
    pred->prev_sign[0] = pred->prev_sign[1];
    pred->prev_sign[1] = sign | 1;

    range = 0x100000;
    sw1 = ref_wrap32(ref_round_shift(
            ref_wrap32(-(int64_t)same_sign1 * (int64_t)pred->pole_weight[1]), 1));
    sw1 = (ref_clip_range(sw1, -range, range) & ~0xF) * 16;

    range = 0x300000;
    weight0 = ref_wrap32((int64_t)254 * (int64_t)pred->pole_weight[0]
                         + (int64_t)0x800000 * (int64_t)same_sign0
                         + (int64_t)sw1);
    pred->pole_weight[0] = ref_clip_range(ref_wrap32(ref_round_shift(weight0, 8)),
                                          -range, range);

    range = 0x3C0000 - pred->pole_weight[0];
    weight1 = ref_wrap32((int64_t)255 * (int64_t)pred->pole_weight[1]
                         + (int64_t)0xC00000 * (int64_t)same_sign1);
    pred->pole_weight[1] = ref_clip_range(ref_wrap32(ref_round_shift(weight1, 8)),
                                          -range, range);

    /* --- reconstructed sample and pole predictor, using the new weights --- */
    reconstructed_sample = ref_clip_symmetric((int64_t)recon_diff
                                              + (int64_t)pred->predicted_sample, 23);
    predictor_term = ref_clip_symmetric(ref_floor_shift(
            (int64_t)pred->pole_weight[0] * (int64_t)pred->previous_reconstructed
            + (int64_t)pred->pole_weight[1] * (int64_t)reconstructed_sample,
            22), 23);
    pred->previous_reconstructed = reconstructed_sample;

    /* --- chronological residual history, newest first --- */
    for (i = order; i > 0; i--)
        pred->residual[i] = pred->residual[i - 1];
    pred->residual[0] = recon_diff;

    /* --- zero-section weights and filtered difference --- */
    srd0 = ref_sign(recon_diff) * (1 << 23);
    predicted_difference = 0;
    for (i = 0; i < order; i++) {
        int32_t old_weight = pred->zero_weight[i];
        int32_t srd = (pred->residual[i + 1] < 0) ? -1 : 1;
        int32_t updated = ref_wrap32(
                (int64_t)old_weight
                - ref_round_shift((int64_t)old_weight
                                  - (int64_t)srd * (int64_t)srd0, 8));
        pred->zero_weight[i] = updated;
        predicted_difference += (int64_t)pred->residual[i] * (int64_t)updated;
    }

    pred->predicted_difference =
        ref_clip_symmetric(ref_floor_shift(predicted_difference, 22), 23);
    pred->predicted_sample =
        ref_clip_symmetric((int64_t)predictor_term
                           + (int64_t)pred->predicted_difference, 23);

    if (out) {
        out->reconstructed_difference = recon_diff;
        out->reconstructed_sample = reconstructed_sample;
    }
}

/* A reference channel: independent dither + four independent predictors. */
struct ref_stream_channel {
    struct ref_dither_channel dither;
    struct ref_predictor subband[REF_SUBBANDS];
    int32_t last_quantized[REF_SUBBANDS];
};

static void ref_stream_channel_reset(struct ref_stream_channel *channel)
{
    int subband;

    ref_dither_reset(&channel->dither);
    for (subband = 0; subband < REF_SUBBANDS; subband++)
        ref_predictor_reset(&channel->subband[subband]);
    memset(channel->last_quantized, 0, sizeof(channel->last_quantized));
}

/*
 * Advance a reference channel by one beat from an emitted codeword.  The
 * dither for this beat is derived from the previous beat's final samples, then
 * the codeword is unpacked with the new parity, then prediction runs.
 */
static void ref_stream_step(struct ref_stream_channel *channel,
                            enum ref_format fmt,
                            uint32_t codeword)
{
    int32_t quantized[REF_SUBBANDS];
    int subband;

    ref_dither_advance(&channel->dither, channel->last_quantized);
    ref_unpack_codeword(fmt, codeword, channel->dither.parity, quantized);
    for (subband = 0; subband < REF_SUBBANDS; subband++)
        ref_predictor_step(&channel->subband[subband], quantized[subband],
                           channel->dither.subband[subband],
                           &all_tables[fmt][subband], NULL);
    for (subband = 0; subband < REF_SUBBANDS; subband++)
        channel->last_quantized[subband] = quantized[subband];
}

/* ------------------------------------------------------------------ */
/* Group 1: dither and codeword contract                               */
/* ------------------------------------------------------------------ */

static void dither_check_channel(const char *tag, int hd,
                                 const int32_t sequence[][REF_SUBBANDS],
                                 int count)
{
    enum ref_format fmt = hd ? REF_APTX_HD : REF_APTX;
    struct ref_dither_channel model;
    struct aptx_channel production;
    int beat;

    ref_dither_reset(&model);
    memset(&production, 0, sizeof(production));

    for (beat = 0; beat < count; beat++) {
        const int32_t *q = sequence[beat];
        uint32_t expected_codeword;
        uint32_t production_codeword;
        int32_t expected_unpacked[REF_SUBBANDS];
        int32_t library_unpacked[REF_SUBBANDS];
        struct aptx_channel unpacker;
        char message[192];
        int subband;

        /* Drive the production dither generator from the same samples. */
        for (subband = 0; subband < REF_SUBBANDS; subband++)
            production.quantize[subband].quantized_sample = q[subband];
        aptx_generate_dither(&production);

        ref_dither_advance(&model, q);

        TASK_CHECK((uint32_t)production.codeword_history == model.history,
                   "dither: 32-bit codeword history differs from model");
        TASK_CHECK(production.dither_parity == model.parity,
                   "dither: parity differs from model");
        for (subband = 0; subband < REF_SUBBANDS; subband++) {
            snprintf(message, sizeof(message),
                     "%s beat %d subband %d dither expected %d got %d",
                     tag, beat, subband, model.subband[subband],
                     production.dither[subband]);
            TASK_CHECK(production.dither[subband] == model.subband[subband],
                       message);
        }

        expected_codeword = ref_pack_codeword(fmt, q, model.parity);
        production_codeword = hd ? aptxhd_pack_codeword(&production)
                                 : aptx_pack_codeword(&production);
        snprintf(message, sizeof(message),
                 "%s beat %d codeword expected %06x got %06x",
                 tag, beat, (unsigned)expected_codeword,
                 (unsigned)production_codeword);
        TASK_CHECK(production_codeword == expected_codeword, message);

        /* Big-endian byte plan must round-trip independently. */
        {
            unsigned char bytes[3];
            ref_store_codeword(fmt, expected_codeword, bytes);
            TASK_CHECK(ref_load_codeword(fmt, bytes) == expected_codeword,
                       "dither: big-endian byte plan did not round-trip");
        }

        /* Unpack both ways using the dither parity generated for this beat. */
        ref_unpack_codeword(fmt, production_codeword, model.parity,
                            expected_unpacked);
        memset(&unpacker, 0, sizeof(unpacker));
        unpacker.dither_parity = model.parity;
        if (hd)
            aptxhd_unpack_codeword(&unpacker, production_codeword);
        else
            aptx_unpack_codeword(&unpacker, (uint16_t)production_codeword);
        for (subband = 0; subband < REF_SUBBANDS; subband++)
            library_unpacked[subband] =
                unpacker.quantize[subband].quantized_sample;

        for (subband = 0; subband < REF_SUBBANDS; subband++) {
            snprintf(message, sizeof(message),
                     "%s beat %d subband %d unpack expected %d got %d",
                     tag, beat, subband, expected_unpacked[subband],
                     library_unpacked[subband]);
            TASK_CHECK(library_unpacked[subband] == expected_unpacked[subband],
                       message);
        }

        /* Packing replaced the high band's low bit with the parity bit. */
        {
            unsigned high_shift = ref_fields(fmt)[3].shift;
            uint32_t packed_parity = (production_codeword >> high_shift) & 1u;
            uint32_t expected_packed_parity =
                (uint32_t)(model.parity ^ (q[0] & 1) ^ (q[1] & 1)
                           ^ (q[2] & 1) ^ (q[3] & 1));
            snprintf(message, sizeof(message),
                     "%s beat %d packed parity expected %u got %u",
                     tag, beat, (unsigned)(expected_packed_parity & 1u),
                     (unsigned)packed_parity);
            TASK_CHECK(packed_parity == (expected_packed_parity & 1u), message);

            /* And unpacking restored the original high-band low bit. */
            snprintf(message, sizeof(message),
                     "%s beat %d high band low bit expected %d got %d",
                     tag, beat, q[3] & 1, expected_unpacked[3] & 1);
            TASK_CHECK((expected_unpacked[3] & 1) == (q[3] & 1), message);
        }
    }
}

static void run_dither_group(void)
{
    enum { DITHER_BEATS = 72 };
    static const int32_t widths_aptx[REF_SUBBANDS] = { 7, 4, 2, 3 };
    static const int32_t widths_hd[REF_SUBBANDS] = { 9, 6, 4, 5 };
    struct ref_rng rng;
    int32_t sequence[DITHER_BEATS][REF_SUBBANDS];
    int hd, beat, subband;

    for (hd = 0; hd < 2; hd++) {
        const int32_t *widths = hd ? widths_hd : widths_aptx;
        rng.state = hd ? 0xC0FFEE01u : 0x51A7BEEFu;

        for (beat = 0; beat < DITHER_BEATS; beat++) {
            for (subband = 0; subband < REF_SUBBANDS; subband++) {
                int32_t low = -(int32_t)((uint32_t)1 << (widths[subband] - 1));
                int32_t high = (int32_t)(((uint32_t)1 << (widths[subband] - 1)) - 1);
                int32_t value = ref_rng_range(&rng, low, high);

                if (beat % 23 == 0)
                    value = low;                     /* most negative codeword */
                else if (beat % 29 == 0)
                    value = high;                    /* most positive codeword */
                else if (beat % 17 == 0)
                    value = 0;                       /* zero codeword */
                sequence[beat][subband] = value;
            }
        }

        dither_check_channel(hd ? "aptx-hd" : "aptx", hd, sequence,
                             DITHER_BEATS);
    }

    /*
     * 32-bit history wrap, driven both ways: the model and the production
     * helper advance together and must agree on every beat, and the sign bit
     * must actually have been reached so the wrap is exercised.
     */
    {
        struct ref_dither_channel model;
        struct aptx_channel production;
        int wrapped = 0;

        ref_dither_reset(&model);
        memset(&production, 0, sizeof(production));
        for (beat = 0; beat < 80; beat++) {
            int32_t samples[REF_SUBBANDS];
            for (subband = 0; subband < REF_SUBBANDS; subband++) {
                samples[subband] = (beat & 1) ? 1 : -1;
                production.quantize[subband].quantized_sample = samples[subband];
            }
            aptx_update_codeword_history(&production);
            ref_dither_advance(&model, samples);
            if (model.history & 0x80000000u)
                wrapped = 1;
            TASK_CHECK((uint32_t)production.codeword_history == model.history,
                       "dither: codeword history differs across 32-bit wrap");
        }
        TASK_CHECK(wrapped == 1,
                   "dither: history never crossed the 32-bit sign bit");
    }

    /*
     * Eight-beat parity sync.  The model independently derives the dither
     * parity for each beat and therefore knows the pre-sync parity equation.
     * The production insert-sync step must force exactly those beats and must
     * leave the emitted total parity equal to the eighth-beat marker.  The
     * selection of which sample to nudge is checked against the model too.
     */
    {
        struct ref_dither_channel left_model;
        struct ref_dither_channel right_model;
        struct aptx_channel channels[2];
        int32_t previous[2][REF_SUBBANDS];
        uint8_t production_sync = 0;
        unsigned model_sync = 0;
        int forced = 0;
        int clean = 0;

        rng.state = 0x13579BDFu;
        ref_dither_reset(&left_model);
        ref_dither_reset(&right_model);
        memset(channels, 0, sizeof(channels));
        memset(previous, 0, sizeof(previous));

        for (beat = 0; beat < 40; beat++) {
            int32_t pre[2][REF_SUBBANDS];
            int32_t parity_before = 0;
            int32_t parity_after = 0;
            int eighth = (model_sync == 7);
            int needs_force;
            int changed = 0;
            int changed_channel = -1;
            int changed_subband = -1;
            int channel, subband;

            for (channel = 0; channel < 2; channel++) {
                struct ref_dither_channel *model =
                    channel == REF_LEFT ? &left_model : &right_model;
                ref_dither_advance(model, previous[channel]);
                channels[channel].dither_parity = model->parity;
                for (subband = 0; subband < REF_SUBBANDS; subband++) {
                    int32_t low = (subband == 0) ? -64 : (subband == 1) ? -8
                                 : (subband == 2) ? -2 : -4;
                    int32_t high = (subband == 0) ? 63 : (subband == 1) ? 7
                                  : (subband == 2) ? 1 : 3;
                    int32_t sample = ref_rng_range(&rng, low, high);
                    pre[channel][subband] = sample;
                    channels[channel].quantize[subband].quantized_sample = sample;
                    /* A genuine neighbouring codeword with the other LSB. */
                    channels[channel].quantize[subband].
                        quantized_sample_parity_change =
                        (sample & 1) ? sample - 1 : sample + 1;
                    /* Descending errors make the selection deterministic. */
                    channels[channel].quantize[subband].error =
                        1000 - (channel * 100 + subband * 10);
                }
            }

            for (channel = 0; channel < 2; channel++) {
                const struct ref_dither_channel *model =
                    channel == REF_LEFT ? &left_model : &right_model;
                parity_before ^= model->parity;
                /* pre[channel][3] low bit is part of the parity equation even
                 * though the packed form will replace it. */
                for (subband = 0; subband < REF_SUBBANDS; subband++)
                    parity_before ^= pre[channel][subband] & 1;
            }
            parity_before &= 1;
            needs_force = ((parity_before ^ eighth) & 1);

            aptx_insert_sync(channels, &production_sync);
            model_sync = (model_sync + 1) & 7;
            TASK_CHECK(production_sync == model_sync,
                       "dither: parity sync index did not advance by one beat");

            for (channel = 0; channel < 2; channel++) {
                const struct ref_dither_channel *model =
                    channel == REF_LEFT ? &left_model : &right_model;
                parity_after ^= model->parity;
                for (subband = 0; subband < REF_SUBBANDS; subband++) {
                    int32_t now =
                        channels[channel].quantize[subband].quantized_sample;
                    parity_after ^= now & 1;
                    if (now != pre[channel][subband]) {
                        ++changed;
                        changed_channel = channel;
                        changed_subband = subband;
                        TASK_CHECK(now == channels[channel].quantize[subband].
                                             quantized_sample_parity_change,
                                   "dither: parity change used the wrong substitute");
                    }
                    previous[channel][subband] = now;
                }
            }
            parity_after &= 1;

            if (needs_force) {
                ++forced;
                TASK_CHECK(changed == 1,
                           "dither: forcing parity did not change exactly one sample");
                /* The nudge must land on the smallest-error candidate. */
                {
                    static const unsigned map[REF_SUBBANDS] = { 1, 2, 0, 3 };
                    int expected_channel = -1;
                    int expected_subband = -1;
                    int32_t best_error = 0;
                    int first = 1;
                    for (channel = 1; channel >= 0; channel--) {
                        for (subband = 0; subband < REF_SUBBANDS; subband++) {
                            int32_t err = channels[channel].quantize[map[subband]].error;
                            if (first || err < best_error) {
                                best_error = err;
                                expected_channel = channel;
                                expected_subband = (int)map[subband];
                                first = 0;
                            }
                        }
                    }
                    TASK_CHECK(changed_channel == expected_channel
                               && changed_subband == expected_subband,
                               "dither: parity nudge did not pick the smallest-error sample");
                }
            } else {
                ++clean;
                TASK_CHECK(changed == 0,
                           "dither: parity was forced on a non-eighth beat");
            }

            /* Emitted total parity must equal the eighth-beat marker. */
            TASK_CHECK(parity_after == (eighth ? 1 : 0),
                       "dither: emitted parity does not equal the eighth marker");
        }

        TASK_CHECK(forced + clean == 40,
                   "dither: parity sync beats were not all classified");
        TASK_CHECK(forced > 0 && clean > 0,
                   "dither: eight-beat sync never had both forced and clean beats");
        TASK_CHECK(forced == 5,
                   "dither: expected five forced beats in 40");
    }

    /*
     * Left and right histories are maintained independently.  Two production
     * channels are fed different sequences and each is compared with its own
     * model, so a shared-history bug in the library would show up here.
     */
    {
        struct ref_dither_channel left_model;
        struct ref_dither_channel right_model;
        struct aptx_channel left_production, right_production;
        int beat, subband;

        ref_dither_reset(&left_model);
        ref_dither_reset(&right_model);
        memset(&left_production, 0, sizeof(left_production));
        memset(&right_production, 0, sizeof(right_production));

        for (beat = 0; beat < 16; beat++) {
            int32_t left_q[REF_SUBBANDS];
            int32_t right_q[REF_SUBBANDS];

            for (subband = 0; subband < REF_SUBBANDS; subband++) {
                left_q[subband] = beat + subband;
                right_q[subband] = -(beat * 2 + subband + 1);
                left_production.quantize[subband].quantized_sample =
                    left_q[subband];
                right_production.quantize[subband].quantized_sample =
                    right_q[subband];
            }
            aptx_generate_dither(&left_production);
            aptx_generate_dither(&right_production);
            ref_dither_advance(&left_model, left_q);
            ref_dither_advance(&right_model, right_q);

            TASK_CHECK((uint32_t)left_production.codeword_history
                       == left_model.history,
                       "dither: left channel history differs from its model");
            TASK_CHECK((uint32_t)right_production.codeword_history
                       == right_model.history,
                       "dither: right channel history differs from its model");
            TASK_CHECK(left_production.dither[0] == left_model.subband[0],
                       "dither: left channel dither differs from its model");
            TASK_CHECK(right_production.dither[0] == right_model.subband[0],
                       "dither: right channel dither differs from its model");
        }
        TASK_CHECK(left_model.history != right_model.history,
                   "dither: left and right histories collapsed together");
        TASK_CHECK(left_model.subband[0] != right_model.subband[0]
                   || left_model.parity != right_model.parity,
                   "dither: left and right dither collapsed together");
    }
}

/* ------------------------------------------------------------------ */
/* Group 2: adaptive predictor contract                                */
/* ------------------------------------------------------------------ */

/*
 * One predictor scenario: alternating signs, silence, steady small values, a
 * sudden jump, near-clipping excursions, and a long random run that crosses
 * the residual history wrap for every subband order.
 */
static void run_predictor_scenario(const char *tag, int hd, int subband,
                                   int32_t base_state)
{
    const struct aptx_tables *tables = &all_tables[hd][subband];
    struct ref_predictor model;
    struct aptx_prediction production;
    struct aptx_invert_quantize invert;
    struct ref_rng rng;
    int32_t max_quantized = tables->tables_size - 2;
    enum { PREDICTOR_BEATS = 160 };
    int beat;

    ref_predictor_reset(&model);
    memset(&production, 0, sizeof(production));
    memset(&invert, 0, sizeof(invert));
    production.prev_sign[0] = 1;
    production.prev_sign[1] = 1;

    /* A non-zero starting factor exercises factor_select from the first beat. */
    model.factor_select = base_state % (tables->factor_max + 1);
    model.quantization_factor =
        (quantization_factors[(model.factor_select & 0xFF) >> 3] << 11)
        >> ((tables->factor_max - model.factor_select) >> 8);
    invert.factor_select = model.factor_select;
    invert.quantization_factor = model.quantization_factor;

    rng.state = 0x2468ACE0u ^ (uint32_t)(hd * 131 + subband * 17);

    for (beat = 0; beat < PREDICTOR_BEATS; beat++) {
        int32_t quantized_sample;
        int32_t dither;
        struct ref_subband_result expected;
        char message[192];
        int i;

        switch ((beat / 8) % 8) {
        case 0: /* alternating signs */
            quantized_sample = (beat & 1) ? max_quantized : -max_quantized;
            break;
        case 1: /* silence */
            quantized_sample = 0;
            break;
        case 2: /* steady small values */
            quantized_sample = (beat % 3) - 1;
            break;
        case 3: /* sudden large jump after stability */
            quantized_sample = (beat & 1) ? max_quantized : -(max_quantized / 2);
            break;
        case 4: /* near positive clip */
            quantized_sample = max_quantized - (beat & 1);
            break;
        case 5: /* near negative clip */
            quantized_sample = -max_quantized + (beat & 1);
            break;
        case 6: /* zero-crossing sweep */
            quantized_sample = ((beat * 7) % (2 * max_quantized + 1))
                               - max_quantized;
            break;
        default: /* uniform noise */
            quantized_sample = ref_rng_range(&rng, -max_quantized,
                                             max_quantized);
            break;
        }
        if (beat % 41 == 0)
            quantized_sample = -max_quantized;
        else if (beat % 43 == 0)
            quantized_sample = max_quantized;

        dither = (int32_t)(ref_rng_next(&rng) & 0xFFFFFFu) - 0x800000;
        if (beat % 37 == 0)
            dither = (beat & 1) ? 0x7FFFFF : -0x800000;

        /*
         * The current symbol is inverted with the quantization factor that
         * was in force before this beat.  When the factor changes, verify the
         * production difference against both the old-factor and new-factor
         * reconstructions so the distinction is asserted rather than assumed.
         */
        {
            int32_t factor_before = invert.quantization_factor;
            int32_t index = (quantized_sample ^ -(quantized_sample < 0)) + 1;
            int32_t half = tables->quantize_intervals[index] / 2;
            int32_t qr;
            int32_t old_factor_diff;
            int32_t new_factor_diff;

            if (quantized_sample < 0)
                half = -half;
            qr = ref_clip_symmetric(
                    ref_round_shift((int64_t)half * ((int64_t)1 << 32)
                                    + (int64_t)dither
                                      * (int64_t)tables->invert_quantize_dither_factors[index],
                                    32), 23);
            old_factor_diff = ref_wrap32(ref_floor_div(
                    (int64_t)factor_before * (int64_t)qr, 1 << 19));

            aptx_process_subband(&invert, &production, quantized_sample,
                                 dither, tables);
            ref_predictor_step(&model, quantized_sample, dither, tables,
                               &expected);

            new_factor_diff = ref_wrap32(ref_floor_div(
                    (int64_t)invert.quantization_factor * (int64_t)qr,
                    1 << 19));

            TASK_CHECK(invert.reconstructed_difference == old_factor_diff,
                       "predictor: current symbol used the new quantization factor");
            if (factor_before != invert.quantization_factor
                && new_factor_diff != old_factor_diff)
                TASK_CHECK(invert.reconstructed_difference != new_factor_diff,
                           "predictor: old/new factor rule is not observable");
        }

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d reversed difference expected %d got %d",
                 tag, subband, beat, expected.reconstructed_difference,
                 invert.reconstructed_difference);
        TASK_CHECK(invert.reconstructed_difference
                   == expected.reconstructed_difference, message);

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d next quantization factor expected %d got %d",
                 tag, subband, beat, model.quantization_factor,
                 invert.quantization_factor);
        TASK_CHECK(invert.quantization_factor == model.quantization_factor,
                   message);

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d factor_select expected %d got %d",
                 tag, subband, beat, model.factor_select, invert.factor_select);
        TASK_CHECK(invert.factor_select == model.factor_select, message);

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d pole weight 0 expected %d got %d",
                 tag, subband, beat, model.pole_weight[0],
                 production.s_weight[0]);
        TASK_CHECK(production.s_weight[0] == model.pole_weight[0], message);

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d pole weight 1 expected %d got %d",
                 tag, subband, beat, model.pole_weight[1],
                 production.s_weight[1]);
        TASK_CHECK(production.s_weight[1] == model.pole_weight[1], message);

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d pole sign 0 expected %d got %d",
                 tag, subband, beat, model.prev_sign[0], production.prev_sign[0]);
        TASK_CHECK(production.prev_sign[0] == model.prev_sign[0], message);

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d pole sign 1 expected %d got %d",
                 tag, subband, beat, model.prev_sign[1], production.prev_sign[1]);
        TASK_CHECK(production.prev_sign[1] == model.prev_sign[1], message);

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d reconstructed sample expected %d got %d",
                 tag, subband, beat, expected.reconstructed_sample,
                 production.previous_reconstructed_sample);
        TASK_CHECK(production.previous_reconstructed_sample
                   == expected.reconstructed_sample, message);

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d predicted difference expected %d got %d",
                 tag, subband, beat, model.predicted_difference,
                 production.predicted_difference);
        TASK_CHECK(production.predicted_difference
                   == model.predicted_difference, message);

        snprintf(message, sizeof(message),
                 "%s subband %d beat %d predicted sample expected %d got %d",
                 tag, subband, beat, model.predicted_sample,
                 production.predicted_sample);
        TASK_CHECK(production.predicted_sample == model.predicted_sample,
                   message);

        for (i = 0; i < tables->prediction_order; i++) {
            snprintf(message, sizeof(message),
                     "%s subband %d beat %d zero weight %d expected %d got %d",
                     tag, subband, beat, i, model.zero_weight[i],
                     production.d_weight[i]);
            TASK_CHECK(production.d_weight[i] == model.zero_weight[i], message);
        }

        /* Past the order, the residual history must have really advanced. */
        if (beat == tables->prediction_order)
            TASK_CHECK(model.residual[1] != 0 || model.residual[0] == 0,
                       "predictor: residual history did not advance");

        /*
         * Once the wrap has happened, the production ring buffer must expose
         * exactly the chronological history the model holds.  This is read
         * out by index arithmetic, not by copying the production layout.
         */
        if (beat >= tables->prediction_order) {
            for (i = 0; i < tables->prediction_order; i++) {
                int entry = (production.pos - i + tables->prediction_order)
                            % tables->prediction_order;
                int32_t slot = production.reconstructed_differences[
                                   tables->prediction_order + entry];
                snprintf(message, sizeof(message),
                         "%s subband %d beat %d residual history %d expected %d got %d",
                         tag, subband, beat, i, model.residual[i], slot);
                TASK_CHECK(slot == model.residual[i], message);
            }
        }
    }

    TASK_CHECK(model.zero_weight[0] != 0,
               "predictor: zero weights never adapted");
    TASK_CHECK(model.factor_select > 0,
               "predictor: factor_select never moved off zero");
    TASK_CHECK(model.pole_weight[0] != 0 || model.pole_weight[1] != 0,
               "predictor: pole weights never adapted");
}

static void run_predictor_group(void)
{
    int hd, subband;

    for (hd = 0; hd < 2; hd++) {
        for (subband = 0; subband < REF_SUBBANDS; subband++) {
            char tag[48];
            snprintf(tag, sizeof(tag), "%s-sub%d",
                     hd ? "aptx-hd" : "aptx", subband);
            run_predictor_scenario(tag, hd, subband,
                                   (int32_t)(hd * 977 + subband * 311 + 7));
        }
    }

    /* A dedicated zero-start scenario, exercising the factor chain from 0. */
    run_predictor_scenario("aptx-sub0-zero", 0, 0, 0);
    run_predictor_scenario("aptx-hd-sub3-zero", 1, 3, 0);

    /* Explicit 24-bit saturation: reconstructed samples must clamp. */
    {
        const struct aptx_tables *tables = &all_tables[0][0];
        struct ref_predictor model;
        struct aptx_prediction production;
        struct aptx_invert_quantize invert;
        int32_t max_quantized = tables->tables_size - 2;
        int clipped = 0;
        int beat;

        ref_predictor_reset(&model);
        memset(&production, 0, sizeof(production));
        memset(&invert, 0, sizeof(invert));
        production.prev_sign[0] = 1;
        production.prev_sign[1] = 1;

        for (beat = 0; beat < 80; beat++) {
            int32_t quantized_sample = (beat % 2) ? max_quantized
                                                  : -max_quantized;
            struct ref_subband_result expected;

            aptx_process_subband(&invert, &production, quantized_sample, 0,
                                 tables);
            ref_predictor_step(&model, quantized_sample, 0, tables, &expected);

            TASK_CHECK(production.predicted_sample == model.predicted_sample,
                       "predictor: saturation scenario predicted sample differs");
            TASK_CHECK(production.previous_reconstructed_sample
                       == model.previous_reconstructed,
                       "predictor: saturation scenario reconstructed sample differs");
            if (model.predicted_sample == (1 << 23) - 1
                || model.predicted_sample == -(1 << 23))
                clipped = 1;
        }
        TASK_CHECK(clipped == 1,
                   "predictor: saturation scenario never reached the 24-bit limit");
    }
}

/* ------------------------------------------------------------------ */
/* Group 3: stream contract                                            */
/* ------------------------------------------------------------------ */

/* Independent QMF synthesis using a chronological tap history, not the
 * production circular buffer.  Only the coefficient tables are shared. */
struct ref_filter_state {
    int32_t history[FILTER_TAPS];   /* [0] is the newest sample */
};

static void ref_filter_push(struct ref_filter_state *state, int32_t sample)
{
    int i;

    for (i = FILTER_TAPS - 1; i > 0; i--)
        state->history[i] = state->history[i - 1];
    state->history[0] = sample;
}

static int32_t ref_filter_convolve(const struct ref_filter_state *state,
                                   const int32_t *coefficients,
                                   unsigned shift)
{
    int64_t sum = 0;
    int tap;

    /* Coefficient 0 pairs with the oldest tap, matching the FIR phase. */
    for (tap = 0; tap < FILTER_TAPS; tap++)
        sum += (int64_t)state->history[FILTER_TAPS - 1 - tap]
               * (int64_t)coefficients[tap];
    return ref_clip_symmetric(ref_round_shift(sum, shift), 23);
}

static void ref_synthesis_pair(struct ref_filter_state state[NB_FILTERS],
                               const int32_t coefficients[NB_FILTERS][FILTER_TAPS],
                               unsigned shift, int32_t low, int32_t high,
                               int32_t output[NB_FILTERS])
{
    int32_t subbands[NB_FILTERS];
    int i;

    subbands[0] = ref_wrap32((int64_t)low + high);
    subbands[1] = ref_wrap32((int64_t)low - high);
    for (i = 0; i < NB_FILTERS; i++) {
        ref_filter_push(&state[i], subbands[1 - i]);
        output[i] = ref_filter_convolve(&state[i], coefficients[i], shift);
    }
}

struct ref_synthesis {
    struct ref_filter_state outer[NB_FILTERS];
    struct ref_filter_state inner[NB_FILTERS][NB_FILTERS];
};

static void ref_synthesis_reset(struct ref_synthesis *synthesis)
{
    memset(synthesis, 0, sizeof(*synthesis));
}

static void ref_synthesis_run(struct ref_synthesis *synthesis,
                              const int32_t subbands[REF_SUBBANDS],
                              int32_t samples[4])
{
    int32_t intermediate[4];
    int i;

    for (i = 0; i < 2; i++)
        ref_synthesis_pair(synthesis->inner[i], aptx_qmf_inner_coeffs, 22,
                           subbands[2 * i + 0], subbands[2 * i + 1],
                           &intermediate[2 * i]);
    for (i = 0; i < 2; i++)
        ref_synthesis_pair(synthesis->outer, aptx_qmf_outer_coeffs, 21,
                           intermediate[0 + i], intermediate[2 + i],
                           &samples[2 * i]);
}

static int32_t ref_load_s24(const unsigned char *p)
{
    return (int32_t)((uint32_t)p[0]
                     | ((uint32_t)p[1] << 8)
                     | ((uint32_t)(int8_t)p[2] << 16));
}

static void ref_store_s24(unsigned char *p, int32_t value)
{
    p[0] = (unsigned char)(((uint32_t)value >> 0) & 0xFF);
    p[1] = (unsigned char)(((uint32_t)value >> 8) & 0xFF);
    p[2] = (unsigned char)(((uint32_t)value >> 16) & 0xFF);
}

static void stream_read_codewords(enum ref_format fmt,
                                  const unsigned char *output,
                                  uint32_t codewords[2])
{
    unsigned bytes = ref_codeword_bits(fmt) / 8;
    int channel;

    for (channel = 0; channel < 2; channel++)
        codewords[channel] =
            ref_load_codeword(fmt, output + (size_t)channel * bytes);
}

/*
 * Compare one production channel's observable state with the reference model.
 * The reference has been fed only the codewords that actually appeared.
 */
static void stream_compare_channel(const char *tag, int beat, int hd,
                                   const struct aptx_channel *production,
                                   const struct ref_stream_channel *reference)
{
    char message[192];
    int subband, i;

    TASK_CHECK((uint32_t)production->codeword_history
               == reference->dither.history,
               "stream: codeword history differs from reference");
    TASK_CHECK(production->dither_parity == reference->dither.parity,
               "stream: dither parity differs from reference");

    for (subband = 0; subband < REF_SUBBANDS; subband++) {
        const struct ref_predictor *ref = &reference->subband[subband];
        const struct aptx_prediction *pred = &production->prediction[subband];
        const struct aptx_invert_quantize *inv =
            &production->invert_quantize[subband];
        int order = all_tables[hd][subband].prediction_order;

        snprintf(message, sizeof(message),
                 "%s beat %d subband %d stream dither expected %d got %d",
                 tag, beat, subband, reference->dither.subband[subband],
                 production->dither[subband]);
        TASK_CHECK(production->dither[subband]
                   == reference->dither.subband[subband], message);

        snprintf(message, sizeof(message),
                 "%s beat %d subband %d stream factor expected %d got %d",
                 tag, beat, subband, ref->quantization_factor,
                 inv->quantization_factor);
        TASK_CHECK(inv->quantization_factor == ref->quantization_factor,
                   message);

        snprintf(message, sizeof(message),
                 "%s beat %d subband %d stream factor_select expected %d got %d",
                 tag, beat, subband, ref->factor_select, inv->factor_select);
        TASK_CHECK(inv->factor_select == ref->factor_select, message);

        snprintf(message, sizeof(message),
                 "%s beat %d subband %d stream reversed difference expected %d got %d",
                 tag, beat, subband, ref->residual[0],
                 inv->reconstructed_difference);
        TASK_CHECK(inv->reconstructed_difference == ref->residual[0], message);

        snprintf(message, sizeof(message),
                 "%s beat %d subband %d stream predicted difference expected %d got %d",
                 tag, beat, subband, ref->predicted_difference,
                 pred->predicted_difference);
        TASK_CHECK(pred->predicted_difference == ref->predicted_difference,
                   message);

        snprintf(message, sizeof(message),
                 "%s beat %d subband %d stream predicted sample expected %d got %d",
                 tag, beat, subband, ref->predicted_sample,
                 pred->predicted_sample);
        TASK_CHECK(pred->predicted_sample == ref->predicted_sample, message);

        TASK_CHECK(pred->s_weight[0] == ref->pole_weight[0]
                   && pred->s_weight[1] == ref->pole_weight[1],
                   "stream: pole weights differ from reference");
        TASK_CHECK(pred->prev_sign[0] == ref->prev_sign[0]
                   && pred->prev_sign[1] == ref->prev_sign[1],
                   "stream: pole sign history differs from reference");
        TASK_CHECK(pred->previous_reconstructed_sample
                   == ref->previous_reconstructed,
                   "stream: previous reconstructed sample differs");

        for (i = 0; i < 24; i++) {
            int32_t expected = (i < order) ? ref->zero_weight[i] : 0;
            snprintf(message, sizeof(message),
                     "%s beat %d subband %d stream zero weight %d expected %d got %d",
                     tag, beat, subband, i, expected, pred->d_weight[i]);
            TASK_CHECK(pred->d_weight[i] == expected, message);
        }
    }
}

/* Sustained stereo 24-bit audio with quiet, loud, stepped and clipping runs. */
static void stream_generate_pcm(unsigned char *pcm, size_t frames)
{
    struct ref_rng rng;
    size_t frame;

    rng.state = 0xDEADBEEFu;
    for (frame = 0; frame < frames; frame++) {
        int phase = (int)((frame / 64) % 6);
        int channel;

        for (channel = 0; channel < 2; channel++) {
            int32_t sample;
            switch (phase) {
            case 0:
                sample = ((int32_t)(frame % 40) - 20) * 1000;
                break;
            case 1:
                sample = ref_rng_range(&rng, -4000000, 4000000);
                break;
            case 2:
                sample = (int32_t)(((frame * 37) % 8000000) - 4000000);
                break;
            case 3:
                sample = ref_rng_range(&rng, -200000, 200000);
                break;
            case 4:
                sample = (frame & 1) ? 0x7FFFFF : -0x800000;
                break;
            default:
                sample = ref_rng_range(&rng, -8388608, 8388607);
                break;
            }
            sample += channel ? 137 : -137;
            if (sample > 0x7FFFFF)
                sample = 0x7FFFFF;
            if (sample < -0x800000)
                sample = -0x800000;
            ref_store_s24(pcm + (frame * 2 + (size_t)channel) * 3, sample);
        }
    }
}

/*
 * encode_finish buffer contract: short buffers must be filled partially and
 * return 0 without resetting; once the padding is complete the call returns 1
 * and the context is reset and reusable.
 */
static void stream_check_finish_chunks(enum ref_format fmt,
                                       const unsigned char *pcm,
                                       size_t pcm_size,
                                       const unsigned char *one_shot_bits,
                                       size_t one_shot_size)
{
    struct aptx_context *ctx;
    static unsigned char prefix[8192];
    size_t prefix_size = 0;
    size_t total_finish = 0;
    size_t expected_finish;
    int rc = 0;
    int iterations = 0;

    ctx = aptx_init(fmt == REF_APTX_HD ? 1 : 0);
    TASK_CHECK(ctx != NULL, "stream: context allocation failed for finish test");
    if (!ctx)
        return;

    {
        size_t consumed = aptx_encode(ctx, pcm, pcm_size, prefix,
                                      sizeof(prefix), &prefix_size);
        TASK_CHECK(consumed == pcm_size,
                   "stream: finish test did not consume the whole input");
        TASK_CHECK(prefix_size == one_shot_size,
                   "stream: finish test prefix differs from reference length");
        TASK_CHECK(memcmp(prefix, one_shot_bits, prefix_size) == 0,
                   "stream: finish test prefix differs from reference bytes");
    }

    /*
     * The finish padding rounds the 90-sample latency up to a whole beat:
     * 23 beats are emitted.  We check the count against an independent
     * computation, not against a second call into the library.
     */
    expected_finish = 23 * ((fmt == REF_APTX_HD) ? 6u : 4u);

    while (!rc) {
        unsigned char small[64];
        size_t produced = 0;
        size_t budget;

        TASK_CHECK(iterations < 200,
                   "stream: encode_finish never completed");
        if (iterations >= 200)
            break;

        switch (iterations % 3) {
        case 0:
            budget = 4;
            break;
        case 1:
            budget = (fmt == REF_APTX_HD) ? 6 : 4;
            break;
        default:
            budget = 13;
            break;
        }
        rc = aptx_encode_finish(ctx, small, budget, &produced);
        TASK_CHECK(produced <= budget,
                   "stream: encode_finish overran a short buffer");
        total_finish += produced;
        ++iterations;
    }

    TASK_CHECK(rc == 1, "stream: encode_finish did not complete");
    TASK_CHECK(total_finish == expected_finish,
               "stream: chunked finish byte count differs from derived count");
    TASK_CHECK(iterations > 1,
               "stream: short finish buffers required only one call");

    /* The context was reset by the completing call and is reusable. */
    {
        unsigned char again[16];
        size_t written = 0;
        size_t consumed = aptx_encode(ctx, pcm, 24, again, sizeof(again),
                                      &written);
        TASK_CHECK(consumed == 24,
                   "stream: finish did not leave a reusable context");
        TASK_CHECK(written == ((fmt == REF_APTX_HD) ? 6u : 4u),
                   "stream: reused context wrote a wrong byte count");
        TASK_CHECK(memcmp(again, one_shot_bits, written) == 0,
                   "stream: reused context does not reproduce the stream head");
    }

    aptx_finish(ctx);
}

static void run_stream_scenario(enum ref_format fmt)
{
    enum { STREAM_BEATS = 400 };
    static unsigned char pcm[STREAM_BEATS * 24];
    static unsigned char bits[STREAM_BEATS * 32];
    static unsigned char decoded[STREAM_BEATS * 32];
    struct aptx_context *encoder;
    struct aptx_context *decoder;
    struct ref_stream_channel encode_reference[2];
    struct ref_stream_channel decode_reference[2];
    struct ref_synthesis decode_synthesis[2];
    uint32_t codewords[2];
    size_t sample_size = (fmt == REF_APTX_HD) ? 6 : 4;
    size_t encoded_bytes = 0;
    size_t decoded_bytes = 0;
    size_t beat;
    int hd = (fmt == REF_APTX_HD);
    int channel;

    stream_generate_pcm(pcm, STREAM_BEATS * 4);

    encoder = aptx_init(hd);
    decoder = aptx_init(hd);
    TASK_CHECK(encoder != NULL && decoder != NULL,
               "stream: context allocation failed");
    if (!encoder || !decoder)
        return;

    for (channel = 0; channel < 2; channel++) {
        ref_stream_channel_reset(&encode_reference[channel]);
        ref_stream_channel_reset(&decode_reference[channel]);
        ref_synthesis_reset(&decode_synthesis[channel]);
    }

    for (beat = 0; beat < STREAM_BEATS; beat++) {
        size_t written = 0;
        size_t consumed;

        /* --- encoder, one beat at a time --- */
        consumed = aptx_encode(encoder, pcm + beat * 24, 24,
                               bits + encoded_bytes, sample_size, &written);
        TASK_CHECK(consumed == 24,
                   "stream: aptx_encode did not consume one beat of input");
        TASK_CHECK(written == sample_size,
                   "stream: aptx_encode wrote a wrong byte count");
        encoded_bytes += written;
        stream_read_codewords(fmt, bits + encoded_bytes - sample_size,
                              codewords);

        for (channel = 0; channel < 2; channel++) {
            struct aptx_channel *production =
                &encoder->channels[channel];

            ref_stream_step(&encode_reference[channel], fmt,
                            codewords[channel]);
            stream_compare_channel("encode", (int)beat, hd, production,
                                   &encode_reference[channel]);
        }

        /* --- decoder, one beat at a time --- */
        consumed = aptx_decode(decoder, bits + beat * sample_size, sample_size,
                               decoded + decoded_bytes,
                               sizeof(decoded) - decoded_bytes, &written);
        TASK_CHECK(consumed == sample_size,
                   "stream: aptx_decode did not consume one beat of input");
        decoded_bytes += written;

        for (channel = 0; channel < 2; channel++) {
            struct aptx_channel *production =
                &decoder->channels[channel];
            struct ref_stream_channel *reference =
                &decode_reference[channel];
            int32_t subbands[REF_SUBBANDS];
            int32_t samples[4];
            int subband;

            ref_stream_step(reference, fmt, codewords[channel]);
            stream_compare_channel("decode", (int)beat, hd, production,
                                   reference);

            /*
             * Independent QMF synthesis, using this beat's reconstructed
             * subband samples.  It runs every beat so the filter history
             * matches the decoder's even during the latency period.
             */
            for (subband = 0; subband < REF_SUBBANDS; subband++)
                subbands[subband] =
                    reference->subband[subband].previous_reconstructed;
            ref_synthesis_run(&decode_synthesis[channel], subbands, samples);

            /* After the 90-sample latency the emitted PCM must match. */
            if (beat >= 22) {
                int sample;
                for (sample = (beat == 22) ? 2 : 0; sample < 4; sample++) {
                    size_t index = ((size_t)(beat - 22) * 4
                                    + (size_t)(sample - 2)) * 6
                                   + (size_t)channel * 3;
                    int32_t got;
                    char message[160];

                    got = ref_load_s24(decoded + index);
                    snprintf(message, sizeof(message),
                             "stream: PCM beat %d sample %d channel %d "
                             "expected %d got %d",
                             (int)beat, sample, channel, samples[sample], got);
                    TASK_CHECK(got == samples[sample], message);
                }
            }
        }

        /*
         * Encoder and decoder processed the same actual codewords this beat,
         * so their prediction state must agree even though they started as
         * separate contexts.
         */
        for (channel = 0; channel < 2; channel++) {
            const struct aptx_channel *enc = &encoder->channels[channel];
            const struct aptx_channel *dec = &decoder->channels[channel];
            char message[160];
            int subband;

            TASK_CHECK(enc->codeword_history == dec->codeword_history,
                       "stream: encoder and decoder codeword histories differ");
            for (subband = 0; subband < REF_SUBBANDS; subband++) {
                snprintf(message, sizeof(message),
                         "stream: beat %d subband %d encoder/decoder "
                         "prediction differs", (int)beat, subband);
                TASK_CHECK(enc->prediction[subband].predicted_sample
                           == dec->prediction[subband].predicted_sample,
                           message);
                snprintf(message, sizeof(message),
                         "stream: beat %d subband %d encoder/decoder "
                         "factor differs", (int)beat, subband);
                TASK_CHECK(enc->invert_quantize[subband].quantization_factor
                           == dec->invert_quantize[subband].quantization_factor,
                           message);
                snprintf(message, sizeof(message),
                         "stream: beat %d subband %d encoder/decoder "
                         "zero weight differs", (int)beat, subband);
                TASK_CHECK(enc->prediction[subband].d_weight[0]
                           == dec->prediction[subband].d_weight[0],
                           message);
            }
        }
    }

    /* 90-sample latency: 1600 input samples yield 1510 output samples. */
    TASK_CHECK(decoded_bytes == (size_t)(STREAM_BEATS * 4 - 90) * 6,
               "stream: decoded byte count does not reflect the 90-sample latency");

    /* Real finish with a generous buffer also has to reset and reuse. */
    {
        unsigned char finish[256];
        size_t written = 0;
        int rc;

        rc = aptx_encode_finish(encoder, finish, sizeof(finish), &written);
        TASK_CHECK(rc == 1, "stream: ample-buffer encode_finish failed");
        TASK_CHECK(written == 23 * sample_size,
                   "stream: ample-buffer finish emitted the wrong padding count");
        /* Completion resets the context, ready for a new stream. */
        TASK_CHECK(encoder->encode_remaining == (LATENCY_SAMPLES + 3) / 4,
                   "stream: encode_finish did not return the context to its reset state");

        /* Encode a new stream from the reset context. */
        {
            unsigned char head[16];
            size_t head_written = 0;
            size_t head_consumed =
                aptx_encode(encoder, pcm, 24, head, sizeof(head),
                            &head_written);
            TASK_CHECK(head_consumed == 24,
                       "stream: encoder after finish rejected input");
            TASK_CHECK(head_written == sample_size
                       && memcmp(head, bits, sample_size) == 0,
                       "stream: encoder after finish does not reproduce the head");
        }
    }

    /* The decoder keeps its latency state; reset makes it reusable. */
    aptx_reset(decoder);
    {
        unsigned char head[64];
        size_t written = 0;
        size_t consumed = aptx_decode(decoder, bits, sample_size, head,
                                      sizeof(head), &written);
        TASK_CHECK(consumed == sample_size,
                   "stream: reset decoder rejected the first beat");
        TASK_CHECK(written == 0,
                   "stream: reset decoder should still be in its latency period");
    }

    /* Decoded PCM is lossy but must not be trivially silent. */
    {
        int nonzero = 0;
        size_t i;
        for (i = 0; i < decoded_bytes; i++)
            if (decoded[i] != 0)
                ++nonzero;
        TASK_CHECK(nonzero > (int)(decoded_bytes / 8),
                   "stream: decoded PCM is unexpectedly silent");
    }

    /*
     * Two interleaved encoder instances must keep separate histories.  Each
     * pair has its own reference model; if state leaked between pairs the
     * per-beat comparisons would fail.
     */
    {
        struct aptx_context *pair_encoder[2];
        struct aptx_context *pair_decoder[2];
        struct ref_stream_channel pair_model[2][2];
        static const size_t offsets[2] = { 0, 53 };
        int pair, step;

        for (pair = 0; pair < 2; pair++) {
            pair_encoder[pair] = aptx_init(hd);
            pair_decoder[pair] = aptx_init(hd);
            TASK_CHECK(pair_encoder[pair] != NULL && pair_decoder[pair] != NULL,
                       "stream: interleaved context allocation failed");
            if (!pair_encoder[pair] || !pair_decoder[pair])
                return;
            for (channel = 0; channel < 2; channel++)
                ref_stream_channel_reset(&pair_model[pair][channel]);
        }

        for (step = 0; step < 96; step++) {
            for (pair = 0; pair < 2; pair++) {
                size_t at = offsets[pair] + (size_t)step;
                unsigned char out[16];
                size_t written = 0;
                size_t consumed;

                if (at >= STREAM_BEATS)
                    continue;

                consumed = aptx_encode(pair_encoder[pair], pcm + at * 24, 24,
                                       out, sample_size, &written);
                TASK_CHECK(consumed == 24,
                           "stream: interleaved encode rejected input");
                stream_read_codewords(fmt, out, codewords);

                for (channel = 0; channel < 2; channel++) {
                    ref_stream_step(&pair_model[pair][channel], fmt,
                                    codewords[channel]);
                    stream_compare_channel("interleaved", step, hd,
                                           &pair_encoder[pair]->channels[channel],
                                           &pair_model[pair][channel]);
                }
            }
        }

        /* The two models must have diverged: they saw different audio. */
        TASK_CHECK(pair_model[0][0].dither.history
                   != pair_model[1][0].dither.history,
                   "stream: interleaved histories collapsed together");

        for (pair = 0; pair < 2; pair++) {
            aptx_finish(pair_encoder[pair]);
            aptx_finish(pair_decoder[pair]);
        }
    }

    stream_check_finish_chunks(fmt, pcm, STREAM_BEATS * 24, bits,
                               encoded_bytes);

    aptx_finish(encoder);
    aptx_finish(decoder);
}

static void run_stream_group(void)
{
    run_stream_scenario(REF_APTX);
    run_stream_scenario(REF_APTX_HD);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int task_group(const char *group)
{
    if (strcmp(group, "dither") == 0) {
        run_dither_group();
        return 1;
    }
    if (strcmp(group, "predictor") == 0) {
        run_predictor_group();
        return 1;
    }
    if (strcmp(group, "stream") == 0) {
        run_stream_group();
        return 1;
    }
    return 0;
}
