/*
 * Transition regression support: shared edit-list specification and the
 * deterministic source material generators.
 *
 * Everything in this header is plain C99/C11: no FFmpeg headers, no libm
 * dependency.  Both the material generator (transition_gen.c) and the
 * independent reference (transition_oracle.c) include this file so the two
 * edit lists live in exactly one place.
 */
#ifndef TASK_TRANSITION_SPEC_H
#define TASK_TRANSITION_SPEC_H

#include <stdint.h>
#include <math.h>

#define TASK_SEGMENTS     3
#define TASK_WIDTH        8
#define TASK_HEIGHT       8
#define TASK_PIXELS       (TASK_WIDTH * TASK_HEIGHT)
#define TASK_CHANNELS     2
#define TASK_SAMPLE_RATE  48000

struct transition_spec {
    const char *id;       /* short id used in file names and logs */
    int rate_num;         /* video frame rate numerator            */
    int rate_den;         /* video frame rate denominator          */
    int frames_per_seg;   /* F: source frames per clip             */
    int overlap_frames;   /* V: video overlap length in frames     */
    int samples_per_seg;  /* A: audio sample frames per clip       */
    int overlap_samples;  /* N: acrossfade length in sample frames */
    int tb_num;           /* video timebase forced before xfade    */
    int tb_den;
    int pts_step;         /* source frame pts increment in tb      */
};

static const struct transition_spec transition_specs[] = {
    /* 25 fps: 25 frames / 48000 samples per clip, 5 frame / 9600 overlap */
    { "r25",   25, 1, 25,  5, 48000,  9600, 1,    25,    1 },
    /* 30000/1001 fps: 30 frames / 48048 samples, 10 frame / 16016 overlap */
    { "ntsc",  30000, 1001, 30, 10, 48048, 16016, 1, 30000, 1001 },
};

#define TASK_SPEC_COUNT (int)(sizeof(transition_specs) / sizeof(transition_specs[0]))

/*
 * Video source material, gray8.  The value changes with the segment, the
 * source frame and the pixel position, and wraps over the full 0..255 range
 * so rounding near both code limits is exercised.  No two (segment, frame)
 * pair shares a constant frame, which keeps mis-attributed sources visible.
 */
static inline uint8_t transition_source_pixel(int seg, int frame, int pixel)
{
    unsigned v = 20u + 37u * (unsigned)seg + 17u * (unsigned)frame
               + 5u * (unsigned)pixel;
    return (uint8_t)(v & 0xffu);
}

/*
 * Audio source material, packed stereo pcm_f32le.  Each channel carries its
 * own bounded marker (constant offset plus a sine whose frequency depends on
 * the segment) so the two channels and the three segments never collapse
 * into silence or into each other.
 */
static inline float transition_source_sample(int seg, long sample, int channel)
{
    static const float amp_l[TASK_SEGMENTS]   = { 0.30f, 0.24f, 0.27f };
    static const float off_l[TASK_SEGMENTS]   = { 0.05f, -0.08f, 0.10f };
    static const int   freq_l[TASK_SEGMENTS]  = { 220, 330, 277 };
    static const float amp_r[TASK_SEGMENTS]   = { 0.20f, 0.22f, 0.18f };
    static const float off_r[TASK_SEGMENTS]   = { -0.10f, 0.12f, -0.04f };
    static const int   freq_r[TASK_SEGMENTS]  = { 440, 550, 660 };
    const float phase = 6.283185307179586f * (float)sample
                      / (float)TASK_SAMPLE_RATE;
    float v;

    if (channel == 0)
        v = off_l[seg] + amp_l[seg] * sinf(phase * (float)freq_l[seg]);
    else
        v = off_r[seg] + amp_r[seg] * sinf(phase * (float)freq_r[seg]);
    return v;
}

/* Resulting timeline lengths, shared by generator and reference. */
static inline int transition_output_frames(const struct transition_spec *s)
{
    return TASK_SEGMENTS * s->frames_per_seg
         - (TASK_SEGMENTS - 1) * s->overlap_frames;
}

static inline long transition_output_samples(const struct transition_spec *s)
{
    return (long)TASK_SEGMENTS * s->samples_per_seg
         - (long)(TASK_SEGMENTS - 1) * s->overlap_samples;
}

#endif /* TASK_TRANSITION_SPEC_H */
