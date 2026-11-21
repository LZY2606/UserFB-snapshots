/*
 * crc_demo.c - optional Layer III CRC error protection example.
 *
 * Build (production decoder with the feature enabled):
 *   cc -std=c99 -O2 -DMINIMP3_CHECK_CRC -DMINIMP3_IMPLEMENTATION \
 *      examples/crc_demo.c -lm -o crc_demo
 *
 * Usage: ./crc_demo file.mp3
 *
 * Each decoded frame prints its crc_status:
 *   0  - unchecked (feature off build, no protection bit, or not Layer III)
 *   1  - protected frame, CRC matched
 *  -1  - protected frame, CRC mismatch: the frame is consumed at its exact
 *        physical length, replaced by the normal amount of zero PCM, while the
 *        bit reservoir keeps the frame's main data for later correct frames.
 */
#include "minimp3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s file.mp3\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
    {
        perror(argv[1]);
        return 1;
    }
    if (fseek(f, 0, SEEK_END))
    {
        fclose(f);
        return 1;
    }
    long size = ftell(f);
    rewind(f);
    if (size <= 0)
    {
        fclose(f);
        return 1;
    }
    uint8_t *buf = (uint8_t*)malloc((size_t)size);
    if (!buf)
    {
        fclose(f);
        return 1;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size)
    {
        free(buf);
        fclose(f);
        return 1;
    }
    fclose(f);

    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    size_t off = 0;
    int frame = 0, muted = 0;
    while (off < (size_t)size)
    {
        mp3dec_frame_info_t info;
        memset(&info, 0, sizeof(info));
        int samples = mp3dec_decode_frame(&dec, buf + off, (int)(size - off), pcm, &info);
        if (!info.frame_bytes)
            break; /* incomplete frame at end of stream */
        if (info.crc_status < 0)
            muted++;
        printf("frame=%d offset=%zu bytes=%d samples=%d ch=%d crc_status=%d\n",
               frame, off, info.frame_bytes, samples, info.channels, info.crc_status);
        off += (size_t)info.frame_bytes;
        frame++;
    }

    printf("frames=%d muted=%d total_scalar_position=%zu\n",
           frame, muted, off);
    free(buf);
    return 0;
}
