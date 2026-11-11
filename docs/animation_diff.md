# Frame diffing for animations

Every time an [Animation] updates, Mordant renders the new frame and compares it against the frame
currently on screen. Instead of rewriting the whole block, it writes only the cells, rows and
styles that changed. This page describes how that works, when it gives up and redraws the whole
frame, and how state is restored when an animation is interrupted.

## What is compared

The comparison is done on **rendered screen state, not on widget objects**. A frame snapshot is
built *after* all of the following have run:

1. The widget's `measure` and `render`, using the current terminal width.
2. Column-width wrapping and layout.
3. Style downsampling for the terminal's ANSI level and hyperlink support.

The rendered `Lines` are then flattened into **cells**, where each cell is one
[grapheme cluster](https://www.unicode.org/reports/tr29/) paired with its downsampled
`TextStyle`. Two cells are equal only when both their text *and* their style match, so a color-only
change is detected even if every visible character is identical.

Because a cell always holds a complete grapheme cluster, a diff can never cut through a combining
mark, an emoji ZWJ sequence, a flag sequence or any other multi-codepoint cluster. Wide characters
(CJK, emoji) occupy their full two-column width in all cursor arithmetic.

## Frame identity and when a snapshot is discarded

Each snapshot carries an identity describing the environment it was rendered in. Two frames are
only ever diffed when their identities match:

- **Terminal generation** — bumped whenever the screen may have changed outside the animation's
  control: output printed while the animation is active, another animation reflowing the screen,
  `stop()`/`clear()`, or an exception during rendering.
- **Column width** — the terminal width used to measure and wrap the frame.
- **Style capabilities** — ANSI color level, hyperlink support, cursor-sequence support, whether
  the output is interactive, and whether a carriage return also moves down a row.

A resize, a capability change, or interleaved output therefore always invalidates the old snapshot
and forces the next frame to be redrawn in full.

## The diff algorithm

With a valid snapshot, the differ works at two levels:

### Rows

It finds the common row prefix and suffix. The rows in between fall into one of three cases:

- **Same height:** each changed row is rewritten in place. The cursor moves between rows with
  relative up/down commands; no line feeds are emitted, so physical wrapping cannot shift the
  geometry.
- **Pure insertion:** new rows appear in one contiguous block. The suffix is scrolled down with
  `CSI n L` (insert line) and the new rows are written.
- **Pure deletion:** rows disappear in one contiguous block. `CSI n M` (delete line) scrolls the
  suffix up.
- A mixed restructure (both inserted and deleted rows with moved content) is not diffed; the whole
  frame is redrawn.

### Within a row

The common cell prefix and suffix of the old and new row are found. The prefix is skipped with a
horizontal cursor move, the middle is rewritten, and the suffix is either skipped (if it did not
shift) or rewritten whole (if the middle's width changed and moved it). A shorter new row erases its
leftover tail with `CSI 0 K`. SGR transitions start from the style the terminal is actually holding
at the cursor, so a rewritten middle never leaks the wrong color into a skipped prefix or suffix.

After the changes, the cursor is moved to the end of the last frame row — the same position a full
redraw leaves it at — and every modified line ends with an SGR reset.

## When diffing is abandoned in favor of a full redraw

A full redraw is used whenever an incremental update cannot be proven safe or is not worth it:

- There is no previous snapshot, the snapshot's generation changed, or its identity no longer
  matches (resize, capability change, interleaved output, stop/clear, prior exception).
- The terminal is not interactive, does not support the full cursor vocabulary (for example the
  IntelliJ run console), or treats a carriage return as a line feed (the wasm target).
- Any rendered line is wider than the terminal and would physically wrap.
- The row structure changed in a way that is not a pure insertion or pure deletion.
- **Byte budget:** the diff is only used when its UTF-8 byte count is *strictly smaller* than the
  bytes a full redraw of the new frame would write. If the diff is equal or larger, the full frame
  is redrawn. This is why small frames (a spinner or a two-character counter) typically keep using
  full redraws: the cursor positioning alone would cost more than the frame text.

## State restoration on interruption

Rendering is fully separated from writing: a frame is rendered and its diff is computed before any
byte is sent. If rendering throws (including a coroutine cancellation), nothing partial reaches the
screen. In that case the snapshot is invalidated, the generation advances, and — on interactive
terminals — an SGR reset (`ESC [ 0 m`) is written directly, bypassing the animation's interceptor so
it cannot trigger a frame reflow. The exception is then rethrown. The next successful `update`
starts from a full redraw.

If a write itself fails partway, the on-screen content can no longer be assumed to match any
snapshot, so the same invalidation and style-reset path runs; the following frame is again a full
redraw. On `stop()` and `clear()` the snapshot is discarded and the generation advanced, so a later
resume never diffs against stale screen state.

Non-interactive output is unchanged: animations render nothing to a non-interactive stream, exactly
as they did before diffing existed.

## Update byte counts

The point of diffing is to reduce the bytes written per frame. Concretely, for a frame of width
`w` cells across `h` rows:

- A full redraw writes all frame bytes plus the cursor-up/erase preamble, so it is roughly
  proportional to the whole frame size.
- A single changed cell in one row writes a constant-size preamble (a carriage return, up to two
  horizontal moves and an SGR reset) plus the changed cluster — independent of `w` and `h`.
- Inserting or deleting `n` rows costs one `CSI n L`/`CSI n M` sequence plus the bytes of the new
  rows, rather than repainting every row.

The hard guarantee is the byte budget above: an accepted diff is always fewer UTF-8 bytes than the
equivalent full redraw. For a concrete sense of scale, consider a single-line frame of 50 ASCII
cells whose last digit changes. A full redraw writes all 50 characters plus a carriage-return
preamble; the diff writes a carriage return, a `CSI 49 C` right move, one digit and an SGR reset —
on the order of a dozen bytes instead of fifty, and the saving grows with frame width. For a
two-character frame, by contrast, the positioning preamble alone exceeds the two-character full
redraw, so the byte budget rejects the diff and the frame is redrawn in full — this is deliberate,
not a missed optimization.

The tests under
`mordant/src/jvmTest/kotlin/com/github/ajalt/mordant/animation/diff/` pin both the exact control
sequences the differ emits (a single-cell edit, row insertion and deletion) and, end to end, that
replaying a diffing animation's bytes onto a virtual terminal yields the same screen as a
full-redraw animation. A replayable trace covering a full first frame, an in-place cell edit, an
unchanged frame, a row insertion and a row deletion lives at
`mordant/src/jvmTest/resources/animation/frame_diff_trace.txt`.

The frame-diffing code lives in `commonMain`; the virtual-terminal tests that exercise it are JVM
tests (`src/jvmTest`). The wasm target deliberately opts out of diffing because its
carriage-return semantics differ (`CR_IMPLIES_LF`), and the IntelliJ run console opts out because
it does not implement the full cursor vocabulary.

[Animation]: api/mordant/com.github.ajalt.mordant.animation/-animation/index.html
