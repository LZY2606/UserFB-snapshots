////////////////////////////////////////////////////////////////////////////
// channel_projection.c
//
// Harness for WavpackUnpackSamplesMapped(). All test material is encoded with
// the library itself (mono, stereo and six-channel integer PCM, including
// multi-block streams and a hybrid WV/WVC pair). The reference for every
// projected decode is a plain full-width WavpackUnpackSamples() decode with
// the map applied column by column afterwards.
//
// The suite runs serially and again with two decoder worker threads, and
// records every allocation performed inside the library so that no
// full-width intermediate PCM buffer can slip in unnoticed.
////////////////////////////////////////////////////////////////////////////

#include "wavpack.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

////////////////////////// allocation interposition //////////////////////////

// The test links the static library directly, so these definitions satisfy
// the library's own malloc/calloc/realloc/free references. Every block gets a
// 16-byte header carrying its size, which lets us audit live usage and the
// largest single allocation while decoding is in progress.

static void *(*real_malloc) (size_t);
static void *(*real_calloc) (size_t, size_t);
static void *(*real_realloc) (void *, size_t);
static void  (*real_free) (void *);

// dlsym() itself may allocate, so the very first allocations are served from a
// static bootstrap arena until the real allocators have been resolved.

static char boot_arena [1 << 16];
static size_t boot_used;
static int allocators_resolved;

static void resolve_real_allocators (void)
{
    if (allocators_resolved)
        return;

    allocators_resolved = 1;

#if defined (__APPLE__)
    // macOS keeps these in libSystem, so no extra link library is required.
    real_malloc = dlsym (RTLD_NEXT, "malloc");
    real_calloc = dlsym (RTLD_NEXT, "calloc");
    real_realloc = dlsym (RTLD_NEXT, "realloc");
    real_free = dlsym (RTLD_NEXT, "free");
#else
    // glibc exports the underlying allocators directly, avoiding a dependency on -ldl
    // (the baseline harness link line does not include it).
    extern void *__libc_malloc (size_t);
    extern void *__libc_calloc (size_t, size_t);
    extern void *__libc_realloc (void *, size_t);
    extern void __libc_free (void *);
    real_malloc = __libc_malloc;
    real_calloc = __libc_calloc;
    real_realloc = __libc_realloc;
    real_free = (void (*) (void *)) __libc_free;
#endif
}

static int in_boot_arena (const void *ptr)
{
    return (const char *) ptr >= boot_arena && (const char *) ptr < boot_arena + sizeof (boot_arena);
}

static void *boot_alloc (size_t size)
{
    size_t aligned = (boot_used + 15u) & ~(size_t) 15u;
    void *ptr;

    if (aligned + size > sizeof (boot_arena))
        return NULL;

    ptr = boot_arena + aligned;
    boot_used = aligned + size;
    return ptr;
}

static int track_active;
static long live_bytes, peak_live_bytes, alloc_events, max_single_alloc;

static void track_alloc (size_t size)
{
    if (!track_active)
        return;

    live_bytes += (long) size;
    alloc_events++;

    if (live_bytes > peak_live_bytes)
        peak_live_bytes = live_bytes;

    if ((long) size > max_single_alloc)
        max_single_alloc = (long) size;
}

static void track_free (size_t size)
{
    if (!track_active)
        return;

    live_bytes -= (long) size;
}

void *malloc (size_t size)
{
    void *raw;

    if (!allocators_resolved)
        resolve_real_allocators ();

    if (!real_malloc)
        return boot_alloc (size);

    raw = real_malloc (size + 16);

    if (!raw)
        return NULL;

    *((size_t *) raw) = size;
    track_alloc (size);
    return (char *) raw + 16;
}

void *calloc (size_t n, size_t size)
{
    size_t total = n * size;
    void *raw;

    if (!allocators_resolved)
        resolve_real_allocators ();

    if (!real_calloc) {
        raw = boot_alloc (total);

        if (raw)
            memset (raw, 0, total);

        return raw;
    }

    raw = real_calloc (1, total + 16);

    if (!raw)
        return NULL;

    *((size_t *) raw) = total;
    memset ((char *) raw + 16, 0, total);
    track_alloc (total);
    return (char *) raw + 16;
}

void *realloc (void *ptr, size_t size)
{
    void *raw, *moved;
    size_t old_size;

    if (!allocators_resolved)
        resolve_real_allocators ();

    if (!ptr)
        return malloc (size);

    if (in_boot_arena (ptr)) {
        void *fresh = malloc (size);

        if (fresh)
            memcpy (fresh, ptr, size);

        return fresh;
    }

    raw = (char *) ptr - 16;
    old_size = *((size_t *) raw);
    track_free (old_size);

    if (!real_realloc)
        return NULL;

    moved = real_realloc (raw, size + 16);

    if (!moved)
        return NULL;

    *((size_t *) moved) = size;
    track_alloc (size);
    return (char *) moved + 16;
}

