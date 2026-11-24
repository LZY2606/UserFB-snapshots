////////////////////////////////////////////////////////////////////////////
//                           **** WAVPACK ****                            //
//                  Hybrid Lossless Wavefile Compressor                   //
//                Copyright (c) 1998 - 2025 David Bryant                  //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// hybrid_plan_test.c
//
// Regression tests for the per-channel hybrid bitrate plan. The tests parse
// the actual WavPack block groups and HYBRID_PROFILE metadata themselves
// (rather than relying on internal state) and compare planned streams against
// independently encoded mono hybrid references.

#include "wavpack.h"
#include "../task_support/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TEST_BLOCK_SAMPLES 128
#define PLAN_MIN_Q8 576
#define PLAN_MAX_Q8 6144

///////////////////////////////////// storage //////////////////////////////////

struct mem_sink {
    unsigned char *data;
    size_t size, cap;
    unsigned blocks;
};

static int sink_block (void *id, void *data, int32_t size)
{
    struct mem_sink *s = id;

    if (size < 32)
        return 0;

    if (s->size + (size_t) size > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;

        while (cap < s->size + (size_t) size)
            cap *= 2;

        s->data = realloc (s->data, cap);

        if (!s->data)
            return 0;

        s->cap = cap;
    }

    memcpy (s->data + s->size, data, (size_t) size);
    s->size += (size_t) size;
    s->blocks++;
    return 1;
}

struct mem_reader {
    const unsigned char *data;
    size_t size, pos;
};

static int32_t mr_read (void *id, void *out, int32_t n)
{
    struct mem_reader *r = id;
    size_t avail = r->size - r->pos;

    if ((size_t) n > avail)
        n = (int32_t) avail;

    memcpy (out, r->data + r->pos, (size_t) n);
    r->pos += (size_t) n;
    return n;
}

static int64_t mr_pos (void *id) { return ((struct mem_reader *) id)->pos; }
static int mr_seek (void *id, int64_t pos) { struct mem_reader *r = id; r->pos = (size_t) pos; return 0; }

static int mr_seek_rel (void *id, int64_t delta, int mode)
{
    struct mem_reader *r = id;

    if (mode == SEEK_CUR) r->pos += (size_t) delta;
    else if (mode == SEEK_END) r->pos = r->size + (size_t) delta;
    else r->pos = (size_t) delta;

    return 0;
}

static int mr_push (void *id, int c) { struct mem_reader *r = id; if (r->pos) r->pos--; return c; }
static int64_t mr_len (void *id) { return ((struct mem_reader *) id)->size; }
static int mr_can_seek (void *id) { (void) id; return 1; }

static WavpackStreamReader64 mem_reader64 = {
    mr_read, NULL, mr_pos, mr_seek, mr_seek_rel, mr_push, mr_len, mr_can_seek, NULL, NULL
};

///////////////////////////////////// helpers //////////////////////////////////

static int32_t *make_pcm (uint32_t frames, int channels, int bps, uint32_t seed)
{
    uint32_t state = seed, i;
    int32_t *pcm = malloc ((size_t) frames * channels * sizeof (*pcm));

    if (!pcm)
        return NULL;

    for (i = 0; i < frames * (uint32_t) channels; i++) {
        state = state * 1664525u + 1013904223u;
        pcm [i] = (int32_t) ((state >> 16) & 65535u) - 32768;

        if (bps == 24)
            pcm [i] <<= 8;
    }

    return pcm;
}

struct encode_args {
    uint32_t frames;
    int channels, bps, workers, dynamic_shaping, total_known;
    uint32_t chunk;
    const WavpackHybridBitratePoint *plan;
    uint32_t plan_count;
};

static int run_encode (const struct encode_args *a, struct mem_sink *wv, struct mem_sink *wvc)
{
    WavpackConfig config;
    WavpackContext *enc;
    int32_t *pcm;
    uint32_t offset;

    pcm = make_pcm (a->frames, a->channels, a->bps, 1925 + a->channels * 7 + a->bps);

    if (!pcm)
        return 0;

    memset (wv, 0, sizeof (*wv));
    memset (wvc, 0, sizeof (*wvc));
    enc = WavpackOpenFileOutput (sink_block, wv, wvc);

    memset (&config, 0, sizeof (config));
    config.bytes_per_sample = a->bps / 8;
    config.bits_per_sample = a->bps;
    config.num_channels = a->channels;
    config.channel_mask = a->channels == 1 ? 0x4 :
        (a->channels == 2 ? 0x3 : (1 << a->channels) - 1);
    config.sample_rate = 48000;
    config.block_samples = a->dynamic_shaping ? 0 : TEST_BLOCK_SAMPLES;
    config.bitrate = 3.0f;
    config.worker_threads = a->workers;
    config.flags = CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC;

    if (a->dynamic_shaping)
        config.flags |= CONFIG_DYNAMIC_SHAPING;
    else
        config.flags |= CONFIG_SHAPE_OVERRIDE;

    if (!enc || !WavpackSetConfiguration64 (enc, &config, a->total_known ? (int64_t) a->frames : -1, NULL)) {
        free (pcm);
        return 0;
    }

    if (a->plan_count && !WavpackSetHybridBitratePlan (enc, a->plan, a->plan_count)) {
        WavpackCloseFile (enc);
        free (pcm);
        return 0;
    }

    if (!WavpackPackInit (enc)) {
        WavpackCloseFile (enc);
        free (pcm);
        return 0;
    }

    for (offset = 0; offset < a->frames;) {
        uint32_t n = a->frames - offset > a->chunk ? a->chunk : a->frames - offset;

        if (!WavpackPackSamples (enc, pcm + offset * a->channels, n)) {
            WavpackCloseFile (enc);
            free (pcm);
            return 0;
        }

        offset += n;
    }

    if (!WavpackFlushSamples (enc)) {
        WavpackCloseFile (enc);
        free (pcm);
        return 0;
    }

    WavpackCloseFile (enc);
    free (pcm);
    return 1;
}

static WavpackContext *open_decoder (struct mem_sink *wv, struct mem_sink *wvc, int use_wvc)
{
    static struct mem_reader rwv, rwvc;
    char error [128];

    rwv.data = wv->data;
    rwv.size = wv->size;
    rwv.pos = 0;

    if (use_wvc) {
        rwvc.data = wvc->data;
        rwvc.size = wvc->size;
        rwvc.pos = 0;
        return WavpackOpenFileInputEx64 (&mem_reader64, &rwv, &rwvc, error, OPEN_WVC, 0);
    }

    return WavpackOpenFileInputEx64 (&mem_reader64, &rwv, NULL, error, 0, 0);
}

// decode the whole stream and (optionally) compare every sample with the source

