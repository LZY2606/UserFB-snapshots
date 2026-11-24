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

TBD

## Three temporal layers

Set `temporal_layers = 3` to enable the optional default-off temporal
hierarchy. The mode requires one AVC spatial layer, at most one encoder
thread, `gop = 0`, `temporal_denoise_flag = 0`, `vbv_size_bytes = 0`, and
two long-term reference slots. Leave `temporal_layers = 0` for the legacy
behavior; all other new fields are then ignored.

The first frame and every explicit `H264E_FRAME_TYPE_KEY` are base-layer
IDR pictures and restart the temporal cycle. Later inputs get layer IDs
`2, 1, 2, 0, ...` in display order. Layer 0 references only the previous
base picture, layer 1 references the newest base picture, and layer 2 is a
non-reference picture which follows the newest base or middle picture.

```c
static void on_temporal_stats(const H264E_temporal_stats_t *stats, void *opaque)
{
    /* The stats object is borrowed only for this callback. */
    *(int *)opaque = stats->temporal_id;
}

H264E_create_param_t p;
memset(&p, 0, sizeof(p));
p.width = 1280;
p.height = 720;
p.num_layers = 1;
p.gop = 0;
p.max_long_term_reference_frames = 2;

p.temporal_layers = 3;
p.temporal_bitrate_bps[0] =  500000;   /* base set {0} */
p.temporal_bitrate_bps[1] = 1500000;   /* layers {0,1} */
p.temporal_bitrate_bps[2] = 3000000;   /* all layers */
p.temporal_buffer_bytes[0] =  200000;
p.temporal_buffer_bytes[1] =  600000;
p.temporal_buffer_bytes[2] = 1200000;
p.temporal_fps_num = 30;
p.temporal_fps_den = 1;

int persist_size, scratch_size;
if (H264E_sizeof(&p, &persist_size, &scratch_size))
    return;
H264E_persist_t *enc = malloc(persist_size);
H264E_scratch_t *scratch = malloc(scratch_size);
if (H264E_init(enc, &p))
    return;

/* Inside the frame loop: */
H264E_run_param_t r;
memset(&r, 0, sizeof(r));
r.frame_type = H264E_FRAME_TYPE_DEFAULT; /* DEFAULT, P, or KEY */
r.qp_min = 20;
r.qp_max = 42;

int temporal_id = 0;
r.temporal_observe = on_temporal_stats;
r.temporal_opaque = &temporal_id;

H264E_io_yuv_t yuv;
/* Fill yuv.yuv[]/stride[] with the I420 input planes. */

unsigned char *out;
int out_size;
if (H264E_encode(enc, scratch, &r, &yuv, &out, &out_size) == 0) {
    /* The returned access unit is already Annex-B and contains SPS/PPS
       with IDR output. Copy/write its original NAL bytes unchanged. */
    fwrite(out, out_size, 1, all_layers_file);
    if (temporal_id <= 1)
        fwrite(out, out_size, 1, base_middle_file);
    if (temporal_id == 0)
        fwrite(out, out_size, 1, base_only_file);
}
```

The three resulting files contain the base layer, the first two layers,
and all pictures respectively. Extraction only selects or copies complete
returned access units; it does not transcode or rewrite NAL headers.
Explicit KEY access units include fresh parameter sets and can also start
a retained subset.

For successful output number `n` (starting at one), cumulative set `k`
receives

```text
floor(n * temporal_bitrate_bps[k] * temporal_fps_den /
      (8 * temporal_fps_num))
```

bytes. The encoder uses the difference from the previous cumulative value,
maintains signed per-set debt, and reports the quota, debt, actual size,
and controller budget in `H264E_temporal_stats_t`. The caller's
`desired_frame_bytes` is ignored in this mode; `qp_min` and `qp_max` still
apply.

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