void free (void *ptr)
{
    void *raw;
    size_t size;

    if (!ptr)
        return;

    if (!allocators_resolved)
        resolve_real_allocators ();

    if (in_boot_arena (ptr) || !real_free)
        return;     // bootstrap allocations are intentionally leaked

    raw = (char *) ptr - 16;
    size = *((size_t *) raw);
    track_free (size);
    real_free (raw);
}

static void tracking_reset (void)
{
    live_bytes = peak_live_bytes = alloc_events = max_single_alloc = 0;
}

static void tracking_on (void)
{
    tracking_reset ();
    track_active = 1;
}

static void tracking_off (void)
{
    track_active = 0;
}

//////////////////////////////// test helpers ////////////////////////////////

static int failures;
static char workdir [1024];

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf ("  FAIL: "); \
        printf (__VA_ARGS__); \
        printf ("\n"); \
        failures++; \
    } \
} while (0)

static int output_cb (void *id, void *bytes, int32_t n)
{
    return fwrite (bytes, 1, (size_t) n, (FILE *) id) == (size_t) n;
}

// Deterministic per-channel marker so every decoded value identifies its frame
// and source channel, while staying inside signed 16-bit range.

static int32_t marker (int frame, int channel)
{
    return (int32_t) (((frame * 1000003u + channel * 9176u + 7u) % 20001u) - 10000u);
}

struct encoded {
    char wv_path [1080];
    char wvc_path [1080];
    int num_channels;
    int channel_mask;
    int total_samples;
    int block_samples;
    int hybrid;
    int floating;
};

static void fill_source (int32_t *buf, const struct encoded *e)
{
    int n, k;

    for (n = 0; n < e->total_samples; ++n)
        for (k = 0; k < e->num_channels; ++k) {
            if (e->floating) {
                float f = (float) (marker (n, k)) / 10000.0f;
                memcpy (buf + n * e->num_channels + k, &f, sizeof (f));
            }
            else
                buf [n * e->num_channels + k] = marker (n, k);
        }
}

static void encode_file (struct encoded *e, const char *name, int num_channels, int channel_mask,
    int total_samples, int block_samples, int hybrid, int floating)
{
    WavpackConfig config;
    WavpackContext *wpc;
    FILE *wvf = NULL, *wvcf = NULL;
    int32_t *source;
    int rc;

    e->num_channels = num_channels;
    e->channel_mask = channel_mask;
    e->total_samples = total_samples;
    e->block_samples = block_samples;
    e->hybrid = hybrid;
    e->floating = floating;

    snprintf (e->wv_path, sizeof (e->wv_path), "%s/%s.wv", workdir, name);
    snprintf (e->wvc_path, sizeof (e->wvc_path), "%s/%s.wvc", workdir, name);

    wvf = fopen (e->wv_path, "wb");
    assert (wvf);

    if (hybrid) {
        wvcf = fopen (e->wvc_path, "wb");
        assert (wvcf);
    }

    wpc = WavpackOpenFileOutput (output_cb, wvf, wvcf);
    assert (wpc);

    memset (&config, 0, sizeof (config));
    config.num_channels = num_channels;
    config.channel_mask = channel_mask;
    config.sample_rate = 48000;
    config.block_samples = block_samples;

    if (floating) {
        config.bytes_per_sample = 4;
        config.bits_per_sample = 32;
        config.float_norm_exp = 127;
    }
    else {
        config.bytes_per_sample = 2;
        config.bits_per_sample = 16;
    }

    if (hybrid) {
        config.flags = CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC;
        config.bitrate = 4.0f;
    }

    rc = WavpackSetConfiguration64 (wpc, &config, total_samples, NULL);
    assert (rc);
    assert (WavpackPackInit (wpc));

    source = malloc ((size_t) total_samples * num_channels * sizeof (int32_t));
    assert (source);
    fill_source (source, e);
    assert (WavpackPackSamples (wpc, source, (uint32_t) total_samples));
    assert (WavpackFlushSamples (wpc));
    free (source);
    WavpackCloseFile (wpc);
    fclose (wvf);

    if (wvcf)
        fclose (wvcf);
}

static WavpackContext *open_decoder (const struct encoded *e, int extra_flags, int use_wvc)
{
    char error[80] = "";
    int flags = extra_flags;
    WavpackContext *wpc;

    if (use_wvc && e->hybrid)
        flags |= OPEN_WVC;

    wpc = WavpackOpenFileInput (e->wv_path, error, flags, 0);
    assert (wpc);
    return wpc;
}

// Plain full-width reference decode of the whole file.

