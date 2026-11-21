/*  examples/dumbseek.c - Seek to an exact PCM frame and write PCM.
 *
 *  Loads a module, builds a deterministic seek index at a fixed sample rate,
 *  then starts an independent renderer at a requested output frame and
 *  writes a short block of 16-bit signed PCM to stdout (or a file).
 *
 *  Usage:
 *    dumbseek <module> <frame> [frames] [output.pcm]
 *
 *  The output is byte-for-byte identical to the same span of the seeded
 *  sequential playback, which is what makes the seek usable for sample-exact
 *  preview, looping and scrubbing.
 *
 *  Built with the `dumbseek` CMake target (no external dependencies).
 */

#include <stdio.h>
#include <stdlib.h>

#include "dumb.h"

#define SAMPLE_RATE 44100
#define CHANNELS 2
#define QUALITY DUMB_RQ_FIR
#define INDEX_LIMIT (10 * 60 * SAMPLE_RATE) /* ten minutes at most */
#define INDEX_STRIDE 4096u
#define BLOCK 4096

int main(int argc, char **argv) {
    const char *path;
    unsigned long long frame;
    unsigned long frames_wanted = 4096;
    const char *output = NULL;
    DUH *duh;
    DUMB_IT_SEEK_INDEX *index;
    DUH_SIGRENDERER *renderer;
    unsigned preroll = 0;
    unsigned long long covered;
    FILE *out = stdout;
    sample_t **samples;
    long done = 0;
    unsigned char *pcm;

    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <module> <frame> [frames] [output.pcm]\n", argv[0]);
        return 2;
    }

    path = argv[1];
    frame = strtoull(argv[2], NULL, 10);
    if (argc > 3)
        frames_wanted = (unsigned long)strtoul(argv[3], NULL, 10);
    if (argc > 4)
        output = argv[4];

    dumb_register_stdfiles();

    duh = dumb_load_any(path, 0, 0);
    if (!duh) {
        fprintf(stderr, "cannot load %s\n", path);
        return 1;
    }

    /* Build the index once; keep it around for further seeks. */
    index = dumb_it_build_seek_index(duh, CHANNELS, SAMPLE_RATE, QUALITY, 0,
                                     INDEX_LIMIT, INDEX_STRIDE);
    if (!index) {
        fprintf(stderr, "cannot build seek index for %s\n", path);
        unload_duh(duh);
        return 1;
    }

    covered = dumb_it_seek_index_frames(index);
    if (frame > covered) {
        fprintf(stderr, "frame %llu is past the covered range (%llu frames)\n",
                frame, covered);
        dumb_it_free_seek_index(index);
        unload_duh(duh);
        return 1;
    }

    renderer = dumb_it_seek_index_start(index, frame, &preroll);
    if (!renderer) {
        fprintf(stderr, "cannot start renderer at frame %llu\n", frame);
        dumb_it_free_seek_index(index);
        unload_duh(duh);
        return 1;
    }

    fprintf(stderr, "seeking to frame %llu (preroll %u frames, %llu covered)\n",
            frame, preroll, covered);

    if (output) {
        out = fopen(output, "wb");
        if (!out) {
            fprintf(stderr, "cannot open %s\n", output);
            duh_end_sigrenderer(renderer);
            dumb_it_free_seek_index(index);
            unload_duh(duh);
            return 1;
        }
    }

    samples = allocate_sample_buffer(CHANNELS, BLOCK);
    pcm = malloc((size_t)BLOCK * CHANNELS * sizeof(short));
    if (!samples || !pcm) {
        fprintf(stderr, "out of memory\n");
        destroy_sample_buffer(samples);
        free(pcm);
        if (out != stdout)
            fclose(out);
        duh_end_sigrenderer(renderer);
        dumb_it_free_seek_index(index);
        unload_duh(duh);
        return 1;
    }

    while ((unsigned long)done < frames_wanted) {
        long want = BLOCK;
        long got;
        long i;
        long written = 0;

        if ((unsigned long)(frames_wanted - done) < (unsigned long)want)
            want = (long)(frames_wanted - done);

        dumb_silence(samples[0], CHANNELS * want);
        got = duh_sigrenderer_generate_samples(renderer, 1.0f, 2.0f, want,
                                               samples);
        if (got <= 0)
            break; /* end of the song */

        /* The seeded PCM is already the final mix; convert to signed 16-bit. */
        for (i = 0; i < got * CHANNELS; i++) {
            long value = samples[0][i];
            short s;
            if (value > 0x7FFFFF)
                value = 0x7FFFFF;
            else if (value < -0x800000)
                value = -0x800000;
            s = (short)(value >> 8);
            pcm[written++] = (unsigned char)(s & 0xFF);
            pcm[written++] = (unsigned char)((s >> 8) & 0xFF);
        }
        fwrite(pcm, 1, (size_t)written, out);
        fflush(out);
        done += got;
    }

    fprintf(stderr, "wrote %ld frames\n", done);

    free(pcm);
    destroy_sample_buffer(samples);
    if (out != stdout)
        fclose(out);
    duh_end_sigrenderer(renderer);
    dumb_it_free_seek_index(index);
    unload_duh(duh);
    return 0;
}
