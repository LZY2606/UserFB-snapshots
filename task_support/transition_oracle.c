/*
 * transition_oracle.c - independent reference for the chained
 * xfade / acrossfade regression.
 *
 * The reference is computed straight from the edit-list specification in
 * transition_spec.h: it never links FFmpeg and never runs the filters
 * under test.  It links only against libc and libm.
 *
 * Endpoint convention (shared with the xfade / acrossfade filters):
 *   in an overlap of length N, sample/frame k (0-based) uses
 *     tri : w = (N-1-k)/N for the first input,  k/N for a linear second
 *     qsin: quarter sine applied to the corresponding linear weight,
 *           qsin(w) = sin(w * pi / 2)
 *   video fade is a linear blend a*(N-k)/N + b*k/N.
 *
 * Sub-commands:
 *   expect      <25|30> <dir>                 write expected gray/f32/meta
 *   check-content <25|30> <gray> <f32>        verify pixels and PCM,
 *                                             first difference on stderr
 *   check-clock <25|30> <vtn> <vtd> <atn> <atd> <vpts> <apts>
 *                                             verify PTS/sample axes from
 *                                             "pts duration" packet lists
 *
 * Exit status is 0 on match, 1 on the first detected difference.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transition_spec.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VIDEO_TOLERANCE 1
#define AUDIO_TOLERANCE 1e-6

static float qsin_weight(double linear)
{
    return (float)sin(linear * M_PI / 2.0);
}

/* Expected 8-bit pixel of a finished-program frame, analytic blend. */
static uint8_t expected_pixel(const TransitionCase *c, int frame, int x, int y)
{
    int s0, f0, s1, f1, k, n, mixed;
    double v;

    mixed = ts_video_origin(c, frame, &s0, &f0, &s1, &f1, &k, &n);
    if (!mixed)
        return ts_source_pixel(c, s0, f0, x, y);

    v = (double)ts_source_pixel(c, s0, f0, x, y) * (double)(n - k) / n
      + (double)ts_source_pixel(c, s1, f1, x, y) * (double)k / n;
    return (uint8_t)lround(v);
}

/* Expected stereo sample of a finished-program sample frame. */
static void expected_sample(const TransitionCase *c, int sample, int channel,
                            float *out)
{
    int s0, p0, s1, p1, k, n, mixed;

    mixed = ts_audio_origin(c, sample, &s0, &p0, &s1, &p1, &k, &n);
    if (!mixed) {
        *out = ts_source_sample(c, s0, channel, p0);
        return;
    }

    const float a = ts_source_sample(c, s0, channel, p0);
    const float b = ts_source_sample(c, s1, channel, p1);
    const double w0 = (double)(n - 1 - k) / n;          /* tri  */
    const double w1 = qsin_weight((double)k / n);       /* qsin */
    *out = (float)(a * w0 + b * w1);
}

