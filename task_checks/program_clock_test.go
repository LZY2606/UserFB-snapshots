package task_checks

import (
	"bytes"
	"context"
	"testing"

	"github.com/asticode/go-astits"
	"github.com/asticode/go-astits/task_support/taskcheck"
)

// This file builds small transport streams out of hand-written 188-byte
// packets. PSI CRCs are computed independently below with a bit-by-bit
// MPEG-2 CRC implementation instead of the library's table-driven one, and
// every expected clock value is derived from the spec formulas in the test
// itself rather than read back from library internals.

const (
	mod33 = int64(1) << 33
	mod27 = mod33 * 300
)

// ---- independent reference formulas (ISO/IEC 13818-1 clock unwrapping) ----

func refUnwrapPCR(prev *int64, raw int64) int64 {
	if prev == nil {
		return raw
	}
	prevMod := *prev % mod27
	if prevMod < 0 {
		prevMod += mod27
	}
	d := raw - prevMod
	if d > mod27/2 {
		d -= mod27
	}
	if d < -mod27/2 {
		d += mod27
	}
	return *prev + d
}

func refUnwrap90k(raw, anchor90 int64) int64 {
	d := (raw - anchor90) % mod33
	if d >= mod33/2 { // exact half circle: keep the smaller value
		d -= mod33
	}
	if d < -mod33/2 {
		d += mod33
	}
	return anchor90 + d
}

func pcr27Value(base, ext int64) int64 { return base*300 + ext }

func intPtr(v int64) *int64 { return &v }

