# Sound volume envelopes

`ma_sound_set_volume_envelope()` attaches a per-sound gain curve that is defined on the
**source material's frame timeline** rather than on engine time. This makes it suitable for
audio-editing previews: when the playback speed (pitch) changes, or when the range or loop
points are moved, the curve follows the material instead of drifting relative to the
speakers.

## API

```c
typedef struct
{
    ma_uint64 frameIndex;   /* Source frame, relative to the start of the data source's range. */
    float     volume;       /* Gain to apply. Must be finite and >= 0. */
} ma_volume_envelope_point;

ma_result ma_sound_set_volume_envelope(ma_sound* pSound,
                                       const ma_volume_envelope_point* pPoints,
                                       ma_uint32 count);
```

`count == 0` clears any existing envelope; `pPoints` may then be `NULL`. For `count > 0` the
point array must be non-`NULL`, frame indices must be strictly increasing, and every volume
must be finite and non-negative; otherwise `MA_INVALID_ARGS` is returned and the sound's
existing configuration is left untouched.

The points are **copied** into memory allocated with the engine's `allocationCallbacks`, so
the caller's array is only borrowed for the duration of the call.

## Semantics

- `frameIndex` uses the same clock as `ma_data_source_get_cursor_in_pcm_frames()`: it is a
  source frame position relative to the beginning of the configured range. It is *not* an
  engine-global time and *not* a count of frames delivered to the speakers.
- Before the first point and after the last point, the endpoint values are held.
- Between two points, the gain is linearly interpolated by source frame index.
- All channels of a frame receive the same gain. The gain is not clipped.
- The envelope is applied to source samples after conversion to f32, and before the existing
  resampler, channel conversion, volume/fade and output processing. The result matches
  pre-weighting the material by source frame and then playing it with the same range, loop
  points and pitch.
- When the source loops naturally, the envelope resumes from the loop point's source
  position, not from frame 0 or the first point. Filter/resampler history is preserved.
- After an explicit `ma_sound_seek_to_pcm_frame()` on an envelope-enabled sound, prefetched
  audio and resampler history from before the jump are discarded, so the next output matches
  a freshly configured sound started at that position.
- Existing `ma_sound_set_volume()` and fade calls are independent of the envelope and keep
  their usual position in the processing chain.

## Intended usage

Set the envelope after initialization and before the first `ma_sound_start()`. Replacing or
clearing it before playback is supported; changing it while the sound is playing, or calling
this concurrently from multiple threads, is not required or supported.

The sound must be backed by an addressable, seekable data source with a known finite length
and cursor. Range and loop points should be configured before playback. Chaining, custom
self-managed range/loop data sources, and replacing the underlying data source or the object
returned by `ma_sound_get_data_source()` are not supported with an envelope attached.

## Memory

The envelope allocates storage proportional to the point count, plus the sound's existing
fixed-size processing cache. No memory is allocated on the audio thread, and the data source
read callback does not allocate.

## Example

See [`offline_render.c`](offline_render.c) for a complete, device-free rendering example.
Build it from the repository root with:

```sh
cc -std=c99 -O2 -DMA_NO_DEVICE_IO -DMA_NO_RESOURCE_MANAGER \
   -DMA_NO_DECODING -DMA_NO_ENCODING -I. \
   examples/volume_envelope/offline_render.c miniaudio.c -lm -pthread \
   -o offline_render
./offline_render
```
