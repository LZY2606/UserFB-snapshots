# VBRI Seek Index in the MP3 Demuxer

The MP3 demuxer (`symphonia-bundle-mp3`) can use the table-of-contents (TOC) carried in a
VBRI header to accelerate seeking. This page documents the coordinate system of the table,
how seek anchors are chosen, how the bit-reservoir preroll is preserved, and when the table
is ignored.

## Table coordinates

A VBRI tag lives in the first MPEG frame of the stream, 32 bytes after the end of the 4-byte
frame header. After the fixed header (`num_bytes`, `num_mpeg_frames`), the TOC consists of
four big-endian `u16` fields — entry count, scale factor, entry width, and frames per
entry — followed by `entry_count` big-endian entries of `entry_width` bytes each (widths of
1, 2, 3, and 4 bytes are supported). The byte span of a group of frames is the entry value
multiplied by the scale factor. The final group may cover fewer than `frames_per_entry`
frames.

All byte offsets are cumulative spans measured from the start of the MPEG frame that
contains the VBRI tag itself — not from the start of the file, and not from the first audio
packet after metadata. Consequently:

* MPEG frame 0 is the VBRI tag frame. It is **not** an audio frame and never maps to a
  packet timestamp. Audio frame `i` (timestamp `i * frame_duration`, relative to the
  track's lower timestamp bound) corresponds to MPEG frame `i + 1`.
* The anchor byte position for group `g` is `first_frame_pos + sum(spans[0..g])`, and its
  timestamp is `(g * frames_per_entry - 1) * frame_duration` for `g > 0`. Group 0 is
  anchored at the first audio packet with the initial timestamp instead, exactly where a
  scan from the start would begin.

Because the table carries absolute byte spans, it can be used on any seekable source, even
one whose total byte length is unknown. Non-seekable sources keep the existing forward-only
behavior and never use the index.

## Anchor selection and reservoir preroll

Both `SeekMode::Accurate` and `SeekMode::Coarse` prefer an indexed anchor on seekable
sources. The anchor is the start of the group containing the target frame — never a
byte-proportional interpolation *within* a group, and the target timestamp is never derived
from the table directly. If the target frame is fewer than four frames (the maximum number
of reference frames considered during a seek) from the start of its group, an earlier group
is selected so that the subsequent frame-by-frame scan parses enough real frame headers to
populate the reference-frame ring before reaching the target.

After the pre-seek, the demuxer runs the same scan it always has: it parses real frame
headers, advances `next_packet_ts` frame by frame, and stops at the frame containing the
required timestamp. If that frame has a non-zero `main_data_begin`, up to four previously
parsed frames are examined and the reader rewinds to the oldest frame required to cover the
bit reservoir. Because the anchor guarantees those previous frames were actually parsed,
the seeked-to position and all subsequently delivered packets are identical to a scan from
the start of the stream.

## Validation and fallback

The TOC is an optional optimization. The whole table is disabled if any of the following
holds:

* zero entry count, zero scale factor, or zero frames per entry,
* an entry width outside 1..=4,
* truncated entries (the frame ends before all entries are read),
* overflow in the cumulative byte spans or in the frame coverage computation,
* frame coverage that does not match `num_mpeg_frames` (the final group may be short, but
  must not be empty),
* cumulative spans exceeding the declared audio byte range (`num_bytes`).

Disabling the table never invalidates the basic VBRI header: the declared frame count and
the skipping of the tag frame are retained.

At seek time, after an anchor is selected, the demuxer verifies that a plausible MPEG frame
header (synced, parseable, and similar to the first frame of the stream) starts exactly at
the anchor position. If this check fails, the index is disabled for the remainder of the
session and the seek falls back to a reliable scan from the first packet, so a bad index
cannot publish incorrect timestamps.

## Trust boundary

The index is metadata, and is trusted only as far as it can be checked cheaply. The demuxer
verifies the *structure* of the table and that selected anchors land on plausible frame
boundaries, but it does not attempt to prove that a structurally complete table describes
the true frame grouping of the stream. A file whose TOC entries are deliberately
mis-filled — while remaining structurally valid and landing on frame boundaries — can still
produce incorrect seek positions. Applications that cannot trust their input should treat
VBRI-derived seeking as a best-effort optimization, which is exactly what the fallback path
provides when the checks above do catch an inconsistency.
