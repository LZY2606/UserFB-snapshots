/*
MIT License

Copyright 2016 Comcast Cable Communications Management, LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

package retime_test

import (
	"encoding/binary"
	"math"
	"math/big"
	"strings"
	"testing"

	"github.com/Comcast/gots/v3"
	"github.com/Comcast/gots/v3/packet"
	"github.com/Comcast/gots/v3/retime"
)

const (
	testVideoPID = 0x100
	testAudioPID = 0x101
	testCuePID   = 0x102
	testOtherPID = 0x103
)

// ---------------------------------------------------------------------------
// Independent bit-level readers, written separately from the gots primitives
// used by the retime implementation.
// ---------------------------------------------------------------------------

type bitReader struct {
	data []byte
	pos  int
}

func (r *bitReader) read(n int) uint64 {
	var v uint64
	for i := 0; i < n; i++ {
		byteIdx := r.pos / 8
		bitIdx := 7 - r.pos%8
		v = v<<1 | uint64((r.data[byteIdx]>>bitIdx)&1)
		r.pos++
	}
	return v
}

// readPTS33 decodes a 5 byte MPEG PTS/DTS field: 4 bit prefix, 3+15+15 time
// bits separated by single marker bits.
func readPTS33(b []byte) (prefix byte, value uint64, markers [3]bool) {
	r := bitReader{data: b}
	prefix = byte(r.read(4))
	hi := r.read(3)
	markers[0] = r.read(1) == 1
	mid := r.read(15)
	markers[1] = r.read(1) == 1
	lo := r.read(15)
	markers[2] = r.read(1) == 1
	value = hi<<30 | mid<<15 | lo
	return
}

// readPCR decodes a 6 byte PCR field: 33 bit base, 6 reserved bits, 9 bit
// extension.
func readPCR(b []byte) (base, ext uint64, reserved byte) {
	r := bitReader{data: b}
	base = r.read(33)
	reserved = byte(r.read(6))
	ext = r.read(9)
	return
}

// mpeg2CRC32 is an independent, textbook table-free MPEG-2 CRC implementation
// (init 0xFFFFFFFF, poly 0x04C11DB7, no reflection, no final XOR).
func mpeg2CRC32(data []byte) uint32 {
	const poly uint32 = 0x04C11DB7
	var crc uint32 = 0xFFFFFFFF
	for _, b := range data {
		crc ^= uint32(b) << 24
		for i := 0; i < 8; i++ {
			if crc&0x80000000 != 0 {
				crc = crc<<1 ^ poly
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

// ---------------------------------------------------------------------------
// TS packet and PES / SCTE-35 builders.
// ---------------------------------------------------------------------------

type testAF struct {
	present bool
	length  int
	pcr     *uint64
	opcr    *uint64
}

func mkPacket(pid int, cc int, pusi bool, scramble uint8, af testAF, payload []byte) *packet.Packet {
	var p packet.Packet
	p[0] = packet.SyncByte
	if pusi {
		p[1] |= 0x40
	}
	p[1] |= byte(pid>>8) & 0x1f
	p[2] = byte(pid)
	ctrl := byte(0)
	if af.present {
		ctrl |= 0x20
	}
	if payload != nil {
		ctrl |= 0x10
	}
	p[3] = scramble<<6 | ctrl | byte(cc&0x0f)

	off := 4
	if af.present {
		p[off] = byte(af.length)
		off++
		var flags byte
		if af.pcr != nil {
			flags |= 0x10
		}
		if af.opcr != nil {
			flags |= 0x08
		}
		p[off] = flags
		off++
		if af.pcr != nil {
			gots.InsertPCR(p[off:off+6], *af.pcr)
			off += 6
		}
		if af.opcr != nil {
			gots.InsertPCR(p[off:off+6], *af.opcr)
			off += 6
		}
		// Remaining adaptation field bytes are stuffing; the payload starts
		// at 5 + adaptation_field_length.
		payloadStart := 5 + af.length
		for ; off < payloadStart; off++ {
			p[off] = 0xFF
		}
		off = payloadStart
	}
	copy(p[off:], payload)
	return &p
}

// encodeTimeField builds a 5 byte PTS/DTS field with the given 4 bit prefix.
func encodeTimeField(prefix byte, v uint64) []byte {
	b := make([]byte, 5)
	b[0] = prefix | byte(v>>29&0x0F) // prefix '001x', 3 high time bits, marker below
	b[1] = byte(v >> 22)
	b[2] = byte(v >> 14) // 7 time bits shifted to the top, marker below
	b[3] = byte(v >> 7)
	b[4] = byte(v << 1)
	b[0] |= 0x01
	b[2] |= 0x01
	b[4] |= 0x01
	return b
}

// buildPES assembles a PES header plus body. pesPacketLength zero mirrors the
// video case where the length is unspecified.
func buildPES(streamID byte, ptsDTSIndicator byte, pts, dts uint64, body []byte) []byte {
	var hdrLen int
	switch ptsDTSIndicator {
	case 2:
		hdrLen = 5
	case 3:
		hdrLen = 10
	}
	out := make([]byte, 0, 9+hdrLen+len(body))
	out = append(out, 0x00, 0x00, 0x01, streamID, 0x00, 0x00)
	out = append(out, 0x80, ptsDTSIndicator<<6, byte(hdrLen))
	switch ptsDTSIndicator {
	case 2:
		out = append(out, encodeTimeField(0x20, pts)...)
	case 3:
		out = append(out, encodeTimeField(0x30, pts)...)
		out = append(out, encodeTimeField(0x10, dts)...)
	}
	return append(out, body...)
}

// chunk splits data into payload sized chunks.
func chunk(data []byte, size int) [][]byte {
	var out [][]byte
	for len(data) > 0 {
		n := size
		if n > len(data) {
			n = len(data)
		}
		out = append(out, data[:n])
		data = data[n:]
	}
	return out
}

// buildSCTE35Section assembles one SCTE-35 section (no pointer field) with a
// raw splice command and descriptor bytes, applying the MPEG-2 CRC.
func buildSCTE35Section(ptsAdjustment uint64, cmdType byte, cmd, descriptors []byte, stuffing int) []byte {
	sectionLen := 11 + len(cmd) + 2 + len(descriptors) + stuffing + 4
	sec := make([]byte, 0, 3+sectionLen)
	sec = append(sec, 0xFC)
	sec = append(sec, 0x30|byte(sectionLen>>8), byte(sectionLen))
	sec = append(sec, 0x00) // protocol_version
	sec = append(sec,
		byte((ptsAdjustment>>32)&0x01),
		byte(ptsAdjustment>>24),
		byte(ptsAdjustment>>16),
		byte(ptsAdjustment>>8),
		byte(ptsAdjustment))
	sec = append(sec, 0x00) // cw_index
	sec = append(sec, 0xFF, 0xF0|byte(len(cmd)>>8), byte(len(cmd)))
	sec = append(sec, cmdType)
	sec = append(sec, cmd...)
	sec = append(sec, byte(len(descriptors)>>8), byte(len(descriptors)))
	sec = append(sec, descriptors...)
	for i := 0; i < stuffing; i++ {
		sec = append(sec, 0xFF)
	}
	return append(sec, gots.ComputeCRC(sec)...)
}

// timeSignalCmd builds the splice_time body of a time_signal command:
// time_specified_flag plus a 33 bit pts_time.
func timeSignalCmd(ptsTime uint64) []byte {
	b := make([]byte, 5)
	b[0] = 0xFE | byte((ptsTime>>32)&0x01)
	binary.BigEndian.PutUint32(b[1:], uint32(ptsTime))
	return b
}

// rawSpliceInsertCmd returns an opaque splice_insert shaped command whose time
// fields must survive retiming untouched.
func rawSpliceInsertCmd() []byte {
	return []byte{
		0x12, 0x34, 0x56, 0x78, // splice_event_id
		0x00,                         // not canceled
		0xE8,                         // out-of-network, program splice, duration flags
		0xFE, 0x01, 0x23, 0x45, 0x67, // splice_time pts 0x1234567
		0xFE, 0x00, 0x98, 0x96, 0x80, // break_duration 0x989680 with auto_return
		0x00, 0x01, 0x02, 0x03, // unique_program_id, avail_num, avails_expected
	}
}

func clonePackets(in []*packet.Packet) []*packet.Packet {
	out := make([]*packet.Packet, len(in))
	for i, p := range in {
		cp := packet.Packet{}
		copy(cp[:], p[:])
		out[i] = &cp
	}
	return out
}

func payloadOf(t *testing.T, p *packet.Packet) []byte {
	t.Helper()
	pay, err := packet.Payload(p)
	if err != nil {
		t.Fatalf("packet has no payload: %v", err)
	}
	return pay
}

// assertBytesEqualExcept fails if any byte position outside allowed changes
// differs between the two packet streams.
func assertBytesEqualExcept(t *testing.T, before, after []*packet.Packet, allowed map[int]map[int]bool) {
	t.Helper()
	if len(before) != len(after) {
		t.Fatalf("packet count changed: %d -> %d", len(before), len(after))
	}
	for i := range before {
		for pos := 0; pos < packet.PacketSize; pos++ {
			if before[i][pos] != after[i][pos] && !allowed[i][pos] {
				t.Fatalf("packet %d byte %d changed unexpectedly: 0x%02X -> 0x%02X",
					i, pos, before[i][pos], after[i][pos])
			}
		}
	}
}

func expectErrContains(t *testing.T, err error, parts ...string) {
	t.Helper()
	if err == nil {
		t.Fatalf("expected an error containing %v, got nil", parts)
	}
	for _, part := range parts {
		if !strings.Contains(err.Error(), part) {
			t.Fatalf("error %q does not contain %q", err.Error(), part)
		}
	}
}

// TestShiftEmpty covers the empty fragment edge case.
func TestShiftEmpty(t *testing.T) {
	out, err := retime.Shift(nil, retime.Config{Delta90k: 90000})
	if err != nil {
		t.Fatal(err)
	}
	if out == nil || len(out) != 0 {
		t.Fatalf("expected empty non-nil slice, got %#v", out)
	}
}

// TestShiftInterleavedClocks is the end-to-end showcase: two media PIDs and a
// cue PID interleave with adaptation-only packets and an unselected PID. A
// video PES header is squeezed by a large adaptation field across two TS
// packets, with other PIDs in between. Every clock is read back with
// independent bit readers before and after the shift.
func TestShiftInterleavedClocks(t *testing.T) {
	const delta int64 = 123456789

	videoPTS0 := uint64(5_000_000)
	videoDTS0 := uint64(4_999_000)
	audioPTS0 := uint64(5_000_500)
	audioPTS1 := audioPTS0 + 90000
	pcr0 := uint64(27_000_017) // base 90000, extension 17
	opcr0 := uint64(9_000_301)
	cueAdj0 := uint64(77_000)
	cueCmdTime0 := uint64(123_456_789)

	videoBody := []byte{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x11, 0x22, 0x33}
	videoPES := buildPES(0xE0, 3, videoPTS0, videoDTS0, videoBody)
	if len(videoPES) < 14 {
		t.Fatalf("test setup: video PES too short")
	}

	// First video packet only carries the first 13 PES bytes: the 170 byte
	// adaptation field squeezes the header across the packet boundary.
	v0 := mkPacket(testVideoPID, 0, true, 0,
		testAF{present: true, length: 170, pcr: &pcr0},
		videoPES[:13])
	// Adaptation-only packet: same continuity counter, it must not advance the
	// payload CC. It carries both PCR and OPCR.
	v1af := mkPacket(testVideoPID, 0, false, 0,
		testAF{present: true, length: 183, pcr: &pcr0, opcr: &opcr0}, nil)
	v1 := mkPacket(testVideoPID, 1, false, 0, testAF{}, videoPES[13:])

	audioPES0 := buildPES(0xC0, 2, audioPTS0, 0, []byte{0x41, 0x42, 0x43})
	a0 := mkPacket(testAudioPID, 0, true, 0, testAF{}, audioPES0)
	audioPES1 := buildPES(0xC0, 2, audioPTS1, 0, []byte{0x44, 0x45})
	a1 := mkPacket(testAudioPID, 1, true, 0, testAF{}, audioPES1)

	// Two sections in one cue payload, including an unknown descriptor and
	// trailing 0xFF stuffing.
	unknownDesc := []byte{0x77, 0x04, 0xDE, 0xAD, 0xBE, 0xEF}
	sec1 := buildSCTE35Section(cueAdj0, 0x06, timeSignalCmd(cueCmdTime0), unknownDesc, 1)
	sec2 := buildSCTE35Section(cueAdj0+50, 0x00, nil, nil, 0)
	cuePayload := append([]byte{0x00}, sec1...)
	cuePayload = append(cuePayload, sec2...)
	for len(cuePayload) < 184 {
		cuePayload = append(cuePayload, 0xFF)
	}
	c0 := mkPacket(testCuePID, 0, true, 0, testAF{}, cuePayload)

	other := mkPacket(testOtherPID, 7, true, 0, testAF{},
		append([]byte{0xDE, 0xAD, 0xBE, 0xEF}, repeatByte(0x5A, 180)...))

	in := []*packet.Packet{v0, a0, v1af, v1, c0, a1, other}
	before := clonePackets(in)

	out, err := retime.Shift(in, retime.Config{
		PESPIDs:  []int{testVideoPID, testAudioPID},
		PCRPIDs:  []int{testVideoPID},
		CuePIDs:  []int{testCuePID},
		Delta90k: delta,
	})
	if err != nil {
		t.Fatalf("Shift returned error: %v", err)
	}
	if len(out) != len(in) {
		t.Fatalf("packet count changed: %d -> %d", len(in), len(out))
	}

	// --- Independent ownership: inputs must be untouched. ---
	for i := range in {
		if *in[i] != *before[i] {
			t.Fatalf("input packet %d was mutated", i)
		}
		if &in[i][0] == &out[i][0] {
			t.Fatalf("output packet %d shares memory with input", i)
		}
	}

	// --- Video PES: reassemble the split header and read both clocks. ---
	v0Pay := payloadOf(t, out[0])
	v1Pay := payloadOf(t, out[3])
	rebuilt := append(append([]byte{}, v0Pay...), v1Pay...)
	if len(rebuilt) < len(videoPES) {
		t.Fatalf("rebuilt PES truncated: %d bytes", len(rebuilt))
	}
	ptsPrefix, ptsVal, ptsMarkers := readPTS33(rebuilt[9:14])
	dtsPrefix, dtsVal, dtsMarkers := readPTS33(rebuilt[14:19])
	if ptsPrefix != 0x3 || !allMarkers(ptsMarkers) {
		t.Fatalf("PTS prefix/markers not preserved: prefix=%d markers=%v", ptsPrefix, ptsMarkers)
	}
	if dtsPrefix != 0x1 || !allMarkers(dtsMarkers) {
		t.Fatalf("DTS prefix/markers not preserved: prefix=%d markers=%v", dtsPrefix, dtsMarkers)
	}
	expectPTS33(t, "video PTS", ptsVal, videoPTS0, delta)
	expectPTS33(t, "video DTS", dtsVal, videoDTS0, delta)
	if (ptsVal - dtsVal) != (videoPTS0 - videoDTS0) {
		t.Fatalf("PTS-DTS spacing changed: %d -> %d", videoPTS0-videoDTS0, ptsVal-dtsVal)
	}
	if string(rebuilt[19:19+len(videoBody)]) != string(videoBody) {
		t.Fatal("video PES body bytes changed")
	}

	// --- Audio PES headers (two PUSI packets: a0 is index 1, a1 is index 5). ---
	for idx, original := range []uint64{audioPTS0, audioPTS1} {
		pktIdx := 1 + idx*4
		pay := payloadOf(t, out[pktIdx])
		prefix, val, markers := readPTS33(pay[9:14])
		if prefix != 0x2 || !allMarkers(markers) {
			t.Fatalf("audio PTS %d prefix/markers lost", idx)
		}
		expectPTS33(t, "audio PTS", val, original, delta)
	}

	// --- PCR and OPCR: base moves by delta, extension phase is preserved. ---
	pcrBase, pcrExt, pcrReserved := readPCR(out[0][6:12])
	_, pcrExtBefore, _ := readPCR(in[0][6:12])
	expectPCRBase(t, "PCR", pcrBase, pcr0/300, delta)
	if pcrExt != pcrExtBefore {
		t.Fatalf("PCR extension phase changed: %d -> %d", pcrExtBefore, pcrExt)
	}
	if pcrReserved != 0x3F {
		t.Fatalf("PCR reserved bits altered: 0x%02X", pcrReserved)
	}
	opcrBase, opcrExt, _ := readPCR(out[2][12:18])
	_, opcrExtBefore, _ := readPCR(in[2][12:18])
	expectPCRBase(t, "OPCR", opcrBase, opcr0/300, delta)
	if opcrExt != opcrExtBefore {
		t.Fatalf("OPCR extension phase changed: %d -> %d", opcrExtBefore, opcrExt)
	}

	// --- Cue: pts_adjustment moves, command time and unknown bytes do not. ---
	cuePay := payloadOf(t, out[4])
	off := 1 // skip pointer_field
	for secIdx, originalAdj := range []uint64{cueAdj0, cueAdj0 + 50} {
		secLen := 3 + int(uint16(cuePay[off+1]&0x03)<<8|uint16(cuePay[off+2]))
		sec := cuePay[off : off+secLen]
		adj := uint40Independent(sec[4:9]) & 0x1FFFFFFFF
		expectPTS33(t, "cue pts_adjustment", adj, originalAdj, delta)
		storedCRC := binary.BigEndian.Uint32(sec[secLen-4:])
		if got := mpeg2CRC32(sec[:secLen-4]); got != storedCRC {
			t.Fatalf("section %d CRC 0x%08X does not verify (computed 0x%08X)", secIdx, storedCRC, got)
		}
		off += secLen
	}
	// The time_signal command pts_time and unknown descriptor must be exactly
	// preserved: only adjustment and CRC changed.
	sec1After := cuePay[1 : 1+len(sec1)]
	cmdTime := uint40Independent(sec1After[14:19]) & 0x1FFFFFFFF
	if cmdTime != cueCmdTime0 {
		t.Fatalf("command splice time changed: %d -> %d", cueCmdTime0, cmdTime)
	}
	// The 5 byte command ends at section offset 19; the descriptor loop length
	// follows at 19..20 and the raw descriptor bytes begin at offset 21.
	if string(sec1After[21:21+len(unknownDesc)]) != string(unknownDesc) {
		t.Fatal("unknown descriptor bytes were re-encoded")
	}

	// --- Strict byte fidelity: enumerate every byte allowed to change. ---
	allowed := map[int]map[int]bool{}
	allow := func(pkt, pos int) {
		if allowed[pkt] == nil {
			allowed[pkt] = map[int]bool{}
		}
		allowed[pkt][pos] = true
	}
	for pos := 6; pos < 12; pos++ { // v0 PCR
		allow(0, pos)
	}
	for pos := 6; pos < 18; pos++ { // v1af PCR + OPCR
		allow(2, pos)
	}
	// Split video PTS: v0 payload begins at packet offset 175 and carries PES
	// logical bytes 9..12; v1 payload begins at 4 and carries byte 13 and DTS.
	for pos := 184; pos <= 187; pos++ { // PTS head (logical 9..12)
		allow(0, pos)
	}
	allow(3, 4)                     // PTS final byte
	for pos := 5; pos <= 9; pos++ { // DTS field in v1 payload (logical 14..18)
		allow(3, pos)
	}
	for _, idx := range []int{1, 5} { // audio PTS at payload 9..13 -> packet 13..17
		for pos := 13; pos <= 17; pos++ {
			allow(idx, pos)
		}
	}
	// Cue sections: payload starts at 4, pointer_field at 4, section 1 at 5.
	for _, sec := range []struct{ start, length int }{
		{5, len(sec1)},
		{5 + len(sec1), len(sec2)},
	} {
		for pos := sec.start + 4; pos < sec.start+9; pos++ {
			allow(4, pos)
		}
		for pos := sec.start + sec.length - 4; pos < sec.start+sec.length; pos++ {
			allow(4, pos)
		}
	}
	assertBytesEqualExcept(t, before, out, allowed)
}

func repeatByte(b byte, n int) []byte {
	out := make([]byte, n)
	for i := range out {
		out[i] = b
	}
	return out
}

func allMarkers(m [3]bool) bool { return m[0] && m[1] && m[2] }

func uint40Independent(b []byte) uint64 {
	return uint64(b[0])<<32 | uint64(b[1])<<24 | uint64(b[2])<<16 | uint64(b[3])<<8 | uint64(b[4])
}

func expectPTS33(t *testing.T, name string, got, original uint64, delta int64) {
	t.Helper()
	want := modBig(original, delta, 1<<33)
	if got != want {
		t.Fatalf("%s = %d, want %d (original %d, delta %d)", name, got, want, original, delta)
	}
}

func expectPCRBase(t *testing.T, name string, gotBase, originalBase uint64, delta int64) {
	t.Helper()
	want := modBig(originalBase, delta, 1<<33)
	if gotBase != want {
		t.Fatalf("%s base = %d, want %d (original base %d, delta %d)", name, gotBase, want, originalBase, delta)
	}
}

// modBig computes (unsigned value + signed delta) mod modulus using big.Int so
// expectations never depend on the implementation's integer handling.
func modBig(value uint64, delta int64, modulus int64) uint64 {
	v := new(big.Int).SetUint64(value)
	v.Add(v, big.NewInt(delta))
	m := big.NewInt(modulus)
	v.Mod(v, m)
	return v.Uint64()
}

// TestShiftPTSWraparound checks 33 bit modular arithmetic in both directions,
// including PTS and DTS and a negative delta that crosses zero.
func TestShiftPTSWraparound(t *testing.T) {
	const modulus uint64 = 1 << 33
	cases := []struct {
		name     string
		pts, dts uint64
		delta    int64
	}{
		{"positive rollover", modulus - 10, modulus - 20, 25},
		{"negative rollover", 5, 8, -25},
		{"full period is identity", 1234567, 7654321, int64(modulus)},
		{"negative full period", 1234567, 7654321, -int64(modulus)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			pesBytes := buildPES(0xE0, 3, tc.pts, tc.dts, []byte{1, 2, 3})
			pkt := mkPacket(testVideoPID, 0, true, 0, testAF{}, pesBytes)
			out, err := retime.Shift([]*packet.Packet{pkt}, retime.Config{
				PESPIDs: []int{testVideoPID}, Delta90k: tc.delta,
			})
			if err != nil {
				t.Fatal(err)
			}
			pay := payloadOf(t, out[0])
			_, gotPTS, ptsMarkers := readPTS33(pay[9:14])
			_, gotDTS, dtsMarkers := readPTS33(pay[14:19])
			if !allMarkers(ptsMarkers) || !allMarkers(dtsMarkers) {
				t.Fatal("timestamp marker bits lost across wraparound")
			}
			if gotPTS != modBig(tc.pts, tc.delta, int64(modulus)) {
				t.Fatalf("PTS wraparound wrong: got %d", gotPTS)
			}
			if gotDTS != modBig(tc.dts, tc.delta, int64(modulus)) {
				t.Fatalf("DTS wraparound wrong: got %d", gotDTS)
			}
			if int64(gotPTS)-int64(gotDTS) != int64(tc.pts)-int64(tc.dts) {
				t.Fatal("PTS/DTS spacing changed across wraparound")
			}
		})
	}
}

// TestShiftPCRWraparound exercises the 2^33*300 PCR domain, extension phase
// preservation and deltas at the int64 limits.
func TestShiftPCRWraparound(t *testing.T) {
	const pcrModulus int64 = (1 << 33) * 300
	// Base near the top of the 33 bit range, extension 123.
	pcr := uint64(((1<<33)-50)*300 + 123)
	opcr := uint64(10*300 + 7)
	deltas := []int64{90000, -90000, math.MaxInt64, math.MinInt64, math.MaxInt64 - 7, math.MinInt64 + 7}
	for _, delta := range deltas {
		pkt := mkPacket(testVideoPID, 0, false, 0,
			testAF{present: true, length: 20, pcr: &pcr, opcr: &opcr}, nil)
		out, err := retime.Shift([]*packet.Packet{pkt}, retime.Config{
			PCRPIDs: []int{testVideoPID}, Delta90k: delta,
		})
		if err != nil {
			t.Fatalf("delta %d: %v", delta, err)
		}
		base, ext, _ := readPCR(out[0][6:12])
		obase, oext, _ := readPCR(out[0][12:18])
		if ext != 123 || oext != 7 {
			t.Fatalf("delta %d: extension phase changed pcr=%d opcr=%d", delta, ext, oext)
		}
		wantBase := new(big.Int).Mod(
			new(big.Int).Add(new(big.Int).SetUint64(pcr/300), big.NewInt(delta)),
			big.NewInt(1<<33)).Uint64()
		if base != wantBase {
			t.Fatalf("delta %d: PCR base %d want %d", delta, base, wantBase)
		}
		wantOBase := new(big.Int).Mod(
			new(big.Int).Add(new(big.Int).SetUint64(opcr/300), big.NewInt(delta)),
			big.NewInt(1<<33)).Uint64()
		if obase != wantOBase {
			t.Fatalf("delta %d: OPCR base %d want %d", delta, obase, wantOBase)
		}
		// Full reconstructed PCR must match the 42 bit modular expectation.
		full := base*300 + ext
		wantFull := new(big.Int).Mod(
			new(big.Int).Add(new(big.Int).SetUint64(pcr), new(big.Int).Mul(big.NewInt(delta), big.NewInt(300))),
			big.NewInt(pcrModulus)).Uint64()
		if full != wantFull {
			t.Fatalf("delta %d: reconstructed PCR %d want %d", delta, full, wantFull)
		}
	}
}

// TestShiftCueWraparound verifies pts_adjustment rolls in the 33 bit domain
// while the command time never moves.
func TestShiftCueWraparound(t *testing.T) {
	const modulus uint64 = 1 << 33
	for _, delta := range []int64{90000, -90000, math.MaxInt64, math.MinInt64} {
		adj := modulus - 100
		cmdTime := uint64(424242)
		sec := buildSCTE35Section(adj, 0x06, timeSignalCmd(cmdTime), nil, 0)
		payload := append([]byte{0x00}, sec...)
		payload = append(payload, repeatByte(0xFF, 184-len(payload))...)
		pkt := mkPacket(testCuePID, 0, true, 0, testAF{}, payload)
		out, err := retime.Shift([]*packet.Packet{pkt}, retime.Config{
			CuePIDs: []int{testCuePID}, Delta90k: delta,
		})
		if err != nil {
			t.Fatalf("delta %d: %v", delta, err)
		}
		gotPay := payloadOf(t, out[0])
		gotSec := gotPay[1 : 1+len(sec)]
		gotAdj := uint40Independent(gotSec[4:9]) & 0x1FFFFFFFF
		if gotAdj != modBig(adj, delta, int64(modulus)) {
			t.Fatalf("delta %d: adjustment %d want %d", delta, gotAdj, modBig(adj, delta, int64(modulus)))
		}
		if got := uint40Independent(gotSec[14:19]) & 0x1FFFFFFFF; got != cmdTime {
			t.Fatalf("delta %d: command time moved %d -> %d", delta, cmdTime, got)
		}
		if mpeg2CRC32(gotSec[:len(gotSec)-4]) != binary.BigEndian.Uint32(gotSec[len(gotSec)-4:]) {
			t.Fatalf("delta %d: CRC does not verify", delta)
		}
	}
}

// TestShiftZeroDelta proves a zero shift reproduces the input byte for byte.
func TestShiftZeroDelta(t *testing.T) {
	pcr := uint64(90000*300 + 17)
	pesBytes := buildPES(0xE0, 3, 111, 99, []byte{9, 8, 7})
	v := mkPacket(testVideoPID, 0, true, 0, testAF{present: true, length: 12, pcr: &pcr}, pesBytes)
	sec := buildSCTE35Section(555, 0x00, nil, nil, 0)
	cuePayload := append([]byte{0x00}, sec...)
	cuePayload = append(cuePayload, repeatByte(0xFF, 184-len(cuePayload))...)
	c := mkPacket(testCuePID, 0, true, 0, testAF{}, cuePayload)
	in := []*packet.Packet{v, c}
	out, err := retime.Shift(in, retime.Config{
		PESPIDs: []int{testVideoPID}, PCRPIDs: []int{testVideoPID}, CuePIDs: []int{testCuePID},
	})
	if err != nil {
		t.Fatal(err)
	}
	for i := range in {
		if *in[i] != *out[i] {
			t.Fatalf("zero delta changed packet %d", i)
		}
	}
}

// TestShiftConfigValidation covers duplicate, out of range and conflicting PIDs.
func TestShiftConfigValidation(t *testing.T) {
	pkt := mkPacket(testVideoPID, 0, true, 0, testAF{}, buildPES(0xE0, 2, 1, 0, nil))
	cases := []struct {
		name string
		cfg  retime.Config
		want string
	}{
		{"duplicate PES PID", retime.Config{PESPIDs: []int{100, 100}}, "more than once"},
		{"duplicate PCR PID", retime.Config{PCRPIDs: []int{100, 100}}, "more than once"},
		{"duplicate cue PID", retime.Config{CuePIDs: []int{100, 100}}, "more than once"},
		{"negative PID", retime.Config{PESPIDs: []int{-1}}, "valid range"},
		{"PID too large", retime.Config{CuePIDs: []int{0x2000}}, "valid range"},
		{"PES cue conflict", retime.Config{PESPIDs: []int{100}, CuePIDs: []int{100}}, "both a PES PID and a cue PID"},
		{"cue PES conflict reversed", retime.Config{CuePIDs: []int{100}, PESPIDs: []int{100}}, "both a PES PID and a cue PID"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := retime.Shift([]*packet.Packet{pkt}, tc.cfg)
			expectErrContains(t, err, tc.want)
		})
	}
	// The same PID may legally serve as PCR PID and media or cue PID.
	if _, err := retime.Shift([]*packet.Packet{pkt}, retime.Config{
		PESPIDs: []int{testVideoPID}, PCRPIDs: []int{testVideoPID},
	}); err != nil {
		t.Fatalf("PES/PCR overlap rejected: %v", err)
	}
}

// TestShiftContinuityCounters checks CC break detection and that
// adaptation-only packets do not advance the payload counter.
func TestShiftContinuityCounters(t *testing.T) {
	pay := buildPES(0xE0, 2, 1, 0, []byte{1})
	ok0 := mkPacket(testVideoPID, 0, true, 0, testAF{}, pay)
	afOnly := mkPacket(testVideoPID, 0, false, 0, testAF{present: true, length: 183}, nil)
	ok1 := mkPacket(testVideoPID, 1, false, 0, testAF{}, []byte{2})
	if _, err := retime.Shift([]*packet.Packet{ok0, afOnly, ok1}, retime.Config{
		PESPIDs: []int{testVideoPID}, Delta90k: 90,
	}); err != nil {
		t.Fatalf("adaptation-only packet should not break payload CC: %v", err)
	}

	bad := mkPacket(testVideoPID, 3, false, 0, testAF{}, []byte{3})
	_, err := retime.Shift([]*packet.Packet{ok0, bad}, retime.Config{
		PESPIDs: []int{testVideoPID}, Delta90k: 90,
	})
	expectErrContains(t, err, "continuity counter break", "PID 256", "packet index 1")

	// Unselected PIDs are not checked.
	otherBad := mkPacket(testOtherPID, 9, false, 0, testAF{}, []byte{3})
	if _, err := retime.Shift([]*packet.Packet{otherBad}, retime.Config{
		PESPIDs: []int{testVideoPID},
	}); err != nil {
		t.Fatalf("unselected PID CC should be ignored: %v", err)
	}
}

// TestShiftPESErrors exercises malformed and truncated PES headers.
func TestShiftPESErrors(t *testing.T) {
	cfg := retime.Config{PESPIDs: []int{testVideoPID}, Delta90k: 90}

	t.Run("bad start code prefix", func(t *testing.T) {
		bad := buildPES(0xE0, 2, 1, 0, nil)
		bad[0] = 0x01
		pkt := mkPacket(testVideoPID, 0, true, 0, testAF{}, bad)
		_, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		expectErrContains(t, err, "start_code_prefix", "PID 256", "packet index 0")
	})

	t.Run("illegal PTS marker", func(t *testing.T) {
		bad := buildPES(0xE0, 2, 1, 0, nil)
		bad[9] &^= 0x01 // clear the first PTS marker bit
		pkt := mkPacket(testVideoPID, 0, true, 0, testAF{}, bad)
		_, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		expectErrContains(t, err, "marker", "PID 256")
	})

	t.Run("reserved PTS DTS flags", func(t *testing.T) {
		bad := buildPES(0xE0, 2, 1, 0, nil)
		bad[7] = 0x40
		pkt := mkPacket(testVideoPID, 0, true, 0, testAF{}, bad)
		_, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		expectErrContains(t, err, "reserved PTS_DTS_flags")
	})

	t.Run("header truncated at fragment end", func(t *testing.T) {
		// header_data_length declares 10 optional bytes but only the fixed
		// header plus three optional bytes fit, because the adaptation field
		// shrinks the payload slot; the fragment ends before the header does.
		head := []byte{0, 0, 1, 0xE0, 0, 0, 0x80, 0x80, 10, 0x31, 0, 1}
		// payload slot is 12 bytes: 4 header + 1 AF length byte + 171 AF bytes.
		pkt := mkPacket(testVideoPID, 0, true, 0, testAF{present: true, length: 171}, head)
		_, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		expectErrContains(t, err, "truncated", "PID 256")
	})

	t.Run("continuation without PUSI", func(t *testing.T) {
		pkt := mkPacket(testVideoPID, 0, false, 0, testAF{}, []byte{0, 0, 1})
		_, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		expectErrContains(t, err, "missing PUSI", "PID 256")
	})

	t.Run("scrambled payload", func(t *testing.T) {
		pkt := mkPacket(testVideoPID, 0, true, 2, testAF{}, buildPES(0xE0, 2, 1, 0, nil))
		_, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		expectErrContains(t, err, "scrambled", "PID 256")
	})

	t.Run("body may end mid media", func(t *testing.T) {
		// A 219 byte PES whose 19 byte header completes inside the first
		// packet; the fragment is allowed to stop in the middle of the body.
		body := repeatByte(0xA5, 200)
		pay := buildPES(0xE0, 3, 50, 40, body)
		p0 := mkPacket(testVideoPID, 0, true, 0, testAF{}, pay[:184])
		p1 := mkPacket(testVideoPID, 1, false, 0, testAF{}, pay[184:])
		out, err := retime.Shift([]*packet.Packet{p0, p1}, cfg)
		if err != nil {
			t.Fatalf("media body cut at fragment end must be allowed: %v", err)
		}
		// Header timestamps moved, body markers stayed put, including the last
		// byte that lands inside the unfinished tail.
		p0Pay := payloadOf(t, out[0])
		_, pts, _ := readPTS33(p0Pay[9:14])
		if pts != modBig(50, 90, 1<<33) {
			t.Fatalf("PTS not shifted in body-split PES: %d", pts)
		}
		if p0Pay[19] != 0xA5 {
			t.Fatal("first body byte was not preserved")
		}
		p1Pay := payloadOf(t, out[1])
		if p1Pay[len(pay)-184-1] != pay[len(pay)-1] {
			t.Fatal("final body byte of the truncated tail was not preserved")
		}
	})

	t.Run("unsupported stream id passes through", func(t *testing.T) {
		// Padding stream (0xBE) has no optional PES header; its bytes stay put.
		pay := []byte{0, 0, 1, 0xBE, 0, 5, 1, 2, 3, 4, 5}
		pkt := mkPacket(testVideoPID, 0, true, 0, testAF{}, pay)
		out, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		if err != nil {
			t.Fatal(err)
		}
		if string(payloadOf(t, out[0])[:len(pay)]) != string(pay) {
			t.Fatal("special stream_id PES was modified")
		}
	})
}

// TestShiftCueErrors covers section validation failures.
func TestShiftCueErrors(t *testing.T) {
	cfg := retime.Config{CuePIDs: []int{testCuePID}, Delta90k: 90}

	t.Run("bad CRC", func(t *testing.T) {
		sec := buildSCTE35Section(100, 0x00, nil, nil, 0)
		sec[10] ^= 0xFF // corrupt a byte inside the section
		payload := append([]byte{0x00}, sec...)
		payload = append(payload, repeatByte(0xFF, 184-len(payload))...)
		_, err := retime.Shift([]*packet.Packet{mkPacket(testCuePID, 0, true, 0, testAF{}, payload)}, cfg)
		expectErrContains(t, err, "CRC mismatch", "PID 258")
	})

	t.Run("encrypted hint", func(t *testing.T) {
		sec := buildSCTE35Section(100, 0x00, nil, nil, 0)
		sec[4] |= 0x80
		binary.BigEndian.PutUint32(sec[len(sec)-4:], mpeg2CRC32(sec[:len(sec)-4]))
		payload := append([]byte{0x00}, sec...)
		payload = append(payload, repeatByte(0xFF, 184-len(payload))...)
		_, err := retime.Shift([]*packet.Packet{mkPacket(testCuePID, 0, true, 0, testAF{}, payload)}, cfg)
		expectErrContains(t, err, "encrypted", "PID 258")
	})

	t.Run("wrong table id", func(t *testing.T) {
		sec := buildSCTE35Section(100, 0x00, nil, nil, 0)
		sec[0] = 0xFB
		binary.BigEndian.PutUint32(sec[len(sec)-4:], mpeg2CRC32(sec[:len(sec)-4]))
		payload := append([]byte{0x00}, sec...)
		payload = append(payload, repeatByte(0xFF, 184-len(payload))...)
		_, err := retime.Shift([]*packet.Packet{mkPacket(testCuePID, 0, true, 0, testAF{}, payload)}, cfg)
		expectErrContains(t, err, "table_id", "PID 258")
	})

	t.Run("section truncated at end", func(t *testing.T) {
		// Declare a 300 byte section but only supply the header packet; the
		// remaining bytes never arrive before the fragment ends.
		header := []byte{
			0x00,             // pointer_field
			0xFC, 0x01, 0x2C, // table id and section_length 300
			0x00, 0x00, 0x00, 0x00, 0x00,
		}
		header = append(header, repeatByte(0xFF, 184-len(header))...)
		pkt := mkPacket(testCuePID, 0, true, 0, testAF{}, header)
		_, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		expectErrContains(t, err, "truncated", "PID 258")
	})

	t.Run("continuation without PUSI", func(t *testing.T) {
		pkt := mkPacket(testCuePID, 0, false, 0, testAF{}, repeatByte(0xAB, 10))
		_, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		expectErrContains(t, err, "missing PUSI", "PID 258")
	})

	t.Run("section overruns pointer boundary", func(t *testing.T) {
		// 185 byte section: 183 bytes land in the first payload, two tail bytes
		// remain. The second PUSI promises only one pointer prefix byte, so the
		// section cannot finish inside the prefix.
		bigSec := buildSCTE35Section(100, 0x00, nil, repeatByte(0x33, 165), 0)
		if len(bigSec) != 185 {
			t.Fatalf("test setup: section is %d bytes, want 185", len(bigSec))
		}
		p0 := mkPacket(testCuePID, 0, true, 0, testAF{},
			append([]byte{0x00}, bigSec[:183]...))
		p1Payload := append([]byte{0x01}, bigSec[183:184]...)
		p1Payload = append(p1Payload, repeatByte(0xFF, 184-len(p1Payload))...)
		p1 := mkPacket(testCuePID, 1, true, 0, testAF{}, p1Payload)
		_, err := retime.Shift([]*packet.Packet{p0, p1}, cfg)
		expectErrContains(t, err, "pointer_field boundary", "PID 258")
	})

	t.Run("scrambled cue", func(t *testing.T) {
		pkt := mkPacket(testCuePID, 0, true, 3, testAF{}, repeatByte(0xFF, 184))
		_, err := retime.Shift([]*packet.Packet{pkt}, cfg)
		expectErrContains(t, err, "scrambled", "PID 258")
	})
}

// TestShiftCuePointerFieldContinuation verifies a section that spans two PUSI
// packets: the pointer_field prefix completes the previous section in place,
// the pointer byte itself is untouched, and a second section follows.
func TestShiftCuePointerFieldContinuation(t *testing.T) {
	const delta int64 = 777000
	first := buildSCTE35Section(8080, 0x00, nil, repeatByte(0x33, 167), 0) // 187 bytes
	second := buildSCTE35Section(9090, 0x06, timeSignalCmd(31337), nil, 2)
	if len(first) <= 183 {
		t.Fatalf("test setup: first section must exceed one payload, got %d", len(first))
	}
	tailLen := len(first) - 183

	// Packet 1: pointer_field 0, first 183 bytes of the section, full payload.
	p0 := mkPacket(testCuePID, 0, true, 0, testAF{},
		append([]byte{0x00}, first[:183]...))
	// Packet 2: PUSI whose pointer prefix carries the tail of section one; the
	// pointer byte stays at its position, section two starts right after.
	p1Payload := make([]byte, 0, 184)
	p1Payload = append(p1Payload, byte(tailLen))
	p1Payload = append(p1Payload, first[183:]...)
	p1Payload = append(p1Payload, second...)
	p1Payload = append(p1Payload, repeatByte(0xFF, 184-len(p1Payload))...)
	p1 := mkPacket(testCuePID, 1, true, 0, testAF{}, p1Payload)

	in := []*packet.Packet{p0, p1}
	before := clonePackets(in)
	out, err := retime.Shift(in, retime.Config{CuePIDs: []int{testCuePID}, Delta90k: delta})
	if err != nil {
		t.Fatal(err)
	}
	if out[1][4] != byte(tailLen) {
		t.Fatalf("pointer_field moved or changed: %d", out[1][4])
	}

	// Reassemble section one across the two packets using logical positions.
	p0Pay := payloadOf(t, out[0])
	p1Pay := payloadOf(t, out[1])
	rebuilt := make([]byte, 0, len(first))
	rebuilt = append(rebuilt, p0Pay[1:]...) // skip pointer_field
	rebuilt = append(rebuilt, p1Pay[1:1+tailLen]...)
	if len(rebuilt) != len(first) {
		t.Fatalf("reassembled section length %d want %d", len(rebuilt), len(first))
	}
	adj := uint40Independent(rebuilt[4:9]) & 0x1FFFFFFFF
	if adj != modBig(8080, delta, 1<<33) {
		t.Fatalf("split section adjustment wrong: %d", adj)
	}
	if mpeg2CRC32(rebuilt[:len(rebuilt)-4]) != binary.BigEndian.Uint32(rebuilt[len(rebuilt)-4:]) {
		t.Fatal("split section CRC does not verify across the pointer boundary")
	}
	// splice_null has no command bytes: the descriptor loop starts at section
	// offset 16; verify every raw descriptor byte survived the split.
	if string(rebuilt[16:16+167]) != string(repeatByte(0x33, 167)) {
		t.Fatal("descriptor bytes inside split section changed")
	}

	secondOff := 1 + tailLen
	gotSecond := p1Pay[secondOff : secondOff+len(second)]
	if got := uint40Independent(gotSecond[4:9]) & 0x1FFFFFFFF; got != modBig(9090, delta, 1<<33) {
		t.Fatalf("second section adjustment wrong: %d", got)
	}
	if got := uint40Independent(gotSecond[14:19]) & 0x1FFFFFFFF; got != 31337 {
		t.Fatal("second section command time was re-encoded")
	}
	if mpeg2CRC32(gotSecond[:len(gotSecond)-4]) != binary.BigEndian.Uint32(gotSecond[len(gotSecond)-4:]) {
		t.Fatal("second section CRC does not verify")
	}

	// Strict fidelity on both packets.
	allowed := map[int]map[int]bool{}
	allow := func(pkt, pos int) {
		if allowed[pkt] == nil {
			allowed[pkt] = map[int]bool{}
		}
		allowed[pkt][pos] = true
	}
	// Section one adjustment lives in packet 0: section offsets 4..8 map to
	// payload offsets 5..9, i.e. packet positions 9..13 (pointer field at 4).
	for pos := 9; pos <= 13; pos++ {
		allow(0, pos)
	}
	// Section one CRC is the last four logical bytes, inside packet 1 prefix.
	for i := 0; i < 4; i++ {
		allow(1, 4+1+tailLen-4+i)
	}
	base := 4 + 1 + tailLen // packet position of section two start
	for pos := base + 4; pos < base+9; pos++ {
		allow(1, pos)
	}
	for pos := base + len(second) - 4; pos < base+len(second); pos++ {
		allow(1, pos)
	}
	assertBytesEqualExcept(t, before, out, allowed)
}

// TestShiftRoleOverlap puts PCR and payload timestamps in the same packet,
// once for a media PID and once for a cue PID.
func TestShiftRoleOverlap(t *testing.T) {
	const delta int64 = 5400
	pcr := uint64(777*300 + 41)

	// PCR + PES on one PID.
	pesBytes := buildPES(0xC0, 2, 666, 0, []byte{7, 7, 7})
	media := mkPacket(testVideoPID, 0, true, 0, testAF{present: true, length: 12, pcr: &pcr}, pesBytes)
	out, err := retime.Shift([]*packet.Packet{media}, retime.Config{
		PESPIDs: []int{testVideoPID}, PCRPIDs: []int{testVideoPID}, Delta90k: delta,
	})
	if err != nil {
		t.Fatal(err)
	}
	base, ext, _ := readPCR(out[0][6:12])
	if base != modBig(777, delta, 1<<33) || ext != 41 {
		t.Fatalf("overlap PCR wrong: base=%d ext=%d", base, ext)
	}
	_, pts, _ := readPTS33(payloadOf(t, out[0])[9:14])
	if pts != modBig(666, delta, 1<<33) {
		t.Fatalf("overlap PTS wrong: %d", pts)
	}

	// PCR + cue section on one PID.
	sec := buildSCTE35Section(222, 0x00, nil, nil, 0)
	cuePayload := append([]byte{0x00}, sec...)
	cuePayload = append(cuePayload, repeatByte(0xFF, 184-len(cuePayload))...)
	cue := mkPacket(testCuePID, 0, true, 0, testAF{present: true, length: 12, pcr: &pcr}, cuePayload)
	out, err = retime.Shift([]*packet.Packet{cue}, retime.Config{
		PCRPIDs: []int{testCuePID}, CuePIDs: []int{testCuePID}, Delta90k: delta,
	})
	if err != nil {
		t.Fatal(err)
	}
	base, ext, _ = readPCR(out[0][6:12])
	if base != modBig(777, delta, 1<<33) || ext != 41 {
		t.Fatalf("cue overlap PCR wrong: base=%d ext=%d", base, ext)
	}
	gotSec := payloadOf(t, out[0])[1 : 1+len(sec)]
	if got := uint40Independent(gotSec[4:9]) & 0x1FFFFFFFF; got != modBig(222, delta, 1<<33) {
		t.Fatalf("cue overlap adjustment wrong: %d", got)
	}
}

// TestShiftSpliceInsertUntouched ensures opaque command bytes, including
// splice_time and break_duration, survive while only adjustment shifts.
func TestShiftSpliceInsertUntouched(t *testing.T) {
	const delta int64 = 12345
	cmd := rawSpliceInsertCmd()
	sec := buildSCTE35Section(5000, 0x05, cmd, nil, 0)
	payload := append([]byte{0x00}, sec...)
	payload = append(payload, repeatByte(0xFF, 184-len(payload))...)
	pkt := mkPacket(testCuePID, 0, true, 0, testAF{}, payload)
	out, err := retime.Shift([]*packet.Packet{pkt}, retime.Config{
		CuePIDs: []int{testCuePID}, Delta90k: delta,
	})
	if err != nil {
		t.Fatal(err)
	}
	got := payloadOf(t, out[0])[1 : 1+len(sec)]
	// The splice_insert command body follows the command type byte at section
	// offset 13, so the opaque command occupies offsets 14 .. 14+len(cmd).
	if string(got[14:14+len(cmd)]) != string(cmd) {
		t.Fatal("splice_insert command bytes were re-encoded")
	}
	if got := uint40Independent(got[4:9]) & 0x1FFFFFFFF; got != modBig(5000, delta, 1<<33) {
		t.Fatalf("adjustment wrong: %d", got)
	}
	if mpeg2CRC32(got[:len(got)-4]) != binary.BigEndian.Uint32(got[len(got)-4:]) {
		t.Fatal("CRC does not verify")
	}
}

// TestShiftNilPacket reports the offending packet index.
func TestShiftNilPacket(t *testing.T) {
	_, err := retime.Shift([]*packet.Packet{nil}, retime.Config{Delta90k: 1})
	expectErrContains(t, err, "packet index 0", "nil")
}
