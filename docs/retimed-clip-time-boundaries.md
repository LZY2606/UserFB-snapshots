# RetimedClip time boundaries

`RetimedClip` is a `Clip` subclass that participates in editorial layout at a
playback rate.  It adds one serialized field, `playback_rate`, and gives the
clip a *program duration* that differs from its *source length*.

This note describes how the two coordinate systems relate.  It is a companion
to `src/opentimelineio/retimedClip.h` and the regression suite in
`task_support/retime_test.cpp`.

## Source coordinates vs. program coordinates

* **Source coordinates** are the media's own time.  `source_range` is a
  half-open interval `[start, end)` in that media, exactly as for a plain
  `Clip`.  When no `source_range` is set, the media reference's
  `available_range` is used, again exactly as for a plain `Clip`.
* **Program coordinates** are the parent composition's time.  A track lays out
  its children one after another; a `RetimedClip` claims
  `|source_range| / |playback_rate|` units of program time.

The duration rule is therefore:

```
program_duration = source_length / abs(playback_rate)
```

The duration keeps the rate of the source `RationalTime`; only its value is
scaled.  Nothing is rounded: fractional program lengths from fractional rates
are preserved.

## The two playback directions

A positive rate plays forward from the source start.  A negative rate plays
backward from the **exclusive** source end.  Writing `W = 1 / playback_rate`
and referring to the trim as `S = source_range`:

For a time `t` in source coordinates, its program time (relative to the
clip's own start in the parent) is:

```
forward  (rate > 0): (t - S.start)          * W
backward (rate < 0): (t - S.end_exclusive)  * W
```

Equivalently, in parent absolute time add the clip's `range_of_child` start.

The inverse (program time to source time) is:

```
forward  (rate > 0): S.start           + program_offset / W
backward (rate < 0): S.end_exclusive   + program_offset / W
```

Because both directions use the same linear map on continuous values, the
forward and reverse conversions are exact inverses and compose across a
parent chain without accumulating rounding.

### Worked example (the one used by the tests)

* source range `[100, 148)` at 24 fps (so `|S| = 48`)
* `playback_rate = -2`
* a 10-frame `Gap` before the clip in the parent track

Program duration is `48 / 2 = 24`.  The clip starts at program frame 10.
Because playback is reversed, source frame 148 (the exclusive end) maps to the
program start, and source frame 100 (the source start) maps to the program
end:

| source boundary | program time |
| --------------- | ------------ |
| 148             | 10           |
| 136             | 16           |
| 100             | 34           |

`RetimedClip::transformed_time()` and `transformed_time_range()` (via the
base `Item` machinery) produce these values, and the reverse conversion
`track->transformed_time(16, clip)` yields `136` again.

## Ranges are rearranged, never negative

`Item::transformed_time_range()` transforms both endpoints independently and
then orders them.  For a reversed clip the two endpoints swap order relative
to the source; the result is rearranged so the returned `TimeRange` always has
a non-negative duration.  For example, the source range `[124, 140)` with rate
`-2` maps to program times `22` and `14`; the returned program range is
`[14, 22)`.

## Handles and transitions

`visible_range()` starts from the trimmed source range and extends it by the
handles reported by the parent track.  Those handles are measured in **program
time**.  They are converted to source lengths by multiplying by
`abs(playback_rate)`, and for reversed playback the head and tail directions
are swapped, because the program head plays the source end.

Example: source `[100, 148)`, rate `-2`, head handle 2, tail handle 3 (program
frames).  In source terms the head handle is `2 * 2 = 4` frames and the tail
handle is `3 * 2 = 6` frames.  Reversed, the head extends the source end and
the tail extends the source start:

```
visible source range = [100 - 6, 148 + 4) = [94, 152)
```

The stored `source_range` is never pre-shrunk or rewritten to achieve this; it
keeps its meaning as the trimmed source interval.

## Trimming

`track_trimmed_to_range()` clips a track to a program-time range.  For an item
that retimes, the program-time head and tail trim amounts must be scaled by
`abs(playback_rate)` to become source-time amounts, and again the directions
swap for reversed playback.  `RetimedClip` overrides
`Item::trim_source_range_in_parent_time()` for this; plain clips keep the
existing 1:1 behaviour.

For the worked example, the clip's program extent in the parent is
`[10, 34)`.  Trimming the parent track to `[14, 22)` removes 4 program frames
from the head (`14 - 10`) and 12 program frames from the tail (`34 - 22`).
Scaled to source time that is `4 * 2 = 8` and `12 * 2 = 24` source frames.
Because playback is reversed, the program head trims the source **end** and
the program tail trims the source **start**:

```
source start = 100 + 24 = 124
source end   = 148 - 8  = 140
source range = [124, 140)
```

The trimmed program length is `16 / 2 = 8` frames, matching the 8-frame trim
range.  These are exactly the values asserted by
`editing_json/trim_reverse` in `task_support/retime_test.cpp`, and the input
track is left unchanged.

The important invariant is that the *duration* relationship is preserved:
the trimmed program length is `source_length / abs(playback_rate)`.

## Nested compositions and mixed rates

Time conversion always walks the real parent chain.  Each step between a child
and its parent converts to the parent's rate using `RationalTime`'s
rate-preserving arithmetic, so 24 fps, 25 fps and 48000 Hz can be mixed in one
tree without integer rounding at any step.  A `source_range` on an intermediate
`Track` or `Stack` shifts that layer's contribution just as it does for plain
items.

## Serialization and errors

`RetimedClip` serializes as `RetimedClip.1` and inherits the `Clip` schema
fields (`media_references`, `active_media_reference_key`, `source_range`,
`effects`, `markers`, `metadata`, ...).  `clone()` and the JSON round-trip
preserve the dynamic type and the playback rate.

The playback rate must be finite and non-zero:

* `set_playback_rate()` returns `false`, fills the `ErrorStatus`, and leaves
  the previous value in place for `0`, `NaN`, `+inf` or `-inf`.
* Deserializing a `RetimedClip.1` whose `playback_rate` is missing or invalid
  fails with an error; it never silently degrades to a plain `Clip`.
