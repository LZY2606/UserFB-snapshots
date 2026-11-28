/*
 * transition_example: tiny, self-contained illustration of the reference
 * math used by the transition regression.
 *
 * It links only against libm and includes the shared edit-list header; it
 * never calls FFmpeg.  For the 25 fps edit list it prints, for one point in
 * each of the two chained overlaps:
 *   - the two source positions being mixed,
 *   - the tri weights (N-1-k)/N and k/N,
 *   - the qsin weights (quarter sine applied to each tri weight),
 *   - the resulting gray pixel and stereo sample values.
 *
 * Build: cc -std=c17 -O2 -Wall -Wextra -I. \
 *          task_support/transition_example.c -o example -lm
 */
#include <stdio.h>
#include <math.h>

#include "transition_spec.h"

static double tri(double w)
{
    return w;
}

static double qsin(double w)
{
    return sin(w * (M_PI / 2.0));
}

static void show_video(const struct transition_spec *s)
{
    const int first_start = s->frames_per_seg - s->overlap_frames;
    const int frame = first_start + 2;          /* k = 2 inside xfade #1 */
    const int k = frame - first_start;
    const double w0 = 1.0 - (double)k / s->overlap_frames;
    const double w1 = (double)k / s->overlap_frames;

    printf("video xfade#1 output frame %d (k=%d, N=%d)\n", frame, k,
           s->overlap_frames);
    for (int pixel = 0; pixel < 3; ++pixel) {
        uint8_t a = transition_source_pixel(0, frame, pixel);
        uint8_t b = transition_source_pixel(1, k, pixel);
        double mixed = a * w0 + b * w1;
        printf("  pixel=%d seg0:frame%d=%d seg1:frame%d=%d "
               "tri(%.4f,%.4f) -> %.4f (stored %u)\n",
               pixel, frame, a, k, b, w0, w1, mixed, (unsigned)(uint8_t)mixed);
    }
}

static void show_audio(const struct transition_spec *s)
{
    const long first_start = s->samples_per_seg - s->overlap_samples;
    const long sample = first_start + 3;          /* k = 3, crossfade #1 */
    const long k = sample - first_start;
    const double w0 = (double)(s->overlap_samples - 1 - k)
                    / s->overlap_samples;
    const double w1 = (double)k / s->overlap_samples;

    printf("audio acrossfade#1 output sample %ld (k=%ld, N=%d)\n", sample,
           k, s->overlap_samples);
    for (int ch = 0; ch < TASK_CHANNELS; ++ch) {
        float a = transition_source_sample(0, sample, ch);
        float b = transition_source_sample(1, k, ch);
        double tri_mix = a * tri(w0) + b * tri(w1);
        double qsin_mix = a * qsin(w0) + b * qsin(w1);
        printf("  ch=%d seg0:sample%ld=%.6f seg1:sample%ld=%.6f\n"
               "        tri  (%.6f,%.6f) -> %.9g\n"
               "        qsin (%.6f,%.6f) -> %.9g\n",
               ch, sample, a, k, b, w0, w1, tri_mix,
               qsin(w0), qsin(w1), qsin_mix);
    }
}

int main(void)
{
    const struct transition_spec *s = &transition_specs[0]; /* 25 fps */

    printf("edit list '%s': %d frames/clip, %d frame overlap, "
           "%d samples/clip, %d sample overlap\n",
           s->id, s->frames_per_seg, s->overlap_frames,
           s->samples_per_seg, s->overlap_samples);
    printf("expected timeline: %d video frames, %ld audio sample frames\n\n",
           transition_output_frames(s), transition_output_samples(s));
    show_video(s);
    putchar('\n');
    show_audio(s);
    return 0;
}
