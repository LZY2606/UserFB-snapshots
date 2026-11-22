package task_checks

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"testing"

	"github.com/asticode/go-astits"
	"github.com/asticode/go-astits/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Independent TS / PSI / PES generator (no production helper is reused)
// ---------------------------------------------------------------------------

func tsCRC32(bs []byte) uint32 {
	crc := uint32(0xffffffff)
	for _, b := range bs {
		crc ^= uint32(b) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = crc<<1 ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

func withCRC(bs []byte) []byte {
	crc := tsCRC32(bs)
	return append(bs, byte(crc>>24), byte(crc>>16), byte(crc>>8), byte(crc))
}

type patProgram struct {
	program uint16
	pmtPID  uint16
}

func makePAT(transportStreamID uint16, programs []patProgram) []byte {
	sectionLength := 5 + 4*len(programs) + 4
	b := []byte{
		0x00,
		0xb0 | byte((sectionLength>>8)&0x0f), byte(sectionLength),
		byte(transportStreamID >> 8), byte(transportStreamID),
		0xc1, // version 0, current
		0x00, 0x00,
	}
	for _, p := range programs {
		b = append(b, byte(p.program>>8), byte(p.program), 0xe0|byte(p.pmtPID>>8), byte(p.pmtPID))
	}
	return withCRC(b)
}

type esDeclaration struct {
	streamType  byte
	pid         uint16
	descriptors []byte
}

func makePMT(program uint16, pcrPID uint16, programDescriptors []byte, es []esDeclaration) []byte {
	sectionLength := 5 + 4 + len(programDescriptors) + 4
	for _, e := range es {
		sectionLength += 5 + len(e.descriptors)
	}
	b := []byte{
		0x02,
		0xb0 | byte((sectionLength>>8)&0x0f), byte(sectionLength),
		byte(program >> 8), byte(program),
		0xc1, // version 0, current
		0x00, 0x00,
		0xe0 | byte(pcrPID>>8), byte(pcrPID),
		0xf0 | byte(len(programDescriptors)>>8), byte(len(programDescriptors)),
	}
	b = append(b, programDescriptors...)
	for _, e := range es {
		b = append(b, e.streamType, 0xe0|byte(e.pid>>8), byte(e.pid),
			0xf0|byte(len(e.descriptors)>>8), byte(len(e.descriptors)))
		b = append(b, e.descriptors...)
	}
	return withCRC(b)
}

func encodePTS(pts int64) []byte {
	p := uint64(pts)
	return []byte{
		byte(0x20 | (p>>29)&0x0e | 1),
		byte(p >> 22),
		byte((p>>14)&0xfe | 1),
		byte(p >> 7),
		byte((p<<1)&0xfe | 1),
	}
}

// makePES builds a PES packet. A negative pts disables the PTS field.
func makePES(streamID byte, pts int64, payload []byte, unbounded bool) []byte {
	b := []byte{0x00, 0x00, 0x01, streamID, 0x00, 0x00, 0x80, 0x00, 0x00}
	var optional []byte
	if pts >= 0 {
		optional = append(optional, encodePTS(pts)...)
		b[7] = 0x80
	}
	b[8] = byte(len(optional))
	b = append(b, optional...)
	if !unbounded {
		l := len(optional) + len(payload)
		b[4] = byte(l >> 8)
		b[5] = byte(l)
	}
	return append(b, payload...)
}

// ---------------------------------------------------------------------------
// Packet assembly helpers
// ---------------------------------------------------------------------------

type packetSpec struct {
	pid     uint16
	pusi    bool
	random  bool
	payload []byte
}

type tsWriter struct {
	buf bytes.Buffer
	cc  map[uint16]uint8
}

func newTSWriter() *tsWriter { return &tsWriter{cc: map[uint16]uint8{}} }

func (w *tsWriter) write(p packetSpec) {
	buf := make([]byte, 188)
	for i := range buf {
		buf[i] = 0xff
	}
	buf[0] = 0x47
	buf[1] = byte(p.pid >> 8)
	if p.pusi {
		buf[1] |= 0x40
	}
	buf[2] = byte(p.pid)
	buf[3] = w.cc[p.pid] & 0x0f
	w.cc[p.pid] += 1

	const headerLen = 4
	if p.random {
		afLen := 183 - len(p.payload)
		if afLen < 1 {
			panic("payload too large for a random access packet")
		}
		buf[3] |= 0x20 | 0x10 // adaptation field + payload
		buf[4] = byte(afLen)
		buf[5] = 0x40 // random_access_indicator
		copy(buf[6+(afLen-1):], p.payload)
	} else if len(p.payload) == 184 {
		buf[3] |= 0x10
		copy(buf[headerLen:], p.payload)
	} else if len(p.payload) > 0 {
		// Stuff with an adaptation field so that the payload is exactly the
		// intended bytes rather than being padded with payload bytes.
		afLen := 184 - len(p.payload) - 1
		buf[3] |= 0x20 | 0x10
		buf[4] = byte(afLen)
		buf[5] = 0x00 // flags: no random access, no PCR
		copy(buf[5+afLen:], p.payload)
	}
	w.buf.Write(buf)
}

func (w *tsWriter) writePSI(pid uint16, section []byte) {
	data := append([]byte{0x00}, section...)
	first := true
	for len(data) > 0 {
		n := len(data)
		if n > 184 {
			n = 184
		}
		w.write(packetSpec{pid: pid, pusi: first, payload: data[:n]})
		data = data[n:]
		first = false
	}
}

func (w *tsWriter) writePES(pid uint16, data []byte, random bool) {
	first := true
	for len(data) > 0 {
		capacity := 184
		if first && random {
			capacity = 182
		}
		n := len(data)
		if n > capacity {
			n = capacity
		}
		w.write(packetSpec{pid: pid, pusi: first, random: first && random, payload: data[:n]})
		data = data[n:]
		first = false
	}
}

// writePESChunks writes a PES using explicit per-packet payload sizes so that
// a header can be forced to span packets.
func (w *tsWriter) writePESChunks(pid uint16, data []byte, random bool, sizes []int) {
	first := true
	for len(data) > 0 {
		capacity := 184
		if first && random {
			capacity = 182
		}
		n := len(data)
		if len(sizes) > 0 {
			if sizes[0] < n {
				n = sizes[0]
			}
			sizes = sizes[1:]
		}
		if n > capacity {
			n = capacity
		}
		w.write(packetSpec{pid: pid, pusi: first, random: first && random, payload: data[:n]})
		data = data[n:]
		first = false
	}
}

func (w *tsWriter) bytes() []byte { return w.buf.Bytes() }

// h264Payload returns payload bytes that do not start with a PES start code,
// which is what real H.264 access units look like.
func h264Payload(size int, seed byte) []byte {
	p := make([]byte, size)
	p[0] = 0
	p[1] = 0
	p[2] = 0
	p[3] = 1
	p[4] = 0x65
	for i := 5; i < size; i++ {
		p[i] = seed
	}
	return p
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// ---------------------------------------------------------------------------
// Instrumented reader
// ---------------------------------------------------------------------------

type rangeReader struct {
	data   []byte
	pos    int64
	ranges [][2]int64
}

func newRangeReader(data []byte) *rangeReader { return &rangeReader{data: data} }

func (r *rangeReader) Read(p []byte) (int, error) {
	if r.pos >= int64(len(r.data)) {
		return 0, io.EOF
	}
	n := copy(p, r.data[r.pos:])
	r.ranges = append(r.ranges, [2]int64{r.pos, r.pos + int64(n)})
	r.pos += int64(n)
	return n, nil
}

func (r *rangeReader) Seek(offset int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
		r.pos = offset
	case io.SeekCurrent:
		r.pos += offset
	case io.SeekEnd:
		r.pos = int64(len(r.data)) + offset
	default:
		return 0, errors.New("bad whence")
	}
	return r.pos, nil
}

// failReader fails once after a configurable number of bytes have been read.
// A zero failAfter disables the failure.
type failReader struct {
	*rangeReader
	failAfter int64
	failed    bool
}

func (r *failReader) Read(p []byte) (int, error) {
	if r.failAfter > 0 && !r.failed && r.pos >= r.failAfter {
		r.failed = true
		return 0, errors.New("injected read error")
	}
	return r.rangeReader.Read(p)
}

// ---------------------------------------------------------------------------
// Demux helpers
// ---------------------------------------------------------------------------

type demuxItem struct {
	pat  bool
	pmt  bool
	pes  bool
	pid  uint16
	pts  int64
	data []byte
}

func describeDemuxData(d *astits.DemuxerData) string {
	if d == nil {
		return "<nil>"
	}
	return fmt.Sprintf("pid=%d pat=%v pmt=%v pes=%v", d.PID, d.PAT != nil, d.PMT != nil, d.PES != nil)
}

// sameDemuxData compares the meaningful content of two DemuxerData values,
// ignoring pointer identities.
func sameDemuxData(a, b *astits.DemuxerData) bool {
	if (a == nil) != (b == nil) {
		return false
	}
	if a == nil {
		return true
	}
	if a.PID != b.PID || (a.PAT != nil) != (b.PAT != nil) || (a.PMT != nil) != (b.PMT != nil) ||
		(a.PES != nil) != (b.PES != nil) || (a.EIT != nil) != (b.EIT != nil) ||
		(a.NIT != nil) != (b.NIT != nil) || (a.SDT != nil) != (b.SDT != nil) || (a.TOT != nil) != (b.TOT != nil) {
		return false
	}
	if a.PAT != nil {
		if a.PAT.TransportStreamID != b.PAT.TransportStreamID || len(a.PAT.Programs) != len(b.PAT.Programs) {
			return false
		}
		for i := range a.PAT.Programs {
			if *a.PAT.Programs[i] != *b.PAT.Programs[i] {
				return false
			}
		}
	}
	if a.PMT != nil {
		if a.PMT.ProgramNumber != b.PMT.ProgramNumber || a.PMT.PCRPID != b.PMT.PCRPID ||
			len(a.PMT.ElementaryStreams) != len(b.PMT.ElementaryStreams) {
			return false
		}
		for i := range a.PMT.ElementaryStreams {
			ea, eb := a.PMT.ElementaryStreams[i], b.PMT.ElementaryStreams[i]
			if ea.ElementaryPID != eb.ElementaryPID || ea.StreamType != eb.StreamType ||
				len(ea.ElementaryStreamDescriptors) != len(eb.ElementaryStreamDescriptors) {
				return false
			}
		}
	}
	if a.PES != nil {
		if !bytes.Equal(a.PES.Data, b.PES.Data) {
			return false
		}
		pa, pb := a.PES.Header.OptionalHeader, b.PES.Header.OptionalHeader
		va := int64(-1)
		vb := int64(-1)
		if pa != nil && pa.PTS != nil {
			va = pa.PTS.Base
		}
		if pb != nil && pb.PTS != nil {
			vb = pb.PTS.Base
		}
		if va != vb || a.PES.Header.StreamID != b.PES.Header.StreamID {
			return false
		}
	}
	return true
}

func drain(t *testing.T, dmx *astits.Demuxer) []demuxItem {
	t.Helper()
	var items []demuxItem
	for {
		d, err := dmx.NextData()
		if err == astits.ErrNoMorePackets {
			return items
		}
		if err != nil {
			t.Fatalf("NextData: %v", err)
		}
		it := demuxItem{pid: d.PID}
		if d.PAT != nil {
			it.pat = true
		}
		if d.PMT != nil {
			it.pmt = true
		}
		if d.PES != nil {
			it.pes = true
			if d.PES.Header != nil && d.PES.Header.OptionalHeader != nil && d.PES.Header.OptionalHeader.PTS != nil {
				it.pts = d.PES.Header.OptionalHeader.PTS.Base
			}
			it.data = d.PES.Data
		}
		items = append(items, it)
	}
}

// TestSeekIndexPoints exercises indexing behaviour: which PES become points,
// epoch transitions and offset accuracy.
func TestSeekIndexPoints(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// -- Two interleaved programs with random access PES and B-frame style
		// out-of-order PTS on non random access PES --------------------------
		w := newTSWriter()
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}, {2, 0x200}}))
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		w.writePSI(0x200, makePMT(2, 0x201, nil, []esDeclaration{{0x1b, 0x201, nil}}))

		// program 1: RA PES at 9000, then a B-frame PES with a lower PTS that
		// is NOT a random access point, then RA PES at 18000.
		w.writePES(0x101, makePES(0xe0, 9000, h264Payload(400, 0xa1), true), true)
		w.writePES(0x101, makePES(0xe0, 4500, h264Payload(120, 0xa2), true), false)
		w.writePES(0x101, makePES(0xe0, 18000, h264Payload(400, 0xa3), true), true)
		// program 2: own RA PES at 27000.
		w.writePES(0x201, makePES(0xe0, 27000, h264Payload(200, 0xb1), true), true)

		raw := w.bytes()
		dmx := astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		idx, err := dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex error: %v", err)
		if err != nil {
			return
		}
		points := idx.Points()

		var p1, p2 []astits.SeekPoint
		for _, p := range points {
			switch p.ProgramNumber {
			case 1:
				p1 = append(p1, p)
			case 2:
				p2 = append(p2, p)
			}
		}
		c.Check(len(p1) == 2, "program 1 points: got %d (%v)", len(p1), p1)
		c.Check(len(p2) == 1, "program 2 points: got %d (%v)", len(p2), p2)
		if len(p1) == 2 {
			c.Check(p1[0].PTS90k == 9000 && p1[1].PTS90k == 18000, "program 1 PTS: %v", p1)
			c.Check(p1[0].Epoch == 0 && p1[1].Epoch == 0, "program 1 epoch: %v", p1)
			c.Check(p1[0].PID == 0x101, "program 1 PID: %d", p1[0].PID)
		}
		if len(p2) == 1 {
			c.Check(p2[0].PID == 0x201 && p2[0].PTS90k == 27000, "program 2 point: %+v", p2[0])
		}

		// Offsets must be the first TS packet of the PES, which carries PUSI.
		for _, p := range points {
			c.Check(p.Offset%188 == 0, "point offset not packet aligned: %d", p.Offset)
			if p.Offset+4 <= int64(len(raw)) {
				hdr := raw[p.Offset : p.Offset+4]
				c.Check(hdr[0] == 0x47 && hdr[1]&0x40 != 0, "point offset %d is not a PUSI packet: %x", p.Offset, hdr)
				pid := uint16(hdr[1]&0x1f)<<8 | uint16(hdr[2])
				c.Check(pid == p.PID, "point PID mismatch at %d: %d vs %d", p.Offset, pid, p.PID)
			}
		}

		// Points() must return an independent copy.
		a := idx.Points()
		if len(a) > 0 {
			a[0].PTS90k = -1
			a[0].ProgramNumber = 999
			b := idx.Points()
			c.Check(b[0].PTS90k != -1 && b[0].ProgramNumber != 999, "Points aliased the index: %+v", b[0])
		}

		// -- Zero-length PES and PES header split across packets -------------
		// The RA PES below has a zero length and spans several packets; the
		// following PES must not be merged into it.
		w = newTSWriter()
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}}))
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		zero := makePES(0xe0, 30000, h264Payload(500, 0xc1), true)
		w.writePES(0x101, zero, true)
		w.writePES(0x101, makePES(0xe0, 60000, h264Payload(10, 0xc2), true), true)
		// A PES whose header is split across packets, with only the first
		// fragment in the random access packet: the point must still be the
		// first packet.
		splitPES := makePES(0xe0, 66000, h264Payload(120, 0xc3), true)
		w.writePESChunks(0x101, splitPES, true, []int{5, 4})

		raw = w.bytes()
		dmx = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		idx, err = dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex (zero length) error: %v", err)
		if err == nil {
			points = idx.Points()
			c.Check(len(points) == 3, "zero length points: got %d (%v)", len(points), points)
			if len(points) == 3 {
				c.Check(points[0].PTS90k == 30000 && points[1].PTS90k == 60000 && points[2].PTS90k == 66000, "zero length PTS: %v", points)
				// The split-header point must point at its PUSI packet, not at
				// the packet where the PTS finished.
				last := points[2]
				c.Check(last.Offset%188 == 0 && raw[last.Offset+1]&0x40 != 0, "split header offset: %d", last.Offset)
			}
		}

		// -- Sections that span packets and a table change mid-PES -----------
		// A PAT section large enough to span two packets, with a PMT change
		// between the first and second packet of a video PES.
		w = newTSWriter()
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}, {2, 0x200}, {3, 0x300}}))
		// Pad the PMT with a private descriptor so the first topology itself
		// spans several TS packets.
		longDesc := bytes.Repeat([]byte{0xab}, 200)
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, longDesc}}))
		// Interleave a half PES on another PID before the point.
		w.write(packetSpec{pid: 0x105, payload: []byte{1, 2, 3, 4}})
		midPES := makePES(0xe0, 10000, h264Payload(400, 0xd1), true)
		w.writePESChunks(0x101, midPES, true, []int{100})
		// A table change while the PES is still being accumulated: the PES
		// keeps its original epoch.
		w.writePSI(0x100, makePMT(1, 0x102, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		// Remaining bytes of the same PES.
		rem := midPES[100:]
		w.write(packetSpec{pid: 0x101, payload: rem[:minInt(len(rem), 184)]})
		if len(rem) > 184 {
			w.write(packetSpec{pid: 0x101, payload: rem[184:]})
		}
		w.writePES(0x101, makePES(0xe0, 20000, h264Payload(100, 0xd2), true), true)

		raw = w.bytes()
		dmx = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		idx, err = dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex (mid-PES table change) error: %v", err)
		if err == nil {
			points = idx.Points()
			c.Check(len(points) == 2, "mid-PES points: %v", points)
			if len(points) == 2 {
				c.Check(points[0].PTS90k == 10000 && points[0].Epoch == 0, "PES crossing a table change must keep epoch 0: %+v", points[0])
				c.Check(points[1].PTS90k == 20000 && points[1].Epoch == 1, "later PES must use epoch 1: %+v", points[1])
			}
		}

		// -- Epochs: duplicate table, topology change, migration, removal -----
		w = newTSWriter()
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}}))
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		w.writePES(0x101, makePES(0xe0, 10000, h264Payload(100, 1), true), true)
		// Duplicate PMT by content: no new epoch.
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		w.writePES(0x101, makePES(0xe0, 20000, h264Payload(100, 2), true), true)
		// PCR PID changes: new epoch.
		w.writePSI(0x100, makePMT(1, 0x102, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		w.writePES(0x101, makePES(0xe0, 30000, h264Payload(100, 3), true), true)
		// Duplicate of the PCR PID change: no new epoch.
		w.writePSI(0x100, makePMT(1, 0x102, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		w.writePES(0x101, makePES(0xe0, 40000, h264Payload(100, 4), true), true)

		raw = w.bytes()
		dmx = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		idx, err = dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex (epochs) error: %v", err)
		if err == nil {
			points = idx.Points()
			var byEpoch = map[uint64][]int64{}
			for _, p := range points {
				byEpoch[p.Epoch] = append(byEpoch[p.Epoch], p.PTS90k)
			}
			c.Check(len(byEpoch[0]) == 2, "epoch 0 points: %v", byEpoch[0])
			c.Check(len(byEpoch[1]) == 2, "epoch 1 points: %v", byEpoch[1])
			c.Check(len(points) == 4, "total epoch points: %d", len(points))
		}

		// -- PMT migration: points keep the old epoch until the new PMT is read
		w = newTSWriter()
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}}))
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		w.writePES(0x101, makePES(0xe0, 10000, h264Payload(100, 1), true), true)
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x110}}))
		// Before the new PMT arrives, the PES still belongs to epoch 0.
		w.writePES(0x101, makePES(0xe0, 20000, h264Payload(100, 2), true), true)
		w.writePSI(0x110, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		// Same content but new PMT PID: new epoch.
		w.writePES(0x101, makePES(0xe0, 30000, h264Payload(100, 3), true), true)

		raw = w.bytes()
		dmx = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		idx, err = dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex (migration) error: %v", err)
		if err == nil {
			points = idx.Points()
			c.Check(len(points) == 3, "migration points: %v", points)
			if len(points) == 3 {
				c.Check(points[0].Epoch == 0 && points[0].PTS90k == 10000, "migration p0: %+v", points[0])
				c.Check(points[1].Epoch == 0 && points[1].PTS90k == 20000, "migration p1: %+v", points[1])
				c.Check(points[2].Epoch == 1 && points[2].PTS90k == 30000, "migration p2: %+v", points[2])
			}
		}

		// -- Program removal and rebuild -------------------------------------
		w = newTSWriter()
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}}))
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		w.writePES(0x101, makePES(0xe0, 10000, h264Payload(100, 1), true), true)
		// Remove the program; the PES below must not be recorded.
		w.writePSI(0, makePAT(1, nil))
		w.writePES(0x101, makePES(0xe0, 20000, h264Payload(100, 2), true), true)
		// Rebuild the program: back to epoch 0.
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}}))
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		w.writePES(0x101, makePES(0xe0, 30000, h264Payload(100, 3), true), true)

		raw = w.bytes()
		dmx = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		idx, err = dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex (removal) error: %v", err)
		if err == nil {
			points = idx.Points()
			c.Check(len(points) == 2, "removal points: %v", points)
			if len(points) == 2 {
				c.Check(points[0].Epoch == 0 && points[0].PTS90k == 10000, "removal p0: %+v", points[0])
				// A program that is removed and rebuilt forms a new epoch.
				c.Check(points[1].Epoch == 1 && points[1].PTS90k == 30000, "removal p1: %+v", points[1])
			}
		}

		// -- Only H.264 random access PES become points ----------------------
		w = newTSWriter()
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}}))
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{
			{0x1b, 0x101, nil}, // H.264
			{0x0f, 0x102, nil}, // AAC
		}))
		w.writePES(0x101, makePES(0xe0, 10000, h264Payload(100, 1), true), true)
		// Audio random access PES must be ignored.
		w.writePES(0x102, makePES(0xc0, 11000, make([]byte, 40), true), true)
		// H.264 PES without a PTS must be ignored.
		w.writePES(0x101, makePES(0xe0, -1, h264Payload(100, 2), true), true)
		// H.264 PES with no random access indicator must be ignored.
		w.writePES(0x101, makePES(0xe0, 12000, h264Payload(100, 3), true), false)

		raw = w.bytes()
		dmx = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		idx, err = dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex (filters) error: %v", err)
		if err == nil {
			points = idx.Points()
			c.Check(len(points) == 1, "filtered points: %v", points)
			if len(points) == 1 {
				c.Check(points[0].PID == 0x101 && points[0].PTS90k == 10000, "filtered point: %+v", points[0])
			}
		}

		// -- Unsupported configurations and lookups ---------------------------
		raw = w.bytes()
		_, err = astits.NewDemuxer(context.Background(), bytes.NewReader(raw)).BuildSeekIndex()
		c.Check(err == nil, "default packet size should be accepted: %v", err)
		_, err = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(192)).BuildSeekIndex()
		c.Check(err != nil, "192-byte packets must be rejected")
		_, err = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketsParser(func([]*astits.Packet) ([]*astits.DemuxerData, bool, error) {
			return nil, true, nil
		})).BuildSeekIndex()
		c.Check(err != nil, "custom packets parser must be rejected")
		_, err = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSkipper(func(*astits.Packet) bool { return false })).BuildSeekIndex()
		c.Check(err != nil, "packet skipper must be rejected")
		_, err = astits.NewDemuxer(context.Background(), io.LimitReader(bytes.NewReader(raw), int64(len(raw)))).BuildSeekIndex()
		c.Check(err != nil, "non seekable reader must be rejected")

		// Unknown program/epoch and targets before the first point.
		dmx = astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		idx, err = dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex (lookups) error: %v", err)
		if err == nil {
			_, err = dmx.SeekIndexed(idx, 7, 0, 10000)
			c.Check(errors.Is(err, astits.ErrSeekIndexNotFound), "unknown program: %v", err)
			_, err = dmx.SeekIndexed(idx, 1, 42, 10000)
			c.Check(errors.Is(err, astits.ErrSeekIndexNotFound), "unknown epoch: %v", err)
			_, err = dmx.SeekIndexed(idx, 1, 0, 9999)
			c.Check(errors.Is(err, astits.ErrSeekIndexNotFound), "target before first point: %v", err)
		}
	})
}

