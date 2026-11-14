// dsd_pcm_regression.c
//
// Regression tests for buffer-size-independent DSD -> PCM decoding and seeking.
//
// The compatibility reference is the PCM produced by reading the file from the
// beginning in a single WavpackUnpackSamples() call. For the primary test file
// that PCM is pinned against an independent reference captured from the
// unmodified upstream library (task_support/reference.h); every chunked read
// pattern and every seek-and-read sequence must reproduce it sample for sample.

#include "task_support/check.h"
#include "wavpack.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REF_FRAMES 257
#define REF_CH 2

#define N REF_FRAMES
#define CH REF_CH
#include "task_support/reference.h"
#undef N
#undef CH

#define MAIN_FILE   ".task-build/dsd_pcm_regression_main.wv"
#define SHORT10_FILE ".task-build/dsd_pcm_regression_short10.wv"
#define SHORT20_FILE ".task-build/dsd_pcm_regression_short20.wv"
#define MONO_FILE   ".task-build/dsd_pcm_regression_mono.wv"
#define QUAD_FILE   ".task-build/dsd_pcm_regression_quad.wv"
#define PCM_FILE    ".task-build/dsd_pcm_regression_pcm16.wv"
#define FLOAT_FILE  ".task-build/dsd_pcm_regression_float.wv"

#define SHORT10_FRAMES 10
#define SHORT20_FRAMES 20
#define MONO_FRAMES 100
#define QUAD_FRAMES 100
#define PCM_FRAMES 200

///////////////////////////// deterministic source material ////////////////////////////////

static uint32_t rng_state;

static void rng_seed (uint32_t seed) { rng_state = seed; }

