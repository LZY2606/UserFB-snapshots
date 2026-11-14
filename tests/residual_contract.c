/*
 * residual_contract.c - independent bitstream-driven regression contract
 *
 * Every FLAC native stream decoded below is synthesized in this file:
 *  - an MSB-first bit writer emits all subframe/frame bits,
 *  - CRC-8 (x^8+x^2+x+1) and CRC-16 (x^16+x^15+x^2+1) are implemented
 *    here, not taken from libFLAC,
 *  - expected PCM is reconstructed with wide integer recurrences written
 *    here (no production restore_signal / fixed helpers),
 *  - the production decoder is only ever a consumer of these bytes and a
 *    source of independently checked output.
 *
 * Scope: 16/24 bit, two channels, blocks of 64..256 samples. No corrupted
 * inputs, no overflow-seeking, no external media.
 */

#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"

/* ------------------------------------------------------------------ */
/* limits and types                                                    */
/* ------------------------------------------------------------------ */

#define MAX_BLOCK 256u
#define MAX_PARTS 16u
#define MAX_FRAMES 16u
#define MAX_STREAM_BYTES (512u * 1024u)
#define SAMPLE_RATE 48000u

#define ASSIGN_INDEPENDENT 0u
#define ASSIGN_LEFT_SIDE 1u
#define ASSIGN_RIGHT_SIDE 2u
#define ASSIGN_MID_SIDE 3u

typedef struct {
    uint32_t start_bit;
    uint32_t end_bit;
    uint32_t count;
} PartRec;

typedef struct {
    int type;                 /* 2 == fixed, 3 == LPC */
    uint32_t order;
    uint32_t bps;             /* effective subframe bits per sample */
    uint32_t wasted;
    uint32_t rice2;           /* 0 = PARTITIONED_RICE, 1 = RICE2 */
    uint32_t part_order;
    uint32_t param[MAX_PARTS];
    uint32_t raw[MAX_PARTS];  /* escape width, 0 only when not escaped */
    int32_t warm[32];
    int32_t qlp[32];
    uint32_t qlp_prec;
    int shift;
    int32_t resid[MAX_BLOCK];
    int32_t signal[MAX_BLOCK];
    PartRec parts[MAX_PARTS];
} SubPlan;

typedef struct {
    uint32_t blocksize;
    uint32_t bps;
    uint32_t assign;
    uint64_t sample_number;
    SubPlan sub[2];
    int32_t want[2][MAX_BLOCK];
    size_t frame_start;
    size_t frame_end;
} FramePlan;

typedef struct {
    FramePlan frames[MAX_FRAMES];
    uint32_t nframes;
    uint32_t bps;
    uint64_t total_samples;
    uint8_t bytes[MAX_STREAM_BYTES];
    size_t len;
} StreamPlan;

typedef struct {
    int escape;
    uint32_t param;
    uint32_t width;
} PartEnc;

/* ------------------------------------------------------------------ */
/* independent CRC implementations (polynomials from the FLAC format)  */
/* ------------------------------------------------------------------ */

static uint8_t own_crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (uint8_t)((crc & 0x80u)
                            ? ((uint32_t)crc << 1) ^ 0x07u
                            : (uint32_t)crc << 1);
    }
    return crc;
}

static uint16_t own_crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000u)
                             ? ((uint32_t)crc << 1) ^ 0x8005u
                             : (uint32_t)crc << 1);
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* MSB-first bit writer                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *base;
    size_t cap;
    size_t len;
    uint32_t acc;
    uint32_t nbits;
} BitBuf;

static void bb_init(BitBuf *b, uint8_t *base, size_t cap)
{
    b->base = base;
    b->cap = cap;
    b->len = 0;
    b->acc = 0;
    b->nbits = 0;
}

static uint32_t bb_bitpos(const BitBuf *b)
{
    return (uint32_t)(b->len * 8u + b->nbits);
}

static void bb_put(BitBuf *b, uint32_t value, uint32_t n)
{
    int k;
    for (k = (int)n - 1; k >= 0; k--) {
        b->acc = (b->acc << 1) | ((value >> (uint32_t)k) & 1u);
        b->nbits++;
        if (b->nbits == 8u) {
            if (b->len >= b->cap) {
                fprintf(stderr, "bit buffer overflow\n");
                abort();
            }
            b->base[b->len++] = (uint8_t)b->acc;
            b->acc = 0;
            b->nbits = 0;
        }
    }
}

static void bb_put_signed(BitBuf *b, int32_t value, uint32_t n)
{
    uint32_t mask = (n >= 32u) ? 0xffffffffu : ((1u << n) - 1u);
    bb_put(b, (uint32_t)value & mask, n);
}

static void bb_unary(BitBuf *b, uint32_t zeros)
{
    uint32_t i;
    for (i = 0; i < zeros; i++)
        bb_put(b, 0u, 1u);
    bb_put(b, 1u, 1u);
}

static void bb_align(BitBuf *b)
{
    while (b->nbits)
        bb_put(b, 0u, 1u);
}

static void bb_utf8(BitBuf *b, uint64_t v)
{
    if (v < 0x80ull) {
        bb_put(b, (uint32_t)v, 8);
    }
    else if (v < 0x800ull) {
        bb_put(b, 0xC0u | (uint32_t)(v >> 6), 8);
        bb_put(b, 0x80u | (uint32_t)(v & 0x3Full), 8);
    }
    else if (v < 0x10000ull) {
        bb_put(b, 0xE0u | (uint32_t)(v >> 12), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 6) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)(v & 0x3Full), 8);
    }
    else if (v < 0x200000ull) {
        bb_put(b, 0xF0u | (uint32_t)(v >> 18), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 12) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 6) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)(v & 0x3Full), 8);
    }
    else if (v < 0x4000000ull) {
        bb_put(b, 0xF8u | (uint32_t)(v >> 24), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 18) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 12) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 6) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)(v & 0x3Full), 8);
    }
    else {
        bb_put(b, 0xFCu | (uint32_t)(v >> 30), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 24) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 18) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 12) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)((v >> 6) & 0x3Full), 8);
        bb_put(b, 0x80u | (uint32_t)(v & 0x3Full), 8);
    }
}

/* ------------------------------------------------------------------ */
/* small integer helpers                                               */
/* ------------------------------------------------------------------ */

static int fits_signed(int64_t v, uint32_t bits)
{
    int64_t lo = -(int64_t)1 << (bits - 1);
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1;
    return v >= lo && v <= hi;
}

static uint32_t signed_bits(int32_t v)
{
    uint32_t n = 1;
    uint32_t a = (v < 0) ? (uint32_t)(-(v + 1)) : (uint32_t)v;
    while (a) { n++; a >>= 1; }
    return n;
}

/* floor division by a power of two, independent of platform shift style */
static int64_t sar_floor(int64_t v, int s)
{
    if (s == 0 || v >= 0)
        return v >> s;
    return -((-v + (((int64_t)1 << s) - 1)) >> s);
}

/* ------------------------------------------------------------------ */
/* reference prediction math (wide integers, no production helpers)    */
/* ------------------------------------------------------------------ */

static void ref_fixed_restore(const SubPlan *sp, uint32_t blocksize,
                              int32_t *out)
{
    int64_t d[MAX_BLOCK];
    uint32_t i;
    for (i = 0; i < sp->order; i++)
        d[i] = sp->warm[i];
    for (i = sp->order; i < blocksize; i++) {
        int32_t r = sp->resid[i - sp->order];
        switch (sp->order) {
            case 0: d[i] = r; break;
            case 1: d[i] = r + d[i - 1]; break;
            case 2: d[i] = r + 2 * d[i - 1] - d[i - 2]; break;
            case 3:
                d[i] = r + 3 * d[i - 1] - 3 * d[i - 2] + d[i - 3];
                break;
            default: /* order 4 */
                d[i] = r + 4 * d[i - 1] - 6 * d[i - 2]
                         + 4 * d[i - 3] - d[i - 4];
                break;
        }
        out[i] = (int32_t)d[i];
    }
    for (i = 0; i < sp->order; i++)
        out[i] = (int32_t)d[i];
}

static void ref_lpc_restore(const SubPlan *sp, uint32_t blocksize,
                            int32_t *out)
{
    int64_t d[MAX_BLOCK];
    uint32_t i, j;
    for (i = 0; i < sp->order; i++)
        d[i] = sp->warm[i];
    for (i = sp->order; i < blocksize; i++) {
        int64_t sum = 0;
        for (j = 0; j < sp->order; j++)
            sum += (int64_t)sp->qlp[j] * d[i - j - 1];
        d[i] = (int64_t)sp->resid[i - sp->order]
               + sar_floor(sum, sp->shift);
        out[i] = (int32_t)d[i];
    }
    for (i = 0; i < sp->order; i++)
        out[i] = (int32_t)d[i];
}

