# This project is no longer maintained.

# go-mp3

[![Go Reference](https://pkg.go.dev/badge/github.com/hajimehoshi/go-mp3.svg)](https://pkg.go.dev/github.com/hajimehoshi/go-mp3) 

An MP3 decoder in pure Go based on [PDMP3](https://github.com/technosaurus/PDMP3).

## Random access

When the `io.Reader` passed to `NewDecoder` also implements `io.Seeker`,
`NewDecoder` walks the stream once to build a compressed-frame index and
sparse checkpoints of the cross-frame decoder state (bit reservoir and
overlap-add history). This one-time initialization cost grows with the
length of the stream; it is paid on construction, not on each seek. Decoded
PCM is never stored.

After that, `Seek` can jump anywhere exactly: every seek re-decodes at most 32
compressed frames, and the bytes read after a seek are identical to what a
sequential read would have produced at the same offset. Seeking to
`Length` is allowed, after which `Read` returns `io.EOF`.

If the source does not implement `io.Seeker`, `Seek` returns an error,
`Length` returns -1, and the decoder streams sequentially as before.

[Slide at golang.tokyo #11](https://docs.google.com/presentation/d/e/2PACX-1vTTXf-LWNRvMVGQ7GI4Wh8EKohot_9CMtlF4dswpYGpuYKOek5NeNP-_QZnNcRFZp9Cwm0pCcykjqDN/pub?start=false&loop=false&delayms=3000)
