////////////////////////////////////////////////////////////////////////////
//                           **** ADPCM-XQ ****                           //
//                  Xtreme Quality ADPCM Encoder/Decoder                  //
//                    Copyright (c) 2024 David Bryant.                    //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// test_adaptive.c
//
// Standalone test harness for adpcm_encode_adaptive(). It never trusts the
// numbers reported by the new entry point directly: every SSE is recomputed
// by decoding the emitted block with the original decoder, and every
// continuation is compared against a control context that encoded the same
// blocks directly with adpcm_encode_block_ex() (no trial encodes).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "adpcm-lib.h"

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf ("FAIL line %d: ", __LINE__); \
        printf (__VA_ARGS__); \
        printf ("\n"); \
        failures++; \
    } \
} while (0)

#define MAX_FRAMES 4096
#define MAX_BLOCK  (8 + MAX_FRAMES * 5)     // generous for any bps / channel count

static uint8_t outbuf [MAX_BLOCK], outbuf2 [MAX_BLOCK], ctrlbuf [MAX_BLOCK];
static int16_t decbuf [MAX_FRAMES * 2 * 2];

// Encode one block with the given context, decode it back with the original
// decoder, and compute the per-channel SSE over exactly "frames" frames.
// Note: adpcm_encode_block_ex() may leave alignment-padding bytes at the end
// of a block unwritten, so the target buffer is zeroed first to keep the
// results (and any later memcmp) deterministic.

static void eval_candidate (void *ctx, const int16_t *pcm, int frames, int nch, int bps,
                            uint8_t *out, size_t *outsize, uint64_t sse [2])
{
    int decoded, ch, i;

    memset (out, 0, MAX_BLOCK);

    if (!adpcm_encode_block_ex (ctx, out, outsize, pcm, frames, bps)) {
        printf ("FAIL: adpcm_encode_block_ex() failed for bps=%d\n", bps);
        failures++;
        *outsize = 0;
        sse [0] = sse [1] = UINT64_MAX;
        return;
    }

    decoded = adpcm_decode_block_ex (decbuf, out, *outsize, nch, bps);

    if (decoded < frames) {
        printf ("FAIL: decoded %d frames, expected at least %d (bps=%d)\n", decoded, frames, bps);
        failures++;
    }

    sse [0] = sse [1] = 0;

    for (ch = 0; ch < nch; ++ch)
        for (i = 0; i < frames; ++i) {
            int32_t e = (int32_t) decbuf [i * nch + ch] - pcm [i * nch + ch];
            sse [ch] += (uint64_t) ((int64_t) e * e);
        }
}

// Apply the documented selection rule to independently measured candidates.

static int rule_pick (unsigned mask, const size_t bytes [6], const uint64_t sse [6][2],
                      const uint64_t max_sse [2], int nch)
{
    int best = 0, bps, ch;

    for (bps = 2; bps <= 5; ++bps) {
        int feasible;

        if (!(mask & (1U << bps)))
            continue;

        feasible = 1;

        for (ch = 0; ch < nch; ++ch)
            if (sse [bps][ch] > max_sse [ch])
                feasible = 0;

        if (feasible && (!best || bytes [bps] < bytes [best]))
            best = bps;
    }

    return best;
}

// Decode the block emitted by adpcm_encode_adaptive() with the original decoder
// and verify that the reported SSE matches the actual reconstruction.

static void verify_reported_sse (const char *tag, const uint8_t *out, const adpcm_adaptive_result *res,
                                 const int16_t *pcm, int frames, int nch)
{
    uint64_t sse [2] = { 0, 0 };
    int decoded, ch, i;

    decoded = adpcm_decode_block_ex (decbuf, out, res->bytes, nch, res->bps);
    CHECK (decoded >= frames, "%s: decode of adaptive output gave %d frames, expected >= %d", tag, decoded, frames);

    for (ch = 0; ch < nch; ++ch)
        for (i = 0; i < frames; ++i) {
            int32_t e = (int32_t) decbuf [i * nch + ch] - pcm [i * nch + ch];
            sse [ch] += (uint64_t) ((int64_t) e * e);
        }

    CHECK (sse [0] == res->sse [0] && (nch > 1 ? sse [1] == res->sse [1] : res->sse [1] == 0),
        "%s: reported sse [%llu %llu] != decoded sse [%llu %llu]", tag,
        (unsigned long long) res->sse [0], (unsigned long long) res->sse [1],
        (unsigned long long) sse [0], (unsigned long long) sse [1]);
}