static void fixed_residuals(const int32_t *sig, uint32_t blocksize,
                            uint32_t order, int32_t *out)
{
    uint32_t i;
    for (i = order; i < blocksize; i++) {
        int64_t r;
        switch (order) {
            case 0: r = sig[i]; break;
            case 1: r = (int64_t)sig[i] - sig[i - 1]; break;
            case 2:
                r = (int64_t)sig[i] - 2 * (int64_t)sig[i - 1]
                    + sig[i - 2];
                break;
            case 3:
                r = (int64_t)sig[i] - 3 * (int64_t)sig[i - 1]
                    + 3 * (int64_t)sig[i - 2] - sig[i - 3];
                break;
            default:
                r = (int64_t)sig[i] - 4 * (int64_t)sig[i - 1]
                    + 6 * (int64_t)sig[i - 2]
                    - 4 * (int64_t)sig[i - 3] + sig[i - 4];
                break;
        }
        out[i - order] = (int32_t)r;
    }
}

static void lpc_residuals(const int32_t *sig, uint32_t blocksize,
                          const SubPlan *sp, int32_t *out)
{
    uint32_t i, j;
    for (i = sp->order; i < blocksize; i++) {
        int64_t sum = 0;
        for (j = 0; j < sp->order; j++)
            sum += (int64_t)sp->qlp[j] * (int64_t)sig[i - j - 1];
        out[i - sp->order] =
            (int32_t)((int64_t)sig[i] - sar_floor(sum, sp->shift));
    }
}

/* ------------------------------------------------------------------ */
/* Rice coding (MSB bitstream)                                         */
/* ------------------------------------------------------------------ */

static void bb_rice_signed(BitBuf *b, int32_t value, uint32_t param)
{
    uint32_t u;
    uint32_t lsbs;
    if (value >= 0)
        u = (uint32_t)(2 * value);
    else
        u = (uint32_t)(-2 * value - 1);
    bb_unary(b, u >> param);
    lsbs = param ? (u & ((1u << param) - 1u)) : 0u;
    if (param)
        bb_put(b, lsbs, param);
}

static uint32_t rice_escape(uint32_t rice2)
{
    return rice2 ? 31u : 15u;
}

static uint32_t rice_plen(uint32_t rice2)
{
    return rice2 ? 5u : 4u;
}

/* ------------------------------------------------------------------ */
/* subframe and frame synthesis                                        */
/* ------------------------------------------------------------------ */

static void put_subframe(BitBuf *b, SubPlan *sp,
                         uint32_t blocksize)
{
    uint32_t typebyte, ps, p, ebps = sp->bps;
    if (sp->type == 3)
        typebyte = 0x40u | ((sp->order - 1u) << 1);
    else
        typebyte = 0x10u | (sp->order << 1);
    if (sp->wasted)
        typebyte |= 1u;
    bb_put(b, typebyte, 8);
    if (sp->wasted)
        bb_unary(b, sp->wasted - 1u);

    if (sp->type == 2) {
        uint32_t i;
        for (i = 0; i < sp->order; i++)
            bb_put_signed(b, sp->warm[i], ebps);
    }
    else {
        uint32_t i;
        for (i = 0; i < sp->order; i++)
            bb_put_signed(b, sp->warm[i], ebps);
        bb_put(b, sp->qlp_prec - 1u, 4);
        bb_put_signed(b, sp->shift, 5);
        for (i = 0; i < sp->order; i++)
            bb_put_signed(b, sp->qlp[i], sp->qlp_prec);
    }

    bb_put(b, sp->rice2 ? 1u : 0u, 2);
    bb_put(b, sp->part_order, 4);

    ps = blocksize >> sp->part_order;
    for (p = 0; p < (1u << sp->part_order); p++) {
        uint32_t u = (p == 0) ? (ps - sp->order) : ps;
        uint32_t base = (p == 0) ? 0u :
                        (ps - sp->order) + (p - 1u) * ps;
        sp->parts[p].start_bit = bb_bitpos(b);
        sp->parts[p].count = u;
        if (sp->raw[p] == 0u && sp->param[p] < rice_escape(sp->rice2)) {
            uint32_t k;
            bb_put(b, sp->param[p], rice_plen(sp->rice2));
            for (k = 0; k < u; k++)
                bb_rice_signed(b, sp->resid[base + k], sp->param[p]);
        }
        else {
            uint32_t k;
            bb_put(b, rice_escape(sp->rice2), rice_plen(sp->rice2));
            bb_put(b, sp->raw[p], 5);
            for (k = 0; k < u; k++)
                bb_put_signed(b, sp->resid[base + k], sp->raw[p]);
        }
        sp->parts[p].end_bit = bb_bitpos(b);
    }
}

static uint32_t bps_nibble(uint32_t bps)
{
    if (bps == 16)
        return 4u;
    return 6u; /* 24 */
}

static uint32_t assign_nibble(uint32_t assign)
{
    switch (assign) {
        case ASSIGN_LEFT_SIDE: return 8u;
        case ASSIGN_RIGHT_SIDE: return 9u;
        case ASSIGN_MID_SIDE: return 0xAu;
        default: return 1u; /* independent, two channels */
    }
}

static void build_frame(StreamPlan *sp, FramePlan *f)
{
    BitBuf b;
    size_t start;
    uint32_t ch, eff_bps;
    uint8_t c8;
    uint16_t c16;
    bb_init(&b, sp->bytes + sp->len, sizeof(sp->bytes) - sp->len);

    start = sp->len;
    /* sync (14) + reserved 0 (1) + variable blocksize strategy (1) */
    bb_put(&b, 0x3ffeu, 14);
    bb_put(&b, 0u, 1);
    bb_put(&b, 1u, 1);
    /* blocksize: 256 has a direct code, anything else uses the 8-bit hint
     * (nibble 6: blocksize = next byte + 1) */
    bb_put(&b, f->blocksize == 256u ? 8u : 6u, 4);
    bb_put(&b, 10u, 4); /* 48000 Hz */
    bb_put(&b, assign_nibble(f->assign), 4);
    bb_put(&b, bps_nibble(f->bps) << 1, 4); /* low bit is the zero pad */
    bb_utf8(&b, f->sample_number);
    if (f->blocksize != 256u)
        bb_put(&b, f->blocksize - 1u, 8);
    c8 = own_crc8(sp->bytes + start, b.len);
    bb_put(&b, c8, 8);

    for (ch = 0; ch < 2; ch++) {
        eff_bps = f->bps;
        if (f->assign != ASSIGN_INDEPENDENT) {
            int side_channel =
                (f->assign == ASSIGN_RIGHT_SIDE) ? (ch == 0) : (ch == 1);
            if (side_channel)
                eff_bps++;
        }
        f->sub[ch].bps = eff_bps - f->sub[ch].wasted;
        put_subframe(&b, &f->sub[ch], f->blocksize);
    }

    bb_align(&b);
    c16 = own_crc16(sp->bytes + start, b.len);
    bb_put(&b, c16, 16);

    f->frame_start = start;
    f->frame_end = start + b.len;
    sp->len += b.len;
}

static void build_streaminfo(StreamPlan *sp)
{
    BitBuf b;
    bb_init(&b, sp->bytes, sizeof(sp->bytes));
    bb_put(&b, 0x66u, 8); /* f */
    bb_put(&b, 0x4Cu, 8); /* L */
    bb_put(&b, 0x61u, 8); /* a */
    bb_put(&b, 0x43u, 8); /* C */
    bb_put(&b, 1u, 1);    /* last metadata block */
    bb_put(&b, 0u, 7);    /* STREAMINFO */
    bb_put(&b, 34u, 24);
    bb_put(&b, 16u, 16);              /* min blocksize */
    bb_put(&b, 256u, 16);             /* max blocksize */
    bb_put(&b, 0u, 24);               /* min framesize unknown */
    bb_put(&b, 0u, 24);               /* max framesize unknown */
    bb_put(&b, SAMPLE_RATE, 20);
    bb_put(&b, 1u, 3);                /* channels - 1 */
    bb_put(&b, sp->bps - 1u, 5);      /* bits per sample - 1 */
    bb_put(&b, (uint32_t)sp->total_samples, 36);
    {
        int i;
        for (i = 0; i < 16; i++)
            bb_put(&b, 0u, 8);        /* MD5 deliberately zero */
    }
    bb_align(&b);
    sp->len = b.len;
}

/* ------------------------------------------------------------------ */
/* plan construction helpers                                           */
/* ------------------------------------------------------------------ */

static int32_t gen_value(uint32_t i, uint32_t salt, int32_t amp)
{
    int64_t v = (int64_t)((i * 73u + salt * 131u) % (2u * (uint32_t)amp + 1u))
                - amp;
    if ((i & 7u) == 3u)
        v -= (int64_t)(i % 11u) * (amp / 16 + 1);
    if ((i & 7u) == 5u)
        v += (int64_t)(i % 13u) * (amp / 16 + 1);
    if (v < -amp) v = -amp;
    if (v > amp) v = amp;
    return (int32_t)v;
}

static void fill_partition_encoding(SubPlan *sp, const PartEnc *enc)
{
    uint32_t p, parts = 1u << sp->part_order;
    for (p = 0; p < parts; p++) {
        if (enc[p].escape) {
            sp->param[p] = rice_escape(sp->rice2);
            sp->raw[p] = enc[p].width;
        }
        else {
            sp->param[p] = enc[p].param;
            sp->raw[p] = 0u;
        }
    }
}

