package astits

import (
	"bytes"
	"errors"
	"testing"

	"github.com/asticode/go-astits/task_support/taskcheck"
)

// This test file builds transport streams and PSI sections with its own,
// independent wire-format helpers (including an independent CRC-32/MPEG-2
// implementation) and parses remuxer output without reusing production code.

// ---------------------------------------------------------------------------
// Independent CRC-32/MPEG-2 (poly 0x04C11DB7, init 0xFFFFFFFF, no reflection)
// ---------------------------------------------------------------------------

func mpegCRC32(b []byte) uint32 {
	crc := uint32(0xffffffff)
	for _, v := range b {
		crc ^= uint32(v) << 24
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

// ---------------------------------------------------------------------------
// Independent PSI section construction
// ---------------------------------------------------------------------------

type testPATProgram struct {
	programNumber uint16
	pmtPID        uint16
}

type testPMTES struct {
	streamType uint8
	pid        uint16
	descs      []byte // raw descriptor bytes
}

// buildSection serializes one generic CRC-protected syntax section.
func buildSection(tableID uint8, tableIDExt uint16, version, sectionNumber, lastSection uint8, body []byte) []byte {
	sectionLength := 5 + len(body) + 4
	out := []byte{
		tableID,
		0xb0 | byte(sectionLength>>8),
		byte(sectionLength),
		byte(tableIDExt >> 8),
		byte(tableIDExt),
		0xc1 | (version << 1), // reserved 11, version, current_next=1
		sectionNumber,
		lastSection,
	}
	out = append(out, body...)
	crc := mpegCRC32(out)
	return append(out, byte(crc>>24), byte(crc>>16), byte(crc>>8), byte(crc))
}

func buildPATSection(tsid uint16, version, sectionNumber, lastSection uint8, programs []testPATProgram) []byte {
	body := make([]byte, 0, 4*len(programs))
	for _, p := range programs {
		body = append(body,
			byte(p.programNumber>>8), byte(p.programNumber),
			0xe0|byte(p.pmtPID>>8), byte(p.pmtPID))
	}
	return buildSection(0x00, tsid, version, sectionNumber, lastSection, body)
}

func buildPMTSection(programNumber, pcrPID uint16, version uint8, programDescs []byte, streams []testPMTES) []byte {
	body := []byte{0xe0 | byte(pcrPID>>8), byte(pcrPID)}
	body = append(body, 0xf0|byte(len(programDescs)>>8), byte(len(programDescs)))
	body = append(body, programDescs...)
	for _, es := range streams {
		body = append(body,
			es.streamType,
			0xe0|byte(es.pid>>8), byte(es.pid),
			0xf0|byte(len(es.descs)>>8), byte(len(es.descs)))
		body = append(body, es.descs...)
	}
	return buildSection(0x02, programNumber, version, 0, 0, body)
}

func descriptor(tag uint8, content ...byte) []byte {
	return append([]byte{tag, byte(len(content))}, content...)
}

func hiByte(pid uint16) byte { return byte(pid >> 8) }
func loByte(pid uint16) byte { return byte(pid) }

func corruptSection(section []byte) []byte {
	out := append([]byte(nil), section...)
	out[len(out)-5] ^= 0xff // body byte, CRC stays as stored
	return out
}

// ---------------------------------------------------------------------------
// Independent TS packet construction
// ---------------------------------------------------------------------------

const testPacketSize = 188

// psiPacket builds one PSI packet. Remaining payload space is stuffed with 0xff.
func psiPacket(pid uint16, pusi bool, cc uint8, payload []byte) []byte {
	pkt := make([]byte, testPacketSize)
	for i := range pkt {
		pkt[i] = 0xff
	}
	pkt[0] = syncByte
	pkt[1] = byte(pid >> 8)
	if pusi {
		pkt[1] |= 0x40
	}
	pkt[2] = byte(pid)
	pkt[3] = 0x10 | (cc & 0x0f) // payload only
	copy(pkt[4:], payload)
	return pkt
}

// psiPacketsPUSI starts a section chain: pointer field 0 followed by sections,
// overflowing into continuation packets without pointer fields.
func psiPacketsPUSI(pid uint16, cc *uint8, sections ...[]byte) [][]byte {
	payload := []byte{0}
	for _, s := range sections {
		payload = append(payload, s...)
	}
	var out [][]byte
	first := true
	for len(payload) > 0 {
		capacity := testPacketSize - 4
		n := capacity
		if n > len(payload) {
			n = len(payload)
		}
		out = append(out, psiPacket(pid, first, *cc, payload[:n]))
		*cc = (*cc + 1) & 0x0f
		payload = payload[n:]
		first = false
	}
	return out
}

// mediaPacket builds a media packet. af is the full adaptation field including
// its length byte (nil means no adaptation field); payload nil means AF only.
func mediaPacket(pid uint16, pusi bool, cc uint8, af, payload []byte) []byte {
	pkt := make([]byte, testPacketSize)
	pkt[0] = syncByte
	if pusi {
		pkt[1] |= 0x40
	}
	pkt[1] |= byte(pid >> 8)
	pkt[2] = byte(pid)
	afc := uint8(0)
	if af != nil {
		afc |= 2
	}
	if payload != nil {
		afc |= 1
	}
	pkt[3] = afc<<4 | (cc & 0x0f)
	off := 4
	if af != nil {
		copy(pkt[off:], af)
		off += 1 + int(af[0])
	}
	if payload != nil {
		copy(pkt[off:], payload)
	}
	return pkt
}

func scrambledMediaPacket(pid uint16, cc uint8) []byte {
	pkt := mediaPacket(pid, true, cc, nil, []byte{0, 0, 1, 0xe0})
	pkt[3] |= 0x80 // even scrambling key
	return pkt
}

// pcrAdaptation builds an 8-byte adaptation field (length 7) carrying a PCR.
func pcrAdaptation(base uint64, ext uint16) []byte {
	value := (base << 15) | (uint64(ext) & 0x1ff)
	return []byte{
		7,    // adaptation_field_length
		0x10, // PCR flag
		byte(value >> 40), byte(value >> 32), byte(value >> 24),
		byte(value >> 16), byte(value >> 8), byte(value),
	}
}

// pesPacket builds a minimal PES packet carrying one PTS and an opaque body.
func pesPacket(streamID byte, pts uint64, body []byte) []byte {
	optionalLength := 5
	totalLength := 3 + optionalLength + len(body)
	out := []byte{
		0, 0, 1, streamID,
		byte(totalLength >> 8), byte(totalLength),
		0x80, // marker bits
		0x80, // PTS only
		byte(optionalLength),
		0x21 | byte((pts>>29)&0x0e),
		byte(pts >> 22),
		byte((pts>>14)&0xfe) | 1,
		byte(pts >> 7),
		byte((pts<<1)&0xfe) | 1,
	}
	return append(out, body...)
}

// feedChunks writes data through the remuxer using arbitrary byte cuts.
func feedChunks(c *taskcheck.C, r *ProgramRemuxer, data []byte, chunk int) {
	c.Check(chunk > 0, "chunk size must be positive")
	for pos := 0; pos < len(data); {
		n := chunk
		if n > len(data)-pos {
			n = len(data) - pos
		}
		written, err := r.Write(data[pos : pos+n])
		c.Check(err == nil, "unexpected write error: %v", err)
		c.Check(written == n, "Write returned %d, want %d", written, n)
		pos += n
	}
}

// feedPackets writes a sequence of TS packets with arbitrary byte cuts.
func feedPackets(c *taskcheck.C, r *ProgramRemuxer, packets [][]byte, chunk int) {
	for _, pkt := range packets {
		feedChunks(c, r, pkt, chunk)
	}
}

// ---------------------------------------------------------------------------
// Independent output parsing
// ---------------------------------------------------------------------------

type testOutputPacket struct {
	raw       []byte
	pid       uint16
	pusi      bool
	hasAF     bool
	hasPay    bool
	cc        uint8
	payload   []byte
	afContent []byte // adaptation field bytes after the length byte (nil when absent)
}

func parseOutputPackets(c *taskcheck.C, data []byte) []testOutputPacket {
	c.Check(len(data)%testPacketSize == 0, "output length %d is not a multiple of 188", len(data))
	var out []testOutputPacket
	for off := 0; off+testPacketSize <= len(data); off += testPacketSize {
		raw := data[off : off+testPacketSize]
		c.Check(raw[0]&0xff == syncByte, "packet at %d lacks sync byte", off)
		afc := (raw[3] >> 4) & 0x03
		p := testOutputPacket{
			raw:    raw,
			pid:    uint16(raw[1]&0x1f)<<8 | uint16(raw[2]),
			pusi:   raw[1]&0x40 != 0,
			hasAF:  afc&2 != 0,
			hasPay: afc&1 != 0,
			cc:     raw[3] & 0x0f,
		}
		pos := 4
		if p.hasAF {
			afLength := int(raw[4])
			p.afContent = raw[5 : 5+afLength]
			pos = 5 + afLength
		}
		if p.hasPay {
			p.payload = raw[pos:]
		}
		out = append(out, p)
	}
	return out
}

// collectSections reassembles every section carried on one output PID.
func collectSections(c *taskcheck.C, packets []testOutputPacket, pid uint16) [][]byte {
	var sections [][]byte
	var current []byte
	var remaining int
	for _, p := range packets {
		if p.pid != pid {
			continue
		}
		payload := p.payload
		if p.pusi {
			c.Check(len(payload) >= 1, "PUSI packet on PID %d has no pointer field", pid)
			pointer := int(payload[0])
			payload = payload[1+pointer:]
		}
		for len(payload) > 0 {
			if remaining == 0 {
				if payload[0] == 0xff {
					break
				}
				c.Check(len(payload) >= 3, "truncated section header on PID %d", pid)
				sectionLength := int(uint16(payload[1]&0x0f)<<8 | uint16(payload[2]))
				remaining = 3 + sectionLength
				current = nil
			}
			n := remaining
			if n > len(payload) {
				n = len(payload)
			}
			current = append(current, payload[:n]...)
			payload = payload[n:]
			remaining -= n
			if remaining == 0 {
				expectedCRC := uint32(current[len(current)-4])<<24 | uint32(current[len(current)-3])<<16 |
					uint32(current[len(current)-2])<<8 | uint32(current[len(current)-1])
				c.Check(mpegCRC32(current[:len(current)-4]) == expectedCRC,
					"output section CRC mismatch on PID %d", pid)
				sections = append(sections, current)
			}
		}
	}
	return sections
}

func sectionVersion(sec []byte) uint8 { return (sec[5] >> 1) & 0x1f }

func parseOutputPAT(c *taskcheck.C, sec []byte) (tsid uint16, programs map[uint16]uint16) {
	c.Check(sec[0] == 0x00, "PAT table id is %d", sec[0])
	tsid = uint16(sec[3])<<8 | uint16(sec[4])
	programs = make(map[uint16]uint16)
	end := len(sec) - 4
	for pos := 8; pos+4 <= end; pos += 4 {
		program := uint16(sec[pos])<<8 | uint16(sec[pos+1])
		pmtPID := uint16(sec[pos+2]&0x1f)<<8 | uint16(sec[pos+3])
		programs[program] = pmtPID
	}
	return tsid, programs
}

func parseOutputPMT(c *taskcheck.C, sec []byte) (program, pcrPID uint16, progDescs []byte, streams []testPMTES) {
	c.Check(sec[0] == 0x02, "PMT table id is %d", sec[0])
	program = uint16(sec[3])<<8 | uint16(sec[4])
	pos := 8
	pcrPID = uint16(sec[pos]&0x1f)<<8 | uint16(sec[pos+1])
	pos += 2
	progInfoLength := int(uint16(sec[pos]&0x0f)<<8 | uint16(sec[pos+1]))
	pos += 2
	progDescs = append([]byte(nil), sec[pos:pos+progInfoLength]...)
	pos += progInfoLength
	end := len(sec) - 4
	for pos+5 <= end {
		streamType := sec[pos]
		esPID := uint16(sec[pos+1]&0x1f)<<8 | uint16(sec[pos+2])
		esInfoLength := int(uint16(sec[pos+3]&0x0f)<<8 | uint16(sec[pos+4]))
		pos += 5
		descs := append([]byte(nil), sec[pos:pos+esInfoLength]...)
		pos += esInfoLength
		streams = append(streams, testPMTES{streamType: streamType, pid: esPID, descs: descs})
	}
	c.Check(pos == end, "PMT section parsed to %d, end is %d", pos, end)
	return program, pcrPID, progDescs, streams
}

func extractPCR(af []byte) (base uint64, ext uint16, ok bool) {
	if len(af) < 7 || af[0]&0x10 == 0 {
		return 0, 0, false
	}
	var value uint64
	for i := 1; i < 7; i++ {
		value = value<<8 | uint64(af[i])
	}
	return value >> 15, uint16(value & 0x1ff), true
}

func extractPTS(payload []byte) (uint64, bool) {
	if len(payload) < 14 || payload[0] != 0 || payload[1] != 0 || payload[2] != 1 {
		return 0, false
	}
	if payload[7]&0xc0 != 0x80 {
		return 0, false
	}
	b := payload[9:14]
	return uint64(b[0]&0x0e)<<29 | uint64(b[1])<<22 |
		uint64(b[2]&0xfe)<<14 | uint64(b[3])<<7 | uint64(b[4]>>1), true
}

// ---------------------------------------------------------------------------
// Shared scenario fixtures
// ---------------------------------------------------------------------------

const (
	programA    uint16 = 0x1001
	programB    uint16 = 0x2002
	tsid        uint16 = 0x4242
	pmtPIDA     uint16 = 0x0100
	pmtPIDB     uint16 = 0x0300
	videoPID    uint16 = 0x0200
	audioPID    uint16 = 0x0201
	pcrOnlyPID  uint16 = 0x0202
	otherPID    uint16 = 0x0400
	outPMTPID   uint16 = 0x1000
	outFirstPID uint16 = 0x0500
)

func newTestRemuxer(c *taskcheck.C) (*ProgramRemuxer, *bytes.Buffer) {
	var buf bytes.Buffer
	r, err := NewProgramRemuxer(&buf, ProgramRemuxConfig{
		ProgramNumber: programA,
		OutputPMTPID:  outPMTPID,
		FirstMediaPID: outFirstPID,
	})
	c.Check(err == nil, "NewProgramRemuxer failed: %v", err)
	return r, &buf
}

func patProgramsA(version uint8, pmtA uint16, includeB bool) []byte {
	progs := []testPATProgram{{programNumber: 0, pmtPID: 0x0010}} // NIT entry must be tolerated
	progs = append(progs, testPATProgram{programNumber: programA, pmtPID: pmtA})
	if includeB {
		progs = append(progs, testPATProgram{programNumber: programB, pmtPID: pmtPIDB})
	}
	return buildPATSection(tsid, version, 0, 0, progs)
}

func pmtA(version uint8, pcrPID uint16, streams []testPMTES, progDescs []byte) []byte {
	return buildPMTSection(programA, pcrPID, version, progDescs, streams)
}

func pmtB(version uint8) []byte {
	return buildPMTSection(programB, otherPID, version, nil, []testPMTES{
		{streamType: 0x06, pid: otherPID},
	})
}

func buildNStreams(n int, firstPID uint16) []testPMTES {
	out := make([]testPMTES, n)
	for i := range out {
		out[i] = testPMTES{streamType: 0x06, pid: firstPID + uint16(i)}
	}
	return out
}

var (
	regDesc    = descriptor(DescriptorTagRegistration, 'H', 'D', 'M', 'V')
	isoDesc    = descriptor(DescriptorTagISO639LanguageAndAudioType, 'e', 'n', 'g', 0x00)
	opaqueDesc = descriptor(0x83, 0xde, 0xad, 0xbe, 0xef)
	caDesc     = descriptor(0x09, 0x12, 0x34, 0xe4, 0x00) // CA PID 0x0400 embedded
)

// ---------------------------------------------------------------------------
// TestProgramTables
// ---------------------------------------------------------------------------

func TestProgramTables(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Config validation.
		_, err := NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: 0, OutputPMTPID: outPMTPID, FirstMediaPID: outFirstPID})
		c.Check(errors.Is(err, ErrProgramNumberZero), "program number zero: %v", err)
		_, err = NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: programA, OutputPMTPID: 0x001f, FirstMediaPID: outFirstPID})
		c.Check(errors.Is(err, ErrRemuxPIDOutOfRange), "low PMT PID: %v", err)
		_, err = NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: programA, OutputPMTPID: outPMTPID, FirstMediaPID: 0x1fff})
		c.Check(errors.Is(err, ErrRemuxPIDOutOfRange), "high media PID: %v", err)
		_, err = NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: programA, OutputPMTPID: outFirstPID, FirstMediaPID: outFirstPID})
		c.Check(errors.Is(err, ErrRemuxMediaPIDCollision), "PID collision: %v", err)

		// Independent CRC must match the library implementation.
		c.Check(mpegCRC32([]byte("123456789")) == computeCRC32([]byte("123456789")),
			"independent CRC diverges from library CRC")

		r, out := newTestRemuxer(c)

		// Media arriving before the first selected table is dropped and the
		// other program's PMT must never be borrowed.
		feedChunks(c, r, mediaPacket(videoPID, true, 0, nil, pesPacket(0xe0, 111, []byte("early"))), 7)
		feedPackets(c, r, psiPacketsPUSI(pmtPIDB, new(uint8), pmtB(0)), 13)

		// Multi-section PAT (2 sections), interleaved with foreign traffic.
		pat0 := buildPATSection(tsid, 0, 0, 1, []testPATProgram{
			{programNumber: 0, pmtPID: 0x0010},
			{programNumber: programA, pmtPID: pmtPIDA},
		})
		pat1 := buildPATSection(tsid, 0, 1, 1, []testPATProgram{
			{programNumber: programB, pmtPID: pmtPIDB},
		})
		var patCC, pmtCC uint8
		feedChunks(c, r, psiPacketsPUSI(PIDPAT, &patCC, pat0)[0], 5)
		feedChunks(c, r, mediaPacket(otherPID, true, 0, nil, []byte{0, 0, 1, 0xbd}), 11)
		c.Check(out.Len() == 0, "no output allowed before the whole PAT version is received")
		feedChunks(c, r, psiPacketsPUSI(PIDPAT, &patCC, pat1)[0], 5)

		// PMT for program A, carrying registration, ISO 639 and opaque descriptors.
		streams := []testPMTES{
			{streamType: byte(StreamTypeH264Video), pid: videoPID, descs: regDesc},
			{streamType: byte(StreamTypeADTS), pid: audioPID, descs: isoDesc},
		}
		pmt := pmtA(0, videoPID, streams, opaqueDesc)
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmt), 3)

		pkts := parseOutputPackets(c, out.Bytes())
		c.Check(len(pkts) >= 2, "expected at least PAT+PMT packets, got %d", len(pkts))
		c.Check(pkts[0].pid == PIDPAT && pkts[0].pusi, "first output packet must be a PUSI PAT")
		c.Check(pkts[1].pid == outPMTPID && pkts[1].pusi, "second output packet must be a PUSI PMT")

		patSections := collectSections(c, pkts, PIDPAT)
		pmtSections := collectSections(c, pkts, outPMTPID)
		c.Check(len(patSections) == 1 && len(pmtSections) == 1, "one PAT and one PMT section expected")
		c.Check(sectionVersion(patSections[0]) == 0 && sectionVersion(pmtSections[0]) == 0,
			"first output tables use version 0")

		gotTSID, gotPAT := parseOutputPAT(c, patSections[0])
		c.Check(gotTSID == tsid, "transport_stream_id not preserved: %d", gotTSID)
		c.Check(len(gotPAT) == 1, "output PAT must list only the selected program, got %d", len(gotPAT))
		c.Check(gotPAT[programA] == outPMTPID, "output PAT points at %#x", gotPAT[programA])

		gotProgram, gotPCR, gotProgDescs, gotStreams := parseOutputPMT(c, pmtSections[0])
		c.Check(gotProgram == programA, "PMT program number %#x", gotProgram)
		c.Check(gotPCR == outFirstPID, "shared PCR PID must map to the video output PID, got %#x", gotPCR)
		c.Check(bytes.Equal(gotProgDescs, opaqueDesc), "program descriptor bytes not preserved")
		c.Check(len(gotStreams) == 2, "PMT stream count %d", len(gotStreams))
		c.Check(gotStreams[0].streamType == byte(StreamTypeH264Video) && gotStreams[0].pid == outFirstPID,
			"video maps to %#x", gotStreams[0].pid)
		c.Check(gotStreams[1].streamType == byte(StreamTypeADTS) && gotStreams[1].pid == outFirstPID+1,
			"audio maps to %#x", gotStreams[1].pid)
		c.Check(bytes.Equal(gotStreams[0].descs, regDesc) && bytes.Equal(gotStreams[1].descs, isoDesc),
			"ES descriptor bytes not preserved")

		// Table CC counts per output PID.
		c.Check(pkts[0].cc == 0 && pkts[1].cc == 0, "first table packets start at CC 0")

		// Replayed identical tables produce nothing.
		before := out.Len()
		feedPackets(c, r, psiPacketsPUSI(PIDPAT, &patCC, pat0, pat1), 4)
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmt), 4)
		c.Check(out.Len() == before, "replayed table version emitted extra bytes")

		// A duplicated section inside one payload is submitted only once.
		feedChunks(c, r, psiPacket(pmtPIDA, true, pmtCC, append(append([]byte{0}, pmt...), pmt...)), 4)
		pmtCC++
		c.Check(out.Len() == before, "duplicate PMT section emitted a table")

		// Version distance 16 is refused, 17 is treated as old, 1 is an update.
		feedPackets(c, r, psiPacketsPUSI(PIDPAT, &patCC, patProgramsA(16, pmtPIDA, true)), 4)
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmtA(16, videoPID, streams, opaqueDesc)), 4)
		feedPackets(c, r, psiPacketsPUSI(PIDPAT, &patCC, patProgramsA(17, pmtPIDA, true)), 4)
		c.Check(out.Len() == before, "distance 16/17 versions must not be accepted")
		// The input version stays at 0: equal content means no new output tables.
		feedPackets(c, r, psiPacketsPUSI(PIDPAT, &patCC, patProgramsA(1, pmtPIDA, true)), 4)
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmtA(1, videoPID, streams, opaqueDesc)), 4)
		c.Check(out.Len() == before, "same topology under a new input version is not re-emitted")
		// A real topology change bumps the shared output version to 1.
		changedStreams := []testPMTES{
			{streamType: byte(StreamTypeH264Video), pid: videoPID, descs: regDesc},
			{streamType: byte(StreamTypeADTS), pid: audioPID, descs: isoDesc},
			{streamType: 0x06, pid: otherPID},
		}
		feedPackets(c, r, psiPacketsPUSI(PIDPAT, &patCC, patProgramsA(2, pmtPIDA, true)), 4)
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmtA(2, videoPID, changedStreams, opaqueDesc)), 4)
		c.Check(out.Len() > before, "a topology change must update the tables")
		pkts = parseOutputPackets(c, out.Bytes())
		patSections = collectSections(c, pkts, PIDPAT)
		pmtSections = collectSections(c, pkts, outPMTPID)
		c.Check(len(patSections) == 2 && len(pmtSections) == 2, "two table generations expected")
		c.Check(sectionVersion(patSections[1]) == 1 && sectionVersion(pmtSections[1]) == 1,
			"PAT/PMT share the incremented output version")

		// Two input versions interleaved: each completes only when whole.
		v5sec0 := buildPATSection(tsid, 5, 0, 1, []testPATProgram{{programNumber: programA, pmtPID: pmtPIDA}})
		v5sec1 := buildPATSection(tsid, 5, 1, 1, []testPATProgram{{programNumber: programB, pmtPID: pmtPIDB}})
		v6sec0 := buildPATSection(tsid, 6, 0, 1, []testPATProgram{{programNumber: programA, pmtPID: pmtPIDA}})
		v6sec1 := buildPATSection(tsid, 6, 1, 1, []testPATProgram{{programNumber: programB, pmtPID: pmtPIDB}})
		before = out.Len()
		feedChunks(c, r, psiPacketsPUSI(PIDPAT, &patCC, v5sec0)[0], 3)
		feedChunks(c, r, psiPacketsPUSI(PIDPAT, &patCC, v6sec0)[0], 3)
		c.Check(out.Len() == before, "interleaved partial versions must not commit")
		feedChunks(c, r, psiPacketsPUSI(PIDPAT, &patCC, v5sec1)[0], 3) // v5 completes
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmtA(5, videoPID, streams, opaqueDesc)), 3)
		feedChunks(c, r, psiPacketsPUSI(PIDPAT, &patCC, v6sec1)[0], 3) // v6 completes later
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmtA(6, videoPID, changedStreams, opaqueDesc)), 3)
		allPkts := parseOutputPackets(c, out.Bytes())
		patSections = collectSections(c, allPkts, PIDPAT)
		c.Check(len(patSections) == 4, "four committed PAT generations, got %d", len(patSections))
		c.Check(sectionVersion(patSections[2]) == 2 && sectionVersion(patSections[3]) == 3,
			"output versions advance modulo policy: %d %d", sectionVersion(patSections[2]), sectionVersion(patSections[3]))

		// A long PMT spans several packets: first has pointer 0, others do not.
		r2, out2 := newTestRemuxer(c)
		var cc2 uint8
		feedPackets(c, r2, psiPacketsPUSI(PIDPAT, &cc2, patProgramsA(0, pmtPIDA, false)), 2)
		many := make([]testPMTES, 40)
		for i := range many {
			many[i] = testPMTES{streamType: 0x06, pid: uint16(0x600 + i)}
		}
		longPMT := pmtA(0, 0x600, many, nil)
		longPackets := psiPacketsPUSI(pmtPIDA, &cc2, longPMT)
		c.Check(len(longPackets) > 1, "long PMT must occupy several TS packets")
		feedPackets(c, r2, longPackets, 2)
		pkts2 := parseOutputPackets(c, out2.Bytes())
		var pmtPackets []testOutputPacket
		for _, p := range pkts2 {
			if p.pid == outPMTPID {
				pmtPackets = append(pmtPackets, p)
			}
		}
		c.Check(len(pmtPackets) > 1, "output PMT also spans packets, got %d", len(pmtPackets))
		c.Check(pmtPackets[0].pusi && pmtPackets[0].payload[0] == 0, "first PMT packet carries pointer 0")
		for i := 1; i < len(pmtPackets); i++ {
			c.Check(!pmtPackets[i].pusi, "continuation PMT packet %d must not set PUSI", i)
		}
		reassembled := collectSections(c, pmtPackets, outPMTPID)
		expectedMany := make([]testPMTES, 40)
		for i := range expectedMany {
			expectedMany[i] = testPMTES{streamType: 0x06, pid: outFirstPID + uint16(i)}
		}
		c.Check(len(reassembled) == 1 && bytes.Equal(reassembled[0],
			pmtA(0, outFirstPID, expectedMany, nil)),
			"long PMT reassembles to one valid remapped section")

		// A section spanning packets is finished by a pointer field that also
		// starts a fresh section in the same payload (both must be accepted).
		rChain, outChain := newTestRemuxer(c)
		var chainCC uint8
		chainPat := patProgramsA(0, pmtPIDA, false)
		chainPackets := psiPacketsPUSI(PIDPAT, &chainCC, chainPat)
		c.Check(len(chainPackets) == 1, "fixture PAT fits one packet")
		// Hand-craft a two-packet chain: first packet holds the start of a PMT
		// section, the PUSI packet closes it via a non-zero pointer field and
		// immediately carries another complete section.
		// 40 streams make the section longer than one PSI payload so it spans packets.
		chainPMT0 := pmtA(0, videoPID, buildNStreams(40, 0x0700), nil)
		chainPMT1 := pmtA(1, videoPID, []testPMTES{
			{streamType: byte(StreamTypeH264Video), pid: videoPID},
			{streamType: byte(StreamTypeADTS), pid: audioPID},
		}, nil)
		c.Check(len(chainPMT0) > testPacketSize-5, "chained fixture section must span TS packets")
		// First PUSI packet fills its whole payload with the section start.
		split := testPacketSize - 5 // bytes available after the pointer field
		firstPkt := psiPacket(pmtPIDA, true, chainCC, append([]byte{0}, chainPMT0[:split]...))
		chainCC++
		// The next PUSI packet uses its pointer field to close the old section
		// and immediately carries a second, complete section.
		remaining := chainPMT0[split:]
		secondPayload := make([]byte, 0, 1+len(remaining)+len(chainPMT1))
		secondPayload = append(secondPayload, byte(len(remaining)))
		secondPayload = append(secondPayload, remaining...)
		secondPayload = append(secondPayload, chainPMT1...)
		secondPkt := psiPacket(pmtPIDA, true, chainCC, secondPayload)
		chainCC++
		feedPackets(c, rChain, psiPacketsPUSI(PIDPAT, &chainCC, chainPat), 3)
		feedChunks(c, rChain, firstPkt, 3)
		beforeChain := outChain.Len()
		c.Check(beforeChain == 0, "a half section must not emit tables")
		feedChunks(c, rChain, secondPkt, 3)
		chainOut := parseOutputPackets(c, outChain.Bytes())
		chainPMTSections := collectSections(c, chainOut, outPMTPID)
		c.Check(len(chainPMTSections) == 2, "pointer field must complete the old section and submit the new one")
		c.Check(sectionVersion(chainPMTSections[0]) == 0 && sectionVersion(chainPMTSections[1]) == 1,
			"both chained sections committed in order")

		// PID exhaustion errors without publishing a half table.
		var outExh bytes.Buffer
		rExh, exhBuildErr := NewProgramRemuxer(&outExh, ProgramRemuxConfig{
			ProgramNumber: programA,
			OutputPMTPID:  0x1ffc,
			FirstMediaPID: 0x1ffd, // only 0x1ffd and 0x1ffe remain for media
		})
		c.Check(exhBuildErr == nil, "tight-range remuxer construction: %v", exhBuildErr)
		var exhCC uint8
		exhStreams := []testPMTES{
			{streamType: byte(StreamTypeH264Video), pid: videoPID},
			{streamType: byte(StreamTypeADTS), pid: audioPID},
			{streamType: 0x06, pid: otherPID},
		}
		feedPackets(c, rExh, psiPacketsPUSI(PIDPAT, &exhCC, patProgramsA(0, pmtPIDA, false)), 2)
		beforeExh := outExh.Len()
		_, exhErr := rExh.Write(psiPacketsPUSI(pmtPIDA, &exhCC, pmtA(0, videoPID, exhStreams, nil))[0])
		c.Check(errors.Is(exhErr, ErrRemuxPIDExhausted), "PID exhaustion reported, got %v", exhErr)
		c.Check(outExh.Len() == beforeExh, "no partial table is published on exhaustion")
		c.Check(errors.Is(rExh.Flush(), ErrRemuxPIDExhausted), "exhaustion error is sticky")

		// Bad CRC stops the remuxer and the error is sticky.
		r3, _ := newTestRemuxer(c)
		var cc3 uint8
		feedPackets(c, r3, psiPacketsPUSI(PIDPAT, &cc3, patProgramsA(0, pmtPIDA, false)), 2)
		bad := corruptSection(pmtA(0, videoPID, streams, opaqueDesc))
		_, crcErr := r3.Write(psiPacketsPUSI(pmtPIDA, &cc3, bad)[0])
		c.Check(errors.Is(crcErr, ErrCRC32Mismatch), "bad CRC must report ErrCRC32Mismatch, got %v", crcErr)
		_, again := r3.Write(mediaPacket(videoPID, true, 0, nil, []byte{1}))
		c.Check(errors.Is(again, ErrCRC32Mismatch), "error must be sticky, got %v", again)

		c.Check(r3.Flush() != nil, "Flush after failure keeps the error")
		c.Check(r.Flush() == nil, "healthy remuxer flushes cleanly")
	})
}

// ---------------------------------------------------------------------------
// TestProgramEpochs
// ---------------------------------------------------------------------------

func TestProgramEpochs(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		r, out := newTestRemuxer(c)
		var patCC, pmtCC uint8

		streams := []testPMTES{
			{streamType: byte(StreamTypeH264Video), pid: videoPID},
			{streamType: byte(StreamTypeADTS), pid: audioPID},
		}

		// Initial topology on PMT PID A.
		feedPackets(c, r, psiPacketsPUSI(PIDPAT, &patCC, patProgramsA(0, pmtPIDA, true)), 4)
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmtA(0, videoPID, streams, nil)), 4)
		c.Check(len(collectSections(c, parseOutputPackets(c, out.Bytes()), PIDPAT)) == 1, "initial PAT missing")

		// In-flight video PES: start then continuation, both admitted.
		pesStart := mediaPacket(videoPID, true, 5, pcrAdaptation(1000, 7), pesPacket(0xe0, 9000, bytes.Repeat([]byte{0xa1}, 100)))
		pesCont := mediaPacket(videoPID, false, 6, nil, bytes.Repeat([]byte{0xa2}, 184))
		feedChunks(c, r, pesStart, 6)
		feedChunks(c, r, pesCont, 6)

		// PAT moves the program to PMT PID B: candidate only, old topology lives.
		feedPackets(c, r, psiPacketsPUSI(PIDPAT, &patCC, patProgramsA(1, pmtPIDB, true)), 4)
		feedChunks(c, r, mediaPacket(videoPID, false, 7, nil, bytes.Repeat([]byte{0xa3}, 184)), 6)
		c.Check(true, "media under candidate binding keeps flowing")

		// A foreign PMT on the candidate PID cannot complete the candidate.
		feedPackets(c, r, psiPacketsPUSI(pmtPIDB, &pmtCC, pmtB(0)), 4)
		// Neither can a later PMT on the old PID.
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmtA(1, videoPID, streams, nil)), 4)
		before := out.Len()
		feedChunks(c, r, mediaPacket(videoPID, false, 8, nil, bytes.Repeat([]byte{0xa4}, 184)), 6)
		c.Check(out.Len() > before, "old topology still admits media while candidate is pending")

		// The correct PMT on PID B commits the migration; surviving ES keeps its PID.
		feedPackets(c, r, psiPacketsPUSI(pmtPIDB, &pmtCC, pmtA(2, videoPID, streams, nil)), 4)
		all := parseOutputPackets(c, out.Bytes())
		pmtSecs := collectSections(c, all, outPMTPID)
		c.Check(len(pmtSecs) == 2, "migration emits a second PMT")
		_, _, _, migrated := parseOutputPMT(c, pmtSecs[1])
		c.Check(migrated[0].pid == outFirstPID, "surviving video keeps output PID %#x", migrated[0].pid)
		// Continuation of the existing PES is not truncated by the table switch.
		feedChunks(c, r, mediaPacket(videoPID, false, 9, nil, bytes.Repeat([]byte{0xa5}, 184)), 6)
		all = parseOutputPackets(c, out.Bytes())
		last := all[len(all)-1]
		c.Check(last.pid == outFirstPID && bytes.Equal(last.raw[4:],
			mediaPacket(videoPID, false, 9, nil, bytes.Repeat([]byte{0xa5}, 184))[4:]),
			"surviving PES continuation copied byte for byte")

		// A complete PAT removing the program revokes admission immediately.
		removed := buildPATSection(tsid, 2, 0, 0, []testPATProgram{
			{programNumber: 0, pmtPID: 0x0010},
			{programNumber: programB, pmtPID: pmtPIDB},
		})
		feedChunks(c, r, psiPacketsPUSI(PIDPAT, &patCC, removed)[0], 4)
		before = out.Len()
		feedChunks(c, r, mediaPacket(videoPID, true, 10, nil, pesPacket(0xe0, 9001, []byte("dropped"))), 6)
		feedChunks(c, r, mediaPacket(audioPID, true, 3, nil, pesPacket(0xc0, 9002, []byte("dropped"))), 6)
		c.Check(out.Len() == before, "media must be dropped once the program is removed")

		// Re-establish through a new PAT + PMT: retired output PIDs are not reused.
		newPMTPID := uint16(0x0102)
		feedPackets(c, r, psiPacketsPUSI(PIDPAT, &patCC, patProgramsA(3, newPMTPID, false)), 4)
		feedPackets(c, r, psiPacketsPUSI(newPMTPID, &pmtCC, pmtA(3, videoPID, streams[:1], nil)), 4)
		all = parseOutputPackets(c, out.Bytes())
		pmtSecs = collectSections(c, all, outPMTPID)
		_, _, _, reestablished := parseOutputPMT(c, pmtSecs[len(pmtSecs)-1])
		c.Check(reestablished[0].pid == outFirstPID+2, "re-added video gets fresh PID %#x", reestablished[0].pid)
		// The fresh identity waits for a PUSI payload.
		before = out.Len()
		feedChunks(c, r, mediaPacket(videoPID, false, 11, nil, []byte{0xaa}), 6)
		c.Check(out.Len() == before, "non-PUSI packet before the new PES start is dropped")
		feedChunks(c, r, mediaPacket(videoPID, true, 12, nil, pesPacket(0xe0, 9003, []byte("restart"))), 6)
		c.Check(out.Len() > before, "PUSI payload starts the new identity")

		// Track removal and re-add on an unchanged binding: PID retired then fresh.
		r2, out2 := newTestRemuxer(c)
		var c2 uint8
		feedPackets(c, r2, psiPacketsPUSI(PIDPAT, &c2, patProgramsA(0, pmtPIDA, false)), 3)
		feedPackets(c, r2, psiPacketsPUSI(pmtPIDA, &c2, pmtA(0, videoPID, streams, nil)), 3)
		feedChunks(c, r2, mediaPacket(videoPID, true, 0, nil, pesPacket(0xe0, 1, []byte("v"))), 3)
		// Audio removed, video survives.
		feedPackets(c, r2, psiPacketsPUSI(pmtPIDA, &c2, pmtA(1, videoPID, streams[:1], nil)), 3)
		pkts2 := parseOutputPackets(c, out2.Bytes())
		secs2 := collectSections(c, pkts2, outPMTPID)
		_, _, _, afterRemoval := parseOutputPMT(c, secs2[len(secs2)-1])
		c.Check(len(afterRemoval) == 1 && afterRemoval[0].pid == outFirstPID, "video PID survives audio removal")
		// Video PES needs no new PUSI.
		before = out2.Len()
		feedChunks(c, r2, mediaPacket(videoPID, false, 1, nil, []byte{0xbb}), 3)
		c.Check(out2.Len() > before, "surviving video PES is not cut by the table change")
		// Audio re-added with the same type: a new PID is allocated, the old one stays retired.
		feedPackets(c, r2, psiPacketsPUSI(pmtPIDA, &c2, pmtA(2, videoPID, streams, nil)), 3)
		pkts2 = parseOutputPackets(c, out2.Bytes())
		secs2 = collectSections(c, pkts2, outPMTPID)
		_, _, _, afterReadd := parseOutputPMT(c, secs2[len(secs2)-1])
		c.Check(afterReadd[1].pid != outFirstPID+1, "re-added audio cannot reuse retired PID, got %#x", afterReadd[1].pid)

		// Stream type change retires the identity and waits for a fresh PES start.
		changed := []testPMTES{{streamType: byte(StreamTypeH265Video), pid: videoPID}}
		feedPackets(c, r2, psiPacketsPUSI(pmtPIDA, &c2, pmtA(3, videoPID, changed, nil)), 3)
		before = out2.Len()
		feedChunks(c, r2, mediaPacket(videoPID, false, 2, nil, []byte{0xcc}), 3)
		c.Check(out2.Len() == before, "type-changed ES waits for PUSI")
		feedChunks(c, r2, mediaPacket(videoPID, true, 3, nil, pesPacket(0xe0, 2, []byte("new"))), 3)
		c.Check(out2.Len() > before, "type-changed ES restarts at PUSI")
		pkts2 = parseOutputPackets(c, out2.Bytes())
		secs2 = collectSections(c, pkts2, outPMTPID)
		_, _, _, changedStreams := parseOutputPMT(c, secs2[len(secs2)-1])
		c.Check(changedStreams[0].streamType == byte(StreamTypeH265Video), "new stream type published")

		// A half-arrived PMT never changes the active tracks.
		r3, out3 := newTestRemuxer(c)
		var c3 uint8
		feedPackets(c, r3, psiPacketsPUSI(PIDPAT, &c3, patProgramsA(0, pmtPIDA, false)), 3)
		feedPackets(c, r3, psiPacketsPUSI(pmtPIDA, &c3, pmtA(0, videoPID, streams[:1], nil)), 3)
		manyStreams := make([]testPMTES, 40)
		for i := range manyStreams {
			manyStreams[i] = testPMTES{streamType: 0x06, pid: uint16(0x600 + i)}
		}
		half := pmtA(1, videoPID, manyStreams, nil)
		// One PUSI packet carrying only the start of the section; zero stuffing
		// (not 0xff) keeps the assembler waiting for the remaining bytes.
		partial := make([]byte, testPacketSize)
		partial[0] = syncByte
		partial[1] = 0x40 | hiByte(pmtPIDA)
		partial[2] = loByte(pmtPIDA)
		partial[3] = 0x10 | (c3 & 0x0f)
		c3++
		copy(partial[4:], append([]byte{0}, half[:60]...))
		c.Check(len(half) > 61, "fixture section must really be truncated")
		feedChunks(c, r3, partial, 3)
		// Start the video PES before probing continuations.
		feedChunks(c, r3, mediaPacket(videoPID, true, 0, nil, pesPacket(0xe0, 3, []byte("start"))), 3)
		before = out3.Len()
		feedChunks(c, r3, mediaPacket(videoPID, false, 1, nil, []byte{0xdd}), 3)
		c.Check(out3.Len() > before, "old topology remains active with a half PMT pending")
		c.Check(errors.Is(r3.Flush(), ErrRemuxPartialPSISection), "flush mid-section must error")
	})
}

