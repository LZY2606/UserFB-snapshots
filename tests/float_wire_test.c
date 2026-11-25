////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//        Bit-level compatibility regression for the float sideband        //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// float_wire_test.c
//
// Independent regression that pins the on-wire behavior of the 32-bit float
// WVX sideband, not just the numeric behavior of the codec. It combines:
//
//   - the self-contained IEEE-754 reference model in float_wire_model.[ch],
//     which predicts shared exponents, main integers, float_shift, the
//     per-sample float_flags paths and the CRC without any float arithmetic;
//   - the public container/chunk traversal in float_wire_packets.[ch], which
//     lets the same model verify both encoded and decoded directions;
//   - real encode/decode through WavpackOpenFileOutput(), PackSamples() and
//     the public reader API for stereo and six-channel material;
//   - handcrafted blocks exercising the new WVX min_shifted_zeros /
//     max_shifted_ones omission windows and sideband cursor positioning.
//
// Every comparison is an exact uint32 bit pattern or an exact bit sequence,
// so negative zero, NaN payloads and subnormals cannot silently change.

#include "check.h"
#include "wavpack.h"
#include "float_wire_model.h"
#include "float_wire_packets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////// tiny MD5 ///////////////////////////////////
//
// Standard RFC 1321 MD5, implemented locally so the regression can prove the
// stored-MD5 round trip independently of any production helper.

typedef struct {
    uint32_t state [4];
    uint64_t bitlen;
    unsigned char buf [64];
    size_t buflen;
} fwt_md5;

