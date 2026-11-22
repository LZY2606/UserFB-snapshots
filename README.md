<h1 align="center">
  <br>
  Pion Interceptor
  <br>
</h1>
<h4 align="center">RTP and RTCP processors for building real time communications</h4>
<p align="center">
  <a href="https://pion.ly"><img src="https://img.shields.io/badge/pion-interceptor-gray.svg?longCache=true&colorB=brightgreen" alt="Pion Interceptor"></a>
  <a href="https://discord.gg/PngbdqpFbt"><img src="https://img.shields.io/badge/join-us%20on%20discord-gray.svg?longCache=true&logo=discord&colorB=brightblue" alt="join us on Discord"></a> <a href="https://bsky.app/profile/pion.ly"><img src="https://img.shields.io/badge/follow-us%20on%20bluesky-gray.svg?longCache=true&logo=bluesky&colorB=brightblue" alt="Follow us on Bluesky"></a>
  <br>
  <img alt="GitHub Workflow Status" src="https://img.shields.io/github/actions/workflow/status/pion/interceptor/test.yaml">
  <a href="https://pkg.go.dev/github.com/pion/interceptor"><img src="https://pkg.go.dev/badge/github.com/pion/interceptor.svg" alt="Go Reference"></a>
  <a href="https://codecov.io/gh/pion/interceptor"><img src="https://codecov.io/gh/pion/interceptor/branch/master/graph/badge.svg" alt="Coverage Status"></a>
  <a href="https://goreportcard.com/report/github.com/pion/interceptor"><img src="https://goreportcard.com/badge/github.com/pion/interceptor" alt="Go Report Card"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
</p>
<br>

Interceptor is a framework for building RTP/RTCP communication software. This framework defines
a interface that each interceptor must satisfy. These interceptors are then run sequentially. We
also then provide common interceptors that will be useful for building RTC software.

