// seek_origin_test.c
//
// Independent C regression for WavPack sample-origin handling and seeking.
// It exercises three areas registered as the "origin", "lanes" and "restart"
// case groups:
//
//   origin  - re-headed mono/stereo materials whose 40-bit sample origins are
//             zero, small, straddle the 32-bit boundary and exceed it; seeks
//             cross block boundaries in a scrambled order.
//   lanes   - hybrid stereo material that genuinely needs the WVC correction
//             stream; WV and WVC are driven through independent memory
//             callbacks and position ledgers, and their distinct byte-level
//             search trajectories are checked against separate catalogs.
//   restart - decode-history recovery: backward and cross-block seeks, seeks
//             without intervening reads, end-of-stream short reads, instance
//             interleaving, and multi-substream preroll for 6-channel material.
//
// Expected PCM is always the exact pre-encode array.  Neither first-pass
// decoder output nor the library header macros are used as an answer.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "task_support/check.h"
#include "wavpack.h"
#include "seek_origin_catalog.h"
#include "seek_origin_materials.h"

#define SEEK_SENTINEL 0x5a5a5a5a

// Print the mandated first-difference line, then fail the enclosing check.
static int so_report_diff(const char *material, int64_t target, int channel,
                          int32_t actual, int32_t expected)
{
    printf("SEEK_PCM_DIFF material=%s target=%lld channel=%d actual=%d "
           "expected=%d\n", material, (long long) target, channel,
           actual, expected);
    fflush(stdout);
    return 0;
}

// Compare decoded interleaved PCM against the pre-encode reference.  On the
// first mismatch a SEEK_PCM_DIFF line is emitted before the failing check.
static int so_compare_pcm(const char *material, int64_t target,
                          const int32_t *got, uint32_t frames, int channels,
                          const int32_t *reference)
{
    for (uint32_t i = 0; i < frames; ++i)
        for (int c = 0; c < channels; ++c) {
            int32_t actual = got[i * channels + c];
            int32_t expected = reference[(target + i) * channels + c];

            if (actual != expected)
                TASK_CHECK(so_report_diff(material, target + i, c,
                                          actual, expected));
        }

    return 0;
}

// A guarded output buffer: guard words surround the decode area and must be
// untouched by every read.
struct so_guarded {
    int32_t *storage;
    int32_t *area;
    int      area_words;
};

static void so_guarded_init(struct so_guarded *g, int area_words)
{
    g->area_words = area_words;
    g->storage = (int32_t *) malloc(((size_t) area_words + 8) *
                                    sizeof(int32_t));
    g->area = g->storage + 4;
    memset(g->storage, 0, ((size_t) area_words + 8) * sizeof(int32_t));
}

static void so_guarded_arm(const struct so_guarded *g)
{
    for (int i = 0; i < 4; ++i) {
        g->storage[i] = SEEK_SENTINEL;
        g->area[g->area_words + i] = SEEK_SENTINEL;
    }
}

static int so_guarded_intact(const struct so_guarded *g)
{
    for (int i = 0; i < 4; ++i)
        if (g->storage[i] != SEEK_SENTINEL ||
            g->area[g->area_words + i] != SEEK_SENTINEL)
            return 0;

    return 1;
}

static void so_guarded_free(struct so_guarded *g)
{
    free(g->storage);
    g->storage = NULL;
    g->area = NULL;
}

//////////////////////////////////// origin ///////////////////////////////////

struct origin_config {
    int      channels;
    int64_t  shift;
    int      unknown_total;
    const char *tag;
};

static const struct origin_config origin_configs[] = {
    { 1, 0LL,                              0, "mono-zero-known"       },
    { 1, 7LL,                              0, "mono-small-known"      },
    { 1, (int64_t) 1 << 32,                0, "mono-boundary-known"   },
    { 1, ((int64_t) 1 << 33) + 123456789,  0, "mono-beyond-known"     },
    { 1, ((int64_t) 1 << 33) + 123456789,  1, "mono-beyond-unknown"   },
    { 2, 0LL,                              0, "stereo-zero-known"     },
    { 2, 7LL,                              0, "stereo-small-known"    },
    { 2, ((int64_t) 1 << 32) - 30,         0, "stereo-straddle-known" },
    { 2, ((int64_t) 1 << 33) + 123456789,  0, "stereo-beyond-known"   },
    { 2, ((int64_t) 1 << 33) + 123456789,  1, "stereo-beyond-unknown" },
};