static int decode_all (struct mem_sink *wv, struct mem_sink *wvc, int use_wvc,
    const int32_t *ref, uint32_t frames, int channels, uint32_t *changed_out)
{
    WavpackContext *dec = open_decoder (wv, wvc, use_wvc);
    int32_t out [256];
    uint32_t offset = 0, got, changes = 0;

    if (!dec || WavpackGetNumChannels (dec) != channels)
        return 0;

    while ((got = WavpackUnpackSamples (dec, out, (uint32_t)(sizeof (out) / sizeof (out [0]) / channels))) != 0) {
        if (offset + got > frames) {
            WavpackCloseFile (dec);
            return 0;
        }

        if (ref && use_wvc &&
            memcmp (out, ref + offset * channels, got * (uint32_t) channels * sizeof (*out))) {
            WavpackCloseFile (dec);
            return 0;
        }

        if (ref && !use_wvc) {
            uint32_t i;

            for (i = 0; i < got * (uint32_t) channels; i++)
                changes += out [i] != ref [offset * channels + i];
        }

        offset += got;
    }

    if (dec)
        WavpackCloseFile (dec);

    if (offset != frames)
        return 0;

    if (changed_out)
        *changed_out = changes;

    return 1;
}

////////////////////////////////// block parsing ////////////////////////////////

struct block_info {
    int64_t index;
    uint32_t samples, flags;
    int initial, final, mono;
    int has_profile;
    int32_t acc [2];          // bitrate accumulator high words from HYBRID_PROFILE
};

static int parse_blocks (const unsigned char *data, size_t size, struct block_info *out, int max_blocks)
{
    size_t pos = 0;
    int count = 0;

    while (pos + 32 <= size && count < max_blocks) {
        const unsigned char *h = data + pos;
        uint32_t ck_size, flags, block_samples, meta_left;
        int64_t block_index;

        if (memcmp (h, "wvpk", 4))
            break;

        ck_size = (uint32_t) h [4] | ((uint32_t) h [5] << 8) |
                  ((uint32_t) h [6] << 16) | ((uint32_t) h [7] << 24);
        block_index = (uint32_t) h [16] | ((uint32_t) h [17] << 8) |
                      ((uint32_t) h [18] << 16) | ((uint32_t) h [19] << 24);
        block_index += (int64_t) h [10] << 32;
        block_samples = (uint32_t) h [20] | ((uint32_t) h [21] << 8) |
                        ((uint32_t) h [22] << 16) | ((uint32_t) h [23] << 24);
        flags = (uint32_t) h [24] | ((uint32_t) h [25] << 8) |
                ((uint32_t) h [26] << 16) | ((uint32_t) h [27] << 24);

        if (ck_size + 8 < 32 || pos + ck_size + 8 > size)
            break;

        out [count].index = block_index;
        out [count].samples = block_samples;
        out [count].flags = flags;
        out [count].initial = !!(flags & INITIAL_BLOCK);
        out [count].final = !!(flags & FINAL_BLOCK);
        out [count].mono = !!(flags & MONO_FLAG);
        out [count].has_profile = 0;
        out [count].acc [0] = out [count].acc [1] = 0;

        meta_left = ck_size - 24;       // metadata bytes follow the 32-byte header
        pos += 32;

        while (meta_left >= 2) {
            unsigned char meta_id = data [pos];
            uint32_t meta_bc = (uint32_t) data [pos + 1] << 1;
            uint32_t header = 2;

            if (meta_id & ID_LARGE) {
                if (meta_left < 4)
                    break;

                meta_bc += (uint32_t) data [pos + 2] << 9;
                meta_bc += (uint32_t) data [pos + 3] << 17;
                header = 4;
            }

            if (meta_bc > meta_left - header)
                break;

            if ((meta_id & ID_UNIQUE) == ID_HYBRID_PROFILE) {
                const unsigned char *p = data + pos + header;
                uint32_t left = meta_bc - (meta_id & ID_ODD_SIZE ? 1 : 0);
                int stereo = !(flags & MONO_FLAG);
                uint32_t need = (stereo ? 4u : 2u) + (stereo ? 4u : 2u);

                if (left >= need) {
                    p += stereo ? 4 : 2;                       // skip slow_level value(s)
                    out [count].acc [0] = (int32_t)(p [0] | (p [1] << 8));

                    if (stereo)
                        out [count].acc [1] = (int32_t)(p [2] | (p [3] << 8));

                    out [count].has_profile = 1;
                }
            }

            pos += header + meta_bc;
            meta_left -= header + meta_bc;
        }

        // skip anything the metadata walk did not consume (e.g. checksums)
        pos = (size_t)((h - data) + ck_size + 8);
        count++;
    }

    return count;
}

#define MAX_BLOCKS 512

struct group {
    int64_t index;
    uint32_t samples;
    int first_block, num_blocks;
};

// split a parsed block array into multichannel block groups

static int parse_groups (const struct block_info *blocks, int nblocks, struct group *groups, int max_groups)
{
    int ngroups = 0, i;

    for (i = 0; i < nblocks; i++) {
        if (blocks [i].initial) {
            if (ngroups >= max_groups)
                return -1;

            groups [ngroups].index = blocks [i].index;
            groups [ngroups].samples = blocks [i].samples;
            groups [ngroups].first_block = i;
            groups [ngroups].num_blocks = 0;
            ngroups++;
        }

        if (ngroups)
            groups [ngroups - 1].num_blocks++;
    }

    return ngroups;
}

// mono hybrid reference at one Q8 budget: returns the stored bitrate
// accumulator high word of the first audio block, or -1 on failure

static int mono_reference_acc (int budget_q8, int bps)
{
    WavpackHybridBitratePoint point;
    uint16_t budget [1] = { (uint16_t) budget_q8 };
    struct encode_args a;
    struct mem_sink wv, wvc;
    struct block_info blocks [8];
    int n;

    point.sample_index = 0;
    point.bits_per_sample_q8 = budget;

    memset (&a, 0, sizeof (a));
    a.frames = TEST_BLOCK_SAMPLES * 2;
    a.channels = 1;
    a.bps = bps;
    a.workers = 0;
    a.chunk = TEST_BLOCK_SAMPLES;
    a.total_known = 1;
    a.plan = &point;
    a.plan_count = 1;

    if (!run_encode (&a, &wv, &wvc))
        return -1;

    n = parse_blocks (wv.data, wv.size, blocks, 8);
    free (wv.data);
    free (wvc.data);

    if (n < 1 || !blocks [0].has_profile)
        return -1;

    return blocks [0].acc [0];
}

static int budget_to_acc (int budget_q8)
{
    int v = budget_q8 - 568;

    return v < 0 ? 0 : v;
}

///////////////////////////////////// cases ////////////////////////////////////

// ---------- schedule group ----------