This package was built for [pion/webrtc](https://github.com/pion/webrtc), but we designed it to be consumable
by anyone. With the following tenets in mind.

* Useful defaults. Each interceptor will be configured to give you a good default experience.
* Unblock unique use cases. New use cases are what is driving WebRTC, we want to empower them.
* Encourage modification. Add your own interceptors without forking. Mixing with the ones we provide.
* Empower learning. This code base should be useful to read and learn even if you aren't using Pion.

### Current Interceptors
* [NACK Generator/Responder](https://github.com/pion/interceptor/tree/master/pkg/nack)
* [Sender and Receiver Reports](https://github.com/pion/interceptor/tree/master/pkg/report)
* [Transport Wide Congestion Control Feedback](https://github.com/pion/interceptor/tree/master/pkg/twcc)
* [RTCP Feedback for Congestion Control](https://github.com/pion/interceptor/tree/master/pkg/rfc8888) as defined by [RFC 8888](https://datatracker.ietf.org/doc/html/rfc8888).
* [JitterBuffer](https://github.com/pion/interceptor/tree/master/pkg/jitterbuffer) Re-order packets and wait for arrival on the remote/inbound RTP path.
* [Packet Dump](https://github.com/pion/interceptor/tree/master/pkg/packetdump)
* [Google Congestion Control](https://github.com/pion/interceptor/tree/master/pkg/gcc)
* [Stats](https://github.com/pion/interceptor/tree/master/pkg/stats) A [webrtc-stats](https://www.w3.org/TR/webrtc-stats/) compliant statistics generation
* [Interval PLI](https://github.com/pion/interceptor/tree/master/pkg/intervalpli) Generate PLI on a interval. Useful when no decoder is available.
* [FlexFec](https://github.com/pion/interceptor/tree/master/pkg/flexfec) – [FlexFEC-03](https://datatracker.ietf.org/doc/html/draft-ietf-payload-flexible-fec-scheme-03) encoder implementation

### Planned Interceptors
* Bandwidth Estimation
  - [NADA](https://tools.ietf.org/html/rfc8698)

### Interceptor Public API
The public interface is defined in [interceptor.go](https://github.com/pion/interceptor/blob/master/interceptor.go).
The methods you need to satisy are broken up into 4 groups.

* `BindRTCPWriter` and `BindRTCPReader` allow you to inspect/modify RTCP traffic.
* `BindLocalStream` and `BindRemoteStream` notify you of a new SSRC stream and allow you to inspect/modify.
* `UnbindLocalStream` and `UnbindRemoteStream` notify you when a SSRC stream has been removed
* `Close` called when the interceptor is closed.

Interceptors also pass Attributes between each other. These are a collection of key/value pairs and are useful for storing metadata
or caching.

[noop.go](https://github.com/pion/interceptor/blob/master/noop.go) is an interceptor that satisfies this interface, but does nothing.
You can embed this interceptor as a starting point so you only need to define exactly what you need.

[chain.go]( https://github.com/pion/interceptor/blob/master/chain.go) is used to combine multiple interceptors into one. They are called
sequentially as the packet moves through them.

### One-shot padding probe clusters (GCC)
`gcc.SendSideBWE` can run a one-shot padding probe cluster when the audio/video
bitrate itself is too low to produce a dense enough packet train for a real
bandwidth sample. The probe packets are RTP padding-only packets that share the
media streams' `twcc.HeaderExtensionInterceptor` (so transport-wide sequence
numbers come from one space) and are measured through the regular TWCC feedback
path. The result is exposed via `GetStats`/`LatestProbeResult` and does **not**
change the GCC target bitrate by itself.

```go
pacer := gcc.NewLeakyBucketPacer(1_000_000)
bwe, _ := gcc.NewSendSideBWE(gcc.SendSideBWEPacer(pacer))
hdrExt := &twcc.HeaderExtensionInterceptor{}

// Locally synthesized audio/video: bind each media stream as usual, sharing
// the one HeaderExtensionInterceptor with the probe stream.
mediaInfo := &interceptor.StreamInfo{
    SSRC: 100, PayloadType: 111, ClockRate: 48000,
    RTPHeaderExtensions: []interceptor.RTPHeaderExtension{
        {URI: "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01", ID: 1},
    },
}
mediaWriter := hdrExt.BindLocalStream(mediaInfo, bwe.AddStream(mediaInfo, rtpSink))
_ = mediaWriter // write synthesized RTP here

// The padding stream must be negotiated with the peer beforehand: it uses an
// application-reserved SSRC/payload type that the receiver must not expect to
// carry decodable media. ClockRate is the RTP clock rate in Hz (ticks per
// second, e.g. 90000 for video, 48000 for Opus audio); probe RTP timestamps
// are derived from it and the RTPTimeBase/LocalTimeBase pair.
err := bwe.EnableProbeStream(gcc.ProbeStreamConfig{
    SSRC: 4001, PayloadType: 127, ClockRate: 90000,
    InitialSequenceNumber: 1,
    RTPTimeBase:           0,
    LocalTimeBase:         time.Now(),
    TWCCExtensionID:       1,
    HeaderExtension:       hdrExt,
    Writer:                rtpSink,
})
if err != nil {
    panic(err)
}

// One cluster at a time: target 2 Mbit/s, 200 padding bytes per packet
// (220 bytes on the wire), 32 packets, 2s timeout.
clusterID, err := bwe.StartProbe(gcc.ProbeConfig{
    TargetBitrate: 2_000_000,
    PaddingBytes:  200,
    Packets:       32,
    Timeout:       2 * time.Second,
})
if err != nil {
    panic(err)
}
_ = clusterID

// Later, after the cluster succeeded, was canceled or timed out:
if result, ok := bwe.LatestProbeResult(); ok {
    fmt.Printf("probe cluster %d: %s, %d/%d packets received, %d bit/s\n",
        result.ClusterID, result.Status, result.Received, result.Sent, result.Bitrate)
}
```

### Examples
The [examples](https://github.com/pion/interceptor/blob/master/examples) directory provides some basic examples. If you need more please file an issue!
You should also look in [pion/webrtc](https://github.com/pion/webrtc) for real world examples.

### Roadmap
The library is used as a part of our WebRTC implementation. Please refer to that [roadmap](https://github.com/pion/webrtc/issues/9) to track our major milestones.

### Community
Pion has an active community on the [Discord](https://discord.gg/PngbdqpFbt).

Follow the [Pion Bluesky](https://bsky.app/profile/pion.ly) or [Pion Twitter](https://twitter.com/_pion) for project updates and important WebRTC news.

We are always looking to support **your projects**. Please reach out if you have something to build!
If you need commercial support or don't want to use public methods you can contact us at [team@pion.ly](mailto:team@pion.ly)

### Contributing
Check out the [contributing wiki](https://github.com/pion/webrtc/wiki/Contributing) to join the group of amazing people making this project possible

### License
MIT License - see [LICENSE](LICENSE) for full text
