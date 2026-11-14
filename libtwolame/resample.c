/*
 *  TwoLAME: an optimized MPEG Audio Layer Two encoder
 *
 *  Copyright (C) 2004-2018 The TwoLAME Project
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

#include "common.h"
#include "mem.h"
#include "resample.h"

/*
 * Continuous, band-limited sample rate converter.
 *
 * The output sample schedule is shared by every channel: output frame k
 * corresponds to source position x_k = k * in_rate / out_rate, measured
 * from source coordinate zero. Each output value is a windowed-sinc
 * (Kaiser-windowed low-pass) convolution of the source, so decimation
 * and interpolation are both properly band limited - no sample dropping
 * or linear interpolation.
 *
 * Each channel carries its own copy of the input history, so the filter
 * state of the channels never mixes.
 *
 * Output is causal from the caller's point of view: a frame is only
 * emitted once all input samples its filter tail reaches have been
 * supplied. At flush time the source is treated as zero outside its
 * actual extent, which both completes the end of the stream and accounts
 * for the zero-extension at the beginning.
 */

#define TL_RS_TAPS      64      /* even; symmetric window, half a window each side */
#define TL_RS_HALF      (TL_RS_TAPS / 2)
#define TL_RS_KAISER_B  10.0    /* ~100 dB stop band, well under 0.25 dB ripple */
#define TL_RS_PHASES    512     /* polyphase sub-sample resolution */
#define TL_RS_MIN_RATE  8000
#define TL_RS_MAX_RATE  96000

struct twolame_resampler {
    int in_rate;
    int out_rate;
    int num_channels;

    /* polyphase windowed-sinc table: table[phase][tap], every phase
     * normalised to unit DC gain */
    double table[TL_RS_PHASES][TL_RS_TAPS];
    double cutoff;              /* radians per input sample */

    /* per-channel circular input history (TL_RS_TAPS most recent samples) */
    short *history[2];
    unsigned long long total_in;    /* number of input frames ever supplied */

    unsigned long long out_count;   /* shared output schedule position k */
    int flushed;

    /* per-channel output staging buffers */
    short *out[2];
    int out_cap[2];

    /* per-channel scratch buffers (float front ends stage shorts here) */
    short *scratch[2];
    int scratch_cap[2];
};

/* Modified Bessel function of the first kind, order zero. */
static double bessel_i0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    double x2_4 = (x * x) / 4.0;
    int m;

    for (m = 1; m < 40; m++) {
        term *= x2_4 / ((double) m * m);
        sum += term;
        if (term < 1e-16 * sum)
            break;
    }
    return sum;
}

/* One windowed-sinc tap evaluated at source distance d (in samples). */
static double prototype_tap(const twolame_resampler *rs, double d)
{
    double sinc, arg, w;

    if (d == 0.0) {
        sinc = rs->cutoff / PI;
    } else {
        sinc = sin(rs->cutoff * d) / (PI * d);
    }

    arg = 2.0 * d / (TL_RS_TAPS - 1);
    if (arg > 1.0)
        arg = 1.0;
    else if (arg < -1.0)
        arg = -1.0;
    w = bessel_i0(TL_RS_KAISER_B * sqrt(1.0 - arg * arg)) / bessel_i0(TL_RS_KAISER_B);

    return sinc * w;
}

/*
 * Build the polyphase table. Phase p represents a fractional source
 * position frac = p / TL_RS_PHASES; tap n reaches source sample
 * base + n - (TL_RS_HALF - 1), at distance n - (TL_RS_HALF - 1) - frac.
 */
static void build_filter(twolame_resampler *rs)
{
    int p, n;

    rs->cutoff = PI;
    if (rs->out_rate < rs->in_rate)
        rs->cutoff = PI * (double) rs->out_rate / (double) rs->in_rate;

    for (p = 0; p < TL_RS_PHASES; p++) {
        double frac = (double) p / (double) TL_RS_PHASES;
        double gain = 0.0;

        for (n = 0; n < TL_RS_TAPS; n++) {
            double d = (double) n - (TL_RS_HALF - 1) - frac;
            rs->table[p][n] = prototype_tap(rs, d);
            gain += rs->table[p][n];
        }
        for (n = 0; n < TL_RS_TAPS; n++)
            rs->table[p][n] /= gain;
    }
}