// The reproducible vector from the specification: stereo 33 frames, left is a
// sawtooth, right alternates +/-1200.

static void gen_main_pcm (int16_t *pcm, int frames)
{
    int i;

    for (i = 0; i < frames; ++i) {
        pcm [i * 2 + 0] = (int16_t) ((i * 1709) % 20000 - 10000);
        pcm [i * 2 + 1] = (int16_t) ((i & 1) ? 1200 : -1200);
    }
}

// Measure all four candidates from identical fresh contexts and print them.

static void measure_candidates (int nch, int rate, int lookahead, int shaping,
                                const int16_t *pcm, int frames,
                                size_t bytes [6], uint64_t sse [6][2])
{
    int bps;

    for (bps = 2; bps <= 5; ++bps) {
        void *c = adpcm_create_context (nch, rate, lookahead, shaping);
        eval_candidate (c, pcm, frames, nch, bps, outbuf2, &bytes [bps], sse [bps]);
        adpcm_free_context (c);
        printf ("  candidate bps=%d  bytes=%2u  sse=[%llu %llu]\n", bps, (unsigned) bytes [bps],
            (unsigned long long) sse [bps][0], (unsigned long long) sse [bps][1]);
    }
}

static void test_main_vector (void)
{
    int16_t pcm [33 * 2];
    size_t bytes [6] = { 0 };
    uint64_t sse [6][2] = {{ 0 }};
    adpcm_adaptive_result res;
    uint64_t max_sse [2];
    size_t ctrlsize, next_a, next_b;
    int expected, ok, bps;
    void *a, *ctrl;

    printf ("test: main stereo vector (33 frames, lookahead=1, dynamic shaping)\n");
    gen_main_pcm (pcm, 33);
    measure_candidates (2, 8000, 1, NOISE_SHAPING_DYNAMIC, pcm, 33, bytes, sse);

    // case 1: limits [100000000, 1000000] should select 4-bit

    max_sse [0] = 100000000; max_sse [1] = 1000000;
    expected = rule_pick (0x3C, bytes, sse, max_sse, 2);
    CHECK (expected == 4, "rule pick for limits [1e8 1e6] was %d, expected 4", expected);

    a = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
    memset (&res, 0xA5, sizeof (res));
    memset (outbuf, 0, sizeof (outbuf));
    ok = adpcm_encode_adaptive (a, outbuf, sizeof (outbuf), pcm, 33, 0x3C, max_sse, &res);
    CHECK (ok, "adaptive call failed for limits [1e8 1e6]");
    CHECK (res.bps == 4, "selected bps=%d, expected 4", res.bps);
    CHECK (res.bytes == bytes [4], "bytes=%u != reference %u", (unsigned) res.bytes, (unsigned) bytes [4]);
    CHECK (res.sse [0] == sse [4][0] && res.sse [1] == sse [4][1], "reported sse mismatch vs reference");
    verify_reported_sse ("main/4bit", outbuf, &res, pcm, 33, 2);
    printf ("  selected bps=%d bytes=%u sse=[%llu %llu]\n", res.bps, (unsigned) res.bytes,
        (unsigned long long) res.sse [0], (unsigned long long) res.sse [1]);

    // control: same initial context, direct encode at the selected bps, then continue

    ctrl = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
    memset (ctrlbuf, 0, sizeof (ctrlbuf));
    adpcm_encode_block_ex (ctrl, ctrlbuf, &ctrlsize, pcm, 33, 4);
    CHECK (ctrlsize == res.bytes && !memcmp (ctrlbuf, outbuf, ctrlsize),
        "adaptive block bytes differ from direct 4-bit encode");

    gen_main_pcm (pcm, 33);     // second block: same data is fine, state differs
    memset (outbuf2, 0, sizeof (outbuf2));
    memset (ctrlbuf, 0, sizeof (ctrlbuf));
    adpcm_encode_block_ex (a, outbuf2, &next_a, pcm, 33, 4);
    adpcm_encode_block_ex (ctrl, ctrlbuf, &next_b, pcm, 33, 4);
    CHECK (next_a == next_b && !memcmp (outbuf2, ctrlbuf, next_a),
        "next block after adaptive differs from control (%u vs %u bytes)",
        (unsigned) next_a, (unsigned) next_b);
    printf ("  next block bytes: adaptive=%u control=%u (match=%d)\n",
        (unsigned) next_a, (unsigned) next_b, next_a == next_b && !memcmp (outbuf2, ctrlbuf, next_a));
    adpcm_free_context (a);
    adpcm_free_context (ctrl);

    // case 2: limits [200000000, 6000000] should select 3-bit

    max_sse [0] = 200000000; max_sse [1] = 6000000;
    expected = rule_pick (0x3C, bytes, sse, max_sse, 2);
    CHECK (expected == 3, "rule pick for limits [2e8 6e6] was %d, expected 3", expected);

    a = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
    memset (outbuf, 0, sizeof (outbuf));
    ok = adpcm_encode_adaptive (a, outbuf, sizeof (outbuf), pcm, 33, 0x3C, max_sse, &res);
    CHECK (ok, "adaptive call failed for limits [2e8 6e6]");
    CHECK (res.bps == 3, "selected bps=%d, expected 3", res.bps);
    CHECK (res.bytes == bytes [3], "bytes=%u != reference %u", (unsigned) res.bytes, (unsigned) bytes [3]);
    CHECK (res.sse [0] == sse [3][0] && res.sse [1] == sse [3][1], "reported sse mismatch vs reference");
    verify_reported_sse ("main/3bit", outbuf, &res, pcm, 33, 2);
    printf ("  selected bps=%d bytes=%u sse=[%llu %llu]\n", res.bps, (unsigned) res.bytes,
        (unsigned long long) res.sse [0], (unsigned long long) res.sse [1]);
    adpcm_free_context (a);

    // the reference bytes for the two cases must differ (sanity that we really switched)

    CHECK (bytes [3] != bytes [4], "3-bit and 4-bit blocks unexpectedly identical size");
    for (bps = 2; bps <= 5; ++bps)
        CHECK (bytes [bps] == (size_t) adpcm_sample_count_to_block_size (33, 2, bps),
            "candidate bps=%d bytes=%u != adpcm_sample_count_to_block_size()=%d",
            bps, (unsigned) bytes [bps], adpcm_sample_count_to_block_size (33, 2, bps));
}

