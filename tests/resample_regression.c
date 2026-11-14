/*
 *  Regression tests for the TwoLAME streaming resampler and its
 *  integration with the four encoding front ends.
 *
 *  Groups:
 *    clock     - output schedule, logical lengths, flush and counting
 *    filter    - band-limited response measured on real core output
 *    frontends - all four PCM entry points and chunk invariance
 *    legacy    - same-rate path, CRC and partial frame behaviour
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "twolame.h"
#include "resample.h"
#include "check.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BIG_BUF (1 << 20)

static unsigned long long ceil_div_mul(unsigned long long n, int out_rate, int in_rate)
{
    return (n * (unsigned long long) out_rate + (unsigned long long) in_rate - 1)
        / (unsigned long long) in_rate;
}

static twolame_options *make_opts(int in_rate, int out_rate, int channels, int crc)
{
    twolame_options *o = twolame_init();
    if (!o)
        return NULL;
    twolame_set_num_channels(o, channels);
    twolame_set_in_samplerate(o, in_rate);
    twolame_set_out_samplerate(o, out_rate);
    twolame_set_bitrate(o, channels == 1 ? 64 : 192);
    twolame_set_mode(o, channels == 1 ? TWOLAME_MONO : TWOLAME_STEREO);
    twolame_set_psymodel(o, 3);
    twolame_set_error_protection(o, crc ? TRUE : FALSE);
    twolame_set_verbosity(o, 0);
    return o;
}

/* Run the resampler core over planar input, optionally split into
 * single samples and irregular blocks. Gathered output is written into
 * out[ch] (caller provides capacity). */
static unsigned long long core_run(int in_rate, int out_rate, int channels,
                                   const short *left, const short *right, int n,
                                   short *out[2], int chunked)
{
    twolame_resampler *rs = twolame_resampler_create(in_rate, out_rate, channels);
    const int pattern[] = { 1, 37, 2049, 19, 3, 128 };
    unsigned long long total = 0;
    int pos = 0, k = 0;

    if (!rs)
        return 0ULL;

    while (pos < n) {
        int blk = chunked ? pattern[k++ % (int) (sizeof(pattern) / sizeof(pattern[0]))] : n;
        const short *in[2];
        short *part[2];
        int got = 0, ch;

        if (blk > n - pos)
            blk = n - pos;
        in[0] = left + pos;
        in[1] = right + pos;
        if (twolame_resampler_process(rs, in, blk, part, &got) != 0) {
            twolame_resampler_destroy(rs);
            return 0ULL;
        }
        for (ch = 0; ch < channels; ch++) {
            memcpy(out[ch] + total, part[ch], (size_t) got * sizeof(short));
        }
        total += (unsigned long long) got;
        pos += blk;
    }

    {
        short *part[2];
        int got = 0, ch;
        if (twolame_resampler_flush(rs, part, &got) != 0) {
            twolame_resampler_destroy(rs);
            return 0ULL;
        }
        for (ch = 0; ch < channels; ch++)
            memcpy(out[ch] + total, part[ch], (size_t) got * sizeof(short));
        total += (unsigned long long) got;

        /* a second flush must produce nothing */
        if (twolame_resampler_flush(rs, part, &got) != 0 || got != 0) {
            twolame_resampler_destroy(rs);
            return 0ULL;
        }
        if (twolame_resampler_count(rs) != total) {
            twolame_resampler_destroy(rs);
            return 0ULL;
        }
    }

    twolame_resampler_destroy(rs);
    return total;
}

/* Parse an MP2 stream: validate every frame header and return the frame
 * count, failing (negative) if the stream does not end on a frame. */