static int case_schedule_basic_cuts (void)
{
    uint16_t b0 [1] = { 768 }, b1 [1] = { 1536 }, b2 [1] = { 2304 }, b3 [1] = { 3072 };
    WavpackHybridBitratePoint plan [4] = {
        { 0, b0 }, { 100, b1 }, { 257, b2 }, { 600, b3 }
    };
    struct encode_args a;
    struct mem_sink wv, wvc;
    struct block_info blocks [MAX_BLOCKS];
    struct group groups [MAX_BLOCKS];
    int nblocks, ngroups, i;
    int64_t cuts [3] = { 100, 257, 600 };

    memset (&a, 0, sizeof (a));
    a.frames = 1000;
    a.channels = 1;
    a.bps = 16;
    a.workers = 0;
    a.chunk = 97;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 4;

    TASK_CHECK (run_encode (&a, &wv, &wvc));
    nblocks = parse_blocks (wv.data, wv.size, blocks, MAX_BLOCKS);
    ngroups = parse_groups (blocks, nblocks, groups, MAX_BLOCKS);
    TASK_CHECK (nblocks > 4 && ngroups > 3);

    for (i = 0; i < nblocks; i++)
        TASK_CHECK (blocks [i].samples > 0);                 // no empty audio blocks

    for (i = 0; i < ngroups; i++) {
        int k;

        if (i)
            TASK_CHECK (groups [i].index == groups [i - 1].index + (int64_t) groups [i - 1].samples);

        for (k = 0; k < 3; k++) {
            // a cut may never sit strictly inside a group
            TASK_CHECK (!(groups [i].index < cuts [k] &&
                cuts [k] < groups [i].index + (int64_t) groups [i].samples));
        }
    }

    for (i = 0; i < 3; i++) {
        int found = 0, k;

        for (k = 0; k < ngroups; k++)
            if (groups [k].index == cuts [i])
                found = 1;

        TASK_CHECK (found);                                  // every cut is a real boundary
    }

    TASK_CHECK (groups [ngroups - 1].index + groups [ngroups - 1].samples == 1000);
    free (wv.data);
    free (wvc.data);
    return 0;
}

static int case_schedule_big_multicut (void)
{
    uint16_t budgets [6][2] = {
        { 768, 1024 }, { 2048, 768 }, { 1024, 3072 }, { 4096, 576 }, { 1536, 1536 }, { 6144, 2304 }
    };
    WavpackHybridBitratePoint plan [6] = {
        { 0, budgets [0] }, { 100, budgets [1] }, { 257, budgets [2] },
        { 513, budgets [3] }, { 900, budgets [4] }, { 1499, budgets [5] }
    };
    struct encode_args a;
    struct mem_sink wv0, wvc0, wv2, wvc2;
    struct block_info b0 [MAX_BLOCKS], b2 [MAX_BLOCKS];
    struct group g0 [MAX_BLOCKS], g2 [MAX_BLOCKS];
    int n0, n2, ng0, ng2, i;

    memset (&a, 0, sizeof (a));
    a.frames = 2000;
    a.channels = 2;
    a.bps = 16;
    a.chunk = 61;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 6;

    a.workers = 0;
    TASK_CHECK (run_encode (&a, &wv0, &wvc0));
    a.workers = 2;
    TASK_CHECK (run_encode (&a, &wv2, &wvc2));

    n0 = parse_blocks (wv0.data, wv0.size, b0, MAX_BLOCKS);
    n2 = parse_blocks (wv2.data, wv2.size, b2, MAX_BLOCKS);
    ng0 = parse_groups (b0, n0, g0, MAX_BLOCKS);
    ng2 = parse_groups (b2, n2, g2, MAX_BLOCKS);
    TASK_CHECK (ng0 == ng2 && ng0 >= 6);

    for (i = 0; i < ng0; i++) {
        TASK_CHECK (g0 [i].index == g2 [i].index);          // worker scheduling cannot move cuts
        TASK_CHECK (g0 [i].samples == g2 [i].samples);
    }

    for (i = 1; i < 6; i++) {
        int found = 0, k;

        for (k = 0; k < ng0; k++)
            if (g0 [k].index == plan [i].sample_index)
                found = 1;

        TASK_CHECK (found);
    }

    free (wv0.data); free (wvc0.data);
    free (wv2.data); free (wvc2.data);
    return 0;
}

static int case_schedule_six_channel (void)
{
    uint16_t budgets [3][6];
    WavpackHybridBitratePoint plan [3];
    struct encode_args a;
    struct mem_sink wv, wvc;
    struct block_info blocks [MAX_BLOCKS];
    struct group groups [MAX_BLOCKS];
    int nblocks, ngroups, i, ch;

    for (i = 0; i < 3; i++)
        for (ch = 0; ch < 6; ch++)
            budgets [i][ch] = (uint16_t)(768 + ((i * 3 + ch) * 337) % (PLAN_MAX_Q8 - 768));

    plan [0].sample_index = 0;     plan [0].bits_per_sample_q8 = budgets [0];
    plan [1].sample_index = 300;   plan [1].bits_per_sample_q8 = budgets [1];
    plan [2].sample_index = 800;   plan [2].bits_per_sample_q8 = budgets [2];

    memset (&a, 0, sizeof (a));
    a.frames = 1379;
    a.channels = 6;
    a.bps = 16;
    a.workers = 2;
    a.chunk = 97;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 3;

    TASK_CHECK (run_encode (&a, &wv, &wvc));
    nblocks = parse_blocks (wv.data, wv.size, blocks, MAX_BLOCKS);
    ngroups = parse_groups (blocks, nblocks, groups, MAX_BLOCKS);
    TASK_CHECK (ngroups >= 3);

    for (i = 0; i < ngroups; i++)
        TASK_CHECK (groups [i].num_blocks == 4);            // stereo, mono, mono, stereo pairing

    for (i = 0; i < nblocks; i++) {
        TASK_CHECK (blocks [i].samples > 0);
        TASK_CHECK (blocks [i].has_profile);

        if (!blocks [i].mono) {
            TASK_CHECK (!(blocks [i].flags & JOINT_STEREO));
            TASK_CHECK (!(blocks [i].flags & CROSS_DECORR));
            TASK_CHECK (!(blocks [i].flags & HYBRID_BALANCE));
        }
    }

    for (i = 0; i < nblocks; i++) {
        int64_t idx = blocks [i].index;
        int active = idx >= 800 ? 2 : (idx >= 300 ? 1 : 0);
        int group_start = i, si, ch = 0;

        // blocks sharing an index are emitted consecutively in stream order
        while (group_start > 0 && blocks [group_start - 1].index == idx)
            group_start--;

        TASK_CHECK (i - group_start < 4);

        // earlier streams consume 1 channel (mono) or 2 channels (stereo)
        for (si = group_start; si < i; si++)
            ch += blocks [si].mono ? 1 : 2;

        if (blocks [i].mono) {
            TASK_CHECK (blocks [i].acc [0] == budget_to_acc (budgets [active][ch]));
            // cross-check against an independently encoded mono hybrid stream
            TASK_CHECK (mono_reference_acc (budgets [active][ch], 16) == blocks [i].acc [0]);
        }
        else {
            TASK_CHECK (blocks [i].acc [0] == budget_to_acc (budgets [active][ch]));
            TASK_CHECK (blocks [i].acc [1] == budget_to_acc (budgets [active][ch + 1]));
            TASK_CHECK (mono_reference_acc (budgets [active][ch + 1], 16) == blocks [i].acc [1]);
        }
    }

    free (wv.data);
    free (wvc.data);
    return 0;
}