// A loud left channel with a very quiet right channel under a tight right-channel
// limit: a candidate that passes any summed/averaged metric must still be
// rejected because the right channel alone exceeds its own limit.

static void test_per_channel_limits (void)
{
    int16_t pcm [40 * 2];
    size_t bytes [6] = { 0 };
    uint64_t sse [6][2] = {{ 0 }};
    uint64_t max_sse [2];
    adpcm_adaptive_result res;
    int i, ok;

    printf ("test: per-channel limits (loud left, quiet right, tight right limit)\n");

    for (i = 0; i < 40; ++i) {
        pcm [i * 2 + 0] = (int16_t) ((i * 1709) % 20000 - 10000);
        pcm [i * 2 + 1] = (int16_t) (((i * 7) % 5) - 2);        // tiny: -2..+2
    }

    measure_candidates (2, 8000, 1, NOISE_SHAPING_DYNAMIC, pcm, 40, bytes, sse);

    // require that 2-bit is worse than 3-bit on the right channel, then place the
    // right-channel limit between them; the left limit is high enough that the
    // 2-bit *total* error is below the sum of the limits

    CHECK (sse [2][1] > sse [3][1], "test construction: 2-bit right sse not worse than 3-bit");
    max_sse [1] = (sse [2][1] + sse [3][1]) / 2;
    max_sse [0] = sse [2][0] + sse [2][1];      // sum test would pass 2-bit
    CHECK (sse [2][0] + sse [2][1] <= max_sse [0] + max_sse [1],
        "test construction: 2-bit would not pass a summed metric");
    CHECK (sse [2][1] > max_sse [1], "test construction: 2-bit right sse within limit");
    printf ("  limits: [%llu %llu], 2-bit right sse=%llu (over), 3-bit right sse=%llu (under)\n",
        (unsigned long long) max_sse [0], (unsigned long long) max_sse [1],
        (unsigned long long) sse [2][1], (unsigned long long) sse [3][1]);

    {
        void *a = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
        memset (outbuf, 0, sizeof (outbuf));
        ok = adpcm_encode_adaptive (a, outbuf, sizeof (outbuf), pcm, 40, 0x3C, max_sse, &res);
        CHECK (ok, "adaptive call failed");
        CHECK (res.bps == 3, "selected bps=%d, expected 3 (2-bit must fail on right channel alone)", res.bps);
        CHECK (res.bytes == bytes [3] && res.sse [0] == sse [3][0] && res.sse [1] == sse [3][1],
            "reported result mismatch vs reference");
        verify_reported_sse ("per-channel", outbuf, &res, pcm, 40, 2);
        printf ("  selected bps=%d bytes=%u sse=[%llu %llu]\n", res.bps, (unsigned) res.bytes,
            (unsigned long long) res.sse [0], (unsigned long long) res.sse [1]);
        adpcm_free_context (a);
    }
}

