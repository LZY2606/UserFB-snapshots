package astits

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"testing"

	"github.com/asticode/go-astits/task_support/taskcheck"
)

const (
	prTestPacketSize = 188
	prTestSync       = 0x47
)

type prTestProgram struct {
	number uint16
	pmtPID uint16
}

type prTestES struct {
	streamType  byte
	pid         uint16
	descriptors []byte
}

func prTestCRC(data []byte) uint32 {
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

func prTestSection(tableID byte, extension uint16, version uint8, currentNext bool, sectionNumber, lastNumber uint8, payload []byte) []byte {
	sectionLength := uint16(9 + len(payload))
	s := []byte{tableID, 0xb0 | byte(sectionLength>>8), byte(sectionLength)}
	s = binary.BigEndian.AppendUint16(s, extension)
	versionByte := byte(0xc0) | ((version & 0x1f) << 1)
	if currentNext {
		versionByte |= 0x01
	}
	s = append(s, versionByte, sectionNumber, lastNumber)
	s = append(s, payload...)
	crc := prTestCRC(s)
	s = append(s, byte(crc>>24), byte(crc>>16), byte(crc>>8), byte(crc))
	return s
}

func prTestPATSection(tsID uint16, version uint8, currentNext bool, number, last uint8, programs []prTestProgram) []byte {
	var payload []byte
	for _, p := range programs {
		payload = binary.BigEndian.AppendUint16(payload, p.number)
		payload = binary.BigEndian.AppendUint16(payload, 0xe000|p.pmtPID)
	}
	return prTestSection(0x00, tsID, version, currentNext, number, last, payload)
}

func prTestPMTSection(program uint16, version uint8, currentNext bool, pcrPID uint16, programDescriptors []byte, streams []prTestES) []byte {
	var payload []byte
	payload = binary.BigEndian.AppendUint16(payload, 0xe000|pcrPID)
	payload = binary.BigEndian.AppendUint16(payload, 0xf000|uint16(len(programDescriptors)))
	payload = append(payload, programDescriptors...)
	for _, es := range streams {
		payload = append(payload, es.streamType)
		payload = binary.BigEndian.AppendUint16(payload, 0xe000|es.pid)
		payload = binary.BigEndian.AppendUint16(payload, 0xf000|uint16(len(es.descriptors)))
		payload = append(payload, es.descriptors...)
	}
	return prTestSection(0x02, program, version, currentNext, 0, 0, payload)
}

func prTestPacket(pid uint16, pusi bool, cc byte, payload []byte) []byte {
	p := make([]byte, prTestPacketSize)
	for i := range p {
		p[i] = 0xff
	}
	p[0] = prTestSync
	p[1] = byte(pid >> 8)
	if pusi {
		p[1] |= 0x40
	}
	p[2] = byte(pid)
	p[3] = 0x10 | (cc & 0x0f)
	if len(payload) > prTestPacketSize-4 {
		panic("payload too large")
	}
	copy(p[4:], payload)
	return p
}

func prTestAdaptationOnlyPCRPacket(pid uint16, cc byte, pcr [6]byte) []byte {
	p := make([]byte, prTestPacketSize)
	for i := range p {
		p[i] = 0xff
	}
	p[0] = prTestSync
	p[1] = byte(pid >> 8)
	p[2] = byte(pid)
	p[3] = 0x20 | (cc & 0x0f)
	p[4] = 183 // adaptation field fills the complete packet
	p[5] = 0x10
	copy(p[6:12], pcr[:])
	return p
}

func prTestAdaptationPayloadPacket(pid uint16, pusi bool, cc byte, af []byte, payload []byte) []byte {
	if len(af)+len(payload)+5 != prTestPacketSize {
		panic(fmt.Sprintf("bad adaptation packet lengths af=%d payload=%d", len(af), len(payload)))
	}
	p := make([]byte, prTestPacketSize)
	p[0] = prTestSync
	p[1] = byte(pid >> 8)
	if pusi {
		p[1] |= 0x40
	}
	p[2] = byte(pid)
	p[3] = 0x30 | (cc & 0x0f)
	p[4] = byte(len(af))
	copy(p[5:], af)
	copy(p[5+len(af):], payload)
	return p
}

func prTestPSIPackets(pid uint16, startCC byte, chunks ...[]byte) [][]byte {
	var out [][]byte
	cc := startCC
	for i, chunk := range chunks {
		var payload []byte
		if i == 0 {
			payload = []byte{0x00}
		}
		payload = append(payload, chunk...)
		out = append(out, prTestPacket(pid, i == 0, cc, payload))
		cc = (cc + 1) & 15
	}
	return out
}

func prTestWriteChunked(r *ProgramRemuxer, stream []byte, chunkSize int) (int, error) {
	if chunkSize <= 0 {
		chunkSize = 1
	}
	total := 0
	for len(stream) > 0 {
		n := chunkSize
		if n > len(stream) {
			n = len(stream)
		}
		wn, err := r.Write(stream[:n])
		total += wn
		if err != nil {
			return total, err
		}
		stream = stream[n:]
	}
	return total, nil
}

type prParsedPacket struct {
	raw            []byte
	pid            uint16
	pusi           bool
	cc             byte
	hasAdaptation  bool
	hasPayload     bool
	adaptationOnly bool
	payloadOffset  int
}

func prParsePackets(c *taskcheck.C, data []byte) []prParsedPacket {
	if len(data)%prTestPacketSize != 0 {
		c.Check(false, "output length %d is not a whole number of TS packets", len(data))
	}
	var out []prParsedPacket
	for len(data) >= prTestPacketSize {
		pkt := data[:prTestPacketSize]
		data = data[prTestPacketSize:]
		if pkt[0] != prTestSync {
			c.Check(false, "bad sync byte 0x%02x", pkt[0])
			continue
		}
		h := prParsedPacket{
			raw:           append([]byte(nil), pkt...),
			pid:           uint16(pkt[1]&0x1f)<<8 | uint16(pkt[2]),
			pusi:          pkt[1]&0x40 != 0,
			cc:            pkt[3] & 0x0f,
			hasAdaptation: pkt[3]&0x20 != 0,
			hasPayload:    pkt[3]&0x10 != 0,
		}
		h.adaptationOnly = h.hasAdaptation && !h.hasPayload
		h.payloadOffset = 4
		if h.hasAdaptation {
			h.payloadOffset = 5 + int(pkt[4])
		}
		out = append(out, h)
	}
	return out
}

func prPacketsForPID(packets []prParsedPacket, pid uint16) []prParsedPacket {
	var out []prParsedPacket
	for _, p := range packets {
		if p.pid == pid {
			out = append(out, p)
		}
	}
	return out
}

func prCCSequence(packets []prParsedPacket) []byte {
	var cc []byte
	for _, p := range packets {
		cc = append(cc, p.cc)
	}
	return cc
}

func prSectionFromPackets(c *taskcheck.C, packets []prParsedPacket, pid uint16) []byte {
	ps := prPacketsForPID(packets, pid)
	if len(ps) == 0 {
		c.Check(false, "no output packets for PID 0x%04x", pid)
		return nil
	}
	var stream []byte
	for i, p := range ps {
		if !p.hasPayload {
			c.Check(false, "table packet has no payload")
			return nil
		}
		payload := p.raw[p.payloadOffset:]
		if i == 0 {
			if !p.pusi || len(payload) < 1 || payload[0] != 0 {
				c.Check(false, "first table packet lacks pointer=0/PUSI")
				return nil
			}
			stream = append(stream, payload[1:]...)
		} else {
			if p.pusi {
				c.Check(false, "continuation table packet has PUSI")
			}
			stream = append(stream, payload...)
		}
	}
	if len(stream) < 3 {
		c.Check(false, "table stream too short")
		return nil
	}
	sl := int(uint16(stream[1]&0x0f)<<8 | uint16(stream[2]))
	total := sl + 3
	if total > len(stream) {
		c.Check(false, "table section length %d exceeds reassembled %d", total, len(stream))
		return nil
	}
	section := stream[:total]
	wantCRC := binary.BigEndian.Uint32(section[len(section)-4:])
	c.Check(prTestCRC(section[:len(section)-4]) == wantCRC, "output section CRC mismatch")
	return section
}

func prCheckCC(c *taskcheck.C, packets []prParsedPacket, expected []byte, message string) {
	got := prCCSequence(packets)
	c.Check(bytes.Equal(got, expected), "%s: expected CC %v, got %v", message, expected, got)
}

type prShortWriter struct {
	buf      *bytes.Buffer
	maxWrite int
}

func (w *prShortWriter) Write(p []byte) (int, error) {
	n := len(p)
	if w.maxWrite > 0 && n > w.maxWrite {
		n = w.maxWrite
	}
	return w.buf.Write(p[:n])
}

type prFailWriter struct {
	err error
}

func (w *prFailWriter) Write([]byte) (int, error) { return 0, w.err }

func TestProgramTables(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		_, err := NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: 0, OutputPMTPID: 0x30, FirstMediaPID: 0x40})
		c.Check(err != nil, "program zero must be rejected")
		_, err = NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x1f, FirstMediaPID: 0x40})
		c.Check(err != nil, "low PMT PID must be rejected")
		_, err = NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x30, FirstMediaPID: 0x1fff})
		c.Check(err != nil, "high media PID must be rejected")

		// A two-section PAT lists the selected program and another program.
		pat0 := prTestPATSection(0x1234, 3, true, 0, 1, []prTestProgram{{number: 1, pmtPID: 0x1000}})
		pat1 := prTestPATSection(0x1234, 3, true, 1, 1, []prTestProgram{{number: 2, pmtPID: 0x2000}})
		patPayload := append([]byte{0x00}, pat0...)
		patPayload = append(patPayload, pat1...)
		patPacket := prTestPacket(0x0000, true, 7, patPayload)

		unknownDescriptor := append([]byte{0x80, 160}, bytes.Repeat([]byte{0xa5}, 160)...)
		pmt := prTestPMTSection(1, 9, true, 0x101,
			[]byte{0x05, 0x04, 'C', 'U', 'E', 'I'},
			[]prTestES{
				{streamType: 0x1b, pid: 0x101, descriptors: []byte{0x0a, 0x03, 'e', 'n', 'g'}},
				{streamType: 0x0f, pid: 0x102, descriptors: unknownDescriptor},
			})
		pmtPackets := prTestPSIPackets(0x1000, 2, pmt[:183], pmt[183:])

		otherPMT := prTestPMTSection(2, 1, true, 0x201, nil, []prTestES{{streamType: 0x1b, pid: 0x201}})
		otherPackets := prTestPSIPackets(0x2000, 4, otherPMT)
		otherMedia := prTestPacket(0x201, true, 8, bytes.Repeat([]byte{0x22}, 184))

		var input []byte
		input = append(input, patPacket...)
		for _, p := range pmtPackets {
			input = append(input, p...)
		}
		for _, p := range otherPackets {
			input = append(input, p...)
		}
		input = append(input, otherMedia...)

		var out bytes.Buffer
		r, err := NewProgramRemuxer(&out, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x0030, FirstMediaPID: 0x0040})
		c.Check(err == nil, "NewProgramRemuxer failed: %v", err)
		n, err := prTestWriteChunked(r, input, 1)
		c.Check(err == nil, "chunked Write failed: %v", err)
		c.Check(n == len(input), "Write consumed %d/%d bytes", n, len(input))
		c.Check(r.Flush() == nil, "Flush failed")

		packets := prParsePackets(c, out.Bytes())
		c.Check(len(packets) == 3, "expected one PAT and two PMT packets, got %d", len(packets))
		patOut := prSectionFromPackets(c, packets, 0)
		c.Check(patOut != nil && patOut[0] == 0x00, "output PAT table id wrong")
		if patOut != nil {
			c.Check(binary.BigEndian.Uint16(patOut[3:5]) == 0x1234, "TSID not preserved")
			c.Check((patOut[5]>>1)&31 == 0, "first output PAT version is %d", (patOut[5]>>1)&31)
			c.Check(patOut[5]&1 == 1, "output PAT current_next=1")
			c.Check(binary.BigEndian.Uint16(patOut[8:10]) == 1, "output PAT program number wrong")
			c.Check(binary.BigEndian.Uint16(patOut[10:12])&0x1fff == 0x30, "output PAT PMT PID wrong")
		}
		pmtOut := prSectionFromPackets(c, packets, 0x30)
		c.Check(pmtOut != nil && pmtOut[0] == 0x02, "output PMT table id wrong")
		if pmtOut != nil {
			c.Check(binary.BigEndian.Uint16(pmtOut[3:5]) == 1, "output PMT program number wrong")
			c.Check((pmtOut[5]>>1)&31 == 0, "first output PMT version is %d", (pmtOut[5]>>1)&31)
			c.Check(binary.BigEndian.Uint16(pmtOut[8:10])&0x1fff == 0x40, "PCR/ES PID mapping wrong: %04x", binary.BigEndian.Uint16(pmtOut[8:10]))
			c.Check(bytes.Contains(pmtOut, unknownDescriptor), "opaque ES descriptor bytes were not copied")
			c.Check(bytes.Contains(pmtOut, []byte{0x0a, 0x03, 'e', 'n', 'g'}), "ISO639 descriptor not copied")
			c.Check(bytes.Contains(pmtOut, []byte{0x05, 0x04, 'C', 'U', 'E', 'I'}), "registration descriptor not copied")
			c.Check(bytes.Contains(pmtOut, []byte{0x1b, 0xe0, 0x40}), "video stream type/PID not represented")
			c.Check(bytes.Contains(pmtOut, []byte{0x0f, 0xe0, 0x41}), "audio stream type/PID not represented")
		}
		prCheckCC(c, prPacketsForPID(packets, 0), []byte{0}, "PAT CC")
		prCheckCC(c, prPacketsForPID(packets, 0x30), []byte{0, 1}, "long PMT CC")
		c.Check(len(prPacketsForPID(packets, 0x201)) == 0, "other-program media was output")

		// A PUSI pointer completes the old long section before carrying a new
		// section in the same TS packet sequence.
		var manyPrograms []prTestProgram
		for i := 0; i < 50; i++ {
			manyPrograms = append(manyPrograms, prTestProgram{number: uint16(1000 + i), pmtPID: uint16(0x2000 + i)})
		}
		longPAT0 := prTestPATSection(0x4321, 4, true, 0, 1, manyPrograms)
		pointerPAT1 := prTestPATSection(0x4321, 4, true, 1, 1, []prTestProgram{{number: 1, pmtPID: 0x1800}})
		c.Check(len(longPAT0) > 183, "test PAT section should be long")
		pointerFirstPayload := append([]byte{0x00}, longPAT0[:183]...)
		pointerFirst := prTestPacket(0, true, 0, pointerFirstPayload)
		remaining := longPAT0[183:]
		pointerSecondPayload := []byte{byte(len(remaining))}
		pointerSecondPayload = append(pointerSecondPayload, remaining...)
		pointerSecondPayload = append(pointerSecondPayload, pointerPAT1...)
		pointerSecond := prTestPacket(0, true, 1, pointerSecondPayload)
		pointerPMT := prTestPacket(0x1800, true, 0, append([]byte{0}, prTestPMTSection(1, 0, true, 0x190, nil, []prTestES{{streamType: 0x1b, pid: 0x190}})...))

		var pointerOut bytes.Buffer
		rPointer, err := NewProgramRemuxer(&pointerOut, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x0060, FirstMediaPID: 0x0070})
		c.Check(err == nil, "pointer constructor")
		_, err = rPointer.Write(append(append(pointerFirst, pointerSecond...), pointerPMT...))
		c.Check(err == nil, "pointer-field sequence failed: %v", err)
		pointerPackets := prParsePackets(c, pointerOut.Bytes())
		pointerPATOut := prSectionFromPackets(c, pointerPackets, 0)
		c.Check(pointerPATOut != nil && binary.BigEndian.Uint16(pointerPATOut[3:5]) == 0x4321, "pointer PAT not committed")
		c.Check(rPointer.Flush() == nil, "pointer Flush failed")

		// Two PAT versions interleave before either complete set is present.
		rVersions, err := NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x30, FirstMediaPID: 0x40})
		c.Check(err == nil, "version constructor")
		patV50 := prTestPATSection(0x7777, 5, true, 0, 1, nil)
		patV51 := prTestPATSection(0x7777, 5, true, 1, 1, []prTestProgram{{number: 1, pmtPID: 0x1000}})
		patV60 := prTestPATSection(0x7777, 6, true, 0, 1, nil)
		patV61 := prTestPATSection(0x7777, 6, true, 1, 1, []prTestProgram{{number: 1, pmtPID: 0x1000}})
		makePATPacket := func(sec []byte) []byte { return prTestPacket(0, true, 0, append([]byte{0}, sec...)) }
		_, _ = rVersions.Write(makePATPacket(patV50))
		c.Check(rVersions.patVer == nil, "incomplete PAT v5 committed")
		c.Check(rVersions.Flush() == nil, "complete section/incomplete version should flush cleanly")
		_, _ = rVersions.Write(makePATPacket(patV60))
		c.Check(rVersions.patVer == nil, "interleaved PAT v6 section committed early")
		_, _ = rVersions.Write(makePATPacket(patV51))
		c.Check(rVersions.patVer != nil && *rVersions.patVer == 5, "PAT v5 did not commit")
		_, _ = rVersions.Write(makePATPacket(patV61))
		c.Check(rVersions.patVer != nil && *rVersions.patVer == 6, "PAT v6 did not update")
		_, _ = rVersions.Write(makePATPacket(patV60))
		c.Check(*rVersions.patVer == 6, "duplicate PAT section changed version")
		_, _ = rVersions.Write(makePATPacket(prTestPATSection(0x7777, 22, true, 0, 0, nil))) // +16
		c.Check(*rVersions.patVer == 6, "distance 16 was accepted")
		_, _ = rVersions.Write(makePATPacket(prTestPATSection(0x7777, 25, true, 0, 0, nil))) // +19 old
		c.Check(*rVersions.patVer == 6, "old modulo-32 version regressed state")
		_, _ = rVersions.Write(makePATPacket(prTestPATSection(0x7777, 7, true, 0, 0, nil))) // +1
		c.Check(*rVersions.patVer == 7, "forward version did not update")

		// Trailing partial TS packet is a Flush error.
		rHalf, _ := NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x30, FirstMediaPID: 0x40})
		_, err = rHalf.Write([]byte{0x47, 0x00, 0x01})
		c.Check(err == nil, "partial packet Write should buffer")
		c.Check(rHalf.Flush() != nil, "half TS packet did not fail Flush")

		// A partially received single section fails Flush; a complete section
		// from an incomplete version does not.
		rPartialSection, _ := NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x30, FirstMediaPID: 0x40})
		_, _ = rPartialSection.Write(prTestPacket(0, true, 0, append([]byte{0}, longPAT0[:183]...)))
		c.Check(rPartialSection.Flush() != nil, "partial section did not fail Flush")
		rIncompleteVersion, _ := NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x30, FirstMediaPID: 0x40})
		_, _ = rIncompleteVersion.Write(makePATPacket(patV50))
		c.Check(rIncompleteVersion.Flush() == nil, "complete section of incomplete version failed Flush")

		// A TSID-only PAT change republishes both tables, while a content-
		// identical newer PAT does not.
		var tsOut bytes.Buffer
		rTS, _ := NewProgramRemuxer(&tsOut, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x30, FirstMediaPID: 0x40})
		tsCheckpoint := 0
		tsPAT1 := prTestPacket(0, true, 0, append([]byte{0}, prTestPATSection(0x1111, 1, true, 0, 0, []prTestProgram{{number: 1, pmtPID: 0x1000}})...))
		tsPMT := prTestPacket(0x1000, true, 0, append([]byte{0}, prTestPMTSection(1, 1, true, 0x100, nil, []prTestES{{streamType: 0x1b, pid: 0x100}})...))
		tsPackets := prWriteAndParseNew(c, rTS, &tsOut, &tsCheckpoint, append(tsPAT1, tsPMT...))
		c.Check(len(tsPackets) == 2, "TSID setup tables")
		tsPAT2 := prTestPacket(0, true, 1, append([]byte{0}, prTestPATSection(0x2222, 2, true, 0, 0, []prTestProgram{{number: 1, pmtPID: 0x1000}})...))
		tsPackets = prWriteAndParseNew(c, rTS, &tsOut, &tsCheckpoint, tsPAT2)
		c.Check(len(tsPackets) == 2, "TSID change did not republish both tables")
		tsPATOut := prSectionFromPackets(c, tsPackets, 0)
		tsPMTOut := prSectionFromPackets(c, tsPackets, 0x30)
		c.Check(tsPATOut != nil && binary.BigEndian.Uint16(tsPATOut[3:5]) == 0x2222, "new TSID not output")
		c.Check(tsPATOut != nil && (tsPATOut[5]>>1)&31 == 1, "TSID PAT output version wrong")
		c.Check(tsPMTOut != nil && (tsPMTOut[5]>>1)&31 == 1, "TSID PMT output version wrong")
		tsPAT3 := prTestPacket(0, true, 2, append([]byte{0}, prTestPATSection(0x2222, 3, true, 0, 0, []prTestProgram{{number: 1, pmtPID: 0x1000}})...))
		tsPackets = prWriteAndParseNew(c, rTS, &tsOut, &tsCheckpoint, tsPAT3)
		c.Check(len(tsPackets) == 0, "content-duplicate PAT was output")

		// Bad table CRC is fatal and sticky.
		badPAT := prTestPATSection(0x7777, 8, true, 0, 0, []prTestProgram{{number: 1, pmtPID: 0x1000}})
		badPAT[len(badPAT)-1] ^= 0xff
		rCRC, _ := NewProgramRemuxer(&bytes.Buffer{}, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x30, FirstMediaPID: 0x40})
		_, err = rCRC.Write(makePATPacket(badPAT))
		c.Check(errors.Is(err, ErrCRC32Mismatch), "bad CRC did not return ErrCRC32Mismatch: %v", err)
		_, err = rCRC.Write(makePATPacket(patV60))
		c.Check(err != nil, "remuxer continued after CRC error")
	})
}