static int count_mp2_frames(const unsigned char *p, int n, int expect_rate)
{
    static const int kb1[] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192,
                               224, 256, 320, 384, 0
                             };
    static const int kb2[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96,
                               112, 128, 144, 160, 0
                             };
    static const int sr1[] = { 44100, 48000, 32000, 0 };
    static const int sr2[] = { 22050, 24000, 16000, 0 };
    int pos = 0, frames = 0;

    while (pos < n) {
        int mpeg1, rate, len, bi;
        if (pos + 4 > n || p[pos] != 0xFF)
            return -1;
        /* Layer II code is binary 10 in the layer bits; mask off the
         * version (ID) and protection bits to accept MPEG-1/2 with or
         * without CRC: 0xFC/0xFD (MPEG-1) and 0xF4/0xF5 (MPEG-2) */
        if ((p[pos + 1] & 0xF6) != 0xF4)
            return -1;
        mpeg1 = (p[pos + 1] >> 3) & 1;
        if (mpeg1)
            rate = sr1[(p[pos + 2] >> 2) & 3];
        else
            rate = sr2[(p[pos + 2] >> 2) & 3];
        if (rate != expect_rate)
            return -2;
        bi = p[pos + 2] >> 4;
        if (bi == 0 || bi == 15)
            return -3;
        /* twolame always packs 1152 samples per frame, including MPEG-2
         * LSF output, so the 144000 numerator applies to both versions */
        len = 144000 * (mpeg1 ? kb1[bi] : kb2[bi]) / rate
              + ((p[pos + 2] >> 1) & 1);
        if (len < 4 || pos + len > n)
            return -4;
        pos += len;
        frames++;
    }
    return frames;
}

static short gen_sine(int i, double freq, int rate, int amp)
{
    return (short) lrint(amp * sin(2.0 * M_PI * freq * (double) i / (double) rate));
}

/* Least-squares amplitude of a sine at freq inside a short buffer. */
static double tone_amplitude(const short *y, int start, int end, double freq, int rate)
{
    double s = 0.0, c = 0.0;
    int i, m = end - start;

    for (i = start; i < end; i++) {
        double ph = 2.0 * M_PI * freq * (double) i / (double) rate;
        s += (double) y[i] * sin(ph);
        c += (double) y[i] * cos(ph);
    }
    return 2.0 * hypot(s, c) / (double) m;
}

/***************************** clock *********************************/

static int run_clock_lengths(void)
{
    static const int ns[] = { 0, 1, 7, 1153, 4096, 100000 };
    int pairs[][2] = { {48000, 44100}, {44100, 48000}, {48000, 16000}, {8000, 48000},
        {96000, 22050}
    };
    size_t i, j;

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        for (j = 0; j < sizeof(ns) / sizeof(ns[0]); j++) {
            int n = ns[j];
            short *l = calloc((size_t) (n ? n : 1), sizeof(short));
            short *r = calloc((size_t) (n ? n : 1), sizeof(short));
            int cap = (int) ceil_div_mul((unsigned long long) n, pairs[i][1], pairs[i][0]) + 64;
            short *ol = calloc((size_t) cap, sizeof(short));
            short *or_ = calloc((size_t) cap, sizeof(short));
            short *out[2] = { ol, or_ };
            unsigned long long got, expect;
            int q;

            TASK_CHECK(l && r && ol && or_);
            for (q = 0; q < n; q++) {
                l[q] = (short) ((q * 193) % 40001 - 20000);
                r[q] = (short) ((q * 127) % 30001 - 15000);
            }
            expect = ceil_div_mul((unsigned long long) n, pairs[i][1], pairs[i][0]);
            got = core_run(pairs[i][0], pairs[i][1], 2, l, r, n, out, 0);
            TASK_CHECK(got == expect);
            free(l);
            free(r);
            free(ol);
            free(or_);
        }
    }
    return 0;
}