// Equal byte counts must resolve to the smaller code size. With only 2 frames
// every code size produces the same (single-chunk) block, so with generous
// limits the 2-bit candidate must win.

static void test_byte_tie (void)
{
    int16_t pcm [2 * 2] = { 100, -100, 900, -900 };
    size_t bytes [6] = { 0 };
    uint64_t sse [6][2] = {{ 0 }};
    uint64_t max_sse [2] = { UINT64_MAX / 2, UINT64_MAX / 2 };
    adpcm_adaptive_result res;
    int ok, bps;

    printf ("test: byte-count tie resolves to smaller bps (frames=2)\n");
    measure_candidates (2, 8000, 1, NOISE_SHAPING_DYNAMIC, pcm, 2, bytes, sse);

    for (bps = 3; bps <= 5; ++bps)
        CHECK (bytes [bps] == bytes [2], "test construction: bytes differ (%u vs %u)",
            (unsigned) bytes [bps], (unsigned) bytes [2]);

    {
        void *a = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
        memset (outbuf, 0, sizeof (outbuf));
        ok = adpcm_encode_adaptive (a, outbuf, sizeof (outbuf), pcm, 2, 0x3C, max_sse, &res);
        CHECK (ok, "adaptive call failed");
        CHECK (res.bps == 2, "selected bps=%d, expected 2 on byte tie", res.bps);
        CHECK (res.bytes == bytes [2], "bytes mismatch");
        verify_reported_sse ("tie", outbuf, &res, pcm, 2, 2);
        printf ("  selected bps=%d bytes=%u (all candidates %u bytes)\n",
            res.bps, (unsigned) res.bytes, (unsigned) bytes [2]);
        adpcm_free_context (a);
    }
}

// The choice must not depend on which other candidates are present in the mask:
// adding infeasible candidates to the mask must not change the outcome, and the
// outcome must equal the rule applied to fully independent encodes.

static void test_mask_independence (void)
{
    int16_t pcm [33 * 2];
    size_t bytes [6] = { 0 };
    uint64_t sse [6][2] = {{ 0 }};
    uint64_t max_sse [2] = { 100000000, 1000000 };      // selects 4-bit with full mask
    adpcm_adaptive_result res_full, res_sub;
    unsigned full_mask = 0x3C, sub_mask = (1 << 3) | (1 << 4);
    int ok;

    printf ("test: candidate/mask independence\n");
    gen_main_pcm (pcm, 33);
    measure_candidates (2, 8000, 1, NOISE_SHAPING_DYNAMIC, pcm, 33, bytes, sse);

    CHECK (rule_pick (full_mask, bytes, sse, max_sse, 2) == 4, "full-mask rule pick != 4");
    CHECK (rule_pick (sub_mask, bytes, sse, max_sse, 2) == 4, "sub-mask rule pick != 4");

    {
        void *a = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
        void *b = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
        size_t next_a, next_b;

        memset (outbuf, 0, sizeof (outbuf));
        memset (outbuf2, 0, sizeof (outbuf2));
        ok = adpcm_encode_adaptive (a, outbuf, sizeof (outbuf), pcm, 33, full_mask, max_sse, &res_full);
        CHECK (ok, "full-mask adaptive failed");
        ok = adpcm_encode_adaptive (b, outbuf2, sizeof (outbuf2), pcm, 33, sub_mask, max_sse, &res_sub);
        CHECK (ok, "sub-mask adaptive failed");

        CHECK (res_full.bps == res_sub.bps && res_full.bytes == res_sub.bytes &&
               res_full.sse [0] == res_sub.sse [0] && res_full.sse [1] == res_sub.sse [1],
            "mask with extra infeasible candidates changed the result");
        CHECK (res_full.bytes == res_sub.bytes && !memcmp (outbuf, outbuf2, res_full.bytes),
            "mask with extra infeasible candidates changed the bytes");

        // and the committed state must be identical too (compare continuations)

        memset (outbuf, 0, sizeof (outbuf));
        memset (outbuf2, 0, sizeof (outbuf2));
        adpcm_encode_block_ex (a, outbuf, &next_a, pcm, 33, 4);
        adpcm_encode_block_ex (b, outbuf2, &next_b, pcm, 33, 4);
        CHECK (next_a == next_b && !memcmp (outbuf, outbuf2, next_a),
            "continuation differs between full-mask and sub-mask contexts");
        printf ("  full-mask bps=%d, sub-mask bps=%d, continuations match=%d\n",
            res_full.bps, res_sub.bps, next_a == next_b && !memcmp (outbuf, outbuf2, next_a));
        adpcm_free_context (a);
        adpcm_free_context (b);
    }
}

