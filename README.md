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

## Random access seek index

For offline 188-byte, unscrambled, lossless transport streams read through an
`io.ReadSeeker`, the demuxer can index the random access points once and then
jump to any program without replaying the tables from the beginning of the file.

Only PES packets on H.264 elementary streams declared by a complete PMT whose
first TS packet has both `payload_unit_start_indicator` and
`random_access_indicator` set, and whose complete header carries a PTS, become
index points. A point's `Offset` is the absolute file offset of that first
188-byte TS packet.

```go
f, _ := os.Open("/path/to/file.ts")
defer f.Close()

dmx := astits.NewDemuxer(context.Background(), f, astits.DemuxerOptPacketSize(188))

// BuildSeekIndex scans the file once. The reader position and the demuxer's
// pending state are preserved, whether the build succeeds or fails.
index, err := dmx.BuildSeekIndex()
if err != nil {
        log.Fatal(err)
}

for _, p := range index.Points() {
        fmt.Printf("program=%d epoch=%d pid=%d pts90k=%d offset=%d\n",
                p.ProgramNumber, p.Epoch, p.PID, p.PTS90k, p.Offset)
}

// Jump to the last access point of program 1, epoch 0 whose PTS is <= 90000.
// The seek goes straight to the point offset: bytes before it are not read.
point, err := dmx.SeekIndexed(index, 1, 0, 90000)
if err != nil {
        log.Fatal(err)
}
fmt.Printf("resuming at %d (PTS %d)\n", point.Offset, point.PTS90k)

// NextData continues sequentially from the landing point.
for {
        d, err := dmx.NextData()
        if err == astits.ErrNoMorePackets {
                break
        }
        if err != nil {
                log.Fatal(err)
        }
        _ = d
}
```

Epochs start at 0 for the first complete PAT binding plus PMT of a program and
increment for every new complete topology: a PMT PID change in the PAT (once a
complete PMT appears on the new PID), a program removed and recreated, or a PMT
whose PCR PID, program descriptors or ordered elementary stream declarations
change. Repeating the same content does not create a new epoch. A program that
is removed stops being indexed immediately.

After a successful `SeekIndexed`, the PAT/PMT classification state active at the
landing packet is restored, pending data and half PES packets are dropped, and
continuation packets are ignored until each PID sees its own payload unit
start. `Rewind` and repeated or backward seeks work with the same index.
`BuildSeekIndex` returns an error when the demuxer has a custom
`PacketsParser` or `PacketSkipper`, when the reader is not an
`io.ReadSeeker`, or when the packet size is not 188 bytes. An index is bound to
the demuxer that built it and is rejected by any other demuxer.

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