// psiCRC32 computes the MPEG-2 PSI CRC-32 (poly 0x04C11DB7, init 0xFFFFFFFF,
// no reflection, no final XOR) one bit at a time.
func psiCRC32(data []byte) uint32 {
	crc := uint32(0xffffffff)
	for _, b := range data {
		crc ^= uint32(b) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = (crc << 1) ^ 0x04c11db7
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

// ---- raw 188-byte TS packet construction ----

type streamBuilder struct {
	buf bytes.Buffer
	cc  map[uint16]uint8
}

func newStreamBuilder() *streamBuilder {
	return &streamBuilder{cc: make(map[uint16]uint8)}
}

// packet writes one 188-byte packet. af is the full adaptation field including
// its leading length byte; nil means no adaptation field.
func (s *streamBuilder) packet(pid uint16, pusi bool, af, payload []byte) {
	if af == nil && payload == nil {
		panic("packet without adaptation field or payload")
	}
	if len(af)+len(payload) > 184 {
		panic("packet overflow")
	}
	afc := byte(0)
	switch {
	case af != nil && payload != nil:
		afc = 3
	case af != nil:
		afc = 2
	default:
		afc = 1
	}
	cc := s.cc[pid]
	s.cc[pid] = (cc + 1) & 0x0f

	b0 := byte(pid>>8) & 0x1f
	if pusi {
		b0 |= 0x40
	}
	pkt := make([]byte, 0, 188)
	pkt = append(pkt, 0x47, b0, byte(pid), afc<<4|cc)
	pkt = append(pkt, af...)
	pkt = append(pkt, payload...)
	for len(pkt) < 188 {
		pkt = append(pkt, 0xff)
	}
	s.buf.Write(pkt)
}

// afPCR builds an adaptation field carrying a PCR, optionally with the
// discontinuity indicator set.
func afPCR(discontinuity bool, base, ext int64) []byte {
	flags := byte(0x10) // PCR flag
	if discontinuity {
		flags |= 0x80
	}
	v := (uint64(base&0x1ffffffff) << 15) | (uint64(0x3f) << 9) | uint64(ext&0x1ff)
	content := []byte{
		flags,
		byte(v >> 40), byte(v >> 32), byte(v >> 24),
		byte(v >> 16), byte(v >> 8), byte(v),
	}
	return append([]byte{byte(len(content))}, content...)
}

// afDiscontinuity builds an adaptation field with only the discontinuity flag.
func afDiscontinuity() []byte {
	return []byte{1, 0x80}
}

// ---- PSI section construction ----

func psiSection(tableID byte, syntax []byte, version uint8, tableIDExt uint16) []byte {
	body := []byte{
		byte(tableIDExt >> 8), byte(tableIDExt),
		0xc1 | ((version & 0x1f) << 1), // reserved 11, current=1
		0x00, 0x00,                     // section number / last section number
	}
	body = append(body, syntax...)
	sectionLength := len(body) + 4 // + CRC32
	head := []byte{
		tableID,
		0xb0 | byte(sectionLength>>8),
		byte(sectionLength),
	}
	section := append(head, body...)
	crc := psiCRC32(section)
	return append(section, byte(crc>>24), byte(crc>>16), byte(crc>>8), byte(crc))
}

// buildPAT builds a PAT section mapping program numbers to PMT PIDs.
func buildPAT(version uint8, tsid uint16, programs map[uint16]uint16) []byte {
	syntax := make([]byte, 0, 4*len(programs))
	// Stable order keeps the generated stream deterministic.
	pnrs := sortedKeys(programs)
	for _, pnr := range pnrs {
		pmtPID := programs[pnr]
		syntax = append(syntax, byte(pnr>>8), byte(pnr),
			byte(0xe0|(pmtPID>>8)&0x1f), byte(pmtPID))
	}
	return psiSection(0x00, syntax, version, tsid)
}

// buildPMT builds a single-section PMT.
func buildPMT(version, pnr, pcrPID uint16, streams [][2]uint16, progDesc []byte) []byte {
	syntax := []byte{
		byte(0xe0 | (pcrPID>>8)&0x1f), byte(pcrPID),
		byte(0xf0 | (len(progDesc) >> 8)), byte(len(progDesc)),
	}
	syntax = append(syntax, progDesc...)
	for _, st := range streams {
		streamType, esPID := st[0], st[1]
		syntax = append(syntax, byte(streamType),
			byte(0xe0|(esPID>>8)&0x1f), byte(esPID),
			0xf0, 0x00)
	}
	return psiSection(0x02, syntax, uint8(version), pnr)
}

func (s *streamBuilder) writeSection(pid uint16, section []byte) {
	payload := append([]byte{0x00}, section...) // pointer field = 0
	s.packet(pid, true, nil, payload)
}

// ---- PES construction ----

func timestampBytes(flag byte, v int64) []byte {
	return []byte{
		(flag << 4) | byte((v>>30)&0x07)<<1 | 0x01,
		byte((v >> 22) & 0xff),
		byte((v>>14)&0xfe) | 0x01,
		byte((v >> 7) & 0xff),
		byte((v<<1)&0xfe) | 0x01,
	}
}

// buildPES assembles a PES packet. When pts or dts is nil the corresponding
// timestamp is omitted; zeroLength forces PES_packet_length to 0 (video only).
func buildPES(streamID byte, pts, dts *int64, payload []byte, zeroLength bool) []byte {
	indicator := byte(0)
	timestamp := []byte{}
	if pts != nil && dts != nil {
		indicator = 3
		timestamp = append(timestamp, timestampBytes(0x3, *pts)...)
		timestamp = append(timestamp, timestampBytes(0x1, *dts)...)
	} else if pts != nil {
		indicator = 2
		timestamp = append(timestamp, timestampBytes(0x2, *pts)...)
	}
	headerDataLength := len(timestamp)
	pes := []byte{0x00, 0x00, 0x01, streamID}
	if zeroLength {
		pes = append(pes, 0x00, 0x00)
	} else {
		length := 3 + headerDataLength + len(payload)
		pes = append(pes, byte(length>>8), byte(length))
	}
	pes = append(pes, 0x80, indicator<<6, byte(headerDataLength))
	pes = append(pes, timestamp...)
	return append(pes, payload...)
}

// stuffAdaptationField extends (or creates) an adaptation field so that a
// short final PES fragment fills its TS packet with adaptation field stuffing
// instead of 0xff payload bytes, mirroring what a real muxer does.
func stuffAdaptationField(coreAF []byte, payloadLen int) []byte {
	if coreAF != nil {
		k := 184 - payloadLen - len(coreAF)
		if k <= 0 {
			return coreAF
		}
		out := make([]byte, 0, len(coreAF)+k)
		out = append(out, coreAF[0]+byte(k))
		out = append(out, coreAF[1:]...)
		out = append(out, bytes.Repeat([]byte{0xff}, k)...)
		return out
	}
	k := 183 - payloadLen // stuffing-only adaptation field, including its length byte
	if k < 0 {
		return nil
	}
	// Length byte, a real zeroed flags byte, then 0xff stuffing.
	out := make([]byte, 0, k+1)
	out = append(out, byte(k), 0x00)
	out = append(out, bytes.Repeat([]byte{0xff}, k-1)...)
	return out
}

// startPES writes the first TS packet of a PES and returns the unwritten tail.
func (s *streamBuilder) startPES(pid uint16, af, pes []byte) []byte {
	capacity := 184 - len(af)
	n := capacity
	if n > len(pes) {
		n = len(pes)
	}
	final := n == len(pes)
	useAF := af
	if final {
		useAF = stuffAdaptationField(af, n)
	}
	s.packet(pid, true, useAF, pes[:n])
	return pes[n:]
}

// continuePES writes the remaining TS packets of a PES, returning the tail
// again so callers can interleave other packets between fragments.
func (s *streamBuilder) continuePES(pid uint16, rest []byte) []byte {
	n := 184
	if n > len(rest) {
		n = len(rest)
	}
	if n == 0 {
		return nil
	}
	final := n == len(rest)
	var af []byte
	if final {
		af = stuffAdaptationField(nil, n)
	}
	s.packet(pid, false, af, rest[:n])
	return rest[n:]
}

func (s *streamBuilder) writePES(pid uint16, af, pes []byte) {
	rest := s.startPES(pid, af, pes)
	for len(rest) > 0 {
		rest = s.continuePES(pid, rest)
	}
}

func payloadBytes(tag byte, n int) []byte {
	out := make([]byte, n)
	for i := range out {
		out[i] = tag ^ byte(i&0xff)
	}
	return out
}

// ---- assertion helpers ----

type expectedPES struct {
	pid     uint16
	program uint16
	epoch   uint64
	payload []byte
	pcr     *int64
	pts     *int64
	dts     *int64
}

func checkInt64Ptr(c *taskcheck.C, name string, got, want *int64) {
	if want == nil {
		c.Check(got == nil, "%s: expected nil pointer, got %v", name, got)
		return
	}
	c.Check(got != nil, "%s: expected pointer to %d, got nil", name, *want)
	if got != nil {
		c.Check(*got == *want, "%s: expected %d, got %d", name, *want, *got)
	}
}

func checkTimedPES(c *taskcheck.C, got *astits.TimedData, want expectedPES) {
	c.Check(got.Data != nil && got.Data.PES != nil, "expected PES data on pid %d", want.pid)
	if got.Data == nil || got.Data.PES == nil {
		return
	}
	c.Check(got.Data.PID == want.pid, "pid: expected %d, got %d", want.pid, got.Data.PID)
	c.Check(bytes.Equal(got.Data.PES.Data, want.payload), "payload mismatch on pid %d", want.pid)
	c.Check(got.ProgramNumber == want.program, "program: expected %d, got %d", want.program, got.ProgramNumber)
	c.Check(got.ClockEpoch == want.epoch, "epoch: expected %d, got %d", want.epoch, got.ClockEpoch)
	checkInt64Ptr(c, "PCR27MHz", got.PCR27MHz, want.pcr)
	checkInt64Ptr(c, "PTS90k", got.PTS90k, want.pts)
	checkInt64Ptr(c, "DTS90k", got.DTS90k, want.dts)
}

// collectTimed reads the whole stream through the public NextTimedData entry.
func collectTimed(t *testing.T, raw []byte) []*astits.TimedData {
	dmx := astits.NewDemuxer(context.Background(), bytes.NewReader(raw),
		astits.DemuxerOptPacketSize(188))
	var out []*astits.TimedData
	for {
		td, err := dmx.NextTimedData()
		if err == astits.ErrNoMorePackets {
			return out
		}
		if err != nil {
			t.Fatalf("NextTimedData failed: %v", err)
		}
		out = append(out, td)
	}
}

func sortedKeys(m map[uint16]uint16) []uint16 {
	keys := make([]uint16, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	for i := 0; i < len(keys); i++ {
		for j := i + 1; j < len(keys); j++ {
			if keys[j] < keys[i] {
				keys[i], keys[j] = keys[j], keys[i]
			}
		}
	}
	return keys
}

// ============================================================================
// TestProgramClockWrap: two interleaved programs, independent PCR-only PIDs,
// PCR extension, PTS/DTS crossing the 33-bit zero point and B-frame reorder.
// ============================================================================

func TestProgramClockWrap(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			pnr1    uint16 = 0x1001
			pnr2    uint16 = 0x1002
			pmt1    uint16 = 0x0100
			pmt2    uint16 = 0x0101
			vpid1   uint16 = 0x0200
			pcrpid1 uint16 = 0x0201
			vpid2   uint16 = 0x0300
			pcrpid2 uint16 = 0x0301
			unkpid  uint16 = 0x0444
		)

		s := newStreamBuilder()
		s.writeSection(0x0000, buildPAT(0, 0x0001, map[uint16]uint16{pnr1: pmt1, pnr2: pmt2}))
		s.writeSection(pmt1, buildPMT(0, pnr1, pcrpid1, [][2]uint16{{0x02, vpid1}}, nil))
		s.writeSection(pmt2, buildPMT(0, pnr2, pcrpid2, [][2]uint16{{0x02, vpid2}}, nil))

		// Program 1 starts its epoch just below the 33-bit wrap.
		baseA := mod33 - 1000
		rawA := pcr27Value(baseA, 250)
		s.packet(pcrpid1, false, afPCR(false, baseA, 250), nil)
		// Program 2 keeps an ordinary small clock with a non-zero extension.
		s.packet(pcrpid2, false, afPCR(false, 1_000_000, 123), nil)

		// PES 1 (program 1): PTS crosses zero upwards, DTS stays below it.
		pes1Payload := payloadBytes(0xa1, 200)
		pes1 := buildPES(0xe0, intPtr(50), intPtr(mod33-500), pes1Payload, false)
		rest := s.startPES(vpid1, nil, pes1)
		// Packet-level interleaving: a program 2 PCR-only packet lands while
		// the program 1 PES is still incomplete and must not touch its anchor.
		rawC2 := pcr27Value(1_000_500, 1)
		s.packet(pcrpid2, false, afPCR(false, 1_000_500, 1), nil)
		for len(rest) > 0 {
			rest = s.continuePES(vpid1, rest)
		}
		want1PTS := baseA + 1050 // 50 wraps to mod33+50 relative to anchor mod33-1000
		want1DTS := mod33 - 500

		// PES 2 (program 2): PTS only, DTS pointer must stay nil. Its anchor is
		// the interleaved PCR above, not the initial one.
		pes2Payload := payloadBytes(0xb2, 80)
		s.writePES(vpid2, nil, buildPES(0xe0, intPtr(1_000_300), nil, pes2Payload, false))

		// PES 3 (program 2): exact half-circle tie against the current anchor,
		// the smaller expansion wins.
		anchor90C2 := rawC2 / 300
		halfPTS := anchor90C2 + mod33/2
		tieWant := anchor90C2 - mod33/2
		pes3Payload := payloadBytes(0xc3, 40)
		s.writePES(vpid2, nil, buildPES(0xe0, intPtr(halfPTS), nil, pes3Payload, false))

		// PES 4 (program 2): no timestamps at all, no fabricated zero pointers.
		pes4Payload := payloadBytes(0xd4, 40)
		s.writePES(vpid2, nil, buildPES(0xe0, nil, nil, pes4Payload, false))

		// Program 1 PCR wraps past zero; signed-difference accumulation.
		rawB := refUnwrapPCR(intPtr(rawA), pcr27Value(900, 100))
		s.packet(pcrpid1, false, afPCR(false, 900, 100), nil)
		anchor90B := rawB / 300

		// PES 5: both timestamps, ordinary offsets from the wrapped anchor.
		pes5Payload := payloadBytes(0xe5, 300)
		pes5 := buildPES(0xe0, intPtr(1500), intPtr(600), pes5Payload, false)
		s.writePES(vpid1, nil, pes5)

		// PES 6: B-frame reorder. DTS advances (900 > 600) while PTS goes
		// backwards (1200 < 1500); anchor-based unwrapping must not force the
		// PTS up by a whole modulus the way monotonic PTS unwrapping would.
		pes6Payload := payloadBytes(0xf6, 60)
		s.writePES(vpid1, nil, buildPES(0xe0, intPtr(1200), intPtr(900), pes6Payload, false))

		// PES 7: PID unknown to every PMT, delivered with empty clock context.
		pes7Payload := payloadBytes(0x17, 30)
		s.writePES(unkpid, nil, buildPES(0xe0, intPtr(1234), nil, pes7Payload, false))

		// PES 8: zero-length video PES, only flushed when PES 9 starts.
		pes8Payload := payloadBytes(0x28, 100)
		pes8 := buildPES(0xe0, intPtr(2000), nil, pes8Payload, true)
		s.writePES(vpid1, nil, pes8)

		// PES 9: another zero-length video PES; PES 8 flushes on this start and
		// PES 9 itself is drained by the EOF dump.
		pes9Payload := payloadBytes(0x39, 100)
		pes9 := buildPES(0xe0, intPtr(2100), nil, pes9Payload, true)
		s.writePES(vpid1, nil, pes9)

		got := collectTimed(t, s.buf.Bytes())

		var tables, pes []*astits.TimedData
		for _, td := range got {
			if td.Data.PES != nil {
				pes = append(pes, td)
			} else {
				tables = append(tables, td)
			}
		}
		c.Check(len(tables) == 3, "expected 1 PAT + 2 PMT, got %d", len(tables))
		for _, td := range tables {
			c.Check(td.ProgramNumber == 0 && td.PCR27MHz == nil && td.PTS90k == nil && td.DTS90k == nil,
				"non-PES data must carry no clock pointers")
		}
		c.Check(len(pes) == 9, "expected 9 PES, got %d", len(pes))
		if len(pes) != 9 {
			return
		}

		checkTimedPES(c, pes[0], expectedPES{vpid1, pnr1, 0, pes1Payload,
			intPtr(rawA), intPtr(want1PTS), intPtr(want1DTS)})
		checkTimedPES(c, pes[1], expectedPES{vpid2, pnr2, 0, pes2Payload,
			intPtr(rawC2), intPtr(1_000_300), nil})
		// PES 3 resolves the half-circle tie against the current anchor to the
		// smaller value; PES 4 keeps the same anchor with no timestamp pointers.
		checkTimedPES(c, pes[2], expectedPES{vpid2, pnr2, 0, pes3Payload,
			intPtr(rawC2), intPtr(tieWant), nil})
		checkTimedPES(c, pes[3], expectedPES{vpid2, pnr2, 0, pes4Payload,
			intPtr(rawC2), nil, nil})
		checkTimedPES(c, pes[4], expectedPES{vpid1, pnr1, 0, pes5Payload,
			intPtr(rawB), intPtr(refUnwrap90k(1500, anchor90B)), intPtr(refUnwrap90k(600, anchor90B))})
		checkTimedPES(c, pes[5], expectedPES{vpid1, pnr1, 0, pes6Payload,
			intPtr(rawB), intPtr(anchor90B + 300), intPtr(anchor90B)})
		// Explicitly prove the B-frame PTS is not pushed up by one modulus.
		c.Check(pes[5].PTS90k != nil && *pes[5].PTS90k == anchor90B+300,
			"B-frame PTS must follow the PCR anchor, not monotonic PTS unwrapping")
		c.Check(*pes[5].PTS90k < *pes[4].PTS90k, "B-frame display time may go backwards")
		checkTimedPES(c, pes[6], expectedPES{unkpid, 0, 0, pes7Payload, nil, nil, nil})
		checkTimedPES(c, pes[7], expectedPES{vpid1, pnr1, 0, pes8Payload,
			intPtr(rawB), intPtr(refUnwrap90k(2000, anchor90B)), nil})
		checkTimedPES(c, pes[8], expectedPES{vpid1, pnr1, 0, pes9Payload,
			intPtr(rawB), intPtr(refUnwrap90k(2100, anchor90B)), nil})

		// Sanity check on the independent PCR wrap arithmetic.
		c.Check(rawB == rawA+569_850, "PCR wrap delta expected %d, got %d", rawA+569_850, rawB)
		c.Check(*pes[0].PTS90k == mod33+50, "PTS crossing zero must expand above 2^33")
	})
}

