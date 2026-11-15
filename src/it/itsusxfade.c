/*  _______         ____    __         ___    ___
 * \    _  \       \    /  \  /       \   \  /   /       '   '  '
 *  |  | \  \       |  |    ||         | \/   |         .      .
 *  |  |  |  |      |  |    ||         ||\  /|  |
 *  |  |  |  |      |  |    ||         || \/ |  |
 *  |  |_/  /        \  \__//          ||    |  |
 * /_______/ynamic    \____/niversal  /__\  /____\usic   /|  .  . ibliotheque
 *                                                      /  \
 *                                                     / .  \
 * itsusxfade.c - Optional sustain-loop crossfade.   / / \  \
 *                                                   | <  /   \_
 * The sustain loop [S,E) is realised as a virtual   |  \/ /\   /
 * short source, so the original sample PCM is never  \_  /  > /
 * edited and a fade is never applied between two      | \ / /
 * already-resampled outputs.                          |  ' /
 *                                                     \__/
 */

#include <stdlib.h>
#include <string.h>

#include "dumb.h"
#include "internal/dumb.h"
#include "internal/it.h"
#include "internal/resampler.h"

/* Only the integer neighbour interpolation qualities run the crossfade; the
 * other qualities retain the legacy resampling path.
 */
int _dumb_it_xfade_quality_supported(int quality) {
    return quality == DUMB_RQ_ALIASING || quality == DUMB_RQ_LINEAR ||
           quality == DUMB_RQ_CUBIC;
}

static int source_channels(const IT_PLAYING *playing) {
    return (playing->sample->flags & IT_SAMPLE_STEREO) ? 2 : 1;
}

/* Read one source channel of one physical frame, normalised into the 16-bit
 * domain consumed by the _16 resampler.
 *
 * 16-bit samples are used unchanged: the _16 resampler's FIR() divides by
 * 32768, exactly as on the legacy path.
 *
 * 8-bit samples are widened (v * 128), which is exact: the _16 FIR then yields
 * v*128/32768 == v/256, the same normalised value the legacy _8 FIR produces.
 * Widening also means an 8-bit blend keeps sub-LSB precision instead of being
 * rounded straight back to a signed char.
 */
static short normalised_source(const IT_PLAYING *playing, long frame, int ch) {
    int channels = source_channels(playing);
    if (playing->sample->flags & IT_SAMPLE_16BIT)
        return ((const short *)playing->sample->data)[frame * channels + ch];
    return (short)((int)((const signed char *)playing->sample->data)
                           [frame * channels + ch] *
                   128);
}

/* Convex blend in a wide integer type. The result always lies between the two
 * inputs, so it can never clip; it is rounded into the resampler's native
 * short domain only at this single point, preserving interpolation gain and
 * phase.
 */
static short blend_frames(short tail, short head, long j, long c) {
    return (short)(((LONG_LONG)tail * (c - j) + (LONG_LONG)head * j) / c);
}

static void copy_frame(const IT_PLAYING *playing, short *dst, long phys_frame,
                       int channels) {
    int ch;
    for (ch = 0; ch < channels; ch++)
        dst[ch] = normalised_source(playing, phys_frame, ch);
}

static void blend_frame(const IT_PLAYING *playing, short *dst, long tail_frame,
                        long head_frame, long j, long c, int channels) {
    int ch;
    for (ch = 0; ch < channels; ch++) {
        short tail = normalised_source(playing, tail_frame, ch);
        short head = normalised_source(playing, head_frame, ch);
        dst[ch] = blend_frames(tail, head, j, c);
    }
}

/* Build the release-tail virtual source (physical S+C .. sample end) and
 * continue from its first frame. Called when a key-off that landed inside a
 * crossfade reaches the end of the blend segment. The fractional position is
 * left untouched by only repositioning the integer part. */