type prOutputES struct {
	streamType byte
	pid        uint16
}

func prParseOutputPMT(c *taskcheck.C, section []byte) (uint16, []prOutputES) {
	if section == nil || len(section) < 16 {
		c.Check(false, "bad output PMT")
		return 0, nil
	}
	pcr := binary.BigEndian.Uint16(section[8:10]) & 0x1fff
	infoLen := int(binary.BigEndian.Uint16(section[10:12]) & 0x0fff)
	off := 12 + infoLen
	end := len(section) - 4
	var es []prOutputES
	for off < end {
		if off+5 > end {
			c.Check(false, "short output PMT ES entry")
			break
		}
		es = append(es, prOutputES{streamType: section[off], pid: binary.BigEndian.Uint16(section[off+1:off+3]) & 0x1fff})
		l := int(binary.BigEndian.Uint16(section[off+3:off+5]) & 0x0fff)
		off += 5 + l
	}
	return pcr, es
}

func prWriteAndParseNew(c *taskcheck.C, r *ProgramRemuxer, buf *bytes.Buffer, checkpoint *int, chunk []byte) []prParsedPacket {
	n, err := prTestWriteChunked(r, chunk, 37)
	c.Check(err == nil, "Write failed: %v", err)
	c.Check(n == len(chunk), "short Write result %d/%d", n, len(chunk))
	raw := buf.Bytes()[*checkpoint:]
	*checkpoint = buf.Len()
	return prParsePackets(c, raw)
}

