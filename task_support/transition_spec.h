#ifndef TASK_TRANSITION_SPEC_H
#define TASK_TRANSITION_SPEC_H

#include <stdint.h>

/*
 * Shared edit-list specification for the chained xfade / acrossfade
 * regression.  Everything in the generator (transition_assets.c), the
 * independent oracle (transition_oracle.c) and the regression driver
 * (transition_cases.c) is derived from the two tables below so that the
 * same edit list drives both picture and sound.
 *
 * The oracle links only against libc and libm; this header therefore
 * carries no FFmpeg dependency.
 */

#define TS_WIDTH        8
#define TS_HEIGHT       8
#define TS_PLANE_SIZE   (TS_WIDTH * TS_HEIGHT)
#define TS_CHANNELS     2
#define TS_SAMPLE_RATE  48000

typedef struct TransitionCase {
    int         id;               /* case selector: 25 or 30 */
    int         fps_num;          /* video frame rate numerator        */
    int         fps_den;          /* video frame rate denominator      */
    int         frames_per_seg;   /* source frames in each of 3 shots  */
    int         overlap_frames;   /* video overlap / transition length */
    int         total_frames;     /* frames in the finished program    */
    int         audio_per_seg;    /* audio sample frames in each shot  */
    int         overlap_samples;  /* audio overlap / crossfade length  */
    int         total_samples;    /* sample frames in the program      */
    const char *name;
} TransitionCase;

/* 25 fps: 3x25 frames / 3x48000 samples, overlaps 5 frames / 9600 samples. */
const TransitionCase *ts_case_25(void);
/* 30000/1001 fps: 3x30 frames / 3x48048 samples, overlaps 10 / 16016. */
const TransitionCase *ts_case_30000(void);
const TransitionCase *ts_find(int id);

/*
 * Source picture material.  Value depends on the source shot, the source
 * frame number and the pixel position, so a constant frame cannot mask a
 * wrong source.  Bounded to [16, 215].
 */
uint8_t ts_source_pixel(const TransitionCase *c, int seg, int frame, int x, int y);

/*
 * Source sound material.  The two channels carry different bounded
 * markers and every shot uses different periods, so silence or a
 * constant tone cannot mask a wrong source or a swapped channel.
 * channel 0 = left, channel 1 = right.
 */
float ts_source_sample(const TransitionCase *c, int seg, int channel, int sample);

/*
 * Timeline mapping for a finished-program video frame (0-based).
 * Returns 0 for a pure source frame and 1 for a frame inside a transition;
 * seg0 and frame0 describe the first-input source position,
 * seg1 and frame1 the second-input source position (untouched when pure),
 * *k receives the transition index and *n the transition length.
 */
int ts_video_origin(const TransitionCase *c, int out_frame,
                    int *seg0, int *frame0, int *seg1, int *frame1,
                    int *k, int *n);

/*
 * Timeline mapping for a finished-program audio sample frame (0-based).
 * Same convention as ts_video_origin(): a crossfade sample reports both
 * source positions plus its index k and the overlap length n.
 */
int ts_audio_origin(const TransitionCase *c, int out_sample,
                    int *seg0, int *sample0, int *seg1, int *sample1,
                    int *k, int *n);

#endif /* TASK_TRANSITION_SPEC_H */