static void xfade_pickup_relmix(DUMB_RESAMPLER *resampler, void *data) {
    IT_PLAYING *playing = (IT_PLAYING *)data;
    long length = playing->sample->length;
    long start = playing->sample->sus_loop_start + playing->xfade_width;
    long frames = length - start;
    int channels = source_channels(playing);
    short *release;
    long i;

    if (frames <= 0) {
        resampler->dir = 0;
        return;
    }

    release = malloc((size_t)frames * channels * sizeof(short));
    if (!release) {
        resampler->dir = 0;
        return;
    }

    for (i = 0; i < frames; i++)
        copy_frame(playing, release + i * channels, start + i, channels);

    free(playing->xfade_buf);
    playing->xfade_buf = release;
    playing->xfade_buf_frames = frames;
    resampler->src = release;
    resampler->pos = 0;
    resampler->start = 0;
    resampler->end = frames;
    resampler->pickup = &it_xfade_pickup_stop;
    playing->xfade_state = XFADE_REL_TAIL;
}

/* Pickup while the held virtual loop reaches its end. */
static void xfade_pickup_loop(DUMB_RESAMPLER *resampler, void *data) {
    IT_PLAYING *playing = (IT_PLAYING *)data;
    ASSERT(playing->xfade_state == XFADE_PRE ||
           playing->xfade_state == XFADE_MIX);
    /* Ordinary held wrap around one virtual period. */
    resampler->pos -= resampler->end - resampler->start;
    playing->xfade_state = XFADE_MIX;
}

void it_xfade_pickup_stop(DUMB_RESAMPLER *resampler, void *data) {
    (void)data;
    if (resampler->dir < 0) {
        resampler->pos = (resampler->start << 1) - 1 - resampler->pos;
        resampler->subpos ^= 65535;
        resampler->dir = 1;
    } else {
        resampler->dir = 0;
    }
}

/* Build the virtual source and put the voice's existing resampler over it.
 * Leaves the voice on the legacy path (xfade_state == XFADE_OFF) if anything
 * rules the feature out or allocation fails.
 */
void _dumb_it_xfade_start(IT_PLAYING *playing, long initial_pos) {
    IT_SAMPLE *sample = playing->sample;
    long s, e, c, period, steady, loop_start, virtual_frames, v, i;
    int channels, quality;
    short *buf;
    long vstart;

    /* May be re-entered on a retrigger or sample-offset effect: drop any
     * virtual source the voice previously owned. */
    _dumb_it_xfade_free(playing);
    playing->xfade_state = XFADE_OFF;
    playing->xfade_buf_frames = 0;
    playing->xfade_width = 0;

    if (playing->xfade_request <= 0)
        return;

    if (!(sample->flags & IT_SAMPLE_SUS_LOOP))
        return;
    if (sample->flags & IT_SAMPLE_PINGPONG_SUS_LOOP)
        return;
    if (sample->flags & IT_SAMPLE_LOOP)
        return;
    if (playing->flags & IT_PLAYING_REVERSE)
        return;

    quality = playing->resampling_quality;
    if (sample->max_resampling_quality >= 0 &&
        quality > sample->max_resampling_quality)
        quality = sample->max_resampling_quality;
    if (!_dumb_it_xfade_quality_supported(quality))
        return;

    s = sample->sus_loop_start;
    e = sample->sus_loop_end;
    if (!(e > s))
        return;

    c = playing->xfade_request;
    if (c > (e - s) / 2)
        c = (e - s) / 2;
    if (c <= 0)
        return;

    steady = e - s - 2 * c;        /* plain physical frames per period */
    period = steady + c;          /* == E-S-C */
    if (period <= 0 || steady < 0)
        return;

    channels = source_channels(playing);
    loop_start = e - c;
    virtual_frames = loop_start + period;

    buf = malloc((size_t)virtual_frames * channels * sizeof(short));
    if (!buf)
        return;

    /* Attack prefix: physical frames 0 .. E-C-1, unchanged. */
    for (v = 0; v < loop_start; v++)
        copy_frame(playing, buf + v * channels, v, channels);

    /* One repeating period, blend first so entry from the attack lands on the
     * first blend frame exactly as the specification describes.
     *   tile[0 .. C-1]      : blend tail E-C+j over head S+j
     *   tile[C .. period-1] : physical S+C .. E-C-1
     */
    for (i = 0; i < c; i++)
        blend_frame(playing, buf + (loop_start + i) * channels, e - c + i,
                    s + i, i, c, channels);
    for (i = 0; i < steady; i++)
        copy_frame(playing, buf + (loop_start + c + i) * channels,
                   s + c + i, channels);

    playing->xfade_buf = buf;
    playing->xfade_buf_frames = virtual_frames;
    playing->xfade_width = c;
    playing->xfade_state = XFADE_PRE;

    /* Map the requested physical starting position onto the virtual source. */
    if (initial_pos < loop_start) {
        vstart = initial_pos;
    } else if (initial_pos < s + c) {
        /* Head region only reached via a blend; align to the matching frame. */
        vstart = loop_start + (initial_pos - s);
    } else if (initial_pos < e) {
        long local = initial_pos - (s + c);
        if (local >= steady)
            local = steady - 1;
        vstart = loop_start + c + local;
    } else {
        vstart = loop_start;
    }
    if (vstart < 0)
        vstart = 0;
    if (vstart >= virtual_frames)
        vstart = virtual_frames - 1;

    dumb_reset_resampler_16(&playing->resampler, buf, channels, vstart,
                            loop_start, loop_start + period, quality);
    playing->resampler.pickup = &xfade_pickup_loop;
    playing->resampler.pickup_data = playing;
}