// TestSeekIndexRestore verifies that building an index leaves the demuxer and
// reader exactly as they were, and that a successful seek resumes cleanly.
func TestSeekIndexRestore(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		build := func() []byte {
			w := newTSWriter()
			w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}, {2, 0x200}}))
			w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
			w.writePSI(0x200, makePMT(2, 0x201, nil, []esDeclaration{{0x1b, 0x201, nil}}))
			for i := 0; i < 6; i++ {
				w.writePES(0x101, makePES(0xe0, int64(10000*(i+1)), h264Payload(150, byte(i)), true), true)
				w.writePES(0x201, makePES(0xe0, int64(10000*(i+1)+5000), h264Payload(80, byte(10+i)), true), true)
			}
			return w.bytes()
		}
		raw := build()

		// -- Reader position and pending data survive BuildSeekIndex ---------
		rr := newRangeReader(raw)
		dmx := astits.NewDemuxer(context.Background(), rr, astits.DemuxerOptPacketSize(188))
		rrRef := newRangeReader(raw)
		dmxRef := astits.NewDemuxer(context.Background(), rrRef, astits.DemuxerOptPacketSize(188))
		for i := 0; i < 3; i++ {
			_, err := dmx.NextData()
			c.Check(err == nil, "warm-up NextData %d: %v", i, err)
			_, _ = dmxRef.NextData()
		}
		posBefore := rr.pos

		idx, err := dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex: %v", err)
		c.Check(rr.pos == posBefore, "reader position changed: %d != %d", rr.pos, posBefore)

		// The sequential pipeline must be identical to an untouched demuxer.
		for i := 0; i < 8; i++ {
			a, ea := dmx.NextData()
			b, eb := dmxRef.NextData()
			c.Check(fmt.Sprint(ea) == fmt.Sprint(eb), "NextData error diverged at %d: %v vs %v", i, ea, eb)
			c.Check(sameDemuxData(a, b), "NextData data diverged at %d: %s vs %s", i, describeDemuxData(a), describeDemuxData(b))
			if ea != nil || eb != nil {
				break
			}
		}

		// -- Failure keeps the previous index and the sequential reader ------
		rrFail := &failReader{rangeReader: newRangeReader(raw), failAfter: int64(len(raw) / 3)}
		dmxFail := astits.NewDemuxer(context.Background(), rrFail, astits.DemuxerOptPacketSize(188))
		failedIdx, err := dmxFail.BuildSeekIndex()
		c.Check(err != nil, "injected read error must fail BuildSeekIndex")
		c.Check(failedIdx == nil, "failed build returned an index")
		c.Check(rrFail.pos == 0, "reader position after failure: %d", rrFail.pos)

		// A pre-existing index stays valid and usable after a failed build.
		// The same switchable reader is used for both builds.
		rrOK := newRangeReader(raw)
		switchable := &failReader{rangeReader: rrOK}
		dmxOK := astits.NewDemuxer(context.Background(), switchable, astits.DemuxerOptPacketSize(188))
		good, err := dmxOK.BuildSeekIndex()
		c.Check(err == nil, "good index: %v", err)
		c.Check(good != nil && len(good.Points()) == 12, "good index points: %v", good)

		// Enable the injected failure, rebuild, then disable it again.
		switchable.failAfter = 100
		switchable.failed = false
		rrOK.pos = 0
		_, err = dmxOK.BuildSeekIndex()
		c.Check(err != nil, "build on a failing reader must fail")
		switchable.failAfter = 0
		rrOK.pos = 0
		if good != nil {
			_, err = dmxOK.SeekIndexed(good, 1, 0, 30000)
			c.Check(err == nil, "old index unusable after failed rebuild: %v", err)
			if err == nil {
				items := drain(t, dmxOK)
				var ok bool
				for _, it := range items {
					if it.pes && it.pts >= 30000 {
						ok = true
					}
				}
				c.Check(ok, "old index did not deliver data after failed rebuild")
			}
		}

		// -- Seek success: no back-scan of the prefix, target PES intact -----
		rr = newRangeReader(raw)
		dmx = astits.NewDemuxer(context.Background(), rr, astits.DemuxerOptPacketSize(188))
		idx, err = dmx.BuildSeekIndex()
		c.Check(err == nil, "BuildSeekIndex (seek): %v", err)
		if err != nil {
			return
		}
		points := idx.Points()
		var target astits.SeekPoint
		for _, p := range points {
			if p.ProgramNumber == 1 && p.PTS90k == 30000 {
				target = p
			}
		}
		c.Check(target.Offset > 0, "target point missing: %v", points)
		rr.ranges = nil
		got, err := dmx.SeekIndexed(idx, 1, 0, 35000)
		c.Check(err == nil, "SeekIndexed: %v", err)
		c.Check(got == target, "seek returned %+v, want %+v", got, target)
		for _, r := range rr.ranges {
			c.Check(r[0] >= target.Offset, "seek read before the target offset: %v", r)
		}

		items := drain(t, dmx)
		var gotPES bool
		for _, it := range items {
			if it.pes && it.pid == 0x101 {
				gotPES = true
				if it.pts == 30000 {
					c.Check(bytes.Equal(it.data, h264Payload(150, 2)), "target PES payload truncated or altered (len=%d)", len(it.data))
				}
				c.Check(it.pts >= 30000, "unexpected earlier PTS %d after seek", it.pts)
			}
			c.Check(!(it.pat && it.pid == 0), "no synthesized PAT after seek")
		}
		c.Check(gotPES, "no PES delivered after seek")
		// Other programs keep flowing.
		var otherProgram bool
		for _, it := range items {
			if it.pes && it.pid == 0x201 {
				otherProgram = true
			}
		}
		c.Check(otherProgram, "other program PES not delivered after seek")

		// -- Table changes after the landing point are still delivered --------
		wTail := newTSWriter()
		wTail.writePSI(0, makePAT(1, []patProgram{{1, 0x100}}))
		wTail.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		wTail.writePES(0x101, makePES(0xe0, 10000, h264Payload(100, 1), true), true)
		wTail.writePES(0x101, makePES(0xe0, 20000, h264Payload(100, 2), true), true)
		// A real PMT change after the second point; new PES must still arrive.
		wTail.writePSI(0x100, makePMT(1, 0x103, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		wTail.writePES(0x101, makePES(0xe0, 30000, h264Payload(100, 3), true), true)
		tailRaw := wTail.bytes()

		rrTail := newRangeReader(tailRaw)
		dmxTail := astits.NewDemuxer(context.Background(), rrTail, astits.DemuxerOptPacketSize(188))
		idxTail, err := dmxTail.BuildSeekIndex()
		c.Check(err == nil, "tail index: %v", err)
		if err == nil {
			p, err := dmxTail.SeekIndexed(idxTail, 1, 0, 20000)
			c.Check(err == nil && p.PTS90k == 20000, "tail seek: %+v %v", p, err)
			tailItems := drain(t, dmxTail)
			var saw20, saw30, sawPMT bool
			for _, it := range tailItems {
				if it.pes && it.pts == 20000 {
					saw20 = true
				}
				if it.pes && it.pts == 30000 {
					saw30 = true
				}
				if it.pmt {
					sawPMT = true
				}
			}
			c.Check(saw20, "landing PES missing after seek")
			c.Check(saw30, "PES after a post-seek PMT change missing")
			c.Check(sawPMT, "the real post-seek PMT change was not delivered")
		}

		// -- Repeated seeks, backwards seeks and reuse after Rewind -----------
		if _, err = dmx.SeekIndexed(idx, 1, 0, 20000); err != nil {
			c.Check(false, "backwards seek: %v", err)
		}
		items = drain(t, dmx)
		var ptsAt20 bool
		for _, it := range items {
			if it.pes && it.pid == 0x101 && it.pts == 20000 {
				ptsAt20 = true
			}
		}
		c.Check(ptsAt20, "backwards seek did not deliver PTS 20000")

		// Reuse after Rewind.
		if _, err := dmx.Rewind(); err != nil {
			c.Check(false, "Rewind: %v", err)
		}
		items = drain(t, dmx)
		c.Check(len(items) > 0, "Rewind then sequential read produced nothing")

		if _, err = dmx.SeekIndexed(idx, 1, 0, 30000); err != nil {
			c.Check(false, "seek after rewind: %v", err)
		}
		items = drain(t, dmx)
		var afterRewind bool
		for _, it := range items {
			if it.pes && it.pid == 0x101 && it.pts == 30000 {
				afterRewind = true
			}
		}
		c.Check(afterRewind, "seek after Rewind did not deliver PTS 30000")

		// Seek after EOF.
		if _, err = dmx.SeekIndexed(idx, 1, 0, 10000); err != nil {
			c.Check(false, "seek after EOF: %v", err)
		}
		items = drain(t, dmx)
		var afterEOF bool
		for _, it := range items {
			if it.pes && it.pid == 0x101 && it.pts == 10000 {
				afterEOF = true
			}
		}
		c.Check(afterEOF, "seek after EOF did not deliver PTS 10000")
	})
}