func TestProgramEpochs(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var out bytes.Buffer
		r, err := NewProgramRemuxer(&out, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x0030, FirstMediaPID: 0x0040})
		c.Check(err == nil, "NewProgramRemuxer: %v", err)
		checkpoint := 0

		write := func(chunks ...[]byte) []prParsedPacket {
			var all []prParsedPacket
			for _, chunk := range chunks {
				all = append(all, prWriteAndParseNew(c, r, &out, &checkpoint, chunk)...)
			}
			return all
		}
		sectionPacket := func(pid uint16, cc byte, sec []byte) []byte {
			return prTestPacket(pid, true, cc, append([]byte{0}, sec...))
		}

		pat1 := sectionPacket(0, 0, prTestPATSection(0x4242, 1, true, 0, 0, []prTestProgram{{number: 1, pmtPID: 0x1000}}))
		c.Check(len(write(pat1)) == 0, "PAT alone must not publish tables")

		// Media before the first selected PMT is discarded.
		beforePMT := prTestPacket(0x101, true, 0, bytes.Repeat([]byte{0x10}, 184))
		c.Check(len(write(beforePMT)) == 0, "media before PMT was output")

		pmt1 := sectionPacket(0x1000, 0, prTestPMTSection(1, 1, true, 0x101, nil, []prTestES{
			{streamType: 0x1b, pid: 0x101},
			{streamType: 0x0f, pid: 0x102},
		}))
		packets := write(pmt1)
		c.Check(len(packets) == 2, "first commit should write PAT and PMT, got %d", len(packets))
		patSec := prSectionFromPackets(c, packets, 0)
		pmtSec := prSectionFromPackets(c, packets, 0x30)
		c.Check(patSec != nil && (patSec[5]>>1)&31 == 0, "first PAT output version not zero")
		c.Check(pmtSec != nil && (pmtSec[5]>>1)&31 == 0, "first PMT output version not zero")
		pcr, es := prParseOutputPMT(c, pmtSec)
		c.Check(pcr == 0x40, "initial PCR mapping 0x%04x", pcr)
		c.Check(len(es) == 2 && es[0].pid == 0x40 && es[1].pid == 0x41, "initial ES order/mapping %+v", es)

		videoStart := prTestPacket(0x101, true, 3, bytes.Repeat([]byte{0xaa}, 184))
		videoCont := prTestPacket(0x101, false, 4, bytes.Repeat([]byte{0xbb}, 184))
		mediaPackets := write(videoStart, videoCont)
		c.Check(len(mediaPackets) == 2, "initial video packets not admitted")
		c.Check(mediaPackets[0].pid == 0x40 && mediaPackets[0].cc == 0, "start PID/CC wrong")
		c.Check(bytes.Equal(mediaPackets[0].raw[4:], bytes.Repeat([]byte{0xaa}, 184)), "start media bytes changed")
		c.Check(mediaPackets[1].pid == 0x40 && mediaPackets[1].cc == 1, "continuation CC wrong")

		// PAT migrates PMT to 0x1100. Old topology remains active.
		pat2 := sectionPacket(0, 1, prTestPATSection(0x4242, 2, true, 0, 0, []prTestProgram{{number: 1, pmtPID: 0x1100}, {number: 2, pmtPID: 0x2000}}))
		c.Check(len(write(pat2)) == 0, "PAT PID migration alone published output")
		c.Check(r.patPMTPID == 0x1100 && r.active.inputPMTPID == 0x1000, "candidate binding state wrong")

		oldPMT := sectionPacket(0x1000, 1, prTestPMTSection(1, 1, true, 0x101, nil, []prTestES{
			{streamType: 0x1b, pid: 0x101},
			{streamType: 0x0f, pid: 0x102},
		}))
		oldContinuation := prTestPacket(0x101, false, 5, bytes.Repeat([]byte{0xcc}, 184))
		afterOld := write(oldPMT, oldContinuation)
		c.Check(len(afterOld) == 1, "old PMT replay changed tables")
		c.Check(afterOld[0].pid == 0x40 && afterOld[0].cc == 2, "old topology did not continue: %+v", afterOld)
		c.Check(r.active.inputPMTPID == 0x1000, "old PID incorrectly completed candidate")

		// Complete PMT on the candidate PID switches topology. Video survives;
		// audio 0x102 is replaced by 0x103 and must receive a fresh PID.
		pmt2 := sectionPacket(0x1100, 0, prTestPMTSection(1, 2, true, 0x101, nil, []prTestES{
			{streamType: 0x1b, pid: 0x101},
			{streamType: 0x0f, pid: 0x103},
		}))
		tablePackets := write(pmt2)
		c.Check(len(tablePackets) == 2, "migration did not publish two tables")
		patSec = prSectionFromPackets(c, tablePackets, 0)
		pmtSec = prSectionFromPackets(c, tablePackets, 0x30)
		c.Check((patSec[5]>>1)&31 == 1 && (pmtSec[5]>>1)&31 == 1, "migration output version not 1")
		_, es = prParseOutputPMT(c, pmtSec)
		c.Check(len(es) == 2 && es[0].pid == 0x40 && es[1].pid == 0x42, "surviving/new mapping wrong: %+v", es)
		prCheckCC(c, prPacketsForPID(tablePackets, 0), []byte{1}, "PAT CC after migration")
		prCheckCC(c, prPacketsForPID(tablePackets, 0x30), []byte{1}, "PMT CC after migration")
		c.Check(r.active.inputPMTPID == 0x1100, "candidate PMT did not become active")

		oldAudio := prTestPacket(0x102, false, 0, bytes.Repeat([]byte{0xdd}, 184))
		newAudioEarly := prTestPacket(0x103, false, 0, bytes.Repeat([]byte{0xee}, 184))
		newAudioStart := prTestPacket(0x103, true, 9, bytes.Repeat([]byte{0xff}, 184))
		videoSurvives := prTestPacket(0x101, false, 6, bytes.Repeat([]byte{0x12}, 184))
		mediaPackets = write(oldAudio, newAudioEarly, newAudioStart, videoSurvives)
		c.Check(len(mediaPackets) == 2, "new identity start filtering failed: %d", len(mediaPackets))
		c.Check(mediaPackets[0].pid == 0x42 && mediaPackets[0].cc == 0, "new audio start mapping/CC wrong")
		c.Check(bytes.Equal(mediaPackets[0].raw[4:], bytes.Repeat([]byte{0xff}, 184)), "new audio payload changed")
		c.Check(mediaPackets[1].pid == 0x40 && mediaPackets[1].cc == 3, "surviving video CC wrong: %+v", mediaPackets[1])

		// Complete removal revokes admission immediately.
		pat3 := sectionPacket(0, 2, prTestPATSection(0x4242, 3, true, 0, 0, []prTestProgram{{number: 2, pmtPID: 0x2000}}))
		removedMedia := prTestPacket(0x101, true, 0, bytes.Repeat([]byte{0x33}, 184))
		c.Check(len(write(pat3, removedMedia)) == 0, "removal did not immediately revoke media")
		c.Check(r.active == nil, "active topology remained after PAT removal")

		// Re-add allocates brand-new output PIDs; 0x40/0x41/0x42 are retired.
		pat4 := sectionPacket(0, 3, prTestPATSection(0x4242, 4, true, 0, 0, []prTestProgram{{number: 1, pmtPID: 0x1200}}))
		c.Check(len(write(pat4)) == 0, "PAT readd alone published")
		pmt3 := sectionPacket(0x1200, 0, prTestPMTSection(1, 3, true, 0x101, nil, []prTestES{
			{streamType: 0x1b, pid: 0x101},
			{streamType: 0x0f, pid: 0x102},
		}))
		tablePackets = write(pmt3)
		c.Check(len(tablePackets) == 2, "readd did not publish tables")
		pmtSec = prSectionFromPackets(c, tablePackets, 0x30)
		c.Check((pmtSec[5]>>1)&31 == 2, "readd output version wrong")
		_, es = prParseOutputPMT(c, pmtSec)
		c.Check(len(es) == 2 && es[0].pid == 0x43 && es[1].pid == 0x44, "readd PIDs were not freshly allocated: %+v", es)

		// Remove and re-add one elementary track while program stays.
		pmt4 := sectionPacket(0x1200, 1, prTestPMTSection(1, 4, true, 0x101, nil, []prTestES{{streamType: 0x1b, pid: 0x101}}))
		tablePackets = write(pmt4)
		c.Check(len(tablePackets) == 2, "track removal did not publish")
		pmtSec = prSectionFromPackets(c, tablePackets, 0x30)
		_, es = prParseOutputPMT(c, pmtSec)
		c.Check(len(es) == 1 && es[0].pid == 0x43, "surviving track wrong after removal: %+v", es)

		pmt5 := sectionPacket(0x1200, 2, prTestPMTSection(1, 5, true, 0x101, nil, []prTestES{
			{streamType: 0x1b, pid: 0x101},
			{streamType: 0x0f, pid: 0x102},
		}))
		tablePackets = write(pmt5)
		c.Check(len(tablePackets) == 2, "track readd did not publish")
		pmtSec = prSectionFromPackets(c, tablePackets, 0x30)
		_, es = prParseOutputPMT(c, pmtSec)
		c.Check(len(es) == 2 && es[0].pid == 0x43 && es[1].pid == 0x45, "readded track reused a retired PID: %+v", es)
		c.Check(r.Flush() == nil, "Flush after epochs failed")

		// Exhausting the monotonic output PID space publishes no half table.
		var exhaustOut bytes.Buffer
		re, err := NewProgramRemuxer(&exhaustOut, ProgramRemuxConfig{ProgramNumber: 1, OutputPMTPID: 0x0030, FirstMediaPID: 0x1ffc})
		c.Check(err == nil, "exhaust constructor")
		exPAT := sectionPacket(0, 0, prTestPATSection(1, 0, true, 0, 0, []prTestProgram{{number: 1, pmtPID: 0x1000}}))
		exPMT := sectionPacket(0x1000, 0, prTestPMTSection(1, 0, true, 0x100, nil, []prTestES{
			{streamType: 0x1b, pid: 0x100},
			{streamType: 0x0f, pid: 0x101},
			{streamType: 0x06, pid: 0x102},
		}))
		_, err = re.Write(append(exPAT, exPMT...))
		c.Check(err == nil, "three near-limit PIDs should fit: %v", err)
		c.Check(exhaustOut.Len() == 376, "initial exhaust tables length %d", exhaustOut.Len())
		exPMT2 := sectionPacket(0x1000, 1, prTestPMTSection(1, 1, true, 0x100, nil, []prTestES{
			{streamType: 0x1b, pid: 0x100},
			{streamType: 0x0f, pid: 0x101},
			{streamType: 0x06, pid: 0x102},
			{streamType: 0x06, pid: 0x103},
		}))
		before := exhaustOut.Len()
		_, err = re.Write(exPMT2)
		c.Check(err != nil, "PID exhaustion did not fail")
		c.Check(errors.Is(err, errProgramRemuxPIDExhausted), "unexpected exhaustion error: %v", err)
		c.Check(exhaustOut.Len() == before, "half of a new table was published")
	})
}

