minih264
==========

[![Build Status](https://travis-ci.org/lieff/minih264.svg)](https://travis-ci.org/lieff/minih264)

Small, but yet reasonably fast H264/SVC encoder single-header library with SSE/NEON optimizations.
Decoder can be popped up in future.

Disclaimer: code highly experimental.

## Comparison with [x264](https://www.videolan.org/developers/x264.html)

Rough comparison with x264 on an i7-6700K:

`x264 -I 30 --profile baseline --preset veryfast --tune zerolatency -b 0 -r 1 --qp 33 --ipratio 1.0 --qcomp 1.0 -o x264.264 --fps 30 vectors/foreman.cif --input-res 352x288 --slices 1 --threads 1`

vs

`./h264enc_x64 vectors/foreman.cif`

| x264         | minih264 |
| ------------ | -------- |
| source: ~4.6mb | 409kb |
| binary: 1.2mb | 100kb |
| time: 0,282s | 0,503s |
| out size: 320kb | 391kb  |

PSNR:
```
x264:     PSNR y:32.774824 u:38.874450 v:39.926132 average:34.084281 min:31.842667 max:36.630286
minih264: PSNR y:33.321686 u:38.858879 v:39.955914 average:34.574459 min:32.389171 max:37.174073
```

First intra frame screenshot (left-to-right: original 152064, minih264 5067, x264 5297 bytes):

![Intra screenshot](images/intra.png?raw=true)

You can compare results in motion using ffplay/mpv players on vectors/out_ref.264 and vectors/x264.264 .

## Usage

### NAL size control: soft threshold vs hard limit

Minih264 offers two ways to keep NAL units small for fixed-size transports.

`desired_nalu_bytes` is a **soft threshold**. When the current NAL grows past
it, the encoder simply starts the next slice before the following macroblock.
The NAL that crossed the threshold is never split, so a single NAL can end up
noticeably larger than `desired_nalu_bytes` (the baseline run, for example,
asks for 128 bytes and sees 222-byte NALs). The old behaviour is unchanged and
is not treated as an API contract violation.

`max_nalu_bytes` (added to `H264E_run_param_t`) is a **hard limit** on the
final, on-the-wire size of every NAL:

* `0` disables it; `desired_nalu_bytes` keeps its historical soft behaviour.
* `64..65535` enables it. `desired_nalu_bytes` is then ignored, and slices are
  started at macroblock boundaries so that no NAL exceeds the limit.
* Any other value returns `H264E_STATUS_BAD_PARAMETER`.

The limit counts the NAL header byte, the actual emulation-prevention (`0x03`)
bytes and the RBSP trailing bits; Annex-B start codes are not counted. SPS, PPS
and every VCL slice obey the same definition. Slicing is decided by measuring
the exact final NAL size (including the pending `mb_skip_run`, the stop bit and
escape bytes), never by truncating bytes or guessing from a raw bit count. A
new slice is a real independent slice: neighbour availability, motion/intra
prediction, coefficient contexts, QP deltas and the deblocking border are all
re-evaluated for the new slice.

If a single macroblock cannot fit even as its own slice, `H264E_encode` returns
`H264E_STATUS_NAL_TOO_SMALL`. No NAL callback fires, the output pointer/length
are set to `NULL`/`0`, and the input image, reference pictures, frame number
and rate-control history are left exactly as before the call, so the caller can
relax the limit and retry the same frame. Only single-layer AVC with a single
thread is supported; combining a positive limit with SVC layers or multiple
threads returns `H264E_STATUS_BAD_PARAMETER`.

```c
#include "minih264e.h"

static void on_nal(const unsigned char *nal, int n, void *token)
{
    (void)token;
    /* nal[] is one NAL without the Annex-B start code; n <= max_nalu_bytes.
       Copy the bytes here: the encoder may reuse the buffer afterwards. */
    fixed_size_transport_submit(nal, n);
}

void encode_frame(H264E_persist_t *enc, H264E_scratch_t *scratch,
                  H264E_io_yuv_t *frame)
{
    H264E_run_param_t run = {0};
    unsigned char *coded;
    int coded_n, ret;

    run.encode_speed    = 5;
    run.qp_min          = 30;
    run.qp_max          = 30;
    run.max_nalu_bytes  = 256;                 /* hard limit, 64..65535 or 0 */
    run.nalu_callback       = on_nal;
    run.nalu_callback_token = 0;

    ret = H264E_encode(enc, scratch, &run, frame, &coded, &coded_n);
    if (ret == H264E_STATUS_NAL_TOO_SMALL)
    {
        /* Nothing was delivered. Relax the limit and retry the same frame. */
        run.max_nalu_bytes = 512;
        ret = H264E_encode(enc, scratch, &run, frame, &coded, &coded_n);
    }
    /* coded[0..coded_n) is the Annex-B stream; its NAL order matches the
       order in which on_nal() was called. */
}
```

## SVC

Minih264 supports both spatial and temporal layers. Spatial layers are almost same as encode 2 independent AVC streams except for Intra frames prediction.
Following diagram shows minih264 SVC scheme for two spatial layers:

![SVC diargam](images/svc.png?raw=true)

That's because P frames spatial prediction are almost useless in practice. But for Intra frames there is a ~20% benefit in full resolution frame size.
Note that decoder must have both base layer I frame _and_ full resolution SVC I frame to decode whole sequence of next P frames in full resolution.

## Limitations

The following major features are not supported compared to x264 (baseline):

 * Trellis quantization.
 * Select prediction mode using Sum of Absolute Transform Differences (SATD).
 * 4x4 motion compensation.

## Interesting links

 * https://www.videolan.org/developers/x264.html
 * https://www.openh264.org/
 * https://github.com/cisco/openh264
 * http://iphome.hhi.de/suehring/tml/
 * https://github.com/oneam/h264bsd
 * https://github.com/fhunleth/hollowcore-h264
 * https://github.com/digetx/h264_decoder
 * https://github.com/lspbeyond/p264decoder
 * https://github.com/jcasal-homer/HomerHEVC
 * https://github.com/ultravideo/kvazaar
 * https://github.com/neocoretechs/h264j
 * https://github.com/jcodec/jcodec