// Mono sources: the second max_sse entry must be ignored and sse[1] reported as 0.

static void test_mono (void)
{
    int16_t pcm [50];
    size_t bytes [6] = { 0 };
    uint64_t sse [6][2] = {{ 0 }};
    uint64_t max_sse [2] = { UINT64_MAX / 2, 0 };       // second entry must be ignored
    adpcm_adaptive_result res;
    size_t ctrlsize, next_a, next_b;
    int i, ok;

    printf ("test: mono source\n");

    for (i = 0; i < 50; ++i)
        pcm [i] = (int16_t) ((i * 1709) % 20000 - 10000);

    measure_candidates (1, 8000, 1, NOISE_SHAPING_STATIC, pcm, 50, bytes, sse);

    {
        void *a = adpcm_create_context (1, 8000, 1, NOISE_SHAPING_STATIC);
        void *ctrl = adpcm_create_context (1, 8000, 1, NOISE_SHAPING_STATIC);
        int expected = rule_pick (0x3C, bytes, sse, max_sse, 1);

        CHECK (expected == 2, "rule pick for generous mono limits = %d, expected 2", expected);
        memset (outbuf, 0, sizeof (outbuf));
        ok = adpcm_encode_adaptive (a, outbuf, sizeof (outbuf), pcm, 50, 0x3C, max_sse, &res);
        CHECK (ok, "mono adaptive failed");
        CHECK (res.bps == expected, "mono bps=%d, expected %d", res.bps, expected);
        CHECK (res.sse [1] == 0, "mono sse[1]=%llu, expected 0", (unsigned long long) res.sse [1]);
        CHECK (res.sse [0] == sse [expected][0] && res.bytes == bytes [expected],
            "mono result mismatch vs reference");
        verify_reported_sse ("mono", outbuf, &res, pcm, 50, 1);

        memset (ctrlbuf, 0, sizeof (ctrlbuf));
        adpcm_encode_block_ex (ctrl, ctrlbuf, &ctrlsize, pcm, 50, expected);
        CHECK (ctrlsize == res.bytes && !memcmp (ctrlbuf, outbuf, ctrlsize),
            "mono adaptive bytes differ from direct encode");
        memset (outbuf2, 0, sizeof (outbuf2));
        memset (ctrlbuf, 0, sizeof (ctrlbuf));
        adpcm_encode_block_ex (a, outbuf2, &next_a, pcm, 50, 3);
        adpcm_encode_block_ex (ctrl, ctrlbuf, &next_b, pcm, 50, 3);
        CHECK (next_a == next_b && !memcmp (outbuf2, ctrlbuf, next_a),
            "mono continuation differs from control");
        printf ("  selected bps=%d bytes=%u sse=[%llu %llu], continuation match=%d\n",
            res.bps, (unsigned) res.bytes, (unsigned long long) res.sse [0],
            (unsigned long long) res.sse [1], next_a == next_b && !memcmp (outbuf2, ctrlbuf, next_a));
        adpcm_free_context (a);
        adpcm_free_context (ctrl);
    }
}

// Two consecutive adaptive blocks with history, across lookahead depths 0/1 and
// static/dynamic shaping: continuations must match a control context that never
// did any trial encodes.