// ============================================================================
// TestProgramClockSnapshot: snapshots taken at the first TS packet stay fixed
// through later PCR/PMT updates, PCR PID changes, payload-less discontinuities,
// missing anchors and Rewind.
// ============================================================================

func TestProgramClockSnapshot(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			pnr    uint16 = 0x2100
			pmtpid uint16 = 0x1000
			vpid   uint16 = 0x0100
			pcrA   uint16 = 0x0101
			pcrB   uint16 = 0x0102
			pcrC   uint16 = 0x0103
			unkpid uint16 = 0x0444
			h264          = 0x1b
		)

		// Anchor values are defined here so both the stream builder and the
		// expectations below share one independent computation.
		raw1 := pcr27Value(50_000, 11)
		raw2 := refUnwrapPCR(intPtr(raw1), pcr27Value(51_000, 22))
		raw3 := pcr27Value(60_000, 0) // first PCR of epoch 1: raw value
		raw4 := pcr27Value(71_000, 5) // first PCR after the discontinuity

		build := func() []byte {
			s := newStreamBuilder()

			// PES on an unknown PID before any PSI: no program, no pointers.
			uPayload := payloadBytes(0x01, 40)
			s.writePES(unkpid, nil, buildPES(0xe0, intPtr(999), nil, uPayload, false))

			s.writeSection(0x0000, buildPAT(0, 0x0002, map[uint16]uint16{pnr: pmtpid}))
			s.writeSection(pmtpid, buildPMT(0, pnr, pcrA, [][2]uint16{{h264, vpid}}, nil))

			s.packet(pcrA, false, afPCR(false, 50_000, 11), nil)

			// PES A spans packets; a PCR update arrives while it is incomplete.
			aPayload := payloadBytes(0xa0, 250)
			pesA := buildPES(0xe0, intPtr(50_200), intPtr(49_900), aPayload, false)
			rest := s.startPES(vpid, nil, pesA)
			s.packet(pcrA, false, afPCR(false, 51_000, 22), nil)
			for len(rest) > 0 {
				rest = s.continuePES(vpid, rest)
			}

			// Duplicated PMT and descriptor-only PMT change: no epoch reset.
			s.writeSection(pmtpid, buildPMT(0, pnr, pcrA, [][2]uint16{{h264, vpid}}, nil))
			s.writeSection(pmtpid, buildPMT(1, pnr, pcrA, [][2]uint16{{h264, vpid}},
				[]byte{0x09, 0x01, 0x42}))

			bPayload := payloadBytes(0xb0, 50)
			s.writePES(vpid, nil, buildPES(0xe0, intPtr(51_200), nil, bPayload, false))

			// PCR PID change: new epoch.
			s.writeSection(pmtpid, buildPMT(2, pnr, pcrB, [][2]uint16{{h264, vpid}}, nil))
			s.packet(pcrB, false, afPCR(false, 60_000, 0), nil)

			// PES C in progress while another PMT moves the PCR PID again.
			cPayload := payloadBytes(0xc0, 260)
			pesC := buildPES(0xe0, intPtr(60_100), nil, cPayload, false)
			rest = s.startPES(vpid, nil, pesC)
			s.writeSection(pmtpid, buildPMT(3, pnr, pcrC, [][2]uint16{{h264, vpid}}, nil))
			s.packet(pcrC, false, afPCR(false, 70_000, 0), nil)
			for len(rest) > 0 {
				rest = s.continuePES(vpid, rest)
			}

			// Payload-less discontinuity on the current PCR PID: new epoch,
			// anchor cleared, even though the packet has no payload.
			s.packet(pcrC, false, afDiscontinuity(), nil)

			dPayload := payloadBytes(0xd0, 70)
			s.writePES(vpid, nil, buildPES(0xe0, intPtr(70_500), nil, dPayload, false))

			// First PCR of the new epoch establishes a fresh anchor.
			s.packet(pcrC, false, afPCR(false, 71_000, 5), nil)

			ePayload := payloadBytes(0xe0, 90)
			s.writePES(vpid, nil, buildPES(0xe0, intPtr(71_500), nil, ePayload, true))
			fPayload := payloadBytes(0xf0, 60)
			s.writePES(vpid, nil, buildPES(0xe0, intPtr(71_600), nil, fPayload, false))
			// Zero-length PES drained through the EOF path.
			gPayload := payloadBytes(0x07, 120)
			s.writePES(vpid, nil, buildPES(0xe0, intPtr(71_700), nil, gPayload, true))

			return s.buf.Bytes()
		}

		raw := build()
		got := collectTimed(t, raw)

		var tables, pes []*astits.TimedData
		for _, td := range got {
			if td.Data.PES != nil {
				pes = append(pes, td)
			} else {
				tables = append(tables, td)
			}
		}
		c.Check(len(tables) == 6, "expected PAT + 5 PMT deliveries, got %d", len(tables))
		c.Check(len(pes) == 8, "expected 8 PES, got %d", len(pes))
		if len(pes) != 8 {
			return
		}

		// U: unknown PID, empty clock context.
		checkTimedPES(c, pes[0], expectedPES{unkpid, 0, 0, payloadBytes(0x01, 40), nil, nil, nil})
		// A: anchored by PCR1 even though PCR2 arrived mid-PES.
		checkTimedPES(c, pes[1], expectedPES{vpid, pnr, 0, payloadBytes(0xa0, 250),
			intPtr(raw1), intPtr(50_200), intPtr(49_900)})
		// B: duplicate/descriptor-only PMTs kept epoch 0; anchor is now PCR2.
		checkTimedPES(c, pes[2], expectedPES{vpid, pnr, 0, payloadBytes(0xb0, 50),
			intPtr(raw2), intPtr(51_200), nil})
		// C: epoch 1 and PCR3, despite the PMT/PCR change arriving mid-PES.
		checkTimedPES(c, pes[3], expectedPES{vpid, pnr, 1, payloadBytes(0xc0, 260),
			intPtr(raw3), intPtr(60_100), nil})
		// D: epoch after the payload-less discontinuity, no anchor.
		checkTimedPES(c, pes[4], expectedPES{vpid, pnr, 3, payloadBytes(0xd0, 70), nil, nil, nil})
		// E/F/G: anchor PCR4 in the post-discontinuity epoch.
		anchor90_4 := raw4 / 300
		checkTimedPES(c, pes[5], expectedPES{vpid, pnr, 3, payloadBytes(0xe0, 90),
			intPtr(raw4), intPtr(refUnwrap90k(71_500, anchor90_4)), nil})
		checkTimedPES(c, pes[6], expectedPES{vpid, pnr, 3, payloadBytes(0xf0, 60),
			intPtr(raw4), intPtr(refUnwrap90k(71_600, anchor90_4)), nil})
		checkTimedPES(c, pes[7], expectedPES{vpid, pnr, 3, payloadBytes(0x07, 120),
			intPtr(raw4), intPtr(refUnwrap90k(71_700, anchor90_4)), nil})

		// Snapshot immutability: keep PES A and verify it does not move while
		// later clocks and epochs evolve.
		snapA := pes[1]
		snapAPCR := *snapA.PCR27MHz
		snapAPTS := *snapA.PTS90k
		snapADTS := *snapA.DTS90k
		c.Check(snapA.ClockEpoch == 0, "snapshot A epoch changed")
		c.Check(*pes[7].PCR27MHz != snapAPCR, "later clock should differ from snapshot")
		c.Check(*snapA.PCR27MHz == snapAPCR && *snapA.PTS90k == snapAPTS && *snapA.DTS90k == snapADTS,
			"snapshot values must be owned and stay fixed")

		// Rewind on the same demuxer must rebuild the program clock from
		// scratch so that re-reading the same stream gives identical results.
		r := bytes.NewReader(raw)
		dmx := astits.NewDemuxer(context.Background(), r,
			astits.DemuxerOptPacketSize(188))
		readAll := func() []*astits.TimedData {
			var out []*astits.TimedData
			for {
				td, err := dmx.NextTimedData()
				if err == astits.ErrNoMorePackets {
					return out
				}
				if err != nil {
					t.Fatalf("re-read failed: %v", err)
				}
				out = append(out, td)
			}
		}
		first := readAll()
		n, err := dmx.Rewind()
		c.Check(err == nil && n == 0, "Rewind failed: n=%d err=%v", n, err)
		again := readAll()

		c.Check(len(first) == len(got), "first pass changed delivery count: %d vs %d", len(first), len(got))
		c.Check(len(again) == len(first), "rewind changed delivery count: %d vs %d", len(again), len(first))
		for i := 0; i < len(again) && i < len(first); i++ {
			a, b := again[i], first[i]
			c.Check(a.Data.PID == b.Data.PID && a.ProgramNumber == b.ProgramNumber && a.ClockEpoch == b.ClockEpoch,
				"rewind mismatch at %d", i)
			c.Check((a.PCR27MHz == nil) == (b.PCR27MHz == nil) &&
				(a.PTS90k == nil) == (b.PTS90k == nil) &&
				(a.DTS90k == nil) == (b.DTS90k == nil),
				"rewind pointer presence mismatch at %d", i)
			if a.PCR27MHz != nil {
				c.Check(*a.PCR27MHz == *b.PCR27MHz, "rewind PCR value mismatch at %d", i)
			}
			if a.PTS90k != nil {
				c.Check(*a.PTS90k == *b.PTS90k, "rewind PTS value mismatch at %d", i)
			}
			if a.DTS90k != nil {
				c.Check(*a.DTS90k == *b.DTS90k, "rewind DTS value mismatch at %d", i)
			}
			if a.Data.PES != nil {
				c.Check(bytes.Equal(a.Data.PES.Data, b.Data.PES.Data), "rewind payload mismatch at %d", i)
			}
		}
	})
}

