/*
    devcheck/backend_contract.c - per-instance backend dispatch contract test.

    Verifies MINIMP3_RUNTIME_DISPATCH behaviour, and the constrained behaviour
    of default / MINIMP3_NO_SIMD / MINIMP3_ONLY_SIMD builds:
      * setter/getter contract (illegal values and unsupported paths fail and
        keep the previous choice);
      * the kernel path actually taken per frame is recorded through
        MINIMP3_BACKEND_TEST_HOOK, not inferred from the getter alone;
      * continuous scalar reference vs continuous SIMD vs per-frame alternating
        schedules: identical stream consumption/metadata and PCM within
        tolerance (4 LSB max, RMS < 1 LSB for int16; 1e-5 absolute for float);
      * backend choice survives internal resync, and only mp3dec_init resets it;
      * mp3dec_ex_t lifecycle: open selects AUTO, set on mp3d works, partial
        reads, backend switches while PCM is buffered, and seek keeps the
        choice without dropping buffered samples;
      * two interleaved decoder instances cannot influence each other.

    The whole program always runs: builds without SIMD exercise the scalar path
    plus the setter-failure branches instead of being skipped.
*/
#define MINIMP3_BACKEND_TEST_HOOK
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_LOG_FRAMES 1024

#ifdef MINIMP3_FLOAT_OUTPUT
typedef float test_sample_t;
#define ABS_TOLERANCE 1e-5
#define RMS_TOLERANCE 1e-5
static double sample_diff(test_sample_t a, test_sample_t b) { return fabs((double)a - (double)b); }
#else
typedef mp3d_sample_t test_sample_t;
#define ABS_TOLERANCE 4.0
#define RMS_TOLERANCE 1.0
static double sample_diff(test_sample_t a, test_sample_t b) { return (double)a - (double)b; }
#endif

/* ---- test hook: evidence of which kernel path every frame really used ---- */
static int g_hook_log[MAX_LOG_FRAMES];
static int g_hook_count;
static int g_hook_scalar_frames;
static int g_hook_simd_frames;

void minimp3_backend_test_hook(const mp3dec_t *dec, int active_backend)
{
    (void)dec;
    if (g_hook_count < MAX_LOG_FRAMES)
        g_hook_log[g_hook_count++] = active_backend;
    if (active_backend == MP3DEC_BACKEND_SCALAR)
        g_hook_scalar_frames++;
    else if (active_backend == MP3DEC_BACKEND_SIMD)
        g_hook_simd_frames++;
}

static void hook_reset(void)
{
    g_hook_count = g_hook_scalar_frames = g_hook_simd_frames = 0;
}

/* ---- per-run captured stream facts ---- */
typedef struct
{
    test_sample_t *pcm;
    size_t capacity, samples;
    int frame_bytes[MAX_LOG_FRAMES];
    int channels[MAX_LOG_FRAMES];
    int hz[MAX_LOG_FRAMES];
    int frame_samples[MAX_LOG_FRAMES];
    int getter_log[MAX_LOG_FRAMES];
    int frames;
    int decoded_frames;
    int hook_scalar, hook_simd, set_failures;
} run_result_t;

static unsigned char *load_file(const char *path, long *size_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)size);
    if (!buf)
        exit(2);
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size)
        exit(2);
    fclose(fp);
    *size_out = size;
    return buf;
}

static void pcm_push(run_result_t *r, const test_sample_t *pcm, size_t n)
{
    if (r->samples + n > r->capacity)
    {
        while (r->samples + n > r->capacity)
            r->capacity = r->capacity ? r->capacity * 2 : 65536;
        r->pcm = (test_sample_t *)realloc(r->pcm, r->capacity * sizeof(test_sample_t));
        if (!r->pcm)
            exit(2);
    }
    memcpy(r->pcm + r->samples, pcm, n * sizeof(test_sample_t));
    r->samples += n;
}

/* schedule: backend requested before frame index i, or -1 to leave it alone */
typedef int (*schedule_fn)(int frame_index);

static int schedule_scalar(int i) { (void)i; return MP3DEC_BACKEND_SCALAR; }
static int schedule_simd(int i)   { (void)i; return MP3DEC_BACKEND_SIMD; }
static int schedule_alt(int i)    { return (i & 1) ? MP3DEC_BACKEND_SIMD : MP3DEC_BACKEND_SCALAR; }