static uint32_t partition_width(const SubPlan *sp, uint32_t p,
                                uint32_t blocksize)
{
    uint32_t ps = blocksize >> sp->part_order;
    uint32_t u = (p == 0) ? (ps - sp->order) : ps;
    uint32_t base = (p == 0) ? 0u :
                    (ps - sp->order) + (p - 1u) * ps;
    uint32_t k, width = 1;
    for (k = 0; k < u; k++) {
        uint32_t need = signed_bits(sp->resid[base + k]);
        if (need > width) width = need;
    }
    return width;
}

static void make_fixed(SubPlan *sp, uint32_t order, uint32_t rice2,
                       uint32_t part_order, const int32_t *sig,
                       uint32_t blocksize, uint32_t wasted,
                       const PartEnc *enc)
{
    memset(sp, 0, sizeof(*sp));
    sp->type = 2;
    sp->order = order;
    sp->wasted = wasted;
    sp->rice2 = rice2;
    sp->part_order = part_order;
    memcpy(sp->signal, sig, sizeof(int32_t) * blocksize);
    memcpy(sp->warm, sig, sizeof(int32_t) * order);
    fixed_residuals(sig, blocksize, order, sp->resid);
    fill_partition_encoding(sp, enc);
}

static void make_lpc(SubPlan *sp, uint32_t order, uint32_t rice2,
                     uint32_t part_order, const int32_t *sig,
                     uint32_t blocksize, uint32_t wasted,
                     const int32_t *qlp, uint32_t qlp_prec, int shift,
                     const PartEnc *enc)
{
    memset(sp, 0, sizeof(*sp));
    sp->type = 3;
    sp->order = order;
    sp->wasted = wasted;
    sp->rice2 = rice2;
    sp->part_order = part_order;
    sp->qlp_prec = qlp_prec;
    sp->shift = shift;
    memcpy(sp->signal, sig, sizeof(int32_t) * blocksize);
    memcpy(sp->warm, sig, sizeof(int32_t) * order);
    memcpy(sp->qlp, qlp, sizeof(int32_t) * order);
    lpc_residuals(sig, blocksize, sp, sp->resid);
    fill_partition_encoding(sp, enc);
}

/* residual-first LPC plan: the PCM signal is reconstructed here with the
 * wide integer recurrence instead of being taken from an encoder */
static void make_lpc_plan(SubPlan *sp, uint32_t order, uint32_t rice2,
                          uint32_t part_order, const int32_t *warm,
                          const int32_t *resid, uint32_t blocksize,
                          uint32_t wasted, const int32_t *qlp,
                          uint32_t qlp_prec, int shift,
                          const PartEnc *enc)
{
    memset(sp, 0, sizeof(*sp));
    sp->type = 3;
    sp->order = order;
    sp->wasted = wasted;
    sp->rice2 = rice2;
    sp->part_order = part_order;
    sp->qlp_prec = qlp_prec;
    sp->shift = shift;
    memcpy(sp->warm, warm, sizeof(int32_t) * order);
    memcpy(sp->qlp, qlp, sizeof(int32_t) * order);
    memcpy(sp->resid, resid, sizeof(int32_t) * (blocksize - order));
    ref_lpc_restore(sp, blocksize, sp->signal);
    fill_partition_encoding(sp, enc);
}

/* residual-first fixed predictor plan */
static void make_fixed_plan(SubPlan *sp, uint32_t order, uint32_t rice2,
                            uint32_t part_order, const int32_t *warm,
                            const int32_t *resid, uint32_t blocksize,
                            uint32_t wasted, const PartEnc *enc)
{
    memset(sp, 0, sizeof(*sp));
    sp->type = 2;
    sp->order = order;
    sp->wasted = wasted;
    sp->rice2 = rice2;
    sp->part_order = part_order;
    memcpy(sp->warm, warm, sizeof(int32_t) * order);
    memcpy(sp->resid, resid, sizeof(int32_t) * (blocksize - order));
    ref_fixed_restore(sp, blocksize, sp->signal);
    fill_partition_encoding(sp, enc);
}

