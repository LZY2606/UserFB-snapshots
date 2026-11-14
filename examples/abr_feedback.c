/*
 *      Example: closed-loop ABR feedback
 *
 *      Encodes a short stereo signal whose loudness changes over time,
 *      retargets the ABR feedback loop twice during the run and writes a
 *      playable MP3 file.  The produced PCM is non-silent, and the ledger
 *      of the feedback loop is printed at the end.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "lame.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLERATE  48000
#define CHANNELS    2
#define FRAME_SIZE  1152
#define BLOCK_FRAMES 128

static unsigned int lcg_state = 0x12345678u;

static double
noise(void)
{
    lcg_state = 1664525u * lcg_state + 1013904223u;
    return ((int) (lcg_state >> 8)) / 2147483648.0;
}

/*
 * Fill one frame with a mixture which is neither silent nor trivially
 * compressible: two tones plus broadband noise, a different tone pair for
 * the right channel.
 */
static void
make_frame(short *pcm, int frame_index)
{
    int     i;
    int     position = frame_index * FRAME_SIZE;
    for (i = 0; i < FRAME_SIZE; ++i) {
        int const t = position + i;
        double  l = 14000.0 * sin(2.0 * 3.14159265358979323846 * 997.0 * t / SAMPLERATE);
        double  r = 14000.0 * sin(2.0 * 3.14159265358979323846 * 1500.0 * t / SAMPLERATE);
        l += 5000.0 * sin(2.0 * 3.14159265358979323846 * 3000.0 * t / SAMPLERATE);
        r += 5000.0 * sin(2.0 * 3.14159265358979323846 * 4000.0 * t / SAMPLERATE);
        l += 4000.0 * noise();
        r += 4000.0 * noise();
        if (l > 32767.0)
            l = 32767.0;
        if (l < -32768.0)
            l = -32768.0;
        if (r > 32767.0)
            r = 32767.0;
        if (r < -32768.0)
            r = -32768.0;
        pcm[CHANNELS * i + 0] = (short) l;
        pcm[CHANNELS * i + 1] = (short) r;
    }
}

int
main(int argc, char **argv)
{
    static const int targets[3] = { 96, 160, 80 };
    char const *path = argc > 1 ? argv[1] : "abr_feedback.mp3";
    lame_t  gfp;
    lame_abr_feedback_stats stats;
    short   pcm[CHANNELS * FRAME_SIZE];
    unsigned char *mp3;
    FILE   *out;
    int     mp3_capacity;
    int     segments, total = 0;

    gfp = lame_init();
    if (gfp == NULL) {
        fprintf(stderr, "lame_init failed\n");
        return 1;
    }

    lame_set_in_samplerate(gfp, SAMPLERATE);
    lame_set_out_samplerate(gfp, SAMPLERATE);
    lame_set_num_channels(gfp, CHANNELS);
    lame_set_mode(gfp, JOINT_STEREO);
    lame_set_VBR(gfp, vbr_abr);
    lame_set_VBR_mean_bitrate_kbps(gfp, targets[0]);
    lame_set_bWriteVbrTag(gfp, 1);
    lame_set_quality(gfp, 2);

    /* enable the option before the encoder is initialised */
    if (lame_set_abr_feedback(gfp, 1) != 0) {
        fprintf(stderr, "lame_set_abr_feedback failed\n");
        lame_close(gfp);
        return 1;
    }
    if (lame_init_params(gfp) < 0) {
        fprintf(stderr, "lame_init_params failed\n");
        lame_close(gfp);
        return 1;
    }

    mp3_capacity = 3 * BLOCK_FRAMES * 2000 + 65536;
    mp3 = (unsigned char *) malloc((size_t) mp3_capacity);
    if (mp3 == NULL) {
        lame_close(gfp);
        return 1;
    }

    for (segments = 0; segments < 3; ++segments) {
        int     frame;
        if (segments > 0) {
            /* a later target applies from the next encoded audio frame */
            if (lame_set_abr_target(gfp, targets[segments]) != 0) {
                fprintf(stderr, "lame_set_abr_target(%d) failed\n", targets[segments]);
                free(mp3);
                lame_close(gfp);
                return 1;
            }
        }
        for (frame = 0; frame < BLOCK_FRAMES; ++frame) {
            int     left = mp3_capacity - total;
            int     got;
            make_frame(pcm, segments * BLOCK_FRAMES + frame);
            got = lame_encode_buffer_interleaved(gfp, pcm, FRAME_SIZE, mp3 + total, left);
            if (got < 0) {
                fprintf(stderr, "encoding failed: %d\n", got);
                free(mp3);
                lame_close(gfp);
                return 1;
            }
            total += got;
        }
        if (lame_get_abr_feedback_stats(gfp, &stats) != 0) {
            fprintf(stderr, "lame_get_abr_feedback_stats failed\n");
            free(mp3);
            lame_close(gfp);
            return 1;
        }
        printf("segment %d, target %d kbps: frames=%llu avg=%d kbps\n",
               segments, targets[segments],
               (unsigned long long) stats.frames, stats.target_kbps);
    }

    {
        int     got = lame_encode_flush(gfp, mp3 + total, mp3_capacity - total);
        if (got < 0) {
            fprintf(stderr, "lame_encode_flush failed: %d\n", got);
            free(mp3);
            lame_close(gfp);
            return 1;
        }
        total += got;
    }

    /* replace the Xing placeholder with the final tag, as documented */
    {
        size_t  tag_size = lame_get_lametag_frame(gfp, mp3, (size_t) total);
        if (tag_size == 0 || (int) tag_size > total) {
            fprintf(stderr, "could not build the Xing tag\n");
        }
    }

    if (lame_get_abr_feedback_stats(gfp, &stats) != 0) {
        fprintf(stderr, "lame_get_abr_feedback_stats failed\n");
        free(mp3);
        lame_close(gfp);
        return 1;
    }

    out = fopen(path, "wb");
    if (out == NULL) {
        fprintf(stderr, "cannot create %s\n", path);
        free(mp3);
        lame_close(gfp);
        return 1;
    }
    if (fwrite(mp3, 1, (size_t) total, out) != (size_t) total) {
        fprintf(stderr, "cannot write %s\n", path);
        fclose(out);
        free(mp3);
        lame_close(gfp);
        return 1;
    }
    fclose(out);

    printf("wrote %s, %d bytes\n", path, total);
    printf("frames=%llu encoded_bits=%llu budget_bit_samples=%llu target=%d\n",
           (unsigned long long) stats.frames,
           (unsigned long long) stats.encoded_bits,
           (unsigned long long) stats.budget_bit_samples, stats.target_kbps);

    lame_close(gfp);
    free(mp3);
    return 0;
}