static void run_frame_decode(const unsigned char *buf, long size, schedule_fn schedule,
                             int change_every_frame, run_result_t *r)
{
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    test_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    long pos = 0;
    int frame_index = 0;
    int decoded_frames = 0;

    memset(r, 0, sizeof(*r));
    hook_reset();
    mp3dec_init(&dec);
    /* fresh instance stores AUTO; getter resolves it to a concrete kernel */
    if (mp3dec_get_backend(&dec) != MP3DEC_BACKEND_SIMD &&
        mp3dec_get_backend(&dec) != MP3DEC_BACKEND_SCALAR)
    {
        fprintf(stderr, "fresh decoder did not resolve AUTO to a real kernel\n");
        exit(1);
    }
    while (pos < size)
    {
        int requested = schedule(frame_index);
        if (change_every_frame || frame_index == 0)
        {
            if (!mp3dec_set_backend(&dec, requested))
                r->set_failures++;
        }
        if (frame_index < MAX_LOG_FRAMES)
            r->getter_log[frame_index] = mp3dec_get_backend(&dec);
        int n = mp3dec_decode_frame(&dec, buf + pos, (int)(size - pos), pcm, &info);
        if (!info.frame_bytes)
            break;
        if (frame_index < MAX_LOG_FRAMES)
        {
            r->frame_bytes[frame_index] = info.frame_bytes;
            r->channels[frame_index] = info.channels;
            r->hz[frame_index] = info.hz;
            r->frame_samples[frame_index] = n;
        }
        if (n)
        {
            pcm_push(r, pcm, (size_t)n * info.channels);
            decoded_frames++;
        }
        pos += info.frame_bytes;
        frame_index++;
    }
    r->frames = frame_index;
    r->decoded_frames = decoded_frames;
    r->hook_scalar = g_hook_scalar_frames;
    r->hook_simd = g_hook_simd_frames;
    /* every decoded frame passes the hook; trailing byte-skip calls return
     * before the kernels are reached and are not instrumented */
    if (g_hook_count != decoded_frames || g_hook_scalar_frames + g_hook_simd_frames != decoded_frames)
    {
        fprintf(stderr, "hook coverage %d/%d decoded frames\n", g_hook_count, decoded_frames);
        exit(1);
    }
    /* getter and the instrumented kernel path must agree frame by frame, and
     * hook values must always name a real kernel */
    for (int i = 0; i < g_hook_count && i < frame_index; i++)
    {
        if (g_hook_log[i] != MP3DEC_BACKEND_SCALAR && g_hook_log[i] != MP3DEC_BACKEND_SIMD)
        {
            fprintf(stderr, "hook logged invalid backend %d at frame %d\n", g_hook_log[i], i);
            exit(1);
        }
        if (g_hook_log[i] != r->getter_log[i])
        {
            fprintf(stderr, "getter/hook disagree at frame %d: %d vs %d\n",
                    i, r->getter_log[i], g_hook_log[i]);
            exit(1);
        }
    }
}

static void check_metadata_equal(const run_result_t *a, const run_result_t *b, const char *tag)
{
    if (a->frames != b->frames || a->samples != b->samples)
    {
        fprintf(stderr, "%s: frame/sample count mismatch %d/%zu vs %d/%zu\n",
                tag, a->frames, a->samples, b->frames, b->samples);
        exit(1);
    }
    for (int i = 0; i < a->frames; i++)
    {
        if (a->frame_bytes[i] != b->frame_bytes[i] ||
            a->channels[i] != b->channels[i] ||
            a->hz[i] != b->hz[i] ||
            a->frame_samples[i] != b->frame_samples[i])
        {
            fprintf(stderr, "%s: metadata mismatch at frame %d\n", tag, i);
            exit(1);
        }
    }
}

static void check_pcm(const run_result_t *ref, const run_result_t *cand, const char *tag,
                      int *max_diff_out, double *rms_out)
{
    double sumsq = 0;
    double maxd = 0;
    if (ref->samples != cand->samples)
    {
        fprintf(stderr, "%s: sample count mismatch %zu vs %zu\n", tag, ref->samples, cand->samples);
        exit(1);
    }
    for (size_t i = 0; i < ref->samples; i++)
    {
        double d = sample_diff(ref->pcm[i], cand->pcm[i]);
        double ad = fabs(d);
        if (ad > maxd)
            maxd = ad;
        sumsq += d * d;
    }
    double rms = ref->samples ? sqrt(sumsq / ref->samples) : 0.0;
    *max_diff_out = (int)(maxd + 0.5);
    *rms_out = rms;
    if (maxd > ABS_TOLERANCE || rms >= RMS_TOLERANCE)
    {
        fprintf(stderr, "%s: tolerance exceeded max=%g rms=%g\n", tag, maxd, rms);
        exit(1);
    }
}