twolame_resampler *twolame_resampler_create(int in_rate, int out_rate, int num_channels)
{
    twolame_resampler *rs;
    int ch;

    if (in_rate < TL_RS_MIN_RATE || in_rate > TL_RS_MAX_RATE)
        return NULL;
    if (out_rate < TL_RS_MIN_RATE || out_rate > TL_RS_MAX_RATE)
        return NULL;
    if (num_channels != 1 && num_channels != 2)
        return NULL;

    rs = (twolame_resampler *) TWOLAME_MALLOC(sizeof(twolame_resampler));
    if (rs == NULL)
        return NULL;
    memset(rs, 0, sizeof(*rs));

    rs->in_rate = in_rate;
    rs->out_rate = out_rate;
    rs->num_channels = num_channels;

    build_filter(rs);

    for (ch = 0; ch < num_channels; ch++) {
        rs->history[ch] = (short *) TWOLAME_MALLOC(TL_RS_TAPS * sizeof(short));
        if (rs->history[ch] == NULL) {
            twolame_resampler_destroy(rs);
            return NULL;
        }
        /* zero history == zero extension of the source */
        memset(rs->history[ch], 0, TL_RS_TAPS * sizeof(short));
    }

    return rs;
}

void twolame_resampler_destroy(twolame_resampler *rs)
{
    int ch;

    if (rs == NULL)
        return;

    for (ch = 0; ch < 2; ch++) {
        TWOLAME_FREE(rs->history[ch]);
        TWOLAME_FREE(rs->out[ch]);
        TWOLAME_FREE(rs->scratch[ch]);
    }
    TWOLAME_FREE(rs);
}

/* Read one input sample, zero-extending at both ends of the source. */
static short history_get(const twolame_resampler *rs, int ch, long long idx)
{
    if (idx < 0 || (unsigned long long) idx >= rs->total_in)
        return 0;
    return rs->history[ch][(unsigned long long) idx % TL_RS_TAPS];
}

static int ensure_cap(short **buf, int *cap, int need)
{
    if (need <= *cap)
        return 0;
    {
        int newcap = (*cap == 0) ? need : *cap;
        short *nb;
        while (newcap < need)
            newcap *= 2;
        nb = (short *) realloc(*buf, (size_t) newcap * sizeof(short));
        if (nb == NULL)
            return -1;
        *buf = nb;
        *cap = newcap;
        return 0;
    }
}

static short clamp_short(double v)
{
    long r = lrint(v);
    if (r > SHRT_MAX)
        return SHRT_MAX;
    if (r < SHRT_MIN)
        return SHRT_MIN;
    return (short) r;
}

/*
 * Append every output frame that is currently computable.
 * In streaming mode a frame is withheld until every source sample its
 * tail reaches has already been supplied. In flush mode missing source
 * samples read as zero and we stop at the logical output length.
 * *produced counts frames appended to rs->out during this call.
 */
static int emit_available(twolame_resampler *rs, int flush_mode,
                          unsigned long long target, int *produced)
{
    unsigned long long k = rs->out_count;
    double step = (double) rs->in_rate / (double) rs->out_rate;
    int ch;

    for (;;) {
        double center, frac;
        long long base, n;
        int tap, phase;

        if (flush_mode && k >= target)
            break;

        center = (double) k * step;
        base = (long long) floor(center);
        frac = center - (double) base;

        if (!flush_mode) {
            /* last source index touched by the filter window */
            if ((unsigned long long) (base + TL_RS_HALF) >= rs->total_in)
                break;          /* wait for future input */
        }

        phase = (int) lround(frac * (double) TL_RS_PHASES);
        if (phase >= TL_RS_PHASES)
            phase = TL_RS_PHASES - 1;

        if (ensure_cap(&rs->out[0], &rs->out_cap[0], *produced + 1) < 0)
            return -1;
        if (rs->num_channels == 2
                && ensure_cap(&rs->out[1], &rs->out_cap[1], *produced + 1) < 0)
            return -1;

        for (ch = 0; ch < rs->num_channels; ch++) {
            double acc = 0.0;
            const double *h = rs->table[phase];
            for (tap = 0; tap < TL_RS_TAPS; tap++) {
                n = base + tap - (TL_RS_HALF - 1);
                acc += (double) history_get(rs, ch, n) * h[tap];
            }
            rs->out[ch][*produced] = clamp_short(acc);
        }

        (*produced)++;
        k++;
    }

    rs->out_count = k;
    return 0;
}

