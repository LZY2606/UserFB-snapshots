# Cut windows: cuts that follow their parent's length

`mlt_producer_set_cut_window()` opts an existing cut into a *cut window*: a
requested interval of source frames that is re-evaluated every time the parent
producer's length changes. It is intended for nested timelines, where a cut
that references a sub-sequence should keep covering the same source frames
when clips are added to or removed from that sub-sequence, and a fixed trim
should remember the source interval it was meant to cover even if the
underlying media is temporarily shortened.

## Interface

```c
int mlt_producer_set_cut_window(mlt_producer cut, mlt_position in, mlt_position out);
```

* `cut` must be an existing, non-blank, non-mix cut (`mlt_producer_cut()`).
  Its parent may be a plain producer, a playlist, or a tractor.
* `in` is the requested starting source frame and must be non-negative.
* `out` is the requested ending source frame (closed interval), or `-1` to
  follow the end of the parent. Otherwise `out >= in`.
* Invalid arguments return non-zero and leave the cut unchanged.
* Calling it again on an enabled cut replaces the requested window.
* Calling `mlt_producer_set_in_and_out()` on an enabled cut exits the
  following mode and applies fixed in/out points by the usual rules.
* Static cuts (the default) are completely unaffected.

## Effective window and empty windows

The cut stores the *requested* window (`_cut_window_in` / `_cut_window_out`
properties). On every change of the parent's length the request is intersected
with `[0, length - 1]`:

* Non-empty intersection: `mlt_producer_get_in()` / `mlt_producer_get_out()` /
  `mlt_producer_get_playtime()` return the intersection, and
  `mlt_producer_get_length()` tracks the parent's length.
* Empty intersection (the parent is shorter than the requested window): the
  cut is normalized to `in = 0`, `out = -1`, `playtime = 0`. The cut object
  and the requested window are retained, so the cut recovers its requested
  frames when the parent grows again. The last effective `out` is never reused
  as the next request.

Example: a request of `[5, 14]` yields 10 frames at parent length 20,
`[5, 7]` at length 8, an empty cut at length 3, and `[5, 14]` again when the
parent returns to 20. A request of `[5, -1]` yields `[5, 19]` at length 20.

Changing only the parent's in/out points without changing its length does not
affect the requested or effective window.

## Empty windows and frame production

* Calling `get_frame` on an empty cut returns the framework's blank test frame
  (`test_image` / `test_audio`) without consuming a frame from the parent.
* A playlist keeps zero-length entries: their position and object identity are
  preserved, `clip_info`, `clip_start`, and the total length track the
  effective window, repeats multiply the effective frame count, and entries
  are never removed automatically.
* Sequential playback and random seeks skip zero-length entries. When the play
  head is past the end, `pause` selects the last entry that still has frames
  and `loop` selects the first one; if every entry is empty, a blank frame is
  produced (no division by zero, no replay of stale frames).

## Service graph behavior

The cut listens to `producer-changed` on its parent, so changes propagate
through playlists, nested playlists, multitracks, and tractors in the same
call. After any mutation returns, the whole dependency chain is consistent,
and any `producer-changed` callback observes the object's window, length, and
playlist entry cache in agreement. Multiple cuts may share one parent; each
follows its own window and none of them modifies the parent's trim. Removing
a playlist entry, closing the cut, or closing the container releases the
listener, so parent and cut never keep each other alive.