static int run_clock_defer_and_reflush(void)
{
    twolame_resampler *rs = twolame_resampler_create(48000, 44100, 1);
    short in[1] = { 10000 };
    const short *inp[1] = { in };
    short *out[2];
    int got = 0;
    unsigned long long before;
    int j;

    TASK_CHECK(rs != NULL);
    /* a single sample cannot produce anything: the filter tail needs
     * future input, so output is withheld */
    TASK_CHECK(twolame_resampler_process(rs, inp, 1, out, &got) == 0);
    TASK_CHECK(got == 0);

    for (j = 0; j < 63; j++)
        TASK_CHECK(twolame_resampler_process(rs, inp, 1, out, &got) == 0);

    before = twolame_resampler_count(rs);
    TASK_CHECK(before > 0);
    TASK_CHECK(twolame_resampler_flush(rs, out, &got) == 0);
    TASK_CHECK(got > 0);
    TASK_CHECK(twolame_resampler_count(rs) == before + (unsigned long long) got);
    /* repeated flush is silent and leaves the count untouched */
    {
        unsigned long long after = twolame_resampler_count(rs);
        TASK_CHECK(twolame_resampler_flush(rs, out, &got) == 0 && got == 0);
        TASK_CHECK(twolame_resampler_count(rs) == after);
    }
    twolame_resampler_destroy(rs);
    return 0;
}

static int run_clock_getter(void)
{
    int pairs[][2] = { {48000, 44100}, {44100, 48000} };
    size_t i;

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        const int n = 50003;
        short *pcm = calloc((size_t) n, sizeof(short));
        unsigned char *mp2 = malloc(BIG_BUF);
        twolame_options *o = make_opts(pairs[i][0], pairs[i][1], 1, 0);
        unsigned long long expect;
        int pos = 0, bytes = 0, q;
        const int sizes[] = { 1, 37, 2049, 19 };

        TASK_CHECK(pcm && mp2 && o);
        for (q = 0; q < n; q++)
            pcm[q] = gen_sine(q, 1000.0, pairs[i][0], 20000);
        TASK_CHECK(twolame_init_params(o) == 0);
        while (pos < n) {
            int blk = sizes[(pos / 1) % 4];
            int b;
            if (blk > n - pos)
                blk = n - pos;
            b = twolame_encode_buffer(o, pcm + pos, NULL, blk, mp2 + bytes, BIG_BUF - bytes);
            TASK_CHECK(b >= 0);
            bytes += b;
            pos += blk;
        }
        {
            int b = twolame_encode_flush(o, mp2 + bytes, BIG_BUF - bytes);
            TASK_CHECK(b >= 0);
            bytes += b;
            TASK_CHECK(twolame_encode_flush(o, mp2 + bytes, BIG_BUF - bytes) == 0);
        }
        expect = ceil_div_mul((unsigned long long) n, pairs[i][1], pairs[i][0]);
        /* the count covers the logical interval only, never frame padding */
        TASK_CHECK(twolame_get_resampled_samples(o) == expect);
        TASK_CHECK(count_mp2_frames(mp2, bytes, pairs[i][1]) >= 0);
        twolame_close(&o);
        free(pcm);
        free(mp2);
    }
    return 0;
}

static int run_clock_empty(void)
{
    twolame_options *o = make_opts(48000, 44100, 2, 0);
    unsigned char mp2[128];
    short pcm[2] = { 1, 2 };

    TASK_CHECK(o);
    TASK_CHECK(twolame_init_params(o) == 0);
    TASK_CHECK(twolame_encode_buffer_interleaved(o, pcm, 0, mp2, sizeof(mp2)) == 0);
    TASK_CHECK(twolame_encode_buffer_float32(o, NULL, NULL, 0, mp2, sizeof(mp2)) == 0);
    TASK_CHECK(twolame_encode_flush(o, mp2, sizeof(mp2)) == 0);
    TASK_CHECK(twolame_encode_flush(o, mp2, sizeof(mp2)) == 0);
    TASK_CHECK(twolame_get_resampled_samples(o) == 0);
    twolame_close(&o);
    return 0;
}

/***************************** filter ********************************/

#define FILTER_N 96000      /* 2 s of 48 kHz input */

static int run_filter_passband(void)
{
    short *in = calloc(FILTER_N, sizeof(short));
    short *gather = calloc(FILTER_N / 2, sizeof(short));
    short *dummy = calloc(FILTER_N / 2, sizeof(short));
    short *out[2] = { gather, dummy };
    const int amp = 20000;
    unsigned long long got;
    double measured, db;
    int i;

    TASK_CHECK(in && gather && dummy);
    for (i = 0; i < FILTER_N; i++)
        in[i] = gen_sine(i, 1000.0, 48000, amp);

    got = core_run(48000, 16000, 1, in, NULL, FILTER_N, out, 0);
    TASK_CHECK(got == ceil_div_mul(FILTER_N, 16000, 48000));

    /* discard both transition regions */
    measured = tone_amplitude(gather, 1000, (int) got - 1000, 1000.0, 16000);
    db = 20.0 * log10(measured / (double) amp);
    TASK_CHECK(fabs(db) <= 0.25);

    free(in);
    free(gather);
    free(dummy);
    return 0;
}

