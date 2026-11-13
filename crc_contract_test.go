// mpa, an MPEG-1 Audio library
// Copyright (C) 2014 KORÁNDI Zoltán <korandi.z@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3 as
// published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

package mpa_test

// This file builds synthetic MPEG-1 Audio Layer I/II frames with a small
// MSB-first bit writer and checks the decoder's strict CRC contract. The
// reference CRC here is deliberately independent of the accumulator in the
// production code.

import (
	"bytes"
	"errors"
	"io"
	"testing"

	"github.com/korandiz/mpa"
)

// bitWriter packs values into a byte slice, MSB first.
type bitWriter struct {
	buf []byte
	cur byte
	n   int // number of bits accumulated in cur (0..7)
}

func (w *bitWriter) writeBits(value uint, n int) {
	for i := n - 1; i >= 0; i-- {
		w.cur <<= 1
		w.cur |= byte((value >> uint(i)) & 1)
		w.n++
		if w.n == 8 {
			w.buf = append(w.buf, w.cur)
			w.cur, w.n = 0, 0
		}
	}
}

func (w *bitWriter) align() {
	if w.n != 0 {
		w.buf = append(w.buf, w.cur<<uint(8-w.n))
		w.cur, w.n = 0, 0
	}
}

func (w *bitWriter) lenBits() int {
	return len(w.buf)*8 + w.n
}

// refCRC is the test-side reference implementation of the MPEG audio CRC:
// init 0xffff, polynomial 0x8005, MSB-first, no final XOR. It shares no code
// with the decoder's accumulator.
type refCRC struct {
	state uint16
}

func (c *refCRC) addBit(b uint) {
	in := (c.state >> 15) ^ uint16(b&1)
	c.state = (c.state << 1) ^ (in * 0x8005)
}

func (c *refCRC) addBits(value uint, n int) {
	for i := n - 1; i >= 0; i-- {
		c.addBit((value >> uint(i)) & 1)
	}
}

// crcOverRange runs the reference CRC over the header tail followed by the
// half-open bit range [startBit, endBit) of frame.
func crcOverRange(frame []byte, headerTail uint, startBit, endBit int) uint16 {
	c := refCRC{state: 0xffff}
	c.addBits(headerTail, 16)
	for pos := startBit; pos < endBit; pos++ {
		c.addBit(uint((frame[pos/8] >> uint(7-pos%8)) & 1))
	}
	return c.state
}

// flipBit toggles a single bit of frame.
func flipBit(frame []byte, bitPos int) {
	frame[bitPos/8] ^= 1 << uint(7-bitPos%8)
}

// Header field codes.
const (
	layerCode1 = 3
	layerCode2 = 2
	modeStereo = 0
	modeJoint  = 1
	modeMono   = 3
)

// Bitrate/sampling tables, duplicated from the spec for frame construction.
var refBitrate = [4][15]int{
	1: {0, 32000, 64000, 96000, 128000, 160000, 192000, 224000, 256000, 288000, 320000, 352000, 384000, 416000, 448000},
	2: {0, 32000, 48000, 56000, 64000, 80000, 96000, 112000, 128000, 160000, 192000, 224000, 256000, 320000, 384000},
}

const refSampleRate = 44100

// frameLayout records where the interesting regions of a built frame live,
// all offsets in bits unless the name says bytes.
type frameLayout struct {
	frame          []byte
	protectedStart int
	protectedEnd   int
	sampleStart    int
	ancillaryByte  int
	headerTail     uint
	storedCRC      uint16
	frameSize      int
}

// frameConfig describes a frame to build.
type frameConfig struct {
	layer     int
	mode      int
	modeExt   int
	protected bool // protection_bit == 0
	brIdx     int
}

// nChannelsOf mirrors the header semantics.
func nChannelsOf(mode int) int {
	if mode == modeMono {
		return 1
	}
	return 2
}