static void test_history (void)
{
    static const struct { int lookahead, shaping; const char *name; } configs [] = {
        { 0, NOISE_SHAPING_STATIC,  "lookahead=0 static"  },
        { 0, NOISE_SHAPING_DYNAMIC, "lookahead=0 dynamic" },
        { 1, NOISE_SHAPING_STATIC,  "lookahead=1 static"  },
        { 1, NOISE_SHAPING_DYNAMIC, "lookahead=1 dynamic" },
    };
    int16_t pcm1 [64 * 2], pcm2 [48 * 2];
    int cfg, i;

    for (i = 0; i < 64; ++i) {
        pcm1 [i * 2 + 0] = (int16_t) ((i * 1709) % 20000 - 10000);
        pcm1 [i * 2 + 1] = (int16_t) ((i & 1) ? 1200 : -1200);
    }
    for (i = 0; i < 48; ++i) {
        pcm2 [i * 2 + 0] = (int16_t) ((i * 991) % 16000 - 8000);
        pcm2 [i * 2 + 1] = (int16_t) (((i * 313) % 7000) - 3500);
    }

    for (cfg = 0; cfg < 4; ++cfg) {
        size_t bytes [6] = { 0 };
        uint64_t sse [6][2] = {{ 0 }};
        uint64_t max_sse [2];
        adpcm_adaptive_result res1, res2;
        size_t ctrl1, ctrl2;
        void *a, *ctrl;

        printf ("test: history over two blocks (%s)\n", configs [cfg].name);
        measure_candidates (2, 8000, configs [cfg].lookahead, configs [cfg].shaping, pcm1, 64, bytes, sse);

        // place the limits between the 2-bit and 3-bit errors so the first block
        // picks 3-bit (a mid-range, interesting choice)

        CHECK (sse [2][0] > sse [3][0] || sse [2][1] > sse [3][1],
            "test construction: 2-bit not worse than 3-bit");
        max_sse [0] = (sse [2][0] + sse [3][0]) / 2;
        max_sse [1] = (sse [2][1] + sse [3][1]) / 2;
        CHECK (rule_pick (0x3C, bytes, sse, max_sse, 2) == 3, "history rule pick != 3");

        a = adpcm_create_context (2, 8000, configs [cfg].lookahead, configs [cfg].shaping);
        ctrl = adpcm_create_context (2, 8000, configs [cfg].lookahead, configs [cfg].shaping);

        memset (outbuf, 0, sizeof (outbuf));
        CHECK (adpcm_encode_adaptive (a, outbuf, sizeof (outbuf), pcm1, 64, 0x3C, max_sse, &res1),
            "history block1 adaptive failed");
        CHECK (res1.bps == 3, "history block1 bps=%d, expected 3", res1.bps);
        verify_reported_sse ("history/block1", outbuf, &res1, pcm1, 64, 2);

        // generous limits for block 2 (winner should be 2-bit, fewest bytes)

        max_sse [0] = max_sse [1] = UINT64_MAX / 2;
        memset (outbuf2, 0, sizeof (outbuf2));
        CHECK (adpcm_encode_adaptive (a, outbuf2, sizeof (outbuf2), pcm2, 48, 0x3C, max_sse, &res2),
            "history block2 adaptive failed");
        verify_reported_sse ("history/block2", outbuf2, &res2, pcm2, 48, 2);

        // control: direct encodes at the same code sizes, no trials

        memset (ctrlbuf, 0, sizeof (ctrlbuf));
        adpcm_encode_block_ex (ctrl, ctrlbuf, &ctrl1, pcm1, 64, res1.bps);
        CHECK (ctrl1 == res1.bytes && !memcmp (ctrlbuf, outbuf, ctrl1),
            "history block1 bytes differ from control");
        memset (ctrlbuf, 0, sizeof (ctrlbuf));
        adpcm_encode_block_ex (ctrl, ctrlbuf, &ctrl2, pcm2, 48, res2.bps);
        CHECK (ctrl2 == res2.bytes && !memcmp (ctrlbuf, outbuf2, ctrl2),
            "history block2 bytes differ from control (%u vs %u)",
            (unsigned) ctrl2, (unsigned) res2.bytes);

        printf ("  block1 bps=%d bytes=%u, block2 bps=%d bytes=%u, control match=%d\n",
            res1.bps, (unsigned) res1.bytes, res2.bps, (unsigned) res2.bytes,
            ctrl2 == res2.bytes && !memcmp (ctrlbuf, outbuf2, ctrl2));
        adpcm_free_context (a);
        adpcm_free_context (ctrl);
    }
}