static void check_setter_contract(int scalar_ok, int simd_ok)
{
    mp3dec_t dec;
    mp3dec_init(&dec);
    if (!mp3dec_set_backend(&dec, MP3DEC_BACKEND_AUTO))
    {
        fprintf(stderr, "AUTO must always be accepted\n");
        exit(1);
    }
    if (mp3dec_set_backend(&dec, 99) || mp3dec_set_backend(&dec, -1))
    {
        fprintf(stderr, "illegal backend values must be rejected\n");
        exit(1);
    }

    int got_scalar = mp3dec_set_backend(&dec, MP3DEC_BACKEND_SCALAR);
    int after_scalar = mp3dec_get_backend(&dec);
    int got_simd = mp3dec_set_backend(&dec, MP3DEC_BACKEND_SIMD);
    int after_simd = mp3dec_get_backend(&dec);

    if (got_scalar != scalar_ok || got_simd != simd_ok)
    {
        fprintf(stderr, "capability mismatch scalar=%d(exp %d) simd=%d(exp %d)\n",
                got_scalar, scalar_ok, got_simd, simd_ok);
        exit(1);
    }
    if (scalar_ok && after_scalar != MP3DEC_BACKEND_SCALAR)
    {
        fprintf(stderr, "scalar selection did not take effect\n");
        exit(1);
    }
    if (simd_ok && after_simd != MP3DEC_BACKEND_SIMD)
    {
        fprintf(stderr, "simd selection did not take effect\n");
        exit(1);
    }
    /* rejected selection must leave the previous choice intact */
    if (!simd_ok)
    {
        mp3dec_set_backend(&dec, MP3DEC_BACKEND_SCALAR);
        int before = mp3dec_get_backend(&dec);
        if (mp3dec_set_backend(&dec, MP3DEC_BACKEND_SIMD))
        {
            fprintf(stderr, "unsupported SIMD unexpectedly accepted\n");
            exit(1);
        }
        if (mp3dec_get_backend(&dec) != before)
        {
            fprintf(stderr, "failed setter changed the backend choice\n");
            exit(1);
        }
    }
    if (!scalar_ok)
    {
        mp3dec_set_backend(&dec, MP3DEC_BACKEND_SIMD);
        int before = mp3dec_get_backend(&dec);
        if (mp3dec_set_backend(&dec, MP3DEC_BACKEND_SCALAR))
        {
            fprintf(stderr, "unsupported SCALAR unexpectedly accepted\n");
            exit(1);
        }
        if (mp3dec_get_backend(&dec) != before)
        {
            fprintf(stderr, "failed setter changed the backend choice\n");
            exit(1);
        }
    }
    /* public mp3dec_init restores AUTO: force a non-AUTO choice first, then
     * the getter must resolve back to the fastest path of this build/CPU */
    if (scalar_ok)
        mp3dec_set_backend(&dec, MP3DEC_BACKEND_SCALAR);
    else
        mp3dec_set_backend(&dec, MP3DEC_BACKEND_SIMD);
    mp3dec_init(&dec);
    int expected_auto = simd_ok ? MP3DEC_BACKEND_SIMD : MP3DEC_BACKEND_SCALAR;
    if (mp3dec_get_backend(&dec) != expected_auto)
    {
        fprintf(stderr, "public mp3dec_init must restore AUTO (resolved path %d expected %d)\n",
                mp3dec_get_backend(&dec), expected_auto);
        exit(1);
    }
}