/* derive the two subframe signals and the expected final L/R PCM */
static void derive_channels(FramePlan *f)
{
    SubPlan *s0 = &f->sub[0];
    SubPlan *s1 = &f->sub[1];
    uint32_t i;
    for (i = 0; i < f->blocksize; i++) {
        int32_t a = s0->signal[i];
        int32_t b = s1->signal[i];
        switch (f->assign) {
            case ASSIGN_INDEPENDENT:
                f->want[0][i] = a;
                f->want[1][i] = b;
                break;
            case ASSIGN_LEFT_SIDE:
                f->want[0][i] = a;
                f->want[1][i] = a - b;
                break;
            case ASSIGN_RIGHT_SIDE:
                f->want[1][i] = b;
                f->want[0][i] = b + a;
                break;
            default: { /* mid-side, floor mid + parity */
                int64_t mid2 = ((int64_t)a << 1) | ((int64_t)b & 1ll);
                f->want[0][i] = (int32_t)sar_floor(mid2 + b, 1);
                f->want[1][i] = (int32_t)sar_floor(mid2 - b, 1);
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* decoder harness                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const StreamPlan *plan;
    const uint8_t *data;
    size_t len;
    size_t pos;
    int chunk_mode;       /* 0 = whole reads, 1 = 1/3/7 byte alternation */
    unsigned chunk_step;
    uint32_t frames_seen;
    uint64_t samples_seen;
    int errors;
    int fail;
    int mismatch_reported;
} Dec;

static FLAC__StreamDecoderReadStatus dec_read(const FLAC__StreamDecoder *d,
                                              FLAC__byte *buf, size_t *n,
                                              void *v)
{
    Dec *c = v;
    size_t want = *n;
    static const size_t chunks[] = {1u, 3u, 7u};
    (void)d;
    if (c->pos >= c->len) {
        *n = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    if (c->chunk_mode) {
        want = chunks[c->chunk_step % 3u];
        c->chunk_step++;
        if (want > *n)
            want = *n;
    }
    if (want > c->len - c->pos)
        want = c->len - c->pos;
    memcpy(buf, c->data + c->pos, want);
    c->pos += want;
    *n = want;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderSeekStatus dec_seek(const FLAC__StreamDecoder *d,
                                              uint64_t off, void *v)
{
    Dec *c = v;
    (void)d;
    if (off > c->len)
        return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
    c->pos = (size_t)off;
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus dec_tell(const FLAC__StreamDecoder *d,
                                              uint64_t *off, void *v)
{
    (void)d;
    *off = ((Dec *)v)->pos;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus dec_len(const FLAC__StreamDecoder *d,
                                               uint64_t *n, void *v)
{
    (void)d;
    *n = ((Dec *)v)->len;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool dec_eof(const FLAC__StreamDecoder *d, void *v)
{
    (void)d;
    return ((Dec *)v)->pos >= ((Dec *)v)->len;
}

static void dec_error(const FLAC__StreamDecoder *d,
                      FLAC__StreamDecoderErrorStatus s, void *v)
{
    Dec *c = v;
    (void)d;
    c->errors++;
    fprintf(stderr, "decoder error status=%d\n", (int)s);
}

static void note_mismatch(Dec *c, const char *what, uint32_t frame,
                          uint32_t channel, int partition, uint32_t sample)
{
    c->fail++;
    if (!c->mismatch_reported) {
        fprintf(stderr,
                "mismatch %s frame=%u channel=%u partition=%d sample=%u\n",
                what, frame, channel, partition, sample);
        c->mismatch_reported = 1;
    }
}

#define CCHECK(c, cond, what, fr, ch, pt, sm)                           \
    do {                                                                 \
        if (!task_check(!!(cond), #cond, __FILE__, __LINE__))           \
            note_mismatch((c), (what), (fr), (ch), (pt), (sm));        \
    } while (0)

/* map a residual array index to its Rice partition, so a residual
 * mismatch is reported with the frame, channel, partition and sample */
static int residual_partition_of(const SubPlan *want, uint32_t blocksize,
                                 uint32_t idx)
{
    uint32_t ps = blocksize >> want->part_order;
    uint32_t first = ps - want->order;
    if (idx < first)
        return 0;
    return 1 + (int)((idx - first) / ps);
}

static void check_subframe(Dec *c, const FLAC__Frame *frame,
                           const FramePlan *fp, uint32_t ch,
                           uint32_t blocksize)
{
    const FLAC__Subframe *got = &frame->subframes[ch];
    const SubPlan *want = &fp->sub[ch];
    const FLAC__EntropyCodingMethod *ecm;
    uint32_t p, parts = 1u << want->part_order;
    CCHECK(c, got->wasted_bits == want->wasted, "wasted_bits",
           c->frames_seen, ch, -1, 0);
    CCHECK(c, (uint32_t)got->type == (uint32_t)want->type, "subframe type",
           c->frames_seen, ch, -1, 0);
    ecm = (want->type == 3) ? &got->data.lpc.entropy_coding_method
                            : &got->data.fixed.entropy_coding_method;
    CCHECK(c, ecm->type ==
              (want->rice2 ? FLAC__ENTROPY_CODING_METHOD_PARTITIONED_RICE2
                           : FLAC__ENTROPY_CODING_METHOD_PARTITIONED_RICE),
           "entropy type", c->frames_seen, ch, -1, 0);
    CCHECK(c, ecm->data.partitioned_rice.order ==
              want->part_order, "partition order", c->frames_seen, ch, -1, 0);

    for (p = 0; p < parts; p++) {
        const FLAC__EntropyCodingMethod_PartitionedRiceContents *rc =
            ecm->data.partitioned_rice.contents;
        CCHECK(c, rc->parameters[p] == want->param[p], "rice parameter",
               c->frames_seen, ch, (int)p, 0);
        CCHECK(c, rc->raw_bits[p] == want->raw[p], "escape raw width",
               c->frames_seen, ch, (int)p, 0);
        CCHECK(c, want->parts[p].start_bit < want->parts[p].end_bit ||
                  want->parts[p].count == 0u,
               "partition bit range", c->frames_seen, ch, (int)p, 0);
        if (p > 0)
            CCHECK(c, want->parts[p].start_bit >= want->parts[p - 1u].end_bit,
                   "partition ordering", c->frames_seen, ch, (int)p, 0);
    }

    if (want->type == 2) {
        const FLAC__Subframe_Fixed *fx = &got->data.fixed;
        uint32_t i;
        CCHECK(c, fx->order == want->order, "fixed order",
               c->frames_seen, ch, -1, 0);
        for (i = 0; i < want->order; i++)
            CCHECK(c, fx->warmup[i] == want->warm[i], "fixed warmup",
                   c->frames_seen, ch, -1, i);
        for (i = 0; i < blocksize - want->order; i++)
            CCHECK(c, fx->residual[i] == want->resid[i], "fixed residual",
                   c->frames_seen, ch,
                   residual_partition_of(want, blocksize, i),
                   want->order + i);
    }
    else {
        const FLAC__Subframe_LPC *lp = &got->data.lpc;
        uint32_t i;
        CCHECK(c, lp->order == want->order, "lpc order",
               c->frames_seen, ch, -1, 0);
        CCHECK(c, lp->qlp_coeff_precision == want->qlp_prec,
               "qlp precision", c->frames_seen, ch, -1, 0);
        CCHECK(c, lp->quantization_level == want->shift, "qlp shift",
               c->frames_seen, ch, -1, 0);
        for (i = 0; i < want->order; i++)
            CCHECK(c, lp->qlp_coeff[i] == want->qlp[i], "qlp coefficient",
                   c->frames_seen, ch, -1, i);
        for (i = 0; i < want->order; i++)
            CCHECK(c, lp->warmup[i] == want->warm[i], "lpc warmup",
                   c->frames_seen, ch, -1, i);
        for (i = 0; i < blocksize - want->order; i++)
            CCHECK(c, lp->residual[i] == want->resid[i], "lpc residual",
                   c->frames_seen, ch,
                   residual_partition_of(want, blocksize, i),
                   want->order + i);
    }
}

static FLAC__StreamDecoderWriteStatus
dec_write(const FLAC__StreamDecoder *d, const FLAC__Frame *frame,
          const FLAC__int32 * const buf[], void *v)
{
    Dec *c = v;
    const FramePlan *fp;
    uint32_t i, ch, blocksize;
    (void)d;
    if (c->frames_seen >= c->plan->nframes) {
        c->fail++;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
    fp = &c->plan->frames[c->frames_seen];
    blocksize = frame->header.blocksize;

    CCHECK(c, frame->header.number_type ==
              FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER, "number type",
           c->frames_seen, 0, -1, 0);
    CCHECK(c, frame->header.number.sample_number == fp->sample_number,
           "sample number", c->frames_seen, 0, -1, 0);
    CCHECK(c, blocksize == fp->blocksize, "blocksize",
           c->frames_seen, 0, -1, 0);
    CCHECK(c, frame->header.channels == 2u, "channels",
           c->frames_seen, 0, -1, 0);
    CCHECK(c, frame->header.bits_per_sample == fp->bps, "bps",
           c->frames_seen, 0, -1, 0);
    CCHECK(c, (uint32_t)frame->header.channel_assignment == fp->assign,
           "channel assignment", c->frames_seen, 0, -1, 0);

    for (ch = 0; ch < 2; ch++)
        check_subframe(c, frame, fp, ch, blocksize);

    for (ch = 0; ch < 2; ch++)
        for (i = 0; i < blocksize; i++)
            CCHECK(c, buf[ch][i] == fp->want[ch][i], "pcm",
                   c->frames_seen, ch, -1, i);

    c->frames_seen++;
    c->samples_seen += blocksize;
    return c->fail ? FLAC__STREAM_DECODER_WRITE_STATUS_ABORT
                   : FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/* one full decode pass; replay after reset+rewind when do_replay */
static int run_decode(const StreamPlan *pl, int chunk_mode, int use_single,
                      int do_replay)
{
    FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
    Dec c;
    int pass;
    TASK_CHECK(d != 0);
    TASK_CHECK(FLAC__stream_decoder_init_stream(
                   d, dec_read, dec_seek, dec_tell, dec_len, dec_eof,
                   dec_write, 0, dec_error, &c) ==
               FLAC__STREAM_DECODER_INIT_STATUS_OK);

    for (pass = 0; pass < (do_replay ? 2 : 1); pass++) {
        memset(&c, 0, sizeof(c));
        c.plan = pl;
        c.data = pl->bytes;
        c.len = pl->len;
        c.pos = 0;
        c.chunk_mode = chunk_mode;
        if (pass == 1) {
            TASK_CHECK(FLAC__stream_decoder_reset(d));
            TASK_CHECK(c.pos == 0);
        }
        if (use_single) {
            FLAC__StreamDecoderState st;
            do {
                TASK_CHECK(FLAC__stream_decoder_process_single(d));
                st = FLAC__stream_decoder_get_state(d);
            } while (st == FLAC__STREAM_DECODER_SEARCH_FOR_METADATA ||
                     st == FLAC__STREAM_DECODER_READ_METADATA ||
                     st == FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC ||
                     st == FLAC__STREAM_DECODER_READ_FRAME);
            TASK_CHECK(st == FLAC__STREAM_DECODER_END_OF_STREAM);
        }
        else {
            TASK_CHECK(FLAC__stream_decoder_process_until_end_of_stream(d));
            TASK_CHECK(FLAC__stream_decoder_get_state(d) ==
                       FLAC__STREAM_DECODER_END_OF_STREAM);
        }
        TASK_CHECK(c.errors == 0);
        TASK_CHECK(c.fail == 0);
        TASK_CHECK(c.frames_seen == pl->nframes);
        TASK_CHECK(c.samples_seen == pl->total_samples);
    }
    TASK_CHECK(FLAC__stream_decoder_finish(d));
    FLAC__stream_decoder_delete(d);
    return 0;
}

/* exercise every delivery/entry combination plus the reset replay */
static int contract_stream(const StreamPlan *pl)
{
    TASK_CHECK(pl->len > 0 && pl->len < MAX_STREAM_BYTES);
    TASK_CHECK(run_decode(pl, 0, 1, 1) == 0);  /* normal reads, single */
    TASK_CHECK(run_decode(pl, 0, 0, 0) == 0);  /* normal reads, rectifier */
    TASK_CHECK(run_decode(pl, 1, 1, 0) == 0);  /* 1/3/7 reads, single */
    TASK_CHECK(run_decode(pl, 1, 0, 1) == 0);  /* 1/3/7 reads, rectifier */
    return 0;
}

/* ================================================================== */
/* residuals group                                                     */
/* ================================================================== */

static StreamPlan rice_plan;
static StreamPlan rice2_plan;

static int build_rice_plan(void)
{
    StreamPlan *pl = &rice_plan;
    FramePlan *f;
    SubPlan *s;
    PartEnc enc[4];
    int32_t sig[2][64];
    uint32_t i;
    const uint32_t n = 64u, order = 1u, ps = 16u;

    memset(pl, 0, sizeof(*pl));
    pl->bps = 16;
    f = &pl->frames[0];
    f->blocksize = n;
    f->bps = 16;
    f->assign = ASSIGN_INDEPENDENT;
    f->sample_number = 0;

    /* channel 0: explicit residual plan, warm-up must not look like data */
    s = &f->sub[0];
    memset(s, 0, sizeof(*s));
    s->type = 2;
    s->order = order;
    s->rice2 = 0;
    s->part_order = 2;
    s->warm[0] = 1234;
    enc[0] = (PartEnc){0, 2u, 0}; /* normal rice param 2 */
    enc[1] = (PartEnc){1, 0u, 0}; /* escape width 0: all zero */
    enc[2] = (PartEnc){0, 0u, 0}; /* normal rice param 0 */
    enc[3] = (PartEnc){1, 0u, 8}; /* escape width 8, two's complement */
    fill_partition_encoding(s, enc);
    /* p0: 15 residuals, positives, negatives, zero, word-spanning unary */
    for (i = 0; i < 15; i++)
        s->resid[i] = (int32_t)i - 7;
    s->resid[3] = 140;   /* u=280, 70 unary zeros: crosses a machine word */
    s->resid[9] = -141;  /* u=281, 70 unary zeros */
    s->resid[12] = 0;
    /* p1 is the width-0 escape partition: residuals stay zero */
    /* p2: rice param 0, 16 entries */
    for (i = 0; i < 16; i++)
        s->resid[15 + 16 + i] = (i % 5 == 0) ? 0 :
                                (i & 1) ? -(int32_t)(i / 2 + 1)
                                        : (int32_t)(i / 3 + 1);
    /* p3: width-8 escape, full two's complement range */
    for (i = 0; i < 16; i++)
        s->resid[15 + 32 + i] =
            (int32_t)(i * 17u % 251u) - 125; /* spans -125..125, incl 0 */
    TASK_CHECK(partition_width(s, 3, n) <= 8);
    ref_fixed_restore(s, n, sig[0]);
    memcpy(s->signal, sig[0], sizeof(sig[0]));
    for (i = 0; i < n; i++)
        TASK_CHECK(fits_signed(sig[0][i], 16));

    /* channel 1: order 2 so the first partition loses two samples */
    s = &f->sub[1];
    memset(s, 0, sizeof(*s));
    s->type = 2;
    s->order = 2;
    s->rice2 = 0;
    s->part_order = 2;
    s->warm[0] = -432;
    s->warm[1] = 77;
    enc[0] = (PartEnc){0, 3u, 0};
    enc[1] = (PartEnc){1, 0u, 0};
    enc[2] = (PartEnc){0, 1u, 0};
    enc[3] = (PartEnc){1, 0u, 7};
    fill_partition_encoding(s, enc);
    for (i = 0; i < ps - 2; i++)
        s->resid[i] = (int32_t)((i * 5) % 11) - 5;
    s->resid[5] = -200; /* long unary under param 3 */
    for (i = 0; i < ps; i++)
        s->resid[(ps - 2) + ps + i] = (int32_t)(i - 8);
    for (i = 0; i < ps; i++)
        s->resid[(ps - 2) + 2 * ps + i] =
            (int32_t)(i * 11u % 121u) - 60;
    TASK_CHECK(partition_width(s, 3, n) <= 7);
    ref_fixed_restore(s, n, sig[1]);
    memcpy(s->signal, sig[1], sizeof(sig[1]));
    for (i = 0; i < n; i++)
        TASK_CHECK(fits_signed(sig[1][i], 16));

    pl->nframes = 1;
    pl->total_samples = n;
    build_streaminfo(pl);
    derive_channels(f);
    build_frame(pl, f);
    TASK_CHECK(f->frame_end > f->frame_start);
    return 0;
}

static int build_rice2_plan(void)
{
    StreamPlan *pl = &rice2_plan;
    FramePlan *f;
    SubPlan *s;
    PartEnc enc[4];
    int32_t sig[2][128];
    uint32_t i;
    const uint32_t n = 128u, ps = 32u;

    memset(pl, 0, sizeof(*pl));
    pl->bps = 16;
    f = &pl->frames[0];
    f->blocksize = n;
    f->bps = 16;
    f->assign = ASSIGN_INDEPENDENT;
    f->sample_number = 0;

    /* channel 0: fixed order 3, RICE2, four differently coded partitions */
    s = &f->sub[0];
    memset(s, 0, sizeof(*s));
    s->type = 2;
    s->order = 3;
    s->rice2 = 1;
    s->part_order = 2;
    s->warm[0] = 11;
    s->warm[1] = -22;
    s->warm[2] = -55; /* arithmetic warm-up: zero second difference,
                        * so the homogeneous part stays linear */
    enc[0] = (PartEnc){0, 5u, 0};  /* normal rice2 param 5 */
    enc[1] = (PartEnc){1, 0u, 0};  /* escape width 0 */
    enc[2] = (PartEnc){0, 3u, 0};  /* normal rice2 param 3 */
    enc[3] = (PartEnc){1, 0u, 12}; /* escape width 12 */
    fill_partition_encoding(s, enc);
    /* Under order-3 restoration a residual is a third difference, so only
     * (v,-3v,3v,-v) quadruples produce a finite pulse: lone impulses would
     * leave a permanent polynomial tail. Signs, zero and word-crossing
     * unary all still occur (-3300 -> 206 unary zeros under parameter 5). */
    s->resid[0] = 2;
    s->resid[1] = -6;
    s->resid[2] = 6;
    s->resid[3] = -2;
    s->resid[4] = 1100;
    s->resid[5] = -3300;
    s->resid[6] = 3300;
    s->resid[7] = -1100;
    s->resid[10] = -3;
    s->resid[11] = 9;
    s->resid[12] = -9;
    s->resid[13] = 3;
    s->resid[20] = 0;
    /* p1 is the width-0 escape partition: residuals stay zero */
    /* p2: rice2 param 3, finite-pulse quadruples plus zeros */
    s->resid[(ps - 3) + ps + 0] = 9;
    s->resid[(ps - 3) + ps + 1] = -27;
    s->resid[(ps - 3) + ps + 2] = 27;
    s->resid[(ps - 3) + ps + 3] = -9;
    s->resid[(ps - 3) + ps + 4] = -7;
    s->resid[(ps - 3) + ps + 5] = 21;
    s->resid[(ps - 3) + ps + 6] = -21;
    s->resid[(ps - 3) + ps + 7] = 7;
    /* p3: width-12 escape, alternating-sign finite pulses */
    for (i = 0; i < ps; i++) {
        static const int32_t cycle[8] = {600, -1800, 1800, -600,
                                         -600, 1800, -1800, 600};
        s->resid[(ps - 3) + 2 * ps + i] = cycle[i % 8];
    }
    TASK_CHECK(partition_width(s, 3, n) <= 12);
    ref_fixed_restore(s, n, sig[0]);
    memcpy(s->signal, sig[0], sizeof(sig[0]));
    for (i = 0; i < n; i++)
        TASK_CHECK(fits_signed(sig[0][i], 16));

    /* channel 1: fixed order 1, RICE2, zero escape then normal */
    s = &f->sub[1];
    memset(s, 0, sizeof(*s));
    s->type = 2;
    s->order = 1;
    s->rice2 = 1;
    s->part_order = 2;
    s->warm[0] = -909;
    enc[0] = (PartEnc){0, 4u, 0};
    enc[1] = (PartEnc){1, 0u, 0};
    enc[2] = (PartEnc){0, 6u, 0};
    enc[3] = (PartEnc){1, 0u, 10};
    fill_partition_encoding(s, enc);
    for (i = 0; i < ps - 1; i++)
        s->resid[i] = (int32_t)((i * 3) % 13) - 6;
    for (i = 0; i < ps; i++)
        s->resid[(ps - 1) + ps + i] =
            (i % 4 == 0) ? 0 : (int32_t)i - 16;
    for (i = 0; i < ps; i++)
        s->resid[(ps - 1) + 2 * ps + i] =
            (int32_t)((i * 53u) % 1001u) - 500;
    TASK_CHECK(partition_width(s, 3, n) <= 10);
    ref_fixed_restore(s, n, sig[1]);
    memcpy(s->signal, sig[1], sizeof(sig[1]));
    for (i = 0; i < n; i++)
        TASK_CHECK(fits_signed(sig[1][i], 16));

    pl->nframes = 1;
    pl->total_samples = n;
    build_streaminfo(pl);
    derive_channels(f);
    build_frame(pl, f);
    TASK_CHECK(f->frame_end > f->frame_start);
    return 0;
}

static int case_residuals_rice(void)
{
    TASK_CHECK(build_rice_plan() == 0);
    return contract_stream(&rice_plan);
}

static int case_residuals_rice2(void)
{
    TASK_CHECK(build_rice2_plan() == 0);
    return contract_stream(&rice2_plan);
}

/* ================================================================== */
/* prediction group                                                    */
/* ================================================================== */

/* place an order-k finite pulse: residuals are the k-th difference of a
 * short bounded pulse, so order-k restoration cannot leave a polynomial
 * tail; sign is selected by v */
static void put_pulse(int32_t *resid, uint32_t at, uint32_t order,
                      int32_t v)
{
    static const int32_t binom[5][5] = {
        {1, 0, 0, 0, 0},
        {1, -1, 0, 0, 0},
        {1, -2, 1, 0, 0},
        {1, -3, 3, -1, 0},
        {1, -4, 6, -4, 1}
    };
    uint32_t j;
    for (j = 0; j <= order; j++)
        resid[at + j] = v * binom[order][j];
}

/* re-derive a subframe's partition encoding from its residuals, so no
 * hard-coded escape width can truncate them */
static uint32_t resid_partition_width(const int32_t *resid,
                                      uint32_t blocksize, uint32_t order,
                                      uint32_t part_order, uint32_t p);
static void fill_enc_from_resid(const int32_t *resid, uint32_t blocksize,
                                uint32_t order, uint32_t part_order,
                                uint32_t rice2, PartEnc *enc);
static void reencode_subframe(SubPlan *sp, uint32_t blocksize)
{
    PartEnc enc[MAX_PARTS];
    fill_enc_from_resid(sp->resid, blocksize, sp->order, sp->part_order,
                        sp->rice2, enc);
    fill_partition_encoding(sp, enc);
}

/* classify each partition from the residuals actually written: an
 * all-zero partition becomes an escape with width 0, wide-residual
 * partitions become correctly sized two's complement escapes (a small
 * Rice parameter would create enormous unary runs), and the remaining
 * small-value partitions carry normal Rice parameters scaled to the
 * residual width */
static void fill_enc_from_resid(const int32_t *resid, uint32_t blocksize,
                                uint32_t order, uint32_t part_order,
                                uint32_t rice2, PartEnc *enc)
{
    uint32_t p, parts = 1u << part_order;
    for (p = 0; p < parts; p++) {
        uint32_t width = resid_partition_width(resid, blocksize, order,
                                               part_order, p);
        if (width <= 1) { /* only zeros occur: width-0 escape */
            enc[p].escape = 1;
            enc[p].param = 0;
            enc[p].width = 0;
        }
        else if (width > 7u || (p & 1u)) {
            enc[p].escape = 1;
            enc[p].param = 0;
            enc[p].width = width;
        }
        else {
            /* parameter tracks residual width so unary runs stay short;
             * keep below the RICE(15)/RICE2(31) escape values */
            uint32_t param = width - 1u;
            if (!rice2 && param > 12u)
                param = 12u;
            if (rice2 && param > 28u)
                param = 28u;
            enc[p].escape = 0;
            enc[p].param = param;
            enc[p].width = 0;
        }
    }
}

/* two's complement width needed for one residual partition (partition 0
 * holds blocksize/2^part_order - order entries) */
static uint32_t resid_partition_width(const int32_t *resid,
                                      uint32_t blocksize, uint32_t order,
                                      uint32_t part_order, uint32_t p)
{
    uint32_t ps = blocksize >> part_order;
    uint32_t cnt = (p == 0) ? (ps - order) : ps;
    uint32_t base = (p == 0) ? 0u : (ps - order) + (p - 1u) * ps;
    uint32_t k, width = 1;
    for (k = 0; k < cnt; k++) {
        uint32_t need = signed_bits(resid[base + k]);
        if (need > width)
            width = need;
    }
    return width;
}

static int case_prediction_fixed(void)
{
    StreamPlan pl;
    static const uint32_t orders[3][2] = {{4, 3}, {2, 1}, {0, 4}};
    static const uint32_t blocks[3] = {128u, 96u, 64u};
    static const uint32_t pord[3] = {2u, 1u, 2u};
    uint32_t k, ch, i;
    int32_t resid[MAX_BLOCK];
    int32_t warm[4];
    PartEnc enc[4];

    memset(&pl, 0, sizeof(pl));
    pl.bps = 16;
    pl.nframes = 3;
    for (k = 0; k < pl.nframes; k++) {
        FramePlan *f = &pl.frames[k];
        f->blocksize = blocks[k];
        f->bps = 16;
        f->assign = ASSIGN_INDEPENDENT;
        f->sample_number = pl.total_samples;
        for (ch = 0; ch < 2; ch++) {
            uint32_t order = orders[k][ch];
            SubPlan *sp = &f->sub[ch];
            memset(resid, 0, sizeof(resid));
            /* arithmetic warm-ups: homogeneous part is linear for every
             * order, so zero residuals produce a bounded signal */
            for (i = 0; i < 4; i++)
                warm[i] = (int32_t)(13 + (int)i * (7 - (int)ch * 5)
                                    + (int)k * 11 - (int)ch * 29);
            /* finite pulses of both signs, spread over the partitions */
            put_pulse(resid, order + 1, order, ch ? -120 : 150);
            put_pulse(resid, f->blocksize / 2u, order, ch ? 90 : -70);
            put_pulse(resid, f->blocksize - order - 5u, order,
                      ch ? -400 : 350);
            resid[order] = 0; /* explicit zero alongside signed entries */
            fill_enc_from_resid(resid, f->blocksize, order, pord[k],
                                ch, enc);
            make_fixed_plan(sp, order, ch, pord[k], warm, resid,
                            f->blocksize, 0, enc);
            for (i = 0; i < f->blocksize; i++)
                TASK_CHECK(fits_signed(sp->signal[i], 16));
            /* the warm-up region must be distinguishable from residuals */
            for (i = 0; i < order; i++)
                TASK_CHECK(sp->warm[i] == warm[i]);
        }
        derive_channels(f);
        pl.total_samples += f->blocksize;
    }
    build_streaminfo(&pl);
    for (k = 0; k < pl.nframes; k++)
        build_frame(&pl, &pl.frames[k]);
    return contract_stream(&pl);
}

static int case_prediction_lpc_mix(void)
{
    StreamPlan pl;
    FramePlan *f = &pl.frames[0];
    PartEnc enc[4];
    /* mixed signs; both filters are marginally stable (roots on the unit
     * circle), so every coefficient's sign or historical position matters
     * to the PCM, while the signal itself stays bounded */
    static const int32_t q0[4] = {1, -1, 1, -1};
    static const int32_t q1[3] = {1, 1, -1};
    /* warm-ups chosen so the very first prediction is a negative,
     * shift-non-aligned sum (floor differs from truncation toward zero) */
    static const int32_t w0[4] = {2, 3, 5, 1};
    static const int32_t w1[3] = {5, 1, 1};
    int32_t resid[MAX_BLOCK];
    uint32_t i;

    memset(&pl, 0, sizeof(pl));
    pl.bps = 16;
    pl.nframes = 1;
    f->blocksize = 128;
    f->bps = 16;
    f->assign = ASSIGN_INDEPENDENT;
    f->sample_number = 0;

    memset(resid, 0, sizeof(resid));
    /* residuals with both signs and zeros; nothing large enough to
     * dominate the predictor's own history */
    for (i = 0; i < 128u - 4u; i++)
        resid[i] = (i % 11 == 0) ? 0 :
                   (i & 1) ? -(int32_t)(i % 7) : (int32_t)(i % 5);
    fill_enc_from_resid(resid, f->blocksize, 4, 2, 0, enc);
    make_lpc_plan(&f->sub[0], 4, 0, 2, w0, resid, f->blocksize, 0,
                  q0, 4, 2, enc);

    memset(resid, 0, sizeof(resid));
    for (i = 0; i < 128u - 3u; i++)
        resid[i] = (i % 9 == 0) ? 0 :
                   (i & 1) ? (int32_t)(i % 6) : -(int32_t)(i % 8);
    fill_enc_from_resid(resid, f->blocksize, 3, 2, 1, enc);
    make_lpc_plan(&f->sub[1], 3, 1, 2, w1, resid, f->blocksize, 0,
                  q1, 4, 2, enc);

    for (i = 0; i < f->blocksize; i++) {
        TASK_CHECK(fits_signed(f->sub[0].signal[i], 16));
        TASK_CHECK(fits_signed(f->sub[1].signal[i], 16));
    }
    /* prove the negative, non-aligned prediction (floor != truncation)
     * really happens at the first predicted sample of each subframe */
    {
        int64_t s0 = (int64_t)q0[0] * w0[3] + (int64_t)q0[1] * w0[2]
                   + (int64_t)q0[2] * w0[1] + (int64_t)q0[3] * w0[0];
        int64_t s1 = (int64_t)q1[0] * w1[2] + (int64_t)q1[1] * w1[1]
                   + (int64_t)q1[2] * w1[0];
        TASK_CHECK(s0 < 0 && (s0 & 3) != 0);
        TASK_CHECK(s1 < 0 && (s1 & 3) != 0);
        /* a negative, non-aligned sum distinguishes floor division (the
         * decoder's right shift) from C truncation toward zero */
        TASK_CHECK(sar_floor(s0, 2) != s0 / 4);
        TASK_CHECK(sar_floor(s1, 2) != s1 / 4);
    }
    pl.total_samples = f->blocksize;
    build_streaminfo(&pl);
    derive_channels(f);
    build_frame(&pl, f);
    return contract_stream(&pl);
}

static int case_prediction_lpc_wide(void)
{
    StreamPlan pl;
    FramePlan *f = &pl.frames[0];
    PartEnc enc[2];
    /* Large coefficients force the decoder's wide-accumulator path
     * (the decision uses the pre-shift worst-case bounds), while the
     * shift of 10 makes the normalized filters stable, so the actual
     * 24-bit signal stays small. Both signs occur and the values are not
     * shift-aligned, so floor rounding is exercised. */
    static const int32_t q0[2] = {400, -200};
    static const int32_t q1[3] = {320, -200, 120};
    static const int32_t w0[2] = {3, 1};
    static const int32_t w1[3] = {1, 3, 1};
    int32_t resid[MAX_BLOCK];
    uint32_t i;

    memset(&pl, 0, sizeof(pl));
    pl.bps = 24;
    pl.nframes = 1;
    f->blocksize = 256;
    f->bps = 24;
    f->assign = ASSIGN_INDEPENDENT;
    f->sample_number = 0;

    memset(resid, 0, sizeof(resid));
    for (i = 0; i < 256u - 2u; i++)
        resid[i] = (i % 13 == 0) ? 0 :
                   (i & 1) ? -(int32_t)(i % 5) : (int32_t)(i % 4);
    fill_enc_from_resid(resid, f->blocksize, 2, 1, 0, enc);
    make_lpc_plan(&f->sub[0], 2, 0, 1, w0, resid, f->blocksize, 0,
                  q0, 15, 10, enc);

    memset(resid, 0, sizeof(resid));
    for (i = 0; i < 256u - 3u; i++)
        resid[i] = (i % 17 == 0) ? 0 :
                   (i & 1) ? (int32_t)(i % 6) : -(int32_t)(i % 3);
    fill_enc_from_resid(resid, f->blocksize, 3, 1, 1, enc);
    make_lpc_plan(&f->sub[1], 3, 1, 1, w1, resid, f->blocksize, 0,
                  q1, 15, 10, enc);

    for (i = 0; i < f->blocksize; i++) {
        TASK_CHECK(fits_signed(f->sub[0].signal[i], 24));
        TASK_CHECK(fits_signed(f->sub[1].signal[i], 24));
    }
    /* worst-case prediction before shift exceeds 32 bits: the decoder
     * must select FLAC__lpc_restore_signal_wide */
    {
        uint64_t max_sample = (uint64_t)1 << 23;
        uint64_t max_pred = max_sample * (400u + 200u);
        TASK_CHECK(max_pred > (uint64_t)INT32_MAX);
    }
    /* negative, non-aligned sums occur here as well */
    {
        int64_t s0 = (int64_t)q0[0] * w0[1] + (int64_t)q0[1] * w0[0];
        int64_t s1 = (int64_t)q1[0] * w1[2] + (int64_t)q1[1] * w1[1]
                   + (int64_t)q1[2] * w1[0];
        TASK_CHECK(s0 < 0 && (s0 & 1023) != 0);
        TASK_CHECK(s1 < 0 && (s1 & 1023) != 0);
    }
    pl.total_samples = f->blocksize;
    build_streaminfo(&pl);
    derive_channels(f);
    build_frame(&pl, f);
    return contract_stream(&pl);
}

static int case_prediction_wasted(void)
{
    StreamPlan pl;
    FramePlan *f = &pl.frames[0];
    PartEnc enc[4];
    static const int32_t q[2] = {5, -3};
    static const int32_t wf[2] = {37, -41};
    static const int32_t wl[2] = {-55, 29};
    static const uint32_t wasted[2] = {3u, 2u};
    int32_t resid[MAX_BLOCK];
    uint32_t i;

    memset(&pl, 0, sizeof(pl));
    pl.bps = 24;
    pl.nframes = 1;
    f->blocksize = 64;
    f->bps = 24;
    f->assign = ASSIGN_INDEPENDENT;
    f->sample_number = 0;

    /* fixed order 2 subframe with 3 wasted bits */
    memset(resid, 0, sizeof(resid));
    put_pulse(resid, 4, 2, 300);
    put_pulse(resid, 30, 2, -250);
    resid[2] = 0;
    fill_enc_from_resid(resid, f->blocksize, 2, 2, 0, enc);
    make_fixed_plan(&f->sub[0], 2, 0, 2, wf, resid, f->blocksize,
                    wasted[0], enc);

    /* LPC order 2 subframe with 2 wasted bits */
    memset(resid, 0, sizeof(resid));
    put_pulse(resid, 4, 2, 120);
    for (i = 0; i < 64u - 2u; i++)
        if (resid[i] == 0 && i % 7 == 0)
            resid[i] = (i & 1) ? -17 : 13;
    fill_enc_from_resid(resid, f->blocksize, 2, 2, 1, enc);
    make_lpc_plan(&f->sub[1], 2, 1, 2, wl, resid, f->blocksize,
                  wasted[1], q, 4, 2, enc);

    for (i = 0; i < f->blocksize; i++) {
        TASK_CHECK(fits_signed(f->sub[0].signal[i], 24 - wasted[0]));
        TASK_CHECK(fits_signed(f->sub[1].signal[i], 24 - wasted[1]));
    }
    pl.total_samples = f->blocksize;
    build_streaminfo(&pl);
    /* independent assignment copies the effective-width signals into want;
     * shift them back up by the wasted bits to match the decoder output */
    derive_channels(f);
    for (i = 0; i < f->blocksize; i++) {
        f->want[0][i] = f->sub[0].signal[i] << wasted[0];
        f->want[1][i] = f->sub[1].signal[i] << wasted[1];
        TASK_CHECK(f->want[0][i] == (f->sub[0].signal[i] << wasted[0]));
        TASK_CHECK(f->want[1][i] == (f->sub[1].signal[i] << wasted[1]));
        TASK_CHECK(fits_signed(f->want[0][i], 24));
        TASK_CHECK(fits_signed(f->want[1][i], 24));
    }
    build_frame(&pl, f);
    return contract_stream(&pl);
}

/* ================================================================== */
/* stream group                                                        */
/* ================================================================== */

static void fill_frame(FramePlan *f, uint32_t idx, uint32_t assign,
                       uint32_t blocksize, uint32_t bps,
                       uint64_t sample_number)
{
    int32_t sig[2][MAX_BLOCK];
    PartEnc enc[4];
    uint32_t ch, i;
    f->blocksize = blocksize;
    f->bps = bps;
    f->assign = assign;
    f->sample_number = sample_number;

    for (ch = 0; ch < 2; ch++)
        for (i = 0; i < blocksize; i++)
            sig[ch][i] = gen_value(i, idx * 17u + ch * 29u,
                                   bps == 16 ? 11000 : 120000);

    for (ch = 0; ch < 2; ch++) {
        enc[0] = (PartEnc){0, 2u + (ch & 1), 0};
        enc[1] = (PartEnc){1, 0u, 0};
        enc[2] = (PartEnc){0, 3u, 0};
        enc[3] = (PartEnc){1, 0u, 8u + ch};
        if ((idx + ch) & 1) {
            static const int32_t q[3] = {6, -4, 2};
            make_lpc(&f->sub[ch], 3, ch, 2, sig[ch], blocksize, 0,
                     q, 4, 1 + (idx & 1), enc);
        }
        else {
            make_fixed(&f->sub[ch], 1 + (idx & 3), ch, 2, sig[ch],
                       blocksize, 0, enc);
        }
        /* sizes the escape partitions from the actual residuals */
        reencode_subframe(&f->sub[ch], blocksize);
    }
}

static int case_stream_independent(void)
{
    StreamPlan pl;
    static const uint32_t blocks[4] = {64u, 128u, 256u, 96u};
    uint32_t k;
    memset(&pl, 0, sizeof(pl));
    pl.bps = 16;
    pl.nframes = 4;
    for (k = 0; k < pl.nframes; k++) {
        FramePlan *f = &pl.frames[k];
        fill_frame(f, k, ASSIGN_INDEPENDENT, blocks[k], 16,
                   pl.total_samples);
        derive_channels(f);
        pl.total_samples += f->blocksize;
    }
    build_streaminfo(&pl);
    for (k = 0; k < pl.nframes; k++)
        build_frame(&pl, &pl.frames[k]);
    return contract_stream(&pl);
}

static int case_stream_left_side(void)
{
    StreamPlan pl;
    static const uint32_t blocks[4] = {128u, 64u, 200u, 256u};
    uint32_t k, i;
    memset(&pl, 0, sizeof(pl));
    pl.bps = 16;
    pl.nframes = 4;
    for (k = 0; k < pl.nframes; k++) {
        FramePlan *f = &pl.frames[k];
        int32_t left[MAX_BLOCK], side[MAX_BLOCK];
        PartEnc enc[4] = {{0, 1u, 0}, {0, 1u, 0}, {0, 1u, 0}, {0, 1u, 0}};
        fill_frame(f, k, ASSIGN_LEFT_SIDE, blocks[k], 16,
                   pl.total_samples);
        /* rebuild from explicit left/side PCM: sub0 = left, sub1 = side */
        for (i = 0; i < f->blocksize; i++) {
            int32_t l = gen_value(i, k * 31u, 10000);
            int32_t r = gen_value(i, k * 31u + 5u, 10000);
            left[i] = l;
            side[i] = l - r;
            TASK_CHECK(fits_signed(side[i], 17));
        }
        make_fixed(&f->sub[0], 2, 0, 2, left, f->blocksize, 0, enc);
        make_fixed(&f->sub[1], 3, 1, 2, side, f->blocksize, 0, enc);
        reencode_subframe(&f->sub[0], f->blocksize);
        reencode_subframe(&f->sub[1], f->blocksize);
        derive_channels(f);
        pl.total_samples += f->blocksize;
    }
    build_streaminfo(&pl);
    for (k = 0; k < pl.nframes; k++)
        build_frame(&pl, &pl.frames[k]);
    return contract_stream(&pl);
}

static int case_stream_right_side(void)
{
    StreamPlan pl;
    static const uint32_t blocks[4] = {200u, 256u, 64u, 128u};
    uint32_t k, i;
    memset(&pl, 0, sizeof(pl));
    pl.bps = 24;
    pl.nframes = 4;
    for (k = 0; k < pl.nframes; k++) {
        FramePlan *f = &pl.frames[k];
        int32_t right[MAX_BLOCK], side[MAX_BLOCK];
        PartEnc enc[4] = {{0, 1u, 0}, {0, 1u, 0}, {0, 1u, 0}, {0, 1u, 0}};
        fill_frame(f, k, ASSIGN_RIGHT_SIDE, blocks[k], 24,
                   pl.total_samples);
        /* sub0 = side (extra bit), sub1 = right */
        for (i = 0; i < f->blocksize; i++) {
            int32_t l = gen_value(i, k * 41u + 2u, 100000);
            int32_t r = gen_value(i, k * 41u + 9u, 100000);
            right[i] = r;
            side[i] = l - r;
            TASK_CHECK(fits_signed(side[i], 25));
        }
        make_fixed(&f->sub[0], 1, 0, 2, side, f->blocksize, 0, enc);
        make_fixed(&f->sub[1], 4, 1, 2, right, f->blocksize, 0, enc);
        reencode_subframe(&f->sub[0], f->blocksize);
        reencode_subframe(&f->sub[1], f->blocksize);
        derive_channels(f);
        pl.total_samples += f->blocksize;
    }
    build_streaminfo(&pl);
    for (k = 0; k < pl.nframes; k++)
        build_frame(&pl, &pl.frames[k]);
    return contract_stream(&pl);
}

static int case_stream_mid_side(void)
{
    StreamPlan pl;
    static const uint32_t blocks[4] = {64u, 128u, 256u, 200u};
    uint32_t k, i;
    memset(&pl, 0, sizeof(pl));
    pl.bps = 16;
    pl.nframes = 4;
    for (k = 0; k < pl.nframes; k++) {
        FramePlan *f = &pl.frames[k];
        int32_t mid[MAX_BLOCK], side[MAX_BLOCK];
        PartEnc enc[4] = {{0, 1u, 0}, {0, 1u, 0}, {0, 1u, 0}, {0, 1u, 0}};
        fill_frame(f, k, ASSIGN_MID_SIDE, blocks[k], 16,
                   pl.total_samples);
        for (i = 0; i < f->blocksize; i++) {
            int32_t l = gen_value(i, k * 23u + 1u, 9000);
            int32_t r = gen_value(i, k * 23u + 8u, 9000);
            if (i == 0) { l = -5; r = -1; }  /* mid = -3: negative odd */
            if (i == 1) { l = -4; r = 1; }   /* side = -5: odd */
            if (i == 2) { l = 7; r = 2; }    /* mid = 4, side = 5 odd */
            if (i == 3) { l = -8; r = -3; }  /* mid floor(-11/2) = -6 */
            mid[i] = (int32_t)sar_floor((int64_t)l + r, 1);
            side[i] = l - r;
            TASK_CHECK(fits_signed(mid[i], 16));
            TASK_CHECK(fits_signed(side[i], 17));
        }
        TASK_CHECK(mid[0] == -3 && (mid[0] & 1));
        TASK_CHECK((side[1] & 1) && (side[2] & 1));
        make_fixed(&f->sub[0], 2, 0, 2, mid, f->blocksize, 0, enc);
        make_fixed(&f->sub[1], 3, 1, 2, side, f->blocksize, 0, enc);
        reencode_subframe(&f->sub[0], f->blocksize);
        reencode_subframe(&f->sub[1], f->blocksize);
        derive_channels(f);
        /* verify the parity corner samples reconstruct the exact L/R */
        TASK_CHECK(f->want[0][0] == -5 && f->want[1][0] == -1);
        TASK_CHECK(f->want[0][1] == -4 && f->want[1][1] == 1);
        TASK_CHECK(f->want[0][2] == 7 && f->want[1][2] == 2);
        TASK_CHECK(f->want[0][3] == -8 && f->want[1][3] == -3);
        pl.total_samples += f->blocksize;
    }
    build_streaminfo(&pl);
    for (k = 0; k < pl.nframes; k++)
        build_frame(&pl, &pl.frames[k]);
    return contract_stream(&pl);
}

/* ------------------------------------------------------------------ */
/* library-internal roundtrip kept as a compatibility reference        */
/* (only this case lets the production encoder build bytes)            */
/* ------------------------------------------------------------------ */

typedef struct RTCheck_s {
    const int32_t *pcm;
    uint32_t n;
    uint32_t got;
    int fail;
} RTCheck;

typedef struct {
    uint8_t buf[65536];
    size_t len;
    size_t pos;
    int fail;
    int dec_errors;
    RTCheck check;
} RT;

static void rt_error(const FLAC__StreamDecoder *d,
                     FLAC__StreamDecoderErrorStatus s, void *v)
{
    RT *r = v;
    (void)d;
    r->dec_errors++;
    fprintf(stderr, "roundtrip decoder error status=%d\n", (int)s);
}

static FLAC__StreamEncoderWriteStatus
rt_write(const FLAC__StreamEncoder *e, const FLAC__byte *p, size_t n,
         uint32_t samples, uint32_t frame, void *v)
{
    RT *r = v;
    (void)e; (void)samples; (void)frame;
    if (r->pos + n > sizeof(r->buf)) {
        r->fail++;
        return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    }
    memcpy(r->buf + r->pos, p, n);
    r->pos += n;
    if (r->pos > r->len)
        r->len = r->pos;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static FLAC__StreamEncoderSeekStatus
rt_seek(const FLAC__StreamEncoder *e, FLAC__uint64 off, void *v)
{
    RT *r = v;
    (void)e;
    if (off > sizeof(r->buf))
        return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
    r->pos = (size_t)off;
    return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

static FLAC__StreamEncoderTellStatus
rt_tell(const FLAC__StreamEncoder *e, FLAC__uint64 *off, void *v)
{
    (void)e;
    *off = ((RT *)v)->pos;
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderWriteStatus
rt_dec_write(const FLAC__StreamDecoder *d, const FLAC__Frame *frame,
             const FLAC__int32 * const p[], void *v)
{
    RT *r = v;
    RTCheck *c = &r->check;
    uint32_t i, ch;
    (void)d;
    for (i = 0; i < frame->header.blocksize; i++)
        for (ch = 0; ch < 2; ch++)
            if (!task_check(p[ch][i] == c->pcm[(c->got + i) * 2u + ch],
                            "roundtrip pcm", __FILE__, __LINE__))
                c->fail++;
    c->got += frame->header.blocksize;
    return c->fail ? FLAC__STREAM_DECODER_WRITE_STATUS_ABORT
                   : FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static FLAC__StreamDecoderReadStatus
rt_dec_read(const FLAC__StreamDecoder *d, FLAC__byte *buf, size_t *n,
            void *v)
{
    RT *r = v;
    size_t want = *n;
    (void)d;
    if (r->len == 0) {
        *n = 0;
        return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }
    if (want > r->len)
        want = r->len;
    memcpy(buf, r->buf, want);
    memmove(r->buf, r->buf + want, r->len - want);
    r->len -= want;
    *n = want;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static int case_stream_lib_roundtrip(void)
{
    enum { N = 320 };
    static int32_t pcm[N * 2];
    RT r;
    FLAC__StreamEncoder *e;
    FLAC__StreamDecoder *d;
    uint32_t i;
    for (i = 0; i < N; i++) {
        pcm[2 * i] = gen_value(i, 4u, 15000);
        pcm[2 * i + 1] = -pcm[2 * i] / 2 + (int32_t)(i % 7);
    }
    memset(&r, 0, sizeof(r));
    e = FLAC__stream_encoder_new();
    TASK_CHECK(e != 0);
    TASK_CHECK(FLAC__stream_encoder_set_channels(e, 2));
    TASK_CHECK(FLAC__stream_encoder_set_bits_per_sample(e, 16));
    TASK_CHECK(FLAC__stream_encoder_set_sample_rate(e, SAMPLE_RATE));
    TASK_CHECK(FLAC__stream_encoder_set_blocksize(e, 64));
    TASK_CHECK(FLAC__stream_encoder_set_verify(e, 1));
    TASK_CHECK(FLAC__stream_encoder_init_stream(
                   e, rt_write, rt_seek, rt_tell, 0, &r) ==
               FLAC__STREAM_ENCODER_INIT_STATUS_OK);
    TASK_CHECK(FLAC__stream_encoder_process_interleaved(e, pcm, N));
    TASK_CHECK(FLAC__stream_encoder_finish(e));
    FLAC__stream_encoder_delete(e);
    TASK_CHECK(r.fail == 0 && r.len > 0);

    r.check.pcm = pcm;
    r.check.n = N;
    d = FLAC__stream_decoder_new();
    TASK_CHECK(d != 0);
    TASK_CHECK(FLAC__stream_decoder_init_stream(
                   d, rt_dec_read, 0, 0, 0, 0, rt_dec_write, 0,
                   rt_error, &r) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
    TASK_CHECK(FLAC__stream_decoder_process_until_end_of_stream(d));
    TASK_CHECK(FLAC__stream_decoder_get_state(d) ==
               FLAC__STREAM_DECODER_END_OF_STREAM);
    TASK_CHECK(FLAC__stream_decoder_finish(d));
    FLAC__stream_decoder_delete(d);
    TASK_CHECK(r.dec_errors == 0);
    TASK_CHECK(r.check.fail == 0);
    TASK_CHECK(r.check.got == N);
    return 0;
}

/* ================================================================== */
/* registration                                                        */
/* ================================================================== */

const struct task_case task_cases[] = {
    {"residuals", "rice_partitions", case_residuals_rice},
    {"residuals", "rice2_partitions", case_residuals_rice2},
    {"prediction", "fixed_orders", case_prediction_fixed},
    {"prediction", "lpc_mixed_coefficients", case_prediction_lpc_mix},
    {"prediction", "lpc_wide_accumulator", case_prediction_lpc_wide},
    {"prediction", "wasted_bits", case_prediction_wasted},
    {"stream", "independent_frames", case_stream_independent},
    {"stream", "left_side_frames", case_stream_left_side},
    {"stream", "right_side_frames", case_stream_right_side},
    {"stream", "mid_side_frames", case_stream_mid_side},
    {"stream", "library_roundtrip", case_stream_lib_roundtrip},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
