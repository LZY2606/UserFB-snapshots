[![GoReportCard](http://goreportcard.com/badge/github.com/asticode/go-astits)](http://goreportcard.com/report/github.com/asticode/go-astits)
[![GoDoc](https://godoc.org/github.com/asticode/go-astits?status.svg)](https://godoc.org/github.com/asticode/go-astits)
[![Test](https://github.com/asticode/go-astits/actions/workflows/test.yml/badge.svg)](https://github.com/asticode/go-astits/actions/workflows/test.yml)
[![Coveralls](https://coveralls.io/repos/github/asticode/go-astits/badge.svg?branch=master)](https://coveralls.io/github/asticode/go-astits)

This is a Golang library to natively demux and mux MPEG Transport Streams (ts) in GO.

WARNING: this library is not yet production ready. Use at your own risks!

# Installation

To install the library use the following:

    go get -u github.com/asticode/go-astits/...

To install the executables use the following:

    go install github.com/asticode/go-astits/cmd
    
# Before looking at the code...

The transport stream is made of packets.<br>
Each packet has a header, an optional adaptation field and a payload.<br>
Several payloads can be appended and parsed as a data.

```
                                           TRANSPORT STREAM
 +--------------------------------------------------------------------------------------------------+
 |                                                                                                  |
 
                       PACKET                                         PACKET
 +----------------------------------------------+----------------------------------------------+----
 |                                              |                                              |
 
 +--------+---------------------------+---------+--------+---------------------------+---------+
 | HEADER | OPTIONAL ADAPTATION FIELD | PAYLOAD | HEADER | OPTIONAL ADAPTATION FIELD | PAYLOAD | ...
 +--------+---------------------------+---------+--------+---------------------------+---------+
 
                                      |         |                                    |         |
                                      +---------+                                    +---------+
                                           |                                              |
                                           +----------------------------------------------+
                                                                DATA
```
    
# Using the library in your code

WARNING: the code below doesn't handle errors for readability purposes. However you SHOULD!

## Demux

```go
// Create a cancellable context in case you want to stop reading packets/data any time you want
ctx, cancel := context.WithCancel(context.Background())

// Handle SIGTERM signal
ch := make(chan os.Signal, 1)
signal.Notify(ch, syscall.SIGTERM)
go func() {
    <-ch
    cancel()
}()

// Open your file or initialize any kind of io.Reader
// Buffering using bufio.Reader is recommended for performance
f, _ := os.Open("/path/to/file.ts")
defer f.Close()

// Create the demuxer
dmx := astits.NewDemuxer(ctx, f)
for {
    // Get the next data
    d, _ := dmx.NextData()
    
    // Data is a PMT data
    if d.PMT != nil {
        // Loop through elementary streams
        for _, es := range d.PMT.ElementaryStreams {
                fmt.Printf("Stream detected: %d\n", es.ElementaryPID)
        }
        return
    }
}
```

## Reading SCTE-35 cue messages

PIDs declared with `stream_type` `0x86` (SCTE-35) in the PMT are demuxed as
`splice_info_section` messages and delivered through the same `NextData()`
stream as PAT, PMT and PES. Cue messages can span several TS packets and
several sections may share one payload unit start; each complete section is
delivered as soon as its bytes and CRC_32 are available. A bad length or CRC
returns an error (including `ErrCRC32Mismatch`) without producing a half
object, and `NextData()` keeps returning the following valid PES or cue data.

Only `protocol_version` 0, unencrypted sections with an explicit
`splice_command_length` are parsed; encrypted sections, other protocol
versions and the reserved length `0xFFF` return an explicit unsupported
error.

```go
dmx := astits.NewDemuxer(ctx, f)
for {
    d, err := dmx.NextData()
    if err == astits.ErrNoMorePackets {
        break
    } else if err != nil {
        // A corrupt cue section surfaces here; the loop can continue afterwards.
        log.Fatal(err)
    }

    // Data is an SCTE-35 splice info section
    if d.SCTE35 == nil {
        continue
    }
    fmt.Printf("SCTE-35 on PID %d, pts_adjustment=%d\n", d.PID, d.SCTE35.PTSAdjustment)

    cmd := d.SCTE35.Command
    switch cmd.Type {
    case astits.SCTE35SpliceCommandTypeInsert:
        in := cmd.Insert
        fmt.Printf("splice_insert event %d out_of_network=%t canceled=%t\n",
            in.EventID, in.OutOfNetwork, in.Canceled)
        // A nil SpliceTime means the time was not specified; it is never zeroed.
        if in.SpliceTime != nil {
            fmt.Printf("  pts=%d adjusted_pts=%d\n",
                in.SpliceTime.PTS, in.SpliceTime.AdjustedPTS)
        }
        for _, c := range in.Components {
            if c.SpliceTime != nil {
                fmt.Printf("  component %d pts=%d\n", c.Tag, c.SpliceTime.PTS)
            }
        }
        if in.Duration != nil {
            // Break duration is a length of time: pts_adjustment is not applied.
            fmt.Printf("  break duration=%d auto_return=%t\n",
                in.Duration.Duration, in.Duration.AutoReturn)
        }
    case astits.SCTE35SpliceCommandTypeTimeSignal:
        if cmd.TimeSignal.SpliceTime != nil {
            fmt.Printf("time_signal pts=%d\n", cmd.TimeSignal.SpliceTime.PTS)
        }
    case astits.SCTE35SpliceCommandTypeNull:
        fmt.Println("splice_null")
    default:
        // Unknown commands keep their declared raw bytes.
        fmt.Printf("unknown splice command 0x%02x (%d bytes)\n", cmd.Type, len(cmd.Raw))
    }

    // Descriptors, including segmentation descriptors, are preserved raw.
    for _, desc := range d.SCTE35.Descriptors {
        fmt.Printf("descriptor tag=0x%02x data=% x\n", desc.Tag, desc.Data)
    }
}
```

## Mux

```go
// Create a cancellable context in case you want to stop writing packets/data any time you want
ctx, cancel := context.WithCancel(context.Background())

// Handle SIGTERM signal
ch := make(chan os.Signal, 1)
signal.Notify(ch, syscall.SIGTERM)
go func() {
    <-ch
    cancel()
}()

// Create your file or initialize any kind of io.Writer
// Buffering using bufio.Writer is recommended for performance
f, _ := os.Create("/path/to/file.ts")
defer f.Close()

// Create the muxer
mx := astits.NewMuxer(ctx, f)

// Add an elementary stream
mx.AddElementaryStream(astits.PMTElementaryStream{
    ElementaryPID: 1,
    StreamType:    astits.StreamTypeMetadata,
})

// Write tables
// Using that function is not mandatory, WriteData will retransmit tables from time to time 
mx.WriteTables()

// Write data
mx.WriteData(&astits.MuxerData{
    PES: &astits.PESData{
        Data: []byte("test"),
    },
    PID: 1,
})
```

## Options

In order to pass options to the demuxer or the muxer, look for the methods prefixed with `DemuxerOpt` or `MuxerOpt` and add them upon calling `NewDemuxer` or `NewMuxer` :

```go
// This is your custom packets parser
p := func(ps []*astits.Packet) (ds []*astits.Data, skip bool, err error) {
        // This is your logic
        skip = true
        return
}

// Now you can create a demuxer with the proper options
dmx := NewDemuxer(ctx, f, DemuxerOptPacketSize(192), DemuxerOptPacketsParser(p))
```

# CLI

This library provides 2 CLIs that will automatically get installed in `GOPATH/bin` on `go get` execution.

## astits-probe

### List streams

    $ astits-probe -i <path to your file> -f <format: text|json (default: text)>

### List packets

    $ astits-probe packets -i <path to your file>

### List data

    $ astits-probe data -i <path to your file> -d <data type: eit|nit|... (repeatable argument | if empty, all data types are shown)>

## astits-es-split

### Split streams into separate .ts files

    $ astits-es-split <path to your file> -o <path to output dir>

# Features and roadmap

- [x] Add demuxer
- [x] Add muxer
- [x] Demux PES packets
- [x] Mux PES packets
- [x] Demux PAT packets
- [x] Mux PAT packets
- [x] Demux PMT packets
- [x] Mux PMT packets
- [x] Demux EIT packets
- [ ] Mux EIT packets
- [x] Demux NIT packets
- [ ] Mux NIT packets
- [x] Demux SDT packets
- [ ] Mux SDT packets
- [x] Demux TOT packets
- [ ] Mux TOT packets
- [ ] Demux BAT packets
- [ ] Mux BAT packets
- [ ] Demux DIT packets
- [ ] Mux DIT packets
- [ ] Demux RST packets
- [ ] Mux RST packets
- [ ] Demux SIT packets
- [ ] Mux SIT packets
- [ ] Mux ST packets
- [ ] Demux TDT packets
- [ ] Mux TDT packets
- [ ] Demux TSDT packets
- [ ] Mux TSDT packets
- [x] Demux SCTE-35 splice info sections
- [ ] Mux SCTE-35 splice info sections