#define ORIGIN_FRAMES 200
#define ORIGIN_BLOCK  64

static int origin_run(const struct origin_config *cfg)
{
    struct so_material m;
    WavpackStreamReader64 reader = so_stream_reader();
    struct pv_lane wv_lane;
    WavpackContext *dec;

    // Scrambled targets; several sit on or just past block boundaries
    // (boundaries are 0, 64, 128 and 164 for these materials).
    static const int64_t targets[] = { 130, 5, 96, 199, 64, 40, 164, 0 };
    static const uint32_t read_sizes[] = { 1, 7, 70 };

    TASK_CHECK(so_material_build(&m, cfg->tag, cfg->channels, 0,
                                 ORIGIN_BLOCK, ORIGIN_FRAMES,
                                 cfg->shift, cfg->unknown_total));

    // Independent catalog checks: raw 40-bit origin, frame total and the
    // relationship of every physical block to the playback-relative window.
    TASK_CHECK(m.wv_cat.origin == cfg->shift);
    TASK_CHECK(m.wv_cat.total_frames == ORIGIN_FRAMES);
    TASK_CHECK(!m.hybrid && m.wvc.size == 0);

    int initial_blocks = 0;

    for (size_t i = 0; i < m.wv_cat.count; ++i) {
        const struct pv_block *b = &m.wv_cat.items[i];

        TASK_CHECK(b->offset + b->length <= m.wv.size);
        TASK_CHECK(b->raw_index == cfg->shift + b->rel_start);
        TASK_CHECK(b->rel_start >= 0 && b->rel_end <= ORIGIN_FRAMES);

        if (b->flags & PV_FLAG_INITIAL) {
            initial_blocks++;
            TASK_CHECK(b->block_samples > 0 && b->block_samples <= ORIGIN_BLOCK);
            TASK_CHECK(b->total_stored ==
                       (cfg->unknown_total ? -1 : (int64_t) ORIGIN_FRAMES));
        }
    }

    TASK_CHECK(initial_blocks == 4);

    dec = so_open_decoder(&m, &reader, &wv_lane, NULL);
    TASK_CHECK(dec != NULL);
    TASK_CHECK(WavpackGetNumSamples64(dec) == ORIGIN_FRAMES);
    TASK_CHECK(WavpackGetNumChannels(dec) == cfg->channels);

    struct so_guarded guard;
    so_guarded_init(&guard, 70 * cfg->channels);

    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
        int64_t target = targets[t];
        const struct pv_block *expected_block =
            pv_catalog_find_initial(&m.wv_cat, target);
        uint32_t want = read_sizes[t % 3];
        uint32_t expected_frames;
        uint32_t got;

        TASK_CHECK(expected_block != NULL);
        TASK_CHECK(target >= expected_block->rel_start &&
                   target < expected_block->rel_end);

        TASK_CHECK(WavpackSeekSample64(dec, target) != 0);

        expected_frames = (uint32_t) (ORIGIN_FRAMES - target);
        if (want < expected_frames)
            expected_frames = want;

        so_guarded_arm(&guard);
        got = WavpackUnpackSamples(dec, guard.area, want);

        // Compare PCM first so any preroll/skip miscalculation is reported
        // through its SEEK_PCM_DIFF line before any positional assertion.
        TASK_CHECK(so_compare_pcm(m.name, target, guard.area, got,
                                  cfg->channels, m.pcm) == 0);
        TASK_CHECK(got == expected_frames);
        TASK_CHECK(WavpackGetSampleIndex64(dec) == target + (int64_t) got);
        TASK_CHECK(so_guarded_intact(&guard));
        TASK_CHECK(WavpackGetNumErrors(dec) == 0);
    }

    WavpackCloseFile(dec);
    pv_lane_free(&wv_lane);
    so_guarded_free(&guard);
    so_material_free(&m);
    return 0;
}

static int case_origin(size_t index)
{
    return origin_run(&origin_configs[index]);
}

//////////////////////////////////// lanes ////////////////////////////////////

#define LANES_FRAMES 300
#define LANES_BLOCK  64
#define LANES_TARGET 150       // inside the group starting at frame 128
#define LANES_READ   40

