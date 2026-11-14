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

# MP4 muxing: legacy and precise sample clocks

The MP4 muxer supports two timing modes. `NewMuxer` keeps the historical
behavior (each sample duration is derived from the difference of two adjacent
packet times, then converted to track ticks), while `NewClockMuxer` records
timing against a precise per-track sample clock driven by absolute packet
times.

## Legacy muxer (unchanged output)

```go
import (
    "io"
    "time"
    "github.com/nareix/joy4/av"
    "github.com/nareix/joy4/format/mp4"
)

func writeLegacy(w io.WriteSeeker, streams []av.CodecData, pkts []av.Packet) error {
    m := mp4.NewMuxer(w)
    if err := m.WriteHeader(streams); err != nil {
        return err
    }
    for _, pkt := range pkts {
        if err := m.WritePacket(pkt); err != nil {
            return err
        }
    }
    return m.WriteTrailer()
}
```

## Precise clock muxer

`NewClockMuxer(w, timescales)` takes one time scale per track, in the same
order as the `CodecData` slice passed to `WriteHeader`. Each time scale must be
in `[1, 1000000000]`; a count mismatch or an out-of-range value is reported by
`WriteHeader` before any file header byte is written. In this mode the first
packet of every track must have `Time == 0` and later packet times must be
strictly increasing. The file is finished with `WriteTrailerAt`, which takes
the absolute end time of the last packet of every track.

```go
func writeClock(w io.WriteSeeker, streams []av.CodecData, timescales []uint32,
        pkts []av.Packet, ends []time.Duration) error {
    m := mp4.NewClockMuxer(w, timescales) // timescales is copied
    if err := m.WriteHeader(streams); err != nil {
        return err
    }
    for _, pkt := range pkts {
        if err := m.WritePacket(pkt); err != nil {
            // A rejected packet is not consumed: the pending samples are
            // untouched, so a later legal packet can still be committed.
            continue
        }
    }
    // All tracks are validated before anything is flushed, so a bad end
    // time can be corrected and the call retried.
    return m.WriteTrailerAt(ends) // ends is copied before validation
}
```

### Rounding rules

- Every **absolute** time endpoint is converted to track ticks independently as
  `round(time_ns * timescale / 1e9)`, rounding to the nearest integer and
  rounding **up** on an exact half tick. Per-segment nanosecond differences are
  never rounded individually.
- A sample duration is the difference of two consecutive quantized endpoints.
- Two endpoints that quantize to the same tick, or a single sample longer than
  `uint32` ticks, reject that packet without consuming it.
- The `stts` total, `mdhd` duration, `tkhd` duration and `mvhd` duration all
  come from the same set of actual sample endpoints. Track durations are
  converted to the 10 kHz movie time scale using floor rounding, and `mvhd`
  takes the maximum across tracks.
- When a box's duration exceeds 32 bits, that `mvhd`/`tkhd`/`mdhd` box is
  written as version 1 (64-bit creation/modification times and duration);
  otherwise it stays version 0. Legacy short files keep their exact byte layout.

# Requirements

Go version >= 1.6

ffmpeg version >= 3.0 (optional)

# TODO

HLS / MPEG-DASH Server

ffmpeg.VideoEncoder / ffmpeg.SWScale

# License

MIT
