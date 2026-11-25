# PLEASE USE joy5 INSTEAD

[joy5](https://github.com/nareix/joy5)

- High performance Copy-on-write gop cache [code](https://github.com/nareix/joy5/blob/master/cmd/avtool/pubsub.go)
- Better av.Packet design [code](https://github.com/nareix/joy5/blob/master/av/av.go)

# JOY4

> Golang audio/video library and streaming server

JOY4 is powerful library written in golang, well-designed interface makes a few lines of code can do a lot of things such as reading, writing, transcoding among variety media formats, or setting up high-performance live streaming server.

# Features 

Well-designed and easy-to-use interfaces:

- Muxer / Demuxer ([doc](https://godoc.org/github.com/nareix/joy4/av#Demuxer) [example](https://github.com/nareix/joy4/blob/master/examples/open_probe_file/main.go))
- Audio Decoder ([doc](https://godoc.org/github.com/nareix/joy4/av#AudioDecoder) [example](https://github.com/nareix/joy4/blob/master/examples/audio_decode/main.go))
- Transcoding ([doc](https://godoc.org/github.com/nareix/joy4/av/transcode) [example](https://github.com/nareix/joy4/blob/master/examples/transcode/main.go))
- Streaming server ([example](https://github.com/nareix/joy4/blob/master/examples/http_flv_and_rtmp_server/main.go))

Support container formats:

- MP4
- MPEG-TS
- FLV
- AAC (ADTS)

RTSP Client
- High level camera bug tolerance
- Support STAP-A

RTMP Client
- Support publishing to nginx-rtmp-server
- Support playing

RTMP / HTTP-FLV Server 
- Support publishing clients: OBS / ffmpeg / Flash Player (>8)
- Support playing clients: Flash Player 11 / VLC / ffplay / mpv
- High performance


Publisher-subscriber packet buffer queue ([doc](https://godoc.org/github.com/nareix/joy4/av/pubsub))

- Customize publisher buffer time and subscriber read position


- Multiple channels live streaming ([example](https://github.com/nareix/joy4/blob/master/examples/rtmp_server_channels/main.go))

Packet filters ([doc](https://godoc.org/github.com/nareix/joy4/av/pktque))

- Wait first keyframe
- Fix timestamp
- Make A/V sync
- Customize ([example](https://github.com/nareix/joy4/blob/master/examples/rtmp_server_channels/main.go#L19))

FFMPEG Golang-style binding ([doc](https://godoc.org/github.com/nareix/joy4/cgo/ffmpeg))
- Audio Encoder / Decoder
- Video Decoder
- Audio Resampler

Support codec and container parsers:

- H264 SPS/PPS/AVCDecoderConfigure parser ([doc](https://godoc.org/github.com/nareix/joy4/codec/h264parser))
- AAC ADTSHeader/MPEG4AudioConfig parser ([doc](https://godoc.org/github.com/nareix/joy4/codec/aacparser))
- MP4 Atoms parser ([doc](https://godoc.org/github.com/nareix/joy4/format/mp4/mp4io))
- FLV AMF0 object parser ([doc](https://godoc.org/github.com/nareix/joy4/format/flv/flvio))

# MP4 recording: sample-clock timing

The MP4 muxer supports two timing models. The default one is unchanged and
remains byte-compatible with earlier releases.

## Legacy timing (`NewMuxer`)

Each sample duration is the difference between two packet times, converted to a
90kHz tick on its own. This is simple, but on long recordings the per-sample
rounding error accumulates, and the final sample has no explicit end time (it
gets a zero duration).

```go
m := mp4.NewMuxer(w) // w is an io.WriteSeeker
if err := m.WriteHeader(streams); err != nil { ... }
for _, pkt := range packets {
    if err := m.WritePacket(pkt); err != nil { ... } // pkt.Time is absolute
}
if err := m.WriteTrailer(); err != nil { ... }
```

## Explicit sample clock (`NewClockMuxer`)

`NewClockMuxer(w, timescales)` records against an explicit sample clock.
`timescales` is copied by the constructor and has one entry per track in
`WriteHeader` order; each value must be in `[1, 1000000000]`. A mismatched
count or an out-of-range value is reported by `WriteHeader` before any bytes are
written.

```go
// One timescale per track: 44100 for audio, 90000 for video.
m := mp4.NewClockMuxer(w, []uint32{44100, 90000})
if err := m.WriteHeader(streams); err != nil { ... }
for _, pkt := range packets {
    if err := m.WritePacket(pkt); err != nil { ... }
}
// ends holds the absolute end time of the last packet on each track.
if err := m.WriteTrailerAt([]time.Duration{audioEnd, videoEnd}); err != nil { ... }
```

In clock mode the first packet on every track must have `Time == 0` and
subsequent times must strictly increase. Packet times are absolute.

### Rounding rule

Every absolute endpoint is converted to that track's ticks as
`round(ns * timescale / 1e9)`, rounding to the nearest integer and rounding an
exact half up. A sample's duration is the difference of two adjacent quantized
endpoints; it is *not* the rounding of each gap separately, which is what avoids
accumulated drift. The intermediate product is computed without overflowing
64 bits.

A commit is rejected (the packet is not consumed and pending samples are left
untouched, so a later valid packet still works) if:

- two adjacent endpoints quantize to the same tick, or
- a single sample duration exceeds `uint32`.

`WriteTrailerAt` validates every track before flushing: an empty track must pass
zero, a non-empty track's end must be later than its last packet and must
quantize to a positive duration. A bad argument can be corrected and retried.
Calling `WriteTrailer` on a clock muxer, or `WriteTrailerAt` on a legacy muxer,
returns an error.

The `stts` sample durations, the `mdhd` duration, and the `tkhd`/`mvhd` movie
durations all derive from the same set of quantized sample endpoints. The movie
timescale is 10000; a track duration is floored when converted to it, and the
movie duration is the maximum across tracks.

### Long recordings

Durations are stored as `uint64` in `mp4io.MovieHeader`, `TrackHeader` and
`MediaHeader`. A box is written with version 1 when its own duration exceeds
`uint32`, otherwise version 0, so short files keep their historical bytes. Both
versions read create time, modify time and duration fully; an unsupported
version or a truncated field is reported as a parse error. Reading a
clock-timed file keeps `ReadPacket`, `CurrentTime` and `SeekToTime` correct past
32 bits of ticks.

# Requirements

Go version >= 1.6

ffmpeg version >= 3.0 (optional)

# TODO

HLS / MPEG-DASH Server

ffmpeg.VideoEncoder / ffmpeg.SWScale

# License

MIT