static int lanes_event_is_catalog_offset(const struct pv_catalog *cat,
                                         int64_t offset)
{
    for (size_t i = 0; i < cat->count; ++i)
        if ((int64_t) cat->items[i].offset == offset)
            return 1;

    return 0;
}

static int case_lanes_trace(void)
{
    struct so_material m;
    WavpackStreamReader64 reader = so_stream_reader();
    struct pv_lane wv_lane, wvc_lane;
    WavpackContext *dec;
    const struct pv_block *wv_target, *wvc_target;
    struct so_guarded guard;

    TASK_CHECK(so_material_build(&m, "hybrid-stereo-lanes", 2, 1, LANES_BLOCK,
                                 LANES_FRAMES, 12345, 0));
    TASK_CHECK(m.hybrid && m.wvc.size > 0);

    // The two lanes describe the same programme but occupy different byte
    // positions, which is the property this case depends on.
    TASK_CHECK(m.wv_cat.total_frames == LANES_FRAMES);
    TASK_CHECK(m.wvc_cat.total_frames == LANES_FRAMES);
    TASK_CHECK(m.wv_cat.origin == 12345 && m.wvc_cat.origin == 12345);

    wv_target = pv_catalog_find_initial(&m.wv_cat, LANES_TARGET);
    wvc_target = pv_catalog_find_initial(&m.wvc_cat, LANES_TARGET);
    TASK_CHECK(wv_target != NULL && wvc_target != NULL);
    TASK_CHECK(wv_target->rel_start == 128 && wvc_target->rel_start == 128);
    TASK_CHECK((int64_t) wv_target->offset != (int64_t) wvc_target->offset);

    dec = so_open_decoder(&m, &reader, &wv_lane, &wvc_lane);
    TASK_CHECK(dec != NULL);

    pv_lane_clear_trace(&wv_lane);
    pv_lane_clear_trace(&wvc_lane);

    TASK_CHECK(WavpackSeekSample64(dec, LANES_TARGET) != 0);
    TASK_CHECK(WavpackGetSampleIndex64(dec) == LANES_TARGET);

    // The header actually loaded in each lane must match that lane's own
    // catalog entry; the WV position must never be reused as the WVC one.
    int64_t wv_loaded = pv_lane_last_header_read(&wv_lane);
    int64_t wvc_loaded = pv_lane_last_header_read(&wvc_lane);

    TASK_CHECK(wv_loaded == (int64_t) wv_target->offset);
    TASK_CHECK(wvc_loaded == (int64_t) wvc_target->offset);
    TASK_CHECK(wv_loaded != wvc_loaded);
    TASK_CHECK(wvc_loaded != (int64_t) wv_target->offset);

    // Every header observed along each search trajectory must be a physical
    // block of that same lane.
    for (size_t i = 0; i < wv_lane.ntrace; ++i)
        if (wv_lane.trace[i].kind == 1)
            TASK_CHECK(lanes_event_is_catalog_offset(&m.wv_cat,
                                                      wv_lane.trace[i].at));

    for (size_t i = 0; i < wvc_lane.ntrace; ++i)
        if (wvc_lane.trace[i].kind == 1)
            TASK_CHECK(lanes_event_is_catalog_offset(&m.wvc_cat,
                                                      wvc_lane.trace[i].at));

    so_guarded_init(&guard, LANES_READ * 2);
    so_guarded_arm(&guard);
    uint32_t got = WavpackUnpackSamples(dec, guard.area, LANES_READ);
    TASK_CHECK(got == LANES_READ);
    TASK_CHECK(so_guarded_intact(&guard));
    TASK_CHECK(so_compare_pcm(m.name, LANES_TARGET, guard.area, got, 2,
                              m.pcm) == 0);
    TASK_CHECK(WavpackGetNumErrors(dec) == 0);

    WavpackCloseFile(dec);
    pv_lane_free(&wv_lane);
    pv_lane_free(&wvc_lane);
    so_guarded_free(&guard);
    so_material_free(&m);
    return 0;
}