// layer2Nbal gives the allocation field widths for the table the decoding
// path selects at 44.1 kHz for the given channel count and bitrate index.
func layer2Nbal(nch, brIdx, sb int) int {
	if nch == 2 && brIdx == 6 {
		// aTabC
		switch {
		case sb <= 1:
			return 4
		case sb <= 7:
			return 3
		default:
			return 0
		}
	}
	if nch == 1 {
		// aTabA: rows 23..26 are the last defined ones; 27..31 are implicit
		// zero rows without a field.
		switch {
		case sb <= 10:
			return 4
		case sb <= 22:
			return 3
		case sb <= 26:
			return 2
		default:
			return 0
		}
	}
	// aTabB (joint stereo): rows 23..29 are two-bit rows.
	switch {
	case sb <= 10:
		return 4
	case sb <= 22:
		return 3
	case sb <= 29:
		return 2
	default:
		return 0
	}
}

// allocIndex is the allocation index written for subband sb when it carries
// signal. Only a handful of subbands are active, leaving room for ancillary
// data inside the fixed-size frame. Every chosen index decodes to a positive,
// ungrouped bit width. In joint stereo the set includes subbands at and above
// the bound, whose allocation is transmitted only once.
func allocIndex(cfg frameConfig, sb int) int {
	nch := nChannelsOf(cfg.mode)
	switch cfg.layer {
	case 1:
		if sb < 4 {
			return 2 // 3 bits per sample
		}
		if cfg.mode == modeJoint && sb < 6 {
			return 2 // shared subbands 4..5 in joint stereo (bound == 4)
		}
		return 0
	case 2:
		if nch == 1 { // aTabA, mono
			if sb <= 2 {
				return 2 // 3 bits per sample
			}
			return 0
		}
		if cfg.brIdx == 6 { // aTabC, stereo
			if sb == 0 || sb == 5 {
				return 4 // 4 bits per sample
			}
			return 0
		}
		// aTabB, joint stereo at 192 kbps, bound == 16
		if sb <= 3 {
			return 2 // 3 bits per sample
		}
		if cfg.mode == modeJoint && sb == 17 {
			return 3 // shared allocation, 3 bits per sample
		}
		return 0
	}
	return 0
}

// sampleCodeWidth returns the decoded width of the quantized sample codes for
// an active subband in cfg.
func sampleCodeWidth(cfg frameConfig, sb int) int {
	if cfg.layer == 2 && nChannelsOf(cfg.mode) == 2 && cfg.brIdx == 6 {
		return 4 // aTabC index 4
	}
	return 3
}

// activeSubbands reports the (channel, subband) pairs that need SCFSI and
// scalefactors. In joint stereo the allocation of subbands at and beyond the
// bound is shared (transmitted once), but the SCFSI and scalefactor fields
// still appear for both channels, mirroring the decoding path.
func activeSubbands(cfg frameConfig) [][2]int {
	nch := nChannelsOf(cfg.mode)

	var active [][2]int
	for sb := 0; sb < 32; sb++ {
		if allocIndex(cfg, sb) == 0 {
			continue
		}
		for ch := 0; ch < nch; ch++ {
			active = append(active, [2]int{ch, sb})
		}
	}
	return active
}