/* Push one source frame into each channel's circular history. */
static void push_frame(twolame_resampler *rs, const short frame[2])
{
    int ch;
    unsigned long long slot = rs->total_in % TL_RS_TAPS;

    for (ch = 0; ch < rs->num_channels; ch++)
        rs->history[ch][slot] = frame[ch];
    rs->total_in++;
}

int twolame_resampler_process(twolame_resampler *rs,
                              const short *const in[],
                              int num_samples,
                              short *out[], int *out_n)
{
    int produced = 0;
    int i, ch;

    if (rs == NULL || in == NULL || out == NULL || out_n == NULL || num_samples < 0)
        return -1;
    if (num_samples == 0 || rs->flushed) {
        *out_n = 0;
        return 0;
    }

    /* Advance one source frame at a time so the ring never overwrites a
     * sample a pending output still needs; emit everything newly ready. */
    for (i = 0; i < num_samples; i++) {
        short frame[2];
        for (ch = 0; ch < rs->num_channels; ch++)
            frame[ch] = in[ch][i];
        push_frame(rs, frame);
        if (emit_available(rs, 0, 0, &produced) < 0)
            return -1;
    }

    for (ch = 0; ch < rs->num_channels; ch++)
        out[ch] = rs->out[ch];
    *out_n = produced;
    return 0;
}

int twolame_resampler_process_interleaved(twolame_resampler *rs,
                                          const short pcm[],
                                          int num_samples,
                                          int num_channels,
                                          short *out[], int *out_n)
{
    int produced = 0;
    int i, ch;

    if (rs == NULL || pcm == NULL || out == NULL || out_n == NULL || num_samples < 0)
        return -1;
    if (num_channels != rs->num_channels)
        return -1;
    if (num_samples == 0 || rs->flushed) {
        *out_n = 0;
        return 0;
    }

    for (i = 0; i < num_samples; i++) {
        short frame[2];
        for (ch = 0; ch < num_channels; ch++)
            frame[ch] = pcm[i * num_channels + ch];
        push_frame(rs, frame);
        if (emit_available(rs, 0, 0, &produced) < 0)
            return -1;
    }

    for (ch = 0; ch < rs->num_channels; ch++)
        out[ch] = rs->out[ch];
    *out_n = produced;
    return 0;
}

int twolame_resampler_flush(twolame_resampler *rs, short *out[], int *out_n)
{
    unsigned long long target;
    int produced = 0;
    int ch;

    if (rs == NULL || out == NULL || out_n == NULL)
        return -1;

    if (rs->flushed) {
        *out_n = 0;
        out[0] = rs->out[0];
        if (rs->num_channels == 2)
            out[1] = rs->out[1];
        return 0;
    }
    rs->flushed = 1;

    /* logical output length: ceil(total_in * out_rate / in_rate) */
    target = (rs->total_in * (unsigned long long) rs->out_rate
              + (unsigned long long) rs->in_rate - 1)
        / (unsigned long long) rs->in_rate;

    if (emit_available(rs, 1, target, &produced) < 0)
        return -1;

    for (ch = 0; ch < rs->num_channels; ch++)
        out[ch] = rs->out[ch];
    *out_n = produced;
    return 0;
}

unsigned long long twolame_resampler_count(const twolame_resampler *rs)
{
    if (rs == NULL)
        return 0;
    return rs->out_count;
}

short *twolame_resampler_scratch(twolame_resampler *rs, int channel, int need)
{
    if (rs == NULL || channel < 0 || channel >= rs->num_channels || need <= 0)
        return NULL;
    if (ensure_cap(&rs->scratch[channel], &rs->scratch_cap[channel], need) < 0)
        return NULL;
    return rs->scratch[channel];
}

// vim:ts=4:sw=4:nowrap:
