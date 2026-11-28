/*
 * transition_gen: generate the three deterministic source clips for one
 * transition edit list.
 *
 * Usage: transition_gen <output-dir> <spec-id>
 *
 * Writes, for each of the three segments, an 8-bit gray rawvideo file and a
 * packed stereo pcm_f32le file:
 *   <dir>/<id>_v<0..2>.gray   F * W * H bytes each
 *   <dir>/<id>_a<0..2>.f32    A * 2 * 4 bytes each
 *
 * Only the C standard library is used.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transition_spec.h"

static const struct transition_spec *find_spec(const char *id)
{
    int i;

    for (i = 0; i < TASK_SPEC_COUNT; ++i)
        if (!strcmp(transition_specs[i].id, id))
            return &transition_specs[i];
    return NULL;
}

static int write_video(const struct transition_spec *s, const char *dir, int seg)
{
    char path[1024];
    FILE *fp;
    int frame, pixel;

    snprintf(path, sizeof(path), "%s/%s_v%d.gray", dir, s->id, seg);
    fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        return 1;
    }
    for (frame = 0; frame < s->frames_per_seg; ++frame)
        for (pixel = 0; pixel < TASK_PIXELS; ++pixel) {
            uint8_t v = transition_source_pixel(seg, frame, pixel);
            if (fwrite(&v, 1, 1, fp) != 1) {
                perror(path);
                fclose(fp);
                return 1;
            }
        }
    if (fclose(fp)) {
        perror(path);
        return 1;
    }
    return 0;
}

static int write_audio(const struct transition_spec *s, const char *dir, int seg)
{
    char path[1024];
    FILE *fp;
    long sample;
    int channel;

    snprintf(path, sizeof(path), "%s/%s_a%d.f32", dir, s->id, seg);
    fp = fopen(path, "wb");
    if (!fp) {
        perror(path);
        return 1;
    }
    for (sample = 0; sample < s->samples_per_seg; ++sample)
        for (channel = 0; channel < TASK_CHANNELS; ++channel) {
            float v = transition_source_sample(seg, sample, channel);
            if (fwrite(&v, sizeof(float), 1, fp) != 1) {
                perror(path);
                fclose(fp);
                return 1;
            }
        }
    if (fclose(fp)) {
        perror(path);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const struct transition_spec *s;
    int seg;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <output-dir> <spec-id>\n", argv[0]);
        return 2;
    }
    s = find_spec(argv[2]);
    if (!s) {
        fprintf(stderr, "unknown spec id '%s'\n", argv[2]);
        return 2;
    }
    for (seg = 0; seg < TASK_SEGMENTS; ++seg) {
        if (write_video(s, argv[1], seg) || write_audio(s, argv[1], seg))
            return 1;
    }
    printf("GEN %s video=%dx%d rate=%d/%d frames_per_seg=%d overlap_frames=%d "
           "output_frames=%d audio=%dHz stereo samples_per_seg=%d "
           "overlap_samples=%d output_samples=%ld\n",
           s->id, TASK_WIDTH, TASK_HEIGHT, s->rate_num, s->rate_den,
           s->frames_per_seg, s->overlap_frames,
           transition_output_frames(s), TASK_SAMPLE_RATE,
           s->samples_per_seg, s->overlap_samples,
           transition_output_samples(s));
    return 0;
}