// buildFrame constructs one frame with nonzero content and, when protected,
// fills the crc_check field with the reference CRC of the protected region.
func buildFrame(cfg frameConfig) frameLayout {
	nch := nChannelsOf(cfg.mode)
	bound := 32
	if cfg.mode == modeJoint {
		bound = 4 * (cfg.modeExt + 1)
	}
	br := refBitrate[cfg.layer][cfg.brIdx]
	// Mirror the decoder's frame-size formula (validateHeader) exactly,
	// including the order of the integer division.
	size := 12 * br
	if cfg.layer != 1 {
		size *= 12
	}
	size /= refSampleRate
	if cfg.layer == 1 {
		size *= 4
	}

	var w bitWriter

	// Header.
	layerCode := 0
	switch cfg.layer {
	case 1:
		layerCode = layerCode1
	case 2:
		layerCode = layerCode2
	}
	protBit := 1
	if cfg.protected {
		protBit = 0
	}
	header := uint32(0xffe00000) |
		uint32(1<<20) | // 12th sync bit
		uint32(1<<19) | // ID: MPEG-1
		uint32(layerCode<<17) |
		uint32(protBit<<16) |
		uint32(cfg.brIdx<<12) |
		uint32(0<<10) | // 44.1 kHz
		uint32(cfg.mode<<6) |
		uint32(cfg.modeExt<<4)
	w.writeBits(uint(header), 32)
	headerTail := uint(header & 0xffff)

	crcBitPos := w.lenBits()
	if cfg.protected {
		w.writeBits(0, 16) // crc_check placeholder
	}

	protectedStart := w.lenBits()

	// Allocation. Joint-stereo subbands at and above the bound carry one
	// shared field instead of one per channel.
	for sb := 0; sb < 32; sb++ {
		chans := nch
		if sb >= bound {
			chans = 1
		}
		for ch := 0; ch < chans; ch++ {
			width := 4
			if cfg.layer == 2 {
				width = layer2Nbal(nch, cfg.brIdx, sb)
			}
			w.writeBits(uint(allocIndex(cfg, sb)), width)
		}
	}

	// Layer II SCFSI, one 2-bit field per allocated channel and subband.
	// Joint-stereo shares the allocation but the SCFSI fields still appear
	// for both channels.
	if cfg.layer == 2 {
		for range activeSubbands(cfg) {
			w.writeBits(1, 2) // scfsi == 1
		}
	}

	protectedEnd := w.lenBits()

	// Scalefactors.
	for range activeSubbands(cfg) {
		w.writeBits(10, 6)
		if cfg.layer == 2 {
			// scfsi == 1: the third scalefactor is present as well.
			w.writeBits(20, 6)
		}
	}

	sampleStart := w.lenBits()

	// Quantized samples.
	groups := 12
	if cfg.layer == 2 {
		groups = 36
	}
	for s := 0; s < groups; s++ {
		for sb := 0; sb < 32; sb++ {
			if allocIndex(cfg, sb) == 0 {
				continue
			}
			chans := nch
			if sb >= bound {
				chans = 1
			}
			nb := sampleCodeWidth(cfg, sb)
			for ch := 0; ch < chans; ch++ {
				if cfg.layer == 2 {
					// Three samples per granule.
					for k := 0; k < 3; k++ {
						v := 1 + (s+sb+ch+k)%(nb-1)
						w.writeBits(uint(v), nb)
					}
				} else {
					v := 1 + (s+sb+ch)%(nb-1)
					w.writeBits(uint(v), nb)
				}
			}
		}
	}

	w.align()
	ancillaryByte := len(w.buf)
	if ancillaryByte+8 > size {
		panic("frame payload leaves no room for ancillary data")
	}

	// Ancillary data pads the frame out to its exact size.
	for len(w.buf) < size {
		w.buf = append(w.buf, 0)
	}
	frame := w.buf[:size]

	var stored uint16
	if cfg.protected {
		stored = crcOverRange(frame, headerTail, protectedStart, protectedEnd)
		frame[crcBitPos/8] = byte(stored >> 8)
		frame[crcBitPos/8+1] = byte(stored)
	}

	return frameLayout{
		frame:          frame,
		protectedStart: protectedStart,
		protectedEnd:   protectedEnd,
		sampleStart:    sampleStart,
		ancillaryByte:  ancillaryByte,
		headerTail:     headerTail,
		storedCRC:      stored,
		frameSize:      size,
	}
}

// allConfigs covers Layer I/II in mono, stereo and joint stereo. All frames
// are protected and use non-free bitrates. The Layer II joint-stereo config
// uses 192 kbps so the selected allocation table (aTabB) has transmittable
// rows beyond the joint-stereo bound, exercising a shared allocation.
func allConfigs() []frameConfig {
	return []frameConfig{
		{1, modeMono, 0, true, 4},
		{1, modeStereo, 0, true, 4},
		{1, modeJoint, 0, true, 4},  // bound = 4
		{2, modeMono, 0, true, 4},   // 64 kbps, aTabA
		{2, modeStereo, 0, true, 6}, // 96 kbps, aTabC
		{2, modeJoint, 3, true, 10}, // 192 kbps, bound = 16, aTabB
	}
}

// decodeOne decodes a single frame and returns the PCM of both channels.
func decodeOne(t *testing.T, d *mpa.Decoder) [2][]float32 {
	t.Helper()
	if err := d.DecodeFrame(); err != nil {
		t.Fatalf("DecodeFrame: %v", err)
	}
	n := d.NSamples()
	out := [2][]float32{make([]float32, n), make([]float32, n)}
	d.ReadSamples(0, out[0])
	d.ReadSamples(1, out[1])
	return out
}