static void decode_full (const struct encoded *e, int extra_flags, int use_wvc, int32_t *out)
{
    WavpackContext *wpc = open_decoder (e, extra_flags, use_wvc);
    int done = 0;

    while (done < e->total_samples) {
        int want = e->total_samples - done;
        uint32_t got;

        if (want > 17)
            want = 17;

        got = WavpackUnpackSamples (wpc, out + done * e->num_channels, (uint32_t) want);
        assert (got);
        done += (int) got;
    }

    assert (done == e->total_samples);
    WavpackCloseFile (wpc);
}

// Projected decode in arbitrary chunk sizes. Returns the number of frames read.

static int decode_mapped (WavpackContext *wpc, int total_samples,
    const int *map, int map_count, const int *chunks, int num_chunks, int32_t *out)
{
    int chunk = 0, done = 0;

    while (done < total_samples) {
        int want = chunks [chunk++ % num_chunks];
        uint32_t got;

        if (want > total_samples - done)
            want = total_samples - done;

        got = WavpackUnpackSamplesMapped (wpc, out + done * map_count, (uint32_t) want,
            map, (uint32_t) map_count);

        if (!got)
            break;

        done += (int) got;
    }

    return done;
}

static void check_projection (const char *label, const struct encoded *e, int open_flags, int use_wvc,
    const int *map, int map_count, const int *chunks, int num_chunks)
{
    int32_t *reference = malloc ((size_t) e->total_samples * e->num_channels * sizeof (int32_t));
    int32_t *projected = malloc ((size_t) e->total_samples * map_count * sizeof (int32_t));
    WavpackContext *wpc;
    int n, j, done;

    assert (reference && projected);
    decode_full (e, open_flags, use_wvc, reference);

    wpc = open_decoder (e, open_flags, use_wvc);
    done = decode_mapped (wpc, e->total_samples, map, map_count, chunks, num_chunks, projected);

    CHECK (done == e->total_samples, "%s: got %d frames, expected %d", label, done, e->total_samples);
    CHECK (WavpackGetNumChannels (wpc) == e->num_channels, "%s: source channel count changed to %d",
        label, WavpackGetNumChannels (wpc));
    CHECK (WavpackGetChannelMask (wpc) == e->channel_mask, "%s: channel mask changed", label);
    CHECK (WavpackGetReducedChannels (wpc) == e->num_channels, "%s: reduced channels leaked", label);

    for (n = 0; n < done; ++n)
        for (j = 0; j < map_count; ++j) {
            int32_t expect = reference [n * e->num_channels + map [j]];
            int32_t actual = projected [n * map_count + j];

            if (expect != actual) {
                CHECK (0, "%s: mismatch frame=%d slot=%d expect=%d got=%d", label, n, j, expect, actual);
                break;
            }
        }

    CHECK (WavpackGetSampleIndex64 (wpc) == e->total_samples, "%s: cursor at %lld",
        label, (long long) WavpackGetSampleIndex64 (wpc));
    CHECK (WavpackGetNumErrors (wpc) == 0, "%s: unexpected decode errors (%d)", label, WavpackGetNumErrors (wpc));

    WavpackCloseFile (wpc);
    free (reference);
    free (projected);
}

////////////////////////////////// test cases /////////////////////////////////

static struct encoded mono_e, stereo_e, six_e, six_hybrid_e, float_e;
static int thread_open_flag;       // 0 for serial, 2 worker threads otherwise

// 1. Mono material with a trivial single-entry map.

static void case_mono (void)
{
    const int map [1] = { 0 };
    const int chunks [] = { 1, 5, 13, 37 };

    check_projection ("mono", &mono_e, thread_open_flag, 0, map, 1, chunks, 4);
}

// 2. Stereo material with reversed order and a duplicated source index (the
//    map count never exceeds the number of source channels).

static void case_stereo_duplicate_reverse (void)
{
    const int reverse_map [2] = { 1, 0 };
    const int duplicate_map [2] = { 1, 1 };
    const int chunks [] = { 3, 32, 33, 64 };

    check_projection ("stereo reverse", &stereo_e, thread_open_flag, 0, reverse_map, 2, chunks, 4);
    check_projection ("stereo duplicate", &stereo_e, thread_open_flag, 0, duplicate_map, 2, chunks, 4);
}

// 3. Six-channel material, the headline {5,0,2,0} map, including reads that
//    straddle the 32-sample compression blocks and an EOF over-read that must
//    leave both the unreturned tail and the surrounding sentinels untouched.

