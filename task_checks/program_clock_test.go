package task_checks

import (
	"bytes"
	"context"
	"encoding/binary"
	"fmt"
	"sort"
	"testing"

	astits "github.com/asticode/go-astits"
	"github.com/asticode/go-astits/task_support/taskcheck"
)

/*
Hand-written 188 byte transport stream builder with an independently
implemented MPEG-2 PSI CRC32 (poly 0x04C11DB7, init 0xFFFFFFFF).
The tests only read the stream back through the public Demuxer entry point.
*/

// ---------- independent CRC ----------

func tcCRC32(bs []byte) uint32 {
	crc := uint32(0xFFFFFFFF)
	for _, b := range bs {
		crc ^= uint32(b) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = (crc << 1) ^ 0x04C11DB7
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

// ---------- low level TS builder ----------

type tcPCR struct {
	base uint64
	ext  uint16
}

type tcTS struct {
	b  bytes.Buffer
	cc map[uint16]byte
}

func newTcTS() *tcTS {
	return &tcTS{cc: map[uint16]byte{}}
}

func (t *tcTS) bytes() []byte { return t.b.Bytes() }

// emit writes one exact 188 byte TS packet. payload nil/empty means an
// adaptation-field-only packet. Continuity counters only increment for
// packets carrying a payload.
func (t *tcTS) emit(pid uint16, pusi bool, payload []byte, pcr *tcPCR, disc bool) {
	n := len(payload)
	if n > 184 {
		panic(fmt.Sprintf("payload %d does not fit one TS packet", n))
	}
	cc := t.cc[pid] & 0x0f
	if n > 0 {
		t.cc[pid] = (cc + 1) & 0x0f
	}

	pkt := make([]byte, 0, 188)
	b1 := byte(pid>>8) & 0x1f
	if pusi {
		b1 |= 0x40
	}
	b3 := cc
	L := 183 - n // adaptation field length following the length byte
	explicitAF := pcr != nil || disc
	hasAF := explicitAF || n < 184
	if hasAF {
		b3 |= 0x20
	}
	if n > 0 {
		b3 |= 0x10
	}
	pkt = append(pkt, 0x47, b1, byte(pid), b3)

	if hasAF {
		pkt = append(pkt, byte(L))
		if L > 0 {
			flags := byte(0)
			if disc {
				flags |= 0x80
			}
			if pcr != nil {
				flags |= 0x10
			}
			used := 1 // flags byte
			pkt = append(pkt, flags)
			if pcr != nil {
				v := (pcr.base << 15) | (uint64(0x3f) << 9) | uint64(pcr.ext)
				pkt = append(pkt,
					byte(v>>40), byte(v>>32), byte(v>>24),
					byte(v>>16), byte(v>>8), byte(v),
				)
				used += 6
			}
			if used > L {
				panic("adaptation field does not fit")
			}
			for i := used; i < L; i++ {
				pkt = append(pkt, 0xff)
			}
		}
	}

	pkt = append(pkt, payload...)
	if len(pkt) != 188 {
		panic(fmt.Sprintf("bad packet length %d", len(pkt)))
	}
	t.b.Write(pkt)
}

func (t *tcTS) pcrOnly(pid uint16, base uint64, ext uint16) {
	t.emit(pid, false, nil, &tcPCR{base: base, ext: ext}, false)
}

func (t *tcTS) discOnly(pid uint16) {
	t.emit(pid, false, nil, nil, true)
}

func (t *tcTS) discPCR(pid uint16, base uint64, ext uint16) {
	t.emit(pid, false, nil, &tcPCR{base: base, ext: ext}, true)
}

// ---------- PSI ----------

// tcSection builds a current, version 0, single section with legal CRC.
func tcSection(tableID byte, extension uint16, body []byte) []byte {
	s := []byte{tableID, 0xB0, 0x00}
	s = append(s, byte(extension>>8), byte(extension))
	s = append(s, 0xC1, 0x00, 0x00) // version 0, current_next=1
	s = append(s, body...)
	s = append(s, 0x00, 0x00, 0x00, 0x00)
	sectionLength := len(s) - 3
	if sectionLength > 0x0fff {
		panic("section too long")
	}
	s[1] = 0xB0 | byte(sectionLength>>8)
	s[2] = byte(sectionLength)
	binary.BigEndian.PutUint32(s[len(s)-4:], tcCRC32(s[:len(s)-4]))
	return s
}

func (t *tcTS) pat(programs map[uint16]uint16) {
	pns := make([]int, 0, len(programs))
	for pn := range programs {
		pns = append(pns, int(pn))
	}
	sort.Ints(pns)
	var body []byte
	for _, v := range pns {
		pn := uint16(v)
		pmtPID := programs[pn]
		body = append(body, byte(pn>>8), byte(pn))
		body = append(body, 0xE0|byte(pmtPID>>8)&0x1f, byte(pmtPID))
	}
	sec := tcSection(0x00, 0x0001, body)
	t.emit(0x0000, true, append([]byte{0x00}, sec...), nil, false)
}

type tcESpec struct {
	pid  uint16
	typ  byte
	lang string // when non empty, an ISO 639 language descriptor is added
}

func (t *tcTS) pmt(programNumber, pmtPID, pcrPID uint16, streams []tcESpec) {
	body := []byte{0xE0 | byte(pcrPID>>8)&0x1f, byte(pcrPID)}
	body = append(body, 0xF0, 0x00) // program info length 0
	for _, es := range streams {
		body = append(body, es.typ)
		body = append(body, 0xE0|byte(es.pid>>8)&0x1f, byte(es.pid))
		var info []byte
		if es.lang != "" {
			if len(es.lang) != 3 {
				panic("lang must be 3 chars")
			}
			info = []byte{0x0a, 0x04, es.lang[0], es.lang[1], es.lang[2], 0x00}
		}
		body = append(body, 0xF0|byte(len(info)>>8)&0x0f, byte(len(info)))
		body = append(body, info...)
	}
	sec := tcSection(0x02, programNumber, body)
	t.emit(pmtPID, true, append([]byte{0x00}, sec...), nil, false)
}

// ---------- PES ----------

func tcEncode90k(v uint64, first byte) []byte {
	return []byte{
		first | byte((v>>30)&0x07)<<1 | 0x01,
		byte(v >> 22),
		byte((v>>14)&0xFE | 0x01),
		byte(v >> 7),
		byte((v<<1)&0xFE | 0x01),
	}
}

// tcPES builds a PES packet. unbounded sets PES_packet_length to zero.
func tcPES(streamID byte, payload []byte, pts, dts *uint64, unbounded bool) []byte {
	var opt []byte
	flags := byte(0)
	switch {
	case pts != nil && dts != nil:
		flags = 0xC0
		opt = append(opt, tcEncode90k(*pts, 0x30)...)
		opt = append(opt, tcEncode90k(*dts, 0x10)...)
	case pts != nil:
		flags = 0x80
		opt = append(opt, tcEncode90k(*pts, 0x20)...)
	}
	pesLength := 3 + len(opt) + len(payload)
	if unbounded {
		pesLength = 0
	}
	out := []byte{0x00, 0x00, 0x01, streamID, byte(pesLength >> 8), byte(pesLength)}
	out = append(out, 0x80, flags, byte(len(opt)))
	out = append(out, opt...)
	out = append(out, payload...)
	return out
}

// emitPESBytes packetizes a complete PES in chunks of at most chunk bytes.
func (t *tcTS) emitPESBytes(pid uint16, pes []byte, chunk int) {
	first := true
	for len(pes) > 0 {
		n := chunk
		if n > len(pes) {
			n = len(pes)
		}
		t.emit(pid, first, pes[:n], nil, false)
		pes = pes[n:]
		first = false
	}
}

// emitContinuation packetizes bytes that follow an already emitted PUSI start.
func (t *tcTS) emitContinuation(pid uint16, data []byte, chunk int) {
	for len(data) > 0 {
		n := chunk
		if n > len(data) {
			n = len(data)
		}
		t.emit(pid, false, data[:n], nil, false)
		data = data[n:]
	}
}

func tcPayload(seed byte, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = seed + byte(i%251)
	}
	return b
}

func u64(v uint64) *uint64 { return &v }

func i64(v int64) *int64 { return &v }

// ---------- reading ----------

func tcReadAll(c *taskcheck.C, dmx *astits.Demuxer) []*astits.TimedData {
	var out []*astits.TimedData
	for {
		td, err := dmx.NextTimedData()
		if err == astits.ErrNoMorePackets {
			break
		}
		if err != nil {
			c.Check(false, "NextTimedData returned error: %v", err)
			break
		}
		out = append(out, td)
	}
	return out
}

func tcPESByPayload(ds []*astits.TimedData) map[string]*astits.TimedData {
	m := map[string]*astits.TimedData{}
	for _, td := range ds {
		if td != nil && td.Data != nil && td.Data.PES != nil {
			m[string(td.Data.PES.Data)] = td
		}
	}
	return m
}

type tcWantPES struct {
	payload []byte
	pn      uint16
	epoch   uint64
	pcr     *int64
	pts     *int64
	dts     *int64
}

func tcExpectPES(c *taskcheck.C, by map[string]*astits.TimedData, w tcWantPES) *astits.TimedData {
	td, ok := by[string(w.payload)]
	c.Check(ok, "missing PES for payload %x", w.payload)
	if !ok {
		return nil
	}
	c.Check(td.Data != nil && td.Data.PES != nil, "PES data is nil")
	c.Check(bytes.Equal(td.Data.PES.Data, w.payload),
		"payload mismatch: got %x want %x", td.Data.PES.Data, w.payload)
	c.Check(td.ProgramNumber == w.pn,
		"ProgramNumber=%d want %d (payload %x)", td.ProgramNumber, w.pn, w.payload[:min(4, len(w.payload))])
	c.Check(td.ClockEpoch == w.epoch,
		"ClockEpoch=%d want %d (payload %x)", td.ClockEpoch, w.epoch, w.payload[:min(4, len(w.payload))])
	tcCheckPtr(c, td.PCR27MHz, w.pcr, "PCR27MHz", w.payload)
	tcCheckPtr(c, td.PTS90k, w.pts, "PTS90k", w.payload)
	tcCheckPtr(c, td.DTS90k, w.dts, "DTS90k", w.payload)
	return td
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func tcCheckPtr(c *taskcheck.C, got *int64, want *int64, label string, payload []byte) {
	if want == nil {
		c.Check(got == nil, "%s should be nil, got %d (payload %x)", label, tcPtrVal(got), payload[:min(4, len(payload))])
		return
	}
	c.Check(got != nil, "%s should be non nil (payload %x)", label, payload[:min(4, len(payload))])
	if got == nil {
		return
	}
	c.Check(*got == *want, "%s=%d want %d (payload %x)", label, *got, *want, payload[:min(4, len(payload))])
}

func tcPtrVal(p *int64) int64 {
	if p == nil {
		return 0
	}
	return *p
}

// tcSignature is a comparable projection used across Rewind re-reads.
type tcSignature struct {
	kind                   string
	pid, programNumber     uint16
	epoch                  uint64
	hasPCR, hasPTS, hasDTS bool
	pcr, pts, dts          int64
	payload                string
	pmtPCRPID              uint16
}

func tcSig(td *astits.TimedData) tcSignature {
	s := tcSignature{
		pid:           td.Data.PID,
		programNumber: td.ProgramNumber,
		epoch:         td.ClockEpoch,
		hasPCR:        td.PCR27MHz != nil,
		hasPTS:        td.PTS90k != nil,
		hasDTS:        td.DTS90k != nil,
	}
	if td.Data.PES != nil {
		s.payload = string(td.Data.PES.Data)
	}
	if td.PCR27MHz != nil {
		s.pcr = *td.PCR27MHz
	}
	if td.PTS90k != nil {
		s.pts = *td.PTS90k
	}
	if td.DTS90k != nil {
		s.dts = *td.DTS90k
	}
	switch {
	case td.Data.PAT != nil:
		s.kind = "pat"
	case td.Data.PMT != nil:
		s.kind = "pmt"
		s.pmtPCRPID = td.Data.PMT.PCRPID
	case td.Data.PES != nil:
		s.kind = "pes"
	default:
		s.kind = "other"
	}
	return s
}

const tcM33 = int64(1) << 33

// TestProgramClockWrap combines two interleaved programs with independent PCR
// PIDs, PCR extensions, PTS/DTS crossing the 33 bit boundary and B-frame style
// backwards PTS ordering.
func TestProgramClockWrap(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts := newTcTS()

		const (
			pn1     = uint16(101)
			pmtPID1 = uint16(0x1001)
			pcrPID1 = uint16(0x1101)
			esPID1  = uint16(0x0101)
			pn2     = uint16(202)
			pmtPID2 = uint16(0x1002)
			pcrPID2 = uint16(0x1102)
			esPID2  = uint16(0x0202)
		)

		// PAT and two PMTs
		ts.pat(map[uint16]uint16{pn1: pmtPID1, pn2: pmtPID2})
		ts.pmt(pn1, pmtPID1, pcrPID1, []tcESpec{{pid: esPID1, typ: 0x1b}})
		ts.pmt(pn2, pmtPID2, pcrPID2, []tcESpec{{pid: esPID2, typ: 0x0f}})

		// Program 1 PCR sits just below the 33 bit base wrap, with extension.
		ts.pcrOnly(pcrPID1, uint64(tcM33-100), 37)
		// Interleave program 2 PCR well before its frames.
		ts.pcrOnly(pcrPID2, 1000, 270)

		pA := tcPayload(0xA1, 200)
		pesA := tcPES(0xe0, pA, u64(50), u64(40), false)
		// Alternate chunks with other program traffic to prove per-PES snapshot.
		ts.emit(esPID1, true, pesA[:60], nil, false)
		pD := tcPayload(0xD4, 40)
		ts.emitPESBytes(esPID2, tcPES(0xc0, pD, u64(1005), nil, false), 30)
		ts.emitContinuation(esPID1, pesA[60:], 60)

		// Program 1 PCR crosses the zero boundary forward.
		ts.pcrOnly(pcrPID1, 120, 250)

		pE := tcPayload(0xE5, 16)
		ts.emitPESBytes(esPID2, tcPES(0xc0, pE, nil, nil, false), 30)

		pB := tcPayload(0xB2, 130)
		ts.emitPESBytes(esPID1, tcPES(0xe0, pB, u64(130), u64(120), false), 55)

		// Program 2 PCR exactly half a circle away from PTS raw 0.
		ts.pcrOnly(pcrPID2, uint64(1)<<32, 0)

		// B frame: PTS goes backwards relative to previous PTS on purpose.
		pC := tcPayload(0xC3, 90)
		ts.emitPESBytes(esPID1, tcPES(0xe0, pC, u64(115), u64(150), false), 40)

		pF := tcPayload(0xF6, 12)
		ts.emitPESBytes(esPID2, tcPES(0xc0, pF, u64(0), nil, false), 30)

		dmx := astits.NewDemuxer(context.Background(), bytes.NewReader(ts.bytes()), astits.DemuxerOptPacketSize(188))
		all := tcReadAll(c, dmx)
		by := tcPESByPayload(all)

		// PAT and both PMTs are delivered with nil time pointers.
		var pat, pmt1, pmt2 *astits.TimedData
		for _, td := range all {
			switch {
			case td.Data.PAT != nil:
				pat = td
			case td.Data.PMT != nil && td.Data.PMT.ProgramNumber == pn1:
				pmt1 = td
			case td.Data.PMT != nil && td.Data.PMT.ProgramNumber == pn2:
				pmt2 = td
			}
		}
		c.Check(pat != nil, "PAT not delivered")
		c.Check(pmt1 != nil && pmt1.ProgramNumber == pn1, "PMT1 not delivered with program number")
		c.Check(pmt2 != nil && pmt2.ProgramNumber == pn2, "PMT2 not delivered with program number")
		if pat != nil {
			c.Check(pat.PCR27MHz == nil && pat.PTS90k == nil && pat.DTS90k == nil, "PAT time pointers must be nil")
		}
		if pmt1 != nil {
			c.Check(pmt1.PCR27MHz == nil && pmt1.PTS90k == nil && pmt1.DTS90k == nil, "PMT time pointers must be nil")
			c.Check(pmt1.Data.PMT.PCRPID == pcrPID1, "PMT1 PCR PID mismatch")
		}

		pcrA := (tcM33-100)*300 + 37
		pcrB := (tcM33+120)*300 + 250
		tcExpectPES(c, by, tcWantPES{payload: pA, pn: pn1, epoch: 0, pcr: i64(pcrA), pts: i64(tcM33 + 50), dts: i64(tcM33 + 40)})
		tcExpectPES(c, by, tcWantPES{payload: pB, pn: pn1, epoch: 0, pcr: i64(pcrB), pts: i64(tcM33 + 130), dts: i64(tcM33 + 120)})
		// Backwards PTS is expanded independently against the anchor.
		tcExpectPES(c, by, tcWantPES{payload: pC, pn: pn1, epoch: 0, pcr: i64(pcrB), pts: i64(tcM33 + 115), dts: i64(tcM33 + 150)})

		tcExpectPES(c, by, tcWantPES{payload: pD, pn: pn2, epoch: 0, pcr: i64(1000*300 + 270), pts: i64(1005), dts: nil})
		// PES without any timestamp still carries the PCR anchor.
		tcExpectPES(c, by, tcWantPES{payload: pE, pn: pn2, epoch: 0, pcr: i64(1000*300 + 270), pts: nil, dts: nil})
		// Exact half circle tie selects the smaller expanded value (0, not 2^33).
		tcExpectPES(c, by, tcWantPES{payload: pF, pn: pn2, epoch: 0, pcr: i64(int64(1) << 32 * 300), pts: i64(0), dts: nil})

		// Every PES payload must be present exactly once.
		c.Check(len(by) == 6, "expected 6 distinct PES, got %d", len(by))
	})
}