static int case_lanes_lossy(void)
{
    struct so_material m;
    WavpackStreamReader64 reader = so_stream_reader();
    struct pv_lane wv_lane;
    WavpackContext *dec;
    struct so_guarded guard_lossless, guard_lossy;
    int differs = 0;

    TASK_CHECK(so_material_build(&m, "hybrid-stereo-lossy", 2, 1, LANES_BLOCK,
                                 LANES_FRAMES, 12345, 0));

    // Reference: with the correction stream the same target decodes exactly.
    {
        struct pv_lane wvc_lane;

        dec = so_open_decoder(&m, &reader, &wv_lane, &wvc_lane);
        TASK_CHECK(dec != NULL);
        TASK_CHECK(WavpackSeekSample64(dec, LANES_TARGET) != 0);
        so_guarded_init(&guard_lossless, LANES_READ * 2);
        so_guarded_arm(&guard_lossless);
        uint32_t got = WavpackUnpackSamples(dec, guard_lossless.area,
                                            LANES_READ);
        TASK_CHECK(got == LANES_READ);
        TASK_CHECK(so_compare_pcm(m.name, LANES_TARGET, guard_lossless.area,
                                  got, 2, m.pcm) == 0);
        TASK_CHECK(WavpackGetNumErrors(dec) == 0);
        WavpackCloseFile(dec);
        pv_lane_free(&wv_lane);
        pv_lane_free(&wvc_lane);
    }

    // Ignoring the WVC lane must still decode, but produce different PCM.
    // Open with only the WV lane (no correction stream attached).
    {
        char error[128];

        pv_lane_init(&wv_lane, "wv-lossy", m.wv.data, m.wv.size);
        dec = WavpackOpenFileInputEx64(&reader, &wv_lane, NULL, error, 0, 0);
    }
    TASK_CHECK(dec != NULL);
    TASK_CHECK(WavpackSeekSample64(dec, LANES_TARGET) != 0);

    so_guarded_init(&guard_lossy, LANES_READ * 2);
    so_guarded_arm(&guard_lossy);
    uint32_t got = WavpackUnpackSamples(dec, guard_lossy.area, LANES_READ);
    TASK_CHECK(got == LANES_READ);
    TASK_CHECK(so_guarded_intact(&guard_lossy));

    for (uint32_t i = 0; i < got * 2; ++i)
        if (guard_lossy.area[i] != guard_lossless.area[i])
            differs = 1;

    TASK_CHECK(differs);

    WavpackCloseFile(dec);
    pv_lane_free(&wv_lane);
    so_guarded_free(&guard_lossless);
    so_guarded_free(&guard_lossy);
    so_material_free(&m);
    return 0;
}

/////////////////////////////////// restart ///////////////////////////////////

#define RESTART_FRAMES 200
#define RESTART_BLOCK  64