// ---------------------------------------------------------------------------
// TestProgramMedia
// ---------------------------------------------------------------------------

type shortWriter struct{ buf bytes.Buffer }

func (w *shortWriter) Write(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	w.buf.Write(p[:1])
	return 1, nil
}

type failWriter struct {
	buf   bytes.Buffer
	calls int
}

func (w *failWriter) Write(p []byte) (int, error) {
	w.calls++
	if w.calls == 1 {
		n := 1
		if n > len(p) {
			n = len(p)
		}
		w.buf.Write(p[:n])
		return n, errors.New("boom")
	}
	return w.buf.Write(p)
}

func TestProgramMedia(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		r, out := newTestRemuxer(c)
		var patCC, pmtCC uint8
		streams := []testPMTES{
			{streamType: byte(StreamTypeH264Video), pid: videoPID, descs: regDesc},
			{streamType: byte(StreamTypeADTS), pid: audioPID, descs: isoDesc},
		}
		feedPackets(c, r, psiPacketsPUSI(PIDPAT, &patCC, patProgramsA(0, pmtPIDA, true)), 4)
		feedPackets(c, r, psiPacketsPUSI(pmtPIDA, &pmtCC, pmtA(0, videoPID, streams, opaqueDesc)), 4)
		tableBytes := out.Len()

		// Foreign program traffic, null packets and input tables never appear.
		feedChunks(c, r, mediaPacket(otherPID, true, 0, nil, []byte{0, 0, 1, 0xbd, 'x'}), 5)
		nullPkt := make([]byte, testPacketSize)
		nullPkt[0] = syncByte
		nullPkt[1] = 0x1f
		nullPkt[2] = 0xff
		feedChunks(c, r, nullPkt, 5)
		feedPackets(c, r, psiPacketsPUSI(pmtPIDB, &pmtCC, pmtB(1)), 5)
		c.Check(out.Len() == tableBytes, "foreign media, null and foreign table packets must be dropped")

		// First media packet: PCR in adaptation field, PES with PTS, opaque body.
		pcrBase := uint64(8589934587)
		pcrExt := uint16(299)
		body := append([]byte{0x10, 0x20, 0x30}, bytes.Repeat([]byte{0x77}, 120)...)
		in1 := mediaPacket(videoPID, true, 9, pcrAdaptation(pcrBase, pcrExt), pesPacket(0xe0, 7654321, body))
		feedChunks(c, r, in1, 1) // one-byte writes: input may be cut anywhere
		in2 := mediaPacket(videoPID, false, 10, nil, bytes.Repeat([]byte{0x66}, 184))
		feedChunks(c, r, in2, 19)

		pkts := parseOutputPackets(c, out.Bytes())
		media := pkts[2:]
		c.Check(len(media) == 2, "two media packets expected, got %d", len(media))
		m0, m1 := media[0], media[1]
		c.Check(m0.pid == outFirstPID && m1.pid == outFirstPID, "media PID rewritten")
		c.Check(m0.cc == 0 && m1.cc == 1, "first payload CC is 0, then increments")
		c.Check(bytes.Equal(m0.raw[4:], in1[4:]), "adaptation field, PES header and body copied verbatim")
		c.Check(bytes.Equal(m1.raw[4:], in2[4:]), "continuation payload copied verbatim")
		gotBase, gotExt, hasPCR := extractPCR(m0.afContent)
		c.Check(hasPCR && gotBase == pcrBase && gotExt == pcrExt, "PCR preserved: %d/%d", gotBase, gotExt)
		gotPTS, hasPTS := extractPTS(m0.payload)
		c.Check(hasPTS && gotPTS == 7654321, "PTS preserved: %d", gotPTS)

		// Adaptation-only packet repeats the last payload CC.
		afOnly := mediaPacket(videoPID, false, 11, pcrAdaptation(pcrBase+1, 0), nil)
		feedChunks(c, r, afOnly, 33)
		pkts = parseOutputPackets(c, out.Bytes())
		afPkt := pkts[len(pkts)-1]
		c.Check(!afPkt.hasPay && afPkt.cc == 1, "adaptation-only CC repeats 1, got %d", afPkt.cc)
		c.Check(bytes.Equal(afPkt.raw[4:], afOnly[4:]), "adaptation-only bytes preserved")
		// Next payload continues at CC 2.
		in3 := mediaPacket(videoPID, false, 12, nil, []byte{0x55})
		feedChunks(c, r, in3, 8)
		pkts = parseOutputPackets(c, out.Bytes())
		c.Check(pkts[len(pkts)-1].cc == 2, "payload CC resumes at 2")

		// Every emitted PID belongs to the advertised set.
		allowed := map[uint16]bool{PIDPAT: true, outPMTPID: true, outFirstPID: true, outFirstPID + 1: true}
		for _, p := range parseOutputPackets(c, out.Bytes()) {
			c.Check(allowed[p.pid], "unexpected output PID %#x", p.pid)
		}

		// Standalone PCR identity: adaptation-only output needs no PUSI.
		r2, out2 := newTestRemuxer(c)
		var c2 uint8
		standalone := []testPMTES{{streamType: byte(StreamTypeH264Video), pid: videoPID}}
		feedPackets(c, r2, psiPacketsPUSI(PIDPAT, &c2, patProgramsA(0, pmtPIDA, false)), 4)
		feedPackets(c, r2, psiPacketsPUSI(pmtPIDA, &c2, pmtA(0, pcrOnlyPID, standalone, nil)), 4)
		pkts2 := parseOutputPackets(c, out2.Bytes())
		sec := collectSections(c, pkts2, outPMTPID)
		_, pcrOut, _, _ := parseOutputPMT(c, sec[0])
		c.Check(pcrOut == outFirstPID+1, "standalone PCR gets its own PID %#x", pcrOut)
		// Non-PUSI, adaptation-only PCR packet is emitted immediately.
		pcrIn := mediaPacket(pcrOnlyPID, false, 0, pcrAdaptation(4242, 9), nil)
		feedChunks(c, r2, pcrIn, 3)
		pkts2 = parseOutputPackets(c, out2.Bytes())
		clockPkt := pkts2[len(pkts2)-1]
		c.Check(clockPkt.pid == outFirstPID+1 && !clockPkt.pusi && clockPkt.cc == 0,
			"standalone PCR adaptation-only packet starts at CC 0 without PUSI")
		cb, ce, ok := extractPCR(clockPkt.afContent)
		c.Check(ok && cb == 4242 && ce == 9, "standalone PCR preserved")

		// The clock PID becomes an ES: old identity retires, a new PID waits for PUSI.
		nowES := []testPMTES{
			{streamType: byte(StreamTypeH264Video), pid: videoPID},
			{streamType: 0x06, pid: pcrOnlyPID},
		}
		feedPackets(c, r2, psiPacketsPUSI(pmtPIDA, &c2, pmtA(1, pcrOnlyPID, nowES, nil)), 4)
		pkts2 = parseOutputPackets(c, out2.Bytes())
		sec = collectSections(c, pkts2, outPMTPID)
		_, _, _, ess := parseOutputPMT(c, sec[len(sec)-1])
		var newClockPID uint16
		for _, es := range ess {
			if es.streamType == 0x06 {
				newClockPID = es.pid
			}
		}
		c.Check(newClockPID != 0 && newClockPID != outFirstPID+1, "former clock PID gets a fresh identity %#x", newClockPID)
		before := out2.Len()
		feedChunks(c, r2, mediaPacket(pcrOnlyPID, false, 1, nil, []byte{0x01}), 3)
		c.Check(out2.Len() == before, "old clock identity must not be treated as media start")
		feedChunks(c, r2, mediaPacket(pcrOnlyPID, true, 2, nil, pesPacket(0xbd, 55, []byte("pes"))), 3)
		c.Check(out2.Len() > before, "new ES identity starts at PUSI")
		pkts2 = parseOutputPackets(c, out2.Bytes())
		c.Check(pkts2[len(pkts2)-1].pid == newClockPID, "new ES uses the freshly allocated PID")

		// PCR 0x1fff means no PCR identity.
		r3, out3 := newTestRemuxer(c)
		var c3 uint8
		feedPackets(c, r3, psiPacketsPUSI(PIDPAT, &c3, patProgramsA(0, pmtPIDA, false)), 4)
		feedPackets(c, r3, psiPacketsPUSI(pmtPIDA, &c3, pmtA(0, 0x1fff, standalone, nil)), 4)
		sec3 := collectSections(c, parseOutputPackets(c, out3.Bytes()), outPMTPID)
		_, noPCR, _, ess3 := parseOutputPMT(c, sec3[0])
		c.Check(noPCR == 0x1fff, "PMT keeps 0x1fff when no PCR is present")
		c.Check(len(ess3) == 1 && ess3[0].pid == outFirstPID, "only the ES gets an identity")

		// Scrambled input is rejected.
		r4, _ := newTestRemuxer(c)
		var c4 uint8
		feedPackets(c, r4, psiPacketsPUSI(PIDPAT, &c4, patProgramsA(0, pmtPIDA, false)), 4)
		feedPackets(c, r4, psiPacketsPUSI(pmtPIDA, &c4, pmtA(0, videoPID, streams, nil)), 4)
		_, scramErr := r4.Write(scrambledMediaPacket(videoPID, 0))
		c.Check(errors.Is(scramErr, ErrRemuxUnsupportedInput), "scrambled packet rejected: %v", scramErr)

		// CA descriptor is rejected.
		r5, _ := newTestRemuxer(c)
		var c5 uint8
		feedPackets(c, r5, psiPacketsPUSI(PIDPAT, &c5, patProgramsA(0, pmtPIDA, false)), 4)
		caStreams := []testPMTES{{streamType: byte(StreamTypeH264Video), pid: videoPID, descs: caDesc}}
		_, caErr := r5.Write(psiPacketsPUSI(pmtPIDA, &c5, pmtA(0, videoPID, caStreams, nil))[0])
		c.Check(errors.Is(caErr, ErrRemuxUnsupportedInput), "CA descriptor rejected: %v", caErr)

		// Flush conditions.
		r6, _ := newTestRemuxer(c)
		_, flushErr := r6.Write(make([]byte, 100))
		c.Check(flushErr == nil, "partial TS packet buffered")
		c.Check(errors.Is(r6.Flush(), ErrRemuxPartialTSData), "partial TS packet flush errors")
		r7, _ := newTestRemuxer(c)
		var c7 uint8
		feedChunks(c, r7, psiPacketsPUSI(PIDPAT, &c7, buildPATSection(tsid, 0, 0, 1,
			[]testPATProgram{{programNumber: programA, pmtPID: pmtPIDA}}))[0], 4)
		c.Check(r7.Flush() == nil, "complete section but unfinished version flushes without committing")
		r8, out8 := newTestRemuxer(c)
		var c8 uint8
		feedPackets(c, r8, psiPacketsPUSI(PIDPAT, &c8, patProgramsA(0, pmtPIDA, false)), 4)
		feedPackets(c, r8, psiPacketsPUSI(pmtPIDA, &c8, pmtA(0, videoPID, streams, nil)), 4)
		feedChunks(c, r8, mediaPacket(videoPID, true, 0, nil, pesPacket(0xe0, 1, bytes.Repeat([]byte{1}, 184))), 4)
		c.Check(r8.Flush() == nil, "flushing mid-PES at a packet boundary is legal")
		c.Check(out8.Len()%testPacketSize == 0, "output always consists of full packets")

		// Short writes are retried; failed writes stay failed.
		sw := &shortWriter{}
		rs, err := NewProgramRemuxer(sw, ProgramRemuxConfig{ProgramNumber: programA, OutputPMTPID: outPMTPID, FirstMediaPID: outFirstPID})
		c.Check(err == nil, "short-writer remuxer construction: %v", err)
		var cs uint8
		feedPackets(c, rs, psiPacketsPUSI(PIDPAT, &cs, patProgramsA(0, pmtPIDA, false)), 4)
		feedPackets(c, rs, psiPacketsPUSI(pmtPIDA, &cs, pmtA(0, videoPID, streams, nil)), 4)
		c.Check(rs.Flush() == nil, "short-writer flush")
		c.Check(bytes.Equal(sw.buf.Bytes(), mustRemux(c, patProgramsA(0, pmtPIDA, false), pmtA(0, videoPID, streams, nil))),
			"short writes produce the same stream")

		fw := &failWriter{}
		rf, err := NewProgramRemuxer(fw, ProgramRemuxConfig{ProgramNumber: programA, OutputPMTPID: outPMTPID, FirstMediaPID: outFirstPID})
		c.Check(err == nil, "fail-writer remuxer construction: %v", err)
		var cf uint8
		// The PAT alone produces no output; the first write happens at PMT commit.
		feedPackets(c, rf, psiPacketsPUSI(PIDPAT, &cf, patProgramsA(0, pmtPIDA, false)), 4)
		_, failErr := rf.Write(psiPacketsPUSI(pmtPIDA, &cf, pmtA(0, videoPID, streams, nil))[0])
		c.Check(failErr != nil, "writer failure is propagated")
		calls := fw.calls
		_, sticky := rf.Write(mediaPacket(videoPID, true, 0, nil, []byte{1}))
		c.Check(sticky != nil && fw.calls == calls, "sticky error does not touch the writer again")
	})
}

// mustRemux runs a minimal two-table scenario into a buffer and returns it.
func mustRemux(c *taskcheck.C, pat, pmt []byte) []byte {
	var buf bytes.Buffer
	r, err := NewProgramRemuxer(&buf, ProgramRemuxConfig{ProgramNumber: programA, OutputPMTPID: outPMTPID, FirstMediaPID: outFirstPID})
	c.Check(err == nil, "helper remuxer construction: %v", err)
	var cc uint8
	if _, err := r.Write(psiPacketsPUSI(PIDPAT, &cc, pat)[0]); err != nil {
		c.Check(false, "helper PAT write: %v", err)
	}
	for _, p := range psiPacketsPUSI(pmtPIDA, &cc, pmt) {
		if _, err := r.Write(p); err != nil {
			c.Check(false, "helper PMT write: %v", err)
		}
	}
	return buf.Bytes()
}