static int read_file(const char *path, void *buf, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ORACLE_ERROR cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (fread(buf, 1, len, f) != len) {
        fprintf(stderr, "ORACLE_ERROR short read on %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return -1;
    }
    sz = ftell(f);
    fclose(f);
    return sz;
}

static int cmd_expect(const TransitionCase *c, const char *dir)
{
    char path[1024];
    uint8_t *gray = malloc((size_t)c->total_frames * TS_PLANE_SIZE);
    float *pcm = malloc((size_t)c->total_samples * TS_CHANNELS *
                        sizeof(float));
    FILE *meta;
    int f, s, ch, x, y;

    if (!gray || !pcm) {
        fprintf(stderr, "ORACLE_ERROR out of memory\n");
        return 2;
    }

    for (f = 0; f < c->total_frames; f++)
        for (y = 0; y < TS_HEIGHT; y++)
            for (x = 0; x < TS_WIDTH; x++)
                gray[f * TS_PLANE_SIZE + y * TS_WIDTH + x] =
                    expected_pixel(c, f, x, y);

    for (s = 0; s < c->total_samples; s++)
        for (ch = 0; ch < TS_CHANNELS; ch++)
            expected_sample(c, s, ch, &pcm[s * TS_CHANNELS + ch]);

    snprintf(path, sizeof(path), "%s/expect.gray", dir);
    FILE *gf = fopen(path, "wb");
    if (!gf || fwrite(gray, 1, (size_t)c->total_frames * TS_PLANE_SIZE, gf)
             != (size_t)c->total_frames * TS_PLANE_SIZE) {
        fprintf(stderr, "ORACLE_ERROR cannot write %s\n", path);
        return 2;
    }
    fclose(gf);

    snprintf(path, sizeof(path), "%s/expect.f32", dir);
    FILE *af = fopen(path, "wb");
    if (!af || fwrite(pcm, sizeof(float),
                      (size_t)c->total_samples * TS_CHANNELS, af)
             != (size_t)c->total_samples * TS_CHANNELS) {
        fprintf(stderr, "ORACLE_ERROR cannot write %s\n", path);
        return 2;
    }
    fclose(af);

    snprintf(path, sizeof(path), "%s/expect.meta", dir);
    meta = fopen(path, "wb");
    if (!meta) {
        fprintf(stderr, "ORACLE_ERROR cannot write %s\n", path);
        return 2;
    }
    fprintf(meta,
            "case=%s total_frames=%d total_samples=%d plane=%dx%d "
            "channels=%d overlap_frames=%d overlap_samples=%d\n",
            c->name, c->total_frames, c->total_samples, TS_WIDTH,
            TS_HEIGHT, TS_CHANNELS, c->overlap_frames, c->overlap_samples);
    fclose(meta);

    free(gray);
    free(pcm);
    printf("ORACLE_EXPECT case=%s frames=%d samples=%d dir=%s\n",
           c->name, c->total_frames, c->total_samples, dir);
    return 0;
}

static void video_origin_str(const TransitionCase *c, int frame,
                             char *buf, size_t len)
{
    int s0, f0, s1, f1, k, n, mixed;
    mixed = ts_video_origin(c, frame, &s0, &f0, &s1, &f1, &k, &n);
    if (mixed)
        snprintf(buf, len, "video frame=%d origin=seg%d:frame%d+seg%d:frame%d k=%d n=%d",
                 frame, s0, f0, s1, f1, k, n);
    else
        snprintf(buf, len, "video frame=%d origin=seg%d:frame%d",
                 frame, s0, f0);
}

static void audio_origin_str(const TransitionCase *c, int sample,
                             char *buf, size_t len)
{
    int s0, p0, s1, p1, k, n, mixed;
    mixed = ts_audio_origin(c, sample, &s0, &p0, &s1, &p1, &k, &n);
    if (mixed)
        snprintf(buf, len, "audio sample=%d origin=seg%d:sample%d+seg%d:sample%d k=%d n=%d",
                 sample, s0, p0, s1, p1, k, n);
    else
        snprintf(buf, len, "audio sample=%d origin=seg%d:sample%d",
                 sample, s0, p0);
}

static int cmd_check_content(const TransitionCase *c,
                             const char *gray_path, const char *f32_path)
{
    const size_t vbytes = (size_t)c->total_frames * TS_PLANE_SIZE;
    const size_t abytes = (size_t)c->total_samples * TS_CHANNELS *
                          sizeof(float);
    uint8_t *got_v, *exp_v;
    float *got_a, *exp_a;
    long sz;
    int i;

    sz = file_size(gray_path);
    if (sz < 0 || (size_t)sz != vbytes) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=length stream=video got=%ld expect=%zu\n",
                sz, vbytes);
        return 1;
    }
    sz = file_size(f32_path);
    if (sz < 0 || (size_t)sz != abytes) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=length stream=audio got=%ld expect=%zu\n",
                sz, abytes);
        return 1;
    }

    got_v = malloc(vbytes);
    exp_v = malloc(vbytes);
    got_a = malloc(abytes);
    exp_a = malloc(abytes);
    if (!got_v || !exp_v || !got_a || !exp_a) {
        fprintf(stderr, "ORACLE_ERROR out of memory\n");
        return 2;
    }

    if (read_file(gray_path, got_v, vbytes) ||
        read_file(f32_path, got_a, abytes))
        return 2;
    for (i = 0; i < c->total_frames; i++)
        for (int p = 0; p < TS_PLANE_SIZE; p++)
            exp_v[i * TS_PLANE_SIZE + p] =
                expected_pixel(c, i, p % TS_WIDTH, p / TS_WIDTH);
    for (i = 0; i < c->total_samples * TS_CHANNELS; i++)
        expected_sample(c, i / TS_CHANNELS, i % TS_CHANNELS, &exp_a[i]);

    for (i = 0; i < (int)vbytes; i++) {
        int d = (int)got_v[i] - (int)exp_v[i];
        if (abs(d) > VIDEO_TOLERANCE) {
            int frame = i / TS_PLANE_SIZE;
            int rem = i % TS_PLANE_SIZE;
            char origin[160];
            video_origin_str(c, frame, origin, sizeof(origin));
            fprintf(stderr,
                    "ORACLE_MISMATCH kind=pixel %s x=%d y=%d got=%u expect=%u\n",
                    origin, rem % TS_WIDTH, rem / TS_WIDTH,
                    got_v[i], exp_v[i]);
            return 1;
        }
    }

    for (i = 0; i < c->total_samples * TS_CHANNELS; i++) {
        float d = got_a[i] - exp_a[i];
        if (fabsf(d) > AUDIO_TOLERANCE) {
            int sample = i / TS_CHANNELS;
            int channel = i % TS_CHANNELS;
            char origin[160];
            audio_origin_str(c, sample, origin, sizeof(origin));
            fprintf(stderr,
                    "ORACLE_MISMATCH kind=pcm %s channel=%s got=%.9g expect=%.9g\n",
                    origin, channel ? "right" : "left",
                    got_a[i], exp_a[i]);
            return 1;
        }
    }

    free(got_v); free(exp_v); free(got_a); free(exp_a);
    printf("ORACLE_CONTENT_OK case=%s frames=%d samples=%d\n",
           c->name, c->total_frames, c->total_samples);
    return 0;
}