// Drive one decoder through backward, cross-block and no-read seeks while a
// second independent decoder interleaves on the same material.
static int case_restart_stereo(void)
{
    struct so_material m;
    WavpackStreamReader64 reader = so_stream_reader();
    struct pv_lane lane_a, lane_b;
    WavpackContext *a, *b;
    struct so_guarded guard;
    static const uint32_t read_sizes[] = { 1, 7, 70 };
    uint32_t cycle = 0;

    // A non-zero origin makes preroll skip arithmetic meaningful here.
    TASK_CHECK(so_material_build(&m, "stereo-restart-origin7", 2, 0,
                                 RESTART_BLOCK, RESTART_FRAMES, 7, 0));

    a = so_open_decoder(&m, &reader, &lane_a, NULL);
    b = so_open_decoder(&m, &reader, &lane_b, NULL);
    TASK_CHECK(a != NULL && b != NULL);

    so_guarded_init(&guard, 70 * 2);

    // Sequential alternating reads, including a count that spans blocks.
    int64_t cursor = 0;
    while (cursor < RESTART_FRAMES) {
        uint32_t want = read_sizes[cycle++ % 3];
        uint32_t expect = (uint32_t) (RESTART_FRAMES - cursor);
        if (want < expect)
            expect = want;

        so_guarded_arm(&guard);
        uint32_t got = WavpackUnpackSamples(a, guard.area, want);
        TASK_CHECK(got == expect);
        TASK_CHECK(so_guarded_intact(&guard));
        TASK_CHECK(so_compare_pcm(m.name, cursor, guard.area, got, 2,
                                  m.pcm) == 0);
        cursor += got;
    }

    TASK_CHECK(WavpackGetSampleIndex64(a) == RESTART_FRAMES);
    TASK_CHECK(WavpackGetNumErrors(a) == 0);

    // Backward seek inside the same block, then a cross-block backward jump.
    TASK_CHECK(WavpackSeekSample64(a, 20) != 0);
    TASK_CHECK(WavpackGetSampleIndex64(a) == 20);
    so_guarded_arm(&guard);
    TASK_CHECK(WavpackUnpackSamples(a, guard.area, 7) == 7);
    TASK_CHECK(so_compare_pcm(m.name, 20, guard.area, 7, 2, m.pcm) == 0);

    TASK_CHECK(WavpackSeekSample64(a, 130) != 0);
    so_guarded_arm(&guard);
    TASK_CHECK(WavpackUnpackSamples(a, guard.area, 7) == 7);
    TASK_CHECK(so_compare_pcm(m.name, 130, guard.area, 7, 2, m.pcm) == 0);

    // Instance B works a different region while A is parked mid-stream.
    TASK_CHECK(WavpackSeekSample64(b, 96) != 0);
    so_guarded_arm(&guard);
    TASK_CHECK(WavpackUnpackSamples(b, guard.area, 7) == 7);
    TASK_CHECK(so_compare_pcm(m.name, 96, guard.area, 7, 2, m.pcm) == 0);
    TASK_CHECK(WavpackGetSampleIndex64(b) == 103);

    // A's history must be untouched by B's activity.
    TASK_CHECK(WavpackGetSampleIndex64(a) == 137);
    so_guarded_arm(&guard);
    TASK_CHECK(WavpackUnpackSamples(a, guard.area, 7) == 7);
    TASK_CHECK(so_compare_pcm(m.name, 137, guard.area, 7, 2, m.pcm) == 0);

    // Consecutive seeks with no read between them; only the last position
    // matters.
    TASK_CHECK(WavpackSeekSample64(a, 96) != 0);
    TASK_CHECK(WavpackSeekSample64(a, 170) != 0);
    TASK_CHECK(WavpackGetSampleIndex64(a) == 170);
    so_guarded_arm(&guard);
    TASK_CHECK(WavpackUnpackSamples(a, guard.area, 7) == 7);
    TASK_CHECK(so_compare_pcm(m.name, 170, guard.area, 7, 2, m.pcm) == 0);

    // Short read at the end of the stream, then a restart from the beginning.
    TASK_CHECK(WavpackSeekSample64(a, 195) != 0);
    so_guarded_arm(&guard);
    uint32_t tail = WavpackUnpackSamples(a, guard.area, 70);
    TASK_CHECK(tail == 5);
    TASK_CHECK(so_guarded_intact(&guard));
    TASK_CHECK(so_compare_pcm(m.name, 195, guard.area, 5, 2, m.pcm) == 0);
    TASK_CHECK(WavpackUnpackSamples(a, guard.area, 70) == 0);  // EOF by reading

    TASK_CHECK(WavpackSeekSample64(a, 0) != 0);
    TASK_CHECK(WavpackGetSampleIndex64(a) == 0);
    so_guarded_arm(&guard);
    TASK_CHECK(WavpackUnpackSamples(a, guard.area, 7) == 7);
    TASK_CHECK(so_compare_pcm(m.name, 0, guard.area, 7, 2, m.pcm) == 0);

    // EOF is expressed by reading to the end, never by seeking to the total.
    TASK_CHECK(WavpackSeekSample64(b, 100) != 0);
    int64_t b_cursor = 100;
    while (1) {
        uint32_t want = read_sizes[cycle++ % 3];
        uint32_t got = WavpackUnpackSamples(b, guard.area, want);

        if (!got)
            break;

        TASK_CHECK(so_compare_pcm(m.name, b_cursor, guard.area, got, 2,
                                  m.pcm) == 0);
        b_cursor += got;
    }
    TASK_CHECK(b_cursor == RESTART_FRAMES);
    TASK_CHECK(WavpackGetSampleIndex64(b) == RESTART_FRAMES);

    TASK_CHECK(WavpackGetNumErrors(a) == 0 && WavpackGetNumErrors(b) == 0);

    WavpackCloseFile(a);
    WavpackCloseFile(b);
    pv_lane_free(&lane_a);
    pv_lane_free(&lane_b);
    so_guarded_free(&guard);
    so_material_free(&m);
    return 0;
}

