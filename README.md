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

### Building TWCC feedback from arrival records

`BuildTransportFeedback` turns an ordered run of transport sequence arrival
records into one or more standalone reduced-size RTCP Transport Layer CC
packets. It takes care of prefix selection, minimal chunk encoding, delta
encoding, header lengths and byte-alignment padding, so callers do not have to
assemble `TransportLayerCC` values themselves.

```go
samples := []rtcp.TransportFeedbackSample{
	{Received: true, ArrivalUS: 64_000*10 + 250},
	{Received: false},                                // this sequence number was lost
	{Received: true, ArrivalUS: 64_000*10 + 2*250},
	{Received: true, ArrivalUS: 64_000*10 + 300*250}, // large delta
}

packets, err := rtcp.BuildTransportFeedback(
	0xAABBCCDD, // sender SSRC
	0x11223344, // media SSRC
	9000,       // base sequence number of samples[0]
	7,          // first feedback packet count
	samples,
	1200,       // maximum bytes per packet, 24..65532
)
if err != nil {
	// invalid arrival time, sample count, or budget
}
for _, packet := range packets {
	var tcc rtcp.TransportLayerCC
	if err := tcc.Unmarshal(packet); err != nil {
		// ...
	}
	// packet is a complete, independently usable reduced-size RTCP packet
}
```

Details:

- `samples[i]` is the transport sequence number `baseSequence + i` (wrapping at
  16 bits). Lost sequence numbers still occupy a slot.
- `ArrivalUS` is ignored when `Received` is false. For received samples it must
  be in `[0, 2^50)` microseconds and a multiple of 250us. Arrival times may go
  backwards between consecutive received samples.
- Each packet takes the longest prefix that fits in `maxBytes`, measured on the
  real marshaled bytes (header, chunks, deltas and 4-byte padding), and encodes
  its statuses with the fewest 2-byte chunks. A delta outside the large-delta
  range closes the packet before that sample; the next packet starts a new
  reference clock. A packet whose prefix contains no received sample uses a
  reference clock of zero.
- `FbPktCount` starts at `firstFeedback` and increments per packet, wrapping at
  8 bits. The input slice and its fields are never modified.
- The existing `TransportLayerCC` `Marshal`/`Unmarshal` API is unchanged, and
  every returned packet can be read back with `TransportLayerCC.Unmarshal`.

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
