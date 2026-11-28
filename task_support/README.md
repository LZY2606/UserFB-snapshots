# A/V transition regression

Independent regression for the chained `xfade` (video) and `acrossfade`
(audio) transition pipeline.  Everything is C and shell: no media samples
are downloaded, and no Python, browser or sound card is used.

## What it exercises

One three-segment edit list drives both picture and sound.  The picture goes
through two chained `xfade=fade` filters and the sound through two chained
dual-input `acrossfade` filters, each time merging segments 1+2 first and
then merging that result with segment 3.  Outputs are `rawvideo` (gray8) and
packed `pcm_f32le` (48 kHz stereo) muxed into NUT, then read back with the
real `ffmpeg` / `ffprobe` binaries.

Two edit lists live in `transition_spec.h`:

| id    | frame rate    | frames/clip | video overlap | samples/clip | audio overlap | result frames | result samples |
|-------|---------------|-------------|---------------|--------------|---------------|---------------|----------------|
| `r25` | 25/1          | 25          | 5             | 48000        | 9600          | 65            | 124800         |
| `ntsc`| 30000/1001    | 30          | 10            | 48048        | 16016         | 70            | 112112         |

The second transition's `offset` is a position in the already merged
timeline (`2*(F-V)` frames), not a re-zeroed offset from segment two.

Source material is synthesized by `transition_gen.c` from deterministic
functions of the segment, source frame, pixel position and (for audio)
source sample and channel.  Neither stream is constant or silent, so a
mis-attributed source cannot hide.

## Files

- `transition_spec.h`     - shared edit lists and source generators (header)
- `transition_gen.c`      - writes the three gray8 / pcm_f32le source clips
- `transition_oracle.c`   - independent reference (C stdlib + libm only;
                            never links FFmpeg, never calls the filters)
- `transition_cases.c`    - registered cases; drives real ffmpeg/ffprobe
- `transition_example.c`  - small standalone illustration of the math
- `driver.c` / `check.h`  - test harness and `TASK_CHECK` assertions
- `build.sh` / `run.sh`   - minimal ffmpeg build and entry points
- `baseline.sh`           - pre-existing two-segment smoke test (retained)

## Running

```sh
# smoke test only: the bundled two-segment normal transition
bash task_support/run.sh

# full build + new reference program + new regression, then the smoke test
bash task_support/run.sh target
```

`run.sh target` builds the current source tree, the oracle, the generator,
the example and the regression driver, then runs three registered groups:

- `rational_timeline` - both edit lists end to end through NUT
- `chunk_invariance`  - upstream `asetnsamples=n=127:p=0` and `n=4096:p=0`
- `oracle_guards`     - pixel / channel-swap / clock-shift negatives

An empty group or a failed `TASK_CHECK` makes the driver (and the script)
exit non-zero.  The script also greps the group/total summary lines.

To point the suite at another build:

```sh
FFMPEG_BIN=/path/to/ffmpeg FFPROBE_BIN=/path/to/ffprobe \
  bash task_support/run.sh target
```

`TRANSITION_GEN_BIN`, `TRANSITION_ORACLE_BIN` and `TASK_WORKDIR` can be
overridden the same way.

The standalone example needs only libm:

```sh
cc -std=c17 -O2 -Wall -Wextra -I. task_support/transition_example.c -o ex -lm
./ex
```

## Reference conventions

- Video is the parsed linear mix `a*w0 + b*w1`, matching `xfade`'s
  `mix(a,b,progress)`; gray8 tolerance is one code value.
- Audio weights follow the existing endpoint convention: in an overlap of
  length `N`, sample `k` uses `(N-1-k)/N` for the first input and `k/N` for
  the second.  The first input curve is `tri`, the second is `qsin` (a
  quarter sine applied to the corresponding weight).  f32 tolerance is
  `1e-6`; frame counts and the sample total are compared exactly.
- Every frame PTS and the continuous audio sample axis are recomputed from
  the integer time bases `ffprobe` reports; decimal `pts_time` strings are
  never parsed and audio packet grouping is not assumed.
- Checks cover the regions before and inside both transitions and the final
  tail frame/sample, not just file length or a whole-stream CRC.

## Why the audio stage uses one demuxer

With this scheduler each input demuxer runs in its own thread.  Feeding
chained `acrossfade` from several demuxers lets an EOF status reach the
filter while its FIFO still holds samples, which non-deterministically
truncates the tail (the observed output length varies between runs).  The
audio stage therefore concatenates the three clips into one file, uses a
single demuxer, and recreates the three inputs inside the graph with
`asplit` + `atrim` (plus the optional `asetnsamples`).  The video stage keeps
three demuxers for the two chained `xfade`s, which is deterministic; the two
stages run in separate ffmpeg invocations and are then muxed into NUT.

## Negative artifacts

`transition_oracle` also produces legal-format, legal-length artifacts that
must each fail validation and report the first difference:

- `mutate-pixel` - changes one pixel inside the second xfade
- `mutate-audio` - swaps one left/right stereo pair inside the second
  crossfade
- a real `ffmpeg -c copy -output_ts_offset` remux that shifts the whole
  stream clock while leaving the decoded samples intact

The failures are content/clock differences surfaced as
`ORACLE_MISMATCH ...` lines naming the frame/sample, pixel, channel or
timestamp; files are never deleted and logs are not substituted for checks.