static void case_sixch_basic (void)
{
    const int map [4] = { 5, 0, 2, 0 };
    const int chunks [] = { 7, 31, 33, 128 };
    const int total = six_e.total_samples;
    int32_t *reference, *guarded;
    WavpackContext *wpc;
    const int32_t sentinel = (int32_t) 0x76543210;
    int n, j;
    size_t slots = (size_t) (total + 16) * 4 + 8;

    check_projection ("six-channel", &six_e, thread_open_flag, 0, map, 4, chunks, 4);

    reference = malloc ((size_t) total * six_e.num_channels * sizeof (int32_t));
    guarded = malloc (slots * sizeof (int32_t));
    assert (reference && guarded);
    decode_full (&six_e, thread_open_flag, 0, reference);

    for (n = 0; n < (int) slots; ++n)
        guarded [n] = sentinel;

    wpc = open_decoder (&six_e, thread_open_flag, 0);

    // decode almost to the end, then ask for more frames than remain
    {
        int done = 0;
        const int lead [] = { 50, 100, 200 };

        while (total - done > 13) {
            int want = lead [(done / 50) % 3];
            uint32_t got = WavpackUnpackSamplesMapped (wpc, guarded + done * 4, (uint32_t) want, map, 4);
            assert (got);
            done += (int) got;
        }
    }

    {
        int64_t index = WavpackGetSampleIndex64 (wpc);
        int remaining = total - (int) index;
        uint32_t got = WavpackUnpackSamplesMapped (wpc, guarded + index * 4, 50, map, 4);

        CHECK ((int) got == remaining, "eof: got %u, expected %d", got, remaining);
        CHECK (WavpackUnpackSamplesMapped (wpc, guarded, 50, map, 4) == 0, "eof: past-EOF call returned data");
    }

    for (n = 0; n < total; ++n)
        for (j = 0; j < 4; ++j)
            CHECK (guarded [n * 4 + j] == reference [n * 6 + map [j]],
                "eof: content mismatch at frame %d slot %d", n, j);

    for (n = total * 4; n < (int) slots; ++n)
        CHECK (guarded [n] == sentinel, "eof: sentinel at tail slot %d was modified", n);

    WavpackCloseFile (wpc);
    free (reference);
    free (guarded);
}

// 4. Hybrid lossless WV/WVC material (plus the lossy WV-only open) and the
//    allocation audit: no allocation may approach full-width PCM size.

static void case_hybrid_and_allocations (void)
{
    const int map [4] = { 5, 0, 2, 0 };
    const int chunks [] = { 7, 31, 33, 128 };
    WavpackContext *wpc;
    int forbidden_width = 128 * six_hybrid_e.num_channels * (int) sizeof (int32_t);   // full-width chunk
    int allowed_width = 128 * 2 * (int) sizeof (int32_t);                              // per-stream temp
    int done = 0, chunk = 0;
    int32_t *out;

    // hybrid lossless with correction stream
    check_projection ("hybrid wvc", &six_hybrid_e, thread_open_flag, 1, map, 4, chunks, 4);

    // hybrid lossy: same .wv opened without the .wvc, mapped vs plain column select
    check_projection ("hybrid lossy", &six_hybrid_e, thread_open_flag, 0, map, 4, chunks, 4);

    out = malloc ((size_t) six_hybrid_e.total_samples * 4 * sizeof (int32_t));
    assert (out);
    wpc = open_decoder (&six_hybrid_e, thread_open_flag, 1);

    tracking_on ();

    while (done < six_hybrid_e.total_samples) {
        int want = chunks [chunk++ % 4];
        uint32_t got;

        if (want > six_hybrid_e.total_samples - done)
            want = six_hybrid_e.total_samples - done;

        got = WavpackUnpackSamplesMapped (wpc, out + done * 4, (uint32_t) want, map, 4);
        assert (got);
        done += (int) got;
    }

    // Decode the entire file again in a single call. If the implementation used a
    // full-width intermediate buffer, the largest allocation would scale with the
    // request size (1025 frames x 6 channels). The per-stream temp may only reach
    // one block's worth of two-channel PCM regardless of how many frames are asked
    // for, so this bound stays the same.

    {
        long chunked_max = max_single_alloc;
        WavpackContext *whole;
        int32_t *whole_out = malloc ((size_t) six_hybrid_e.total_samples * 4 * sizeof (int32_t));
        int32_t *reference = malloc ((size_t) six_hybrid_e.total_samples * 6 * sizeof (int32_t));
        int n, j;
        uint32_t got;

        assert (whole_out && reference);
        decode_full (&six_hybrid_e, thread_open_flag, 1, reference);
        whole = open_decoder (&six_hybrid_e, thread_open_flag, 1);
        tracking_reset ();

        got = WavpackUnpackSamplesMapped (whole, whole_out, (uint32_t) six_hybrid_e.total_samples, map, 4);
        CHECK (got == (uint32_t) six_hybrid_e.total_samples, "alloc: whole-file call returned %u frames", got);

        for (n = 0; n < six_hybrid_e.total_samples; ++n)
            for (j = 0; j < 4; ++j)
                CHECK (whole_out [n * 4 + j] == reference [n * 6 + map [j]],
                    "alloc: whole-file content mismatch at frame %d slot %d", n, j);

        printf ("  alloc: whole-file single call, largest single %ld bytes (chunked was %ld; full-width would be %d)\n",
            max_single_alloc, chunked_max, forbidden_width);

        // the whole-file request is eight times longer than the biggest chunk, yet the
        // largest scratch allocation must not grow beyond what the chunked run needed
        CHECK (max_single_alloc <= chunked_max,
            "alloc: scratch grew from %ld to %ld when request size grew", chunked_max, max_single_alloc);

        WavpackCloseFile (whole);
        free (whole_out);
        free (reference);
    }

    tracking_off ();

    CHECK (max_single_alloc < forbidden_width,
        "alloc: single allocation %ld bytes reaches full-width PCM (%d)", max_single_alloc, forbidden_width);
    CHECK (max_single_alloc <= allowed_width + 4096,
        "alloc: single allocation %ld exceeds two-channel temp budget %d (+4096 slack)",
        max_single_alloc, allowed_width);
    printf ("  alloc: %ld events, peak live %ld bytes, largest single %ld bytes (full-width would be %d)\n",
        alloc_events, peak_live_bytes, max_single_alloc, forbidden_width);

    WavpackCloseFile (wpc);
    free (out);
}