// TestCRCProtectedRegions verifies that CRC failures are reported exactly when
// a protected field changes, and that sample/ancillary corruption and
// unprotected frames stay on the normal decoding path.
func TestCRCProtectedRegions(t *testing.T) {
	for _, cfg := range allConfigs() {
		name := [...]string{"", "LayerI", "LayerII"}[cfg.layer] +
			"-" + [...]string{"stereo", "joint", "dual", "mono"}[cfg.mode]
		t.Run(name, func(t *testing.T) {
			good := buildFrame(cfg)

			// The stored CRC must match the independent reference.
			if got := crcOverRange(good.frame, good.headerTail, good.protectedStart, good.protectedEnd); got != good.storedCRC {
				t.Fatalf("reference CRC mismatch: %04x != %04x", got, good.storedCRC)
			}

			// Strict mode accepts the intact frame.
			strict := &mpa.Decoder{Input: bytes.NewReader(good.frame), CheckCRC: true}
			pcmStrict := decodeOne(t, strict)

			// Default (lax) mode accepts it too and produces identical PCM.
			lax := &mpa.Decoder{Input: bytes.NewReader(good.frame)}
			pcmLax := decodeOne(t, lax)
			for ch := 0; ch < strict.NChannels(); ch++ {
				for i := range pcmStrict[ch] {
					if pcmStrict[ch][i] != pcmLax[ch][i] {
						t.Fatalf("PCM differs between strict and lax mode at ch%d sample %d", ch, i)
					}
				}
			}

			// A corrupted protected field is rejected in strict mode...
			badProt := make([]byte, len(good.frame))
			copy(badProt, good.frame)
			flipBit(badProt, good.protectedStart+3)
			d := &mpa.Decoder{Input: bytes.NewReader(badProt), CheckCRC: true}
			err := d.DecodeFrame()
			var crcErr mpa.CRCError
			if !errors.As(err, &crcErr) {
				t.Fatalf("protected corruption: want CRCError, got %v", err)
			}
			if crcErr.Stored != good.storedCRC {
				t.Fatalf("Stored = %04x, want %04x", crcErr.Stored, good.storedCRC)
			}
			wantComputed := crcOverRange(badProt, good.headerTail, good.protectedStart, good.protectedEnd)
			if crcErr.Computed != wantComputed || crcErr.Computed == crcErr.Stored {
				t.Fatalf("Computed = %04x, want %04x (stored %04x)", crcErr.Computed, wantComputed, crcErr.Stored)
			}

			// ...and accepted in lax mode.
			laxBad := &mpa.Decoder{Input: bytes.NewReader(badProt)}
			if err := laxBad.DecodeFrame(); err != nil {
				t.Fatalf("lax mode rejected protected corruption: %v", err)
			}

			// Corruption in the sample region must not trigger a CRC error.
			badSample := make([]byte, len(good.frame))
			copy(badSample, good.frame)
			flipBit(badSample, good.sampleStart+5)
			d = &mpa.Decoder{Input: bytes.NewReader(badSample), CheckCRC: true}
			if err := d.DecodeFrame(); err != nil {
				t.Fatalf("sample corruption reported as CRC error: %v", err)
			}

			// Corruption in the ancillary region must not trigger one either.
			badAnc := make([]byte, len(good.frame))
			copy(badAnc, good.frame)
			ancBit := good.ancillaryByte*8 + 3
			if ancBit/8 >= len(badAnc) {
				t.Fatal("no ancillary room")
			}
			flipBit(badAnc, ancBit)
			d = &mpa.Decoder{Input: bytes.NewReader(badAnc), CheckCRC: true}
			if err := d.DecodeFrame(); err != nil {
				t.Fatalf("ancillary corruption reported as CRC error: %v", err)
			}

			// An unprotected frame with corrupted allocation is always decoded.
			unprotCfg := cfg
			unprotCfg.protected = false
			unprot := buildFrame(unprotCfg)
			flipBit(unprot.frame, unprot.protectedStart+3)
			d = &mpa.Decoder{Input: bytes.NewReader(unprot.frame), CheckCRC: true}
			if err := d.DecodeFrame(); err != nil {
				t.Fatalf("unprotected frame rejected in strict mode: %v", err)
			}
			d = &mpa.Decoder{Input: bytes.NewReader(unprot.frame)}
			if err := d.DecodeFrame(); err != nil {
				t.Fatalf("unprotected frame rejected in lax mode: %v", err)
			}

			// A structurally invalid protected field (Layer I alloc == 15)
			// cannot be fully parsed, so it must never surface as a CRCError:
			// the syntax error wins and the rest of the frame is discarded.
			if cfg.layer == 1 {
				invalid := make([]byte, len(good.frame))
				copy(invalid, good.frame)
				invalid[good.protectedStart/8] |= 0xf0 // first alloc field -> 15
				d = &mpa.Decoder{Input: bytes.NewReader(invalid), CheckCRC: true}
				err := d.DecodeFrame()
				var badCRC mpa.CRCError
				if errors.As(err, &badCRC) {
					t.Fatalf("invalid allocation reported as CRCError: %v", err)
				}
				var mal mpa.MalformedStream
				if !errors.As(err, &mal) {
					t.Fatalf("invalid allocation: want MalformedStream, got %v", err)
				}
				// The stream must be positioned at the exact next frame.
				next := &mpa.Decoder{Input: d.Input}
				if e := next.DecodeFrame(); e != io.EOF {
					t.Fatalf("after syntax error the frame remainder was not discarded: %v", e)
				}
			}
		})
	}
}

