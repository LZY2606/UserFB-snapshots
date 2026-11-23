# rtmp
golang rtmp lib

## Timed playback subscriptions

`pubsub.AddSub` writes all cached GOP packets as fast as possible. For local
replay of an RTMP live cache, `AddSubTimed` paces packets by media time instead.

```go
s := &pubsub.PubSub{}
go s.SetPub(reader) // av.PacketReader

closeCh := make(chan bool)
err := s.AddSubTimed(closeCh, writer, pubsub.PlaybackOptions{
    MaxLag: 500 * time.Millisecond, // required, must be > 0
    Clock:  nil,                    // nil means the real clock
})
```

`AddSubTimed` starts at the newest cached keyframe (audio before it is not
written). The first video packet is emitted with `Time == 0`, and later packets
are scheduled from their DTS deltas, with `CTime` and payloads unchanged.
Metadata and sequence headers are written immediately before their media
packet with the same `Time`.

If the next media packet is more than `MaxLag` late, stale media is dropped and
playback restarts from the newest cached keyframe that this subscriber has not
yet received. A publisher change restarts playback the same way, re-emitting
the new stream's configuration even when its bytes are identical. Each
subscriber keeps its own cursor and anchor; notifications are only hints, so
duplicate notifications and stale timer wakes never replay a packet. Closing
`closeCh` ends the subscription immediately.

`PlaybackClock` (`Now() time.Time`, `NewTimer(time.Duration) PlaybackTimer`)
can be implemented in memory to drive playback deterministically in tests.

## Thanks 

 - [joy5](https://github.com/nareix/joy5)  