// Odd frame counts (non-integral chunks, short tails) and the frame-count
// boundaries 2 and 4096.

static void test_frame_counts (void)
{
    static const int counts [] = { 2, 3, 5, 30, 33, 37, 255, 4096 };
    int16_t *pcm = malloc (sizeof (int16_t) * 4096 * 2);
    int t, i;

    printf ("test: odd frame counts and boundaries\n");
    CHECK (pcm != NULL, "malloc failed");

    for (i = 0; i < 4096 * 2; ++i)
        pcm [i] = (int16_t) ((i * 1709 + (i >> 3) * 97) % 24000 - 12000);

    for (t = 0; t < 8; ++t) {
        int frames = counts [t];
        size_t bytes [6] = { 0 };
        uint64_t sse [6][2] = {{ 0 }};
        uint64_t max_sse [2] = { UINT64_MAX / 2, UINT64_MAX / 2 };
        adpcm_adaptive_result res;
        int expected, ok;
        void *a;

        measure_candidates (2, 8000, 1, NOISE_SHAPING_DYNAMIC, pcm, frames, bytes, sse);
        expected = rule_pick (0x3C, bytes, sse, max_sse, 2);
        CHECK (expected == 2, "frames=%d: generous rule pick=%d, expected 2", frames, expected);

        a = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
        memset (outbuf, 0, sizeof (outbuf));
        ok = adpcm_encode_adaptive (a, outbuf, sizeof (outbuf), pcm, frames, 0x3C, max_sse, &res);
        CHECK (ok, "frames=%d: adaptive failed", frames);
        CHECK (res.bps == expected && res.bytes == bytes [expected],
            "frames=%d: bps=%d bytes=%u, expected bps=%d bytes=%u",
            frames, res.bps, (unsigned) res.bytes, expected, (unsigned) bytes [expected]);
        CHECK (res.sse [0] == sse [expected][0] && res.sse [1] == sse [expected][1],
            "frames=%d: sse mismatch vs reference", frames);
        verify_reported_sse ("frames", outbuf, &res, pcm, frames, 2);
        printf ("  frames=%4d: bps=%d bytes=%u sse=[%llu %llu] ok\n", frames, res.bps,
            (unsigned) res.bytes, (unsigned long long) res.sse [0], (unsigned long long) res.sse [1]);
        adpcm_free_context (a);
    }

    free (pcm);
}

// Every failure mode must return 0 and leave the context, the output buffer,
// and the result structure completely untouched; a retry after a failure must
// behave as if the failure never happened.

static void test_failures (void)
{
    int16_t pcm [33 * 2];
    adpcm_adaptive_result res, res2;
    uint64_t generous [2] = { UINT64_MAX / 2, UINT64_MAX / 2 };
    uint64_t impossible [2] = { 0, 0 };
    uint64_t sse [6][2] = {{ 0 }};
    size_t bytes [6] = { 0 };
    size_t min_block, next_a, next_b;
    int ok, i;
    void *a, *ctrl;

    printf ("test: failure modes leave everything untouched, retry is clean\n");
    gen_main_pcm (pcm, 33);
    measure_candidates (2, 8000, 1, NOISE_SHAPING_DYNAMIC, pcm, 33, bytes, sse);
    CHECK (sse [5][0] > 0 || sse [5][1] > 0, "test construction: zero limits are not impossible");

    min_block = (size_t) adpcm_sample_count_to_block_size (33, 2, 2);

    a = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
    ctrl = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);

    // each failing call is checked for: return 0, out untouched, result untouched