func prRewrittenPacket(pkt []byte, pid uint16, cc byte) []byte {
	out := append([]byte(nil), pkt...)
	out[1] = (out[1] & 0xe0) | byte(pid>>8)
	out[2] = byte(pid)
	out[3] = (out[3] & 0xf0) | cc
	return out
}

func TestProgramMedia(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var out bytes.Buffer
		r, err := NewProgramRemuxer(&out, ProgramRemuxConfig{ProgramNumber: 7, OutputPMTPID: 0x0042, FirstMediaPID: 0x0040})
		c.Check(err == nil, "NewProgramRemuxer: %v", err)
		checkpoint := 0
		write := func(data []byte) []prParsedPacket {
			return prWriteAndParseNew(c, r, &out, &checkpoint, data)
		}
		sectionPacket := func(pid uint16, cc byte, sec []byte) []byte {
			return prTestPacket(pid, true, cc, append([]byte{0}, sec...))
		}

		pat := sectionPacket(0, 0, prTestPATSection(0x9999, 0, true, 0, 0, []prTestProgram{{number: 7, pmtPID: 0x1000}}))
		pmt := sectionPacket(0x1000, 0, prTestPMTSection(7, 0, true, 0x110, nil, []prTestES{
			{streamType: 0x1b, pid: 0x100, descriptors: []byte{0x0a, 0x03, 'e', 'n', 'g'}},
			{streamType: 0x0f, pid: 0x101},
		}))
		tables := write(append(pat, pmt...))
		c.Check(len(tables) == 2, "tables not published: %d", len(tables))
		pmtSec := prSectionFromPackets(c, tables, 0x42)
		pcr, es := prParseOutputPMT(c, pmtSec)
		c.Check(len(es) == 2 && es[0].pid == 0x40 && es[1].pid == 0x41, "ES allocation wrong: %+v", es)
		c.Check(pcr == 0x43, "independent PCR should skip PMT PID 0x42 and get 0x43, got 0x%04x", pcr)

		// A continuation before the first PUSI is dropped.
		early := prTestPacket(0x100, false, 9, bytes.Repeat([]byte{0x60}, 184))
		c.Check(len(write(early)) == 0, "early ES continuation was output")

		videoPayload := make([]byte, 182)
		for i := range videoPayload {
			videoPayload[i] = byte(i * 7)
		}
		videoStart := prTestAdaptationPayloadPacket(0x100, true, 4, []byte{0x00}, videoPayload)
		got := write(videoStart)
		c.Check(len(got) == 1, "video start not output")
		if len(got) == 1 {
			c.Check(got[0].pid == 0x40 && got[0].cc == 0, "first video packet PID/CC wrong")
			c.Check(bytes.Equal(got[0].raw, prRewrittenPacket(videoStart, 0x40, 0)), "video start bytes other than PID/CC changed")
		}

		var pcrBytes [6]byte
		for i := range pcrBytes {
			pcrBytes[i] = byte(0x70 + i)
		}
		pcrOnly := prTestAdaptationOnlyPCRPacket(0x110, 11, pcrBytes)
		got = write(pcrOnly)
		c.Check(len(got) == 1, "independent PCR adaptation packet not output")
		if len(got) == 1 {
			c.Check(got[0].pid == 0x43 && got[0].cc == 0, "PCR-only first CC wrong: %d", got[0].cc)
			c.Check(bytes.Equal(got[0].raw, prRewrittenPacket(pcrOnly, 0x43, 0)), "PCR packet bytes/clock changed")
		}
		pcrOnly2 := prTestAdaptationOnlyPCRPacket(0x110, 12, pcrBytes)
		got = write(pcrOnly2)
		c.Check(len(got) == 1 && got[0].cc == 0, "second adaptation-only CC should reuse last payload/none value 0")
		if len(got) == 1 {
			c.Check(bytes.Equal(got[0].raw, prRewrittenPacket(pcrOnly2, 0x43, 0)), "second PCR packet changed")
		}

		// Adaptation-only after an ES payload reuses that payload CC.
		videoAF := prTestAdaptationOnlyPCRPacket(0x100, 13, pcrBytes)
		got = write(videoAF)
		c.Check(len(got) == 1 && got[0].pid == 0x40 && got[0].cc == 0, "ES adaptation-only CC wrong")
		if len(got) == 1 {
			c.Check(bytes.Equal(got[0].raw, prRewrittenPacket(videoAF, 0x40, 0)), "ES adaptation field changed")
		}
		videoCont := prTestPacket(0x100, false, 14, bytes.Repeat([]byte{0x77}, 184))
		got = write(videoCont)
		c.Check(len(got) == 1 && got[0].cc == 1, "payload after adaptation-only CC wrong")
		if len(got) == 1 {
			c.Check(bytes.Equal(got[0].raw, prRewrittenPacket(videoCont, 0x40, 1)), "video continuation bytes changed")
		}

		// Move the clock PID into an ES. The PCR-only identity must retire and
		// the new ES identity must wait for a PUSI payload.
		pmt2 := sectionPacket(0x1000, 1, prTestPMTSection(7, 1, true, 0x110, nil, []prTestES{
			{streamType: 0x1b, pid: 0x100},
			{streamType: 0x0f, pid: 0x101},
			{streamType: 0x06, pid: 0x110},
		}))
		tables = write(pmt2)
		c.Check(len(tables) == 2, "PCR-to-ES PMT update did not publish tables")
		pmtSec = prSectionFromPackets(c, tables, 0x42)
		pcr, es = prParseOutputPMT(c, pmtSec)
		c.Check(pcr == 0x44, "PCR did not move to new ES output PID: %04x", pcr)
		c.Check(len(es) == 3 && es[2].pid == 0x44 && es[2].streamType == 0x06, "new ES identity mapping wrong: %+v", es)

		oldClockPacket := prTestAdaptationOnlyPCRPacket(0x110, 15, pcrBytes)
		earlyES := prTestPacket(0x110, false, 0, bytes.Repeat([]byte{0x88}, 184))
		c.Check(len(write(append(oldClockPacket, earlyES...))) == 0, "new ES identity started without PUSI")

		newESPayload := bytes.Repeat([]byte{0x99}, 184)
		newESStart := prTestPacket(0x110, true, 1, newESPayload)
		got = write(newESStart)
		c.Check(len(got) == 1 && got[0].pid == 0x44 && got[0].cc == 0, "new ES PUSI start wrong")
		if len(got) == 1 {
			c.Check(bytes.Equal(got[0].raw, prRewrittenPacket(newESStart, 0x44, 0)), "new ES payload changed")
		}

		// Surviving video CC/identity continues across the PMT replacement.
		videoAfterTable := prTestPacket(0x100, false, 16, bytes.Repeat([]byte{0xaa}, 184))
		got = write(videoAfterTable)
		c.Check(len(got) == 1 && got[0].pid == 0x40 && got[0].cc == 2, "surviving video CC did not continue")
		if len(got) == 1 {
			c.Check(bytes.Equal(got[0].raw, prRewrittenPacket(videoAfterTable, 0x40, 2)), "surviving video body changed")
		}

		// Null and an unlisted program PID are not output.
		nullPacket := prTestPacket(0x1fff, false, 0, bytes.Repeat([]byte{0xff}, 184))
		otherPacket := prTestPacket(0x0201, true, 0, bytes.Repeat([]byte{0xbb}, 184))
		c.Check(len(write(append(nullPacket, otherPacket...))) == 0, "null/other PID output")
		c.Check(r.Flush() == nil, "Flush in middle of PES failed")

		// A short underlying writer is still fully completed.
		var shortBuf bytes.Buffer
		sw := &prShortWriter{buf: &shortBuf, maxWrite: 7}
		rs, err := NewProgramRemuxer(sw, ProgramRemuxConfig{ProgramNumber: 7, OutputPMTPID: 0x0050, FirstMediaPID: 0x0060})
		c.Check(err == nil, "short-writer constructor")
		_, err = rs.Write(append(pat, pmt...))
		c.Check(err == nil, "short writer Write failed: %v", err)
		c.Check(shortBuf.Len() == 376, "short writer output length %d", shortBuf.Len())
		c.Check(rs.Flush() == nil, "short writer Flush")

		// Writer failure is sticky.
		writerErr := errors.New("writer exploded")
		rf, err := NewProgramRemuxer(&prFailWriter{err: writerErr}, ProgramRemuxConfig{ProgramNumber: 7, OutputPMTPID: 0x0050, FirstMediaPID: 0x0060})
		c.Check(err == nil, "fail-writer constructor")
		_, err = rf.Write(pat)
		c.Check(err == nil, "PAT-only processing unexpectedly wrote/failed: %v", err)
		_, err = rf.Write(pmt)
		c.Check(errors.Is(err, writerErr), "expected writer error, got %v", err)
		_, err = rf.Write(pat)
		c.Check(errors.Is(err, writerErr), "writer error was not sticky on Write")
		c.Check(errors.Is(rf.Flush(), writerErr), "writer error was not sticky on Flush")
	})
}