/* resync on garbage and truncated input must not touch the backend choice */
static void check_resync_keeps_backend(const unsigned char *buf, long size)
{
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    test_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    static const unsigned char garbage[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };

    mp3dec_init(&dec);
    int can_scalar = mp3dec_set_backend(&dec, MP3DEC_BACKEND_SCALAR);
    int wanted = can_scalar ? MP3DEC_BACKEND_SCALAR : MP3DEC_BACKEND_SIMD;
    if (!can_scalar && !mp3dec_set_backend(&dec, MP3DEC_BACKEND_SIMD))
    {
        fprintf(stderr, "no usable backend for resync test\n");
        exit(1);
    }
    int before = mp3dec_get_backend(&dec);
    if (before != wanted)
    {
        fprintf(stderr, "resync test setup failed\n");
        exit(1);
    }
    /* no valid frame: decode_frame takes the memset resync path */
    if (mp3dec_decode_frame(&dec, garbage, (int)sizeof(garbage), pcm, &info) != 0)
    {
        fprintf(stderr, "garbage unexpectedly decoded\n");
        exit(1);
    }
    if (mp3dec_get_backend(&dec) != wanted)
    {
        fprintf(stderr, "resync reset the backend choice\n");
        exit(1);
    }
    /* stream continues on the same path, hook evidence included */
    hook_reset();
    int n = mp3dec_decode_frame(&dec, buf, (int)size, pcm, &info);
    if (!n || !info.frame_bytes)
    {
        fprintf(stderr, "stream did not recover after resync\n");
        exit(1);
    }
    if (mp3dec_get_backend(&dec) != wanted || g_hook_count != 1 || g_hook_log[0] != wanted)
    {
        fprintf(stderr, "post-resync path not preserved (hook=%d count=%d)\n",
                g_hook_count ? g_hook_log[0] : -1, g_hook_count);
        exit(1);
    }
}

/* frames with bad side-info take the internal history-reset branch; the
 * user's backend choice must still survive every such rejection */
static void check_bad_sideinfo_keeps_backend(const char *path)
{
    long size;
    unsigned char *buf = load_file(path, &size);
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    test_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    long pos = 0;
    int rejected = 0, decoded = 0;

    mp3dec_init(&dec);
    int can_scalar = mp3dec_set_backend(&dec, MP3DEC_BACKEND_SCALAR);
    int wanted = can_scalar ? MP3DEC_BACKEND_SCALAR : MP3DEC_BACKEND_SIMD;
    if (!can_scalar && !mp3dec_set_backend(&dec, MP3DEC_BACKEND_SIMD))
    {
        fprintf(stderr, "no usable backend for side-info test\n");
        exit(1);
    }
    hook_reset();
    while (pos < size)
    {
        int n = mp3dec_decode_frame(&dec, buf + pos, (int)(size - pos), pcm, &info);
        if (!info.frame_bytes)
            break;
        if (mp3dec_get_backend(&dec) != wanted)
        {
            fprintf(stderr, "bad side-info reset changed the backend choice\n");
            exit(1);
        }
        if (n)
            decoded++;
        else
            rejected++;
        pos += info.frame_bytes;
    }
    if (!rejected)
    {
        fprintf(stderr, "%s did not exercise the bad side-info branch\n", path);
        exit(1);
    }
    for (int i = 0; i < g_hook_count; i++)
    {
        if (g_hook_log[i] != wanted)
        {
            fprintf(stderr, "kernel path changed around bad side-info frame\n");
            exit(1);
        }
    }
    printf("%s rejected=%d decoded=%d backend_kept=%s\n", path, rejected, decoded,
           wanted == MP3DEC_BACKEND_SIMD ? "SIMD" : "SCALAR");
    free(buf);
}

