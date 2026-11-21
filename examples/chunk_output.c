/**************************************************************************************
 * Example: low-latency chunked (streaming) bitstream output with uavs3e.
 *
 * The encoder invokes cfg.output_chunk on its encoding worker thread as soon
 * as a prefix of the current picture's bitstream has become stable - while
 * later CTU rows are still waiting for mode search. This example simply
 * writes every chunk to the output file in callback order; the result is
 * byte-identical to the enc_stat_t.buf/bytes the encoder returns per frame.
 *
 * Configuration envelope for chunked output (uavs3e_create fails with
 * COM_ERR_UNSUPPORTED otherwise):
 *   8-bit I420, fixed QP (rc_type == RC_TYPE_NULL), max_b_frames == 0,
 *   frm_threads == 1, wpp_threads == 1, adaptive_dqp == 0,
 *   alf_enable == 0, sao_enable == 0, use_pic_sign == 0.
 * Deblocking may stay enabled.
 *
 * Build (from the repository root):
 *   cc -std=c99 -O2 -Iinc -I. examples/chunk_output.c \
 *      .task-build/base-lib/libuavs3e.a -lm -pthread -o chunk_output
 **************************************************************************************/

#include "uavs3e.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *fp;          /* output stream the chunks are written to, in order */
    long  frame_bytes; /* bytes written for the current picture so far */
    long  total_bytes;
} chunk_writer_t;

/* NOTE: data is borrowed - it is only valid during this call, so consume or
 * copy it immediately. The callback runs synchronously on an encoder worker
 * thread and must not call back into the encoder. */
static void on_chunk(void *opaque, long long poc, const unsigned char *data, int bytes, int completed_rows, int final)
{
    chunk_writer_t *w = (chunk_writer_t *)opaque;

    if (bytes > 0) {
        fwrite(data, 1, (size_t)bytes, w->fp); /* chunks join in call order */
    }
    w->frame_bytes += bytes;
    w->total_bytes += bytes;
    fprintf(stderr, "poc=%lld chunk=%5d bytes (rows=%2d%s)\n",
            poc, bytes, completed_rows, final ? ", final" : "");
    (void)final; /* exactly one final chunk per picture; it may carry zero bytes */
}

int main(void)
{
    enc_cfg_t cfg;
    uavs3e_load_default_cfg(&cfg);

    cfg.horizontal_size    = 128;
    cfg.vertical_size      = 256;
    cfg.fps_num            = 25;
    cfg.fps_den            = 1;
    cfg.bit_depth_input    = 8;
    cfg.bit_depth_internal = 8;
    cfg.ctu_size           = 64;
    cfg.max_b_frames       = 0;
    cfg.frm_threads        = 1;
    cfg.wpp_threads        = 1;
    cfg.adaptive_gop       = 0;
    cfg.lookahead          = 0;
    cfg.i_period           = 16;
    cfg.qp                 = 34;
    cfg.rc_type            = RC_TYPE_NULL; /* fixed QP */
    cfg.adaptive_dqp       = 0;
    cfg.alf_enable         = 0;
    cfg.sao_enable         = 0;
    cfg.use_pic_sign       = 0;
    cfg.use_deblock        = 1;            /* deblocking may stay on */

    chunk_writer_t writer;
    writer.fp          = fopen("chunk_output.avs3", "wb");
    writer.frame_bytes = 0;
    writer.total_bytes = 0;
    if (!writer.fp) {
        fprintf(stderr, "cannot open output file\n");
        return 1;
    }

    cfg.output_chunk        = on_chunk;
    cfg.output_chunk_opaque = &writer;

    int err = 0;
    void *h = uavs3e_create(&cfg, &err);
    if (!h) {
        fprintf(stderr, "uavs3e_create failed, err=%d\n", err);
        fclose(writer.fp);
        return 1;
    }

    for (int f = 0; f < 8; f++) {
        com_img_t *img = NULL;
        if (uavs3e_get_img(h, &img) != COM_OK) {
            fprintf(stderr, "uavs3e_get_img failed\n");
            break;
        }
        for (int k = 0; k < 3; k++) {
            for (int y = 0; y < img->height[k]; y++) {
                unsigned char *p = (unsigned char *)img->planes[k] + y * img->stride[k];
                for (int x = 0; x < img->width[k]; x++) {
                    p[x] = (unsigned char)((x * 13 + y * 7 + (x * y) % 37 + f * 11 + k * 61) & 255);
                }
            }
        }
        img->pts = f;

        enc_stat_t stat;
        memset(&stat, 0, sizeof(stat));
        if (uavs3e_enc(h, &stat, img) != COM_OK) {
            break;
        }
        /* the chunks written during this call already equal stat.buf:
         * stat.bytes == writer.frame_bytes at this point */
        if (stat.bytes != writer.frame_bytes) {
            fprintf(stderr, "chunk assembly mismatch!\n");
            break;
        }
        writer.frame_bytes = 0;
    }

    enc_stat_t stat;
    memset(&stat, 0, sizeof(stat));
    while (uavs3e_enc(h, &stat, NULL) == COM_OK) {
        /* flush remaining frames */
    }

    uavs3e_free(h);
    fclose(writer.fp);
    fprintf(stderr, "wrote %ld bytes to chunk_output.avs3\n", writer.total_bytes);
    return 0;
}
