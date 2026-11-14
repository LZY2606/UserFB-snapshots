# ebml-go

[![GoDoc](https://godoc.org/github.com/at-wat/ebml-go?status.svg)](http://godoc.org/github.com/at-wat/ebml-go) [![ci](https://github.com/at-wat/ebml-go/actions/workflows/ci.yml/badge.svg)](https://github.com/at-wat/ebml-go/actions/workflows/ci.yml) [![codecov](https://codecov.io/gh/at-wat/ebml-go/branch/master/graph/badge.svg)](https://codecov.io/gh/at-wat/ebml-go) [![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

## A pure Go implementation of bi-directional EBML encoder/decoder

EBML (Extensible Binary Meta Language) is a binary and byte-aligned format that was originally developed for the Matroska audio-visual container.
See https://matroska.org/ for details.

This package implements EBML Marshaler and Unmarshaler for Go.
Currently, commonly used elements of WebM subset is supported.


## Usage

Check out the examples placed under [./examples](./examples/) directory.

API is documented using [GoDoc](http://godoc.org/github.com/at-wat/ebml-go).
EBML can be `Marshal`-ed and `Unmarshal`-ed between tagged struct and binary stream through `io.Reader` and `io.Writer`.

### Timed block reader

`mkvcore.NewTimedBlockReader` exposes one reader per Matroska track and carries
the container clock (TimecodeScale, cluster/block timecodes, CodecDelay) and
BlockGroup DiscardPadding down to every compressed frame. All timestamps are in
nanoseconds, frames are not decoded, and a nil `DurationNS` means the duration
is genuinely unknown rather than zero.

```go
package main

import (
	"fmt"
	"io"
	"os"

	"github.com/at-wat/ebml-go/mkvcore"
)

func main() {
	f, err := os.Open("audio.mka")
	if err != nil {
		panic(err)
	}
	defer f.Close()

	readers, err := mkvcore.NewTimedBlockReader(f)
	if err != nil {
		panic(err)
	}
	r := readers[0]
	defer r.Close()

	for {
		frame, err := r.ReadFrame()
		if err == io.EOF {
			break
		}
		if err != nil {
			// Parse errors and clock overflows are delivered here instead of
			// being silently turned into EOF.
			panic(err)
		}

		duration := int64(-1)
		if frame.DurationNS != nil {
			duration = *frame.DurationNS
		}
		fmt.Printf("%d bytes @ %dns, duration %dns, present starts at %dns\n",
			len(frame.Data), frame.TimestampNS, duration, frame.PresentStartNS)
	}
}
```


## References

- [Matroska Container Specifications](https://matroska.org/technical/specs/index.html)
- [WebM Container Guidelines](https://www.webmproject.org/docs/container/)


## License

This package is licensed under [Apache License Version 2.0](./LICENSE).