#define EXPECT_FAIL(...) do { \
        memset (outbuf, 0xAA, sizeof (outbuf)); \
        memset (&res, 0x5A, sizeof (res)); \
        ok = adpcm_encode_adaptive (__VA_ARGS__); \
        CHECK (!ok, "failing call returned success"); \
        for (i = 0; i < 64; ++i) \
            if (outbuf [i] != 0xAA) { \
                CHECK (0, "failing call modified outbuf"); \
                break; \
            } \
        {   unsigned char *p = (unsigned char *) &res; \
            for (i = 0; i < (int) sizeof (res); ++i) \
                if (p [i] != 0x5A) { \
                    CHECK (0, "failing call modified result"); \
                    break; \
                } \
        } \
    } while (0)

    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, 33, 0x3C, impossible, &res);   // no candidate qualifies
    EXPECT_FAIL (a, outbuf, min_block - 1, pcm, 33, 0x3C, generous, &res);       // capacity too small
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, 1, 0x3C, generous, &res);      // frames too small
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, 4097, 0x3C, generous, &res);   // frames too large
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, 0, 0x3C, generous, &res);
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, -5, 0x3C, generous, &res);
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, 33, 0, generous, &res);        // empty mask
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, 33, 1 << 1, generous, &res);   // no valid bits
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, 33, (1 << 2) | (1 << 6), generous, &res); // invalid bit
    EXPECT_FAIL (a, NULL, sizeof (outbuf), pcm, 33, 0x3C, generous, &res);
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), NULL, 33, 0x3C, generous, &res);
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, 33, 0x3C, NULL, &res);
    EXPECT_FAIL (a, outbuf, sizeof (outbuf), pcm, 33, 0x3C, generous, NULL);
    EXPECT_FAIL (NULL, outbuf, sizeof (outbuf), pcm, 33, 0x3C, generous, &res);

    // now retry with relaxed limits on the same context: must equal a control
    // context that never saw any of the failures

    memset (outbuf, 0, sizeof (outbuf));
    memset (ctrlbuf, 0, sizeof (ctrlbuf));
    ok = adpcm_encode_adaptive (a, outbuf, sizeof (outbuf), pcm, 33, 0x3C, generous, &res);
    CHECK (ok, "retry after failures returned 0");
    ok = adpcm_encode_adaptive (ctrl, ctrlbuf, sizeof (ctrlbuf), pcm, 33, 0x3C, generous, &res2);
    CHECK (ok, "control adaptive failed");
    CHECK (res.bps == res2.bps && res.bytes == res2.bytes &&
           res.sse [0] == res2.sse [0] && res.sse [1] == res2.sse [1],
        "retry after failures differs from untouched control");
    CHECK (res.bytes == res2.bytes && !memcmp (outbuf, ctrlbuf, res.bytes),
        "retry bytes differ from control");

    // and continuations must match as well

    memset (outbuf2, 0, sizeof (outbuf2));
    memset (ctrlbuf, 0, sizeof (ctrlbuf));
    adpcm_encode_block_ex (a, outbuf2, &next_a, pcm, 33, 4);
    adpcm_encode_block_ex (ctrl, ctrlbuf, &next_b, pcm, 33, 4);
    CHECK (next_a == next_b && !memcmp (outbuf2, ctrlbuf, next_a),
        "continuation after failure+retry differs from control");
    printf ("  retry bps=%d bytes=%u, control bps=%d bytes=%u, continuation match=%d\n",
        res.bps, (unsigned) res.bytes, res2.bps, (unsigned) res2.bytes,
        next_a == next_b && !memcmp (outbuf2, ctrlbuf, next_a));

    adpcm_free_context (a);
    adpcm_free_context (ctrl);

    // capacity that fits only the smallest candidate must still succeed with it

    a = adpcm_create_context (2, 8000, 1, NOISE_SHAPING_DYNAMIC);
    memset (outbuf, 0, sizeof (outbuf));
    ok = adpcm_encode_adaptive (a, outbuf, min_block, pcm, 33, 0x3C, generous, &res);
    CHECK (ok && res.bps == 2 && res.bytes == min_block,
        "tight capacity: ok=%d bps=%d bytes=%u, expected bps=2 bytes=%u",
        ok, res.bps, (unsigned) res.bytes, (unsigned) min_block);
    printf ("  tight capacity (%u bytes): bps=%d ok\n", (unsigned) min_block, res.bps);
    adpcm_free_context (a);
}

int main (void)
{
    test_main_vector ();
    test_per_channel_limits ();
    test_byte_tie ();
    test_mask_independence ();
    test_mono ();
    test_history ();
    test_frame_counts ();
    test_failures ();

    if (failures) {
        printf ("\n%d FAILURE(S)\n", failures);
        return 1;
    }

    printf ("\nall tests passed\n");
    return 0;
}
