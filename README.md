# wav codec

[![GoDoc](https://godoc.org/github.com/go-audio/wav?status.svg)](https://godoc.org/github.com/go-audio/wav)
[![Go Report Card](https://goreportcard.com/badge/github.com/go-audio/wav)](https://goreportcard.com/report/github.com/go-audio/wav)
[![Coverage Status](https://codecov.io/gh/go-audio/wav/graph/badge.svg)](https://codecov.io/gh/go-audio/wav)
[![Build Status](https://travis-ci.org/go-audio/wav.svg)](https://travis-ci.org/go-audio/wav)

Wav audio file encoder and decoder. See [GoDoc](https://godoc.org/github.com/go-audio/wav) for more details.

## RF64 (EBU Tech 3306)

Long recordings can outgrow the 32-bit RIFF length fields. The decoder reads
`RF64/WAVE` containers transparently: `ds64` length substitution (including
per-chunk table entries), unknown chunks around `fmt` and after `data`, and
tail metadata via the usual `ReadInfo`, `FwdToPCM`, `NextChunk`, `PCMBuffer`,
`FullPCMBuffer`, `PCMLen`, `Duration`, `Rewind` and `ReadMetadata` APIs. The
existing RIFF APIs and default encoder output are unchanged.

The encoder can reserve the RF64 header area up front and promote in place
when the finished container is too large:

```go
out, err := os.Create("long-recording.wav")
if err != nil {
	log.Fatal(err)
}
e := wav.NewEncoder(out, sampleRate, bitDepth, channels, 1)
e.AutoRF64 = true // set before the first Write or WriteFrame
// ... Write()/WriteFrame() as usual ...
if err := e.Close(); err != nil { // the caller still closes the writer
	log.Fatal(err)
}
```

`AutoRF64` is frozen at the first `Write` or `WriteFrame`. A 28-byte `JUNK`
chunk is reserved after `WAVE`; if the final RIFF or data size reaches
`0xffffffff`, `Close` rewrites that area as `ds64` in place (audio is never
copied) and switches the outer and `data` lengths to `0xffffffff`. Small files
stay ordinary `RIFF` with the reserved `JUNK` and real 32-bit lengths.
Trailer metadata, the word-alignment byte for odd PCM and the reserved chunk
count towards `riffSize` but not towards `dataSize` or the frame count.

Supported scope: integer PCM, 8/16/24/32 bits, mono or stereo, on 64-bit Go.

