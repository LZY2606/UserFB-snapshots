/* Example: split an ADTS file into raw AAC access units and conceal one.
 *
 * Build from the repository root after running task_support/build.sh:
 *   cc -std=c99 -O2 -Iinclude examples/conceal_example.c \
 *      .task-build/library/libfaad.a -lm -o .task-build/conceal_example
 *   .task-build/conceal_example task_support/normal.aac
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neaacdec.h"

int main(int argc, char **argv)
{
    const char *path = (argc > 1) ? argv[1] : "task_support/normal.aac";
    FILE *file = fopen(path, "rb");
    long file_size;
    unsigned char *data;
    size_t offset = 0;
    unsigned frame_index = 0;
    int concealed = 0;
    void *pcm_buffer;

    /* task_support/normal.aac is AAC-LC, 48 kHz, stereo. */
    unsigned char asc[2] = { 0x11, 0x90 };
    unsigned long samplerate = 0;
    unsigned char channels = 0;

    if (!file) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 1;
    }
    data = malloc((size_t)file_size);
    if (!data || fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        fprintf(stderr, "cannot read %s\n", path);
        free(data);
        fclose(file);
        return 1;
    }
    fclose(file);

    NeAACDecHandle decoder = NeAACDecOpen();
    NeAACDecConfigurationPtr config = NeAACDecGetCurrentConfiguration(decoder);
    config->outputFormat = FAAD_FMT_FLOAT;
    config->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration(decoder, config);

    if (NeAACDecInit2(decoder, asc, sizeof(asc), &samplerate, &channels) != 0) {
        fprintf(stderr, "NeAACDecInit2 failed\n");
        NeAACDecClose(decoder);
        free(data);
        return 1;
    }
    printf("configured %lu Hz, %u channels\n", samplerate, channels);

    /* 1024 frames * 2 channels * sizeof(float). */
    pcm_buffer = malloc(1024UL * channels * sizeof(float));
    if (!pcm_buffer) {
        NeAACDecClose(decoder);
        free(data);
        return 1;
    }

    while (offset + 7 <= (size_t)file_size) {
        unsigned char *header = data + offset;
        unsigned frame_length;
        unsigned char *au;
        unsigned long au_size;
        NeAACDecFrameInfo info;
        void *pcm;
        void *buffer_pointer = pcm_buffer;

        if (header[0] != 0xff || (header[1] & 0xf0) != 0xf0) {
            fprintf(stderr, "ADTS sync lost at %zu\n", offset);
            break;
        }

        frame_length = ((unsigned)(header[3] & 0x03) << 11) |
                       ((unsigned)header[4] << 3) |
                       (header[5] >> 5);
        if (frame_length < 7 || offset + frame_length > (size_t)file_size) {
            fprintf(stderr, "bad ADTS frame length at %zu\n", offset);
            break;
        }

        au = header + 7;
        au_size = frame_length - 7;

        /* Pretend the fourth access unit did not arrive.  Three good AUs
           have already established concealment history. */
        if (frame_index == 3 && !concealed) {
            concealed = 1;
            pcm = NeAACDecConceal2(decoder, &info, &buffer_pointer,
                                   1024UL * channels * sizeof(float));
            printf("frame %u: concealed error=%u samples=%lu pcm=%p\n",
                   frame_index, info.error, info.samples, pcm);
        } else {
            pcm = NeAACDecDecode2(decoder, &info, au, au_size,
                                  &buffer_pointer,
                                  1024UL * channels * sizeof(float));
            printf("frame %u: decoded error=%u consumed=%lu samples=%lu "
                   "pcm=%p\n", frame_index, info.error,
                   info.bytesconsumed, info.samples, pcm);
        }

        if (info.error != 0) {
            fprintf(stderr, "decoder error %u: %s\n", info.error,
                    NeAACDecGetErrorMessage(info.error));
        }

        offset += frame_length;
        frame_index++;
    }

    free(pcm_buffer);
    NeAACDecClose(decoder);
    free(data);
    return 0;
}