static int run_filter_alias(void)
{
    short *in = calloc(FILTER_N, sizeof(short));
    short *gather = calloc(FILTER_N / 2, sizeof(short));
    short *dummy = calloc(FILTER_N / 2, sizeof(short));
    short *out[2] = { gather, dummy };
    const int amp = 20000;
    unsigned long long got;
    double alias_amp, db;
    int i;

    TASK_CHECK(in && gather && dummy);
    for (i = 0; i < FILTER_N; i++)
        in[i] = gen_sine(i, 12000.0, 48000, amp);

    got = core_run(48000, 16000, 1, in, NULL, FILTER_N, out, 0);
    TASK_CHECK(got == ceil_div_mul(FILTER_N, 16000, 48000));

    /* 12 kHz folds to 16-12 = 4 kHz after 48 -> 16 decimation */
    alias_amp = tone_amplitude(gather, 1000, (int) got - 1000, 4000.0, 16000);
    db = 20.0 * log10(alias_amp / (double) amp);
    TASK_CHECK(db <= -60.0);

    free(in);
    free(gather);
    free(dummy);
    return 0;
}

static int run_filter_dc(void)
{
    const int n = 8000;
    short *in = calloc(n, sizeof(short));
    short *gather = calloc(n + 64, sizeof(short));
    short *dummy = calloc(n + 64, sizeof(short));
    short *out[2] = { gather, dummy };
    unsigned long long got;
    int i;

    TASK_CHECK(in && gather && dummy);
    for (i = 0; i < n; i++)
        in[i] = 12345;

    got = core_run(48000, 16000, 1, in, NULL, n, out, 0);
    TASK_CHECK(got > 2000);
    for (i = 500; i < (int) got - 500; i++)
        TASK_CHECK(abs((int) gather[i] - 12345) <= 2);

    free(in);
    free(gather);
    free(dummy);
    return 0;
}

static int run_filter_isolation(void)
{
    const int n = 20000;
    short *l = calloc(n, sizeof(short));
    short *r = calloc(n, sizeof(short));
    short *ol[2], *mono_l[1], *mono_r[1];
    short *gl = calloc(n + 64, sizeof(short));
    short *gr = calloc(n + 64, sizeof(short));
    short *gml = calloc(n + 64, sizeof(short));
    short *gmr = calloc(n + 64, sizeof(short));
    unsigned long long stereo_n, mono_ln, mono_rn;
    int i;

    TASK_CHECK(l && r && gl && gr && gml && gmr);
    for (i = 0; i < n; i++) {
        l[i] = gen_sine(i, 1000.0, 44100, 20000);
        r[i] = gen_sine(i, 7000.0, 44100, 15000);
    }
    ol[0] = gl;
    ol[1] = gr;
    mono_l[0] = gml;
    mono_r[0] = gmr;

    stereo_n = core_run(44100, 48000, 2, l, r, n, ol, 1);
    mono_ln = core_run(44100, 48000, 1, l, NULL, n, mono_l, 1);
    mono_rn = core_run(44100, 48000, 1, r, NULL, n, mono_r, 1);
    TASK_CHECK(stereo_n == mono_ln && stereo_n == mono_rn);
    TASK_CHECK(memcmp(gl, gml, (size_t) stereo_n * sizeof(short)) == 0);
    TASK_CHECK(memcmp(gr, gmr, (size_t) stereo_n * sizeof(short)) == 0);

    free(l);
    free(r);
    free(gl);
    free(gr);
    free(gml);
    free(gmr);
    return 0;
}

/*************************** frontends *******************************/

