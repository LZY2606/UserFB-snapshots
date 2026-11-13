[![Test](https://github.com/nlnwa/gowarc/actions/workflows/test.yml/badge.svg)](https://github.com/nlnwa/gowarc/actions/workflows/test.yml)
[![Release](https://img.shields.io/github/release/nlnwa/gowarc.svg)](https://github.com/nlnwa/gowarc/releases/latest)
[![License](https://img.shields.io/github/license/nlnwa/gowarc)](/LICENSE)
[![PkgGoDev](https://pkg.go.dev/badge/github.com/nlnwa/gowarc/v3.svg)](https://pkg.go.dev/github.com/nlnwa/gowarc/v3)

# gowarc

A library for creating, parsing and evaluating WARC-files, written in go.

### What is WARC?

The WARC format offers a standard way to structure, manage and store billions of resources collected from the web and
elsewhere. It is used to build applications for harvesting, managing, accessing, mining and exchanging content.

To learn more about the WARC standard, read the specification
at https://iipc.github.io/warc-specifications/specifications/warc-format/warc-1.1/

## Library documentation

#### Installation

```
$ go get github.com/nlnwa/gowarc/v3
```

#### Create a new WARC record

To get you started, here is a simple example of how to create a new WARC record.

```go
package main

import (
	"fmt"
	"github.com/nlnwa/gowarc/v3"
)

func main() {
	builder := gowarc.NewRecordBuilder(gowarc.Response)
	_, err := builder.WriteString("HTTP/1.1 200 OK\nDate: Tue, 19 Sep 2016 17:18:40 GMT\nServer: Apache/2.0.54 (Ubuntu)\n" +
		"Last-Modified: Mon, 16 Jun 2013 22:28:51 GMT\nETag: \"3e45-67e-2ed02ec0\"\nAccept-Ranges: bytes\n" +
		"Content-Length: 19\nConnection: close\nContent-Type: text/plain\n\nThis is the content")
	if err != nil {
		panic(err)
	}
	builder.AddWarcHeader(gowarc.WarcRecordID, "<urn:uuid:e9a0cecc-0221-11e7-adb1-0242ac120008>")
	builder.AddWarcHeader(gowarc.WarcDate, "2006-01-02T15:04:05Z")
	builder.AddWarcHeader(gowarc.ContentLength, "257")
	builder.AddWarcHeader(gowarc.ContentType, "application/http;msgtype=response")
	builder.AddWarcHeader(gowarc.WarcBlockDigest, "sha1:B285747AD7CC57AA74BCE2E30B453C8D1CB71BA4")

	if wr, v, err := builder.Build(); err == nil {
		fmt.Println(wr, v)
	}
}
```

### godoc

For complete documentation and examples consult the godoc online at: https://pkg.go.dev/github.com/nlnwa/gowarc/v3

### Segmenting large records

Captured HTTP responses can be larger than a single WARC file should hold. The
marshaler implements [WARC 1.1 record segmentation](https://iipc.github.io/warc-specifications/specifications/warc-format/warc-1.1/#record-segmentation)
for cached `response` and `resource` records (and for the continuation records
they produce).

`Marshal` takes a positive `maxSize` measured in complete, uncompressed bytes of
one physical WARC record: the version line, header fields, separators and the
content block all count towards it. When the record does not fit, the first
segment is written and a continuation record is returned. Pass it back to
`Marshal` (optionally with a different `maxSize`) and repeat until `nil` is
returned. A `maxSize` of zero or less writes the whole record unchanged, and a
record that fits is always written without any segmentation fields.

```go
var w io.Writer
record := /* a cached gowarc.Response or gowarc.Resource record */

marshaler := gowarc.NewMarshaler()
current := record
for current != nil {
    continuation, _, err := marshaler.Marshal(w, current, 1<<20) // 1 MiB per physical record
    if err != nil {
        panic(err)
    }
    if current != record {
        _ = current.Close() // continuations own their own buffered suffix
    }
    current = continuation
}
```

Segment semantics follow WARC 1.1:

- The first segment keeps the original type, record id, date, target and
  business fields and gets `WARC-Segment-Number: 1`.
- Every continuation has its own record id, the same target and date, a
  `WARC-Segment-Origin-ID` pointing at the first segment and an increasing
  segment number.
- Each segment's `Content-Length` covers only its own block and its
  `WARC-Block-Digest` is a SHA-256 base16 digest of that segment's real bytes.
- The original `WARC-Payload-Digest` stays on the first segment, where it keeps
  describing the complete logical response; it is never copied to continuations.
- Only the final segment carries `WARC-Segment-Total-Length`.
- The raw block bytes are cut contiguously; the HTTP header is written once, in
  the first segment, and is neither rewritten nor repeated.
- If even one content byte plus the segment framing cannot fit, `Marshal`
  returns `gowarc.ErrSegmentTooSmall` after writing nothing, so no empty
  continuation and no infinite loop is possible.

To have the file writer consume the whole chain automatically, rotating to new
files as needed, enable `WithSegmentation` together with `WithMaxFileSize`.
Records that fit into a new file are only rotated, never split; otherwise the
first segment starts in the new file and the remaining segments go into
subsequent files. Each physical record is still an independent gzip member when
compression is enabled.

```go
writer := gowarc.NewWarcFileWriter(
    gowarc.WithMaxFileSize(100<<20), // 100 MiB per WARC file
    gowarc.WithSegmentation(),
)
defer writer.Close()

responses := writer.Write(record)
```

Reassembling segmented records on read is not part of this feature; use the
segment headers (`WARC-Segment-Number`, `WARC-Segment-Origin-ID` and
`WARC-Segment-Total-Length`) to locate and join the parts.


## Command line tools

[warchaeology](https://github.com/nationallibraryofnorway/warchaeology) is a command line tool based on gowarc.
