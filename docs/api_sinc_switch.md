---
layout: default
---

# Seamless Sinc Quality Switching

The three sinc based converters (`SRC_SINC_BEST_QUALITY`,
`SRC_SINC_MEDIUM_QUALITY` and `SRC_SINC_FASTEST`) can be switched between one
another while a stream is running. The transition is seamless: the converter
is not rebuilt, the accumulated audio history and the current source phase are
kept, so no past audio is lost and the playback position never jumps. Audio is
still obtained through the usual [full](api_full.md) or
[callback](api_callback.md) APIs (`src_process()` and
`src_callback_read()`).

`SRC_LINEAR` and `SRC_ZERO_ORDER_HOLD` do not participate in this feature.
Converters which are never prepared keep their original behaviour and memory
footprint exactly.

## Preparation

```c
int src_prepare_sinc_switch (SRC_STATE *state) ;
```

This must be called on a freshly created (`src_new()` /
`src_callback_new()`) or freshly reset (`src_reset()`) sinc converter, before
any input has been submitted. The call allocates, once, the storage required to
support later quality changes (a ring buffer large enough for the widest
sinc window). After preparation neither normal processing nor a quality switch
allocates any audio buffer.

The function is idempotent: calling it again on an already prepared converter
succeeds without allocating. Calling it on a converter which has already
consumed input, or on a `SRC_LINEAR` / `SRC_ZERO_ORDER_HOLD` converter,
returns an error and leaves the converter untouched.

## Changing quality

```c
int src_set_sinc_quality (SRC_STATE *state, int converter_type, long fade_frames) ;
```

`converter_type` must be one of the three sinc converter types. With
`fade_frames == 0` the new quality is used from the next output frame onwards.

With `fade_frames == N` where `N > 0`, the next `N` output frames are a
crossfade. For frame `j` of the crossfade (`j = 1 ... N`) the produced sample
on every channel is

```
output = old_quality_result * (1 - j/N) + new_quality_result * (j/N)
```

so the same weight is applied to all channels. Both results are evaluated at
the same source coordinate, each with filter history that continues naturally
from the start of the stream. The new quality is therefore never restarted
"from zero" as if it were new audio, and samples already produced are never
reordered, inserted or removed.

Only one crossfade may be active at a time. A second `src_set_sinc_quality()`
request while a crossfade is running returns an error and the running
crossfade continues unchanged. Requesting the quality that is already active
is a no-op and starts no transition.

The switch does not change the conversion ratio or the accumulated source
phase. At a fixed ratio, changing the input chunk sizes or output capacities
does not change the result beyond floating point rounding. Ratio changes are
still handled with the original `src_process()` / `src_callback_read()`
semantics.

## Reset, clone and end of input

`src_reset()` clears the buffered audio and any transition progress but keeps
the preparation. If a crossfade is unfinished when `src_reset()` is called, the
transition is discarded and the converter reverts to the previously stable
(old) quality.

`src_clone()` duplicates the whole switching state, so a clone can finish a
crossfade that was already in progress independently of the original.

End of input is handled exactly as for an ordinary stream; the converter does
not extend the program to accommodate a fade.

Both functions return zero on success and a non-zero error code (decodable
with `src_strerror()`) on failure. Invalid requests — an unprepared converter,
a bad converter type or a negative `fade_frames` — return an error without
modifying the converter state.