static int case_schedule_unknown_length (void)
{
    uint16_t b0 [2] = { 1024, 1024 }, b1 [2] = { 2048, 2048 }, b2 [2] = { 3072, 3072 };
    WavpackHybridBitratePoint plan [3] = {
        { 0, b0 }, { 300, b1 }, { 700, b2 }
    };
    struct encode_args a;
    struct mem_sink wv_hold, wvc_hold, wv_full, wvc_full;
    struct block_info bh [MAX_BLOCKS], bf [MAX_BLOCKS];
    struct group gh [MAX_BLOCKS], gf [MAX_BLOCKS];
    int nh, nf, ngh, ngf;

    memset (&a, 0, sizeof (a));
    a.channels = 2;
    a.bps = 16;
    a.workers = 0;
    a.chunk = 50;
    a.plan = plan;
    a.plan_count = 3;

    a.frames = 250;
    a.total_known = 0;
    TASK_CHECK (run_encode (&a, &wv_hold, &wvc_hold));
    nh = parse_blocks (wv_hold.data, wv_hold.size, bh, MAX_BLOCKS);
    ngh = parse_groups (bh, nh, gh, MAX_BLOCKS);
    TASK_CHECK (ngh >= 1);
    TASK_CHECK (gh [0].index == 0);

    // points not yet reached must not have produced any output
    TASK_CHECK (gh [ngh - 1].index + gh [ngh - 1].samples == 250);

    a.frames = 1000;
    a.total_known = 0;
    TASK_CHECK (run_encode (&a, &wv_full, &wvc_full));
    nf = parse_blocks (wv_full.data, wv_full.size, bf, MAX_BLOCKS);
    ngf = parse_groups (bf, nf, gf, MAX_BLOCKS);
    TASK_CHECK (ngf >= 3);

    {
        int found_300 = 0, found_700 = 0, k;

        for (k = 0; k < ngf; k++) {
            if (gf [k].index == 300)
                found_300 = 1;

            if (gf [k].index == 700)
                found_700 = 1;
        }

        TASK_CHECK (found_300 && found_700);
    }

    free (wv_hold.data); free (wvc_hold.data);
    free (wv_full.data); free (wvc_full.data);
    return 0;
}

static int case_schedule_short_tail (void)
{
    uint16_t b0 [1] = { 1024 }, b1 [1] = { 4096 };
    WavpackHybridBitratePoint plan [2] = { { 0, b0 }, { 250, b1 } };
    struct encode_args a;
    struct mem_sink wv, wvc;
    struct block_info blocks [MAX_BLOCKS];
    struct group groups [MAX_BLOCKS];
    int nblocks, ngroups;

    memset (&a, 0, sizeof (a));
    a.frames = 300;
    a.channels = 1;
    a.bps = 16;
    a.workers = 0;
    a.chunk = 37;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 2;

    TASK_CHECK (run_encode (&a, &wv, &wvc));
    nblocks = parse_blocks (wv.data, wv.size, blocks, MAX_BLOCKS);
    ngroups = parse_groups (blocks, nblocks, groups, MAX_BLOCKS);
    TASK_CHECK (ngroups >= 2);
    TASK_CHECK (groups [ngroups - 1].samples == 50);         // short tail block
    TASK_CHECK (groups [ngroups - 1].index == 250);
    TASK_CHECK (groups [ngroups - 1].samples > 0);

    free (wv.data);
    free (wvc.data);
    return 0;
}

static int case_schedule_dynamic_shaping (void)
{
    uint16_t b0 [2] = { 1280, 1280 }, b1 [2] = { 2560, 2560 }, b2 [2] = { 3840, 3840 };
    WavpackHybridBitratePoint plan [3] = {
        { 0, b0 }, { 500, b1 }, { 1100, b2 }
    };
    struct encode_args a;
    struct mem_sink wv, wvc;
    struct block_info blocks [MAX_BLOCKS];
    struct group groups [MAX_BLOCKS];
    int nblocks, ngroups, i;

    memset (&a, 0, sizeof (a));
    a.frames = 1500;
    a.channels = 2;
    a.bps = 16;
    a.workers = 0;
    a.chunk = 123;
    a.total_known = 1;
    a.dynamic_shaping = 1;
    a.plan = plan;
    a.plan_count = 3;

    TASK_CHECK (run_encode (&a, &wv, &wvc));
    nblocks = parse_blocks (wv.data, wv.size, blocks, MAX_BLOCKS);
    ngroups = parse_groups (blocks, nblocks, groups, MAX_BLOCKS);
    TASK_CHECK (ngroups >= 3);

    for (i = 0; i < ngroups; i++) {
        int64_t end = groups [i].index + groups [i].samples;

        // the blocks ending on cuts must keep their full length despite DNS shrinking
        if (end == 500 || end == 1100)
            TASK_CHECK (groups [i].index == 500 || groups [i].index == 0 ||
                        groups [i].index == 1100);

        TASK_CHECK (groups [i].samples >= 16);
    }

    TASK_CHECK (groups [0].index == 0);
    free (wv.data);
    free (wvc.data);
    return 0;
}

static int case_schedule_split_vs_merged (void)
{
    uint16_t b0 [6] = { 800, 900, 1000, 1100, 1200, 1300 };
    uint16_t b1 [6] = { 4000, 3800, 3600, 3400, 3200, 3000 };
    WavpackHybridBitratePoint plan [2] = { { 0, b0 }, { 433, b1 } };
    struct encode_args a;
    struct mem_sink wv_a, wvc_a, wv_b, wvc_b;
    struct block_info ba [MAX_BLOCKS], bb [MAX_BLOCKS];
    struct group ga [MAX_BLOCKS], gb [MAX_BLOCKS];
    int na, nb, nga, ngb, i;
    int32_t *pcm;
    uint32_t changed;

    memset (&a, 0, sizeof (a));
    a.frames = 1000;
    a.channels = 6;
    a.bps = 16;
    a.workers = 2;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 2;

    a.chunk = 1;
    TASK_CHECK (run_encode (&a, &wv_a, &wvc_a));
    a.chunk = 500;
    TASK_CHECK (run_encode (&a, &wv_b, &wvc_b));

    na = parse_blocks (wv_a.data, wv_a.size, ba, MAX_BLOCKS);
    nb = parse_blocks (wv_b.data, wv_b.size, bb, MAX_BLOCKS);
    nga = parse_groups (ba, na, ga, MAX_BLOCKS);
    ngb = parse_groups (bb, nb, gb, MAX_BLOCKS);
    TASK_CHECK (nga == ngb);

    for (i = 0; i < nga; i++) {
        TASK_CHECK (ga [i].index == gb [i].index);
        TASK_CHECK (ga [i].samples == gb [i].samples);
    }

    for (i = 0; i < na; i++) {
        TASK_CHECK (ba [i].has_profile && bb [i].has_profile);
        TASK_CHECK (ba [i].acc [0] == bb [i].acc [0]);
        TASK_CHECK (ba [i].acc [1] == bb [i].acc [1]);
    }

    pcm = make_pcm (a.frames, a.channels, 16, 1925 + a.channels * 7 + 16);
    TASK_CHECK (pcm);
    TASK_CHECK (decode_all (&wv_a, &wvc_a, 1, pcm, a.frames, a.channels, NULL));
    TASK_CHECK (decode_all (&wv_b, &wvc_b, 1, pcm, a.frames, a.channels, NULL));
    TASK_CHECK (decode_all (&wv_a, &wvc_a, 0, pcm, a.frames, a.channels, &changed));
    TASK_CHECK (changed > 0);
    free (pcm);

    free (wv_a.data); free (wvc_a.data);
    free (wv_b.data); free (wvc_b.data);
    return 0;
}