// TestProgramClockSnapshot chains a PCR update mid PES, a PCR PID change, a
// payload-less discontinuity, a missing anchor and Rewind, and verifies the
// context captured at the first PES packet never follows later clock state.
func TestProgramClockSnapshot(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts := newTcTS()

		const (
			pn      = uint16(7)
			pmtPID  = uint16(0x1007)
			pcrPID0 = uint16(0x1107)
			// After the PMT change the PCR rides the elementary PID itself, so a
			// PES first packet may carry the PCR it must snapshot.
			pcrPID1 = uint16(0x0107)
			esPID   = uint16(0x0107)
			esPID2  = uint16(0x0108)
		)

		ts.pat(map[uint16]uint16{pn: pmtPID})
		ts.pmt(pn, pmtPID, pcrPID0, []tcESpec{
			{pid: esPID, typ: 0x1b},
			{pid: esPID2, typ: 0x1b},
		})

		// Anchor for the cross-packet PES X.
		ts.pcrOnly(pcrPID0, 50000, 100)
		pcrX := int64(50000)*300 + 100

		px := tcPayload(0x71, 120)
		pesX := tcPES(0xe0, px, u64(50100), u64(50050), false)
		c.Check(len(pesX) > 70, "PES X test setup too short")
		ts.emit(esPID, true, pesX[:30], nil, false)

		// PCR update while X is only half assembled.
		ts.pcrOnly(pcrPID0, 50020, 0)

		// PMT changes the PCR PID: new epoch, old anchor cleared.
		ts.pmt(pn, pmtPID, pcrPID1, []tcESpec{
			{pid: esPID, typ: 0x1b},
			{pid: esPID2, typ: 0x1b},
		})

		ts.emit(esPID, false, pesX[30:70], nil, false)

		// Payload-less discontinuity on the new PCR PID starts another epoch.
		ts.discOnly(pcrPID1)

		// Finish X: it must keep epoch 0 and its original anchor.
		ts.emit(esPID, false, pesX[70:], nil, false)

		// New PES Y in epoch 2 has no PCR anchor: all time pointers stay nil.
		py := []byte("YY-missing-anchor")
		ts.emitPESBytes(esPID, tcPES(0xe0, py, u64(60000), nil, false), 40)

		// PES Z carries its own PCR on its first packet: apply then snapshot.
		pz := tcPayload(0x72, 80)
		pesZ := tcPES(0xe0, pz, u64(70050), nil, false)
		ts.emit(esPID, true, pesZ[:40], &tcPCR{base: 70000, ext: 0}, false)
		// Mid Z PCR update must not move Z's anchor.
		ts.pcrOnly(pcrPID1, 70100, 0)
		ts.emitContinuation(esPID, pesZ[40:], 40)

		// Unbounded (zero length) video PES U delivered at the next PUSI.
		ts.pcrOnly(pcrPID1, 80000, 150)
		pcrU := int64(80000)*300 + 150
		pu := tcPayload(0x73, 60)
		pesU := tcPES(0xe0, pu, u64(80010), nil, true)
		ts.emit(esPID, true, pesU[:25], nil, false)
		ts.emit(esPID, false, pesU[25:], nil, false)

		// Next start flushes U and snapshots V against the same anchor.
		pv := []byte("VV-next-start")
		pesV := tcPES(0xe0, pv, u64(80020), nil, false)
		ts.emit(esPID, true, pesV, nil, false)

		// Zero length PES W on another ES PID only delivered at EOF.
		pw := []byte("WW-eof-zero")
		pesW := tcPES(0xe0, pw, u64(80030), nil, true)
		ts.emitPESBytes(esPID2, pesW, 20)
		// Later PCR must not change W's snapshot either.
		ts.pcrOnly(pcrPID1, 81000, 0)

		dmx := astits.NewDemuxer(context.Background(), bytes.NewReader(ts.bytes()), astits.DemuxerOptPacketSize(188))
		first := tcReadAll(c, dmx)
		by := tcPESByPayload(first)

		tdX := tcExpectPES(c, by, tcWantPES{payload: px, pn: pn, epoch: 0, pcr: i64(pcrX), pts: i64(50100), dts: i64(50050)})
		tcExpectPES(c, by, tcWantPES{payload: py, pn: pn, epoch: 2, pcr: nil, pts: nil, dts: nil})
		tcExpectPES(c, by, tcWantPES{payload: pz, pn: pn, epoch: 2, pcr: i64(70000 * 300), pts: i64(70050), dts: nil})
		tcExpectPES(c, by, tcWantPES{payload: pu, pn: pn, epoch: 2, pcr: i64(pcrU), pts: i64(80010), dts: nil})
		tcExpectPES(c, by, tcWantPES{payload: pv, pn: pn, epoch: 2, pcr: i64(pcrU), pts: i64(80020), dts: nil})
		tcExpectPES(c, by, tcWantPES{payload: pw, pn: pn, epoch: 2, pcr: i64(pcrU), pts: i64(80030), dts: nil})

		// After everything has been read, X still owns its original values.
		if tdX != nil {
			c.Check(tdX.ClockEpoch == 0, "X epoch changed to %d", tdX.ClockEpoch)
			c.Check(tdX.PCR27MHz != nil && *tdX.PCR27MHz == pcrX, "X PCR anchor mutated")
			c.Check(tdX.PTS90k != nil && *tdX.PTS90k == 50100, "X PTS mutated")
			c.Check(tdX.DTS90k != nil && *tdX.DTS90k == 50050, "X DTS mutated")
		}

		// Project the first pass.
		sigs1 := make([]tcSignature, 0, len(first))
		for _, td := range first {
			sigs1 = append(sigs1, tcSig(td))
		}
		c.Check(len(sigs1) >= 9, "expected PAT+2PMT+6PES, got %d", len(sigs1))

		// Rewind and re-read the exact same stream: results must be identical.
		n, err := dmx.Rewind()
		c.Check(err == nil, "Rewind error: %v", err)
		c.Check(n == 0, "Rewind offset=%d want 0", n)
		second := tcReadAll(c, dmx)
		c.Check(len(second) == len(first), "rewind read count %d != first pass %d", len(second), len(first))
		if len(second) == len(first) {
			for i := range first {
				s1, s2 := tcSig(first[i]), tcSig(second[i])
				c.Check(s1 == s2, "rewind mismatch at #%d: %+v != %+v", i, s2, s1)
			}
		}

		// Re-assert X once more after the full re-read.
		if tdX != nil {
			c.Check(tdX.PCR27MHz != nil && *tdX.PCR27MHz == pcrX, "X PCR changed after rewind read")
		}

		// A PES with no PAT/PMT at all is delivered with program 0 and nil times.
		tsUnknown := newTcTS()
		pu0 := []byte("no-program")
		tsUnknown.emitPESBytes(0x0900, tcPES(0xe0, pu0, u64(123), u64(120), false), 40)
		dmxU := astits.NewDemuxer(context.Background(), bytes.NewReader(tsUnknown.bytes()), astits.DemuxerOptPacketSize(188))
		unk := tcReadAll(c, dmxU)
		c.Check(len(unk) == 1, "unknown program stream should yield 1 PES, got %d", len(unk))
		if len(unk) == 1 {
			u := unk[0]
			c.Check(u.Data != nil && u.Data.PES != nil && bytes.Equal(u.Data.PES.Data, pu0), "unknown PES payload mismatch")
			c.Check(u.ProgramNumber == 0 && u.ClockEpoch == 0, "unknown PES context should be zero value")
			c.Check(u.PCR27MHz == nil && u.PTS90k == nil && u.DTS90k == nil, "unknown PES time pointers must be nil")
		}
	})
}

