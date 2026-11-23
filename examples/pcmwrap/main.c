/*
 * Command line example: wrap a raw little-endian PCM file as audio-only OP1a
 *
 * Usage:
 *   pcmwrap <output.mxf> <pcm-format> <edit-rate> <sequence-offset> \
 *           <max-edit-units-per-body> <input.pcm | ->
 *
 *   pcm-format   : s16le-1ch | s16le-2ch | s24le-1ch | s24le-2ch
 *   edit-rate    : 25 | 29.97
 *   input.pcm    : raw PCM path, or - for standard input
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mxf/mxf.h>

#include "pcm_wrap.h"


#define CHUNK_SIZE      (64 * 1024)


static int parse_format(const char *text, PCMWrapPCMFormat *format)
{
    if (strcmp(text, "s16le-1ch") == 0)
    {
        *format = PCM_WRAP_PCM_S16LE_1CH;
    }
    else if (strcmp(text, "s16le-2ch") == 0)
    {
        *format = PCM_WRAP_PCM_S16LE_2CH;
    }
    else if (strcmp(text, "s24le-1ch") == 0)
    {
        *format = PCM_WRAP_PCM_S24LE_1CH;
    }
    else if (strcmp(text, "s24le-2ch") == 0)
    {
        *format = PCM_WRAP_PCM_S24LE_2CH;
    }
    else
    {
        return 0;
    }
    return 1;
}

static int parse_rate(const char *text, PCMWrapEditRate *rate)
{
    if (strcmp(text, "25") == 0)
    {
        *rate = PCM_WRAP_EDIT_RATE_25_1;
    }
    else if (strcmp(text, "29.97") == 0 || strcmp(text, "30000/1001") == 0)
    {
        *rate = PCM_WRAP_EDIT_RATE_30000_1001;
    }
    else
    {
        return 0;
    }
    return 1;
}

int main(int argc, const char **argv)
{
    MXFFile *mxfFile = NULL;
    PCMWrapWriter *writer = NULL;
    PCMWrapPCMFormat pcmFormat;
    PCMWrapEditRate editRate;
    long sequenceOffset;
    long maxUnitsPerBody;
    FILE *input = NULL;
    uint8_t *buffer = NULL;
    size_t numRead;
    int result = 1;

    if (argc != 7 ||
        !parse_format(argv[2], &pcmFormat) ||
        !parse_rate(argv[3], &editRate))
    {
        fprintf(stderr,
                "Usage: %s <output.mxf> <s16le-1ch|s16le-2ch|s24le-1ch|s24le-2ch>\n"
                "               <25|29.97> <sequence-offset 0-4> <max-units-per-body 1-1024>\n"
                "               <input.pcm | ->\n",
                argv[0]);
        return 2;
    }

    sequenceOffset = strtol(argv[4], NULL, 10);
    maxUnitsPerBody = strtol(argv[5], NULL, 10);
    if (sequenceOffset < 0 || sequenceOffset > 4 ||
        maxUnitsPerBody < 1 || maxUnitsPerBody > 1024)
    {
        fprintf(stderr, "sequence offset must be 0-4 and max units per body 1-1024\n");
        return 2;
    }

    if (strcmp(argv[6], "-") == 0)
    {
        input = stdin;
    }
    else
    {
        input = fopen(argv[6], "rb");
        if (input == NULL)
        {
            fprintf(stderr, "failed to open input '%s'\n", argv[6]);
            return 1;
        }
    }

    if (!mxf_disk_file_open_new(argv[1], &mxfFile))
    {
        fprintf(stderr, "failed to open output '%s'\n", argv[1]);
        goto cleanup;
    }

    writer = pcm_wrap_begin(mxfFile, pcmFormat, editRate,
                            (uint32_t)sequenceOffset, (uint32_t)maxUnitsPerBody);
    if (writer == NULL)
    {
        fprintf(stderr, "pcm_wrap_begin failed\n");
        goto cleanup;
    }

    buffer = malloc(CHUNK_SIZE);
    if (buffer == NULL)
    {
        goto cleanup;
    }

    while ((numRead = fread(buffer, 1, CHUNK_SIZE, input)) > 0)
    {
        if (!pcm_wrap_push(writer, buffer, numRead))
        {
            fprintf(stderr, "pcm_wrap_push failed\n");
            goto cleanup;
        }
    }
    if (ferror(input))
    {
        fprintf(stderr, "failed reading input PCM\n");
        goto cleanup;
    }

    if (!pcm_wrap_finish(writer))
    {
        fprintf(stderr, "pcm_wrap_finish failed\n");
        goto cleanup;
    }

    result = 0;

cleanup:
    free(buffer);
    pcm_wrap_destroy(writer);
    if (mxfFile != NULL)
    {
        mxf_file_close(&mxfFile);
    }
    if (input != NULL && input != stdin)
    {
        fclose(input);
    }
    return result;
}