static int case_schedule_workers (void)
{
    uint16_t b0 [6], b1 [6];
    WavpackHybridBitratePoint plan [2] = { { 0, b0 }, { 512, b1 } };
    struct encode_args a;
    struct mem_sink wv0, wvc0, wv2, wvc2;
    struct block_info x0 [MAX_BLOCKS], x2 [MAX_BLOCKS];
    struct group g0 [MAX_BLOCKS], g2 [MAX_BLOCKS];
    int n0, n2, ng0, ng2, i;
    int32_t *pcm;

    for (i = 0; i < 6; i++) {
        b0 [i] = (uint16_t)(700 + i * 250);
        b1 [i] = (uint16_t)(1500 + i * 400);
    }

    memset (&a, 0, sizeof (a));
    a.frames = 1200;
    a.channels = 6;
    a.bps = 16;
    a.chunk = 77;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 2;

    a.workers = 0;
    TASK_CHECK (run_encode (&a, &wv0, &wvc0));
    a.workers = 2;
    TASK_CHECK (run_encode (&a, &wv2, &wvc2));

    n0 = parse_blocks (wv0.data, wv0.size, x0, MAX_BLOCKS);
    n2 = parse_blocks (wv2.data, wv2.size, x2, MAX_BLOCKS);
    ng0 = parse_groups (x0, n0, g0, MAX_BLOCKS);
    ng2 = parse_groups (x2, n2, g2, MAX_BLOCKS);
    TASK_CHECK (ng0 == ng2);

    for (i = 0; i < ng0; i++) {
        TASK_CHECK (g0 [i].index == g2 [i].index);
        TASK_CHECK (g0 [i].samples == g2 [i].samples);
    }

    pcm = make_pcm (a.frames, a.channels, 16, 1925 + a.channels * 7 + 16);
    TASK_CHECK (pcm);
    TASK_CHECK (decode_all (&wv0, &wvc0, 1, pcm, a.frames, a.channels, NULL));
    TASK_CHECK (decode_all (&wv2, &wvc2, 1, pcm, a.frames, a.channels, NULL));
    free (pcm);

    free (wv0.data); free (wvc0.data);
    free (wv2.data); free (wvc2.data);
    return 0;
}

// ---------- budgets group ----------

static int case_budget_mono_reference (void)
{
    static const int budgets [4] = { 576, 768, 2048, 6144 };
    uint16_t b [4];
    WavpackHybridBitratePoint plan [4];
    struct encode_args a;
    struct mem_sink wv, wvc;
    struct block_info blocks [MAX_BLOCKS];
    int n, i, ref [4];

    for (i = 0; i < 4; i++) {
        b [i] = (uint16_t) budgets [i];
        plan [i].sample_index = i ? i * 100 : 0;
        plan [i].bits_per_sample_q8 = &b [i];

        ref [i] = mono_reference_acc (budgets [i], 16);
        TASK_CHECK (ref [i] >= 0);
        TASK_CHECK (ref [i] == budget_to_acc (budgets [i]));
    }

        memset (&a, 0, sizeof (a));
        a.frames = 500;
        a.channels = 1;
        a.bps = 16;
        a.workers = 0;
        a.chunk = 33;
        a.total_known = 1;
        a.plan = plan;
        a.plan_count = 4;

        TASK_CHECK (run_encode (&a, &wv, &wvc));
        n = parse_blocks (wv.data, wv.size, blocks, MAX_BLOCKS);
        TASK_CHECK (n >= 4);

        for (i = 0; i < n; i++) {
            int active = blocks [i].index >= 300 ? 3 :
                blocks [i].index >= 200 ? 2 : blocks [i].index >= 100 ? 1 : 0;

            TASK_CHECK (blocks [i].has_profile);
            TASK_CHECK (blocks [i].acc [0] == ref [active]);
        }

        free (wv.data);
        free (wvc.data);

    return 0;
}

static int case_budget_stereo_independent (void)
{
    uint16_t b0 [2] = { 768, 3072 }, b1 [2] = { 4096, 1024 };
    WavpackHybridBitratePoint plan [2] = { { 0, b0 }, { 200, b1 } };
    struct encode_args a;
    struct mem_sink wv, wvc;
    struct block_info blocks [MAX_BLOCKS];
    int n, i, ref_lo, ref_hi, ref_4096, ref_1024;

    ref_lo = mono_reference_acc (768, 16);
    ref_hi = mono_reference_acc (3072, 16);
    ref_4096 = mono_reference_acc (4096, 16);
    ref_1024 = mono_reference_acc (1024, 16);
    TASK_CHECK (ref_lo >= 0 && ref_hi >= 0 && ref_4096 >= 0 && ref_1024 >= 0);
    TASK_CHECK (ref_lo != ref_hi);

    memset (&a, 0, sizeof (a));
    a.frames = 400;
    a.channels = 2;
    a.bps = 16;
    a.workers = 0;
    a.chunk = 41;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 2;

    TASK_CHECK (run_encode (&a, &wv, &wvc));
    n = parse_blocks (wv.data, wv.size, blocks, MAX_BLOCKS);
    TASK_CHECK (n >= 2);

    for (i = 0; i < n; i++) {
        int active = blocks [i].index >= 200 ? 1 : 0;
        int expect0 = active ? ref_4096 : ref_lo;
        int expect1 = active ? ref_1024 : ref_hi;

        TASK_CHECK (blocks [i].has_profile && !blocks [i].mono);
        TASK_CHECK (!(blocks [i].flags & JOINT_STEREO));
        TASK_CHECK (!(blocks [i].flags & CROSS_DECORR));
        TASK_CHECK (!(blocks [i].flags & HYBRID_BALANCE));
        TASK_CHECK (blocks [i].acc [0] == expect0);
        TASK_CHECK (blocks [i].acc [1] == expect1);
    }

    free (wv.data);
    free (wvc.data);
    return 0;
}

