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
* [client-absolute-timestamp](examples/client-absolute-timestamp/main.go)
* [client-codec-h264-save-to-disk](examples/client-codec-h264-save-to-disk/main.go)
* [client-codec-h264-convert-to-jpeg](examples/client-codec-h264-convert-to-jpeg/main.go)
* [client-codec-mpeg4audio-save-to-disk](examples/client-codec-mpeg4audio-save-to-disk/main.go)
* [muxer](examples/muxer/main.go)

### Encrypted HLS (AES-128)

The same `Client` entry point plays traditional AES-128 encrypted MPEG-TS and fMP4 playlists. `EXT-X-KEY:METHOD=AES-128` keys are fetched with the configured `HTTPClient`, request context and `OnRequest` callback; segments and fMP4 initialization sections are decrypted before track discovery, timestamp conversion or data callbacks run.

Supported scope:

* complete MPEG-TS segments and complete fMP4 segments, including `EXT-X-MAP`;
* `KEYFORMAT` absent or `identity`, and `KEYFORMATVERSIONS` absent or `1`;
* explicit `IV=0x...`, or the segment's real media sequence number when `IV` is absent;
* A/B key URI rotation and `METHOD=NONE`;
* at most one `EXT-X-MAP` per media playlist.

Low-latency parts, `SAMPLE-AES`, DRM/non-identity key formats, runtime MAP changes and discontinuous timelines are outside this change. Unsupported encryption methods, key formats and versions return an error instead of passing ciphertext to the MPEG-TS or fMP4 processors.

```go
package main

import (
	"log"
	"net/http"

	"github.com/bluenviron/gohlslib/v2"
	"github.com/bluenviron/gohlslib/v2/pkg/codecs"
)

func main() {
	client := &gohlslib.Client{
		URI: "https://example.com/live/index.m3u8",
		OnRequest: func(req *http.Request) {
			req.Header.Set("Authorization", "Bearer token")
		},
	}

	client.OnTracks = func(tracks []*gohlslib.Track) error {
		for _, track := range tracks {
			if _, ok := track.Codec.(*codecs.H264); ok {
				client.OnDataH26x(track, func(pts, dts int64, au [][]byte) {
					log.Printf("H264 access unit: pts=%d dts=%d nalus=%d", pts, dts, len(au))
				})
			}
		}
		return nil
	}

	if err := client.Start(); err != nil {
		panic(err)
	}
	defer client.Close()

	if err := client.Wait2(); err != nil {
		log.Printf("client stopped: %v", err)
	}
}
```

## API Documentation

[Click to open the API Documentation](https://pkg.go.dev/github.com/bluenviron/gohlslib#pkg-index)

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