// 5. Old and new entry points alternate on one instance, then the map changes
//    and the instance is seeked; no projection state may leak between calls.

static void case_alternate_and_switch (void)
{
    const int map_a [4] = { 5, 0, 2, 0 };
    const int map_b [2] = { 3, 3 };
    const int total = six_e.total_samples;
    int32_t *reference, *plain_buf, *mapped_buf;
    WavpackContext *wpc;
    int done = 0, use_mapped = 0, n, j;

    reference = malloc ((size_t) total * 6 * sizeof (int32_t));
    plain_buf = malloc ((size_t) total * 6 * sizeof (int32_t));
    mapped_buf = malloc ((size_t) total * 4 * sizeof (int32_t));
    assert (reference && plain_buf && mapped_buf);
    decode_full (&six_e, thread_open_flag, 0, reference);

    wpc = open_decoder (&six_e, thread_open_flag, 0);

    while (done < total) {
        int want = total - done;
        uint32_t got;

        if (want > 9)
            want = 9;

        if ((use_mapped++ & 1) == 0) {
            got = WavpackUnpackSamples (wpc, plain_buf + done * 6, (uint32_t) want);
            assert (got == (uint32_t) want);

            for (n = 0; n < want * 6; ++n)
                CHECK (plain_buf [done * 6 + n] == reference [done * 6 + n],
                    "alternate: plain mismatch at frame %d slot %d", done + n / 6, n % 6);
        }
        else {
            got = WavpackUnpackSamplesMapped (wpc, mapped_buf + done * 4, (uint32_t) want, map_a, 4);
            assert (got == (uint32_t) want);

            for (n = 0; n < want; ++n)
                for (j = 0; j < 4; ++j)
                    CHECK (mapped_buf [(done + n) * 4 + j] == reference [(done + n) * 6 + map_a [j]],
                        "alternate: mapped mismatch at %d slot %d", done + n, j);
        }

        done += want;
    }

    // switch to a different map: output must follow map_b with no residue of map_a
    {
        int32_t bbuf [64 * 2];
        int got;

        assert (WavpackSeekSample64 (wpc, 0));
        got = (int) WavpackUnpackSamplesMapped (wpc, bbuf, 64, map_b, 2);
        CHECK (got == 64, "switch: got %d frames", got);

        for (n = 0; n < got; ++n)
            for (j = 0; j < 2; ++j)
                CHECK (bbuf [n * 2 + j] == reference [n * 6 + map_b [j]],
                    "switch: wrong column at frame %d slot %d", n, j);
    }

    // after a seek the plain entry still works and source layout queries are intact
    {
        int32_t pbuf [16 * 6];

        assert (WavpackSeekSample64 (wpc, 40));
        CHECK ((int) WavpackUnpackSamples (wpc, pbuf, 16) == 16, "switch: plain decode after seek failed");

        for (n = 0; n < 16 * 6; ++n)
            CHECK (pbuf [n] == reference [(40 + n / 6) * 6 + n % 6], "switch: plain data mismatch at %d", n);
    }

    CHECK (WavpackGetNumChannels (wpc) == 6 && WavpackGetChannelMask (wpc) == 0x3f &&
        WavpackGetReducedChannels (wpc) == 6, "switch: source layout queries changed");

    WavpackCloseFile (wpc);
    free (reference);
    free (plain_buf);
    free (mapped_buf);
}

// 6. Seek to several positions and compare projected output against the
//    reference, including the final cursor position.

