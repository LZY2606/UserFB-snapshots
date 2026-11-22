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

## MP4 reference files (external media)

Editing tools sometimes export a reference MP4 whose chunks stay in
external media files instead of the main `mdat`. Read such files with
`NewDemuxerWithResolver`; the resolver receives each dref url location
exactly as stored and the library never opens paths or network
resources itself. Resolved readers are cached per demuxer by the raw
location string and shared across tracks. The example below keeps the
external media in memory and needs no codec or external tool support:

```go
package main

import (
	"bytes"
	"fmt"
	"io"

	"github.com/nareix/joy4/format/mp4"
)

func main() {
	// referenceMP4Bytes is the reference MP4; the media map holds the
	// external media keyed by the raw location strings stored in its
	// dref url entries.
	var referenceMP4Bytes []byte
	var externalTrackA, externalTrackB []byte

	mainFile := bytes.NewReader(referenceMP4Bytes)
	media := map[string][]byte{
		"track-A.bin\x00": externalTrackA,
		"track-B.bin\x00": externalTrackB,
	}

	resolve := func(location string) (io.ReadSeeker, error) {
		raw, ok := media[location]
		if !ok {
			return nil, fmt.Errorf("unknown media location %q", location)
		}
		// A fresh reader per location; successful results are cached by
		// the demuxer, failed ones are retried on the next ReadPacket.
		return bytes.NewReader(raw), nil
	}

	demuxer := mp4.NewDemuxerWithResolver(mainFile, resolve)
	streams, err := demuxer.Streams() // only parses indices/config
	if err != nil {
		panic(err)
	}
	_ = streams

	for {
		packet, err := demuxer.ReadPacket()
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(err)
		}
		fmt.Printf("stream=%d time=%v bytes=%d\n", packet.Idx, packet.Time, len(packet.Data))
	}
}
```

The plain `NewDemuxer` keeps reading self-contained MP4 files and
returns an explicit error when it meets an external sample instead of
reading from the wrong file offset.

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

# Requirements

Go version >= 1.6

ffmpeg version >= 3.0 (optional)

# TODO

HLS / MPEG-DASH Server

ffmpeg.VideoEncoder / ffmpeg.SWScale

# License

MIT