// TestProgramClockEpoch checks per program epoch rules: initial epoch 0,
// discontinuity indicators on the current PCR PID (with or without payload),
// isolation from other PIDs/programs, PMT PCR PID changes, stability under
// repeated or descriptor-only PMTs, and PCR PID 0x1fff meaning no anchor.
func TestProgramClockEpoch(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ts := newTcTS()

		const (
			pn1     = uint16(11)
			pmtPID1 = uint16(0x2000)
			pcrA    = uint16(0x2100)
			pcrB    = uint16(0x2200)
			esPID1  = uint16(0x0111)
			pn2     = uint16(22)
			pmtPID2 = uint16(0x2002)
			pcrPID2 = uint16(0x2300)
			esPID2  = uint16(0x0222)
		)
		es1 := []tcESpec{{pid: esPID1, typ: 0x1b}}

		ts.pat(map[uint16]uint16{pn1: pmtPID1, pn2: pmtPID2})
		ts.pmt(pn1, pmtPID1, pcrA, es1)
		ts.pmt(pn2, pmtPID2, pcrPID2, []tcESpec{{pid: esPID2, typ: 0x0f}})

		step := 0
		frame := func() []byte {
			step++
			return []byte(fmt.Sprintf("E%02d", step))
		}
		sendFrame := func(payload []byte, pts uint64) {
			ts.emitPESBytes(esPID1, tcPES(0xe0, payload, &pts, nil, false), 40)
		}

		// Epoch 0 with a PCR anchor.
		ts.pcrOnly(pcrA, 1000, 0)
		e1 := frame()
		sendFrame(e1, 1000)

		// Payload-less discontinuity on current PCR PID -> epoch 1, no anchor.
		ts.discOnly(pcrA)
		e2 := frame()
		sendFrame(e2, 1100)

		// A fresh PCR seeds epoch 1.
		ts.pcrOnly(pcrA, 1200, 0)
		e3 := frame()
		sendFrame(e3, 1200)

		// Discontinuity and a new PCR in the very same adaptation-only packet:
		// epoch 2 first, then the PCR seeds it.
		ts.discPCR(pcrA, 1300, 0)
		e4 := frame()
		sendFrame(e4, 1300)

		// Discontinuity on an unrelated PID must not move program 11's epoch.
		ts.discOnly(0x2999)
		e5 := frame()
		sendFrame(e5, 1301)

		// Discontinuity on program 22's PCR PID does not affect program 11.
		ts.discOnly(pcrPID2)
		e6 := frame()
		sendFrame(e6, 1302)

		// Repeated identical PMT keeps epoch 2.
		ts.pmt(pn1, pmtPID1, pcrA, es1)
		e7 := frame()
		sendFrame(e7, 1303)

		// Descriptor-only PMT change with the same PCR PID keeps epoch 2.
		es1Desc := []tcESpec{{pid: esPID1, typ: 0x1b, lang: "eng"}}
		ts.pmt(pn1, pmtPID1, pcrA, es1Desc)
		e8 := frame()
		sendFrame(e8, 1304)

		// PCR PID change starts epoch 3 and clears the anchor.
		ts.pmt(pn1, pmtPID1, pcrB, es1Desc)
		e9 := frame()
		sendFrame(e9, 1400)

		// PCR on the new PID anchors epoch 3.
		ts.pcrOnly(pcrB, 2000, 0)
		e10 := frame()
		sendFrame(e10, 2000)

		// PCR PID 0x1fff means no anchor and starts epoch 4.
		ts.pmt(pn1, pmtPID1, 0x1fff, es1Desc)
		// Even a PCR carried on PID 0x1fff must not fill the anchor.
		ts.pcrOnly(0x1fff, 5, 0)
		e11 := frame()
		sendFrame(e11, 2100)

		// Program 22 rebuilds its own epoch 1 anchor independently.
		ts.pcrOnly(pcrPID2, 900, 0)
		e22 := []byte("P22")
		ts.emitPESBytes(esPID2, tcPES(0xc0, e22, u64(900), nil, false), 40)

		dmx := astits.NewDemuxer(context.Background(), bytes.NewReader(ts.bytes()), astits.DemuxerOptPacketSize(188))
		all := tcReadAll(c, dmx)
		by := tcPESByPayload(all)

		tcExpectPES(c, by, tcWantPES{payload: e1, pn: pn1, epoch: 0, pcr: i64(300000), pts: i64(1000)})
		tcExpectPES(c, by, tcWantPES{payload: e2, pn: pn1, epoch: 1, pcr: nil, pts: nil})
		tcExpectPES(c, by, tcWantPES{payload: e3, pn: pn1, epoch: 1, pcr: i64(360000), pts: i64(1200)})
		tcExpectPES(c, by, tcWantPES{payload: e4, pn: pn1, epoch: 2, pcr: i64(390000), pts: i64(1300)})
		tcExpectPES(c, by, tcWantPES{payload: e5, pn: pn1, epoch: 2, pcr: i64(390000), pts: i64(1301)})
		tcExpectPES(c, by, tcWantPES{payload: e6, pn: pn1, epoch: 2, pcr: i64(390000), pts: i64(1302)})
		tcExpectPES(c, by, tcWantPES{payload: e7, pn: pn1, epoch: 2, pcr: i64(390000), pts: i64(1303)})
		tcExpectPES(c, by, tcWantPES{payload: e8, pn: pn1, epoch: 2, pcr: i64(390000), pts: i64(1304)})
		tcExpectPES(c, by, tcWantPES{payload: e9, pn: pn1, epoch: 3, pcr: nil, pts: nil})
		tcExpectPES(c, by, tcWantPES{payload: e10, pn: pn1, epoch: 3, pcr: i64(600000), pts: i64(2000)})
		tcExpectPES(c, by, tcWantPES{payload: e11, pn: pn1, epoch: 4, pcr: nil, pts: nil})
		tcExpectPES(c, by, tcWantPES{payload: e22, pn: pn2, epoch: 1, pcr: i64(270000), pts: i64(900)})

		// Rewind resets epochs: first frame is epoch 0 anchored again.
		_, err := dmx.Rewind()
		c.Check(err == nil, "Rewind error: %v", err)
		all2 := tcReadAll(c, dmx)
		by2 := tcPESByPayload(all2)
		tcExpectPES(c, by2, tcWantPES{payload: e1, pn: pn1, epoch: 0, pcr: i64(300000), pts: i64(1000)})
		tcExpectPES(c, by2, tcWantPES{payload: e11, pn: pn1, epoch: 4, pcr: nil, pts: nil})
	})
}