// chunkReader delivers the underlying data in reads of at most chunk bytes,
// exercising every buffering boundary.
type chunkReader struct {
	data  []byte
	chunk int
	pos   int
}

func (r *chunkReader) Read(p []byte) (int, error) {
	if r.pos >= len(r.data) {
		return 0, io.EOF
	}
	n := r.chunk
	if n > len(p) {
		n = len(p)
	}
	if n > len(r.data)-r.pos {
		n = len(r.data) - r.pos
	}
	copy(p, r.data[r.pos:r.pos+n])
	r.pos += n
	return n, nil
}

// errReader serves the first frame's header and then fails with a fixed
// sentinel error.
type errReader struct {
	head     []byte
	sentinel error
	pos      int
}

func (r *errReader) Read(p []byte) (int, error) {
	if r.pos < len(r.head) {
		n := copy(p, r.head[r.pos:])
		r.pos += n
		return n, nil
	}
	return 0, r.sentinel
}

// TestCRCRecovery checks frame-accurate recovery after a rejected frame, the
// untouched synthesis history, truncation and I/O error semantics, the
// unsupported-protection cases, and retention of CheckCRC across Input
// switches.
func TestCRCRecovery(t *testing.T) {
	for _, cfg := range allConfigs() {
		name := [...]string{"", "LayerI", "LayerII"}[cfg.layer] +
			"-" + [...]string{"stereo", "joint", "dual", "mono"}[cfg.mode]
		t.Run(name, func(t *testing.T) {
			goodA := buildFrame(cfg)
			goodC := buildFrame(cfg)

			// Bad middle frame: protected bit flipped, with a fake Layer III
			// syncword hidden in its ancillary data.
			badB := buildFrame(cfg)
			flipBit(badB.frame, badB.protectedStart+3)
			for i, b := range []byte{0xff, 0xfa, 0x90, 0x54} {
				if badB.ancillaryByte+i >= len(badB.frame) {
					t.Fatal("no ancillary room for fake syncword")
				}
				badB.frame[badB.ancillaryByte+i] = b
			}

			stream := append(append(append([]byte{}, goodA.frame...), badB.frame...), goodC.frame...)
			clean := append(append([]byte{}, goodA.frame...), goodC.frame...)

			for _, chunk := range []int{1, 3, 7, 64, 4096} {
				// Damaged stream.
				d := &mpa.Decoder{Input: &chunkReader{data: stream, chunk: chunk}, CheckCRC: true}
				pcmA := decodeOne(t, d)

				err := d.DecodeFrame()
				var crcErr mpa.CRCError
				if !errors.As(err, &crcErr) {
					t.Fatalf("chunk %d: want CRCError on bad frame, got %v", chunk, err)
				}

				// The next frame must be the real frame C, not the fake
				// syncword inside B's ancillary data.
				pcmC := decodeOne(t, d)
				if err := d.DecodeFrame(); err != io.EOF {
					t.Fatalf("chunk %d: want io.EOF after last frame, got %v", chunk, err)
				}

				// Reference: the same two good frames decoded back to back
				// with the bad one removed.
				ref := &mpa.Decoder{Input: &chunkReader{data: clean, chunk: chunk}, CheckCRC: true}
				refA := decodeOne(t, ref)
				refC := decodeOne(t, ref)

				for k := range pcmA[0] {
					if pcmA[0][k] != refA[0][k] || pcmA[1][k] != refA[1][k] {
						t.Fatalf("chunk %d: frame A PCM mismatch at sample %d", chunk, k)
					}
				}
				for k := range pcmC[0] {
					if pcmC[0][k] != refC[0][k] || pcmC[1][k] != refC[1][k] {
						t.Fatalf("chunk %d: frame C PCM mismatch at sample %d (rejected frame advanced synthesis history)", chunk, k)
					}
				}
			}
		})
	}

	// Truncated frames always report io.ErrUnexpectedEOF.
	cfg := allConfigs()[0]
	good := buildFrame(cfg)
	for _, cut := range []int{5, 6, len(good.frame) / 2, len(good.frame) - 1} {
		d := &mpa.Decoder{Input: bytes.NewReader(good.frame[:cut]), CheckCRC: true}
		err := d.DecodeFrame()
		if err != io.ErrUnexpectedEOF {
			t.Fatalf("truncation at %d bytes: want io.ErrUnexpectedEOF, got %v", cut, err)
		}
	}

	// Underlying I/O errors are passed through verbatim.
	sentinel := errors.New("synthetic read failure")
	d := &mpa.Decoder{
		Input:    &errReader{head: good.frame[:4], sentinel: sentinel},
		CheckCRC: true,
	}
	if err := d.DecodeFrame(); err != sentinel {
		t.Fatalf("I/O error: want sentinel, got %v", err)
	}

	// Protected Layer III: strict mode refuses to guess the protected region.
	// In the Layer III table bitrate index 5 is 64 kbps.
	l3Header := uint32(0xffe00000) | uint32(1<<20) | uint32(1<<19) | uint32(1<<17) | // Layer III
		uint32(0<<16) | // protected
		uint32(5<<12) | // 64 kbps, non-free
		uint32(3<<6) // mono
	l3Size := 144 * 64000 / refSampleRate
	var l3 bytes.Buffer
	for i := 0; i < 3; i++ {
		l3.WriteByte(byte(l3Header >> 24))
		l3.WriteByte(byte(l3Header >> 16))
		l3.WriteByte(byte(l3Header >> 8))
		l3.WriteByte(byte(l3Header))
		l3.Write(make([]byte, l3Size-4))
	}
	d = &mpa.Decoder{Input: bytes.NewReader(l3.Bytes()), CheckCRC: true}
	if err := d.DecodeFrame(); err != mpa.ErrUnsupportedCRC {
		t.Fatalf("protected Layer III: want ErrUnsupportedCRC, got %v", err)
	}

	// Protected free-format frame: likewise unsupported.
	ffHeader := uint32(0xffe00000) | uint32(1<<20) | uint32(1<<19) | uint32(3<<17) | // Layer I
		uint32(0<<16) | // protected
		uint32(0<<12) | // free format
		uint32(3<<6) // mono
	var ff bytes.Buffer
	ff.WriteByte(byte(ffHeader >> 24))
	ff.WriteByte(byte(ffHeader >> 16))
	ff.WriteByte(byte(ffHeader >> 8))
	ff.WriteByte(byte(ffHeader))
	ff.Write(make([]byte, 200))
	d = &mpa.Decoder{Input: bytes.NewReader(ff.Bytes()), CheckCRC: true}
	if err := d.DecodeFrame(); err != mpa.ErrUnsupportedCRC {
		t.Fatalf("protected free format: want ErrUnsupportedCRC, got %v", err)
	}

	// CheckCRC survives the automatic reset on Input switch.
	unprotCfg := cfg
	unprotCfg.protected = false
	unprot := buildFrame(unprotCfg)
	d = &mpa.Decoder{Input: bytes.NewReader(unprot.frame), CheckCRC: true}
	if err := d.DecodeFrame(); err != nil {
		t.Fatalf("unprotected warm-up frame: %v", err)
	}
	if !d.CheckCRC {
		t.Fatal("CheckCRC cleared before Input switch")
	}
	badProt := make([]byte, len(good.frame))
	copy(badProt, good.frame)
	flipBit(badProt, good.protectedStart+3)
	d.Input = bytes.NewReader(badProt)
	err := d.DecodeFrame()
	var crcErr mpa.CRCError
	if !errors.As(err, &crcErr) {
		t.Fatalf("after Input switch: want CRCError (CheckCRC not retained), got %v", err)
	}
}

