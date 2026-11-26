<h1 align="center">
  <br>
  Pion RTCP
  <br>
</h1>
<h4 align="center">A Go implementation of RTCP</h4>
<p align="center">
  <a href="https://pion.ly"><img src="https://img.shields.io/badge/pion-rtcp-gray.svg?longCache=true&colorB=brightgreen" alt="Pion RTCP"></a>
  <a href="https://sourcegraph.com/github.com/pion/rtcp?badge"><img src="https://sourcegraph.com/github.com/pion/rtcp/-/badge.svg" alt="Sourcegraph Widget"></a>
  <a href="https://pion.ly/slack"><img src="https://img.shields.io/badge/join-us%20on%20slack-gray.svg?longCache=true&logo=slack&colorB=brightgreen" alt="Slack Widget"></a>
  <br>
  <img alt="GitHub Workflow Status" src="https://img.shields.io/github/actions/workflow/status/pion/rtcp/test.yaml">
  <a href="https://pkg.go.dev/github.com/pion/rtcp"><img src="https://pkg.go.dev/badge/github.com/pion/rtcp.svg" alt="Go Reference"></a>
  <a href="https://codecov.io/gh/pion/rtcp"><img src="https://codecov.io/gh/pion/rtcp/branch/master/graph/badge.svg" alt="Coverage Status"></a>
  <a href="https://goreportcard.com/report/github.com/pion/rtcp"><img src="https://goreportcard.com/badge/github.com/pion/rtcp" alt="Go Report Card"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
</p>
<br>

See [DESIGN.md](DESIGN.md) for an overview of features and future goals.

### Roadmap
The library is used as a part of our WebRTC implementation. Please refer to that [roadmap](https://github.com/pion/webrtc/issues/9) to track our major milestones.

### Community
Pion has an active community on the [Slack](https://pion.ly/slack).

Follow the [Pion Twitter](https://twitter.com/_pion) for project updates and important WebRTC news.

We are always looking to support **your projects**. Please reach out if you have something to build!
If you need commercial support or don't want to use public methods you can contact us at [team@pion.ly](mailto:team@pion.ly)

### Contributing
Check out the [contributing wiki](https://github.com/pion/webrtc/wiki/Contributing) to join the group of amazing people making this project possible

### License
MIT License - see [LICENSE](LICENSE) for full text

### Transport-wide congestion control feedback

`BuildTransportFeedback` turns an ordered run of arrival observations into one or
more ready-to-send reduced-size RTCP packets (PT=205, FMT=15). The caller supplies
one `TransportFeedbackSample` per transport-wide sequence number; lost packets
still occupy a slot with `Received: false`.

```go
samples := []rtcp.TransportFeedbackSample{
	{Received: true, ArrivalUS: 0},
	{Received: false},
	{Received: true, ArrivalUS: 500},
	{Received: true, ArrivalUS: 1_000_000},
}

packets, err := rtcp.BuildTransportFeedback(
	senderSSRC, // SSRC of the packet sender
	mediaSSRC,  // SSRC of the media source
	100,        // base transport-wide sequence number
	7,          // first feedback packet count
	samples,
	1200,       // maximum bytes per packet, [24, 65532]
)
for _, raw := range packets {
	// raw is a complete, independently marshaled RTCP packet.
}
```

Each returned packet is produced with `TransportLayerCC.Marshal` and is at most
`maxBytes` long. Arrival times must be in `[0, 2^50)` microseconds and a multiple
of 250; they may be out of order. Samples are fragmented greedily into the
longest prefix that fits, with the status chunks chosen for the fewest possible
chunks, so the packet size stays minimal under the budget. The input slice and
its fields are never modified; invalid arrival times, an out-of-range budget, or
too many samples return `nil` and an error before any packet is built.
