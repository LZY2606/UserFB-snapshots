# Offline volume envelope example

`envelope_offline.c` renders a short excerpt through a device-less `ma_engine`, applies a volume
envelope with `ma_sound_set_volume_envelope()`, and writes the engine's real output to a WAV file.
No audio device, resource manager or decoder is required.

## Build

From the repository root:

```sh
cc -std=c99 -O2 -DMA_NO_DEVICE_IO -DMA_NO_RESOURCE_MANAGER -DMA_NO_DECODING -DMA_NO_ENCODING -I. \
   examples/envelope_offline.c -lm -pthread -o envelope_offline
```

## Run

```sh
./envelope_offline                 # writes envelope_example.wav
./envelope_offline path/out.wav    # writes somewhere else
```

## What it demonstrates

- The curve's `frameIndex` values are **source frames relative to the range start**, not output
  frames and not the engine's global clock. The example plays a range of `[0, 480)` and a loop of
  `[160, 400)` at pitch `1.6`; the curve stays glued to the material as the pitch stretches it.
- A natural loop re-enters the curve at the loop's own source position (`frameIndex` 160) instead
  of restarting the curve at 0.
- `ma_sound_set_volume_envelope()` copies the points. The example overwrites its stack array right
  after the call to show the sound does not reference it.
- Rendering happens in irregular chunks (`RENDER_CHUNK = 97`), mimicking a preview loop whose
  buffer sizes do not line up with anything.
- Existing `ma_sound_set_volume()` and fade processing still apply on top of the envelope, at their
  normal place after resampling and channel conversion.

## API summary

```c
typedef struct {
    ma_uint64 frameIndex;   /* Relative to the data source's range start. */
    float     volume;       /* Linear gain; finite and non-negative. */
} ma_volume_envelope_point;

ma_result ma_sound_set_volume_envelope(ma_sound* pSound,
                                       const ma_volume_envelope_point* pPoints,
                                       ma_uint32 pointCount);
```

- Set it after the sound is initialized and before the first playback. Do not call it from the
  audio thread or concurrently with playback.
- `pointCount == 0` clears the envelope; `pPoints` may then be `NULL`.
- Otherwise `pPoints` must be non-`NULL`, frame indices must be strictly increasing, and every
  volume must be finite and non-negative. Invalid input returns `MA_INVALID_ARGS` and leaves the
  previous envelope unchanged.
- The points are allocated with the engine's allocation callbacks and copied; the caller's array is
  never retained.
- Positions before the first point hold the first volume, positions after the last hold the last
  volume, and positions between points are linearly interpolated. One gain is shared by all
  channels of a frame and no clipping is applied.
- A sound with no envelope behaves exactly as it did before this API existed.

After an explicit `ma_sound_seek_to_pcm_frame()` on a sound with an envelope, pre-read source data
and resampler history are discarded, so the next output matches a fresh sound that was seeked to
the same position before starting. A natural loop keeps its filtering history.
