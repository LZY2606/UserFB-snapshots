/*
    layer2_contract.c - independent fixed-point contract for the MPEG Audio
    Layer II branch of minimp3.

    The production decoder, its public interface and the existing PCM/PSNR
    regression are untouched.  This file adds three self-contained check
    groups:

      digits - grouped quantization (bit allocation levels 17/18/19): how an
               explicit triple of 3/5/9 level samples is packed bit-by-bit
               into a 5/7/10 bit codeword, how the real
               L12_dequantize_granule unpacks it, and exactly which bits are
               consumed.  Plain (non-grouped) samples frame the group and the
               stream is read at every start offset 0..7.
      scfsi  - the four scalefactor selection information reuse patterns:
               how many 6 bit fields are really read, which scale each of the
               twelve subband moments uses, and grouped/non-grouped rebuild.
      frames - fully planned Layer II frames (mono, stereo, joint stereo)
               decoded through the public mp3dec_decode_frame interface, with
               an independent read-back model of every header/bitalloc/SCFSI/
               payload field and a read-only observation point comparing the
               pre-synthesis subband values right after L12_apply_scf_384.

    The independent model below never calls the production get_bits, codeword
    splitting, dequantization or scalefactor code; it only reads the static
    allocation table constants (format data) and drives the public decoder and
    the two production entry points under test.  Expected values are always
    computed by the model, never copied from production output.
*/
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#ifndef MINIMP3_LAYER2_OBSERVE
#define MINIMP3_LAYER2_OBSERVE 1
#endif
#include "minimp3.h"
#include "task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Independent bit writer / reader (model side, not production code)  */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint8_t *data;
    int      byte_size;
    int      pos;        /* bits written */
} model_bs_t;

static void model_bs_init(model_bs_t *bs, uint8_t *data, int byte_size)
{
    memset(data, 0, (size_t)byte_size);
    bs->data = data;
    bs->byte_size = byte_size;
    bs->pos = 0;
}

static void model_put(model_bs_t *bs, uint32_t value, int nbits)
{
    int i;
    for (i = nbits - 1; i >= 0; i--)
    {
        int bit = (int)((value >> i) & 1u);
        int byte_index = bs->pos >> 3;
        if (byte_index >= bs->byte_size)
        {
            TASK_CHECK(0, "model bit writer overflowed its buffer");
            return;
        }
        bs->data[byte_index] = (uint8_t)(bs->data[byte_index] |
                                         (bit << (7 - (bs->pos & 7))));
        bs->pos++;
    }
}