/* two instances interleaved, including per-frame switches, stay independent */
static void check_instance_isolation(const unsigned char *buf, long size,
                                     int scalar_ok, int simd_ok)
{
    if (!(scalar_ok && simd_ok))
        return; /* isolation of distinct paths only meaningful with both */
    mp3dec_t a, b;
    mp3dec_frame_info_t ia, ib;
    test_sample_t pa[MINIMP3_MAX_SAMPLES_PER_FRAME], pb[MINIMP3_MAX_SAMPLES_PER_FRAME];
    long pa_pos = 0, pb_pos = 0;
    int frame = 0;
    int want_a = MP3DEC_BACKEND_SCALAR, want_b = MP3DEC_BACKEND_SIMD;

    mp3dec_init(&a);
    mp3dec_init(&b);
    mp3dec_set_backend(&a, want_a);
    mp3dec_set_backend(&b, want_b);
    while (pa_pos < size && pb_pos < size)
    {
        int na = mp3dec_decode_frame(&a, buf + pa_pos, (int)(size - pa_pos), pa, &ia);
        int nb = mp3dec_decode_frame(&b, buf + pb_pos, (int)(size - pb_pos), pb, &ib);
        if (!ia.frame_bytes || !ib.frame_bytes)
            break;
        if (mp3dec_get_backend(&a) != want_a || mp3dec_get_backend(&b) != want_b)
        {
            fprintf(stderr, "instance backends leaked across instances at frame %d\n", frame);
            exit(1);
        }
        if (ia.frame_bytes != ib.frame_bytes || ia.channels != ib.channels || ia.hz != ib.hz || na != nb)
        {
            fprintf(stderr, "interleaved instances diverged in metadata\n");
            exit(1);
        }
        pa_pos += ia.frame_bytes;
        pb_pos += ib.frame_bytes;
        frame++;
        /* swap the choice on instance a only; instance b must not move */
        if ((frame & 1) == 0)
        {
            want_a = (frame & 2) ? MP3DEC_BACKEND_SIMD : MP3DEC_BACKEND_SCALAR;
            if (!mp3dec_set_backend(&a, want_a))
            {
                fprintf(stderr, "instance isolation switch rejected\n");
                exit(1);
            }
        }
        if (mp3dec_get_backend(&b) != MP3DEC_BACKEND_SIMD)
        {
            fprintf(stderr, "instance b backend changed from another instance's switch\n");
            exit(1);
        }
    }
}

/* extended API: AUTO at open, set on mp3d, buffered reads, seek keeps choice */
static void check_extended_api(const unsigned char *buf, long size, int scalar_ok, int simd_ok)
{
    mp3dec_ex_t ref, tst;
    if (mp3dec_ex_open_buf(&ref, buf, (size_t)size, MP3D_SEEK_TO_SAMPLE) ||
        mp3dec_ex_open_buf(&tst, buf, (size_t)size, MP3D_SEEK_TO_SAMPLE))
    {
        fprintf(stderr, "mp3dec_ex_open_buf failed\n");
        exit(1);
    }
    /* open leaves the stored choice at AUTO; getter resolves to the fastest
     * path available in this build */
    int auto_path = simd_ok ? MP3DEC_BACKEND_SIMD : MP3DEC_BACKEND_SCALAR;
    if (mp3dec_get_backend(&ref.mp3d) != auto_path ||
        mp3dec_get_backend(&tst.mp3d) != auto_path)
    {
        fprintf(stderr, "extended open must start at AUTO\n");
        exit(1);
    }

    int ref_backend = scalar_ok ? MP3DEC_BACKEND_SCALAR : MP3DEC_BACKEND_SIMD;
    if (!mp3dec_set_backend(&ref.mp3d, ref_backend) ||
        !mp3dec_set_backend(&tst.mp3d, ref_backend))
    {
        fprintf(stderr, "extended backend selection failed\n");
        exit(1);
    }

    /* full reference read through the extended API */
    run_result_t rr;
    memset(&rr, 0, sizeof(rr));
    test_sample_t chunk[4096];
    for (;;)
    {
        size_t got = mp3dec_ex_read(&ref, chunk, sizeof(chunk) / sizeof(chunk[0]));
        if (!got)
        {
            if (ref.last_error)
            {
                fprintf(stderr, "reference extended read error %d\n", ref.last_error);
                exit(1);
            }
            break;
        }
        pcm_push(&rr, chunk, got);
    }

    /* small odd-sized reads, toggling the backend between calls: buffered PCM
     * must be neither re-decoded nor discarded, samples stay contiguous */
    size_t idx = 0;
    size_t partial = rr.samples / 4;
    static const size_t read_sizes[] = { 101, 37, 256, 7, 512 };
    int si = 0;
    while (idx < partial)
    {
        size_t want = read_sizes[si++ % (sizeof(read_sizes) / sizeof(read_sizes[0]))];
        if (want > partial - idx)
            want = partial - idx;
        size_t got = mp3dec_ex_read(&tst, chunk, want);
        if (got != want)
        {
            fprintf(stderr, "extended short read returned %zu of %zu\n", got, want);
            exit(1);
        }
        for (size_t k = 0; k < got; k++)
        {
            if (sample_diff(rr.pcm[idx + k], chunk[k]) > ABS_TOLERANCE)
            {
                fprintf(stderr, "buffered PCM changed after backend switch at %zu\n", idx + k);
                exit(1);
            }
        }
        idx += got;
        int toggle = (si & 1) ? MP3DEC_BACKEND_SCALAR : MP3DEC_BACKEND_SIMD;
        if (!mp3dec_set_backend(&tst.mp3d, toggle))
        {
            /* constrained build: choice must remain the supported one */
            if (mp3dec_get_backend(&tst.mp3d) != ref_backend)
            {
                fprintf(stderr, "failed extended setter changed choice\n");
                exit(1);
            }
        }
    }
    if (mp3dec_get_backend(&tst.mp3d) != ref_backend && !(scalar_ok && simd_ok))
    {
        fprintf(stderr, "extended choice changed in constrained build\n");
        exit(1);
    }

    /* seek clears history, backend choice must survive it */
    uint64_t seek_pos = rr.samples / 2;
    int before_seek = mp3dec_get_backend(&tst.mp3d);
    if (mp3dec_ex_seek(&tst, seek_pos))
    {
        fprintf(stderr, "mp3dec_ex_seek failed\n");
        exit(1);
    }
    if (mp3dec_get_backend(&tst.mp3d) != before_seek)
    {
        fprintf(stderr, "seek reset the backend choice\n");
        exit(1);
    }
    if (simd_ok && scalar_ok)
    {
        /* after seek the new choice applies to subsequent kernels */
        if (!mp3dec_set_backend(&tst.mp3d, MP3DEC_BACKEND_SIMD))
        {
            fprintf(stderr, "post-seek SIMD selection failed\n");
            exit(1);
        }
    }
    idx = (size_t)seek_pos;
    for (;;)
    {
        size_t got = mp3dec_ex_read(&tst, chunk, sizeof(chunk) / sizeof(chunk[0]));
        if (!got)
        {
            if (tst.last_error)
            {
                fprintf(stderr, "post-seek read error %d\n", tst.last_error);
                exit(1);
            }
            break;
        }
        if (idx + got > rr.samples)
        {
            fprintf(stderr, "post-seek read overrun\n");
            exit(1);
        }
        for (size_t k = 0; k < got; k++)
        {
            if (sample_diff(rr.pcm[idx + k], chunk[k]) > ABS_TOLERANCE)
            {
                fprintf(stderr, "post-seek PCM mismatch at %zu\n", idx + k);
                exit(1);
            }
        }
        idx += got;
    }
    if (idx != rr.samples)
    {
        fprintf(stderr, "post-seek stream length mismatch %zu vs %zu\n", idx, rr.samples);
        exit(1);
    }

    mp3dec_ex_close(&ref);
    mp3dec_ex_close(&tst);
    free(rr.pcm);
}

