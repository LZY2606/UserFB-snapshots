# This project is no longer maintained.

# go-mp3

[![Go Reference](https://pkg.go.dev/badge/github.com/hajimehoshi/go-mp3.svg)](https://pkg.go.dev/github.com/hajimehoshi/go-mp3) 

An MP3 decoder in pure Go based on [PDMP3](https://github.com/technosaurus/PDMP3).

[Slide at golang.tokyo #11](https://docs.google.com/presentation/d/e/2PACX-1vTTXf-LWNRvMVGQ7GI4Wh8EKohot_9CMtlF4dswpYGpuYKOek5NeNP-_QZnNcRFZp9Cwm0pCcykjqDN/pub?start=false&loop=false&delayms=3000)

## Supported streams

The decoder accepts MPEG-1 and MPEG-2 Layer III streams, including
**free-format** (bitrate index 0) files and network byte streams.

Free-format frames do not carry their length in the header, so the decoder
locks the frame length from three consecutive, mutually compatible frame
headers, taking each frame's padding bit into account. A sync word appearing
inside main data is never mistaken for a frame boundary. Once locked, frames
are decoded through the same side information, main data and bit reservoir
path as ordinary streams; the free bitrate is never mapped to a bitrate-table
entry.

Free-format support has the following properties:

- The MPEG version, sample rate and channel mode are fixed for the whole
  stream; the unpadded frame length is constant within 64–2000 bytes.
- Padding may differ from frame to frame. The presence of a CRC word is
  interpreted from each header's protection bit as usual.
- Look-ahead used for locking never exceeds 8192 bytes of compressed data;
  every byte read ahead is handed back to the decoder in order, without
  duplication or loss. The state belongs to the individual `Decoder`.
- Streams are decoded on the fly: no whole-file buffering and no full-PCM
  caching, so a non-seekable `Reader` that returns one byte at a time yields
  the same PCM as a seekable file.
- A stream that cannot provide three compatible headers returns an error.
  After a successful lock, the last complete frame is decoded at EOF; a
  truncated final frame produces no PCM.
- For seekable sources `Length` counts complete audio frames only (the
  look-ahead is not counted twice) and seeking back to offset 0 re-reads the
  whole stream. For non-seekable sources `Length` returns -1.

The output is always 16-bit little-endian, 2-channel PCM (4 bytes per
sample), even when the source is mono.

Not in scope: MPEG-2.5, streams that switch format mid-way, and history
reconstruction for random (non-zero) seeks.