static int encode_short_planar(twolame_options * o, const short *p, int n, int chunked,
                               unsigned char *buf)
{
    int pos = 0, bytes = 0, k = 0;
    const int sizes[] = { 1, 37, 2049, 19 };
    int ch = twolame_get_num_channels(o);

    while (pos < n) {
        int blk = chunked ? sizes[k++ % 4] : n;
        int b;
        if (blk > n - pos)
            blk = n - pos;
        b = twolame_encode_buffer(o, p + pos, ch == 2 ? p + n + pos : NULL, blk,
                                  buf + bytes, BIG_BUF - bytes);
        if (b < 0)
            return -1;
        bytes += b;
        pos += blk;
    }
    return bytes;
}

static int encode_short_interleaved(twolame_options * o, const short *p, int n, int chunked,
                                    unsigned char *buf)
{
    int pos = 0, bytes = 0, k = 0;
    const int sizes[] = { 1, 37, 2049, 19 };
    int ch = twolame_get_num_channels(o);

    while (pos < n) {
        int blk = chunked ? sizes[k++ % 4] : n;
        int b;
        if (blk > n - pos)
            blk = n - pos;
        b = twolame_encode_buffer_interleaved(o, p + pos * ch, blk, buf + bytes,
                                              BIG_BUF - bytes);
        if (b < 0)
            return -1;
        bytes += b;
        pos += blk;
    }
    return bytes;
}

static int encode_float_planar(twolame_options * o, const short *sp, int n, int chunked,
                               unsigned char *buf)
{
    int pos = 0, bytes = 0, k = 0;
    const int sizes[] = { 1, 37, 2049, 19 };
    int ch = twolame_get_num_channels(o);
    float *fl = malloc((size_t) n * sizeof(float));
    float *fr = malloc((size_t) n * sizeof(float));
    int i;

    if (!fl || !fr)
        return -1;
    for (i = 0; i < n; i++)
        fl[i] = (float) sp[i] / 32768.0f;
    if (ch == 2)
        for (i = 0; i < n; i++)
            fr[i] = (float) sp[n + i] / 32768.0f;

    while (pos < n) {
        int blk = chunked ? sizes[k++ % 4] : n;
        int b;
        if (blk > n - pos)
            blk = n - pos;
        b = twolame_encode_buffer_float32(o, fl + pos, ch == 2 ? fr + pos : NULL, blk,
                                          buf + bytes, BIG_BUF - bytes);
        if (b < 0)
            return -1;
        bytes += b;
        pos += blk;
    }
    free(fl);
    free(fr);
    return bytes;
}

static int encode_float_interleaved(twolame_options * o, const short *sp, int n, int chunked,
                                    unsigned char *buf)
{
    int pos = 0, bytes = 0, k = 0;
    const int sizes[] = { 1, 37, 2049, 19 };
    int ch = twolame_get_num_channels(o);
    float *f = malloc((size_t) n * (size_t) ch * sizeof(float));
    int i;

    if (!f)
        return -1;
    for (i = 0; i < n * ch; i++)
        f[i] = (float) sp[i] / 32768.0f;

    while (pos < n) {
        int blk = chunked ? sizes[k++ % 4] : n;
        int b;
        if (blk > n - pos)
            blk = n - pos;
        b = twolame_encode_buffer_float32_interleaved(o, f + pos * ch, blk, buf + bytes,
                                                      BIG_BUF - bytes);
        if (b < 0)
            return -1;
        bytes += b;
        pos += blk;
    }
    free(f);
    return bytes;
}

typedef int (*enc_fn)(twolame_options *, const short *, int, int, unsigned char *);

