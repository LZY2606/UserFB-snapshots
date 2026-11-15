/*
 * transition_example.c - tiny standalone example of the transition
 * reference API.
 *
 * It prints, for each edit list, the analytic source position and value
 * at a few finished-program indices: a pure video frame, a frame inside
 * the first xfade, and a stereo sample inside the second acrossfade.
 *
 * Build (from the repository root):
 *   cc -std=c17 -O2 -Wall -Wextra -Itask_support \
 *      task_support/transition_spec.c task_support/transition_example.c \
 *      -o transition_example -lm
 */

#include <math.h>
#include <stdio.h>

#include "transition_spec.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void show_video(const TransitionCase *c, int frame)
{
    int s0, f0, s1, f1, k, n;
    int mixed = ts_video_origin(c, frame, &s0, &f0, &s1, &f1, &k, &n);

    if (!mixed) {
        printf("  video frame %d: pure seg%d frame%d pixel(0,0)=%u\n",
               frame, s0, f0,
               ts_source_pixel(c, s0, f0, 0, 0));
        return;
    }

    unsigned a = ts_source_pixel(c, s0, f0, 0, 0);
    unsigned b = ts_source_pixel(c, s1, f1, 0, 0);
    double blend = (double)a * (n - k) / n + (double)b * k / n;
    printf("  video frame %d: xfade k=%d/%d seg%d:f%d(%u) + seg%d:f%d(%u)"
           " blend=%.3f\n",
           frame, k, n, s0, f0, a, s1, f1, b, blend);
}

static void show_audio(const TransitionCase *c, int sample)
{
    int s0, p0, s1, p1, k, n;
    int mixed = ts_audio_origin(c, sample, &s0, &p0, &s1, &p1, &k, &n);

    if (!mixed) {
        printf("  audio sample %d: pure seg%d sample%d L=%.6f R=%.6f\n",
               sample, s0, p0,
               ts_source_sample(c, s0, 0, p0),
               ts_source_sample(c, s0, 1, p0));
        return;
    }

    for (int ch = 0; ch < TS_CHANNELS; ch++) {
        float a = ts_source_sample(c, s0, ch, p0);
        float b = ts_source_sample(c, s1, ch, p1);
        double w0 = (double)(n - 1 - k) / n;                          /* tri  */
        double w1 = sin(((double)k / n) * M_PI / 2.0);                /* qsin */
        printf("  audio sample %d: acrossfade k=%d/%d %s "
               "seg%d:s%d(%.6f)*%.4f + seg%d:s%d(%.6f)*%.4f = %.6f\n",
               sample, k, n, ch ? "right" : "left ",
               s0, p0, a, w0, s1, p1, b, w1,
               a * w0 + b * w1);
    }
}

int main(void)
{
    const TransitionCase *cases[] = { ts_case_25(), ts_case_30000() };

    for (int i = 0; i < 2; i++) {
        const TransitionCase *c = cases[i];
        printf("case %s: %d frames, %d samples\n",
               c->name, c->total_frames, c->total_samples);
        show_video(c, 0);
        show_video(c, c->frames_per_seg - c->overlap_frames + 1);
        /* first sample inside the second acrossfade region */
        show_audio(c, 2 * (c->audio_per_seg - c->overlap_samples) + 1);
    }
    return 0;
}
