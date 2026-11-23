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

## Two-pass bitrate targeting

`minih264e_2pass.c` / `minih264e_2pass.h` add an optional two-pass mode: a
first pass records per-macroblock statistics while really encoding, and a
second, separate process plans one QP per frame against a byte target for the
whole clip.  The classic library calls and the classic command line are
unchanged and byte-compatible; installing the measurement hook does not alter
the bitstream.

The two-pass mode is restricted to a single spatial layer, a single thread,
no long-term references, no denoise, and no VBV stuffing or frame dropping.
Frame size, frame rate and GOP are fixed and input frames are not reordered.

### Command line

```sh
# pass 1: real encode, per-frame records written to stats.txt
./h264e_test -gop20 -qp28 -pass1stats.txt in_352x288.yuv pass1.264

# pass 2: separate process, whole-clip target of 500000 bytes
./h264e_test -gop20 -pass2stats.txt -targetbytes500000 in_352x288.yuv out.264
```

`-targetbytes<N>` is only used by `-pass2` (with `-pass1` it is ignored).
`-qpmin<N>` / `-qpmax<N>` narrow the legal planning window (default 10..51);
a plain `-qp<N>` fixes both bounds, which makes pass 2 use that single QP.
Add `-stats` to either pass to print per-frame diagnostics.  Pass 2 verifies
each frame's size, identity fingerprint and I/P class against the record
before encoding it, and fails rather than continuing with a mismatched record.

### Statistics file format

Plain text, versioned, one ASCII token per field.  `double` values are written
with `%.17g`, so they round-trip exactly.  The fingerprint is lowercase hex.

```
minih264e_2pass 1
width 352
height 288
gop 20
frames 3
frame 0 idr 1 width 352 height 288 fnv 99b88352e5bbc1e5 c 50679.161481168194 h 225
frame 1 idr 0 width 352 height 288 fnv 7b6a1901b422c365 c 40792.515759659233 h 61
frame 2 idr 0 width 352 height 288 fnv 2b60b120563c9565 c 39890.056272818503 h 63
end
```

Per record:

* `index` is the consecutive input frame number; records are contiguous.
* `idr` is the final class observed in the encoded access unit (1 = IDR).
* `fnv` is FNV-1a-64 (offset 14695981039346656037, prime 1099511628211) over
  the valid I420 pixels in Y/U/V row order, without stride padding.
* `c` and `h` are measured in the production bit-writing path:
  `c = sum(bits_mb * 2^((qp_mb-26)/6))` and
  `h = au_bytes*8 - sum(bits_mb)`, where `bits_mb` are the VCL syntax bits
  submitted by each macroblock (no NAL/slice headers, no stop bit, no
  emulation-prevention bytes).  A deferred `mb_skip_run` is charged to the last
  skipped macroblock of the run, and a run open at the end of a slice is
  settled on the last macroblock of that slice.  `h` is never negative.

### Planning model

For every frame class (I and P) a feedback factor `a` starts at 1.  While
frames remain, the smallest integer `q` in the legal window is chosen so that
`sum(h) + (1/s(q)) * sum(a_class * c)` over all unprocessed records does not
exceed the remaining byte target times 8.  If even the maximum QP cannot fit,
the maximum QP is used and the plan is reported as saturated; no claim is made
that the target was met.  After a successful access unit, the target shrinks
by its actual byte count (never by the prediction), and only that frame's
class factor moves:

```
a_new = clamp((a_old + max(actual_bits - h, 0) * s(q) / c) / 2, 0.25, 4)
```

with `c == 0` leaving the factor unchanged.  A failed encode does not advance
the record cursor or spend budget.  The model does not promise an exact file
size for arbitrary material.

### Library API

```c
#include "minih264e_2pass.h"

H264E_2pass_writer_t *w;
H264E_mb_stats_hook_t hook;
H264E_2pass_frame_stats_t stats;   /* borrowed; valid until the next call */

H264E_2pass_writer_open(&w, "stats.txt", width, height, gop);
H264E_2pass_writer_hook(w, &hook);
H264E_set_mb_stats_hook(enc, &hook);        /* from minih264e.h */

H264E_2pass_writer_begin(w, frame_index, &yuv);
H264E_encode(enc, scratch, &run, &yuv, &coded, &size);
H264E_2pass_writer_end(w, size, is_idr, &stats);   /* stats.mb[], stats.c/h */
H264E_2pass_writer_close(w, &frames);
H264E_2pass_writer_destroy(w);
```

Pass 2 uses `H264E_2pass_file_open()`, `H264E_2pass_planner_open()`, then per
frame `H264E_2pass_planner_next()` / `H264E_2pass_planner_commit()` (or
`H264E_2pass_planner_abort()` on failure) and
`H264E_2pass_planner_diag()` for diagnostics.


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
