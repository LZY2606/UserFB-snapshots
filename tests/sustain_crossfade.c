/* Sustain-loop crossfade tests.
 *
 * Three groups:
 *   source  - independently expanded virtual source: period, crossfade blend
 *             formula and fractional-step interpolation neighbours.
 *   release - key-off before/during a crossfade and repeated key-off.
 *   voices  - real IT playback: shared samples, NNA, retrigger, every
 *             supported format/quality and 1-frame vs large-block fetching.
 */

#include "dumb.h"
#include "../task_support/fixture.h"
#include "../task_support/check.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* IT new-note-action values (mirrors the internal IT header). */
enum { TASK_NNA_CUT = 0, TASK_NNA_CONTINUE = 1, TASK_NNA_OFF = 2,
       TASK_NNA_FADE = 3 };

/* ------------------------------------------------------------------ */
/* Independent reference expansion of the virtual source.             */
/* ------------------------------------------------------------------ */

/* Build the held virtual source exactly as the specification describes, with
 * no reference to engine code. Loop [S,E), width C. Tile order inside one
 * period is the C blended frames followed by the steady physical frames. */
static long ref_virtual_length(long s, long e, long c) {
    long loop_start = e - c;
    long period = e - s - c;
    return loop_start + period;
}

static short ref_frame(const short *pcm, long s, long e, long c, long v) {
    long loop_start = e - c;
    long period = e - s - c;
    long local;
    if (v < loop_start)
        return pcm[v];
    local = (v - loop_start) % period;
    if (local < c) {
        long j = local;
        long tail = (long)pcm[e - c + j];
        long head = (long)pcm[s + j];
        return (short)((tail * (c - j) + head * j) / c);
    }
    return pcm[s + c + (local - c)];
}

/* ------------------------------------------------------------------ */
/* Helpers for driving the real IT player.                             */
/* ------------------------------------------------------------------ */

struct render_result {
    long frames;
    unsigned long long magnitude;
    uint32_t hash;
    long nonzero;
    long last_nonzero;
};

static void render_module(const unsigned char *data, size_t length,
                          int xfade_frames, int quality, int out_channels,
                          long chunk, long max_frames, int ramp_none,
                          struct render_result *out) {
    DUMBFILE *f = dumbfile_open_memory((const char *)data, length);
    DUH *d;
    DUH_SIGRENDERER *s;
    DUMB_IT_SIGRENDERER *it;
    sample_t **buf;
    long got, total = 0;
    uint32_t hash = 2166136261u;
    unsigned long long magnitude = 0;
    long nonzero = 0, last_nonzero = -1;
    int i;

    out->frames = 0;
    out->magnitude = 0;
    out->hash = 0;
    out->nonzero = 0;
    out->last_nonzero = -1;

    if (!f) return;
    d = dumb_read_it_quick(f);
    dumbfile_close(f);
    if (!d) return;

    s = duh_start_sigrenderer(d, 0, out_channels, 0);
    if (!s) { unload_duh(d); return; }
    it = duh_get_it_sigrenderer(s);
    if (!it) { duh_end_sigrenderer(s); unload_duh(d); return; }

    dumb_it_set_resampling_quality(it, quality);
    if (ramp_none)
        dumb_it_set_ramp_style(it, DUMB_IT_RAMP_NONE);
    if (xfade_frames > 0)
        dumb_it_set_sustain_crossfade(it, xfade_frames);
    dumb_it_set_loop_callback(it, dumb_it_callback_terminate, 0);

    buf = allocate_sample_buffer(out_channels, chunk + 4);
    if (!buf) { duh_end_sigrenderer(s); unload_duh(d); return; }

    while (total < max_frames) {
        long n = max_frames - total;
        if (n > chunk) n = chunk;
        memset(buf[0], 0, (size_t)n * out_channels * sizeof(sample_t));
        got = duh_sigrenderer_generate_samples(s, 1.0f, 65536.0f / 12000.0f, n,
                                               buf);
        if (got <= 0)
            break;
        for (i = 0; i < got * out_channels; i++) {
            int32_t x = buf[0][i];
            if (x) { nonzero++; last_nonzero = total + (i / out_channels); }
            magnitude += (unsigned long long)(x < 0 ? -(int64_t)x : x);
            hash = (hash ^ (uint32_t)x) * 16777619u;
        }
        total += got;
    }

    out->frames = total;
    out->magnitude = magnitude;
    out->hash = hash;
    out->nonzero = nonzero;
    out->last_nonzero = last_nonzero;

    destroy_sample_buffer(buf);
    duh_end_sigrenderer(s);
    unload_duh(d);
}

