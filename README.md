# gohlslib

[![Test](https://github.com/bluenviron/gohlslib/actions/workflows/test.yml/badge.svg?branch=main)](https://github.com/bluenviron/gohlslib/actions/workflows/test.yml?query=branch%3Amain)
[![Lint](https://github.com/bluenviron/gohlslib/actions/workflows/lint.yml/badge.svg?branch=main)](https://github.com/bluenviron/gohlslib/actions/workflows/lint.yml?query=branch%3Amain)
[![Go Report Card](https://goreportcard.com/badge/github.com/bluenviron/gohlslib)](https://goreportcard.com/report/github.com/bluenviron/gohlslib)
[![CodeCov](https://codecov.io/gh/bluenviron/gohlslib/branch/main/graph/badge.svg)](https://app.codecov.io/gh/bluenviron/gohlslib/tree/main)
[![PkgGoDev](https://pkg.go.dev/badge/github.com/bluenviron/gohlslib/v2)](https://pkg.go.dev/github.com/bluenviron/gohlslib/v2#pkg-index)

HLS client and muxer library for the Go programming language, written for [MediaMTX](https://github.com/bluenviron/mediamtx).

Go &ge; 1.25 is required.

Features:

* Client

  * Read streams in MPEG-TS, fMP4 or Low-latency format
  * Read AES-128 encrypted MPEG-TS and fMP4 segments (including encrypted EXT-X-MAP)
  * Read a single video track and/or multiple audio tracks
  * Read tracks encoded with AV1, VP9, H265, H264, Opus, MPEG-4 Audio (AAC)
  * Get absolute timestamp of incoming data

* Muxer

  * Generate streams in MPEG-TS, fMP4 or Low-latency format
  * Write a single video track and/or multiple audio tracks
  * Write tracks encoded with AV1, VP9, H265, H264, Opus, MPEG-4 audio (AAC), KLV
  * Save generated segments on disk

* General

  * Parse and produce M3U8 playlists
  * Examples

## Table of contents

* [Examples](#examples)
* [API Documentation](#api-documentation)
* [Specifications](#specifications)
* [Related projects](#related-projects)

## Examples

* [playlist-parser](examples/playlist-parser/main.go)
* [client](examples/client/main.go)
* [client-aes128](examples/client-aes128/main.go)
* [client-absolute-timestamp](examples/client-absolute-timestamp/main.go)
* [client-codec-h264-save-to-disk](examples/client-codec-h264-save-to-disk/main.go)
* [client-codec-h264-convert-to-jpeg](examples/client-codec-h264-convert-to-jpeg/main.go)
* [client-codec-mpeg4audio-save-to-disk](examples/client-codec-mpeg4audio-save-to-disk/main.go)
* [muxer](examples/muxer/main.go)

## API Documentation

[Click to open the API Documentation](https://pkg.go.dev/github.com/bluenviron/gohlslib#pkg-index)

## Encrypted playback (AES-128)

The client transparently decrypts traditional HLS playlists that use
`EXT-X-KEY` with `METHOD=AES-128`:

```go
c := &gohlslib.Client{
	URI: "https://myserver/mystream/index.m3u8",
}
c.OnTracks = func(tracks []*gohlslib.Track) error {
	for _, track := range tracks {
		switch track.Codec.(type) {
		case *codecs.H264:
			c.OnDataH26x(track, func(pts int64, dts int64, au [][]byte) {
				// au contains decrypted data
			})
		}
	}
	return nil
}
if err := c.Start(); err != nil {
	panic(err)
}
defer c.Close()
panic(c.Wait2())
```

Supported:

* `METHOD=AES-128` with `KEYFORMAT` missing or `identity` and
  `KEYFORMATVERSIONS` missing or `1`.
* Encrypted MPEG-TS and fMP4 segments. For fMP4, an encrypted `EXT-X-MAP`
  is supported too (it must carry an explicit `IV`).
* Explicit `IV` values (`0x` followed by 1 to 32 hex digits) and implicit IVs
  computed from `EXT-X-MEDIA-SEQUENCE` plus the segment position, including
  when joining a live stream mid-window or after the playlist has slid.
* Key rotation between `AES-128` keys and `METHOD=NONE`, including within the
  same playlist.
* Key URIs are resolved relative to the media playlist URL; key requests use
  the client's `HTTPClient`, request context and `OnRequest` callback, so
  custom authentication headers and cancellation work as usual.
* Byte-range requests on unencrypted segments keep their original behavior.
  Encrypted objects are required to be returned whole by the server.

Not supported: low-latency parts, `SAMPLE-AES`, DRM key formats, MAP changes
during playback and discontinuous timelines. Unsupported methods, formats or
versions are reported as errors instead of being played as encrypted media.

## Specifications

|name|area|
|----|----|
|[RFC2616, HTTP 1.1](https://datatracker.ietf.org/doc/html/rfc2616)|protocol|
|[RFC8216, HLS](https://datatracker.ietf.org/doc/html/rfc8216)|protocol|
|[HLS v2](https://datatracker.ietf.org/doc/html/draft-pantos-hls-rfc8216bis)|protocol|
|[HTTP Live Streaming by Apple](https://developer.apple.com/documentation/http-live-streaming)|protocol|
|[Codec specifications](https://github.com/bluenviron/mediacommon#specifications)|codecs|
|[Golang project layout](https://github.com/golang-standards/project-layout)|project layout|

## Related projects

* [MediaMTX](https://github.com/bluenviron/mediamtx)
* [gortsplib](https://github.com/bluenviron/gortsplib)
* [gortmplib](https://github.com/bluenviron/gortmplib)
* [mediacommon](https://github.com/bluenviron/mediacommon)