/* Read a "pts duration" packet list produced by ffprobe csv parsing. */
static int read_packets(const char *path, long long **pts,
                        long long **dur, int *count)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int n = 0, cap = 0;

    if (!f) {
        fprintf(stderr, "ORACLE_ERROR cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    *pts = NULL; *dur = NULL;
    while (fgets(line, sizeof(line), f)) {
        long long p, d;
        if (sscanf(line, "%lld,%lld", &p, &d) != 2)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            *pts = realloc(*pts, (size_t)cap * sizeof(long long));
            *dur = realloc(*dur, (size_t)cap * sizeof(long long));
            if (!*pts || !*dur)
                return -1;
        }
        (*pts)[n] = p;
        (*dur)[n] = d;
        n++;
    }
    fclose(f);
    *count = n;
    return 0;
}

static int cmd_check_clock(const TransitionCase *c,
                           long long vtn, long long vtd,
                           long long atn, long long atd,
                           const char *vpts_path, const char *apts_path)
{
    long long *vpts, *vdur, *apts, *adur;
    int nv, na, i;
    long long vstep, cursor, samples;

    if (read_packets(vpts_path, &vpts, &vdur, &nv) ||
        read_packets(apts_path, &apts, &adur, &na)) {
        fprintf(stderr, "ORACLE_ERROR cannot read packet lists\n");
        return 2;
    }

    if (nv != c->total_frames) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=clock stream=video packet_count=%d expect_frames=%d\n",
                nv, c->total_frames);
        return 1;
    }

    /* One frame in stream time-base ticks; must be an exact integer. */
    vstep = vtd * c->fps_den / c->fps_num;
    if (vstep * c->fps_num != vtd * c->fps_den) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=clock stream=video reason=nonintegral_step tb=%lld/%lld fps=%d/%d\n",
                vtn, vtd, c->fps_num, c->fps_den);
        return 1;
    }
    for (i = 0; i < nv; i++) {
        long long expect = (long long)i * vstep;
        if (vpts[i] != expect) {
            fprintf(stderr,
                    "ORACLE_MISMATCH kind=clock stream=video packet=%d got_pts=%lld expect_pts=%lld tb=%lld/%lld\n",
                    i, vpts[i], expect, vtn, vtd);
            return 1;
        }
        if (vdur[i] != vstep) {
            fprintf(stderr,
                    "ORACLE_MISMATCH kind=clock stream=video packet=%d got_dur=%lld expect_dur=%lld\n",
                    i, vdur[i], vstep);
            return 1;
        }
    }

    if (na < 1) {
        fprintf(stderr, "ORACLE_MISMATCH kind=clock stream=audio no_packets\n");
        return 1;
    }
    if (atn != 1 || atd != TS_SAMPLE_RATE) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=clock stream=audio tb=%lld/%lld expect=1/%d\n",
                atn, atd, TS_SAMPLE_RATE);
        return 1;
    }
    cursor = 0;
    samples = 0;
    for (i = 0; i < na; i++) {
        if (apts[i] != cursor) {
            fprintf(stderr,
                    "ORACLE_MISMATCH kind=clock stream=audio packet=%d got_pts=%lld expect_pts=%lld tb=%lld/%lld\n",
                    i, apts[i], cursor, atn, atd);
            return 1;
        }
        if (adur[i] <= 0) {
            fprintf(stderr,
                    "ORACLE_MISMATCH kind=clock stream=audio packet=%d bad_duration=%lld\n",
                    i, adur[i]);
            return 1;
        }
        cursor += adur[i];
        samples += adur[i] * atn; /* samples = ticks * tb_num when tb=1/rate */
    }
    if (samples != c->total_samples) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=clock stream=audio total_samples=%lld expect=%d\n",
                samples, c->total_samples);
        return 1;
    }

    free(vpts); free(vdur); free(apts); free(adur);
    printf("ORACLE_CLOCK_OK case=%s video_packets=%d audio_packets=%d samples=%lld\n",
           c->name, nv, na, samples);
    return 0;
}