// TestSeekIndexIsolation covers index ownership and the effect of a table
// change that happens after a point was recorded.
func TestSeekIndexIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		w := newTSWriter()
		w.writePSI(0, makePAT(1, []patProgram{{1, 0x100}}))
		w.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		w.writePES(0x101, makePES(0xe0, 10000, h264Payload(100, 1), true), true)
		w.writePES(0x101, makePES(0xe0, 20000, h264Payload(100, 2), true), true)
		raw := w.bytes()

		d1 := astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		d2 := astits.NewDemuxer(context.Background(), bytes.NewReader(raw), astits.DemuxerOptPacketSize(188))
		idx1, err := d1.BuildSeekIndex()
		c.Check(err == nil, "d1 index: %v", err)
		idx2, err := d2.BuildSeekIndex()
		c.Check(err == nil, "d2 index: %v", err)
		if idx1 == nil || idx2 == nil {
			return
		}

		// Using an index with another demuxer must fail and leave it usable.
		_, err = d2.SeekIndexed(idx1, 1, 0, 20000)
		c.Check(errors.Is(err, astits.ErrSeekIndexOwner), "foreign index: %v", err)
		items := drain(t, d2)
		c.Check(len(items) > 0, "demuxer unusable after foreign index rejection")

		// Indexes built by different demuxers carry the same content.
		c.Check(fmt.Sprint(idx1.Points()) == fmt.Sprint(idx2.Points()), "indexes disagree: %v vs %v", idx1.Points(), idx2.Points())

		// The index does not become stale when the demuxer's live topology is
		// mutated by later data: the points keep their original epoch.
		w2 := newTSWriter()
		w2.writePSI(0, makePAT(1, []patProgram{{1, 0x100}}))
		w2.writePSI(0x100, makePMT(1, 0x101, nil, []esDeclaration{{0x1b, 0x101, nil}}))
		for i := 0; i < 3; i++ {
			w2.writePES(0x101, makePES(0xe0, int64(10000*(i+1)), h264Payload(100, byte(i)), true), true)
			// A PMT change between PES must not re-epoch PES that already
			// started, and it re-epochs the following ones only after it is
			// fully read.
			w2.writePSI(0x100, makePMT(1, 0x101, []byte{0x05, 0x04, 0x48, 0x44, 0x54, 0x56}, []esDeclaration{{0x1b, 0x101, nil}}))
		}
		raw2 := w2.bytes()
		d3 := astits.NewDemuxer(context.Background(), bytes.NewReader(raw2), astits.DemuxerOptPacketSize(188))
		idx3, err := d3.BuildSeekIndex()
		c.Check(err == nil, "d3 index: %v", err)
		if idx3 != nil {
			points := idx3.Points()
			c.Check(len(points) == 3, "d3 points: %v", points)
			for _, p := range points[:1] {
				c.Check(p.Epoch == 0, "first PMT should be epoch 0: %+v", p)
			}
			// Each distinct PMT topology creates a new epoch for PES that
			// start after it, and the PES that started before the second PMT
			// was read stays on the first epoch.
			byEpoch := map[uint64]int{}
			for _, p := range points {
				byEpoch[p.Epoch]++
			}
			c.Check(len(byEpoch) == 2, "expected two epochs, got %v (%v)", byEpoch, points)
			c.Check(byEpoch[0] == 1, "only the first PES should be epoch 0: %v", points)
			c.Check(byEpoch[1] == 2, "later PES should use epoch 1: %v", points)
		}

		// A second index built after the first one is independent.
		again, err := d3.BuildSeekIndex()
		c.Check(err == nil, "rebuild: %v", err)
		if again != nil && idx3 != nil {
			a := again.Points()
			b := idx3.Points()
			if len(a) > 0 {
				a[0].PTS90k = -12345
			}
			c.Check(len(b) > 0 && b[0].PTS90k != -12345, "rebuild aliased previous index")
		}
	})
}