static int run_frontends_all_match(void)
{
    const enc_fn fns[4] = {
        encode_short_planar, encode_short_interleaved,
        encode_float_planar, encode_float_interleaved
    };
    const int n = 7777;
    int ch = 2;
    short *planar = calloc((size_t) n * 2, sizeof(short));
    short *inter = calloc((size_t) n * 2, sizeof(short));
    unsigned char *ref = malloc(BIG_BUF);
    unsigned char *cur = malloc(BIG_BUF);
    int ref_bytes = -1;
    size_t f;
    int i;

    TASK_CHECK(planar && inter && ref && cur);
    for (i = 0; i < n * 2; i++) {
        short v = (short) ((i * 193 + 17) % 40001 - 20000);
        planar[i] = v;
    }
    for (i = 0; i < n; i++) {
        inter[2 * i] = planar[i];
        inter[2 * i + 1] = planar[n + i];
    }

    for (f = 0; f < 4; f++) {
        const short *src = (f == 1 || f == 3) ? inter : planar;
        int one, split;
        twolame_options *o1 = make_opts(48000, 44100, ch, 0);
        twolame_options *o2 = make_opts(48000, 44100, ch, 0);
        TASK_CHECK(o1 && o2);
        TASK_CHECK(twolame_init_params(o1) == 0);
        TASK_CHECK(twolame_init_params(o2) == 0);

        one = fns[f](o1, src, n, 0, ref);
        split = fns[f](o2, src, n, 1, cur);
        one += twolame_encode_flush(o1, ref + one, BIG_BUF - one);
        split += twolame_encode_flush(o2, cur + split, BIG_BUF - split);
        TASK_CHECK(one > 0 && split == one);
        TASK_CHECK(memcmp(ref, cur, (size_t) one) == 0);

        if (ref_bytes < 0)
            ref_bytes = one;
        else
            TASK_CHECK(one == ref_bytes && memcmp(ref, cur, (size_t) one) == 0);

        twolame_close(&o1);
        twolame_close(&o2);
    }

    /* frame count follows the logical resampled length */
    {
        unsigned long long L = ceil_div_mul(n, 44100, 48000);
        TASK_CHECK(count_mp2_frames(ref, ref_bytes, 44100)
                   == (int) ((L + TWOLAME_SAMPLES_PER_FRAME - 1) / TWOLAME_SAMPLES_PER_FRAME));
    }
    free(planar);
    free(inter);
    free(ref);
    free(cur);
    return 0;
}

static int run_frontends_mono(void)
{
    const int n = 12345;
    short *pcm = calloc((size_t) n, sizeof(short));
    unsigned char *a = malloc(BIG_BUF), *b = malloc(BIG_BUF);
    twolame_options *o1 = make_opts(48000, 16000, 1, 0);
    twolame_options *o2 = make_opts(48000, 16000, 1, 0);
    int ba, bb, fa;
    int i;

    TASK_CHECK(pcm && a && b && o1 && o2);
    for (i = 0; i < n; i++)
        pcm[i] = gen_sine(i, 500.0, 48000, 22000);
    TASK_CHECK(twolame_init_params(o1) == 0);
    TASK_CHECK(twolame_init_params(o2) == 0);

    ba = encode_short_planar(o1, pcm, n, 0, a);
    bb = encode_short_interleaved(o2, pcm, n, 1, b);
    TASK_CHECK(ba > 0 && ba == bb && memcmp(a, b, (size_t) ba) == 0);

    ba += twolame_encode_flush(o1, a + ba, BIG_BUF - ba);
    bb += twolame_encode_flush(o2, b + bb, BIG_BUF - bb);
    TASK_CHECK(ba == bb && memcmp(a, b, (size_t) ba) == 0);
    TASK_CHECK(twolame_get_resampled_samples(o1) == ceil_div_mul(n, 16000, 48000));
    fa = count_mp2_frames(a, ba, 16000);
    TASK_CHECK(fa >= 1);

    twolame_close(&o1);
    twolame_close(&o2);
    free(pcm);
    free(a);
    free(b);
    return 0;
}