static const uint32_t fwt_md5_k [64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

static const int fwt_md5_s [64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5,9,14,20, 5,9,14,20, 5,9,14,20, 5,9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

#define FWT_LROT(x,n) (((x) << (n)) | ((x) >> (32 - (n))))

static void fwt_md5_compress (fwt_md5 *c, const unsigned char block [64])
{
    uint32_t m [16], a, b, cc, d;
    int i;

    for (i = 0; i < 16; i++)
        m [i] = (uint32_t) block [i * 4] | ((uint32_t) block [i * 4 + 1] << 8) |
            ((uint32_t) block [i * 4 + 2] << 16) | ((uint32_t) block [i * 4 + 3] << 24);

    a = c->state [0]; b = c->state [1]; cc = c->state [2]; d = c->state [3];

    for (i = 0; i < 64; i++) {
        uint32_t f, g;

        if (i < 16) { f = (b & cc) | (~b & d); g = (uint32_t) i; }
        else if (i < 32) { f = (d & b) | (~d & cc); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d; g = (3 * i + 5) & 15; }
        else { f = cc ^ (b | ~d); g = (7 * i) & 15; }

        f = f + a + fwt_md5_k [i] + m [g];
        a = d; d = cc; cc = b;
        b = b + FWT_LROT (f, fwt_md5_s [i]);
    }

    c->state [0] += a; c->state [1] += b; c->state [2] += cc; c->state [3] += d;
}

static void fwt_md5_init (fwt_md5 *c)
{
    c->state [0] = 0x67452301; c->state [1] = 0xefcdab89;
    c->state [2] = 0x98badcfe; c->state [3] = 0x10325476;
    c->bitlen = 0; c->buflen = 0;
}

static void fwt_md5_update (fwt_md5 *c, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *) data;
    c->bitlen += (uint64_t) len * 8;

    while (len) {
        size_t take = 64 - c->buflen;

        if (take > len)
            take = len;

        memcpy (c->buf + c->buflen, p, take);
        c->buflen += take; p += take; len -= take;

        if (c->buflen == 64) {
            fwt_md5_compress (c, c->buf);
            c->buflen = 0;
        }
    }
}

static void fwt_md5_final (fwt_md5 *c, unsigned char out [16])
{
    uint64_t bits = c->bitlen;
    int i;

    c->buf [c->buflen++] = 0x80;

    if (c->buflen > 56) {
        while (c->buflen < 64)
            c->buf [c->buflen++] = 0;

        fwt_md5_compress (c, c->buf);
        c->buflen = 0;
    }

    while (c->buflen < 56)
        c->buf [c->buflen++] = 0;

    for (i = 0; i < 8; i++)
        c->buf [56 + i] = (unsigned char) (bits >> (i * 8));

    fwt_md5_compress (c, c->buf);

    for (i = 0; i < 4; i++) {
        out [i * 4]     = (unsigned char) c->state [i];
        out [i * 4 + 1] = (unsigned char) (c->state [i] >> 8);
        out [i * 4 + 2] = (unsigned char) (c->state [i] >> 16);
        out [i * 4 + 3] = (unsigned char) (c->state [i] >> 24);
    }
}

////////////////////////////////// test material //////////////////////////////

// Explicit IEEE-754 patterns built from fields, never from float arithmetic.

#define P(sign,exp,mant) fwm_make (sign, exp, mant)

// Anchors shared by several groups. The block maximum finite value is chosen
// deliberately so that every other sample's representation shifts against it.
// Block exponents are the rounded values: field(max_mag + 0x7f0000), matching
// scan_float_data(), not the raw exponent fields.

#define G_MAX           0x447a0000u     // 1000.0, raw exponent 136
#define G_MAX_EXP       137             // rounded block exponent with 0x7f0000
#define G_BIG_MAX       0x7f7fffffu     // largest finite float, exponent 254
#define G_BIG_MAX_EXP   255             // rounded block exponent with 0x7f0000
#define G_W_MAX         0x42c80000u     // 100.0, raw exponent 134
#define G_W_EXP         134             // rounded block exponent with 0x7f0000
#define G_W_SAMPLE      0x41200000u     // exponent 130, shift_count 4 vs 100.0
#define G_PURE_MAX      0x44800000u     // 1024.0, zero mantissa so its own
#define G_PURE_EXP      137             //   shift_count is zero (max_exp == exp)

typedef struct {
    const char     *name;
    const uint32_t *patterns;
    int             n;
    unsigned        expect_flags;
    int             expect_max_exp;
    int             expect_shift;
    int             expect_wvx;
} fwt_group;

// G1: SHIFT_SENT with every special value class and the max_exp >= 25 false
// zero exponent byte path.

static const uint32_t g_sent [] = {
    G_MAX,                                  // block maximum, exponent 136
    0x3f800000u,                            // +1.0, nine low zeros shifted out
    0x3f8001ffu,                            // nine low ones shifted out
    0x3f8000abu,                            // mixed low bits -> SHIFT_SENT
    0xbf800000u,                            // -1.0
    0x33800000u,                            // tiny finite: main integer goes to zero
    0x00000001u,                            // smallest positive subnormal
    0x807fffffu,                            // largest negative subnormal
    0x00000000u,                            // positive zero
    0x80000000u,                            // negative zero
    0x7f800000u,                            // positive infinity
    0xff800000u,                            // negative infinity
    0x7fc12345u,                            // positive quiet NaN with payload
    0xff812345u,                            // negative NaN with a different payload
    0x7f800001u,                            // smallest NaN payload
    0x3f7fffffu                             // largest float below 1.0
};

// G2: SHIFT_ONES only (every shifted low part is all ones for its shift),
// plus zeros. Mantissa low parts are built to fill the whole shift mask.

static const uint32_t g_ones [] = {
    G_PURE_MAX,                             // block maximum, its own shift is zero
    0x3f8003ffu,                            // shift 10, low 10 bits all ones
    0x400001ffu,                            // shift 9, low 9 bits all ones
    0x3e800fffu,                            // shift 12, low 12 bits all ones
    0xbe800fffu,                            // negative counterpart
    0x00000000u,                            // positive zero
    0x80000000u                             // negative zero forces sign bits
};

// G3: SHIFT_SAME, shifted low parts are all ones or all zeros for their
// shift count, never mixed; the two subnormals are false zeros.

static const uint32_t g_same [] = {
    G_MAX,
    0x3f8003ffu,                            // shift 10, ones
    0x3f800000u,                            // shift 10, zeros
    0x40000000u,                            // shift 9, zeros (2.0)
    0xc00001ffu,                            // shift 9, ones, negative
    0x007fffffu,                            // largest subnormal: false zero
    0x00800000u                             // smallest normal: false zero
};

// G4: integer-precision floats only; no sideband is required at all.

static const uint32_t g_integer [] = {
    G_MAX,                                  // 1000.0 is itself an integer
    0x3f800000u,                            // 1.0
    0x40000000u,                            // 2.0
    0xc0800000u,                            // -4.0
    0x41200000u,                            // 10.0
    0x41000000u,                            // 8.0
    0xbf800000u,                            // -1.0
    0x00000000u                             // true positive zero
};

// G5: the largest finite value dominates, pushing every small finite value
// into false-zero territory (max_exp 255, exponent bytes required).

static const uint32_t g_big [] = {
    G_BIG_MAX,                              // largest finite float
    0x00800000u,                            // smallest normal -> false zero
    0x33800000u,                            // tiny finite -> false zero
    0x00000001u,                            // smallest subnormal -> false zero
    0x80000001u,                            // negative smallest subnormal
    0x007fffffu,                            // largest subnormal
    0x3f800000u,                            // 1.0 -> false zero at this scale
    0x80000000u,                            // negative zero
    0x7fc12345u,                            // NaN payload
    0xff800000u                             // negative infinity
};

// G6a: max_shifted_ones material. shift_count is 4; with max_shifted_ones=3
// the reader restores one low zero bit, so only the top three low bits are
// transmitted. Every pattern keeps bit 0 clear, matching the restored zero.

static const uint32_t g_window_ones [] = {
    G_W_MAX,                                // exponent 134, block maximum
    G_W_SAMPLE | 0xeu,                      // low 1110: sent 111, zero restored
    G_W_SAMPLE | 0xau,                      // low 1010: sent 101
    G_W_SAMPLE | 0x6u,                      // low 0110: sent 011
    G_W_SAMPLE | 0x2u,                      // low 0010: sent 001
    G_W_SAMPLE                              // low 0000: sent 000
};

// G6b: min_shifted_zeros material. Two low bits are omitted as zeros and only
// the top two shifted bits are transmitted, so every pattern has bits 1 and 0
// clear by construction.

static const uint32_t g_window_zeros [] = {
    G_W_MAX,
    G_W_SAMPLE | 0xcu,                      // low 1100: sent 11, two zeros
    G_W_SAMPLE | 0x8u,                      // low 1000: sent 10
    G_W_SAMPLE | 0x4u,                      // low 0100: sent 01
    G_W_SAMPLE                              // low 0000: sent 00
};

static const fwt_group groups [] = {
    { "shift_sent",  g_sent,    (int) sizeof g_sent / sizeof *g_sent,
      FWM_SHIFT_SENT | FWM_ZEROS_SENT | FWM_NEG_ZEROS | FWM_EXCEPTIONS, G_MAX_EXP, 0, 1 },
    { "shift_ones",  g_ones,    (int) sizeof g_ones / sizeof *g_ones,
      FWM_SHIFT_ONES | FWM_ZEROS_SENT | FWM_NEG_ZEROS, G_MAX_EXP, 0, 1 },
    { "shift_same",  g_same,    (int) sizeof g_same / sizeof *g_same,
      FWM_SHIFT_SAME | FWM_ZEROS_SENT, G_MAX_EXP, 0, 1 },
    { "integer",     g_integer, (int) sizeof g_integer / sizeof *g_integer,
      0, G_MAX_EXP, -1, 0 },
    { "big_scale",   g_big,     (int) sizeof g_big / sizeof *g_big,
      FWM_SHIFT_ONES | FWM_ZEROS_SENT | FWM_NEG_ZEROS | FWM_EXCEPTIONS,
      G_BIG_MAX_EXP, 0, 1 }
};

#define FWT_NGROUPS ((int) (sizeof groups / sizeof *groups))

// The public encoder rejects custom block sizes below 16 frames. Small
// handcrafted groups are therefore repeated cyclically to 20 frames; cycling
// introduces no new field class, so the block-wide scheme, max_exp and
// float_shift stay identical while every bit count is still model-derived.

#define FWT_BLOCK_FRAMES 20

static int expand_patterns (const uint32_t *src, int src_n, uint32_t dst [64])
{
    int n = (src_n >= 16) ? src_n : FWT_BLOCK_FRAMES;
    int i;

    for (i = 0; i < n; i++)
        dst [i] = src [i % src_n];

    return n;
}

static const char *flag_names (unsigned flags, char *buf, size_t n)
{
    size_t used = 0;
    int k;

    struct { unsigned bit; const char *name; } table [] = {
        { FWM_SHIFT_ONES, "SHIFT_ONES" }, { FWM_SHIFT_SAME, "SHIFT_SAME" },
        { FWM_SHIFT_SENT, "SHIFT_SENT" }, { FWM_ZEROS_SENT, "ZEROS_SENT" },
        { FWM_NEG_ZEROS, "NEG_ZEROS" }, { FWM_EXCEPTIONS, "EXCEPTIONS" }
    };

    buf [0] = 0;

    for (k = 0; k < (int) (sizeof table / sizeof *table); k++)
        if (flags & table [k].bit)
            used += (size_t) snprintf (buf + used, n - used, "%s%s",
                used ? "|" : "", table [k].name);

    if (!used)
        snprintf (buf, n, "none");

    return buf;
}

////////////////////////////////// model self test ////////////////////////////

// Pure model round trip: scan, emit, restore. This pins the reference itself
// and proves per-sample sideband offsets describe one shared stream.

static void test_model_group (const fwt_group *g)
{
    fwm_item items [64];
    fwm_plan plan;
    int32_t integers [64];
    uint32_t restored [64];
    unsigned char sidebuf [256];
    fwm_bits bs;
    int valid_bits, i;

    TASK_CHECK (g->n <= 64);

    valid_bits = fwm_scan (g->patterns, g->n, integers, items, &plan);

    TASK_CHECK (plan.flags == g->expect_flags);
    TASK_CHECK (plan.max_exp == g->expect_max_exp);
    TASK_CHECK (!!plan.wvx_needed == g->expect_wvx);

    if (g->expect_shift >= 0)
        TASK_CHECK (plan.float_shift == g->expect_shift);

    fwm_bits_init_write (&bs, sidebuf, sizeof sidebuf);
    fwm_send (g->patterns, g->n, items, &plan, &bs);
    TASK_CHECK (!bs.error);
    TASK_CHECK (bs.bitpos == valid_bits);

    // Adjacent samples share one sideband: offsets must be a strict walk of
    // the per-item widths with no per-sample realignment.
    {
        int cursor = 0;

        for (i = 0; i < g->n; i++) {
            TASK_CHECK (items [i].bit_offset == cursor);
            cursor += items [i].bit_count;
        }

        TASK_CHECK (cursor == valid_bits);
    }

    fwm_bits_init_read (&bs, sidebuf, sizeof sidebuf);
    {
        uint32_t crc = 0xffffffffu;

        fwm_restore (integers, g->n, &plan, 0, 0, &bs, restored, &crc);
        TASK_CHECK (!bs.error);
        TASK_CHECK (bs.bitpos == valid_bits);       // exact consumed length
        TASK_CHECK (crc == plan.crc);

        for (i = 0; i < g->n; i++)
            TASK_CHECK (restored [i] == g->patterns [i]);
    }
}

static void test_model_integer_group (void)
{
    const fwt_group *g = &groups [3];
    fwm_item items [64];
    fwm_plan plan;
    int32_t integers [64];
    uint32_t restored [64];
    uint32_t crc = 0xffffffffu;
    int i;

    fwm_scan (g->patterns, g->n, integers, items, &plan);
    TASK_CHECK (plan.flags == 0);
    TASK_CHECK (plan.wvx_needed == 0);
    TASK_CHECK (plan.float_shift > 0);             // trailing integer zeros stripped

    fwm_restore_nowvx (integers, g->n, &plan, restored, &crc);
    TASK_CHECK (crc == plan.crc);

    for (i = 0; i < g->n; i++)
        TASK_CHECK (restored [i] == g->patterns [i]);
}

static void test_model_self (void)
{
    int k;

    for (k = 0; k < FWT_NGROUPS; k++)
        test_model_group (&groups [k]);

    test_model_integer_group ();
}

////////////////////////////////// public encoding ////////////////////////////

static int fwt_block_out (void *id, void *data, int32_t n)
{
    return fwp_sink_block (id, data, n);
}

static WavpackContext *encode_monophonic (const fwt_group *g, fwp_sink *sink,
                                          unsigned char *storage, size_t cap,
                                          unsigned char md5 [16])
{
    WavpackConfig cfg;
    WavpackContext *wpc;
    int32_t frames [64];
    fwt_md5 md;
    int i;

    TASK_CHECK (g->n <= 64);
    memcpy (frames, g->patterns, (size_t) g->n * sizeof *frames);

    fwt_md5_init (&md);
    fwt_md5_update (&md, frames, (size_t) g->n * sizeof *frames);
    fwt_md5_final (&md, md5);

    memset (&cfg, 0, sizeof cfg);
    cfg.bytes_per_sample = 4;
    cfg.bits_per_sample = 32;
    cfg.float_norm_exp = 127;
    cfg.num_channels = 1;
    cfg.channel_mask = 4;             // mono, front center
    cfg.sample_rate = 48000;
    cfg.block_samples = (int32_t) g->n;
    cfg.flags = CONFIG_MD5_CHECKSUM;  // persist the MD5 capability on decode

    fwp_sink_init (sink, storage, cap);
    wpc = WavpackOpenFileOutput (fwt_block_out, sink, NULL);
    TASK_CHECK (wpc != NULL);
    TASK_CHECK (WavpackSetConfiguration64 (wpc, &cfg, g->n, NULL));
    TASK_CHECK (WavpackPackInit (wpc));
    TASK_CHECK (WavpackPackSamples (wpc, frames, (uint32_t) g->n));
    TASK_CHECK (WavpackFlushSamples (wpc));
    TASK_CHECK (WavpackStoreMD5Sum (wpc, md5));
    TASK_CHECK (WavpackFlushSamples (wpc));
    WavpackCloseFile (wpc);
    TASK_CHECK (!sink->failed);
    TASK_CHECK (sink->len > sizeof (WavpackHeader));

    for (i = 0; i < g->n; i++)
        TASK_CHECK ((uint32_t) frames [i] == g->patterns [i]);   // in-place contract

    return NULL;
}

// First-difference diagnostics: when a bit-level comparison fails this prints
// exactly which layer (FLOAT_INFO, sideband bit or reconstructed pattern) and
// which sample diverged first, instead of only saying the file differed.

static void report_group (const char *what, const fwt_group *g)
{
    fprintf (stderr, "FAIL first difference in group '%s' at %s\n", g->name, what);
}

#define REQUIRE(cond,what) do { task_checks++; if (!(cond)) { report_group ((what), g); exit (42); } } while (0)

// Compare the model plan against the actual ID_FLOAT_INFO chunk and the
// predicted LSB-first sideband bits against the physical WVX chunk, including
// its consumed length, CRC and padding (padding excluded from valid bits).

static void compare_wvx_chunk (const fwt_group *g, const fwp_block *block,
                                const fwm_plan *plan, const fwm_item *items,
                                int valid_bits, const unsigned char *expected)
{
    const fwp_chunk *info, *wvx;
    char namebuf [128];

    info = fwp_find_chunk (block, ID_FLOAT_INFO);
    REQUIRE (info != NULL, "missing ID_FLOAT_INFO chunk");
    REQUIRE (info->byte_length == 4, "FLOAT_INFO length");
    REQUIRE (info->data [0] == plan->flags, "float_flags byte");
    REQUIRE (info->data [1] == (unsigned char) plan->float_shift, "float_shift byte");
    REQUIRE (info->data [2] == (unsigned char) plan->max_exp, "float_max_exp byte");
    REQUIRE (info->data [3] == 127, "float_norm_exp byte");

    if (!plan->wvx_needed) {
        REQUIRE (fwp_find_chunk (block, ID_WVX_BITSTREAM) == NULL, "unexpected WVX chunk");
        REQUIRE (fwp_find_chunk (block, ID_WVX_NEW_BITSTREAM) == NULL, "unexpected new WVX chunk");
        return;
    }

    wvx = fwp_find_chunk (block, ID_WVX_BITSTREAM);
    REQUIRE (wvx != NULL, "missing ID_WVX_BITSTREAM chunk");
    REQUIRE (wvx->byte_length > 4, "WVX too short for CRC");

    {
        uint32_t stored_crc = (uint32_t) wvx->data [0] | ((uint32_t) wvx->data [1] << 8) |
            ((uint32_t) wvx->data [2] << 16) | ((uint32_t) wvx->data [3] << 24);
        int physical_bits = (wvx->byte_length - 4) * 8;
        int padding_bits, i;

        REQUIRE (stored_crc == plan->crc, "crc_x in WVX preamble");
        REQUIRE (physical_bits >= valid_bits, "WVX physically holds valid bits");
        padding_bits = physical_bits - valid_bits;
        REQUIRE (padding_bits >= 0 && padding_bits <= 16, "WVX padding only aligns bytes");

        for (i = 0; i < valid_bits; i++) {
            int want = (expected [i >> 3] >> (i & 7)) & 1;
            int got = (wvx->data [4 + (i >> 3)] >> (i & 7)) & 1;

            if (want != got) {
                int s;

                for (s = 0; s < g->n; s++)
                    if (i >= items [s].bit_offset &&
                        i < items [s].bit_offset + items [s].bit_count)
                        break;

                fprintf (stderr,
                    "FAIL group '%s' sideband bit %d in sample %d (pattern 0x%08x) want %d got %d\n",
                    g->name, i, s, g->patterns [s % g->n], want, got);
                exit (42);
            }
        }
    }

    flag_names (plan->flags, namebuf, sizeof namebuf);
    printf ("  group %-10s flags=%-31s wvx_bits=%d max_exp=%d\n",
        g->name, namebuf, valid_bits, plan->max_exp);
}

// Full public encode -> chunk traversal -> public decode of one group. Small
// groups are cyclically expanded to a legal block size first; the model plan
// is rebuilt over the exact expanded patterns so all comparisons stay exact.

static void test_encoded_group (const fwt_group *g)
{
    static unsigned char storage [4096];
    fwt_group eg;
    uint32_t expanded [64];
    fwp_sink sink;
    unsigned char md5_in [16], md5_got [16];
    fwm_item items [64];
    fwm_plan plan;
    int32_t integers [64];
    unsigned char expected [256];
    fwm_bits model_bs;
    int valid_bits;
    fwp_block block;
    fwp_memory mem;
    WavpackContext *wpc;
    int32_t decoded [64];
    int i;

    eg = *g;
    eg.patterns = expanded;
    eg.n = expand_patterns (g->patterns, g->n, expanded);

    encode_monophonic (&eg, &sink, storage, sizeof storage, md5_in);

    valid_bits = fwm_scan (eg.patterns, eg.n, integers, items, &plan);
    TASK_CHECK (plan.flags == g->expect_flags);
    TASK_CHECK (plan.max_exp == g->expect_max_exp);
    TASK_CHECK (!!plan.wvx_needed == g->expect_wvx);

    fwm_bits_init_write (&model_bs, expected, sizeof expected);
    fwm_send (eg.patterns, eg.n, items, &plan, &model_bs);
    TASK_CHECK (model_bs.bitpos == valid_bits);

    REQUIRE (fwp_parse_block (sink.data, sink.len, &block) == 0, "block parse");
    REQUIRE (block.block_samples == (uint32_t) eg.n, "block sample count");
    REQUIRE (block.flags & FLOAT_DATA, "FLOAT_DATA header flag");
    compare_wvx_chunk (&eg, &block, &plan, items, valid_bits, expected);

    fwp_memory_init (&mem, sink.data, sink.len);
    wpc = WavpackOpenFileInputEx64 (&fwp_memory_reader, &mem, NULL,
        (char [128]) {0}, 0, 0);
    REQUIRE (wpc != NULL, "public decoder open");
    TASK_CHECK (WavpackGetMode (wpc) & MODE_FLOAT);
    TASK_CHECK (WavpackGetNumChannels (wpc) == 1);
    TASK_CHECK (WavpackGetNumSamples64 (wpc) == eg.n);

    {
        uint32_t got = WavpackUnpackSamples (wpc, decoded, (uint32_t) eg.n);
        TASK_CHECK (got == (uint32_t) eg.n);             // frame count exact
    }

    for (i = 0; i < eg.n; i++)
        if ((uint32_t) decoded [i] != eg.patterns [i]) {
            fprintf (stderr, "FAIL group '%s' decoded sample %d pattern 0x%08x -> 0x%08x\n",
                g->name, i, eg.patterns [i], (uint32_t) decoded [i]);
            exit (42);
        }

    TASK_CHECK (WavpackGetNumErrors (wpc) == 0);
    TASK_CHECK (WavpackLossyBlocks (wpc) == 0);

    TASK_CHECK (WavpackGetMD5Sum (wpc, md5_got));
    TASK_CHECK (memcmp (md5_got, md5_in, 16) == 0);      // stored MD5 round trip

    WavpackCloseFile (wpc);
}

static void test_encoded_groups (void)
{
    int k;

    for (k = 0; k < FWT_NGROUPS; k++)
        test_encoded_group (&groups [k]);
}

////////////////////////////////// handcrafted WVX windows ////////////////////

typedef struct {
    const char     *name;
    const uint32_t *patterns;
    int             n;
    int             min_zeros;
    int             max_ones;
} fwt_window_case;

static const fwt_window_case window_cases [] = {
    { "max_shifted_ones", g_window_ones, (int) sizeof g_window_ones / sizeof *g_window_ones, 1, 3 },
    { "min_shifted_zeros", g_window_zeros, (int) sizeof g_window_zeros / sizeof *g_window_zeros, 2, 0 }
};

// Build a real block with the public encoder, then replace only its WVX chunk
// with handcrafted new-format material exercising the omission windows. The
// integer stream and every other chunk are untouched, so the audio CRC stays
// valid; only the block checksum is recomputed.

static void test_window_case (const fwt_window_case *wc)
{
    static unsigned char real_storage [4096], rebuilt_storage [4096];
    uint32_t expanded [64];
    fwt_group eg;
    fwp_sink sink;
    unsigned char md5 [16];
    fwm_item items [64];
    fwm_plan plan;
    int32_t integers [64];
    unsigned char sidebuf [256], literalbuf [256];
    fwm_bits bs;
    fwp_block src;
    fwp_builder builder;
    size_t rebuilt_len;
    fwp_memory mem;
    WavpackContext *wpc;
    int32_t decoded [64];
    uint32_t restored [64];
    uint32_t crc;
    int valid_bits, literal_bits, i, cursor;

    eg.name = wc->name;
    eg.patterns = expanded;
    eg.n = expand_patterns (wc->patterns, wc->n, expanded);
    eg.expect_flags = FWM_SHIFT_SENT;
    eg.expect_max_exp = G_W_EXP;
    eg.expect_shift = 0;
    eg.expect_wvx = 1;

    encode_monophonic (&eg, &sink, real_storage, sizeof real_storage, md5);
    TASK_CHECK (fwp_parse_block (sink.data, sink.len, &src) == 0);

    valid_bits = fwm_scan (eg.patterns, eg.n, integers, items, &plan);
    literal_bits = valid_bits;
    TASK_CHECK (plan.flags & FWM_SHIFT_SENT);
    TASK_CHECK (plan.max_exp == G_W_EXP);

    fwm_bits_init_write (&bs, literalbuf, sizeof literalbuf);
    fwm_send (eg.patterns, eg.n, items, &plan, &bs);
    TASK_CHECK (bs.bitpos == literal_bits);

    fwm_bits_init_write (&bs, sidebuf, sizeof sidebuf);
    valid_bits = fwm_send_windowed (eg.patterns, eg.n, items, &plan,
        wc->min_zeros, wc->max_ones, &bs);
    TASK_CHECK (!bs.error);
    TASK_CHECK (valid_bits > 0);

    // The windowed stream must be strictly shorter than the literal layout.
    TASK_CHECK (valid_bits < literal_bits);

    fwp_builder_init_from (&builder, rebuilt_storage, sizeof rebuilt_storage, sink.data);
    rebuilt_len = fwp_rebuild_with_wvx (&src, &builder, ID_WVX_NEW_BITSTREAM,
        wc->min_zeros, wc->max_ones, sidebuf, valid_bits, plan.crc);
    TASK_CHECK (rebuilt_len > 0 && rebuilt_len <= sizeof rebuilt_storage);

    {
        fwp_block rebuilt;

        TASK_CHECK (fwp_parse_block (rebuilt_storage, rebuilt_len, &rebuilt) == 0);
        TASK_CHECK (fwp_find_chunk (&rebuilt, ID_WVX_NEW_BITSTREAM) != NULL);
        TASK_CHECK (fwp_find_chunk (&rebuilt, ID_WVX_BITSTREAM) == NULL);
        TASK_CHECK (WavpackVerifySingleBlock (rebuilt_storage, 1) != 0);
    }

    // The model reader must recover every omitted bit and stop exactly at the
    // valid bit count, proving the cursor lands on the next sample.
    fwm_bits_init_read (&bs, sidebuf, sizeof sidebuf);
    crc = 0xffffffffu;
    fwm_restore (integers, eg.n, &plan, wc->min_zeros, wc->max_ones,
        &bs, restored, &crc);
    TASK_CHECK (!bs.error);
    TASK_CHECK (bs.bitpos == valid_bits);
    TASK_CHECK (crc == plan.crc);

    cursor = 0;

    for (i = 0; i < eg.n; i++) {
        TASK_CHECK (restored [i] == eg.patterns [i]);
        cursor += items [i].bit_count;
    }

    TASK_CHECK (cursor == literal_bits);

    // The production decoder must accept the handcrafted block unchanged.
    fwp_memory_init (&mem, rebuilt_storage, rebuilt_len);
    wpc = WavpackOpenFileInputEx64 (&fwp_memory_reader, &mem, NULL,
        (char [128]) {0}, 0, 0);
    TASK_CHECK (wpc != NULL);
    TASK_CHECK (WavpackUnpackSamples (wpc, decoded, (uint32_t) eg.n) == (uint32_t) eg.n);
    TASK_CHECK (WavpackGetNumErrors (wpc) == 0);
    TASK_CHECK (WavpackLossyBlocks (wpc) == 0);

    for (i = 0; i < eg.n; i++)
        TASK_CHECK ((uint32_t) decoded [i] == eg.patterns [i]);

    WavpackCloseFile (wpc);
    printf ("  window %-18s min_zeros=%d max_ones=%d literal_bits=%d windowed_bits=%d\n",
        wc->name, wc->min_zeros, wc->max_ones, literal_bits, valid_bits);
}

static void test_handcrafted_windows (void)
{
    int k;

    for (k = 0; k < (int) (sizeof window_cases / sizeof *window_cases); k++)
        test_window_case (&window_cases [k]);
}

////////////////////////////////// multichannel round trips ///////////////////

// Deterministic, code-generated per-channel material. Each channel draws from
// a different anchor group so its block-level float plan is different, and
// special values (negative zero, NaN payloads, smallest/largest subnormals,
// infinities and false-zero-scale finites) are sprinkled by index.

static uint32_t channel_pattern (int channel, int frame)
{
    static const uint32_t specials [] = {
        0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u,
        0x7fc12345u, 0xff812345u, 0x00000001u, 0x80000001u,
        0x007fffffu, 0x807fffffu, 0x33800000u, 0x3f7fffffu
    };
    uint32_t anchor = (uint32_t) (0x42c80000u + channel * 0x100000u);
    uint32_t low = (uint32_t) ((channel * 7 + frame * 13) & 0x3fffff);
    int pick = (channel * 5 + frame * 3) % (int) (sizeof specials / sizeof *specials);

    if ((frame + channel) % 4 == 0)
        return specials [pick];

    if ((frame & 3) == 1)
        return 0x3f800000u | (low & 0xffffu);     // ordinary finite audio

    if ((frame & 3) == 2)
        return 0x40000000u | (low & 0x7ffffu);   // integer-precision-ish float

    return anchor | (low & 0x7fffffu);           // channel-specific big finite
}

typedef struct {
    int channels;
    int channel_mask;
    int frames;
    int block_samples;
    int read_batch;
} fwt_mc_case;

static const fwt_mc_case mc_cases [] = {
    { 2, 0x3,      40, 16, 3 },     // stereo across three blocks, tiny reads
    { 6, 0x3f,     32, 16, 5 }      // 5.1: three stereo streams, two blocks
};

static void test_multichannel (const fwt_mc_case *mc)
{
    static unsigned char storage [1 << 16];
    int total = mc->channels * mc->frames;
    int32_t *input, *output;
    uint32_t *expected;
    fwp_sink sink;
    WavpackConfig cfg;
    WavpackContext *wpc;
    fwt_md5 md;
    unsigned char md5_in [16], md5_got [16];
    int ch, fr, i;

    input = malloc ((size_t) total * sizeof *input);
    output = malloc ((size_t) total * sizeof *output);
    expected = malloc ((size_t) total * sizeof *expected);
    TASK_CHECK (input && output && expected);

    for (fr = 0; fr < mc->frames; fr++)
        for (ch = 0; ch < mc->channels; ch++) {
            uint32_t p = channel_pattern (ch, fr);
            expected [fr * mc->channels + ch] = p;
            input [fr * mc->channels + ch] = (int32_t) p;
        }

    fwt_md5_init (&md);
    fwt_md5_update (&md, input, (size_t) total * sizeof *input);
    fwt_md5_final (&md, md5_in);

    memset (&cfg, 0, sizeof cfg);
    cfg.bytes_per_sample = 4;
    cfg.bits_per_sample = 32;
    cfg.float_norm_exp = 127;
    cfg.num_channels = mc->channels;
    cfg.channel_mask = mc->channel_mask;
    cfg.sample_rate = 48000;
    cfg.block_samples = mc->block_samples;
    cfg.flags = CONFIG_MD5_CHECKSUM;

    fwp_sink_init (&sink, storage, sizeof storage);
    wpc = WavpackOpenFileOutput (fwt_block_out, &sink, NULL);
    TASK_CHECK (wpc);
    TASK_CHECK (WavpackSetConfiguration64 (wpc, &cfg, mc->frames, NULL));
    TASK_CHECK (WavpackPackInit (wpc));

    for (fr = 0; fr < mc->frames; fr++)
        TASK_CHECK (WavpackPackSamples (wpc, input + fr * mc->channels, 1));

    TASK_CHECK (WavpackFlushSamples (wpc));
    TASK_CHECK (WavpackStoreMD5Sum (wpc, md5_in));
    TASK_CHECK (WavpackFlushSamples (wpc));
    WavpackCloseFile (wpc);
    TASK_CHECK (!sink.failed);

    // Traverse every block and state exactly which FLOAT_INFO/WVX material
    // appeared in the audio blocks; a byte-presence probe could not attribute
    // flags to a block. Non-audio blocks (the RIFF header and the MD5
    // trailer) are parsed but have no float metadata.
    {
        size_t off = 0;
        int audio_blocks = 0;

        while (off < sink.len) {
            fwp_block block;
            const fwp_chunk *info, *wvx, *wvx_new;
            char namebuf [128];

            TASK_CHECK (fwp_parse_block (storage + off, sink.len - off, &block) == 0);

            if (!block.block_samples) {
                off += block.ckSize + 8;
                continue;
            }

            TASK_CHECK (block.flags & FLOAT_DATA);
            info = fwp_find_chunk (&block, ID_FLOAT_INFO);
            TASK_CHECK (info != NULL);
            wvx = fwp_find_chunk (&block, ID_WVX_BITSTREAM);
            wvx_new = fwp_find_chunk (&block, ID_WVX_NEW_BITSTREAM);
            flag_names (info->data [0], namebuf, sizeof namebuf);
            printf ("  %d-channel audio block %d: FLOAT_INFO flags=%s shift=%d max_exp=%d wvx=%s\n",
                mc->channels, audio_blocks, namebuf, info->data [1], info->data [2],
                wvx_new ? "new" : (wvx ? "legacy" : "none"));
            TASK_CHECK (wvx_new == NULL);       // encoder still writes legacy WVX
            off += block.ckSize + 8;
            audio_blocks++;
        }

        TASK_CHECK (audio_blocks >= 2);         // material really spans blocks
    }

    {
        fwp_memory mem;

        fwp_memory_init (&mem, storage, sink.len);
        wpc = WavpackOpenFileInputEx64 (&fwp_memory_reader, &mem, NULL,
            (char [128]) {0}, 0, 0);
        TASK_CHECK (wpc);
        TASK_CHECK (WavpackGetMode (wpc) & MODE_FLOAT);
        TASK_CHECK (WavpackGetNumChannels (wpc) == mc->channels);
        TASK_CHECK (WavpackGetNumSamples64 (wpc) == mc->frames);

        i = 0;

        while (i < mc->frames) {
            int want = mc->read_batch;
            uint32_t got;

            if (want > mc->frames - i)
                want = mc->frames - i;

            got = WavpackUnpackSamples (wpc, output + i * mc->channels, (uint32_t) want);
            TASK_CHECK (got == (uint32_t) want);     // each small read is exact
            i += want;
        }

        TASK_CHECK (WavpackGetSampleIndex64 (wpc) == mc->frames);
        TASK_CHECK (WavpackGetNumErrors (wpc) == 0);
        TASK_CHECK (WavpackLossyBlocks (wpc) == 0);
        TASK_CHECK (WavpackGetMD5Sum (wpc, md5_got));
        TASK_CHECK (memcmp (md5_got, md5_in, 16) == 0);
        WavpackCloseFile (wpc);
    }

    for (i = 0; i < total; i++)
        if ((uint32_t) output [i] != expected [i]) {
            fprintf (stderr, "FAIL %d-channel sample word %d (ch %d frame %d) 0x%08x -> 0x%08x\n",
                mc->channels, i, i % mc->channels, i / mc->channels,
                expected [i], (uint32_t) output [i]);
            exit (42);
        }

    free (input); free (output); free (expected);
}

static void test_multichannels (void)
{
    int k;

    for (k = 0; k < (int) (sizeof mc_cases / sizeof *mc_cases); k++)
        test_multichannel (&mc_cases [k]);
}

////////////////////////////////// entry point ////////////////////////////////

void task_cases (void)
{
    puts ("float wire regression: independent IEEE-754 sideband model");
    fflush (stdout);
    test_model_self ();
    puts ("model self-test OK");
    fflush (stdout);
    test_encoded_groups ();
    puts ("public encode/decode bit-level comparison OK");
    fflush (stdout);
    test_handcrafted_windows ();
    puts ("handcrafted new WVX window material OK");
    fflush (stdout);
    test_multichannels ();
    puts ("stereo and six-channel multi-block round trips OK");
    fflush (stdout);
}