/* Key-off. Decided purely from the voice's current integer source position;
 * interpolation read-ahead never influences it. */
void _dumb_it_xfade_note_off(IT_PLAYING *playing) {
    DUMB_RESAMPLER *resampler;
    IT_SAMPLE *sample;
    long s, e, c, steady, period, loop_start, pos, local, phys_start, frames, i;
    int channels;
    short *release;

    if (playing->xfade_state == XFADE_OFF ||
        playing->xfade_state == XFADE_REL_MIX ||
        playing->xfade_state == XFADE_REL_TAIL)
        return;

    resampler = &playing->resampler;
    sample = playing->sample;
    s = sample->sus_loop_start;
    e = sample->sus_loop_end;
    c = playing->xfade_width;
    steady = e - s - 2 * c;
    period = steady + c;
    loop_start = e - c;
    pos = resampler->pos;

    if (pos >= loop_start) {
        local = pos - loop_start;
        local %= period;
        if (local < c) {
            /* Already inside the crossfade: finish this blend, then install the
             * S+C release tail at the blend boundary without another loop. */
            playing->xfade_state = XFADE_REL_MIX;
            resampler->end = loop_start + c;
            resampler->pickup = &xfade_pickup_relmix;
            return;
        }
        phys_start = s + c + (local - c);
    } else {
        /* Still in the attack prefix. */
        phys_start = pos;
    }

    /* Not yet in the crossfade: cancel every future loop and play straight on
     * from the current physical position to the original sample end.
     */
    frames = sample->length - phys_start;
    channels = source_channels(playing);
    if (frames <= 0) {
        resampler->dir = 0;
        playing->xfade_state = XFADE_REL_TAIL;
        return;
    }

    release = malloc((size_t)frames * channels * sizeof(short));
    if (!release) {
        resampler->dir = 0;
        playing->xfade_state = XFADE_REL_TAIL;
        return;
    }

    for (i = 0; i < frames; i++)
        copy_frame(playing, release + i * channels, phys_start + i, channels);

    free(playing->xfade_buf);
    playing->xfade_buf = release;
    playing->xfade_buf_frames = frames;
    resampler->src = release;
    resampler->pos = 0;
    resampler->start = 0;
    resampler->end = frames;
    resampler->pickup = &it_xfade_pickup_stop;
    playing->xfade_state = XFADE_REL_TAIL;
}

