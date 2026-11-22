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

### Exporting Opus to Ogg

`ExportOpusOgg` remuxes one Opus audio track of an MP4 into an Ogg Opus stream ([RFC 7845](https://www.rfc-editor.org/rfc/rfc7845.html)).
The compressed Opus packets are copied verbatim — no decoding, re-encoding or payload rewrite — and the audible start/end samples do not move.

```go
in, err := os.Open("input.mp4")
if err != nil { /* ... */ }
defer in.Close()

out, err := os.Create("output.ogg")
if err != nil { /* ... */ }
defer out.Close()

// trackID identifies the Opus track; serial is the Ogg stream serial number.
if err := mp4.ExportOpusOgg(in, out, trackID, serial); err != nil {
	// All failures are wrapped in mp4.ErrOpusExport except read/write errors
	// raised while copying packets, which are returned unchanged.
}
```

Behavior and contracts:

- Parsing always starts at the beginning of the `io.ReadSeeker`, and the reader's original position is restored in every case, success or failure.
- Every validation check finishes before the first byte is written, so rejected input never yields a partial Ogg stream. Read and write errors during the copy phase are propagated as-is; already-written pages are not rolled back.
- The layout is deterministic: OpusHead alone on the BOS page, one OpusTags page (vendor `go-mp4`, zero comments), then one page per audio packet. Page numbers start at 0, the serial is the argument verbatim, the two header pages use granule 0, and only the final audio page carries EOS. All page CRCs are computed per the Ogg framing specification.
- `OpusHead.pre_skip` is the edit start `S`. In MP4 `dOps.PreSkip` is informative and is not added to `S`. The input sample rate and output gain are copied into the little-endian OpusHead fields unchanged.
- Audio page granules are the cumulative original 48 kHz decode positions; only the EOS page is trimmed to the edit end `E`, so the audible length is exactly `E-S`. Packets from the first one up to the packet covering `E` are kept (pre-roll history included); whole packets at or after `E` are dropped.

Supported input boundaries:

- Non-fragmented, self-contained, unencrypted plain MP4. Other tracks and multiple `mdat` boxes are allowed; their bytes and inter-chunk padding are never copied.
- Exactly one Opus sample description: `dOps.version=0`, channel mapping family 0, mono or stereo, sample entry channel count matching `dOps`, 48000 Hz.
- Track `mdhd` timescale 48000; exactly one rate-1 edit with non-negative `media_time` and a positive `segment_duration` that converts to 48 kHz frames with exact integer arithmetic (no floating-point rounding). The movie timescale may differ.
- `stsc` may vary the samples-per-chunk count; `stsz` may be uniform or per-sample; chunk offsets may use `stco` or `co64`. Decode order follows the chunk tables and is never reordered by physical file addresses.
- Packet sizes are limited to 1..65024 bytes (a length divisible by 255 still gets the zero terminating lacing value). Each packet's TOC duration is computed per [RFC 6716](https://www.rfc-editor.org/rfc/rfc6716.html) and independently checked against `stts`; only the final source packet may carry a positive, shorter effective duration. Packet length and a fixed 20 ms assumption are never used as durations.
- Rejected explicitly: fragmented or encrypted files, `ctts`, multiple sample descriptions, non-local data references, corrupted tables or byte ranges, unsupported channel mappings, and integer overflow.

A runnable Go call example is provided as a small CLI:

```sh
go run ./cmd/opusexport -i input.mp4 -o output.ogg -track 1 -serial 123456
```

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