// Six-channel zero-origin material: every segment has multiple substreams.
// Seeking into a non-first segment must preroll and recover all of them, not
// just the leading substream.
static int case_restart_multistream(void)
{
    struct so_material m;
    WavpackStreamReader64 reader = so_stream_reader();
    struct pv_lane lane;
    WavpackContext *dec;
    struct so_guarded guard;

    enum { MS_FRAMES = 257, MS_CHANNELS = 6, MS_BLOCK = 64 };
    static const int64_t targets[] = { 33, 70, 200, 256 };

    TASK_CHECK(so_material_build(&m, "sixch-zero-preroll", MS_CHANNELS, 0,
                                 MS_BLOCK, MS_FRAMES, 0, 0));

    // Confirm the material really is multi-substream (more than one physical
    // block per multichannel segment).
    int groups = 0, max_substreams = 0, run = 0;

    for (size_t i = 0; i < m.wv_cat.count; ++i) {
        if (m.wv_cat.items[i].flags & PV_FLAG_INITIAL) {
            groups++;
            if (run > max_substreams)
                max_substreams = run;
            run = 1;
        }
        else
            run++;
    }

    if (run > max_substreams)
        max_substreams = run;

    TASK_CHECK(groups == 5);
    TASK_CHECK(max_substreams >= 2);   // genuinely multi-substream segments

    dec = so_open_decoder(&m, &reader, &lane, NULL);
    TASK_CHECK(dec != NULL);
    so_guarded_init(&guard, MS_FRAMES * MS_CHANNELS);

    for (size_t t = 0; t < sizeof(targets) / sizeof(targets[0]); ++t) {
        int64_t target = targets[t];
        uint32_t want = 37;
        uint32_t expect = (uint32_t) (MS_FRAMES - target);

        if (want < expect)
            expect = want;

        const struct pv_block *blk =
            pv_catalog_find_initial(&m.wv_cat, target);
        TASK_CHECK(blk != NULL);

        TASK_CHECK(WavpackSeekSample64(dec, target) != 0);
        TASK_CHECK(WavpackGetSampleIndex64(dec) == target);

        so_guarded_arm(&guard);
        uint32_t got = WavpackUnpackSamples(dec, guard.area, want);
        TASK_CHECK(got == expect);
        TASK_CHECK(so_guarded_intact(&guard));
        TASK_CHECK(so_compare_pcm(m.name, target, guard.area, got,
                                  MS_CHANNELS, m.pcm) == 0);
        TASK_CHECK(WavpackGetNumErrors(dec) == 0);
    }

    WavpackCloseFile(dec);
    pv_lane_free(&lane);
    so_guarded_free(&guard);
    so_material_free(&m);
    return 0;
}

////////////////////////////////// registration ///////////////////////////////

static int run_origin_0(void) { return case_origin(0); }
static int run_origin_1(void) { return case_origin(1); }
static int run_origin_2(void) { return case_origin(2); }
static int run_origin_3(void) { return case_origin(3); }
static int run_origin_4(void) { return case_origin(4); }
static int run_origin_5(void) { return case_origin(5); }
static int run_origin_6(void) { return case_origin(6); }
static int run_origin_7(void) { return case_origin(7); }
static int run_origin_8(void) { return case_origin(8); }
static int run_origin_9(void) { return case_origin(9); }

const struct task_case task_cases[] = {
    { "origin",  "mono-zero-known",       run_origin_0 },
    { "origin",  "mono-small-known",      run_origin_1 },
    { "origin",  "mono-boundary-known",   run_origin_2 },
    { "origin",  "mono-beyond-known",     run_origin_3 },
    { "origin",  "mono-beyond-unknown",   run_origin_4 },
    { "origin",  "stereo-zero-known",     run_origin_5 },
    { "origin",  "stereo-small-known",    run_origin_6 },
    { "origin",  "stereo-straddle-known", run_origin_7 },
    { "origin",  "stereo-beyond-known",   run_origin_8 },
    { "origin",  "stereo-beyond-unknown", run_origin_9 },
    { "lanes",   "hybrid-trace",          case_lanes_trace },
    { "lanes",   "hybrid-lossy-differs",  case_lanes_lossy },
    { "restart", "stereo-history",        case_restart_stereo },
    { "restart", "sixch-substream-preroll", case_restart_multistream },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