/*
 * Structural self-check of the timeline: every region (before / inside /
 * between / inside / after the two overlaps) must contain the expected
 * number of entries, the first entry must come from shot 0 and the final
 * frame/sample from shot 2.  Runs without any FFmpeg output.
 */
static int selftest_axis(const TransitionCase *c, int audio)
{
    const int L = audio ? c->audio_per_seg : c->frames_per_seg;
    const int n = audio ? c->overlap_samples : c->overlap_frames;
    const int total = audio ? c->total_samples : c->total_frames;
    int mixed_count = 0, last_seg0 = -1;
    int saw_pure_between = 0, i;

    if (total != 3 * L - 2 * n) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=selftest %s total=%d expect=%d\n",
                audio ? "audio" : "video", total, 3 * L - 2 * n);
        return 1;
    }

    for (i = 0; i < total; i++) {
        int s0, p0, s1, p1, k, nn, mixed;
        if (audio)
            mixed = ts_audio_origin(c, i, &s0, &p0, &s1, &p1, &k, &nn);
        else
            mixed = ts_video_origin(c, i, &s0, &p0, &s1, &p1, &k, &nn);

        if (mixed) {
            if (nn != n || k < 0 || k >= n) {
                fprintf(stderr,
                        "ORACLE_MISMATCH kind=selftest %s index=%d bad k=%d n=%d\n",
                        audio ? "audio" : "video", i, k, nn);
                return 1;
            }
            mixed_count++;
        } else if (mixed_count > 0 && mixed_count < 2 * n) {
            saw_pure_between = 1;       /* pure region between overlaps */
        }
        last_seg0 = s0;
    }

    if (mixed_count != 2 * n) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=selftest %s mixed=%d expect=%d\n",
                audio ? "audio" : "video", mixed_count, 2 * n);
        return 1;
    }
    if (!saw_pure_between) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=selftest %s missing pure middle region\n",
                audio ? "audio" : "video");
        return 1;
    }
    if (last_seg0 != 2) {
        fprintf(stderr,
                "ORACLE_MISMATCH kind=selftest %s last_seg=%d expect=2\n",
                audio ? "audio" : "video", last_seg0);
        return 1;
    }
    return 0;
}

static int cmd_selftest(const TransitionCase *c)
{
    if (selftest_axis(c, 0) || selftest_axis(c, 1))
        return 1;
    printf("ORACLE_SELFTEST_OK case=%s frames=%d samples=%d\n",
           c->name, c->total_frames, c->total_samples);
    return 0;
}

int main(int argc, char **argv)
{
    const TransitionCase *c;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s expect <25|30> <dir>\n"
                "       %s check-content <25|30> <gray> <f32>\n"
                "       %s check-clock <25|30> <vtn> <vtd> <atn> <atd> <vpts> <apts>\n"
                "       %s selftest <25|30>\n",
                argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    c = ts_find(atoi(argv[2]));
    if (!c) {
        fprintf(stderr, "ORACLE_ERROR unknown case id '%s'\n", argv[2]);
        return 2;
    }

    if (!strcmp(argv[1], "expect") && argc == 4)
        return cmd_expect(c, argv[3]);
    if (!strcmp(argv[1], "check-content") && argc == 5)
        return cmd_check_content(c, argv[3], argv[4]);
    if (!strcmp(argv[1], "check-clock") && argc == 9)
        return cmd_check_clock(c, atoll(argv[3]), atoll(argv[4]),
                               atoll(argv[5]), atoll(argv[6]),
                               argv[7], argv[8]);
    if (!strcmp(argv[1], "selftest") && argc == 3)
        return cmd_selftest(c);

    fprintf(stderr, "ORACLE_ERROR bad arguments\n");
    return 2;
}
