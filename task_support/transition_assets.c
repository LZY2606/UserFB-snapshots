/*
 * transition_assets.c - generate the raw picture and sound material that
 * drives the chained xfade / acrossfade regression.
 *
 * The output is deliberately raw (gray8 rawvideo and stereo pcm_f32le):
 * no demuxer, no decoder and no lavfi source stand between the regression
 * and these bytes, so every value can be traced back to ts_source_*().
 *
 * Usage: transition_assets <case id: 25|30> <output directory>
 * Writes v0.gray v1.gray v2.gray a0.f32 a1.f32 a2.f32 and a manifest.
 *
 * Links only against libc and libm.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transition_spec.h"

static int write_all(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "transition_assets: cannot open %s: %s\n",
                path, strerror(errno));
        return -1;
    }
    if (fwrite(buf, 1, len, f) != len) {
        fprintf(stderr, "transition_assets: short write on %s\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    const TransitionCase *c;
    char path[1024];
    uint8_t *frame;
    float *audio;
    int seg, fr, s, x, y;
    FILE *manifest;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <25|30> <output directory>\n", argv[0]);
        return 2;
    }
    c = ts_find(atoi(argv[1]));
    if (!c) {
        fprintf(stderr, "transition_assets: unknown case id '%s'\n", argv[1]);
        return 2;
    }

    frame = malloc((size_t)c->frames_per_seg * TS_PLANE_SIZE);
    audio = malloc((size_t)c->audio_per_seg * TS_CHANNELS * sizeof(float));
    if (!frame || !audio) {
        fprintf(stderr, "transition_assets: out of memory\n");
        return 1;
    }

    for (seg = 0; seg < 3; seg++) {
        for (fr = 0; fr < c->frames_per_seg; fr++)
            for (y = 0; y < TS_HEIGHT; y++)
                for (x = 0; x < TS_WIDTH; x++)
                    frame[fr * TS_PLANE_SIZE + y * TS_WIDTH + x] =
                        ts_source_pixel(c, seg, fr, x, y);
        snprintf(path, sizeof(path), "%s/v%d.gray", argv[2], seg);
        if (write_all(path, frame,
                      (size_t)c->frames_per_seg * TS_PLANE_SIZE))
            return 1;

        for (s = 0; s < c->audio_per_seg; s++)
            for (int ch = 0; ch < TS_CHANNELS; ch++)
                audio[s * TS_CHANNELS + ch] =
                    ts_source_sample(c, seg, ch, s);
        snprintf(path, sizeof(path), "%s/a%d.f32", argv[2], seg);
        if (write_all(path, audio,
                      (size_t)c->audio_per_seg * TS_CHANNELS * sizeof(float)))
            return 1;
    }

    snprintf(path, sizeof(path), "%s/manifest.txt", argv[2]);
    manifest = fopen(path, "wb");
    if (!manifest) {
        fprintf(stderr, "transition_assets: cannot open %s: %s\n",
                path, strerror(errno));
        return 1;
    }
    fprintf(manifest,
            "case=%s fps=%d/%d frames_per_seg=%d overlap_frames=%d "
            "total_frames=%d audio_per_seg=%d overlap_samples=%d "
            "total_samples=%d sample_rate=%d channels=%d plane=%dx%d\n",
            c->name, c->fps_num, c->fps_den, c->frames_per_seg,
            c->overlap_frames, c->total_frames, c->audio_per_seg,
            c->overlap_samples, c->total_samples, TS_SAMPLE_RATE,
            TS_CHANNELS, TS_WIDTH, TS_HEIGHT);
    fclose(manifest);

    free(frame);
    free(audio);
    printf("ASSETS case=%s dir=%s video_shots=3 audio_shots=3\n",
           c->name, argv[2]);
    return 0;
}