// ============================================================================
// TestProgramClockEpoch: epoch transitions, program isolation, PCR PID 0x1fff
// and first-in-epoch PCR handling.
// ============================================================================

func TestProgramClockEpoch(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			pnr1  uint16 = 0x0501
			pnr2  uint16 = 0x0502
			pmt1  uint16 = 0x0500
			pmt2  uint16 = 0x0501
			v1    uint16 = 0x0510
			pcr1  uint16 = 0x0511
			pcr1b uint16 = 0x0512
			v2    uint16 = 0x0520
			pcr2  uint16 = 0x0521
		)

		s := newStreamBuilder()
		s.writeSection(0x0000, buildPAT(0, 0x0003, map[uint16]uint16{pnr1: pmt1, pnr2: pmt2}))
		s.writeSection(pmt1, buildPMT(0, pnr1, pcr1, [][2]uint16{{0x02, v1}}, nil))
		s.writeSection(pmt2, buildPMT(0, pnr2, pcr2, [][2]uint16{{0x0f, v2}}, nil))

		// Epoch 0: first PCR taken raw, second one accumulated.
		anchor00 := pcr27Value(100_000, 10)
		s.packet(pcr1, false, afPCR(false, 100_000, 10), nil)
		anchor01 := refUnwrapPCR(intPtr(anchor00), pcr27Value(100_900, 20))
		s.packet(pcr1, false, afPCR(false, 100_900, 20), nil)
		payloadA := payloadBytes(0xa1, 50)
		s.writePES(v1, nil, buildPES(0xe0, intPtr(100_500), nil, payloadA, false))

		// Program 2 runs its own independent clock.
		anchor20 := pcr27Value(5_000, 0)
		s.packet(pcr2, false, afPCR(false, 5_000, 0), nil)
		payloadB := payloadBytes(0xb2, 50)
		s.writePES(v2, nil, buildPES(0xc0, intPtr(5_100), nil, payloadB, false))

		// Payload-less discontinuity on program 1's PCR PID: epoch 1, no anchor.
		s.packet(pcr1, false, afDiscontinuity(), nil)
		payloadC := payloadBytes(0xc3, 50)
		s.writePES(v1, nil, buildPES(0xe0, intPtr(100_700), nil, payloadC, false))

		// Discontinuity packet that also carries a PCR: the epoch is bumped
		// first, then the PCR becomes the raw first anchor of the new epoch.
		anchor10 := pcr27Value(500, 5)
		s.packet(pcr1, false, afPCR(true, 500, 5), nil)
		payloadD := payloadBytes(0xd4, 50)
		s.writePES(v1, nil, buildPES(0xe0, intPtr(480), nil, payloadD, false))

		// A discontinuity on program 2's PCR PID is invisible to program 1.
		s.packet(pcr2, false, afDiscontinuity(), nil)
		payloadB2 := payloadBytes(0xb5, 40)
		s.writePES(v2, nil, buildPES(0xc0, intPtr(5_200), nil, payloadB2, false))
		payloadE := payloadBytes(0xe6, 40)
		s.writePES(v1, nil, buildPES(0xe0, intPtr(490), nil, payloadE, false))

		// Discontinuity flag on the video PID itself: not a PCR PID, no effect.
		payloadF := payloadBytes(0xf7, 170)
		pesF := buildPES(0xe0, intPtr(495), nil, payloadF, false)
		rest := s.startPES(v1, afDiscontinuity(), pesF)
		for len(rest) > 0 {
			rest = s.continuePES(v1, rest)
		}

		// Duplicated PMT and descriptor-only change: epoch stays 2.
		s.writeSection(pmt1, buildPMT(0, pnr1, pcr1, [][2]uint16{{0x02, v1}}, nil))
		s.writeSection(pmt1, buildPMT(1, pnr1, pcr1, [][2]uint16{{0x02, v1}},
			[]byte{0x09, 0x01, 0x77}))

		// PCR PID change: epoch 3.
		s.writeSection(pmt1, buildPMT(2, pnr1, pcr1b, [][2]uint16{{0x02, v1}}, nil))
		anchor2 := pcr27Value(700, 0)
		s.packet(pcr1b, false, afPCR(false, 700, 0), nil)
		payloadG := payloadBytes(0xa8, 40)
		s.writePES(v1, nil, buildPES(0xe0, intPtr(710), nil, payloadG, false))

		// PCR PID 0x1fff: epoch 4, no anchor; stray PCRs on the old PID are ignored.
		s.writeSection(pmt1, buildPMT(3, pnr1, 0x1fff, [][2]uint16{{0x02, v1}}, nil))
		s.packet(pcr1b, false, afPCR(false, 777, 0), nil)
		payloadH := payloadBytes(0xb9, 40)
		s.writePES(v1, nil, buildPES(0xe0, intPtr(720), nil, payloadH, false))

		// PCR PID becomes the video PID itself; the PES start packet carries
		// the PCR and that PCR is applied before the snapshot is taken. The
		// anchor is placed just below the 33-bit wrap on purpose.
		s.writeSection(pmt1, buildPMT(4, pnr1, v1, [][2]uint16{{0x02, v1}}, nil))
		anchor4 := pcr27Value(mod33-1000, 100)
		payloadI := payloadBytes(0xca, 40)
		pesI := buildPES(0xe0, intPtr(mod33-950), nil, payloadI, false)
		s.writePES(v1, afPCR(false, mod33-1000, 100), pesI)

		// Normal PCR flow crossing the 33-bit wrap in this epoch: two small
		// signed differences, the second one jumps forward across zero.
		anchor5 := refUnwrapPCR(intPtr(anchor4), pcr27Value(mod33-100, 1))
		s.packet(v1, false, afPCR(false, mod33-100, 1), nil)
		anchor6 := refUnwrapPCR(intPtr(anchor5), pcr27Value(50, 2))
		s.packet(v1, false, afPCR(false, 50, 2), nil)
		payloadJ := payloadBytes(0xdb, 40)
		s.writePES(v1, nil, buildPES(0xe0, intPtr(60), nil, payloadJ, false))

		// Zero-length video PES delivered through the EOF dump.
		payloadK := payloadBytes(0xec, 130)
		s.writePES(v1, nil, buildPES(0xe0, intPtr(61), nil, payloadK, true))

		got := collectTimed(t, s.buf.Bytes())
		var tables, pes []*astits.TimedData
		for _, td := range got {
			if td.Data.PES != nil {
				pes = append(pes, td)
			} else {
				tables = append(tables, td)
			}
		}
		c.Check(len(tables) == 8, "expected PAT + 7 PMT deliveries, got %d", len(tables))
		c.Check(len(pes) == 12, "expected 12 PES, got %d", len(pes))
		if len(pes) != 12 {
			return
		}

		idx := 0
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 0, payloadA, intPtr(anchor01), intPtr(100_500), nil})
		idx++
		checkTimedPES(c, pes[idx], expectedPES{v2, pnr2, 0, payloadB, intPtr(anchor20), intPtr(5_100), nil})
		idx++
		// After the payload-less discontinuity: epoch 1 with no anchor.
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 1, payloadC, nil, nil, nil})
		idx++
		// The discontinuity packet carrying a PCR bumped the epoch again and
		// its PCR is the raw first anchor of epoch 2.
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 2, payloadD, intPtr(anchor10), intPtr(480), nil})
		idx++
		// Program 2 discontinuity leaves its clock without an anchor.
		checkTimedPES(c, pes[idx], expectedPES{v2, pnr2, 1, payloadB2, nil, nil, nil})
		idx++
		// Program 1 untouched by program 2's flag.
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 2, payloadE, intPtr(anchor10), intPtr(490), nil})
		idx++
		// Discontinuity flag on the video PID changes nothing clock-wise.
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 2, payloadF, intPtr(anchor10), intPtr(495), nil})
		idx++
		// PCR PID change starts epoch 3.
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 3, payloadG, intPtr(anchor2), intPtr(710), nil})
		idx++
		// PCR PID 0x1fff: epoch 4 and no anchor despite the stray PCR.
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 4, payloadH, nil, nil, nil})
		idx++
		// PCR in the PES start packet is applied before snapshotting; epoch 5.
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 5, payloadI,
			intPtr(anchor4), intPtr(mod33 - 950), nil})
		idx++
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 5, payloadJ,
			intPtr(anchor6), intPtr(refUnwrap90k(60, anchor6/300)), nil})
		idx++
		checkTimedPES(c, pes[idx], expectedPES{v1, pnr1, 5, payloadK,
			intPtr(anchor6), intPtr(refUnwrap90k(61, anchor6/300)), nil})

		// Explicit arithmetic checks on the extension and wrap accumulation.
		c.Check(anchor01 == 30_270_020, "accumulated PCR expected 30270020, got %d", anchor01)
		c.Check(anchor10 == 150_005, "first PCR of epoch must be raw base*300+ext, got %d", anchor10)
		c.Check(anchor4 == pcr27Value(mod33-1000, 100), "in-packet PCR must keep its extension phase, got %d", anchor4)
		c.Check(anchor5 == anchor4+(900*300-99), "pre-wrap PCR delta, got %d", anchor5)
		c.Check(anchor6 == anchor5+(150*300+1), "post-wrap PCR delta, got %d", anchor6)
		c.Check(anchor6 > mod27, "unwrapped clock should exceed one modulus, got %d", anchor6)
	})
}
