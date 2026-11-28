go-mp4
------

[![Go Reference](https://pkg.go.dev/badge/github.com/abema/go-mp4.svg)](https://pkg.go.dev/github.com/abema/go-mp4)
![Test](https://github.com/abema/go-mp4/actions/workflows/test.yml/badge.svg)
[![Coverage Status](https://coveralls.io/repos/github/abema/go-mp4/badge.svg)](https://coveralls.io/github/abema/go-mp4)

go-mp4 is a Go library that provides low-level I/O interfaces for MP4 files.
It allows you to parse or build any MP4 box (atom) directly.

go-mp4 provides very flexible interfaces for reading boxes.
If you want to read only specific parts of an MP4 file, this library can extract just those boxes via the io.ReadSeeker interface.

On the other hand, this library is not suitable for complex data conversions.

## Integration with your Go application

### Reading

Using [ReadBoxStructure](https://pkg.go.dev/github.com/abema/go-mp4#ReadBoxStructure) or [ReadBoxStructureFromInternal](https://pkg.go.dev/github.com/abema/go-mp4#ReadBoxStructureFromInternal), you can scan box(atom) tree by depth-first order.

```go
// expand all boxes
_, err := mp4.ReadBoxStructure(file, func(h *mp4.ReadHandle) (interface{}, error) {
	fmt.Println("depth", len(h.Path))

	// Box Type (e.g. "mdhd", "tfdt", "mdat")
	fmt.Println("type", h.BoxInfo.Type.String())

	// Box Size
	fmt.Println("size", h.BoxInfo.Size)

	if h.BoxInfo.IsSupportedType() {
		// Payload
		box, _, err := h.ReadPayload()
		if err != nil {
			return nil, err
		}
		str, err := mp4.Stringify(box, h.BoxInfo.Context)
		if err != nil {
			return nil, err
		}
		fmt.Println("payload", str)

		// Expands children
		return h.Expand()
	}
	return nil, nil
})
```

[ExtractBox](https://pkg.go.dev/github.com/abema/go-mp4#ExtractBox), [ExtractBoxes](https://pkg.go.dev/github.com/abema/go-mp4#ExtractBoxes), [ExtractBoxWithPayload](https://pkg.go.dev/github.com/abema/go-mp4#ExtractBoxWithPayload), [ExtractBoxesWithPayload](https://pkg.go.dev/github.com/abema/go-mp4#ExtractBoxesWithPayload), and [Probe](https://pkg.go.dev/github.com/abema/go-mp4#Probe) are wrapper functions of ReadBoxStructure.

```go
// extract specific boxes
boxes, err := mp4.ExtractBoxWithPayload(file, nil, mp4.BoxPath{mp4.BoxTypeMoov(), mp4.BoxTypeTrak(), mp4.BoxTypeTkhd()})
if err != nil {
   :
}
for _, box := range boxes {
  tkhd := box.Payload.(*mp4.Tkhd)
  fmt.Println("track ID:", tkhd.TrackID)
}
```

```go
// get basic informations
info, err := mp4.Probe(bufseekio.NewReadSeeker(file, 1024, 4))  
if err != nil {
   :
}
fmt.Println("track num:", len(info.Tracks))
```

### Writing

Writer helps you write a box tree.
The following sample code edits an emsg box and writes the result to another file.

```go
r := bufseekio.NewReadSeeker(inputFile, 128*1024, 4)
w := mp4.NewWriter(outputFile)
_, err = mp4.ReadBoxStructure(r, func(h *mp4.ReadHandle) (interface{}, error) {
	switch h.BoxInfo.Type {
	case mp4.BoxTypeEmsg():
		// write box size and box type
		_, err := w.StartBox(&h.BoxInfo)
		if err != nil {
			return nil, err
		}
		// read payload
		box, _, err := h.ReadPayload()
		if err != nil {
			return nil, err
		}
		// update MessageData
		emsg := box.(*mp4.Emsg)
		emsg.MessageData = []byte("hello world")
		// write box playload
		if _, err := mp4.Marshal(w, emsg, h.BoxInfo.Context); err != nil {
			return nil, err
		}
		// rewrite box size
		_, err = w.EndBox()
		return nil, err
	default:
		// copy all
		return nil, w.CopyBox(r, &h.BoxInfo)
	}
})
```

Please note that the sample code above may not work for some MP4 files.
If your MP4 files contain certain box types (e.g., stco, mfra), you need to update them when the offset of the mdat box changes.
The following sample code adds a metadata box and updates the chunk offsets in the stco boxes.

[Sample Code: Insert Metadata and Update stco box](https://gist.github.com/sunfish-shogi/cccde016a38c66d32c07a0234368804e)

### User-defined Boxes

You can define additional box types as follows:

```go
func BoxTypeXxxx() BoxType { return mp4.StrToBoxType("xxxx") }

func init() {
	mp4.AddBoxDef(&Xxxx{}, 0)
}

type Xxxx struct {
	FullBox  `mp4:"0,extend"`
	UI32      uint32 `mp4:"1,size=32"`
	ByteArray []byte `mp4:"2,size=8,len=dynamic"`
}

func (*Xxxx) GetType() BoxType {
	return BoxTypeXxxx()
}
```

### Buffering

go-mp4 does not buffer I/O.
If you need to reduce the number of Read calls, you can wrap your io.ReadSeeker with [bufseekio](https://github.com/sunfish-shogi/bufseekio).

### Exporting an Opus track to Ogg Opus

`ExportOpusOgg` remuxes one Opus audio track of an MP4 file into an Ogg Opus
stream (RFC 7845). The compressed Opus packets are copied byte-for-byte —
neither decoded nor rewritten — so only the container changes. The reader is
parsed from the start of the file and is always seeked back to its original
position before `ExportOpusOgg` returns, on both success and failure.

```go
var input io.ReadSeeker // e.g. *os.File of the MP4
var output io.Writer    // e.g. *os.File for the .ogg

// serial is the Ogg stream serial number written into every page.
err := mp4.ExportOpusOgg(input, output, trackID, serial)
if err != nil {
    log.Fatal(err)
}
```

A runnable command-line example lives under
[`example/exportopusogg`](example/exportopusogg).

The single rate-1 edit list entry defines the audible half-open window
`[S, E)` on the track's 48 kHz media timeline. Packets are kept in decode
order from the first packet up to and including the first packet covering
`E`, so the decoder history needed for the first audible packet is retained;
whole packets starting at or after `E` are dropped. The OpusHead `pre_skip`
field is set to `S` (the MP4 `dOps.PreSkip` value is informative only and is
never added to it), and only the final (EOS) page carries granule position
`E`, so the audible length is exactly `E-S`. Every page is written in a fixed
layout: OpusHead alone on the BOS page, one OpusTags page (vendor `go-mp4`,
no comments), then one packet per page with sequentially increasing page
numbers and correct Ogg CRCs.

Supported input is deliberately narrow:

- non-fragmented, self-contained, unencrypted plain MP4 (no `moof`/`mvex`/
  `styp`, no `sinf`/`saiz`/`saio`/`enca`/`encv`, no `ctts`);
- the named `trackID` exists exactly once and has exactly one Opus sample
  description; `dOps.version` is 0, channel mapping family is 0 with one or
  two channels matching the sample entry, and the sample rate is 48000;
- the track `mdhd` timescale is 48000; the movie timescale may differ. There
  is exactly one edit list entry with rate 1.0, a non-negative `media_time`
  and a positive `segment_duration` that converts to an exact whole number
  of 48 kHz frames; empty edits are rejected;
- `stsc` may vary samples-per-chunk, `stsz` may use uniform or per-sample
  sizes, and chunk offsets may use `stco` or `co64`; multiple `mdat` boxes
  and other tracks are allowed, but every chunk byte span must lie inside a
  single `mdat`.

Each packet's RFC 6716 TOC duration is checked independently against the
`stts` table (the final packet may have a shorter positive effective
duration), packet sizes must be between 1 and 65024 bytes, and all table,
range, window and integer-overflow checks complete before the first byte is
written to the output, so invalid input never produces a partial Ogg stream.
Read or write errors during the copy phase are returned unchanged and pages
already written are not rolled back.

## Command Line Tool

Install mp4tool as follows:

```sh
go install github.com/abema/go-mp4/cmd/mp4tool@latest

mp4tool -help
```

For example, `mp4tool dump MP4_FILE_NAME` command prints MP4 box tree as follows:

```
[moof] Size=504
  [mfhd] Size=16 Version=0 Flags=0x000000 SequenceNumber=1
  [traf] Size=480
    [tfhd] Size=28 Version=0 Flags=0x020038 TrackID=1 DefaultSampleDuration=9000 DefaultSampleSize=33550 DefaultSampleFlags=0x1010000
    [tfdt] Size=20 Version=1 Flags=0x000000 BaseMediaDecodeTimeV1=0
    [trun] Size=424 ... (use -a option to show all)
[mdat] Size=44569 Data=[...] (use -mdat option to expand)
```