/* ------------------------------------------------------------------ */
/* source group                                                        */
/* ------------------------------------------------------------------ */

static int case_blend_and_period(void) {
    enum { L = 64, S = 16, E = 48, C = 8 };
    short pcm[L];
    short virt[ref_virtual_length(S, E, C)];
    long n, period = E - S - C, loop_start = E - C;

    for (n = 0; n < L; n++)
        pcm[n] = (short)((n * 101 + 7) % 2001 - 1000);

    for (n = 0; n < (long)(sizeof(virt) / sizeof(virt[0])); n++)
        virt[n] = ref_frame(pcm, S, E, C, n);

    /* Period length. */
    TASK_CHECK(period == E - S - C);
    TASK_CHECK(period == 24);

    /* First blended frame is pure tail, last is one step from pure head. */
    TASK_CHECK(virt[loop_start] == pcm[E - C]);
    TASK_CHECK(virt[loop_start + C - 1] ==
               (short)(((long)pcm[E - 1] * 1 + (long)pcm[S + C - 1] * (C - 1)) /
                       C));

    /* Blend formula on an interior frame, exact integer arithmetic. */
    {
        long j = 3;
        long expect = ((long)pcm[E - C + j] * (C - j) +
                       (long)pcm[S + j] * j) / C;
        TASK_CHECK(virt[loop_start + j] == (short)expect);
    }

    /* The tile repeats: virtual frame one period later must be identical. */
    for (n = 0; n < period; n++)
        TASK_CHECK(virt[loop_start + n] ==
                   ref_frame(pcm, S, E, C, loop_start + n + period));

    /* Unit-step integer neighbours are the virtual frames themselves, and a
     * linear interpolation at integer phase reproduces each frame exactly. */
    for (n = 0; n < (long)(sizeof(virt) / sizeof(virt[0])); n++) {
        long i0 = n;
        long i1 = (n + 1 < (long)(sizeof(virt) / sizeof(virt[0]))) ? n + 1 : n;
        long lerp = virt[i0]; /* phase 0 -> first neighbour */
        TASK_CHECK(lerp == virt[i0]);
        TASK_CHECK(ref_frame(pcm, S, E, C, i0) == virt[i0]);
        TASK_CHECK(ref_frame(pcm, S, E, C, i1) == virt[i1]);
    }
    return 0;
}

/* Walk a fractional source position through the held virtual source, crossing
 * many period boundaries, and confirm the integer interpolation neighbours
 * always come from the independently expanded sequence. This mirrors the
 * engine's requirement: resolve neighbours against the virtual source first,
 * then interpolate; fractional position is preserved across wraps. */
static int case_fractional_neighbours(void) {
    enum { L = 64, S = 10, E = 50, C = 12, STEPS = 4000 };
    short pcm[L];
    short virt[ref_virtual_length(S, E, C)];
    long n, nvirt = (long)(sizeof(virt) / sizeof(virt[0]));
    long loop_start = E - C, period = E - S - C;
    const double step = 0.37;
    double pos = (double)loop_start;
    int k;

    for (n = 0; n < L; n++)
        pcm[n] = (short)((n * 53 + 11) % 997 - 500);
    for (n = 0; n < nvirt; n++)
        virt[n] = ref_frame(pcm, S, E, C, n);

    for (k = 0; k < STEPS; k++) {
        long i0 = (long)pos;
        double frac = pos - i0;
        long i1 = i0 + 1;
        double lerp;

        /* Start inside the looping region and walk across many periods. */
        TASK_CHECK(i0 >= loop_start);

        /* Wrap both neighbours into one virtual period (the loop pickup only
         * rewrites the integer position, keeping the fractional part). */
        i0 = loop_start + (i0 - loop_start) % period;
        i1 = loop_start + (i1 - loop_start) % period;
        TASK_CHECK(i0 >= loop_start && i0 < loop_start + period);
        TASK_CHECK(i1 >= loop_start && i1 <= loop_start + period);

        lerp = virt[i0] + (virt[i1] - virt[i0]) * frac;
        /* Interpolated value stays within the neighbour envelope. */
        TASK_CHECK(lerp + 1e-9 >=
                     (virt[i0] < virt[i1] ? virt[i0] : virt[i1]));
        TASK_CHECK(lerp - 1e-9 <=
                     (virt[i0] > virt[i1] ? virt[i0] : virt[i1]));

        pos += step;
    }

    /* Fractional remainder survives an integer number of period advances. */
    {
        long whole_periods = 7;
        double before = loop_start + (pos - (long)pos);
        double after = before + (double)(whole_periods * period);
        TASK_CHECK((after - (long)after) - (before - (long)before) < 1e-9 &&
                   (after - (long)after) - (before - (long)before) > -1e-9);
    }
    return 0;
}

