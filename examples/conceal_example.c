/*
 * Example: split an ADTS AAC file into raw access units, skip one AU as if
 * it had not arrived, and insert one concealment frame.
 *
 * Build from the repository root after task_support/build.sh:
 *   cc -std=c99 -O2 -Iinclude examples/conceal_example.c \
 *      .task-build/library/libfaad.a -lm -o .task-build/conceal_example
 *
 * Run:
 *   .task-build/conceal_example task_support/normal.aac out.pcm 3
 *
 * The output is interleaved 32-bit float PCM at the decoder sample rate.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neaacdec.h"

static unsigned adts_length(const unsigned char *p)
{
    return ((unsigned)(p[3] & 0x03) << 11) |
           ((unsigned)p[4] << 3) |
           ((unsigned)(p[5] >> 5));
}

int main(int argc, char **argv)
{
    const char *input_path = argc > 1 ? argv[1] : "task_support/normal.aac";
    const char *output_path = argc > 2 ? argv[2] : "concealed.pcm";
    unsigned missing_au = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 10) : 3;
    FILE *in = NULL, *out = NULL;
    long file_size;
    unsigned char *file_data = NULL;
    NeAACDecHandle decoder = NULL;
    NeAACDecConfigurationPtr config;
    unsigned char asc[2];
    unsigned long sample_rate = 0;
    unsigned char channels = 0;
    unsigned char sf_index, channel_config;
    size_t offset = 0;
    unsigned au_index = 0;
    /* Supported scope is mono/stereo 1024-sample AAC-LC. */
    unsigned char caller_storage[2 * 1024 * sizeof(float)];
    void *caller_buffer = caller_storage;
    int result = 1;

    in = fopen(input_path, "rb");
    if (!in)
    {
        fprintf(stderr, "cannot open %s\n", input_path);
        goto done;
    }
    if (fseek(in, 0, SEEK_END) != 0 || (file_size = ftell(in)) <= 7)
    {
        fprintf(stderr, "bad input file\n");
        goto done;
    }
    rewind(in);
    file_data = (unsigned char*)malloc((size_t)file_size);
    if (!file_data || fread(file_data, 1, (size_t)file_size, in) != (size_t)file_size)
    {
        fprintf(stderr, "cannot read input\n");
        goto done;
    }

    if (file_data[0] != 0xff || (file_data[1] & 0xf6) != 0xf0)
    {
        fprintf(stderr, "this simple example expects ADTS AAC\n");
        goto done;
    }

    sf_index = (unsigned char)((file_data[2] >> 2) & 0x0f);
    channel_config = (unsigned char)(((file_data[2] & 0x01) << 2) |
                                     ((file_data[3] >> 6) & 0x03));
    if ((sf_index != 3 && sf_index != 4) ||
        (channel_config != 1 && channel_config != 2))
    {
        fprintf(stderr, "example supports 44.1/48 kHz mono/stereo only\n");
        goto done;
    }

    /* AudioSpecificConfig: AAC-LC (object type 2), 4-bit sr_index,
       4-bit channelConfiguration. */
    asc[0] = (unsigned char)((2u << 3) | (sf_index >> 1));
    asc[1] = (unsigned char)(((sf_index & 1u) << 7) |
                             (channel_config << 3));

    decoder = NeAACDecOpen();
    if (!decoder)
        goto done;
    config = NeAACDecGetCurrentConfiguration(decoder);
    config->outputFormat = FAAD_FMT_FLOAT;
    config->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration(decoder, config);
    if (NeAACDecInit2(decoder, asc, sizeof(asc), &sample_rate, &channels) != 0)
    {
        fprintf(stderr, "NeAACDecInit2 failed\n");
        goto done;
    }

    out = fopen(output_path, "wb");
    if (!out)
    {
        fprintf(stderr, "cannot open %s\n", output_path);
        goto done;
    }

    while (offset + 7 <= (size_t)file_size)
    {
        unsigned char *header = file_data + offset;
        unsigned frame_length = adts_length(header);
        unsigned char *au;
        unsigned long au_size;
        NeAACDecFrameInfo info;
        void *pcm;

        if (header[0] != 0xff || (header[1] & 0xf6) != 0xf0 ||
            frame_length < 8 || (size_t)frame_length > (size_t)file_size - offset)
        {
            fprintf(stderr, "invalid ADTS frame at AU %u\n", au_index);
            goto done;
        }

        /* Most ADTS streams have a 7-byte header (no CRC). */
        au = header + 7;
        au_size = frame_length - 7;

        if (au_index == missing_au)
        {
            unsigned long buffer_bytes = 1024UL * channels * (unsigned long)sizeof(float);
            fprintf(stderr, "concealing missing AU %u\n", au_index);
            pcm = NeAACDecConceal2(decoder, &info, &caller_buffer, buffer_bytes);
        }
        else
        {
            pcm = NeAACDecDecode(decoder, &info, au, au_size);
        }

        if (info.error)
        {
            fprintf(stderr, "AU %u: decoder error %u: %s\n", au_index,
                    info.error, NeAACDecGetErrorMessage(info.error));
            goto done;
        }
        if (pcm && info.samples)
        {
            if (fwrite(pcm, sizeof(float), info.samples, out) != info.samples)
            {
                fprintf(stderr, "write failed\n");
                goto done;
            }
        }

        offset += frame_length;
        au_index++;
    }

    fprintf(stderr, "wrote %u AUs to %s at %lu Hz, %u channels\n",
            au_index, output_path, sample_rate, channels);
    result = 0;

done:
    if (out)
        fclose(out);
    if (in)
        fclose(in);
    if (decoder)
        NeAACDecClose(decoder);
    free(file_data);
    return result;
}