// readAll drains a Reader and returns the PCM bytes and the terminal error.
func readAll(r *mpa.Reader) ([]byte, error) {
	var out []byte
	buf := make([]byte, 128)
	for {
		n, err := r.Read(buf)
		out = append(out, buf[:n]...)
		if err != nil {
			return out, err
		}
	}
}

// TestCRCReader makes sure the streaming Reader surfaces CRC failures instead
// of turning the rejected frame into successful PCM output.
func TestCRCReader(t *testing.T) {
	for _, cfg := range allConfigs() {
		name := [...]string{"", "LayerI", "LayerII"}[cfg.layer] +
			"-" + [...]string{"stereo", "joint", "dual", "mono"}[cfg.mode]
		t.Run(name, func(t *testing.T) {
			goodA := buildFrame(cfg)
			goodC := buildFrame(cfg)
			badB := buildFrame(cfg)
			flipBit(badB.frame, badB.protectedStart+3)
			stream := append(append(append([]byte{}, goodA.frame...), badB.frame...), goodC.frame...)

			// Strict Reader: it must fail with CRCError, and the only PCM
			// emitted must be frame A's.
			r := &mpa.Reader{
				Decoder: &mpa.Decoder{Input: bytes.NewReader(stream), CheckCRC: true},
				Format:  mpa.S16_LE,
			}
			out, err := readAll(r)
			var crcErr mpa.CRCError
			if !errors.As(err, &crcErr) {
				t.Fatalf("Reader: want CRCError, got %v", err)
			}
			wantPrefix, err := readAll(&mpa.Reader{
				Decoder: &mpa.Decoder{Input: bytes.NewReader(goodA.frame), CheckCRC: true},
				Format:  mpa.S16_LE,
			})
			if err != nil && err != io.EOF {
				t.Fatalf("reference Reader: %v", err)
			}
			if !bytes.Equal(out, wantPrefix) {
				t.Fatalf("Reader emitted %d bytes before error, want exactly %d bytes of frame A", len(out), len(wantPrefix))
			}

			// Lax Reader on the same stream must deliver all three frames.
			lax := &mpa.Reader{Decoder: &mpa.Decoder{Input: bytes.NewReader(stream)}, Format: mpa.S16_LE}
			laxOut, laxErr := readAll(lax)
			if laxErr != io.EOF {
				t.Fatalf("lax Reader: want io.EOF, got %v", laxErr)
			}
			nSamples := 384
			if cfg.layer == 2 {
				nSamples = 1152
			}
			wantLen := 3 * 2 * 2 * nSamples // 3 frames, stereo, 16-bit
			if len(laxOut) != wantLen {
				t.Fatalf("lax Reader emitted %d bytes, want %d", len(laxOut), wantLen)
			}

			// Strict Reader over an unprotected corrupted stream succeeds.
			unprotCfg := cfg
			unprotCfg.protected = false
			unprot := buildFrame(unprotCfg)
			flipBit(unprot.frame, unprot.protectedStart+3)
			ok := &mpa.Reader{
				Decoder: &mpa.Decoder{Input: bytes.NewReader(unprot.frame), CheckCRC: true},
				Format:  mpa.S16_LE,
			}
			if _, err := readAll(ok); err != io.EOF {
				t.Fatalf("unprotected stream through Reader: want io.EOF, got %v", err)
			}
		})
	}
}