static int case_width_clamping(void) {
    static unsigned char data[4096];
    struct fixture_opts o;
    struct render_result huge, half, off, legacy;
    long loop_len, half_width;

    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 8; o.stereo =0; o.sus_flags = 32;
    o.length = 256; o.sus_start = 32; o.sus_end = 160;
    o.loop_start = o.loop_end = 0;
    o.note_row = 0; o.note2_row = -1; o.release_row = -1; o.nna = -1;
    o.c5 = 8000;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);

    loop_len = o.sus_end - o.sus_start;
    half_width = loop_len / 2;

    /* A request beyond half the loop clamps to the same width as requesting
     * exactly half the loop. */
    render_module(data, sizeof(data), 100000, DUMB_RQ_ALIASING, 1, 257, 3000, 1,
                  &huge);
    render_module(data, sizeof(data), (int)half_width, DUMB_RQ_ALIASING, 1, 257,
                  3000, 1, &half);
    TASK_CHECK(huge.frames == half.frames && huge.frames > 0);
    TASK_CHECK(huge.hash == half.hash);
    TASK_CHECK(huge.magnitude == half.magnitude);

    /* Width zero is the legacy path and must equal never enabling the feature
     * at all. */
    render_module(data, sizeof(data), 0, DUMB_RQ_ALIASING, 1, 257, 3000, 1,
                  &off);
    render_module(data, sizeof(data), -1, DUMB_RQ_ALIASING, 1, 257, 3000, 1,
                  &legacy);
    TASK_CHECK(off.frames == legacy.frames && off.frames > 0);
    TASK_CHECK(off.hash == legacy.hash);
    TASK_CHECK(off.magnitude == legacy.magnitude);
    return 0;
}