static uint32_t rng_next (void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static int32_t main_source [REF_FRAMES * REF_CH];
static int32_t short10_source [SHORT10_FRAMES * 2];
static int32_t short20_source [SHORT20_FRAMES * 2];
static int32_t mono_source [MONO_FRAMES];
static int32_t quad_source [QUAD_FRAMES * 4];
static int32_t pcm_source [PCM_FRAMES * 2];
static int32_t float_source [PCM_FRAMES * 2];

static void generate_sources (void)
{
    int i;

    // this must match task_support/baseline.c exactly (same stream the pinned
    // reference PCM in task_support/reference.h was captured from)

    rng_seed (0x12345678u);

    for (i = 0; i < REF_FRAMES * REF_CH; ++i)
        main_source [i] = (rng_next () >> 24) & 0xff;

    rng_seed (0x87654321u);

    for (i = 0; i < SHORT10_FRAMES * 2; ++i)
        short10_source [i] = (rng_next () >> 24) & 0xff;

    rng_seed (0x0badf00du);

    for (i = 0; i < SHORT20_FRAMES * 2; ++i)
        short20_source [i] = (rng_next () >> 24) & 0xff;

    rng_seed (0x10203040u);

    for (i = 0; i < MONO_FRAMES; ++i)
        mono_source [i] = (rng_next () >> 24) & 0xff;

    rng_seed (0x55667788u);

    for (i = 0; i < QUAD_FRAMES * 4; ++i)
        quad_source [i] = (rng_next () >> 24) & 0xff;

    rng_seed (0x99aabbccu);

    for (i = 0; i < PCM_FRAMES * 2; ++i)
        pcm_source [i] = (int32_t) (int16_t) (rng_next () >> 8);

    rng_seed (0xddeeff00u);

    for (i = 0; i < PCM_FRAMES * 2; ++i)
        float_source [i] = (int32_t) (0x3f000000u | (rng_next () & 0x007fffffu));
}

///////////////////////////// file creation helpers ////////////////////////////////

static int block_output (void *id, void *data, int32_t n)
{
    return fwrite (data, 1, n, (FILE *) id) == (size_t) n;
}

static int write_file (const char *path, const int32_t *source, int frames, WavpackConfig *cfg)
{
    WavpackContext *w;
    FILE *f;
    int ok;

    f = fopen (path, "wb");

    if (!f)
        return 0;

    w = WavpackOpenFileOutput (block_output, f, NULL);
    ok = w && WavpackSetConfiguration64 (w, cfg, frames, NULL) && WavpackPackInit (w) &&
        WavpackPackSamples (w, (int32_t *) source, frames) && WavpackFlushSamples (w);

    if (w)
        WavpackCloseFile (w);

    fclose (f);
    return ok;
}

static int write_dsd_file (const char *path, const int32_t *source, int frames, int channels, int mask)
{
    WavpackConfig cfg;

    memset (&cfg, 0, sizeof (cfg));
    cfg.num_channels = channels;
    cfg.channel_mask = mask;
    cfg.sample_rate = 352800;
    cfg.bytes_per_sample = 1;
    cfg.bits_per_sample = 8;
    cfg.qmode = QMODE_DSD_MSB_FIRST;
    cfg.block_samples = 64;

    return write_file (path, source, frames, &cfg);
}

static int files_ready;

static int make_files (void)
{
    WavpackConfig cfg;

    if (files_ready)
        return 1;

    generate_sources ();

    if (!write_dsd_file (MAIN_FILE, main_source, REF_FRAMES, REF_CH, 0x3))
        return 0;

    if (!write_dsd_file (SHORT10_FILE, short10_source, SHORT10_FRAMES, 2, 0x3))
        return 0;

    if (!write_dsd_file (SHORT20_FILE, short20_source, SHORT20_FRAMES, 2, 0x3))
        return 0;

    if (!write_dsd_file (MONO_FILE, mono_source, MONO_FRAMES, 1, 0x4))
        return 0;

    if (!write_dsd_file (QUAD_FILE, quad_source, QUAD_FRAMES, 4, 0x33))
        return 0;

    memset (&cfg, 0, sizeof (cfg));
    cfg.num_channels = 2;
    cfg.channel_mask = 0x3;
    cfg.sample_rate = 44100;
    cfg.bytes_per_sample = 2;
    cfg.bits_per_sample = 16;
    cfg.block_samples = 64;

    if (!write_file (PCM_FILE, pcm_source, PCM_FRAMES, &cfg))
        return 0;

    cfg.bytes_per_sample = 4;
    cfg.bits_per_sample = 32;
    cfg.float_norm_exp = 127;

    if (!write_file (FLOAT_FILE, float_source, PCM_FRAMES, &cfg))
        return 0;

    files_ready = 1;
    return 1;
}

///////////////////////////// reading helpers ////////////////////////////////

static WavpackContext *open_audio (const char *path, int flags)
{
    char error [128];
    WavpackContext *w = WavpackOpenFileInput (path, error, flags, 0);

    if (!w)
        fprintf (stderr, "open %s: %s\n", path, error);

    return w;
}

static int out_channels (WavpackContext *w)
{
    return WavpackGetReducedChannels (w);
}

// Read up to "frames" frames using the cycling list of chunk sizes, returning
// the number of frames actually read (short only at end of file).

static uint32_t read_chunked (WavpackContext *w, int32_t *dst, uint32_t frames, const uint32_t *chunks, int num_chunks)
{
    int ch = out_channels (w);
    uint32_t got = 0;
    int ci = 0;

    while (got < frames) {
        uint32_t want = chunks [ci++ % num_chunks], n;

        if (want > frames - got)
            want = frames - got;

        if (!(n = WavpackUnpackSamples (w, dst + (size_t) got * ch, want)))
            break;

        got += n;
    }

    return got;
}

///////////////////////////// chunking group ////////////////////////////////

static int chunk_whole_matches_reference (void)
{
    static int32_t buf [REF_FRAMES * REF_CH];
    WavpackContext *w;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (WavpackGetNumSamples64 (w) == REF_FRAMES);
    TASK_CHECK (WavpackGetNumChannels (w) == REF_CH);
    TASK_CHECK (WavpackGetSampleIndex64 (w) == 0);
    TASK_CHECK (WavpackUnpackSamples (w, buf, REF_FRAMES) == REF_FRAMES);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference, sizeof (buf)) == 0);
    TASK_CHECK (WavpackGetSampleIndex64 (w) == REF_FRAMES);
    TASK_CHECK (WavpackUnpackSamples (w, buf, 1) == 0);     // clean EOF, no error
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int chunk_single_frame_reads (void)
{
    static int32_t buf [REF_FRAMES * REF_CH];
    WavpackContext *w;
    int i;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_AS_PCM)) != NULL);

    for (i = 0; i < REF_FRAMES; ++i) {
        TASK_CHECK (WavpackUnpackSamples (w, buf + i * REF_CH, 1) == 1);
        TASK_CHECK (WavpackGetSampleIndex64 (w) == i + 1);
    }

    TASK_CHECK (memcmp (buf, dsd_pcm_reference, sizeof (buf)) == 0);
    TASK_CHECK (WavpackUnpackSamples (w, buf, 1) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int chunk_varied_sizes_with_zero_reads (void)
{
    static const uint32_t chunks [] = { 1, 2, 3, 5, 8, 13, 21, 64, 100, 7 };
    static int32_t buf [REF_FRAMES * REF_CH];
    int32_t tmp [REF_CH];
    WavpackContext *w;
    uint32_t got = 0, want;
    int ci = 0;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_AS_PCM)) != NULL);

    while (got < REF_FRAMES) {
        // a zero-frame read must not consume audio or disturb the position
        TASK_CHECK (WavpackUnpackSamples (w, tmp, 0) == 0);
        TASK_CHECK (WavpackGetSampleIndex64 (w) == (int64_t) got);

        want = chunks [ci++ % (sizeof (chunks) / sizeof (chunks [0]))];

        if (want > REF_FRAMES - got)
            want = REF_FRAMES - got;

        TASK_CHECK (WavpackUnpackSamples (w, buf + (size_t) got * REF_CH, want) == want);
        got += want;
    }

    TASK_CHECK (WavpackGetSampleIndex64 (w) == REF_FRAMES);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference, sizeof (buf)) == 0);
    TASK_CHECK (WavpackUnpackSamples (w, tmp, 1) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int chunk_block_boundary_straddle (void)
{
    static const uint32_t chunks [] = { 63, 65, 129, 1, 31, 64 };
    static int32_t buf [REF_FRAMES * REF_CH];
    WavpackContext *w;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (read_chunked (w, buf, REF_FRAMES, chunks, sizeof (chunks) / sizeof (chunks [0])) == REF_FRAMES);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference, sizeof (buf)) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

///////////////////////////// seeking group ////////////////////////////////

static int seek_first_frames (void)
{
    static const int targets [] = { 0, 5, 1, 6, 2, 7, 3, 8, 4, 15, 16, 17 };
    int32_t buf [40 * REF_CH];
    WavpackContext *w;
    uint32_t expect;
    size_t t;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_AS_PCM)) != NULL);

    for (t = 0; t < sizeof (targets) / sizeof (targets [0]); ++t) {
        int target = targets [t];

        expect = 32;

        if ((int64_t) target + expect > REF_FRAMES)
            expect = REF_FRAMES - target;

        TASK_CHECK (WavpackSeekSample64 (w, target));
        TASK_CHECK (WavpackGetSampleIndex64 (w) == target);
        TASK_CHECK (WavpackUnpackSamples (w, buf, expect) == expect);
        TASK_CHECK (memcmp (buf, dsd_pcm_reference + (size_t) target * REF_CH, expect * REF_CH * sizeof (int32_t)) == 0);
        TASK_CHECK (WavpackGetSampleIndex64 (w) == (int64_t) target + expect);
    }

    // seek almost to the end and read through a clean EOF

    TASK_CHECK (WavpackSeekSample64 (w, REF_FRAMES - 3));
    TASK_CHECK (WavpackUnpackSamples (w, buf, 3) == 3);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference + (REF_FRAMES - 3) * REF_CH, 3 * REF_CH * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackUnpackSamples (w, buf, 1) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int seek_block_boundaries (void)
{
    static const int targets [] = { 31, 32, 33, 63, 64, 65, 95, 96, 127, 128, 129, 191, 192, 193, 255, 256 };
    static const uint32_t seven [] = { 7 };
    static int32_t buf [REF_FRAMES * REF_CH];
    WavpackContext *w;
    uint32_t remaining;
    size_t t;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_AS_PCM)) != NULL);

    for (t = 0; t < sizeof (targets) / sizeof (targets [0]); ++t) {
        int target = targets [t];

        remaining = REF_FRAMES - target;
        TASK_CHECK (WavpackSeekSample64 (w, target));
        TASK_CHECK (WavpackGetSampleIndex64 (w) == target);
        TASK_CHECK (read_chunked (w, buf, remaining, seven, 1) == remaining);
        TASK_CHECK (memcmp (buf, dsd_pcm_reference + (size_t) target * REF_CH, (size_t) remaining * REF_CH * sizeof (int32_t)) == 0);
        TASK_CHECK (WavpackGetSampleIndex64 (w) == REF_FRAMES);
    }

    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int seek_read_interleaved (void)
{
    static const uint32_t three [] = { 3 };
    static int32_t buf [REF_FRAMES * REF_CH];
    WavpackContext *w;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_AS_PCM)) != NULL);

    // partial read, then seek away: no leftover audio may leak past the seek
    TASK_CHECK (read_chunked (w, buf, 10, three, 1) == 10);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference, 10 * REF_CH * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackGetSampleIndex64 (w) == 10);

    TASK_CHECK (WavpackSeekSample64 (w, 200));
    TASK_CHECK (WavpackGetSampleIndex64 (w) == 200);
    TASK_CHECK (WavpackUnpackSamples (w, buf, 20) == 20);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference + 200 * REF_CH, 20 * REF_CH * sizeof (int32_t)) == 0);

    // backward seek into the extrapolated first frames
    TASK_CHECK (WavpackSeekSample64 (w, 3));
    TASK_CHECK (WavpackUnpackSamples (w, buf, 10) == 10);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference + 3 * REF_CH, 10 * REF_CH * sizeof (int32_t)) == 0);

    // seek to a block boundary and read through the end of the file
    TASK_CHECK (WavpackSeekSample64 (w, 128));
    TASK_CHECK (read_chunked (w, buf, REF_FRAMES - 128, three, 1) == REF_FRAMES - 128);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference + 128 * REF_CH, (REF_FRAMES - 128) * REF_CH * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackUnpackSamples (w, buf, 1) == 0);

    // repeated seek to the same position without an intervening read
    TASK_CHECK (WavpackSeekSample64 (w, 64));
    TASK_CHECK (WavpackSeekSample64 (w, 64));
    TASK_CHECK (WavpackGetSampleIndex64 (w) == 64);
    TASK_CHECK (WavpackUnpackSamples (w, buf, 4) == 4);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference + 64 * REF_CH, 4 * REF_CH * sizeof (int32_t)) == 0);

    // seek back to the very beginning
    TASK_CHECK (WavpackSeekSample64 (w, 0));
    TASK_CHECK (WavpackUnpackSamples (w, buf, 6) == 6);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference, 6 * REF_CH * sizeof (int32_t)) == 0);

    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int seek_invalid_positions (void)
{
    int32_t buf [8 * REF_CH];
    WavpackContext *w;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_AS_PCM)) != NULL);

    TASK_CHECK (WavpackUnpackSamples (w, buf, 5) == 5);
    TASK_CHECK (WavpackGetSampleIndex64 (w) == 5);

    // negative and past-the-end positions are rejected without disturbing anything
    TASK_CHECK (!WavpackSeekSample64 (w, -1));
    TASK_CHECK (WavpackGetSampleIndex64 (w) == 5);
    TASK_CHECK (WavpackUnpackSamples (w, buf, 3) == 3);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference + 5 * REF_CH, 3 * REF_CH * sizeof (int32_t)) == 0);

    TASK_CHECK (!WavpackSeekSample64 (w, REF_FRAMES));
    TASK_CHECK (!WavpackSeekSample64 (w, 1000000));
    TASK_CHECK (WavpackGetSampleIndex64 (w) == 8);
    TASK_CHECK (WavpackUnpackSamples (w, buf, 3) == 3);
    TASK_CHECK (memcmp (buf, dsd_pcm_reference + 8 * REF_CH, 3 * REF_CH * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackGetSampleIndex64 (w) == 11);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

///////////////////////////// compatibility group ////////////////////////////////

static int compat_native_mode (void)
{
    static int32_t buf [REF_FRAMES * REF_CH];
    static const uint32_t five [] = { 5 };
    WavpackContext *w;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_NATIVE)) != NULL);
    TASK_CHECK (WavpackUnpackSamples (w, buf, REF_FRAMES) == REF_FRAMES);
    TASK_CHECK (memcmp (buf, main_source, sizeof (main_source)) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);

    TASK_CHECK ((w = open_audio (MAIN_FILE, OPEN_DSD_NATIVE)) != NULL);
    TASK_CHECK (read_chunked (w, buf, REF_FRAMES, five, 1) == REF_FRAMES);
    TASK_CHECK (memcmp (buf, main_source, sizeof (main_source)) == 0);
    TASK_CHECK (WavpackGetSampleIndex64 (w) == REF_FRAMES);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int check_short_file (const char *path, int frames)
{
    static int32_t whole [SHORT20_FRAMES * 2], buf [SHORT20_FRAMES * 2];
    static const uint32_t one [] = { 1 }, three [] = { 3 };
    WavpackContext *w;
    int target = frames / 4, count = frames / 2, i;

    // the whole-file read is the compatibility reference for this file
    TASK_CHECK ((w = open_audio (path, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (WavpackGetNumSamples64 (w) == frames);
    TASK_CHECK (WavpackUnpackSamples (w, whole, frames) == (uint32_t) frames);
    TASK_CHECK (WavpackUnpackSamples (w, buf, 1) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);

    // any chunking reproduces it exactly
    TASK_CHECK ((w = open_audio (path, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (read_chunked (w, buf, frames, one, 1) == (uint32_t) frames);
    TASK_CHECK (memcmp (buf, whole, (size_t) frames * 2 * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackGetSampleIndex64 (w) == frames);
    WavpackCloseFile (w);

    TASK_CHECK ((w = open_audio (path, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (read_chunked (w, buf, frames, three, 1) == (uint32_t) frames);
    TASK_CHECK (memcmp (buf, whole, (size_t) frames * 2 * sizeof (int32_t)) == 0);
    WavpackCloseFile (w);

    // and so does seeking into the file
    TASK_CHECK ((w = open_audio (path, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (WavpackSeekSample64 (w, target));
    TASK_CHECK (WavpackGetSampleIndex64 (w) == target);
    TASK_CHECK (WavpackUnpackSamples (w, buf, count) == (uint32_t) count);
    for (i = 0; i < count * 2; ++i)
        TASK_CHECK (buf [i] == whole [target * 2 + i]);
    TASK_CHECK (WavpackSeekSample64 (w, 0));
    TASK_CHECK (WavpackUnpackSamples (w, buf, frames) == (uint32_t) frames);
    TASK_CHECK (memcmp (buf, whole, (size_t) frames * 2 * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int compat_short_files (void)
{
    TASK_CHECK (make_files ());
    TASK_CHECK (check_short_file (SHORT10_FILE, SHORT10_FRAMES) == 0);
    TASK_CHECK (check_short_file (SHORT20_FILE, SHORT20_FRAMES) == 0);
    return 0;
}

static int compat_mono (void)
{
    static int32_t whole [MONO_FRAMES], buf [MONO_FRAMES];
    static const uint32_t one [] = { 1 };
    WavpackContext *w;
    int i;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (MONO_FILE, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (WavpackGetNumChannels (w) == 1);
    TASK_CHECK (WavpackUnpackSamples (w, whole, MONO_FRAMES) == MONO_FRAMES);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);

    TASK_CHECK ((w = open_audio (MONO_FILE, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (read_chunked (w, buf, MONO_FRAMES, one, 1) == MONO_FRAMES);
    TASK_CHECK (memcmp (buf, whole, sizeof (whole)) == 0);

    TASK_CHECK (WavpackSeekSample64 (w, 4));
    TASK_CHECK (WavpackUnpackSamples (w, buf, 10) == 10);
    for (i = 0; i < 10; ++i)
        TASK_CHECK (buf [i] == whole [4 + i]);

    TASK_CHECK (WavpackSeekSample64 (w, 50));
    TASK_CHECK (read_chunked (w, buf, MONO_FRAMES - 50, one, 1) == MONO_FRAMES - 50);
    TASK_CHECK (memcmp (buf, whole + 50, (MONO_FRAMES - 50) * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int compat_multichannel_2ch_max (void)
{
    static int32_t quad [QUAD_FRAMES * 4], stereo [QUAD_FRAMES * 2], buf [QUAD_FRAMES * 4];
    static const uint32_t one [] = { 1 }, three [] = { 3 };
    WavpackContext *w;
    int f;

    TASK_CHECK (make_files ());
    TASK_CHECK ((w = open_audio (QUAD_FILE, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (WavpackGetNumChannels (w) == 4);
    TASK_CHECK (WavpackUnpackSamples (w, quad, QUAD_FRAMES) == QUAD_FRAMES);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);

    TASK_CHECK ((w = open_audio (QUAD_FILE, OPEN_DSD_AS_PCM)) != NULL);
    TASK_CHECK (read_chunked (w, buf, QUAD_FRAMES, three, 1) == QUAD_FRAMES);
    TASK_CHECK (memcmp (buf, quad, sizeof (quad)) == 0);
    WavpackCloseFile (w);

    // OPEN_2CH_MAX reduces to the front left/right channels
    TASK_CHECK ((w = open_audio (QUAD_FILE, OPEN_DSD_AS_PCM | OPEN_2CH_MAX)) != NULL);
    TASK_CHECK (WavpackGetReducedChannels (w) == 2);
    TASK_CHECK (WavpackUnpackSamples (w, stereo, QUAD_FRAMES) == QUAD_FRAMES);

    for (f = 0; f < QUAD_FRAMES; ++f) {
        TASK_CHECK (stereo [f * 2] == quad [f * 4]);
        TASK_CHECK (stereo [f * 2 + 1] == quad [f * 4 + 1]);
    }

    WavpackCloseFile (w);

    // and the reduced output is also chunking and seeking invariant
    TASK_CHECK ((w = open_audio (QUAD_FILE, OPEN_DSD_AS_PCM | OPEN_2CH_MAX)) != NULL);
    TASK_CHECK (read_chunked (w, buf, QUAD_FRAMES, one, 1) == QUAD_FRAMES);
    TASK_CHECK (memcmp (buf, stereo, sizeof (stereo)) == 0);

    TASK_CHECK (WavpackSeekSample64 (w, 10));
    TASK_CHECK (WavpackUnpackSamples (w, buf, 20) == 20);
    TASK_CHECK (memcmp (buf, stereo + 10 * 2, 20 * 2 * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

static int compat_pcm_and_float (void)
{
    static int32_t buf [PCM_FRAMES * 2];
    static const uint32_t seven [] = { 7 }, nine [] = { 9 };
    WavpackContext *w;

    TASK_CHECK (make_files ());

    // ordinary integer PCM is not affected by the DSD delivery path
    TASK_CHECK ((w = open_audio (PCM_FILE, 0)) != NULL);
    TASK_CHECK (WavpackUnpackSamples (w, buf, PCM_FRAMES) == PCM_FRAMES);
    TASK_CHECK (memcmp (buf, pcm_source, sizeof (pcm_source)) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);

    TASK_CHECK ((w = open_audio (PCM_FILE, 0)) != NULL);
    TASK_CHECK (read_chunked (w, buf, PCM_FRAMES, seven, 1) == PCM_FRAMES);
    TASK_CHECK (memcmp (buf, pcm_source, sizeof (pcm_source)) == 0);
    TASK_CHECK (WavpackSeekSample64 (w, 64));
    TASK_CHECK (WavpackUnpackSamples (w, buf, 100) == 100);
    TASK_CHECK (memcmp (buf, pcm_source + 64 * 2, 100 * 2 * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);

    // and neither is floating-point PCM
    TASK_CHECK ((w = open_audio (FLOAT_FILE, 0)) != NULL);
    TASK_CHECK (WavpackUnpackSamples (w, buf, PCM_FRAMES) == PCM_FRAMES);
    TASK_CHECK (memcmp (buf, float_source, sizeof (float_source)) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);

    TASK_CHECK ((w = open_audio (FLOAT_FILE, 0)) != NULL);
    TASK_CHECK (read_chunked (w, buf, PCM_FRAMES, nine, 1) == PCM_FRAMES);
    TASK_CHECK (memcmp (buf, float_source, sizeof (float_source)) == 0);
    TASK_CHECK (WavpackSeekSample64 (w, 13));
    TASK_CHECK (WavpackUnpackSamples (w, buf, 50) == 50);
    TASK_CHECK (memcmp (buf, float_source + 13 * 2, 50 * 2 * sizeof (int32_t)) == 0);
    TASK_CHECK (WavpackGetNumErrors (w) == 0);
    WavpackCloseFile (w);
    return 0;
}

///////////////////////////// case registry ////////////////////////////////

const struct task_case task_cases [] = {
    { "chunking", "whole_read_matches_reference", chunk_whole_matches_reference },
    { "chunking", "single_frame_reads", chunk_single_frame_reads },
    { "chunking", "varied_sizes_with_zero_reads", chunk_varied_sizes_with_zero_reads },
    { "chunking", "block_boundary_straddle", chunk_block_boundary_straddle },
    { "seeking", "first_frames", seek_first_frames },
    { "seeking", "block_boundaries", seek_block_boundaries },
    { "seeking", "read_interleaved", seek_read_interleaved },
    { "seeking", "invalid_positions", seek_invalid_positions },
    { "compatibility", "native_mode", compat_native_mode },
    { "compatibility", "short_files", compat_short_files },
    { "compatibility", "mono", compat_mono },
    { "compatibility", "multichannel_2ch_max", compat_multichannel_2ch_max },
    { "compatibility", "pcm_and_float", compat_pcm_and_float }
};

const size_t task_case_count = sizeof (task_cases) / sizeof (task_cases [0]);