static uint32_t model_get(const uint8_t *data, int start_bit, int nbits)
{
    uint32_t value = 0;
    int i;
    for (i = 0; i < nbits; i++)
    {
        int abs_bit = start_bit + i;
        int bit = (data[abs_bit >> 3] >> (7 - (abs_bit & 7))) & 1;
        value = (value << 1) | (uint32_t)bit;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Format tables (independent copies of the ISO 11172-3 tables; the    */
/* production tables are only read through L12_subband_alloc_table).   */
/* ------------------------------------------------------------------ */

static const uint8_t model_bitalloc_code_tab[7][16] =
{
    /* tab 0: offset 0,  4 bits, MPEG1 groups 0/1 */
    { 0,17, 3, 4, 5,6,7, 8,9,10,11,12,13,14,15,16 },
    /* tab 1: offset 16, 4 bits, MPEG1 group 2 */
    { 0,17,18, 3,19,4,5, 6,7, 8, 9,10,11,12,13,16 },
    /* tab 2: offset 32, 3 bits, MPEG1 group 3 */
    { 0,17,18, 3,19,4,5,16, 0,0,0,0,0,0,0,0 },
    /* tab 3: offset 40, 2 bits, MPEG1 group 4 */
    { 0,17,18,16, 0,0,0,0,0,0,0,0,0,0,0,0 },
    /* tab 4: offset 44, low-rate MPEG1 */
    { 0,17,18,19, 4,5,6, 7,8, 9,10,11,12,13,14,15 },
    /* tab 5: offset 60, MPEG2 */
    { 0,17,18, 3,19,4,5, 6,7, 8, 9,10,11,12,13,14 },
    /* tab 6: offset 76, Layer I */
    { 0, 2, 3, 4, 5,6,7, 8,9,10,11,12,13,14,15,16 }
};

typedef struct
{
    int tab_index, code_tab_width, band_count;
} model_alloc_group_t;

static const model_alloc_group_t model_mpeg1_groups[4] =
{
    { 0, 4, 3 }, { 1, 4, 8 }, { 2, 3, 12 }, { 3, 2, 7 }
};

static const unsigned model_mpeg1_l2_bitrates[15] =
    { 0,32,48,56,64,80,96,112,128,160,192,224,256,320,384 };

typedef struct
{
    int total_bands, stereo_bands;
    model_alloc_group_t groups[4];
    int ngroups;
} model_alloc_t;

/* Independent reconstruction of L12_subband_alloc_table for MPEG1 L2. */
static void model_alloc_plan(int stereo_mode, int mode_ext, int bitrate_idx,
                             int sample_rate_idx, model_alloc_t *plan)
{
    unsigned kbps = model_mpeg1_l2_bitrates[bitrate_idx];
    if (stereo_mode != MODE_MONO)
        kbps >>= 1;
    if (kbps == 0)
        kbps = 192; /* production free-format fallback */

    if (kbps < 56)
    {
        static const model_alloc_group_t low[2] = { { 4, 4, 2 }, { 4, 3, 10 } };
        plan->groups[0] = low[0];
        plan->groups[1] = low[1];
        plan->ngroups = 2;
        plan->total_bands = sample_rate_idx == 2 ? 12 : 8;
    } else
    {
        memcpy(plan->groups, model_mpeg1_groups, sizeof(model_mpeg1_groups));
        plan->ngroups = 4;
        plan->total_bands = 27;
        if (kbps >= 96 && sample_rate_idx != 1)
            plan->total_bands = 30;
    }

    if (stereo_mode == MODE_MONO)
        plan->stereo_bands = 0;
    else if (stereo_mode == MODE_JOINT_STEREO)
        plan->stereo_bands = (mode_ext << 2) + 4;
    else
        plan->stereo_bands = 32;
    if (plan->stereo_bands > plan->total_bands)
        plan->stereo_bands = plan->total_bands;
}

static int model_group_of_band(const model_alloc_t *plan, int band)
{
    int start = 0, g;
    for (g = 0; g < plan->ngroups; g++)
    {
        if (band < start + plan->groups[g].band_count)
            return g;
        start += plan->groups[g].band_count;
    }
    return plan->ngroups - 1;
}

static int model_ba_bits(const model_alloc_t *plan, int band)
{
    return plan->groups[model_group_of_band(plan, band)].code_tab_width;
}

static int model_ba_tab_index(const model_alloc_t *plan, int band)
{
    return plan->groups[model_group_of_band(plan, band)].tab_index;
}

static uint8_t model_decode_ba(const model_alloc_t *plan, int band, uint32_t code)
{
    return model_bitalloc_code_tab[model_ba_tab_index(plan, band)][code & 15u];
}

/* Find a bit allocation code that decodes to the wanted class. */
static int model_find_code(const model_alloc_t *plan, int band, int want_grouped)
{
    int width = model_ba_bits(plan, band);
    int ti = model_ba_tab_index(plan, band);
    int code, count = 1 << width;
    for (code = 0; code < count; code++)
    {
        int ba = model_bitalloc_code_tab[ti][code];
        if (want_grouped ? (ba >= 17 && ba <= 19) : (ba >= 2 && ba <= 16))
            return code;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Grouped quantization model (allocation levels 17, 18 and 19)        */
/* ------------------------------------------------------------------ */

static int grouped_modulus(int ba)
{
    return (2 << (ba - 17)) + 1;                       /* 3, 5, 9 */
}

static int grouped_code_bits(int ba)
{
    int mod = grouped_modulus(ba);
    return mod + 2 - (mod >> 3);                       /* 5, 7, 10 */
}

static int model_sample_span(int ba)
{
    /* Production normalization denominators: grouped radices 3/5/9, plain
       2^ba - 1 (g_deq_L12 rows DQ(3), DQ(7), DQ(15), ... DQ(65535)). */
    return ba >= 17 ? grouped_modulus(ba) : (1 << ba) - 1;
}

/* Pack an explicit triple of centered samples into one codeword using the
   production mixed-radix ordering: the first transmitted sample is the least
   significant radix digit. */
static uint32_t model_pack_group(int ba, const int samples[3])
{
    int mod = grouped_modulus(ba);
    int digit[3], k;
    uint32_t code = 0;
    for (k = 0; k < 3; k++)
    {
        digit[k] = samples[k] + mod / 2;              /* undo the center */
        TASK_CHECK(digit[k] >= 0 && digit[k] < mod, "grouped digit inside radix range");
    }
    for (k = 2; k >= 0; k--)
        code = code * (uint32_t)mod + (uint32_t)digit[k];
    return code;
}

/* Independent inverse of the production code/=mod split. */
static void model_unpack_group(int ba, uint32_t code, int out[3])
{
    int mod = grouped_modulus(ba), k;
    for (k = 0; k < 3; k++)
    {
        out[k] = (int)(code % (uint32_t)mod) - mod / 2;
        code /= (uint32_t)mod;
    }
}

/* ------------------------------------------------------------------ */
/* Scalefactor model                                                   */
/* ------------------------------------------------------------------ */

static const int model_scfsi_reads[4] = { 3, 2, 1, 2 };
/* Final array position -> transmitted field index.  The reader always
   advances the scf pointer after every mask hit, so absent mask bits simply
   keep the stale previous value: pattern 1 reuses field 0 at position 1, and
   pattern 3 reuses field 1 at position 2. */
static const int model_scfsi_pick[4][3] =
{
    { 0, 1, 2 }, { 0, 0, 1 }, { 0, 0, 0 }, { 0, 1, 1 }
};

/* Expected float scale for a 6 bit scalefactor index, computed in double
   precision exactly as the contract requires. */
static double model_scale_value(int ba, int index)
{
    double levels = (double)model_sample_span(ba);
    return 2.0 * pow(2.0, -(double)index / 3.0) / levels;
}

static const unsigned model_sample_rates[3] = { 44100, 48000, 32000 };

static int scf_float_close(double expected, float actual)
{
    double tol = 2e-6 * (1.0 + fabs(expected));
    return fabs((double)actual - expected) <= tol;
}

/* Production granule buffer layout, derived independently from the format:
   slot i is left (even i) at 18*(i/2) or right (odd i) at 576+18*((i-1)/2),
   granule group j occupies moments 3*j..3*j+2. */
static int model_slot_offset(int slot, int j, int k)
{
    int band = slot >> 1;
    return (slot & 1) * 576 + 18 * band + 3 * j + k;
}

/* ================================================================== */
/* digits group                                                        */
/* ================================================================== */

static int check_digits(void)
{
    static const int levels[3] = { 17, 18, 19 };
    static const int expect_mod[3] = { 3, 5, 9 };
    static const int expect_bits[3] = { 5, 7, 10 };
    const int plain_ba_before = 3;
    const int plain_ba_after = 4;
    int li, offset;

    for (li = 0; li < 3; li++)
    {
        int ba = levels[li];
        int mod = grouped_modulus(ba);
        int nbits = grouped_code_bits(ba);
        int d0, d1, d2;
        char msg[192];

        TASK_CHECK(mod == expect_mod[li], "grouped modulus is 3, 5 then 9");
        TASK_CHECK(nbits == expect_bits[li], "codeword width is 5, 7 then 10 bits");
        TASK_CHECK((1u << nbits) >= (unsigned)(mod * mod * mod),
                   "all legal triples fit the codeword width");

        for (d0 = 0; d0 < mod; d0++)
        for (d1 = 0; d1 < mod; d1++)
        for (d2 = 0; d2 < mod; d2++)
        {
            int triple[3], expect[3], got[3], j, k, m;
            uint32_t code;
            uint8_t bytes[32];
            float grbuf[2 * 576];

            triple[0] = d0 - mod / 2;
            triple[1] = d1 - mod / 2;
            triple[2] = d2 - mod / 2;
            code = model_pack_group(ba, triple);
            model_unpack_group(ba, code, expect);
            TASK_CHECK(expect[0] == triple[0] && expect[1] == triple[1] &&
                       expect[2] == triple[2], "model pack/unpack keeps sample order");

            for (offset = 0; offset < 8; offset++)
            {
                model_bs_t wbs;
                bs_t prod_bs;
                L12_scale_info sci[1];
                /* Plain bands carry group_size separate codewords, one per
                   moment; the grouped band carries one codeword per group. */
                int plain_v_before[4][3], plain_v_after[4][3];
                int group_pos[4];
                int total_bits;

                model_bs_init(&wbs, bytes, (int)sizeof(bytes));

                /* Pad with `offset` zero bits so the production read, which
                   starts at bit offset 0..7, crosses byte boundaries. */
                model_put(&wbs, 0, offset);

                /* Production walks granule groups first and, inside each
                   group, the slots.  Plain slots carry three codewords per
                   group; the grouped slot carries one codeword per group. */
                for (j = 0; j < 4; j++)
                {
                    for (m = 0; m < 3; m++)
                    {
                        plain_v_before[j][m] = (d0 + j + m) & ((1 << plain_ba_before) - 1);
                        model_put(&wbs, (uint32_t)plain_v_before[j][m], plain_ba_before);
                    }
                    group_pos[j] = wbs.pos;
                    model_put(&wbs, code, nbits);
                    for (m = 0; m < 3; m++)
                    {
                        plain_v_after[j][m]  = (d2 + j + m + 1) & ((1 << plain_ba_after) - 1);
                        model_put(&wbs, (uint32_t)plain_v_after[j][m], plain_ba_after);
                    }
                }
                total_bits = offset + 4 * (3 * plain_ba_before + nbits + 3 * plain_ba_after);
                TASK_CHECK(wbs.pos == total_bits, "planned stream bit count");

                /* Bit-level position check: every grouped codeword is found
                   at its planned absolute, possibly byte-crossing position. */
                for (j = 0; j < 4; j++)
                {
                    snprintf(msg, sizeof(msg),
                             "level %d codeword bit position group %d offset %d", ba, j, offset);
                    TASK_CHECK(model_get(bytes, group_pos[j], nbits) == code, msg);
                }

                memset(sci, 0, sizeof(sci));
                sci->total_bands = 3;
                sci->stereo_bands = 0;
                sci->bitalloc[0] = (uint8_t)plain_ba_before;   /* slot 0: left band 0 */
                sci->bitalloc[2] = (uint8_t)ba;                /* slot 2: left band 1 */
                sci->bitalloc[4] = (uint8_t)plain_ba_after;    /* slot 4: left band 2 */

                memset(grbuf, 0, sizeof(grbuf));
                prod_bs.buf = bytes;
                prod_bs.limit = (int)sizeof(bytes) * 8;
                prod_bs.pos = offset;
                TASK_CHECK(L12_dequantize_granule(grbuf, &prod_bs, sci, 3) == 12,
                           "granule returns 12 floats");
                snprintf(msg, sizeof(msg),
                         "level %d production consumed exactly the planned bits at offset %d",
                         ba, offset);
                TASK_CHECK(prod_bs.pos == total_bits, msg);

                /* Grouped band: sample order and centered integer values. */
                for (j = 0; j < 4; j++)
                for (k = 0; k < 3; k++)
                {
                    got[k] = (int)grbuf[model_slot_offset(2, j, k)];
                    snprintf(msg, sizeof(msg),
                             "level %d grouped sample band1 group %d digit %d", ba, j, k);
                    TASK_CHECK(got[k] == triple[k], msg);
                }

                /* Plain neighbors, independently centered, moment by moment. */
                for (j = 0; j < 4; j++)
                for (m = 0; m < 3; m++)
                {
                    int half_before = (1 << (plain_ba_before - 1)) - 1;
                    int half_after  = (1 << (plain_ba_after - 1)) - 1;
                    TASK_CHECK((int)grbuf[model_slot_offset(0, j, m)] ==
                               plain_v_before[j][m] - half_before,
                               "plain sample before the grouped codeword");
                    TASK_CHECK((int)grbuf[model_slot_offset(4, j, m)] ==
                               plain_v_after[j][m] - half_after,
                               "plain sample after the grouped codeword");
                }

                /* The mono stream has no right-channel codes: every right
                   slot, and every band beyond the plan, stays exactly zero. */
                for (k = 0; k < 576; k++)
                    TASK_CHECK(grbuf[576 + k] == 0.0f, "mono right channel subband stays zero");
                for (k = 18 * 3; k < 576; k++)
                    TASK_CHECK(grbuf[k] == 0.0f, "unallocated subband region stays zero");
            }
        }
    }
    return 1;
}

/* ================================================================== */
/* scfsi group                                                         */
/* ================================================================== */

#define SCFSI_BANDS 4

typedef struct
{
    int ba;
    int field[3];      /* 6 bit indices in transmission order (serial) */
    int indices[3];    /* final per-segment indices after SCFSI reuse */
} scfsi_slot_plan_t;

static int check_scfsi(void)
{
    /* Rotating field indices so 0, 1, 2, 62 and 63 are all covered. */
    static const int rotating_index[8] = { 0, 7, 1, 31, 2, 45, 62, 63 };
    static const int mono_ba[SCFSI_BANDS] = { 18, 5, 19, 7 };       /* grouped + plain */
    static const int joint_left_ba[SCFSI_BANDS] = { 17, 6, 18, 4 };
    static const int joint_right_ba[SCFSI_BANDS] = { 18, 7, 0, 0 };
    int scenario;

    for (scenario = 0; scenario < 2; scenario++)
    {
        int joint = scenario;
        int stereo_bands = joint ? 2 : 0;
        int pattern, field_serial = 0;
        int covered[64] = { 0 };
        char msg[192];

        for (pattern = 0; pattern < 4; pattern++)
        {
            uint8_t bytes[512];       /* 6 bit scalefactor fields only */
            uint8_t code_bytes[64];   /* 2 bit SCFSI codes, verified separately */
            model_bs_t wbs, cbs;
            bs_t prod_bs;
            L12_scale_info sci[1];
            scfsi_slot_plan_t plan[64];
            double model_scf[64][3];
            float grbuf_template[2 * 576];
            float grbuf[2 * 576];
            uint8_t payload[512];
            model_bs_t pbs;
            int expect_fields = 0, expect_payload_bits = 0;
            int slot, b, ch, seg, f, j, k, moment;

            model_bs_init(&wbs, bytes, (int)sizeof(bytes));
            model_bs_init(&cbs, code_bytes, (int)sizeof(code_bytes));
            memset(sci, 0, sizeof(sci));
            memset(plan, 0, sizeof(plan));
            memset(model_scf, 0, sizeof(model_scf));
            sci->total_bands = SCFSI_BANDS;
            sci->stereo_bands = (uint8_t)stereo_bands;

            /* 1. SCFSI 2 bit codes.  In the real stream these are consumed by
               L12_read_scale_info before the scalefactor fields, so they are
               written to their own buffer and read back independently below. */
            for (slot = 0; slot < 2 * SCFSI_BANDS; slot++)
            {
                int code_pos;
                b = slot >> 1;
                ch = slot & 1;
                if (ch == 1 && b >= stereo_bands)
                {
                    sci->bitalloc[slot] = 0;
                    sci->scfcod[slot] = 6;
                    continue;
                }
                sci->bitalloc[slot] = (uint8_t)(joint ?
                    (ch == 0 ? joint_left_ba[b] : joint_right_ba[b]) : mono_ba[b]);
                sci->scfcod[slot] = (uint8_t)pattern;
                code_pos = cbs.pos;
                model_put(&cbs, (uint32_t)pattern, 2);
                TASK_CHECK((int)model_get(code_bytes, code_pos, 2) == pattern,
                           "SCFSI 2 bit code read back at its planned position");
            }
            TASK_CHECK(cbs.pos == 2 * (joint ? SCFSI_BANDS + stereo_bands : SCFSI_BANDS),
                       "exactly one 2 bit SCFSI code per allocated slot");

            /* 2. The 6 bit fields, with deliberately different indices. */
            for (slot = 0; slot < 2 * SCFSI_BANDS; slot++)
            {
                int ba = sci->bitalloc[slot];
                if (ba == 0)
                    continue;
                for (f = 0; f < model_scfsi_reads[pattern]; f++)
                {
                    int idx = rotating_index[field_serial++ % 8];
                    int field_pos = wbs.pos;
                    plan[slot].ba = ba;
                    plan[slot].field[f] = idx;
                    covered[idx] = 1;
                    model_put(&wbs, (uint32_t)idx, 6);
                    expect_fields++;
                    TASK_CHECK((int)model_get(bytes, field_pos, 6) == idx,
                               "scalefactor 6 bit field read back at planned position");
                }
                for (seg = 0; seg < 3; seg++)
                {
                    int src = model_scfsi_pick[pattern][seg];
                    plan[slot].indices[seg] = plan[slot].field[src];
                    model_scf[slot][seg] =
                        model_scale_value(ba, plan[slot].indices[seg]);
                }
            }

            /* 3. Real scalefactor reader over the independent field stream. */
            prod_bs.buf = bytes;
            prod_bs.limit = (int)sizeof(bytes) * 8;
            prod_bs.pos = 0;
            L12_read_scalefactors(&prod_bs, sci->bitalloc, sci->scfcod,
                                  SCFSI_BANDS * 2, sci->scf);
            snprintf(msg, sizeof(msg),
                     "SCFSI pattern %d read exactly %d 6-bit fields (joint=%d)",
                     pattern, expect_fields, joint);
            TASK_CHECK(prod_bs.pos == expect_fields * 6, msg);

            for (slot = 0; slot < 2 * SCFSI_BANDS; slot++)
            for (seg = 0; seg < 3; seg++)
            {
                b = slot >> 1;
                ch = slot & 1;
                if (ch == 1 && b >= stereo_bands)
                {
                    TASK_CHECK(sci->scf[3 * slot + seg] == 0.0f,
                               "shared-codeword band keeps a silent right-channel scale slot");
                    continue;
                }
                snprintf(msg, sizeof(msg),
                         "scale ch=%d band=%d seg=%d pattern=%d matches double model",
                         ch, b, seg, pattern);
                TASK_CHECK(scf_float_close(model_scf[slot][seg],
                                           sci->scf[3 * slot + seg]), msg);
            }

            if (joint)
            {
                /* Left and right keep independent scale sets inside the
                   boundary: the right scale must never be a copy of left. */
                for (b = 0; b < stereo_bands; b++)
                for (seg = 0; seg < 3; seg++)
                {
                    int left = 2 * b, right = 2 * b + 1;
                    snprintf(msg, sizeof(msg),
                             "joint band %d keeps distinct left/right scales seg %d", b, seg);
                    TASK_CHECK(plan[left].indices[seg] != plan[right].indices[seg] &&
                               sci->scf[3 * left + seg] != sci->scf[3 * right + seg], msg);
                }
            }

            /* 4. Payload: grouped and plain codewords, then real dequant. */
            model_bs_init(&pbs, payload, (int)sizeof(payload));
            {
                /* Production walks granule groups first, then slots; plain
                   slots emit three codewords per group, grouped slots one. */
                int payload_offset[64][4][3];
                memset(payload_offset, 0, sizeof(payload_offset));
                for (j = 0; j < 4; j++)
                for (slot = 0; slot < 2 * SCFSI_BANDS; slot++)
                {
                    int ba = sci->bitalloc[slot];
                    if (ba == 0)
                        continue;
                    if (ba >= 17)
                    {
                        int mod = grouped_modulus(ba);
                        int triple[3];
                        payload_offset[slot][j][0] = pbs.pos;
                        for (k = 0; k < 3; k++)
                            triple[k] = (slot * 5 + j * 11 + k * 3) % mod - mod / 2;
                        model_put(&pbs, model_pack_group(ba, triple),
                                  grouped_code_bits(ba));
                        expect_payload_bits += grouped_code_bits(ba);
                    } else
                    {
                        for (k = 0; k < 3; k++)
                        {
                            int v = (slot * 3 + j * 7 + k * 5) & ((1 << ba) - 1);
                            payload_offset[slot][j][k] = pbs.pos;
                            model_put(&pbs, (uint32_t)v, ba);
                            expect_payload_bits += ba;
                        }
                    }
                }

                memset(grbuf_template, 0, sizeof(grbuf_template));
                prod_bs.buf = payload;
                prod_bs.limit = (int)sizeof(payload) * 8;
                prod_bs.pos = 0;
                L12_dequantize_granule(grbuf_template, &prod_bs, sci, 3);
                TASK_CHECK(prod_bs.pos == expect_payload_bits,
                           "grouped and plain payload consumption is exact");

                /* Apply each of the three scale segments independently and
                   rebuild every subband value from the model. */
                for (seg = 0; seg < 3; seg++)
                {
                    memcpy(grbuf, grbuf_template, sizeof(grbuf));
                    L12_apply_scf_384(sci, sci->scf + seg, grbuf);
                    for (slot = 0; slot < 2 * SCFSI_BANDS; slot++)
                    {
                        int ba = sci->bitalloc[slot];
                        b = slot >> 1;
                        ch = slot & 1;
                        if (ch == 1 && b >= stereo_bands)
                        {
                            for (moment = 0; moment < 12; moment++)
                                TASK_CHECK(grbuf[ch * 576 + 18 * b + moment] == 0.0f,
                                           "right channel outside joint boundary rebuilds to zero");
                            continue;
                        }
                        for (j = 0; j < 4; j++)
                        for (k = 0; k < 3; k++)
                        {
                            int moment = 3 * j + k;
                            double raw;
                            if (ba >= 17)
                            {
                                int digits[3];
                                uint32_t code = model_get(payload,
                                    payload_offset[slot][j][0], grouped_code_bits(ba));
                                model_unpack_group(ba, code, digits);
                                raw = digits[k];
                            } else
                            {
                                uint32_t v = model_get(payload,
                                    payload_offset[slot][j][k], ba);
                                raw = (double)v - (double)((1 << (ba - 1)) - 1);
                            }
                            snprintf(msg, sizeof(msg),
                                     "rebuilt subband slot=%d seg=%d moment=%d pattern=%d joint=%d",
                                     slot, seg, moment, pattern, joint);
                            TASK_CHECK(scf_float_close(raw * model_scf[slot][seg],
                                                       grbuf[ch * 576 + 18 * b + moment]),
                                       msg);
                        }
                    }
                }
            }
        }

        TASK_CHECK(covered[0] && covered[1] && covered[2] && covered[62] && covered[63],
                   "scalefactor indices 0, 1, 2, 62 and 63 all covered");
    }
    return 1;
}

/* ================================================================== */
/* frames group                                                        */
/* ================================================================== */

#define FRAME_PLAN_COUNT 6

typedef struct
{
    int stereo_mode;     /* MODE_MONO, MODE_JOINT_STEREO or 0 (plain stereo) */
    int mode_ext;
    int bitrate_idx;
    int sample_rate_idx;
    int grouped_band;
    int plain_band;
} frame_plan_t;

typedef struct
{
    model_alloc_t alloc;
    uint8_t  bitalloc[64];
    uint8_t  ba_code[64];
    uint8_t  scfcod[64];
    int      ba_offset[64], scfcod_offset[64];
    int      scf_index[64][3];
    int      scf_field[64][3]; /* transmitted 6 bit indices in serial order */
    int      scf_field_pos[256], scf_field_idx[256], scf_field_count;
    double   scf_value[64][3];
    int      payload_offset[3][64][4][3];
    int      payload_int[3][64][12];  /* each payload round reuses the 12 moments */
    int      ba_bits, scfsi_bits, scale_bits, payload_bits, pad_bits;
} frame_model_t;

static void frame_build_plan(frame_plan_t *plans)
{
    memset(plans, 0, sizeof(frame_plan_t) * FRAME_PLAN_COUNT);
    plans[0].stereo_mode = MODE_MONO;          plans[0].bitrate_idx = 11;
    plans[0].sample_rate_idx = 0;              plans[0].grouped_band = 1;
    plans[0].plain_band = 4;
    plans[1].stereo_mode = MODE_MONO;          plans[1].bitrate_idx = 12;
    plans[1].sample_rate_idx = 2;              plans[1].grouped_band = 2;
    plans[1].plain_band = 5;
    plans[2].stereo_mode = 0;                  plans[2].bitrate_idx = 12;
    plans[2].sample_rate_idx = 0;              plans[2].grouped_band = 1;
    plans[2].plain_band = 4;                   /* plain stereo */
    plans[3].stereo_mode = MODE_JOINT_STEREO;  plans[3].mode_ext = 3;
    plans[3].bitrate_idx = 12;                 plans[3].sample_rate_idx = 0;
    plans[3].grouped_band = 1;                 plans[3].plain_band = 6;
    plans[4].stereo_mode = MODE_JOINT_STEREO;  plans[4].mode_ext = 0;
    plans[4].bitrate_idx = 12;                 plans[4].sample_rate_idx = 0;
    plans[4].grouped_band = 2;                 plans[4].plain_band = 5;
    plans[5].stereo_mode = 0;                  plans[5].bitrate_idx = 11;
    plans[5].sample_rate_idx = 0;              plans[5].grouped_band = 3;
    plans[5].plain_band = 1;                   /* stereo again */
}

static unsigned frame_byte_size(const frame_plan_t *p)
{
    static const unsigned hz[3] = { 44100, 48000, 32000 };
    unsigned kbps = model_mpeg1_l2_bitrates[p->bitrate_idx];
    return 1152u * kbps * 125u / hz[p->sample_rate_idx];
}

static void frame_write_header(const frame_plan_t *p, uint8_t hdr[4])
{
    hdr[0] = 0xff;
    hdr[1] = 0xfd;                                   /* MPEG1, Layer II, no CRC */
    hdr[2] = (uint8_t)((p->bitrate_idx << 4) | (p->sample_rate_idx << 2));
    hdr[3] = (uint8_t)((p->stereo_mode << 6) | (p->mode_ext << 4));
}

static int build_frame(const frame_plan_t *p, uint8_t *frame, unsigned frame_cap,
                       frame_model_t *m, unsigned *frame_size_out)
{
    uint8_t hdr[4];
    unsigned frame_bytes = frame_byte_size(p);
    model_bs_t wbs;
    int slot, b, ch, g, seg, f, k;
    char msg[192];
    static const int field_rotation[8] = { 0, 17, 1, 33, 2, 48, 62, 63 };
    int field_serial = 0, pattern_seen[4] = { 0, 0, 0, 0 };

    TASK_CHECK(frame_bytes <= frame_cap, "frame buffer large enough");
    memset(frame, 0, frame_bytes);
    memset(m, 0, sizeof(*m));
    frame_write_header(p, hdr);
    frame_write_header(p, frame);
    model_alloc_plan(p->stereo_mode, p->mode_ext, p->bitrate_idx,
                     p->sample_rate_idx, &m->alloc);

    /* Cross-check the independent allocation plan against the production
       format table accessor. */
    {
        L12_scale_info prod_sci[1];
        const L12_subband_alloc_t *prod = L12_subband_alloc_table(hdr, prod_sci);
        TASK_CHECK((int)prod_sci->total_bands == m->alloc.total_bands,
                   "model total bands match production table");
        TASK_CHECK((int)prod_sci->stereo_bands == m->alloc.stereo_bands,
                   "model stereo bands match production table");
        for (g = 0; g < m->alloc.ngroups; g++)
        {
            TASK_CHECK(prod[g].band_count == m->alloc.groups[g].band_count,
                       "allocation group band count matches table");
            TASK_CHECK(prod[g].code_tab_width == m->alloc.groups[g].code_tab_width,
                       "allocation group code width matches table");
        }
    }

    model_bs_init(&wbs, frame + 4, (int)(frame_bytes - 4));

    /* 1. bit allocation, encoded through the table in both model and stream.
       Only a small explicit band set is active so the planned payload always
       fits the header-derived frame size with padding; every other band is a
       valid zero allocation. */
    m->ba_bits = 0;
    for (b = 0; b < m->alloc.total_bands; b++)
    {
        int width = model_ba_bits(&m->alloc, b);
        for (ch = 0; ch < 2; ch++)
        {
            int code, ba;
            slot = 2 * b + ch;
            if (ch == 1 && b >= m->alloc.stereo_bands)
            {
                m->bitalloc[slot] = 0;
                m->ba_code[slot] = 0;
                m->scfcod[slot] = 6;
                continue;
            }
            if (b == p->grouped_band)
                code = model_find_code(&m->alloc, b, 1);
            else if (b == p->plain_band ||
                     (b >= 0 && b <= 3 && b != p->grouped_band))
                code = model_find_code(&m->alloc, b, 0);
            else
                code = 0;   /* zero bit allocation */
            TASK_CHECK(code >= 0, "planned grouped/plain allocation is encodable");
            ba = model_decode_ba(&m->alloc, b, (uint32_t)code);
            m->ba_offset[slot] = wbs.pos;
            model_put(&wbs, (uint32_t)code, width);
            m->ba_bits += width;
            m->ba_code[slot] = (uint8_t)code;
            m->bitalloc[slot] = (uint8_t)ba;
        }
    }
    TASK_CHECK(model_find_code(&m->alloc, p->grouped_band, 1) >= 0,
               "frame contains grouped quantization");
    TASK_CHECK(model_find_code(&m->alloc, p->plain_band, 0) >= 0,
               "frame contains plain quantization");

    /* Parse-phase bit allocation.  In joint stereo the exterior right slots
       keep the left band's bitalloc until after the scalefactors are read
       (production zeroes them only at the end of L12_read_scale_info), so the
       parser consumes SCFSI codes and scalefactor fields for them even though
       they carry no payload and are discarded afterwards. */
    int parse_ba[64];
    for (slot = 0; slot < 2 * m->alloc.total_bands; slot++)
    {
        int band = slot >> 1, ch = slot & 1;
        /* Stale exterior-right slots exist only in stereo modes. */
        if (m->alloc.stereo_bands > 0 && ch == 1 && band >= m->alloc.stereo_bands)
            parse_ba[slot] = m->bitalloc[2 * band];
        else
            parse_ba[slot] = m->bitalloc[slot];
    }

    /* 2. SCFSI codes, one per parse-active slot.  Every pattern 0..3 appears
       across the genuinely allocated (payload-bearing) slots. */
    m->scfsi_bits = 0;
    {
        int allocated_serial = 0;
        for (slot = 0; slot < 2 * m->alloc.total_bands; slot++)
        {
            if (parse_ba[slot] == 0)
            {
                m->scfcod[slot] = 6;
                continue;
            }
            m->scfcod[slot] = (uint8_t)(allocated_serial++ & 3);
            if (m->bitalloc[slot] != 0)
                pattern_seen[m->scfcod[slot]] = 1;
            m->scfcod_offset[slot] = wbs.pos;
            model_put(&wbs, m->scfcod[slot], 2);
            m->scfsi_bits += 2;
        }
    }
    TASK_CHECK(pattern_seen[0] && pattern_seen[1] && pattern_seen[2] &&
               pattern_seen[3], "all four SCFSI reuse patterns present");

    /* 3. scalefactor 6 bit fields plus double precision expected scales.
       Stale exterior-right fields are written (they consume bits) but carry
       no expected value since the slot is zeroed before the payload. */
    m->scf_field_count = 0;
    m->scale_bits = 0;
    for (slot = 0; slot < 2 * m->alloc.total_bands; slot++)
    {
        int pattern;
        if (parse_ba[slot] == 0)
            continue;
        pattern = m->scfcod[slot];
        for (f = 0; f < model_scfsi_reads[pattern]; f++)
        {
            int idx = field_rotation[field_serial++ % 8];
            m->scf_field_pos[m->scf_field_count] = wbs.pos;
            m->scf_field_idx[m->scf_field_count] = idx;
            m->scf_field_count++;
            m->scf_field[slot][f] = idx;
            model_put(&wbs, (uint32_t)idx, 6);
            m->scale_bits += 6;
        }
        /* Stale exterior-right slots still parse scalefactors; production
           later copies the left raw samples into the right channel and scales
           them by these retained values. */
        for (seg = 0; seg < 3; seg++)
        {
            int src = model_scfsi_pick[pattern][seg];
            m->scf_index[slot][seg] = m->scf_field[slot][src];
            m->scf_value[slot][seg] =
                model_scale_value(parse_ba[slot], m->scf_index[slot][seg]);
        }
    }
    {
        int need[5] = { 0, 1, 2, 62, 63 }, n, found;
        for (n = 0; n < 5; n++)
        {
            found = 0;
            for (f = 0; f < m->scf_field_count; f++)
                if (m->scf_field_idx[f] == need[n])
                    found = 1;
            TASK_CHECK(found, "frame covers scalefactor indices 0,1,2,62,63");
        }
    }

    /* 4. payload samples.  A frame carries three full payload rounds (the
       production decoder invokes L12_dequantize_granule three times, once per
       384 sample block).  Within a round production walks granule groups
       first, then slots; plain slots emit three codewords per group, grouped
       slots one. */
    m->payload_bits = 0;
    for (int round = 0; round < 3; round++)
    {
        for (g = 0; g < 4; g++)
        {
            for (slot = 0; slot < 2 * m->alloc.total_bands; slot++)
            {
                int ba = m->bitalloc[slot];
                if (ba == 0)
                    continue;
                if (ba >= 17)
                {
                    int mod = grouped_modulus(ba), triple[3];
                    for (k = 0; k < 3; k++)
                    {
                        triple[k] = (slot * 5 + round * 13 + g * 11 + k * 3) % mod - mod / 2;
                        m->payload_int[round][slot][3 * g + k] = triple[k];
                    }
                    m->payload_offset[round][slot][g][0] = wbs.pos;
                    model_put(&wbs, model_pack_group(ba, triple), grouped_code_bits(ba));
                    m->payload_bits += grouped_code_bits(ba);
                } else
                {
                    int half = (1 << (ba - 1)) - 1;
                    for (k = 0; k < 3; k++)
                    {
                        int v = (slot * 3 + round * 13 + g * 7 + k * 5) & ((1 << ba) - 1);
                        m->payload_int[round][slot][3 * g + k] = v - half;
                        m->payload_offset[round][slot][g][k] = wbs.pos;
                        model_put(&wbs, (uint32_t)v, ba);
                        m->payload_bits += ba;
                    }
                }
            }
        }
    }

    m->pad_bits = (int)(frame_bytes * 8u) -
                  (32 + m->ba_bits + m->scfsi_bits + m->scale_bits + m->payload_bits);
    snprintf(msg, sizeof(msg), "planned frame fits (pad_bits=%d)", m->pad_bits);
    TASK_CHECK(m->pad_bits >= 0, msg);
    TASK_CHECK(wbs.pos == m->ba_bits + m->scfsi_bits + m->scale_bits + m->payload_bits,
               "model writer position matches bit budget");
    *frame_size_out = frame_bytes;
    return 1;
}

/* Independent read-back of every planned field from the finished bytes. */
static void model_readback_check(const frame_plan_t *p, const uint8_t *frame,
                                 const frame_model_t *m)
{
    const uint8_t *body = frame + 4;
    int slot, f, b, ch, g, k;
    char msg[192];

    /* Header, parsed bit-by-bit without production macros. */
    TASK_CHECK(frame[0] == 0xff && (frame[1] & 0xf0) == 0xf0, "frame sync word");
    TASK_CHECK(((frame[1] >> 3) & 1) == 1 && ((frame[1] >> 1) & 3) == 2,
               "header says MPEG1 Layer II");
    TASK_CHECK((frame[1] & 1) == 1, "synthetic frame carries no CRC word");
    TASK_CHECK((frame[2] >> 4) == p->bitrate_idx, "header bitrate index read back");
    TASK_CHECK(((frame[2] >> 2) & 3) == p->sample_rate_idx, "header sample rate read back");
    TASK_CHECK((frame[3] >> 6) == p->stereo_mode, "header stereo mode read back");
    TASK_CHECK(((frame[3] >> 4) & 3) == p->mode_ext, "header joint mode extension read back");

    for (slot = 0; slot < 2 * m->alloc.total_bands; slot++)
    {
        b = slot >> 1;
        ch = slot & 1;
        if (ch == 1 && b >= m->alloc.stereo_bands)
            continue;
        TASK_CHECK((int)model_get(body, m->ba_offset[slot],
                                  model_ba_bits(&m->alloc, b)) == m->ba_code[slot],
                   "bit allocation code read back at planned position");
        if (m->bitalloc[slot])
            TASK_CHECK((int)model_get(body, m->scfcod_offset[slot], 2) == m->scfcod[slot],
                       "SCFSI code read back at planned position");
    }
    for (f = 0; f < m->scf_field_count; f++)
    {
        snprintf(msg, sizeof(msg), "scalefactor field %d read back", f);
        TASK_CHECK((int)model_get(body, m->scf_field_pos[f], 6) == m->scf_field_idx[f], msg);
    }
    for (slot = 0; slot < 2 * m->alloc.total_bands; slot++)
    {
        int ba = m->bitalloc[slot];
        if (ba == 0)
            continue;
        for (int round = 0; round < 3; round++)
        for (g = 0; g < 4; g++)
        {
            if (ba >= 17)
            {
                int digits[3];
                uint32_t code = model_get(body, m->payload_offset[round][slot][g][0],
                                          grouped_code_bits(ba));
                model_unpack_group(ba, code, digits);
                for (k = 0; k < 3; k++)
                {
                    snprintf(msg, sizeof(msg), "grouped payload slot %d round %d group %d",
                             slot, round, g);
                    TASK_CHECK(digits[k] == m->payload_int[round][slot][3 * g + k], msg);
                }
            } else
            {
                int half = (1 << (ba - 1)) - 1;
                for (k = 0; k < 3; k++)
                {
                    uint32_t v = model_get(body, m->payload_offset[round][slot][g][k], ba);
                    snprintf(msg, sizeof(msg),
                             "plain payload slot %d round %d group %d moment %d",
                             slot, round, g, k);
                    TASK_CHECK((int)v - half == m->payload_int[round][slot][3 * g + k],
                               msg);
                }
            }
        }
    }
}

static void check_one_frame(const frame_plan_t *p, const uint8_t *frame,
                            unsigned frame_bytes, const frame_model_t *m,
                            int frame_index)
{
    mp3dec_t dec[1];
    mp3dec_frame_info_t info[1];
    static short pcm[1152 * 2 + 8];
    short *guard = pcm + 1152 * 2;
    int samples, n, nz, k, ch, b, moment, granule;
    char msg[192];

    model_readback_check(p, frame, m);
    for (k = 0; k < 8; k++)
        guard[k] = (short)0xA5A5;

    mp3dec_init(dec);
    layer2_obs_reset();
    samples = mp3dec_decode_frame(dec, frame, (int)frame_bytes, pcm, info);
    snprintf(msg, sizeof(msg), "frame %d accepted by public decoder", frame_index);
    TASK_CHECK(samples == 1152, msg);
    TASK_CHECK(info->frame_bytes == (int)frame_bytes, "consumed bytes equal planned frame size");
    TASK_CHECK(info->frame_offset == 0, "frame starts at offset zero");
    TASK_CHECK(info->layer == 2, "public info reports Layer II");
    TASK_CHECK(info->channels == (p->stereo_mode == MODE_MONO ? 1 : 2),
               "public info channel count");
    TASK_CHECK((unsigned)info->hz == model_sample_rates[p->sample_rate_idx],
               "public info sample rate");
    TASK_CHECK((unsigned)info->bitrate_kbps == model_mpeg1_l2_bitrates[p->bitrate_idx],
               "public info bitrate");
    n = samples * info->channels;
    for (nz = 0, k = 0; k < n; k++)
        if (pcm[k] != 0)
            nz = 1;
    TASK_CHECK(nz, "decoded PCM is not silent");
    for (k = 0; k < 8; k++)
        TASK_CHECK(guard[k] == (short)0xA5A5, "PCM output guard region untouched");

    TASK_CHECK(layer2_obs_count() == 3, "three pre-synth observation records");
    for (granule = 0; granule < 3; granule++)
    {
        const layer2_obs_t *obs = layer2_obs_get(granule);
        TASK_CHECK(obs != 0, "observation record available");
        TASK_CHECK(obs->total_bands == m->alloc.total_bands &&
                   obs->stereo_bands == m->alloc.stereo_bands,
                   "observation allocation shape");
        for (ch = 0; ch < info->channels; ch++)
        for (b = 0; b < m->alloc.total_bands; b++)
        for (moment = 0; moment < 12; moment++)
        {
            int slot = 2 * b + ch;
            int have = (ch == 0) || (b < m->alloc.stereo_bands);
            if (!have)
            {
                /* Exterior joint-stereo right: production copies the left raw
                   samples into the right channel and scales them with the
                   stale, separately parsed right scalefactor.  With no left
                   codeword the raw values are zero and stay zero. */
                if (m->bitalloc[2 * b] == 0)
                {
                    TASK_CHECK(obs->values[ch][b][moment] == 0.0f,
                               "exterior joint-stereo right channel zero without a left codeword");
                } else
                {
                    double expected = (double)m->payload_int[granule][2 * b][moment] *
                                      m->scf_value[slot][granule];
                    snprintf(msg, sizeof(msg),
                             "exterior right reuse frame=%d granule=%d band=%d moment=%d",
                             frame_index, granule, b, moment);
                    TASK_CHECK(scf_float_close(expected, obs->values[ch][b][moment]), msg);
                }
                continue;
            }
            {
                /* L12_apply_scf_384 scales every moment of one 384 sample
                   block by the same block (granule) scalefactor. */
                double expected = (double)m->payload_int[granule][slot][moment] *
                                  m->scf_value[slot][granule];
                snprintf(msg, sizeof(msg),
                         "pre-synth frame=%d granule=%d ch=%d band=%d moment=%d",
                         frame_index, granule, ch, b, moment);
                TASK_CHECK(scf_float_close(expected, obs->values[ch][b][moment]), msg);
            }
        }
    }
}

/* Same-header stereo chain: exercises the production frame-to-frame matching
   path (hdr_compare in mp3dec_decode_frame), not only exact-size acceptance. */
static void check_stereo_chain(const frame_plan_t *p)
{
    unsigned frame_bytes = frame_byte_size(p);
    uint8_t *stream = (uint8_t *)malloc(frame_bytes * 3);
    frame_model_t models[3];
    mp3dec_t dec[1];
    mp3dec_frame_info_t info[1];
    static short pcm[1152 * 2];
    int g, total_samples = 0;
    char msg[160];

    TASK_CHECK(stream != 0, "stereo chain allocation");
    for (g = 0; g < 3; g++)
    {
        unsigned used;
        build_frame(p, stream + g * frame_bytes, frame_bytes, &models[g], &used);
    }
    mp3dec_init(dec);
    for (g = 0; g < 3; g++)
    {
        int samples = mp3dec_decode_frame(dec, stream + g * frame_bytes,
                                         (int)(frame_bytes * (3 - g)), pcm, info);
        snprintf(msg, sizeof(msg), "chained stereo frame %d matched and decoded", g);
        TASK_CHECK(samples == 1152 && info->frame_bytes == (int)frame_bytes, msg);
        total_samples += samples;
    }
    TASK_CHECK(total_samples == 3 * 1152, "continuous stereo chain yields 3456 samples");
    free(stream);
}

static int check_frames(void)
{
    frame_plan_t plans[FRAME_PLAN_COUNT];
    frame_model_t models[FRAME_PLAN_COUNT];
    unsigned frame_sizes[FRAME_PLAN_COUNT];
    unsigned total = 0, cap = 0, fi;
    uint8_t *stream, *cursor;

    frame_build_plan(plans);
    for (fi = 0; fi < FRAME_PLAN_COUNT; fi++)
    {
        frame_sizes[fi] = frame_byte_size(&plans[fi]);
        cap += frame_sizes[fi];
    }
    stream = (uint8_t *)malloc(cap);
    TASK_CHECK(stream != 0, "frame stream allocation");
    cursor = stream;

    /* One contiguous stream of planned Layer II frames. */
    for (fi = 0; fi < FRAME_PLAN_COUNT; fi++)
    {
        unsigned used;
        build_frame(&plans[fi], cursor, cap - (unsigned)(cursor - stream),
                    &models[fi], &used);
        cursor += used;
        total += used;
    }

    cursor = stream;
    for (fi = 0; fi < FRAME_PLAN_COUNT; fi++)
    {
        /* Exact-length slice: the production parser accepts a buffer that is
           precisely one frame even though modes change between frames. */
        check_one_frame(&plans[fi], cursor, frame_sizes[fi], &models[fi], (int)fi);
        cursor += frame_sizes[fi];
    }
    TASK_CHECK((unsigned)(cursor - stream) == total, "contiguous stream walked exactly once");
    free(stream);

    check_stereo_chain(&plans[2]);
    return 1;
}

/* ================================================================== */

int task_group(const char *group)
{
    if (!strcmp(group, "digits"))
        return check_digits();
    if (!strcmp(group, "scfsi"))
        return check_scfsi();
    if (!strcmp(group, "frames"))
        return check_frames();
    TASK_CHECK(0, "unknown test group");
    return 0;
}
