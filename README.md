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

# Requirements

Go version >= 1.6

ffmpeg version >= 3.0 (optional)

# Receiving multiple published streams on one connection

By default `Server.HandlePublish` serves the classic one-stream-per-connection
flow. Set `Server.HandlePublishSession` instead to accept several streams that
publish concurrently over a single RTMP connection (each created with its own
`createStream` and identified by the message stream id). The handshake, chunk
streaming and AMF0 encoding stay shared; only the per-stream probing and
publishing state is kept per stream.

```go
package main

import (
	"fmt"
	"io"

	"github.com/nareix/joy4/format/rtmp"
)

func main() {
	server := &rtmp.Server{
		Addr: ":1935",
		HandlePublishSession: func(sess *rtmp.PublishSession) {
			// One connection can carry several interleaved publishers.
			// ReadEvent returns events of every stream on this connection.
			for {
				ev, err := sess.ReadEvent()
				if err == io.EOF {
					return
				}
				if err != nil {
					panic(err)
				}

				switch ev.Kind {
				case rtmp.PublishEventStart:
					// ev.Name is the raw name from the publish command.
					fmt.Printf("stream %d started: %s\n", ev.StreamID, ev.Name)

				case rtmp.PublishEventHeader:
					// ev.Streams holds the probed codecs of this stream.
					fmt.Printf("stream %d header: %d track(s)\n", ev.StreamID, len(ev.Streams))

				case rtmp.PublishEventPacket:
					// ev.Packet keeps the stream's own track index, DTS,
					// composition time and keyframe flag.
					fmt.Printf("stream %d packet idx=%d time=%s\n",
						ev.StreamID, ev.Packet.Idx, ev.Packet.Time)

				case rtmp.PublishEventEnd:
					fmt.Printf("stream %d ended\n", ev.StreamID)
				}
			}
		},
	}
	if err := server.ListenAndServe(); err != nil {
		panic(err)
	}
}
```

`ReadEvent` returns `io.EOF` once the connection is closed and every already
received event has been drained; `Close` shuts down the whole connection. The
single-stream `HandlePublish` / `HandlePlay` callbacks keep their original
behavior and are used whenever `HandlePublishSession` is not set.

# TODO

HLS / MPEG-DASH Server

ffmpeg.VideoEncoder / ffmpeg.SWScale

# License

MIT