static int run_frontends_long_lengths(void)
{
    int pairs[][2] = { {44100, 48000}, {48000, 44100} };
    size_t i;

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        const int n = 100000;
        short *pcm = calloc((size_t) n * 2, sizeof(short));
        unsigned char *mp2 = malloc(BIG_BUF);
        twolame_options *o = make_opts(pairs[i][0], pairs[i][1], 2, 0);
        unsigned long long L;
        int bytes, frames, q;

        TASK_CHECK(pcm && mp2 && o);
        for (q = 0; q < n * 2; q++)
            pcm[q] = (short) ((q * 97 + 5) % 50001 - 25000);
        TASK_CHECK(twolame_init_params(o) == 0);
        bytes = encode_short_interleaved(o, pcm, n, 1, mp2);
        TASK_CHECK(bytes >= 0);
        bytes += twolame_encode_flush(o, mp2 + bytes, BIG_BUF - bytes);
        TASK_CHECK(twolame_encode_flush(o, mp2 + bytes, BIG_BUF - bytes) == 0);

        L = ceil_div_mul(n, pairs[i][1], pairs[i][0]);
        TASK_CHECK(twolame_get_resampled_samples(o) == L);
        frames = count_mp2_frames(mp2, bytes, pairs[i][1]);
        TASK_CHECK(frames == (int) ((L + 1151) / 1152));
        twolame_close(&o);
        free(pcm);
        free(mp2);
    }
    return 0;
}

/***************************** legacy ********************************/

/* Reproduce the fixed baseline.c encoding and pin its exact size. */
#define LEGACY_N 5003

static void legacy_pcm(short *pcm)
{
    int i;
    for (i = 0; i < LEGACY_N; i++) {
        pcm[2 * i] = (short) ((i * 193) % 40001 - 20000);
        pcm[2 * i + 1] = (short) ((i * 127) % 30001 - 15000);
    }
}

static int run_legacy_anchor(void)
{
    short *pcm = calloc((size_t) LEGACY_N * 2, sizeof(short));
    unsigned char *a = malloc(BIG_BUF), *b = malloc(BIG_BUF);
    twolame_options *o1 = make_opts(44100, 44100, 2, 0);
    twolame_options *o2 = make_opts(44100, 44100, 2, 0);
    int ba, bb;

    TASK_CHECK(pcm && a && b && o1 && o2);
    legacy_pcm(pcm);
    TASK_CHECK(twolame_init_params(o1) == 0);
    TASK_CHECK(twolame_init_params(o2) == 0);

    ba = encode_short_interleaved(o1, pcm, LEGACY_N, 0, a);
    bb = encode_short_interleaved(o2, pcm, LEGACY_N, 1, b);
    TASK_CHECK(ba >= 0 && ba == bb && memcmp(a, b, (size_t) ba) == 0);
    ba += twolame_encode_flush(o1, a + ba, BIG_BUF - ba);
    bb += twolame_encode_flush(o2, b + bb, BIG_BUF - bb);
    TASK_CHECK(ba == bb && memcmp(a, b, (size_t) ba) == 0);
    TASK_CHECK(ba == 3130);
    TASK_CHECK(count_mp2_frames(a, ba, 44100) == 5);
    TASK_CHECK(twolame_get_resampled_samples(o1) == LEGACY_N);

    twolame_close(&o1);
    twolame_close(&o2);
    free(pcm);
    free(a);
    free(b);
    return 0;
}

static int run_legacy_crc(void)
{
    short *pcm = calloc((size_t) LEGACY_N * 2, sizeof(short));
    unsigned char *a = malloc(BIG_BUF), *b = malloc(BIG_BUF);
    twolame_options *o1 = make_opts(44100, 44100, 2, 1);
    twolame_options *o2 = make_opts(44100, 44100, 2, 1);
    int ba, bb, pos;

    TASK_CHECK(pcm && a && b && o1 && o2);
    legacy_pcm(pcm);
    TASK_CHECK(twolame_init_params(o1) == 0);
    TASK_CHECK(twolame_init_params(o2) == 0);

    ba = encode_short_interleaved(o1, pcm, LEGACY_N, 0, a);
    bb = encode_short_interleaved(o2, pcm, LEGACY_N, 1, b);
    ba += twolame_encode_flush(o1, a + ba, BIG_BUF - ba);
    bb += twolame_encode_flush(o2, b + bb, BIG_BUF - bb);
    TASK_CHECK(ba == bb && memcmp(a, b, (size_t) ba) == 0);

    /* protection bit clear means CRC is present: MPEG-1 Layer II 0xFC */
    for (pos = 0; pos < ba;) {
        int len;
        TASK_CHECK(a[pos] == 0xFF && a[pos + 1] == 0xFC);
        len = 144000 * 192 / 44100;
        pos += len;
    }
    TASK_CHECK(pos == ba);

    twolame_close(&o1);
    twolame_close(&o2);
    free(pcm);
    free(a);
    free(b);
    return 0;
}