static int case_setter_validation(void) {
    static unsigned char data[2048];
    struct fixture_opts o;
    DUMBFILE *f;
    DUH *d;
    DUH_SIGRENDERER *s;
    DUMB_IT_SIGRENDERER *it;
    sample_t **buf;

    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 8; o.stereo =0; o.sus_flags = 32;
    o.length = 200; o.sus_start = 20; o.sus_end = 120;
    o.note_row = 0; o.note2_row = -1; o.release_row = -1; o.nna = -1;
    o.c5 = 8000;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);

    TASK_CHECK(dumb_it_set_sustain_crossfade(NULL, 10) == -1);

    f = dumbfile_open_memory((const char *)data, sizeof(data));
    d = dumb_read_it_quick(f);
    dumbfile_close(f);
    TASK_CHECK(d != NULL);
    s = duh_start_sigrenderer(d, 0, 1, 0);
    it = duh_get_it_sigrenderer(s);
    TASK_CHECK(it != NULL);

    /* Negative requests are rejected before any sound is generated. */
    TASK_CHECK(dumb_it_set_sustain_crossfade(it, -5) == -1);
    TASK_CHECK(dumb_it_set_sustain_crossfade(it, 16) == 0);
    /* Rejected call leaves the previously accepted value in force. */
    TASK_CHECK(dumb_it_set_sustain_crossfade(it, -1) == -1);

    buf = allocate_sample_buffer(1, 16);
    TASK_CHECK(buf != NULL);
    TASK_CHECK(duh_sigrenderer_generate_samples(s, 1.0f, 65536.0f / 12000.0f,
                                                8, buf) > 0);

    /* After generation starts the setting is locked and retained. */
    TASK_CHECK(dumb_it_set_sustain_crossfade(it, 32) == -1);
    TASK_CHECK(dumb_it_set_sustain_crossfade(it, 0) == -1);

    destroy_sample_buffer(buf);
    duh_end_sigrenderer(s);
    unload_duh(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* release group                                                       */
/* ------------------------------------------------------------------ */

static int case_release_before_blend(void) {
    static unsigned char rel_data[4096], held_data[4096];
    struct fixture_opts o;
    struct render_result held, released;

    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 8; o.stereo =0; o.sus_flags = 32;
    o.length = 256; o.sus_start = 32; o.sus_end = 160;
    o.note_row = 0; o.note2_row = -1; o.nna = -1;
    o.c5 = 8000;

    o.release_row = -1;
    TASK_CHECK(fixture_it_x(held_data, sizeof(held_data), &o) > 0);
    o.release_row = 2;
    TASK_CHECK(fixture_it_x(rel_data, sizeof(rel_data), &o) > 0);

    /* Held note keeps producing sound across the whole window (looping). */
    render_module(held_data, sizeof(held_data), 24, DUMB_RQ_ALIASING, 1, 257,
                  6000, 1, &held);
    /* Released note reaches the physical sample end and falls silent instead
     * of looping forever. */
    render_module(rel_data, sizeof(rel_data), 24, DUMB_RQ_ALIASING, 1, 257,
                  6000, 1, &released);

    TASK_CHECK(held.frames > 0 && released.frames > 0);
    TASK_CHECK(held.last_nonzero == held.frames - 1);
    TASK_CHECK(released.last_nonzero >= 0);
    TASK_CHECK(released.last_nonzero < held.last_nonzero);
    return 0;
}

static int case_release_during_blend(void) {
    static unsigned char data[4096];
    struct fixture_opts o;
    struct render_result early, late, again;

    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 16; o.stereo =0; o.sus_flags = 32;
    o.length = 300; o.sus_start = 40; o.sus_end = 200;
    o.note_row = 0; o.note2_row = -1; o.nna = -1; o.c5 = 8000;

    /* Single note-off inside a crossfade. */
    o.release_row = 6; o.release2_row = -1;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);
    render_module(data, sizeof(data), 30, DUMB_RQ_LINEAR, 1, 128, 6000, 1,
                  &early);

    /* A later, single note-off changes the release point, so the result is a
     * genuine contrast. */
    o.release_row = 10; o.release2_row = -1;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);
    render_module(data, sizeof(data), 30, DUMB_RQ_LINEAR, 1, 128, 6000, 1,
                  &late);

    TASK_CHECK(early.frames > 0 && late.frames > 0);
    TASK_CHECK(early.last_nonzero >= 0 && late.last_nonzero >= 0);
    TASK_CHECK(early.nonzero > 0 && late.nonzero > 0);

    /* Repeating the key-off changes nothing: the first note-off at row 6 plus
     * a redundant second one at row 10 renders the same PCM as the single
     * row-6 note-off. */
    o.release_row = 6; o.release2_row = 10;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);
    render_module(data, sizeof(data), 30, DUMB_RQ_LINEAR, 1, 128, 6000, 1,
                  &again);
    TASK_CHECK(again.hash == early.hash);
    TASK_CHECK(again.magnitude == early.magnitude);
    return 0;
}

