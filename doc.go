/*
Package oggvorbis decodes audio from ogg/vorbis files.

In addition to a single ogg/vorbis logical stream, the Reader supports chained
ogg streams: a physical file that concatenates several independently encoded
vorbis logical streams, one after another. Each segment runs from a
beginning-of-stream page to an end-of-stream page, has a constant serial
number, and numbers its pages from zero; the next segment uses a serial number
that has not appeared before.

# Global sample coordinates

All positions and lengths use one global frame coordinate over the whole
chain. Frame zero is the first frame of the first segment, and every following
segment starts immediately after the previous segment's last valid frame.
Coordinates count frames, not interleaved float values; Reader.Position always
returns the index of the next frame that Read will hand out, and page
readahead, decoder buffering and the per-segment granule reset never make it
move backwards. The PCM produced by Read is exactly the per-segment decodes
concatenated in order; a single Read call may cross a segment boundary. An
end-of-stream page's granule only trims the tail of its own segment.

# Supported chains

Every segment of a chain must share the first segment's sample rate and
channel count. No resampling or channel conversion is performed. Setup
packets, codebooks, block sizes, bitrates and comment headers may differ
between segments. SampleRate, Channels, Bitrate and CommentHeader always
report the first segment's values, even after playback or a seek moves into a
later segment.

Malformed containers are reported as errors and never skipped: a format
mismatch between segments, a different serial appearing before end of stream,
a segment missing its end-of-stream page before another beginning-of-stream
page, a continued-packet flag that does not match the previous page's lacing,
a broken page, a bad CRC or a page sequence gap.

# End of file compatibility

A final segment whose last page lacks the end-of-stream flag is still accepted
when the physical end of file is reached right after a complete page and no
packet is left unfinished; the last valid granule position is then used as the
segment's end. A truncated page, a dangling (unfinished) packet, or another
beginning-of-stream page after such a segment is an error, not a compatibility
case.

# Seek index and its cost

With an io.Reader, data is consumed strictly in order and at most one page is
read ahead, so the whole chain is never buffered; Length stays zero until the
physical end of file is confirmed.

With an io.ReadSeeker, NewReader scans the container once: every page is
parsed and CRC-checked, and an index of page offsets/granules, per-segment
header packets and trimmed segment lengths is built. No audio is decoded
ahead and no compressed page content is retained, so the index grows with the
number of pages, segments and header packets rather than with the audio's
duration; the PCM working set stays bounded by one decoder's block size.
SetPosition binary-searches the index for the containing segment and anchor
page, re-reads only the pages needed to rebuild any multi-page packet and the
vorbis preroll, and resumes at the exact target frame; unrelated prefixes are
never rescanned or decoded.
*/
package oggvorbis // import "github.com/jfreymuth/oggvorbis"