static void case_seek (void)
{
    const int map [3] = { 4, 1, 0 };
    const int targets [] = { 0, 33, 100, 500, 1024 };
    int32_t *reference = malloc ((size_t) six_e.total_samples * 6 * sizeof (int32_t));
    WavpackContext *wpc = open_decoder (&six_e, thread_open_flag, 0);
    int t;

    assert (reference);
    decode_full (&six_e, thread_open_flag, 0, reference);

    for (t = 0; t < (int) (sizeof (targets) / sizeof (targets [0])); ++t) {
        int start = targets [t];
        int want = six_e.total_samples - start;
        int32_t *out;
        int n, j, got;

        if (want > 40)
            want = 40;

        out = malloc ((size_t) want * 3 * sizeof (int32_t));
        assert (out);
        assert (WavpackSeekSample64 (wpc, start));
        got = (int) WavpackUnpackSamplesMapped (wpc, out, (uint32_t) want, map, 3);
        CHECK (got == want, "seek: target %d returned %d frames", start, got);
        CHECK ((int) WavpackGetSampleIndex64 (wpc) == start + got, "seek: cursor mismatch at target %d", start);

        for (n = 0; n < got; ++n)
            for (j = 0; j < 3; ++j)
                CHECK (out [n * 3 + j] == reference [(start + n) * 6 + map [j]],
                    "seek: mismatch target=%d frame=%d slot=%d", start, n, j);

        free (out);
    }

    WavpackCloseFile (wpc);
    free (reference);
}

// 7. Every invalid argument must fail closed: zero return, readable error,
//    input unread, index unchanged, output untouched. Also covers the type
//    rejections (float data, OPEN_2CH_MAX) and the zero-frame call.

static void case_invalid_args (void)
{
    const int good_map [4] = { 5, 0, 2, 0 };
    const int bad_count_map [7] = { 0, 1, 2, 3, 4, 5, 6 };
    const int negative_map [3] = { 0, -1, 2 };
    const int high_map [2] = { 0, 6 };
    int32_t guard [64];
    const int32_t sentinel = (int32_t) 0x1234abcd;
    WavpackContext *wpc = open_decoder (&six_e, thread_open_flag, 0);
    WavpackContext *fwpc, *twowpc;
    int64_t index_before;
    int k, g;
    char error[80];

    struct attempt { const char *name; const int *map; uint32_t count; } attempts [] = {
        { "null map", NULL, 4 },
        { "zero count", good_map, 0 },
        { "count too large", bad_count_map, 7 },
        { "negative index", negative_map, 3 },
        { "index too high", high_map, 2 },
    };

    for (k = 0; k < (int) (sizeof (attempts) / sizeof (attempts [0])); ++k) {
        for (g = 0; g < (int) (sizeof (guard) / sizeof (guard [0])); ++g)
            guard [g] = sentinel;

        index_before = WavpackGetSampleIndex64 (wpc);

        {
            uint32_t rc = WavpackUnpackSamplesMapped (wpc, guard, 8, attempts [k].map, attempts [k].count);

            CHECK (rc == 0, "invalid '%s': returned %u", attempts [k].name, rc);
            CHECK (WavpackGetErrorMessage (wpc) && WavpackGetErrorMessage (wpc) [0],
                "invalid '%s': no error message", attempts [k].name);
            CHECK (WavpackGetSampleIndex64 (wpc) == index_before,
                "invalid '%s': sample index advanced", attempts [k].name);
        }

        for (g = 0; g < (int) (sizeof (guard) / sizeof (guard [0])); ++g)
            CHECK (guard [g] == sentinel, "invalid '%s': output slot %d modified", attempts [k].name, g);
    }

    // zero-frame call has no side effects and needs no validation
    for (g = 0; g < (int) (sizeof (guard) / sizeof (guard [0])); ++g)
        guard [g] = sentinel;

    index_before = WavpackGetSampleIndex64 (wpc);
    CHECK (WavpackUnpackSamplesMapped (wpc, guard, 0, good_map, 4) == 0, "zero-frame call returned nonzero");
    CHECK (WavpackGetSampleIndex64 (wpc) == index_before, "zero-frame call advanced the index");

    for (g = 0; g < (int) (sizeof (guard) / sizeof (guard [0])); ++g)
        CHECK (guard [g] == sentinel, "zero-frame call modified output slot %d", g);

    // a valid call still works after all the rejections
    CHECK (WavpackUnpackSamplesMapped (wpc, guard, 8, good_map, 4) == 8, "valid call after invalid ones failed");
    CHECK (WavpackGetSampleIndex64 (wpc) == index_before + 8, "valid call advanced index incorrectly");

    WavpackCloseFile (wpc);

    // floating point data is rejected up front
    fwpc = open_decoder (&float_e, 0, 0);
    for (g = 0; g < (int) (sizeof (guard) / sizeof (guard [0])); ++g)
        guard [g] = sentinel;
    CHECK (WavpackUnpackSamplesMapped (fwpc, guard, 8, good_map, 4) == 0, "float: mapping was accepted");
    CHECK (WavpackGetErrorMessage (fwpc) && WavpackGetErrorMessage (fwpc) [0], "float: no error message");
    for (g = 0; g < (int) (sizeof (guard) / sizeof (guard [0])); ++g)
        CHECK (guard [g] == sentinel, "float: output was written");
    WavpackCloseFile (fwpc);

    // OPEN_2CH_MAX instances are rejected up front
    twowpc = WavpackOpenFileInput (six_e.wv_path, error, OPEN_2CH_MAX | thread_open_flag, 0);
    assert (twowpc);
    CHECK (WavpackGetReducedChannels (twowpc) == 2, "2ch_max: reduced channel query wrong");
    CHECK (WavpackGetNumChannels (twowpc) == 6, "2ch_max: source channel count must stay 6");
    for (g = 0; g < (int) (sizeof (guard) / sizeof (guard [0])); ++g)
        guard [g] = sentinel;
    CHECK (WavpackUnpackSamplesMapped (twowpc, guard, 8, good_map, 4) == 0, "2ch_max: mapping was accepted");
    CHECK (WavpackGetErrorMessage (twowpc) && WavpackGetErrorMessage (twowpc) [0], "2ch_max: no error message");
    WavpackCloseFile (twowpc);
}