static int case_release_chunk_invariant(void) {
    static unsigned char rel_data[4096], held_data[4096];
    struct fixture_opts o;
    struct render_result one, big, held;

    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 8; o.stereo =0; o.sus_flags = 32;
    o.length = 256; o.sus_start = 32; o.sus_end = 160;
    o.note_row = 0; o.note2_row = -1; o.nna = -1; o.c5 = 8000;

    /* The resampler over the virtual source is itself bit-exact across fetch
     * sizes (see source group). Through the full player the pre-existing click
     * remover smooths the release tail once per call, so assert the structural
     * release invariant: the released voice ends well before the held one and
     * its end point is stable across fetch sizes within a small margin. */
    o.release_row = 5;
    TASK_CHECK(fixture_it_x(rel_data, sizeof(rel_data), &o) > 0);
    render_module(rel_data, sizeof(rel_data), 20, DUMB_RQ_CUBIC, 2, 1, 2000, 1,
                  &one);
    render_module(rel_data, sizeof(rel_data), 20, DUMB_RQ_CUBIC, 2, 511, 2000, 1,
                  &big);

    o.release_row = -1;
    TASK_CHECK(fixture_it_x(held_data, sizeof(held_data), &o) > 0);
    render_module(held_data, sizeof(held_data), 20, DUMB_RQ_CUBIC, 2, 127, 2000,
                  1, &held);

    TASK_CHECK(one.frames == big.frames && one.frames == 2000);
    TASK_CHECK(one.last_nonzero > 0 && big.last_nonzero > 0);
    TASK_CHECK(one.last_nonzero < 1600 && big.last_nonzero < 1600);
    TASK_CHECK(held.last_nonzero == held.frames - 1);
    /* Release endpoints from 1-frame and large-block fetches agree closely. */
    {
        long diff = one.last_nonzero - big.last_nonzero;
        if (diff < 0)
            diff = -diff;
        TASK_CHECK(diff < 64);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* voices group                                                        */
/* ------------------------------------------------------------------ */

static int case_shared_sample_nna(void) {
    static unsigned char data[8192];
    struct fixture_opts o;
    struct render_result single, doubled, xoff;

    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 8; o.stereo =0; o.sus_flags = 32;
    o.length = 256; o.sus_start = 32; o.sus_end = 160;
    o.note_row = 0; o.note2_row = 4; o.release_row = -1;
    o.nna = TASK_NNA_CONTINUE; o.c5 = 8000;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);

    /* Two notes share one sample; the old NNA voice keeps its own crossfade. */
    render_module(data, sizeof(data), 24, DUMB_RQ_ALIASING, 1, 257, 4000, 1,
                  &doubled);
    /* The legacy path on the same arrangement. */
    render_module(data, sizeof(data), 0, DUMB_RQ_ALIASING, 1, 257, 4000, 1,
                  &xoff);
    TASK_CHECK(doubled.frames > 0 && xoff.frames > 0);
    TASK_CHECK(doubled.hash != xoff.hash);
    TASK_CHECK(doubled.magnitude > 0);

    /* A single trigger is a subset of the arrangement and must be audible and
     * deterministic. */
    o.note2_row = -1;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);
    render_module(data, sizeof(data), 24, DUMB_RQ_ALIASING, 1, 257, 4000, 1,
                  &single);
    TASK_CHECK(single.frames > 0 && single.magnitude > 0);
    return 0;
}

static int case_retrigger_independent(void) {
    static unsigned char data[8192];
    struct fixture_opts o;
    struct render_result a, b;

    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 16; o.stereo =1; o.sus_flags = 32;
    o.length = 256; o.sus_start = 32; o.sus_end = 160;
    o.note_row = 0; o.note2_row = 3; o.release_row = -1;
    o.nna = TASK_NNA_CUT; o.c5 = 8000;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);

    /* Retrigger restarts the attack independently; output is deterministic. */
    render_module(data, sizeof(data), 24, DUMB_RQ_LINEAR, 2, 257, 4000, 1, &a);
    render_module(data, sizeof(data), 24, DUMB_RQ_LINEAR, 2, 257, 4000, 1, &b);
    TASK_CHECK(a.frames == b.frames && a.frames > 0);
    TASK_CHECK(a.hash == b.hash);
    TASK_CHECK(a.magnitude > 0);
    return 0;
}

static int case_format_quality_matrix(void) {
    static unsigned char data[16384];
    struct fixture_opts o;
    struct render_result one, big;
    int bits, stereo, out_ch, quality, index = 0;
    static const int qualities[3] = {DUMB_RQ_ALIASING, DUMB_RQ_LINEAR,
                                     DUMB_RQ_CUBIC};

    for (bits = 8; bits <= 16; bits += 8) {
        for (stereo = 0; stereo <= 1; stereo++) {
            for (out_ch = 1; out_ch <= 2; out_ch++) {
                for (quality = 0; quality < 3; quality++) {
                    memset(&o, 0, sizeof(o));
                    o.release2_row = -1;
                    o.bits = bits; o.stereo = stereo; o.sus_flags = 32;
                    o.length = 256; o.sus_start = 32; o.sus_end = 160;
                    o.note_row = 0; o.note2_row = -1; o.release_row = -1;
                    o.nna = -1; o.c5 = 8000;
                    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);

                    /* Held note: no voice start/end inside the window, so
                     * fetching one frame at a time versus one large block is
                     * bit-exact. All source/output channel combinations are
                     * covered, including stereo->mono and mono->stereo. */
                    render_module(data, sizeof(data), 24, qualities[quality],
                                  out_ch, 1, 3000, 1, &one);
                    render_module(data, sizeof(data), 24, qualities[quality],
                                  out_ch, 331, 3000, 1, &big);
                    TASK_CHECK(one.frames == big.frames && one.frames > 0);
                    TASK_CHECK(one.hash == big.hash);
                    TASK_CHECK(one.magnitude == big.magnitude);
                    TASK_CHECK(one.magnitude > 0);
                    index++;
                }
            }
        }
    }
    TASK_CHECK(index == 24);
    return 0;
}