int main(void)
{
    static const char *vectors[] =
    {
        "vectors/l3-si_block.bit",
        "vectors/l3-compl.bit",
        "vectors/M2L3_noise.bit"
    };

    /* capabilities of this build, discovered through the public API */
    mp3dec_t probe;
    mp3dec_init(&probe);
    int auto_backend = mp3dec_get_backend(&probe);
    int scalar_ok = mp3dec_set_backend(&probe, MP3DEC_BACKEND_SCALAR);
    int simd_ok = mp3dec_set_backend(&probe, MP3DEC_BACKEND_SIMD);
    mp3dec_init(&probe);
    if (auto_backend != (simd_ok ? MP3DEC_BACKEND_SIMD : MP3DEC_BACKEND_SCALAR))
    {
        fprintf(stderr, "AUTO did not resolve to the fastest available path\n");
        return 1;
    }
    printf("config: auto_backend=%s scalar_supported=%d simd_supported=%d\n",
           auto_backend == MP3DEC_BACKEND_SIMD ? "SIMD" : "SCALAR", scalar_ok, simd_ok);

    check_setter_contract(scalar_ok, simd_ok);

    for (size_t v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++)
    {
        long size;
        unsigned char *buf = load_file(vectors[v], &size);
        run_result_t scalar_ref, simd_run, alt_run;

        /* continuous scalar reference (every build has the scalar path or
         * emulates this run through its sole backend) */
        if (scalar_ok)
            run_frame_decode(buf, size, schedule_scalar, 1, &scalar_ref);
        else
            run_frame_decode(buf, size, schedule_simd, 1, &scalar_ref);
        if (scalar_ref.set_failures)
        {
            fprintf(stderr, "reference run had setter failures\n");
            return 1;
        }

        if (simd_ok)
        {
            run_frame_decode(buf, size, schedule_simd, 1, &simd_run);
            if (simd_run.hook_scalar != 0 || simd_run.hook_simd != simd_run.decoded_frames)
            {
                fprintf(stderr, "SIMD run took scalar kernels: hook s=%d n=%d decoded=%d\n",
                        simd_run.hook_scalar, simd_run.hook_simd, simd_run.decoded_frames);
                return 1;
            }
        } else
        {
            /* constrained scalar build: SIMD requests must fail, path unchanged */
            run_frame_decode(buf, size, schedule_simd, 1, &simd_run);
            if (simd_run.set_failures != simd_run.frames)
            {
                fprintf(stderr, "expected every SIMD request to fail, got %d/%d\n",
                        simd_run.set_failures, simd_run.frames);
                return 1;
            }
            if (simd_run.hook_simd != 0 || simd_run.hook_scalar != simd_run.decoded_frames)
            {
                fprintf(stderr, "scalar-only build unexpectedly used SIMD kernels\n");
                return 1;
            }
        }

        /* per-frame alternating schedule: setter between every decode */
        run_frame_decode(buf, size, schedule_alt, 1, &alt_run);
        if (scalar_ok && simd_ok)
        {
            if (alt_run.hook_scalar == 0 || alt_run.hook_simd == 0 ||
                alt_run.hook_scalar + alt_run.hook_simd != alt_run.decoded_frames)
            {
                fprintf(stderr, "alternating run did not exercise both kernels: s=%d n=%d decoded=%d\n",
                        alt_run.hook_scalar, alt_run.hook_simd, alt_run.decoded_frames);
                return 1;
            }
            /* per-frame evidence: the hook must match the requested schedule */
            for (int i = 0; i < g_hook_count; i++)
            {
                if (g_hook_log[i] != schedule_alt(i))
                {
                    fprintf(stderr, "frame %d ran kernel %d, schedule requested %d\n",
                            i, g_hook_log[i], schedule_alt(i));
                    return 1;
                }
            }
        } else
        {
            /* every request for the missing path must have been rejected */
            if (alt_run.set_failures == 0)
            {
                fprintf(stderr, "alternating run never hit the unsupported-path branch\n");
                return 1;
            }
            int sole = simd_ok ? MP3DEC_BACKEND_SIMD : MP3DEC_BACKEND_SCALAR;
            for (int i = 0; i < g_hook_count; i++)
            {
                if (g_hook_log[i] != sole)
                {
                    fprintf(stderr, "constrained alternating run left its sole backend\n");
                    return 1;
                }
            }
        }

        check_metadata_equal(&scalar_ref, &simd_run, vectors[v]);
        check_metadata_equal(&scalar_ref, &alt_run, vectors[v]);

        int maxd1, maxd2;
        double rms1, rms2;
        check_pcm(&scalar_ref, &simd_run, vectors[v], &maxd1, &rms1);
        check_pcm(&scalar_ref, &alt_run, vectors[v], &maxd2, &rms2);

        printf("%s frames=%d samples=%zu ch=%d hz=%d | scalar_hook=%d simd_hook=%d"
               " alt_hook(s=%d,n=%d fails=%d) | vs_simd max=%d rms=%.4f vs_alt max=%d rms=%.4f\n",
               vectors[v], scalar_ref.frames, scalar_ref.samples,
               scalar_ref.channels[0], scalar_ref.hz[0],
               scalar_ref.hook_scalar, scalar_ref.hook_simd,
               alt_run.hook_scalar, alt_run.hook_simd, alt_run.set_failures,
               maxd1, rms1, maxd2, rms2);

        check_resync_keeps_backend(buf, size);
        check_instance_isolation(buf, size, scalar_ok, simd_ok);
        check_extended_api(buf, size, scalar_ok, simd_ok);

        free(buf);
        free(scalar_ref.pcm);
        free(simd_run.pcm);
        free(alt_run.pcm);
    }

    check_bad_sideinfo_keeps_backend("vectors/l3-nonstandard-sideinfo-size.bit");

    printf("MINIMP3_BACKEND_OK\n");
    return 0;
}