// 8. Corrupt a non-first substream. Both plain and projected decodes must
//    consume the damaged stream, record the error and mute the affected group,
//    even when the map selects none of that substream's channels.

static void corrupt_block_one (const char *src_path, const char *dst_path)
{
    FILE *in = fopen (src_path, "rb");
    FILE *out;
    long *positions;
    int block = 0, capacity = 64, target = 1;       // second wvpk block = first non-initial substream
    long target_pos, target_end;
    int c;

    assert (in);
    positions = malloc ((size_t) capacity * sizeof (long));
    assert (positions);

    while (1) {
        WavpackHeader hdr;
        long pos = ftell (in);

        if (fread (&hdr, sizeof (hdr), 1, in) != 1)
            break;

        if (memcmp (hdr.ckID, "wvpk", 4))
            break;

        if (block == capacity) {
            capacity *= 2;
            positions = realloc (positions, (size_t) capacity * sizeof (long));
            assert (positions);
        }

        positions [block++] = pos;
        fseek (in, pos + hdr.ckSize + 8, SEEK_SET);
    }

    assert (block >= 3);
    target_pos = positions [target];
    fseek (in, target_pos, SEEK_SET);

    {
        WavpackHeader hdr;
        assert (fread (&hdr, sizeof (hdr), 1, in) == 1);
        target_end = target_pos + hdr.ckSize + 8;
    }

    out = fopen (dst_path, "w+b");
    assert (out);
    fseek (in, 0, SEEK_SET);

    while ((c = fgetc (in)) != EOF)
        fputc ((unsigned char) c, out);

    // flip several payload bytes spread through the second substream's block so
    // that (with block checksums bypassed) the damage reaches the codec itself
    {
        long span = target_end - (target_pos + 36);
        int flip;

        assert (span > 16);

        for (flip = 0; flip < 4; ++flip) {
            long at = target_pos + 36 + span * (flip + 1) / 5;
            int val;

            fseek (out, at, SEEK_SET);
            val = fgetc (out);
            assert (val != EOF);
            fseek (out, at, SEEK_SET);
            fputc ((unsigned char) val ^ (0xa5 ^ (flip * 37)), out);
        }
    }

    fclose (in);
    fclose (out);
    free (positions);
}

