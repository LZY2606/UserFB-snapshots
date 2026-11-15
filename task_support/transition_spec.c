#include "transition_spec.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const TransitionCase case_25 = {
    .id              = 25,
    .fps_num         = 25,
    .fps_den         = 1,
    .frames_per_seg  = 25,
    .overlap_frames  = 5,
    .total_frames    = 65,       /* 3*25 - 2*5  */
    .audio_per_seg   = 48000,
    .overlap_samples = 9600,
    .total_samples   = 124800,   /* 3*48000 - 2*9600 */
    .name            = "r25",
};

static const TransitionCase case_30000 = {
    .id              = 30,
    .fps_num         = 30000,
    .fps_den         = 1001,
    .frames_per_seg  = 30,
    .overlap_frames  = 10,
    .total_frames    = 70,       /* 3*30 - 2*10 */
    .audio_per_seg   = 48048,
    .overlap_samples = 16016,
    .total_samples   = 112112,   /* 3*48048 - 2*16016 */
    .name            = "r30000-1001",
};

const TransitionCase *ts_case_25(void)    { return &case_25; }
const TransitionCase *ts_case_30000(void) { return &case_30000; }

const TransitionCase *ts_find(int id)
{
    if (id == 25)
        return &case_25;
    if (id == 30)
        return &case_30000;
    return 0;
}

uint8_t ts_source_pixel(const TransitionCase *c, int seg, int frame, int x, int y)
{
    /* Every term is source-position dependent; clips stay well inside 8 bit. */
    unsigned v = (unsigned)(16u
                 + (unsigned)seg * 41u
                 + (unsigned)frame * 7u
                 + (unsigned)x * 3u
                 + (unsigned)y * 5u
                 + (unsigned)(c->fps_den % 7u));
    v = 16u + (v % 200u);           /* 16..215, never saturates the blend */
    return (uint8_t)v;
}

float ts_source_sample(const TransitionCase *c, int seg, int channel, int sample)
{
    /* Distinct bounded markers per shot and channel. */
    const double t  = (double)(sample + 1);
    const double base = (double)(seg - 1) * 0.05;
    double v;

    if (channel == 0) {
        const double period = 97.0 + 23.0 * seg + 0.01 * c->fps_num;
        v = 0.30 * sin(2.0 * M_PI * t / period) + base;
    } else {
        const double period = 71.0 + 17.0 * seg + 0.01 * c->fps_den;
        v = 0.25 * cos(2.0 * M_PI * t / period) - base;
    }
    return (float)v;               /* |v| <= 0.4, safely inside [-1, 1] */
}

/*
 * Chained-overlap timeline, matching the verified xfade / acrossfade
 * scheduling.  For shots of length L joined with an overlap of length n
 * (s = L - n is the transition start in finished-program index):
 *   [0, s)        : shot 0, index o
 *   [s, L)        : blend shot0[s+k] with shot1[k], k = o - s
 *   [L, 2s)       : shot 1, index o - s
 *   [2s, 2s + n)  : blend shot1[s+k] with shot2[k]
 *   [2s + n, ...) : shot 2, index o - 2s
 * The finished length 3L - 2n follows from the tail shot2[n .. L-1].
 *
 * xfade renders n transition frames with progress 1 - k/n (the k=0
 * frame is therefore pure first input); acrossfade renders n crossfade
 * samples with the tri / qsin endpoint gains documented in the oracle.
 */
int ts_video_origin(const TransitionCase *c, int out_frame,
                    int *seg0, int *frame0, int *seg1, int *frame1,
                    int *k, int *n)
{
    const int L = c->frames_per_seg;
    const int ov = c->overlap_frames;
    const int s = L - ov;
    int mixed = 0;
    int s0 = 0, f0 = 0, s1 = 0, f1 = 0, kk = 0;

    if (out_frame < s) {
        s0 = 0; f0 = out_frame;
    } else if (out_frame < L) {
        kk = out_frame - s;
        s0 = 0; f0 = s + kk;
        s1 = 1; f1 = kk;
        mixed = 1;
    } else if (out_frame < 2 * s) {
        s0 = 1; f0 = out_frame - s;
    } else if (out_frame < 2 * s + ov) {
        kk = out_frame - 2 * s;
        s0 = 1; f0 = s + kk;
        s1 = 2; f1 = kk;
        mixed = 1;
    } else {
        s0 = 2; f0 = out_frame - 2 * s;
    }

    if (seg0)   *seg0 = s0;
    if (frame0) *frame0 = f0;
    if (seg1)   *seg1 = s1;
    if (frame1) *frame1 = f1;
    if (k)      *k = kk;
    if (n)      *n = ov;
    return mixed;
}

int ts_audio_origin(const TransitionCase *c, int out_sample,
                    int *seg0, int *sample0, int *seg1, int *sample1,
                    int *k, int *n)
{
    const int L = c->audio_per_seg;
    const int ov = c->overlap_samples;
    const int s = L - ov;
    int mixed = 0;
    int s0 = 0, p0 = 0, s1 = 0, p1 = 0, kk = 0;

    if (out_sample < s) {
        s0 = 0; p0 = out_sample;
    } else if (out_sample < L) {
        kk = out_sample - s;
        s0 = 0; p0 = s + kk;
        s1 = 1; p1 = kk;
        mixed = 1;
    } else if (out_sample < 2 * s) {
        s0 = 1; p0 = out_sample - s;
    } else if (out_sample < 2 * s + ov) {
        kk = out_sample - 2 * s;
        s0 = 1; p0 = s + kk;
        s1 = 2; p1 = kk;
        mixed = 1;
    } else {
        s0 = 2; p0 = out_sample - 2 * s;
    }

    if (seg0)    *seg0 = s0;
    if (sample0) *sample0 = p0;
    if (seg1)    *seg1 = s1;
    if (sample1) *sample1 = p1;
    if (k)       *k = kk;
    if (n)       *n = ov;
    return mixed;
}