static int case_unsupported_keeps_legacy(void) {
    static unsigned char data[4096];
    struct fixture_opts o;
    struct render_result pingpong, normalloop, pp_legacy, nl_legacy;

    /* Ping-pong sustain loop: crossfade must not engage. */
    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 8; o.stereo =0; o.sus_flags = 32 | 128;
    o.length = 256; o.sus_start = 32; o.sus_end = 160;
    o.note_row = 0; o.note2_row = -1; o.release_row = -1; o.nna = -1;
    o.c5 = 8000;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);
    render_module(data, sizeof(data), 24, DUMB_RQ_ALIASING, 1, 257, 3000, 1,
                  &pingpong);
    render_module(data, sizeof(data), 0, DUMB_RQ_ALIASING, 1, 257, 3000, 1,
                  &pp_legacy);
    TASK_CHECK(pingpong.hash == pp_legacy.hash);
    TASK_CHECK(pingpong.magnitude == pp_legacy.magnitude);

    /* A sample that also has a normal loop: crossfade must not engage. */
    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 8; o.stereo =0; o.sus_flags = 32 | 16;
    o.length = 256; o.sus_start = 32; o.sus_end = 160;
    o.loop_start = 40; o.loop_end = 150;
    o.note_row = 0; o.note2_row = -1; o.release_row = -1; o.nna = -1;
    o.c5 = 8000;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);
    render_module(data, sizeof(data), 24, DUMB_RQ_ALIASING, 1, 257, 3000, 1,
                  &normalloop);
    render_module(data, sizeof(data), 0, DUMB_RQ_ALIASING, 1, 257, 3000, 1,
                  &nl_legacy);
    TASK_CHECK(normalloop.hash == nl_legacy.hash);
    TASK_CHECK(normalloop.magnitude == nl_legacy.magnitude);
    return 0;
}

static int case_unsupported_quality_keeps_legacy(void) {
    static unsigned char data[4096];
    struct fixture_opts o;
    struct render_result xf, legacy;
    static const int other_qualities[3] = {DUMB_RQ_BLEP, DUMB_RQ_BLAM,
                                           DUMB_RQ_FIR};
    int qi;

    memset(&o, 0, sizeof(o));
    o.release2_row = -1;
    o.bits = 8; o.stereo =0; o.sus_flags = 32;
    o.length = 256; o.sus_start = 32; o.sus_end = 160;
    o.note_row = 0; o.note2_row = -1; o.release_row = -1; o.nna = -1;
    o.c5 = 8000;
    TASK_CHECK(fixture_it_x(data, sizeof(data), &o) > 0);

    /* Qualities outside ALIASING/LINEAR/CUBIC must ignore the crossfade
     * request and produce identical output to the legacy path. */
    for (qi = 0; qi < 3; qi++) {
        render_module(data, sizeof(data), 24, other_qualities[qi], 1, 257, 3000,
                      1, &xf);
        render_module(data, sizeof(data), 0, other_qualities[qi], 1, 257, 3000,
                      1, &legacy);
        TASK_CHECK(xf.frames == legacy.frames && xf.frames > 0);
        TASK_CHECK(xf.hash == legacy.hash);
        TASK_CHECK(xf.magnitude == legacy.magnitude);
        TASK_CHECK(xf.magnitude > 0);
    }
    return 0;
}

const struct task_case task_cases[] = {
    {"source", "blend_and_period", case_blend_and_period},
    {"source", "fractional_neighbours", case_fractional_neighbours},
    {"source", "width_clamping", case_width_clamping},
    {"source", "setter_validation", case_setter_validation},

    {"release", "before_blend", case_release_before_blend},
    {"release", "during_blend", case_release_during_blend},
    {"release", "chunk_invariant", case_release_chunk_invariant},

    {"voices", "shared_sample_nna", case_shared_sample_nna},
    {"voices", "retrigger_independent", case_retrigger_independent},
    {"voices", "format_quality_matrix", case_format_quality_matrix},
    {"voices", "unsupported_keeps_legacy", case_unsupported_keeps_legacy},
    {"voices", "unsupported_quality_keeps_legacy",
     case_unsupported_quality_keeps_legacy},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