static void case_corrupt_substream (void)
{
    char corrupt_path [1080];
    char error[80];
    const int map_all [6] = { 0, 1, 2, 3, 4, 5 };
    const int map_first_only [2] = { 0, 1 };
    WavpackContext *plain, *mapped, *selective;
    int32_t *pbuf, *mbuf, *sbuf;
    int frames = 64, got_p, got_m, got_s, n, j;

    snprintf (corrupt_path, sizeof (corrupt_path), "%s/corrupt-%d.wv", workdir, thread_open_flag ? 2 : 0);
    corrupt_block_one (six_e.wv_path, corrupt_path);

    pbuf = malloc ((size_t) frames * 6 * sizeof (int32_t));
    mbuf = malloc ((size_t) frames * 6 * sizeof (int32_t));
    sbuf = malloc ((size_t) frames * 2 * sizeof (int32_t));
    assert (pbuf && mbuf && sbuf);

    // OPEN_NO_CHECKSUM forces the damaged block through the codec instead of
    // being neutralized at verification time, exercising the CRC/mute path.

    plain = WavpackOpenFileInput (corrupt_path, error, OPEN_NO_CHECKSUM | thread_open_flag, 0);
    mapped = WavpackOpenFileInput (corrupt_path, error, OPEN_NO_CHECKSUM | thread_open_flag, 0);
    selective = WavpackOpenFileInput (corrupt_path, error, OPEN_NO_CHECKSUM | thread_open_flag, 0);
    assert (plain && mapped && selective);

    got_p = (int) WavpackUnpackSamples (plain, pbuf, (uint32_t) frames);
    got_m = (int) WavpackUnpackSamplesMapped (mapped, mbuf, (uint32_t) frames, map_all, 6);
    got_s = (int) WavpackUnpackSamplesMapped (selective, sbuf, (uint32_t) frames, map_first_only, 2);

    CHECK (got_p == got_m, "corrupt: frame counts differ (%d vs %d)", got_p, got_m);
    CHECK (WavpackGetNumErrors (plain) > 0, "corrupt: plain decode recorded no error");
    CHECK (WavpackGetNumErrors (mapped) == WavpackGetNumErrors (plain),
        "corrupt: error counts differ (mapped=%d plain=%d)", WavpackGetNumErrors (mapped), WavpackGetNumErrors (plain));
    CHECK (WavpackGetNumErrors (selective) > 0,
        "corrupt: unselected damaged substream was not counted (errors=%d)", WavpackGetNumErrors (selective));
    CHECK (WavpackGetSampleIndex64 (mapped) == WavpackGetSampleIndex64 (plain),
        "corrupt: cursors differ");
    CHECK (got_s == got_p, "corrupt: selective frame count differs");

    for (n = 0; n < got_m; ++n)
        for (j = 0; j < 6; ++j)
            CHECK (mbuf [n * 6 + j] == pbuf [n * 6 + j],
                "corrupt: projected output diverges at frame %d slot %d", n, j);

    for (n = 0; n < got_s; ++n)
        for (j = 0; j < 2; ++j)
            CHECK (sbuf [n * 2 + j] == pbuf [n * 6 + map_first_only [j]],
                "corrupt: selective output diverges at frame %d slot %d", n, j);

    // the codec-level failure must mute the damaged group; confirm at least one
    // muted frame exists inside the first 32-sample compression block and that the
    // projection carries the same zeros rather than stale or arbitrary data

    {
        int muted = 0, span = got_m < 32 ? got_m : 32;

        for (n = 0; n < span; ++n)
            for (j = 0; j < 6; ++j)
                if (pbuf [n * 6 + j] == 0)
                    muted++;

        CHECK (muted > 0, "corrupt: expected muted samples in the damaged block, found none");

        for (n = 0; n < span; ++n)
            for (j = 0; j < 6; ++j)
                if (pbuf [n * 6 + j] == 0)
                    CHECK (mbuf [n * 6 + j] == 0, "corrupt: projection did not preserve mute at frame %d slot %d", n, j);
    }

    WavpackCloseFile (plain);
    WavpackCloseFile (mapped);
    WavpackCloseFile (selective);
    free (pbuf);
    free (mbuf);
    free (sbuf);
}

/////////////////////////////////// driver ////////////////////////////////////

struct test_case {
    const char *name;
    void (*fn) (void);
};

int main (int argc, char **argv)
{
    struct test_case cases [] = {
        { "mono projection", case_mono },
        { "stereo duplicate and reverse", case_stereo_duplicate_reverse },
        { "six-channel projection with EOF sentinels", case_sixch_basic },
        { "hybrid WV/WVC, lossy, allocation audit", case_hybrid_and_allocations },
        { "old/new alternation, map switch, seek", case_alternate_and_switch },
        { "seek then project", case_seek },
        { "invalid maps and rejected types, zero side effects", case_invalid_args },
        { "corrupt non-first substream", case_corrupt_substream },
    };
    int mode, i, total_failures = 0;

    assert (argc == 2);
    snprintf (workdir, sizeof (workdir), "%s", argv [1]);

    // material: several compression blocks per file (block sizes < total frames)
    encode_file (&mono_e, "mono", 1, 0x4, 300, 37, 0, 0);
    encode_file (&stereo_e, "stereo", 2, 0x3, 400, 32, 0, 0);
    encode_file (&six_e, "six", 6, 0x3f, 1025, 32, 0, 0);
    encode_file (&six_hybrid_e, "sixhybrid", 6, 0x3f, 1025, 32, 1, 0);
    encode_file (&float_e, "floats", 2, 0x3, 256, 32, 0, 1);

    for (mode = 0; mode < 2; ++mode) {
        thread_open_flag = mode ? (2 << OPEN_THREADS_SHFT) : 0;
        failures = 0;
        printf ("--- %s ---\n", mode ? "2 worker threads" : "serial");

        for (i = 0; i < (int) (sizeof (cases) / sizeof (cases [0])); ++i) {
            int before = failures;
            printf ("[%d/8] %s\n", i + 1, cases [i].name);
            cases [i].fn ();

            if (failures == before)
                printf ("  ok\n");
        }

        total_failures += failures;
    }

    if (total_failures) {
        printf ("CHANNEL_PROJECTION_FAILED failures=%d\n", total_failures);
        return 1;
    }

    printf ("WAVPACK_CHANNEL_PROJECTION_OK cases=8\n");
    return 0;
}
