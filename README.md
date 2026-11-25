# rtmp

golang rtmp lib

## Enhanced RTMP HEVC (hvc1)

The existing `av.Packet`, `flv.Muxer`/`flv.Demuxer`, and `rtmp.Conn`
interfaces also carry Enhanced RTMP v2 HEVC video. Set `TrackID` for a
nonzero Multitrack track. `av.HEVCDecoderConfig` is an
`HEVCDecoderConfigurationRecord`; `av.HEVC` contains the length-prefixed NAL
units. Each HEVC packet includes a private configuration snapshot in `HEVC`
and `HEVCConfig`. The outer packet timestamp is DTS; `CTime` is the
composition offset in milliseconds.

```go
package main

import (
	"time"

	"github.com/notedit/rtmp/av"
	"github.com/notedit/rtmp/format/flv"
)

func writeHEVCTrackSwitch(mux *flv.Muxer, config4, frame4, config1, frame1 []byte) error {
	// Track 2 starts with a four-byte NAL length field.
	if err := mux.WritePacket(av.Packet{
		Type:    av.HEVCDecoderConfig,
		TrackID: 2,
		Time:    100 * time.Millisecond,
		Data:    config4,
	}); err != nil {
		return err
	}
	if err := mux.WritePacket(av.Packet{
		Type:       av.HEVC,
		TrackID:    2,
		IsKeyFrame: true,
		Time:       140 * time.Millisecond, // DTS
		CTime:      10 * time.Millisecond,  // PTS = DTS + CTime
		Data:       frame4,                 // 4-byte length prefixes
	}); err != nil {
		return err
	}

	// A new SequenceStart on the same track changes its NAL length width
	// immediately; here the replacement record selects one-byte prefixes.
	if err := mux.WritePacket(av.Packet{
		Type:    av.HEVCDecoderConfig,
		TrackID: 2,
		Time:    200 * time.Millisecond,
		Data:    config1,
	}); err != nil {
		return err
	}
	return mux.WritePacket(av.Packet{
		Type:    av.HEVC,
		TrackID: 2,
		Time:    240 * time.Millisecond,
		Data:    frame1, // 1-byte length prefixes
	})
}
```

On the receiving side, call `ReadPacket` as before. A Multitrack message is
returned as one packet per call, in member order; the demuxer or RTMP
connection maintains the configuration for each track independently.

## Thanks

 - [joy5](https://github.com/nareix/joy5)
