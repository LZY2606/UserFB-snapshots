# Chained xfade / acrossfade transition regression

This directory holds a self-contained regression for an audio/video
transition chain.  One edit list drives both picture and sound; an
independent C reference (no FFmpeg linkage) computes the expected result
and locates the first wrong source position, pixel, channel or timestamp.

Everything here is C and shell.  No media samples are downloaded and no
Python, browser or sound card is used.

## What is exercised

Three source shots are joined with two chained transitions:

* video: two chained `xfade=transition=fade` filters,
* audio: two chained two-input `acrossfade` filters (`c1=tri`, `c2=qsin`),
* both chains first merge shots 1 and 2, then merge that result with
  shot 3,
* output is `rawvideo` (gray8) and `pcm_f32le` (48 kHz stereo) muxed in
  NUT, then read back with the real `ffmpeg` and `ffprobe`.

Two edit lists share the same code path:

| case            | frames/shot | video overlap | program frames | audio samples/shot | audio overlap | program samples |
|-----------------|-------------|---------------|----------------|--------------------|---------------|-----------------|
| 25 fps          | 25          | 5             | 65             | 48000              | 9600          | 124800          |
| 30000/1001 fps  | 30          | 10            | 70             | 48048              | 16016         | 112112          |

The second video `xfade` offset is the position in the *finished*
program (40 frames), not an offset re-zeroed from shot two.  For the
fractional-rate case the duration/offset are given in microseconds and
all PTS checks use the parsed stream time base (`1/60000`, one frame =
2002 ticks), never decimal-second strings.

## Files

| file                       | role |
|----------------------------|------|
| `transition_spec.h/.c`     | edit-list tables, source material formulas, timeline/origin mapping. Pure libc + libm. |
| `transition_assets.c`      | standalone generator of the six raw source streams (`v0..2.gray`, `a0..2.f32`). |
| `transition_oracle.c`      | standalone independent reference (`expect`, `check-content`, `check-clock`). Links only libc + libm, never FFmpeg. |
| `transition_cases.c`       | regression cases registered through `check.h` as `task_cases` / `task_case_count`. |
| `transition_example.c`     | small C example printing analytic source positions and values. |
| `driver.c` / `check.h`     | generic case runner and assertion macro (pre-existing). |
| `run.sh` / `build.sh`      | build and entry point. |
| `baseline.sh` / `baseline_check.c` | original two-shot sanity regression (kept). |

## Running

```sh
# Original two-shot sanity transition only:
bash task_support/run.sh

# Full new regression on top of a fresh build:
bash task_support/run.sh target
```

`run.sh target` builds the minimal in-tree ffmpeg/ffprobe, the reference
and generator binaries and the regression, then runs the three groups:

* `rational_timeline` - both edit lists end to end: frame/sample counts,
  PTS and continuous audio sample axes, and every pixel/PCM value against
  the analytic reference;
* `chunk_invariance` - upstream audio reblocked through
  `asetnsamples=127:p=0` and `asetnsamples=4096:p=0`: decoded pixels and
  PCM stay byte-identical and the clock stays continuous even though the
  packet grouping differs;
* `oracle_guards` - three legal-but-wrong artifacts that must each be
  rejected with the first difference located:
  1. one pixel changed inside a video transition,
  2. one left/right stereo sample pair swapped inside an audio crossfade,
  3. a real `ffmpeg -c copy -output_ts_offset` remux that moves the whole
     clock while leaving the decoded content unchanged.

An empty group or any failed `TASK_CHECK` makes the run exit non-zero.

### Pointing at another build

```sh
FFMPEG_BIN=/path/to/ffmpeg FFPROBE_BIN=/path/to/ffprobe \
  bash task_support/run.sh target
```

The same checks run unchanged; `TRANSITION_ORACLE_BIN` and
`TRANSITION_ASSETS_BIN` default to the binaries built under
`.task-build/` and can be overridden the same way.

## Reference conventions

The reference never runs the filters under test.  From the shared
timeline mapping it evaluates:

* video: analytic linear blend of the two source pixels,
* audio: the two channels independently at their own source positions.

Endpoint convention, matching the filters: in an overlap of length `N`,
sample/frame `k` uses

* `tri`: first-input weight `(N-1-k)/N`,
* linear second weight `k/N`,
* `qsin`: quarter sine applied to the linear weight,
  `sin((k/N) * pi/2)`.

Tolerances: 8-bit pixels allow 1 code value; `f32` content uses
`1e-6`; timestamps and sample counts are compared exactly.  Checks cover
the regions before, inside and after both transitions, interior samples
and the final frame - not just file length, a whole-stream CRC or the
first transition.

Mismatch reports name the stream and the first differing location, for
example:

```
ORACLE_MISMATCH kind=pixel video frame=22 origin=seg0:frame22+seg1:frame2 k=2 n=5 x=3 y=4 got=79 expect=56
ORACLE_MISMATCH kind=pcm audio sample=80000 origin=... k=3200 n=9600 channel=left got=... expect=...
ORACLE_MISMATCH kind=clock stream=video packet=0 got_pts=358 expect_pts=0 tb=1/51200
```

## Building the pieces by hand

```sh
cc -std=c17 -O2 -Wall -Wextra -Itask_support \
   task_support/transition_spec.c task_support/transition_oracle.c \
   -o transition_oracle -lm
cc -std=c17 -O2 -Wall -Wextra -Itask_support \
   task_support/transition_spec.c task_support/transition_assets.c \
   -o transition_assets -lm
cc -std=c17 -O2 -Wall -Wextra -Itask_support \
   task_support/transition_spec.c task_support/transition_example.c \
   -o transition_example -lm
```