static int case_budget_entropy_effect (void)
{
    uint16_t low [2] = { 576, 576 }, high [2] = { 6144, 6144 };
    WavpackHybridBitratePoint plan_lo [1] = { { 0, low } };
    WavpackHybridBitratePoint plan_hi [1] = { { 0, high } };
    struct encode_args a;
    struct mem_sink wv_lo, wvc_lo, wv_hi, wvc_hi;
    int32_t *pcm;
    uint32_t changed_lo, changed_hi;

    memset (&a, 0, sizeof (a));
    a.frames = 800;
    a.channels = 2;
    a.bps = 16;
    a.workers = 0;
    a.chunk = 64;
    a.total_known = 1;

    a.plan = plan_lo;
    a.plan_count = 1;
    TASK_CHECK (run_encode (&a, &wv_lo, &wvc_lo));

    a.plan = plan_hi;
    TASK_CHECK (run_encode (&a, &wv_hi, &wvc_hi));

    // the budgets reach the entropy coder: a higher WV budget spends more bytes
    // in the lossy stream and leaves less work for the correction stream
    TASK_CHECK (wv_hi.size > wv_lo.size);
    TASK_CHECK (wvc_hi.size < wvc_lo.size);

    pcm = make_pcm (a.frames, a.channels, 16, 1925 + a.channels * 7 + 16);
    TASK_CHECK (pcm);
    TASK_CHECK (decode_all (&wv_lo, &wvc_lo, 0, pcm, a.frames, a.channels, &changed_lo));
    TASK_CHECK (decode_all (&wv_hi, &wvc_hi, 0, pcm, a.frames, a.channels, &changed_hi));
    TASK_CHECK (changed_lo > changed_hi);                     // low budget quantizes more
    TASK_CHECK (decode_all (&wv_lo, &wvc_lo, 1, pcm, a.frames, a.channels, NULL));
    TASK_CHECK (decode_all (&wv_hi, &wvc_hi, 1, pcm, a.frames, a.channels, NULL));
    free (pcm);

    free (wv_lo.data); free (wvc_lo.data);
    free (wv_hi.data); free (wvc_hi.data);
    return 0;
}

static int case_budget_identical_stereo (void)
{
    uint16_t b0 [2] = { 1536, 1536 }, b1 [2] = { 3072, 3072 };
    WavpackHybridBitratePoint plan [2] = { { 0, b0 }, { 200, b1 } };
    WavpackConfig config;
    WavpackContext *enc;
    struct mem_sink wv, wvc;
    struct block_info blocks [MAX_BLOCKS];
    int n, i;
    int32_t pcm [600 * 2];

    for (i = 0; i < 600; i++) {
        pcm [i * 2] = (i * 37) % 2000 - 1000;
        pcm [i * 2 + 1] = pcm [i * 2];                    // identical stereo channels
    }

    memset (&wv, 0, sizeof (wv));
    memset (&wvc, 0, sizeof (wvc));
    enc = WavpackOpenFileOutput (sink_block, &wv, &wvc);
    memset (&config, 0, sizeof (config));
    config.bytes_per_sample = 2;
    config.bits_per_sample = 16;
    config.num_channels = 2;
    config.channel_mask = 3;
    config.sample_rate = 48000;
    config.block_samples = TEST_BLOCK_SAMPLES;
    config.bitrate = 3.0f;
    config.flags = CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC | CONFIG_SHAPE_OVERRIDE;

    TASK_CHECK (enc && WavpackSetConfiguration64 (enc, &config, 600, NULL));
    TASK_CHECK (WavpackSetHybridBitratePlan (enc, plan, 2));
    TASK_CHECK (WavpackPackInit (enc));
    TASK_CHECK (WavpackPackSamples (enc, pcm, 600));
    TASK_CHECK (WavpackFlushSamples (enc));
    WavpackCloseFile (enc);

    n = parse_blocks (wv.data, wv.size, blocks, MAX_BLOCKS);
    TASK_CHECK (n >= 2);

    for (i = 0; i < n; i++) {
        TASK_CHECK (!(blocks [i].flags & FALSE_STEREO));   // folding stays disabled
        TASK_CHECK (!(blocks [i].flags & MONO_FLAG));
        TASK_CHECK (blocks [i].acc [0] == blocks [i].acc [1]);
    }

    TASK_CHECK (decode_all (&wv, &wvc, 1, pcm, 600, 2, NULL));
    free (wv.data);
    free (wvc.data);
    return 0;
}

static int case_budget_24bit (void)
{
    uint16_t b0 [3] = { 1024, 2048, 3072 }, b1 [3] = { 4096, 5120, 6144 };
    WavpackHybridBitratePoint plan [2] = { { 0, b0 }, { 300, b1 } };
    struct encode_args a;
    struct mem_sink wv, wvc;
    struct block_info blocks [MAX_BLOCKS];
    struct group groups [MAX_BLOCKS];
    int nblocks, ngroups;
    int32_t *pcm;

    memset (&a, 0, sizeof (a));
    a.frames = 700;
    a.channels = 3;
    a.bps = 24;
    a.workers = 2;
    a.chunk = 53;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 2;

    TASK_CHECK (run_encode (&a, &wv, &wvc));
    nblocks = parse_blocks (wv.data, wv.size, blocks, MAX_BLOCKS);
    ngroups = parse_groups (blocks, nblocks, groups, MAX_BLOCKS);
    TASK_CHECK (ngroups >= 2);

    {
        int found = 0, k;

        for (k = 0; k < ngroups; k++)
            if (groups [k].index == 300)
                found = 1;

        TASK_CHECK (found);
    }

    pcm = make_pcm (a.frames, a.channels, 24, 1925 + a.channels * 7 + 24);
    TASK_CHECK (pcm);
    TASK_CHECK (decode_all (&wv, &wvc, 1, pcm, a.frames, a.channels, NULL));
    free (pcm);

    free (wv.data);
    free (wvc.data);
    return 0;
}

// ---------- compatibility group ----------

static int case_compat_wvc_exact (void)
{
    uint16_t b0 [6], b1 [6], b2 [6];
    WavpackHybridBitratePoint plan [3] = { { 0, b0 }, { 400, b1 }, { 900, b2 } };
    struct encode_args a;
    struct mem_sink wv, wvc;
    int32_t *pcm;
    int i;

    for (i = 0; i < 6; i++) {
        b0 [i] = (uint16_t)(800 + i * 300);
        b1 [i] = (uint16_t)(2000 + i * 300);
        b2 [i] = (uint16_t)(3500 + i * 400);
    }

    memset (&a, 0, sizeof (a));
    a.frames = 1379;
    a.channels = 6;
    a.bps = 16;
    a.workers = 2;
    a.chunk = 97;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 3;

    TASK_CHECK (run_encode (&a, &wv, &wvc));
    TASK_CHECK (wv.blocks == wvc.blocks);

    pcm = make_pcm (a.frames, a.channels, 16, 1925 + a.channels * 7 + 16);
    TASK_CHECK (pcm);
    TASK_CHECK (decode_all (&wv, &wvc, 1, pcm, a.frames, a.channels, NULL));
    free (pcm);

    free (wv.data);
    free (wvc.data);
    return 0;
}

