# rtmp
golang rtmp lib

## Enhanced RTMP v2 multitrack HEVC

The same `ReadPacket`/`WritePacket` interfaces now carry Enhanced RTMP v2
video: plain `hvc1` messages plus `OneTrack` and `ManyTracks` multitrack
messages. A multitrack message yields one `av.Packet` per track; remaining
tracks are returned by the following `ReadPacket` calls before the next
message is read. The FLV demuxer and RTMP connection behave identically and
each keeps its own track-to-configuration state.

New packet fields (legacy callers can leave them zero):

- `av.HEVCDecoderConfig` / `av.HEVC` / `av.HEVCEndOfSequence` packet types
- `Packet.TrackID` — the track this packet belongs to (`0` for plain messages)
- `Packet.HEVC` — an independent snapshot of the track configuration captured
  when the packet was produced; later config switches never change it

Only `hvc1` is accepted. A `SequenceStart` replaces one track's
`HEVCDecoderConfigurationRecord`, a `SequenceEnd` clears it, and frames on an
unconfigured track fail with an explicit error. The per-track NAL length width
(1, 2 or 4 bytes) comes from that track's own record; the outer timestamp stays
the decode time while the in-frame 24-bit value remains the composition-time
offset.

### Example: non-zero track and a configuration switch

```go
var w io.ReadWriter // an RTMP connection transport, or an *os.File for FLV

conn := rtmp.NewConn(w)
conn.Stage = rtmp.StageDataStart

// Track 2 is non-zero, so it is sent with OneTrack framing.
// Track 0 would use plain enhanced hvc1 messages.
conn.WritePacket(av.Packet{
    Type: av.HEVCDecoderConfig, IsKeyFrame: true, TrackID: 2,
    Data: hevcConfig4Byte, // HEVCDecoderConfigurationRecord, 4-byte NAL width
})
conn.WritePacket(av.Packet{
    Type: av.HEVC, IsKeyFrame: true, TrackID: 2,
    Time: 40 * time.Millisecond, CTime: 0,
    Data: hevcFrame4Byte, // length-prefixed NAL units, width from the config
})

// Configuration switch on the same track: new frames must use the new width.
conn.WritePacket(av.Packet{
    Type: av.HEVCDecoderConfig, IsKeyFrame: true, TrackID: 2,
    Data: hevcConfig2Byte, // now 2-byte NAL width
})
conn.WritePacket(av.Packet{
    Type: av.HEVC, TrackID: 2,
    Time: 120 * time.Millisecond, CTime: -3 * time.Millisecond,
    Data: hevcFrame2Byte,
})

for {
    pkt, err := conn.ReadPacket()
    if err == io.EOF {
        break
    }
    if err != nil {
        // Truncated data, unknown FourCC, unsupported modes, unconfigured
        // tracks and NAL length overruns all return an explicit error;
        // whether to keep reading is the caller's decision.
        log.Fatal(err)
    }
    switch pkt.Type {
    case av.HEVCDecoderConfig:
        // pkt.TrackID identifies the track; pkt.HEVC.NALLengthSize is 1/2/4
    case av.HEVC:
        // pkt.Time is decode time, pkt.CTime is the composition offset,
        // pkt.HEVC is this packet's own configuration snapshot
    case av.HEVCEndOfSequence:
        // pkt.TrackID is now unconfigured until a new SequenceStart arrives
    }
}
```

A runnable version covering FLV mux/demux, RTMP chunking with a chunk size
below the enhanced header length, non-zero tracks and config switching lives
in `examples/enhanced_rtmp`. The multitrack test suite is in
`format/flv/enhanced_video_test.go`.

## Thanks 

 - [joy5](https://github.com/nareix/joy5)  
