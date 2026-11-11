<h1 align="center">
  <br>
  Pion Opus
  <br>
</h1>
<h4 align="center">Pure Go implementation of the Opus Codec</h4>
<p align="center">
  <a href="https://pion.ly"><img src="https://img.shields.io/badge/pion-opus-gray.svg?longCache=true&colorB=brightgreen" alt="Opus"></a>
  <a href="https://discord.gg/PngbdqpFbt"><img src="https://img.shields.io/badge/join-us%20on%20discord-gray.svg?longCache=true&logo=discord&colorB=brightblue" alt="join us on Discord"></a> <a href="https://bsky.app/profile/pion.ly"><img src="https://img.shields.io/badge/follow-us%20on%20bluesky-gray.svg?longCache=true&logo=bluesky&colorB=brightblue" alt="Follow us on Bluesky"></a>
  <br>
  <img alt="GitHub Workflow Status" src="https://img.shields.io/github/actions/workflow/status/pion/opus/test.yaml">
  <a href="https://pkg.go.dev/github.com/pion/opus"><img src="https://pkg.go.dev/badge/github.com/pion/opus.svg" alt="Go Reference"></a>
  <a href="https://codecov.io/gh/pion/opus"><img src="https://codecov.io/gh/pion/opus/branch/master/graph/badge.svg" alt="Coverage Status"></a>
  <a href="https://goreportcard.com/report/github.com/pion/opus"><img src="https://goreportcard.com/badge/github.com/pion/opus" alt="Go Report Card"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
</p>
<br>

This package provides a Pure Go implementation of the [Opus Codec](https://opus-codec.org/)

### Why Opus?
* **open and royalty-free** - No license fees or restrictions. Use it as you wish!
* **versatile** - Wide bitrate support. Can be used in constrained networks and high quality stereo.
* **ubiquitous** - Used in video streaming, gaming, storing music and video conferencing.

### Why a Go implementation?
* **empower interesting use cases** - This project also exports the internals of the Encoder and Decoder.
                                      Allowing for things like analysis of a Opus bitstream without decoding the entire thing.
* **learning** - This project was written to be read by others. It includes excerpts and links to [RFC 6716][rfc6716]
* **safety** - Go provides memory safety. Avoids a class of bugs that are devastating in sensitive environments.
* **maintainability** - Go was designed to build simple, reliable, and efficient software.
* **inspire** - Go is a power language, but lacking in media libraries. We hope this project inspires the next generation to build
                more media libraries for Go.

You can read more [here](https://pion.ly/blog/pion-opus/)

### RFCs
#### Implemented
- **RFC 6716**: [Definition of the Opus Audio Codec][rfc6716]

[rfc6716]: https://tools.ietf.org/html/rfc6716

### Running
See our [examples](examples) for demonstrations of how to use this package.

### Planning a trimmed export
`PlanTrim` works out which complete, decode-ordered Opus packets a fresh
48 kHz decoder must be fed to export an audible interval, without decoding
or re-encoding anything. All times are 48 kHz *per-channel* sample frames:
the unit is unchanged by the decoder's output rate and by whether a packet
is mono or stereo.

```go
// packets are raw Opus packets, without OpusHead/OpusTags, container pages
// or RTP headers. preSkip and endTrim come from the container, also in
// 48 kHz per-channel frames.
plan, err := opus.PlanTrim(packets, preSkip, endTrim, start, end)
if err != nil {
    log.Fatal(err)
}

const channels = 2 // 48 kHz stereo output
decoder, err := opus.NewDecoderWithOutput(48000, channels)
if err != nil {
    log.Fatal(err)
}

// A 120 ms packet is at most 5760 per-channel frames.
out := make([]float32, 5760*channels)
for _, packet := range packets[plan.FirstPacket:plan.EndPacket] {
    if _, _, err := decoder.DecodeFloat32(packet, out); err != nil {
        log.Fatal(err)
    }
}
// out holds the decoded selected packets. Skip plan.Discard frames per
// channel, then keep plan.Keep frames per channel.
```

To turn per-channel frame counts into indices into an interleaved PCM
buffer, multiply by the channel count. For stereo interleaved output
(`[L0, R0, L1, R1, ...]`), frame `f` starts at index `f*2`:

```go
const channels = 2 // stereo interleaved output

firstInterleavedSample := plan.Discard * channels // first exported frame
exportLength           := plan.Keep * channels    // exported sample count
```

In general, for `channels` channels, per-channel frame `f` and channel `c`
live at interleaved index `f*channels + c`, so the export is the slice
`decoded[plan.Discard*channels : (plan.Discard+plan.Keep)*channels]`.

`PlanTrim` keeps a fixed 80 ms preroll (3840 frames) before the target
start when the stream contains that much audio, so the result is not
promised to be sample-identical to decoding the whole file from the
beginning.

### Roadmap
The library is used as a part of our WebRTC implementation. Please refer to that [roadmap](https://github.com/pion/webrtc/issues/9) to track our major milestones.

See also [Issue 9](https://github.com/pion/opus/issues/9)

### Community
Pion has an active community on the [Discord](https://discord.gg/PngbdqpFbt).

Follow the [Pion Bluesky](https://bsky.app/profile/pion.ly) or [Pion Twitter](https://twitter.com/_pion) for project updates and important WebRTC news.

We are always looking to support **your projects**. Please reach out if you have something to build!
If you need commercial support or don't want to use public methods you can contact us at [team@pion.ly](mailto:team@pion.ly)

### Contributing
Check out the [contributing wiki](https://github.com/pion/webrtc/wiki/Contributing) to join the group of amazing people making this project possible

### License
MIT License - see [LICENSE](LICENSE) for full text