static int case_compat_lossy (void)
{
    uint16_t b0 [2] = { 768, 768 };
    WavpackHybridBitratePoint plan [1] = { { 0, b0 } };
    struct encode_args a;
    struct mem_sink wv, wvc;
    int32_t *pcm;
    uint32_t changed = 0;

    memset (&a, 0, sizeof (a));
    a.frames = 600;
    a.channels = 2;
    a.bps = 16;
    a.workers = 0;
    a.chunk = 64;
    a.total_known = 1;
    a.plan = plan;
    a.plan_count = 1;

    TASK_CHECK (run_encode (&a, &wv, &wvc));
    pcm = make_pcm (a.frames, a.channels, 16, 1925 + a.channels * 7 + 16);
    TASK_CHECK (pcm);
    TASK_CHECK (decode_all (&wv, &wvc, 0, pcm, a.frames, a.channels, &changed));
    TASK_CHECK (changed > 0);
    free (pcm);

    free (wv.data);
    free (wvc.data);
    return 0;
}

static int case_compat_clear_plan (void)
{
    WavpackConfig config;
    WavpackContext *enc;
    struct mem_sink wv1, wvc1, wv2, wvc2;
    WavpackHybridBitratePoint plan;
    uint16_t budget [2] = { 1024, 2048 };
    int32_t *pcm;
    uint32_t frames = 500, offset;

    plan.sample_index = 0;
    plan.bits_per_sample_q8 = budget;
    pcm = make_pcm (frames, 2, 16, 1234);
    TASK_CHECK (pcm);

    // reference: no plan at all
    memset (&wv1, 0, sizeof (wv1));
    memset (&wvc1, 0, sizeof (wvc1));
    enc = WavpackOpenFileOutput (sink_block, &wv1, &wvc1);
    memset (&config, 0, sizeof (config));
    config.bytes_per_sample = 2;
    config.bits_per_sample = 16;
    config.num_channels = 2;
    config.channel_mask = 3;
    config.sample_rate = 48000;
    config.block_samples = TEST_BLOCK_SAMPLES;
    config.bitrate = 3.0f;
    config.flags = CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC | CONFIG_SHAPE_OVERRIDE;
    TASK_CHECK (enc && WavpackSetConfiguration64 (enc, &config, (int64_t) frames, NULL));
    TASK_CHECK (WavpackPackInit (enc));

    for (offset = 0; offset < frames; offset += 73)
        TASK_CHECK (WavpackPackSamples (enc, pcm + offset * 2, frames - offset > 73 ? 73 : frames - offset));

    TASK_CHECK (WavpackFlushSamples (enc));
    WavpackCloseFile (enc);

    // set a plan and then clear it before packing
    memset (&wv2, 0, sizeof (wv2));
    memset (&wvc2, 0, sizeof (wvc2));
    enc = WavpackOpenFileOutput (sink_block, &wv2, &wvc2);
    TASK_CHECK (enc && WavpackSetConfiguration64 (enc, &config, (int64_t) frames, NULL));
    TASK_CHECK (WavpackSetHybridBitratePlan (enc, &plan, 1));
    TASK_CHECK (WavpackSetHybridBitratePlan (enc, NULL, 0));
    TASK_CHECK (WavpackPackInit (enc));

    for (offset = 0; offset < frames; offset += 73)
        TASK_CHECK (WavpackPackSamples (enc, pcm + offset * 2, frames - offset > 73 ? 73 : frames - offset));

    TASK_CHECK (WavpackFlushSamples (enc));
    WavpackCloseFile (enc);

    TASK_CHECK (wv1.size == wv2.size && !memcmp (wv1.data, wv2.data, wv1.size));
    TASK_CHECK (wvc1.size == wvc2.size && !memcmp (wvc1.data, wvc2.data, wvc1.size));

    free (pcm);
    free (wv1.data); free (wvc1.data);
    free (wv2.data); free (wvc2.data);
    return 0;
}

static WavpackContext *open_reject_context (struct mem_sink *wv, struct mem_sink *wvc,
    int channels, int bps, int hybrid, int total, WavpackConfig *out_config)
{
    WavpackContext *enc;
    WavpackConfig config;

    memset (wv, 0, sizeof (*wv));
    memset (wvc, 0, sizeof (*wvc));
    enc = WavpackOpenFileOutput (sink_block, wv, wvc);
    memset (&config, 0, sizeof (config));
    config.bytes_per_sample = bps / 8;
    config.bits_per_sample = bps;
    config.num_channels = channels;
    config.channel_mask = channels == 1 ? 0x4 : (channels == 2 ? 0x3 : (1 << channels) - 1);
    config.sample_rate = 48000;
    config.block_samples = TEST_BLOCK_SAMPLES;
    config.bitrate = 3.0f;
    config.flags = hybrid ? (CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC | CONFIG_SHAPE_OVERRIDE) :
        CONFIG_HIGH_FLAG;

    if (!enc || !WavpackSetConfiguration64 (enc, &config, total, NULL)) {
        WavpackCloseFile (enc);
        return NULL;
    }

    *out_config = config;
    return enc;
}