static int run_legacy_partial(void)
{
    const int n = 100;
    short *pcm = calloc((size_t) n * 2, sizeof(short));
    unsigned char *mp2 = malloc(BIG_BUF);
    twolame_options *o = make_opts(48000, 48000, 2, 0);
    int bytes, tail;

    TASK_CHECK(pcm && mp2 && o);
    TASK_CHECK(twolame_init_params(o) == 0);
    bytes = twolame_encode_buffer_interleaved(o, pcm, n, mp2, BIG_BUF);
    TASK_CHECK(bytes == 0);      /* less than one frame: nothing emitted */
    tail = twolame_encode_flush(o, mp2, BIG_BUF);
    TASK_CHECK(tail > 0);
    TASK_CHECK(count_mp2_frames(mp2, tail, 48000) == 1);
    TASK_CHECK(twolame_encode_flush(o, mp2, BIG_BUF) == 0);
    TASK_CHECK(twolame_get_resampled_samples(o) == (unsigned long long) n);
    twolame_close(&o);
    free(pcm);
    free(mp2);
    return 0;
}

static int run_legacy_reject_and_scale(void)
{
    twolame_options *bad1 = make_opts(7999, 44100, 2, 0);
    twolame_options *bad2 = make_opts(96001, 44100, 2, 0);
    twolame_options *bad3 = make_opts(44100, 12345, 2, 0);
    twolame_options *o1 = make_opts(44100, 44100, 2, 0);
    twolame_options *o2 = make_opts(44100, 44100, 2, 0);
    short *pcm = calloc((size_t) 2000 * 2, sizeof(short));
    unsigned char *a = malloc(BIG_BUF), *b = malloc(BIG_BUF);
    int ba, bb, i;

    TASK_CHECK(bad1 && bad2 && bad3 && o1 && o2 && pcm && a && b);
    TASK_CHECK(twolame_init_params(bad1) != 0);
    TASK_CHECK(twolame_init_params(bad2) != 0);
    TASK_CHECK(twolame_init_params(bad3) != 0);

    for (i = 0; i < 4000; i++)
        pcm[i] = (short) ((i * 211) % 40001 - 20000);
    TASK_CHECK(twolame_init_params(o1) == 0);
    TASK_CHECK(twolame_init_params(o2) == 0);
    twolame_set_scale(o2, 0.5f);
    ba = encode_short_interleaved(o1, pcm, 2000, 0, a);
    bb = encode_short_interleaved(o2, pcm, 2000, 0, b);
    ba += twolame_encode_flush(o1, a + ba, BIG_BUF - ba);
    bb += twolame_encode_flush(o2, b + bb, BIG_BUF - bb);
    TASK_CHECK(ba == bb && memcmp(a, b, (size_t) ba) != 0);

    twolame_close(&bad1);
    twolame_close(&bad2);
    twolame_close(&bad3);
    twolame_close(&o1);
    twolame_close(&o2);
    free(pcm);
    free(a);
    free(b);
    return 0;
}

#define CASE(g, n) { #g, #n, run_##g##_##n }

const struct task_case task_cases[] = {
    CASE(clock, lengths),
    CASE(clock, defer_and_reflush),
    CASE(clock, getter),
    CASE(clock, empty),
    CASE(filter, passband),
    CASE(filter, alias),
    CASE(filter, dc),
    CASE(filter, isolation),
    CASE(frontends, all_match),
    CASE(frontends, mono),
    CASE(frontends, long_lengths),
    CASE(legacy, anchor),
    CASE(legacy, crc),
    CASE(legacy, partial),
    CASE(legacy, reject_and_scale),
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);

// vim:ts=4:sw=4:nowrap:
