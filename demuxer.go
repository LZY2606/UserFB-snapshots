package astits

import (
	"context"
	"errors"
	"fmt"
	"io"

	"github.com/asticode/go-astikit"
)

// Sync byte
const syncByte = '\x47'

// Errors
var (
	ErrNoMorePackets                = errors.New("astits: no more packets")
	ErrPacketMustStartWithASyncByte = errors.New("astits: packet must start with a sync byte")
)

// Demuxer represents a demuxer
// https://en.wikipedia.org/wiki/MPEG_transport_stream
// http://seidl.cs.vsb.cz/download/dvb/DVB_Poster.pdf
// http://www.etsi.org/deliver/etsi_en/300400_300499/300468/01.13.01_40/en_300468v011301o.pdf
type Demuxer struct {
	ctx        context.Context
	dataBuffer []*DemuxerData
	l          astikit.CompleteLogger

	optPacketSize    int
	optPacketsParser PacketsParser
	optPacketSkipper PacketSkipper

	clocks       *programClocks
	packetBuffer *packetBuffer
	packetPool   *packetPool
	programMap   *programMap
	r            io.Reader
	streamMap    *elementaryStreamMap
	// timedBuffer holds parsed data not yet returned by NextTimedData. The
	// three read entry points (NextPacket, NextData, NextTimedData) are not
	// meant to be mixed on the same demuxer, so only the buffer of the active
	// read mode is populated.
	timedBuffer []*TimedData
	readMode    demuxerReadMode
}

// demuxerReadMode records which read entry point is in use. A demuxer is
// driven through exactly one of them; Rewind resets the mode.
type demuxerReadMode uint8

const (
	demuxerReadModeUnset demuxerReadMode = iota
	demuxerReadModePacket
	demuxerReadModeData
	demuxerReadModeTimed
)

// PacketsParser represents an object capable of parsing a set of packets containing a unique payload spanning over those packets
// Use the skip returned argument to indicate whether the default process should still be executed on the set of packets
type PacketsParser func(ps []*Packet) (ds []*DemuxerData, skip bool, err error)

// PacketSkipper represents an object capable of skipping a packet before parsing its payload. Its header and adaptation field is parsed and provided to the object.
// Use this option if you need to filter out unwanted packets from your pipeline. NextPacket() will return the next unskipped packet if any.
type PacketSkipper func(p *Packet) (skip bool)

// NewDemuxer creates a new transport stream based on a reader
func NewDemuxer(ctx context.Context, r io.Reader, opts ...func(*Demuxer)) (d *Demuxer) {
	// Init
	d = &Demuxer{
		clocks:     newProgramClocks(),
		ctx:        ctx,
		l:          astikit.AdaptStdLogger(nil),
		programMap: newProgramMap(),
		r:          r,
		streamMap:  newElementaryStreamMap(),
	}
	d.packetPool = newPacketPool(d.programMap)
	d.packetPool.clocks = d.clocks

	// Apply options
	for _, opt := range opts {
		opt(d)
	}

	return
}

// DemuxerOptLogger returns the option to set the logger
func DemuxerOptLogger(l astikit.StdLogger) func(*Demuxer) {
	return func(d *Demuxer) {
		d.l = astikit.AdaptStdLogger(l)
	}
}

// DemuxerOptPacketSize returns the option to set the packet size
func DemuxerOptPacketSize(packetSize int) func(*Demuxer) {
	return func(d *Demuxer) {
		d.optPacketSize = packetSize
	}
}

// DemuxerOptPacketsParser returns the option to set the packets parser
func DemuxerOptPacketsParser(p PacketsParser) func(*Demuxer) {
	return func(d *Demuxer) {
		d.optPacketsParser = p
	}
}

// DemuxerOptPacketSkipper returns the option to set the packet skipper
func DemuxerOptPacketSkipper(s PacketSkipper) func(*Demuxer) {
	return func(d *Demuxer) {
		d.optPacketSkipper = s
	}
}

// NextPacket retrieves the next packet
func (dmx *Demuxer) NextPacket() (p *Packet, err error) {
	// Check ctx error
	// TODO Handle ctx error another way since if the read blocks, everything blocks
	// Maybe execute everything in a goroutine and listen the ctx channel in the same for loop
	if err = dmx.ctx.Err(); err != nil {
		return
	}
	dmx.readMode = demuxerReadModePacket

	// Create packet buffer if not exists
	if dmx.packetBuffer == nil {
		if dmx.packetBuffer, err = newPacketBuffer(dmx.r, dmx.optPacketSize, dmx.optPacketSkipper); err != nil {
			err = fmt.Errorf("astits: creating packet buffer failed: %w", err)
			return
		}
	}

	// Fetch next packet from buffer
	if p, err = dmx.packetBuffer.next(); err != nil {
		if err != ErrNoMorePackets {
			err = fmt.Errorf("astits: fetching next packet from buffer failed: %w", err)
		}
		return
	}
	return
}

// NextData retrieves the next data
func (dmx *Demuxer) NextData() (d *DemuxerData, err error) {
	// Check data buffer
	if len(dmx.dataBuffer) > 0 {
		d = dmx.dataBuffer[0]
		dmx.dataBuffer = dmx.dataBuffer[1:]
		return
	}

	// Fetch and process the next parsed payload
	var ds []*DemuxerData
	var snap *clockSnapshot
	if ds, snap, err = dmx.fetchParsedData(); err != nil {
		return nil, err
	}
	// fetchParsedData drives NextPacket internally; restore the data mode so
	// processData fills the buffer of this entry point only.
	dmx.readMode = demuxerReadModeData
	return dmx.processData(ds, snap), nil
}

// NextTimedData retrieves the next data together with the program clock context
// captured when its first TS packet was received. The delivery order and the
// embedded DemuxerData are identical to NextData; only the clock information is
// added. PAT/PMT and other non-PES data are delivered normally with nil time
// pointers.
func (dmx *Demuxer) NextTimedData() (td *TimedData, err error) {
	// Check timed data buffer
	if len(dmx.timedBuffer) > 0 {
		td = dmx.timedBuffer[0]
		dmx.timedBuffer = dmx.timedBuffer[1:]
		return
	}

	// Fetch and process the next parsed payload
	var ds []*DemuxerData
	var snap *clockSnapshot
	if ds, snap, err = dmx.fetchParsedData(); err != nil {
		return nil, err
	}
	// fetchParsedData drives NextPacket internally; restore the timed mode so
	// processData fills the timed buffer of this entry point only.
	dmx.readMode = demuxerReadModeTimed
	dmx.processData(ds, snap)

	td = dmx.timedBuffer[0]
	dmx.timedBuffer = dmx.timedBuffer[1:]
	return
}

// fetchParsedData pulls packets until the packet pool flushes a complete
// payload and parses it, draining the pool at the end of the stream. The
// clock snapshot returned alongside belongs to the flushed payload and is
// consumed exactly once per flush.
func (dmx *Demuxer) fetchParsedData() (ds []*DemuxerData, snap *clockSnapshot, err error) {
	for {
		// Get next packet
		var p *Packet
		if p, err = dmx.NextPacket(); err != nil {
			// If the end of the stream has been reached, we dump the packet pool
			if err == ErrNoMorePackets {
				for {
					// Dump packet pool
					ps := dmx.packetPool.dumpUnlocked()
					if len(ps) == 0 {
						return nil, nil, ErrNoMorePackets
					}
					// The pool attached the snapshot of this PID's queued payload.
					snap = dmx.packetPool.consumeClock(ps[0].Header.PID)

					// Parse data
					var errParseData error
					if ds, errParseData = parseData(ps, dmx.optPacketsParser, dmx.programMap, dmx.streamMap); errParseData != nil {
						// Log error as there may be some incomplete data here
						// We still want to try to parse all packets, in case final data is complete
						dmx.l.Error(fmt.Errorf("astits: parsing data failed: %w", errParseData))
						continue
					}
					if len(ds) > 0 {
						return ds, snap, nil
					}
				}
			}
			return nil, nil, fmt.Errorf("astits: fetching next packet failed: %w", err)
		}

		// Add packet to the pool
		ps := dmx.packetPool.addUnlocked(p)
		if len(ps) == 0 {
			continue
		}
		// The pool attached the snapshot captured when this payload started.
		snap = dmx.packetPool.consumeClock(ps[0].Header.PID)

		// Parse data
		if ds, err = parseData(ps, dmx.optPacketsParser, dmx.programMap, dmx.streamMap); err != nil {
			return nil, nil, fmt.Errorf("astits: building new data failed: %w", err)
		}
		if len(ds) > 0 {
			return ds, snap, nil
		}
	}
}

// processData updates the program/stream maps and the program clock tracker
// from a parsed batch, and feeds both read buffers. The clock snapshot used
// for a PES is the one captured when its first packet was received, so table
// updates within the same batch can never change it retroactively.
func (dmx *Demuxer) processData(ds []*DemuxerData, snap *clockSnapshot) (d *DemuxerData) {
	// Check whether there is data to be processed
	if len(ds) > 0 {
		// Process data
		d = ds[0]
		if dmx.readMode == demuxerReadModeData {
			dmx.dataBuffer = append(dmx.dataBuffer, ds[1:]...)
		}

		// Update program map and clock tracker
		for _, v := range ds {
			if v.PAT != nil {
				for _, pgm := range v.PAT.Programs {
					// Program number 0 is reserved to NIT
					if pgm.ProgramNumber > 0 {
						dmx.programMap.setUnlocked(pgm.ProgramMapID, pgm.ProgramNumber)
					}
				}
			}
			if v.PMT != nil {
				dmx.clocks.applyPMT(v.PMT)
				for _, es := range v.PMT.ElementaryStreams {
					dmx.streamMap.setUnlocked(es.ElementaryPID, v.PMT.ProgramNumber)
				}
			}
			if dmx.readMode == demuxerReadModeTimed {
				dmx.timedBuffer = append(dmx.timedBuffer, dmx.clocks.timedData(v, snap))
			}
		}
	}
	return
}

// Rewind rewinds the demuxer reader
func (dmx *Demuxer) Rewind() (n int64, err error) {
	dmx.dataBuffer = []*DemuxerData{}
	dmx.timedBuffer = []*TimedData{}
	dmx.readMode = demuxerReadModeUnset
	dmx.clocks.reset()
	dmx.packetBuffer = nil
	dmx.packetPool = newPacketPool(dmx.programMap)
	dmx.packetPool.clocks = dmx.clocks
	if n, err = rewind(dmx.r); err != nil {
		err = fmt.Errorf("astits: rewinding reader failed: %w", err)
		return
	}
	return
}
