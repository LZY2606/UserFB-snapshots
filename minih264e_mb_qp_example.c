/*
    minih264e per-macroblock QP map example (screen sharing use-case).

    Idea: macroblocks that contain rendered text get a lower (better) luma QP
    via H264E_run_param_t::mb_qp, while the rest of the frame is left to the
    built-in rate control, exactly as before.

    The map interacts with the whole-frame quality limits as follows:
      - qp_min/qp_max still bound EVERY macroblock of the frame, both the
        values chosen by the rate control and the values given in the map;
      - a non-zero map entry outside [qp_min; qp_max] is rejected with
        H264E_STATUS_BAD_PARAMETER (it is never silently clipped);
      - a zero map entry means "no opinion": the rate control picks the QP
        for that macroblock, bounded by qp_min/qp_max as usual.

    No external images are used: a synthetic "desktop" frame with text-like
    blocks is generated below. The map is borrowed by a single synchronous
    H264E_encode() call only, so it is re-supplied (or dropped) per frame.

    Build (next to minih264e_test.c):
        cc -std=c11 -O2 minih264e_mb_qp_example.c -o mb_qp_example
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#if defined(__APPLE__) && defined(__aarch64__)
/* Old Apple intrinsic spelling used by this upstream snapshot. */
#define vtbl2q_u8 vqtbl2_u8
#endif
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"

#define W   160                         // frame width  (multiple of 16)
#define H   96                          // frame height (multiple of 16)
#define NMBX (W/16)                     // macroblock columns
#define NMBY (H/16)                     // macroblock rows

#define QP_BACKGROUND 34                // whole-frame quality limits...
#define QP_TEXT      22                 // ...must cover the map values too

/* Fake "text lines": mark horizontal bands as text macroblocks */
static int is_text_mb(int mbx, int mby)
{
    return (mby % 3) == 1 && mbx > 0 && mbx < NMBX - 1;
}

int main(void)
{
    H264E_create_param_t create_param;
    H264E_run_param_t run_param;
    H264E_io_yuv_t yuv;
    H264E_persist_t *enc;
    H264E_scratch_t *scratch;
    unsigned char *coded_data, *img;
    unsigned char mb_qp[NMBY][NMBX];    // whole-frame map, raster order
    int sizeof_persist, sizeof_scratch, sizeof_coded_data;
    int error, frame, x, y, mbx, mby;

    memset(&create_param, 0, sizeof(create_param));
    create_param.width  = W;
    create_param.height = H;
    create_param.gop    = 30;
    create_param.enableNEON = 1;
    create_param.num_layers = 1;        // QP map is AVC single-layer only

    error = H264E_sizeof(&create_param, &sizeof_persist, &sizeof_scratch);
    assert(!error);
    enc     = (H264E_persist_t*)malloc(sizeof_persist);
    scratch = (H264E_scratch_t*)malloc(sizeof_scratch);
    img     = (unsigned char*)malloc(W*H*3/2);
    assert(enc && scratch && img);

    error = H264E_init(enc, &create_param);
    assert(!error);

    yuv.yuv[0] = img;             yuv.stride[0] = W;
    yuv.yuv[1] = img + W*H;       yuv.stride[1] = W/2;
    yuv.yuv[2] = img + W*H*5/4;   yuv.stride[2] = W/2;

    memset(&run_param, 0, sizeof(run_param));
    run_param.encode_speed = 5;
    run_param.desired_frame_bytes = 4000;
    // Whole-frame limits stay in charge: the rate control keeps the
    // background inside [qp_min; qp_max], and the map below may only
    // specify values from the same interval, so qp_min must be lowered
    // to QP_TEXT to make the better text QP a legal map entry.
    run_param.qp_min = QP_TEXT;
    run_param.qp_max = QP_BACKGROUND;

    // Text macroblocks -> better QP, everything else -> 0 = "rate control decides"
    for (mby = 0; mby < NMBY; mby++)
        for (mbx = 0; mbx < NMBX; mbx++)
            mb_qp[mby][mbx] = is_text_mb(mbx, mby) ? QP_TEXT : 0;

    run_param.mb_qp        = &mb_qp[0][0];
    run_param.mb_qp_stride = NMBX;                  // bytes per map row
    run_param.mb_qp_bytes  = sizeof(mb_qp);         // covers last valid cell

    for (frame = 0; frame < 3; frame++)
    {
        // Synthetic "desktop": flat background + sharp text-like stripes
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
            {
                int text = is_text_mb(x/16, y/16);
                img[y*W + x] = (unsigned char)(text
                    ? (((x/4 + y/4) & 1) ? 20 : 235)          // high-contrast glyphs
                    : 96 + ((x + y + frame) & 15));           // smooth background
            }
        memset(img + W*H, 128, W*H/2);

        error = H264E_encode(enc, scratch, &run_param, &yuv, &coded_data, &sizeof_coded_data);
        assert(!error);
        printf("frame %d: %d bytes (map entries: text QP=%d, background=rate control)\n",
            frame, sizeof_coded_data, QP_TEXT);
        // NOTE: run_param.mb_qp is borrowed for this call only; the encoder
        // does not inherit it, so leaving it set for the next frame is a
        // deliberate per-frame choice, not a requirement.
    }

    free(img);
    free(scratch);
    free(enc);
    return 0;
}