static int case_compat_rejected_then_continue (void)
{
    WavpackConfig config;
    WavpackContext *enc;
    WavpackHybridBitratePoint good [2], bad [2];
    uint16_t good_b [2][2] = { { 1024, 1024 }, { 2048, 2048 } };
    uint16_t bad_budget [2] = { 100, 100 };
    uint16_t nine [9] = { 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024 };
    WavpackHybridBitratePoint nine_point = { 0, nine };
    struct mem_sink wv, wvc;
    int32_t *pcm;
    uint32_t offset;

    good [0].sample_index = 0;
    good [0].bits_per_sample_q8 = good_b [0];
    good [1].sample_index = 200;
    good [1].bits_per_sample_q8 = good_b [1];

    // call before configuration is rejected
    memset (&wv, 0, sizeof (wv));
    memset (&wvc, 0, sizeof (wvc));
    enc = WavpackOpenFileOutput (sink_block, &wv, &wvc);
    TASK_CHECK (enc);
    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, good, 2));
    TASK_CHECK (WavpackGetErrorMessage (enc) [0]);
    WavpackCloseFile (enc);

    // non-hybrid, 8-bit PCM, and >8 channels are rejected
    enc = open_reject_context (&wv, &wvc, 2, 16, 0, 500, &config);
    TASK_CHECK (enc && !WavpackSetHybridBitratePlan (enc, good, 2));
    WavpackCloseFile (enc);

    enc = open_reject_context (&wv, &wvc, 2, 8, 1, 500, &config);
    TASK_CHECK (enc && !WavpackSetHybridBitratePlan (enc, good, 2));
    WavpackCloseFile (enc);

    enc = open_reject_context (&wv, &wvc, 9, 16, 1, 500, &config);
    TASK_CHECK (enc && !WavpackSetHybridBitratePlan (enc, &nine_point, 1));
    WavpackCloseFile (enc);

    // malformed plans
    enc = open_reject_context (&wv, &wvc, 2, 16, 1, 500, &config);
    TASK_CHECK (enc);

    bad [0] = good [0];
    bad [1] = good [1];
    bad [0].sample_index = 10;
    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, bad, 2));       // first point not zero

    bad [0].sample_index = 0;
    bad [1].sample_index = 0;
    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, bad, 2));       // not strictly increasing

    bad [1].sample_index = 600;
    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, bad, 2));       // point beyond total

    bad [1].sample_index = 200;
    bad [1].bits_per_sample_q8 = bad_budget;
    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, bad, 2));       // budget below 576
    bad_budget [0] = bad_budget [1] = 7000;
    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, bad, 2));       // budget above 6144

    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, NULL, 2));      // NULL with nonzero count
    TASK_CHECK (WavpackGetErrorMessage (enc) [0]);

    // after PackInit the plan can no longer be changed
    TASK_CHECK (WavpackPackInit (enc));
    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, good, 2));
    WavpackCloseFile (enc);

    // a rejected plan leaves the context usable for ordinary hybrid encoding
    enc = open_reject_context (&wv, &wvc, 2, 16, 1, 500, &config);
    TASK_CHECK (enc);
    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, bad, 2));
    TASK_CHECK (WavpackPackInit (enc));
    pcm = make_pcm (500, 2, 16, 99);
    TASK_CHECK (pcm);

    for (offset = 0; offset < 500; offset += 64)
        TASK_CHECK (WavpackPackSamples (enc, pcm + offset * 2, 500 - offset > 64 ? 64 : 500 - offset));

    TASK_CHECK (WavpackFlushSamples (enc));
    WavpackCloseFile (enc);
    free (pcm);
    free (wv.data);
    free (wvc.data);

    // and a valid plan accepted right after a rejected one still works end to end
    memset (&wv, 0, sizeof (wv));
    memset (&wvc, 0, sizeof (wvc));
    {
        struct encode_args a;

        memset (&a, 0, sizeof (a));
        a.frames = 500;
        a.channels = 2;
        a.bps = 16;
        a.workers = 0;
        a.chunk = 64;
        a.total_known = 1;
        a.plan = bad;
        a.plan_count = 2;
        TASK_CHECK (!run_encode (&a, &wv, &wvc));                 // bad plan fails the encode setup
        a.plan = good;
        TASK_CHECK (run_encode (&a, &wv, &wvc));                  // valid plan succeeds
        pcm = make_pcm (500, 2, 16, 1925 + 2 * 7 + 16);
        TASK_CHECK (pcm);
        TASK_CHECK (decode_all (&wv, &wvc, 1, pcm, 500, 2, NULL));
        free (pcm);
        free (wv.data);
        free (wvc.data);
    }

    return 0;
}

static int case_compat_invalid_keeps_old_plan (void)
{
    WavpackConfig config;
    WavpackContext *enc;
    WavpackHybridBitratePoint good [1], bad [2];
    uint16_t good_b [2] = { 1024, 2048 };
    uint16_t bad_b [2] = { 100, 100 };
    struct mem_sink wv, wvc;
    struct block_info blocks [MAX_BLOCKS];
    struct group groups [MAX_BLOCKS];
    int nblocks, ngroups;
    int32_t *pcm;
    uint32_t offset;

    good [0].sample_index = 0;
    good [0].bits_per_sample_q8 = good_b;
    bad [0].sample_index = 0;
    bad [0].bits_per_sample_q8 = bad_b;
    bad [1].sample_index = 100;
    bad [1].bits_per_sample_q8 = bad_b;

    enc = open_reject_context (&wv, &wvc, 2, 16, 1, 500, &config);
    TASK_CHECK (enc);
    TASK_CHECK (WavpackSetHybridBitratePlan (enc, good, 1));
    TASK_CHECK (!WavpackSetHybridBitratePlan (enc, bad, 2));     // must not replace good plan
    TASK_CHECK (WavpackPackInit (enc));

    pcm = make_pcm (500, 2, 16, 777);
    TASK_CHECK (pcm);

    for (offset = 0; offset < 500; offset += 64)
        TASK_CHECK (WavpackPackSamples (enc, pcm + offset * 2, 500 - offset > 64 ? 64 : 500 - offset));

    TASK_CHECK (WavpackFlushSamples (enc));
    WavpackCloseFile (enc);

    nblocks = parse_blocks (wv.data, wv.size, blocks, MAX_BLOCKS);
    ngroups = parse_groups (blocks, nblocks, groups, MAX_BLOCKS);
    TASK_CHECK (ngroups >= 1);
    {
        int k;

        for (k = 0; k < ngroups; k++)
            TASK_CHECK (groups [k].index != 100);                // rejected plan's cut never appears
    }

    for (offset = 0; offset < (uint32_t) nblocks; offset++) {
        TASK_CHECK (blocks [offset].acc [0] == budget_to_acc (1024));
        TASK_CHECK (blocks [offset].acc [1] == budget_to_acc (2048));
    }

    TASK_CHECK (decode_all (&wv, &wvc, 1, pcm, 500, 2, NULL));

    free (pcm);
    free (wv.data);
    free (wvc.data);
    return 0;
}

////////////////////////////////// registration ////////////////////////////////

const struct task_case task_cases [] = {
    { "schedule", "basic_cuts",            case_schedule_basic_cuts },
    { "schedule", "big_multicut",          case_schedule_big_multicut },
    { "schedule", "six_channel_budgets",   case_schedule_six_channel },
    { "schedule", "unknown_length",        case_schedule_unknown_length },
    { "schedule", "short_tail",            case_schedule_short_tail },
    { "schedule", "dynamic_shaping",       case_schedule_dynamic_shaping },
    { "schedule", "split_vs_merged",       case_schedule_split_vs_merged },
    { "schedule", "workers",               case_schedule_workers },

    { "budgets", "mono_reference",         case_budget_mono_reference },
    { "budgets", "stereo_independent",     case_budget_stereo_independent },
    { "budgets", "entropy_effect",         case_budget_entropy_effect },
    { "budgets", "identical_stereo",       case_budget_identical_stereo },
    { "budgets", "twentyfour_bit",         case_budget_24bit },

    { "compatibility", "wvc_exact",        case_compat_wvc_exact },
    { "compatibility", "wv_only_lossy",    case_compat_lossy },
    { "compatibility", "clear_plan",       case_compat_clear_plan },
    { "compatibility", "rejected_continue",case_compat_rejected_then_continue },
    { "compatibility", "invalid_keeps_old",case_compat_invalid_keeps_old_plan },
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]);