void _dumb_it_xfade_free(IT_PLAYING *playing) {
    if (playing->xfade_buf) {
        free(playing->xfade_buf);
        playing->xfade_buf = NULL;
        playing->xfade_buf_frames = 0;
    }
}

/* Pickup used only if a duplicate voice cannot allocate its own virtual
 * buffer: play the real sample straight through and stop at its end. */
static void xfade_pickup_stop_plain(DUMB_RESAMPLER *resampler, void *data) {
    (void)data;
    if (resampler->dir < 0) {
        resampler->pos = (resampler->start << 1) - 1 - resampler->pos;
        resampler->subpos ^= 65535;
        resampler->dir = 1;
    } else {
        resampler->dir = 0;
    }
}

/* After dup_playing() copied the resampler (which points at the source's
 * virtual buffer), give the duplicate its own private buffer and repoint it.
 */
void _dumb_it_xfade_dup_fixup(IT_PLAYING *dst, const IT_PLAYING *src) {
    int channels;
    size_t bytes;

    dst->xfade_buf = NULL;
    if (src->xfade_state == XFADE_OFF || !src->xfade_buf)
        return;

    channels = source_channels(dst);
    bytes = (size_t)src->xfade_buf_frames * channels * sizeof(short);
    dst->xfade_buf = malloc(bytes);
    if (!dst->xfade_buf) {
        /* Out of memory: cannot keep the crossfade on this duplicate. Rebuild a
         * correctly-typed legacy resampler over the real sample data so the
         * width-specific dispatch below never sees a mismatched source. */
        int bits = (dst->sample->flags & IT_SAMPLE_16BIT) ? 16 : 8;
        dumb_reset_resampler_n(bits, &dst->resampler, dst->sample->data,
                               channels, 0, 0, dst->sample->length,
                               dst->resampling_quality);
        dst->resampler.pickup = &xfade_pickup_stop_plain;
        dst->resampler.pickup_data = dst;
        dst->xfade_state = XFADE_OFF;
        dst->xfade_width = 0;
        return;
    }
    memcpy(dst->xfade_buf, src->xfade_buf, bytes);
    dst->resampler.src = dst->xfade_buf;
}

long _dumb_it_xfade_resample(IT_PLAYING *playing, int dst_channels,
                             sample_t *dst, long dst_size,
                             DUMB_VOLUME_RAMP_INFO *volume_left,
                             DUMB_VOLUME_RAMP_INFO *volume_right,
                             float delta) {
    int stereo_source = source_channels(playing) == 2;

    if (dst_channels >= 2) {
        if (stereo_source)
            return dumb_resample_16_2_2(&playing->resampler, dst, dst_size,
                                        volume_left, volume_right, delta);
        return dumb_resample_16_1_2(&playing->resampler, dst, dst_size,
                                    volume_left, volume_right, delta);
    }

    if (stereo_source)
        return dumb_resample_16_2_1(&playing->resampler, dst, dst_size,
                                    volume_left, volume_right, delta);
    return dumb_resample_16_1_1(&playing->resampler, dst, dst_size,
                                volume_left, delta);
}

void _dumb_it_xfade_get_current_sample(IT_PLAYING *playing, int dst_channels,
                                       DUMB_VOLUME_RAMP_INFO *volume_left,
                                       DUMB_VOLUME_RAMP_INFO *volume_right,
                                       sample_t *dst) {
    int stereo_source = source_channels(playing) == 2;

    if (dst_channels >= 2) {
        if (stereo_source)
            dumb_resample_get_current_sample_16_2_2(
                &playing->resampler, volume_left, volume_right, dst);
        else
            dumb_resample_get_current_sample_16_1_2(
                &playing->resampler, volume_left, volume_right, dst);
        return;
    }

    if (stereo_source)
        dumb_resample_get_current_sample_16_2_1(&playing->resampler,
                                                volume_left, volume_right, dst);
    else
        dumb_resample_get_current_sample_16_1_1(&playing->resampler,
                                                volume_left, dst);
}
