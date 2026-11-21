/*
 * Minimal example: decode raw AAC-LC access units and synthesise a missing one
 * with NeAACDecConceal / NeAACDecConceal2.
 *
 * Build (static library from task_support/build.sh):
 *   cc -std=c99 -Iinclude examples/conceal_example.c \
 *      .task-build/library/libfaad.a -lm -o conceal_example
 *
 * The caller knows which access units it did not receive (e.g. from RTP
 * sequence numbers).  For each gap it calls NeAACDecConceal once; each call
 * produces exactly one 1024-sample-per-channel block.  A complete access unit
 * must have decoded successfully before the first call.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neaacdec.h"

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s input.raw\n", argv0);
}

int main(int argc, char **argv)
{
    NeAACDecHandle h;
    NeAACDecConfigurationPtr config;
    NeAACDecFrameInfo info;
    unsigned char asc[2] = {0x11, 0x90}; /* AAC-LC, 48 kHz, stereo */
    unsigned long samplerate = 0;
    unsigned char channels = 0;
    FILE *in;
    unsigned char frame[8192];
    long size;
    int got_good_au = 0;

    if (argc != 2)
    {
        usage(argv[0]);
        return 1;
    }

    in = fopen(argv[1], "rb");
    if (!in)
    {
        perror("open");
        return 1;
    }
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    rewind(in);
    if (size <= 0 || (size_t)size > sizeof(frame))
    {
        fprintf(stderr, "input must be one raw access unit up to %zu bytes\n",
                sizeof(frame));
        fclose(in);
        return 1;
    }
    if (fread(frame, 1, (size_t)size, in) != (size_t)size)
    {
        perror("read");
        fclose(in);
        return 1;
    }
    fclose(in);

    h = NeAACDecOpen();
    if (!h)
        return 1;

    config = NeAACDecGetCurrentConfiguration(h);
    config->defObjectType = LC;
    config->outputFormat = FAAD_FMT_FLOAT;
    config->dontUpSampleImplicitSBR = 1;
    if (!NeAACDecSetConfiguration(h, config))
    {
        NeAACDecClose(h);
        return 1;
    }

    if (NeAACDecInit2(h, asc, sizeof(asc), &samplerate, &channels) != 0)
    {
        NeAACDecClose(h);
        return 1;
    }

    /* Decode one complete access unit.  The first AU of a stream has its
       samples suppressed by the decoder's priming logic, but it still counts
       as a good AU for concealment purposes. */
    {
        void *pcm = NeAACDecDecode(h, &info, frame, (unsigned long)size);
        if (info.error != 0 || pcm == NULL)
        {
            fprintf(stderr, "decode failed: %s\n",
                    NeAACDecGetErrorMessage(info.error));
            NeAACDecClose(h);
            return 1;
        }
        printf("decoded: %lu samples, %u ch, %lu Hz\n",
               info.samples, info.channels, info.samplerate);
        got_good_au = 1;
    }

    if (!got_good_au)
    {
        NeAACDecClose(h);
        return 1;
    }

    /* A missing access unit follows: synthesise one block. */
    {
        void *pcm = NeAACDecConceal(h, &info);
        if (pcm == NULL || info.error != 0)
        {
            fprintf(stderr, "conceal failed: %s\n",
                    NeAACDecGetErrorMessage(info.error));
            NeAACDecClose(h);
            return 1;
        }
        printf("concealed: %lu samples (%u ch), bytesconsumed=%lu\n",
               info.samples, info.channels, info.bytesconsumed);
    }

    /* The same, with the caller supplying the output buffer.  The capacity is
       in bytes and must hold one whole frame (1024 * channels * sizeof(float)
       for FAAD_FMT_FLOAT). */
    {
        float *out = (float*)malloc(1024 * channels * sizeof(float));
        void *slot = out;
        void *pcm;

        if (!out)
        {
            NeAACDecClose(h);
            return 1;
        }
        pcm = NeAACDecConceal2(h, &info, &slot, 1024 * channels * sizeof(float));
        if (pcm == NULL || info.error != 0)
        {
            fprintf(stderr, "conceal2 failed: %s\n",
                    NeAACDecGetErrorMessage(info.error));
            free(out);
            NeAACDecClose(h);
            return 1;
        }
        printf("concealed2: %lu samples into caller buffer (%p)\n",
               info.samples, pcm);
        free(out);
    }

    NeAACDecClose(h);
    return 0;
}
