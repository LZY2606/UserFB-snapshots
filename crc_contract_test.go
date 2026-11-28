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

package mpa

import (
	"bytes"
	"errors"
	"io"
	"testing"
)

// The tests in this file are self-contained: they build MPEG-1 Layer I/II
// frames with a bit writer and compare the decoder against an independent
// reference implementation of the CRC. They do not rely on the ISO compliance
// vectors, which are not distributed with the source.

// crcBitWriter writes bits MSB-first.
type crcBitWriter struct {
	buf   []byte
	nbits int
}

func (w *crcBitWriter) write(v uint32, n int) {
	for i := n - 1; i >= 0; i-- {
		if w.nbits%8 == 0 {
			w.buf = append(w.buf, 0)
		}
		if (v>>uint(i))&1 != 0 {
			w.buf[w.nbits/8] |= 1 << uint(7-w.nbits%8)
		}
		w.nbits++
	}
}

// patch16 overwrites 16 bits at the given bit position.
func (w *crcBitWriter) patch16(pos int, v uint16) {
	for i := 0; i < 16; i++ {
		bit := (v >> uint(15-i)) & 1
		idx, mask := (pos+i)/8, byte(1)<<uint(7-(pos+i)%8)
		if bit != 0 {
			w.buf[idx] |= mask
		} else {
			w.buf[idx] &^= mask
		}
	}
}

func crcBitsOf(v uint32, n int) []int {
	out := make([]int, 0, n)
	for i := n - 1; i >= 0; i-- {
		out = append(out, int((v>>uint(i))&1))
	}
	return out
}

// crcRef is an independent reference implementation of the MPEG-1 Audio CRC:
// initial value 0xffff, polynomial 0x8005, MSB-first, no final XOR.
func crcRef(bits []int) uint16 {
	reg := uint16(0xffff)
	for _, b := range bits {
		msb := int(reg>>15) & 1
		reg <<= 1
		if msb^b != 0 {
			reg ^= 0x8005
		}
	}
	return reg
}

// crcBitRange is a half-open range of bit positions within a frame.
type crcBitRange struct {
	start, end int
}

func (r crcBitRange) empty() bool { return r.end <= r.start }

// crcFrameCfg describes a synthetic Layer I or II frame.
type crcFrameCfg struct {
	layer      int // 1 or 2
	protected  bool
	br, sf     int
	pad        int
	mode       int
	modeExt    int
	original   int // value of the original/copy header bit
	allocCount int // number of lowest subbands with non-zero allocation
	sfVal      int
	scfsiVal   int
	seed       int
	ancillary  []byte
}

// crcBuiltFrame is a frame plus the bit positions of its fields.
type crcBuiltFrame struct {
	data    []byte
	crc     uint16
	header  crcBitRange // the last 16 header bits
	alloc   crcBitRange
	scfsi   crcBitRange
	sf      crcBitRange
	samples crcBitRange
	anc     crcBitRange
}

func crcNChannels(mode int) int {
	if mode == ModeMono {
		return 1
	}
	return 2
}

func crcBound(mode, modeExt int) int {
	if mode == ModeJointStereo {
		return 4 * (modeExt + 1)
	}
	return 32
}

// crcTestAllocTable selects the Layer II bit allocation table using the
// standard's table selection, independently of the decoder's table map. This
// way a wrong table choice in the decoder shows up as a CRC mismatch.
func crcTestAllocTable(nch, sf, br int) *allocationTable {
	var tables [16]*allocationTable
	switch nch {
	case 1:
		switch sf {
		case 0: // 44.1 kHz
			tables = [16]*allocationTable{aTabB, aTabC, aTabC, aTabA,
				aTabA, aTabA, aTabB, aTabB, aTabB, aTabB, aTabB}
		case 1: // 48 kHz
			tables = [16]*allocationTable{aTabA, aTabC, aTabC, aTabA,
				aTabA, aTabA, aTabA, aTabA, aTabA, aTabA, aTabA}
		case 2: // 32 kHz
			tables = [16]*allocationTable{aTabB, aTabD, aTabD, aTabA,
				aTabA, aTabA, aTabB, aTabB, aTabB, aTabB, aTabB}
		}
	default:
		switch sf {
		case 0: // 44.1 kHz
			tables = [16]*allocationTable{aTabB, nil, nil, nil, aTabC,
				nil, aTabC, aTabA, aTabA, aTabA, aTabB, aTabB, aTabB,
				aTabB, aTabB}
		case 1: // 48 kHz
			tables = [16]*allocationTable{aTabA, nil, nil, nil, aTabC,
				nil, aTabC, aTabA, aTabA, aTabA, aTabA, aTabA, aTabA,
				aTabA, aTabA}
		case 2: // 32 kHz
			tables = [16]*allocationTable{aTabB, nil, nil, nil, aTabD,
				nil, aTabD, aTabA, aTabA, aTabA, aTabB, aTabB, aTabB,
				aTabB, aTabB}
		}
	}
	return tables[br]
}

func crcFrameSize(layer, br, sf, pad int) int {
	if layer == 1 {
		return (12*bitrateBps[1][br]/samplingFreqHz[sf] + pad) * 4
	}
	return 144*bitrateBps[layer][br]/samplingFreqHz[sf] + pad
}

// buildCRCFrame builds one frame. The CRC-protected field order mirrors the
// stream syntax: the last 16 header bits, the actual bit allocation (shared
// joint-stereo allocation once), and, for Layer II, the SCFSI of every actual
// channel field. Scalefactors, samples and ancillary bits are not covered.
func buildCRCFrame(cfg crcFrameCfg) crcBuiltFrame {
	var w crcBitWriter
	f := crcBuiltFrame{}

	h := uint32(0xffe00000) | 1<<20 | 1<<19
	h |= uint32(4-cfg.layer) << 17
	if !cfg.protected {
		h |= 1 << 16
	}
	h |= uint32(cfg.br) << 12
	h |= uint32(cfg.sf) << 10
	h |= uint32(cfg.pad) << 9
	h |= uint32(cfg.mode) << 6
	h |= uint32(cfg.modeExt) << 4
	h |= uint32(cfg.original) << 2

	w.write(h, 32)
	f.header = crcBitRange{16, 32}

	// Bits covered by the CRC, in stream order.
	protected := crcBitsOf(h, 32)[16:]
	addP := func(v, n int) {
		protected = append(protected, crcBitsOf(uint32(v), n)...)
		w.write(uint32(v), n)
	}

	crcPos := -1
	if cfg.protected {
		crcPos = w.nbits
		w.write(0, 16) // placeholder for crc_check
	}

	nch := crcNChannels(cfg.mode)
	bound := crcBound(cfg.mode, cfg.modeExt)
	alloc := [2][32]int{}

	f.alloc.start = w.nbits
	if cfg.layer == 1 {
		for sb := 0; sb < 32; sb++ {
			n := nch
			if sb >= bound {
				n = 1
			}
			for ch := 0; ch < n; ch++ {
				raw := 0
				if sb < cfg.allocCount {
					raw = 1 // 1..14 are legal 4-bit allocation values
				}
				addP(raw, 4)
				if raw > 0 {
					alloc[ch][sb] = raw + 1
				} else {
					alloc[ch][sb] = 0
				}
			}
		}
	} else {
		tab := crcTestAllocTable(nch, cfg.sf, cfg.br)
		if tab == nil {
			panic("buildCRCFrame: invalid test configuration")
		}
		for sb := 0; sb < 32; sb++ {
			n := nch
			if sb >= bound {
				n = 1
			}
			for ch := 0; ch < n; ch++ {
				index := 0
				if sb < cfg.allocCount && tab[sb].nbal > 0 {
					index = 1
				}
				addP(index, tab[sb].nbal)
				alloc[ch][sb] = int(tab[sb].bits[index])
			}
		}
	}
	for sb := bound; sb < 32; sb++ {
		alloc[1][sb] = alloc[0][sb]
	}
	f.alloc.end = w.nbits

	if cfg.layer == 2 {
		f.scfsi.start = w.nbits
		for sb := 0; sb < 32; sb++ {
			for ch := 0; ch < nch; ch++ {
				if alloc[ch][sb] != 0 {
					addP(cfg.scfsiVal, 2)
				}
			}
		}
		f.scfsi.end = w.nbits
	}

	f.sf.start = w.nbits
	for sb := 0; sb < 32; sb++ {
		for ch := 0; ch < nch; ch++ {
			if alloc[ch][sb] == 0 {
				continue
			}
			if cfg.layer == 1 {
				w.write(uint32(cfg.sfVal), 6)
			} else {
				w.write(uint32(cfg.sfVal), 6)
				if cfg.scfsiVal == 0 || cfg.scfsiVal == 3 {
					w.write(uint32(cfg.sfVal), 6)
				}
				if cfg.scfsiVal <= 1 {
					w.write(uint32(cfg.sfVal), 6)
				}
			}
		}
	}
	f.sf.end = w.nbits

	f.samples.start = w.nbits
	if cfg.layer == 1 {
		for s := 0; s < 12; s++ {
			for sb := 0; sb < 32; sb++ {
				n := nch
				if sb >= bound {
					n = 1
				}
				for ch := 0; ch < n; ch++ {
					a := alloc[ch][sb]
					if a == 0 {
						continue
					}
					v := (s*13 + sb*7 + ch*3 + cfg.seed) & ((1 << uint(a)) - 1)
					w.write(uint32(v), a)
				}
			}
		}
	} else {
		for gr := 0; gr < 12; gr++ {
			for sb := 0; sb < 32; sb++ {
				n := nch
				if sb >= bound {
					n = 1
				}
				for ch := 0; ch < n; ch++ {
					a := alloc[ch][sb]
					switch {
					case a > 0:
						for s := 0; s < 3; s++ {
							v := (gr*11 + sb*5 + s*3 + ch*2 + cfg.seed) & ((1 << uint(a)) - 1)
							w.write(uint32(v), a)
						}
					case a < 0:
						v := (gr*11 + sb*5 + ch*2 + cfg.seed) & ((1 << uint(-a)) - 1)
						w.write(uint32(v), -a)
					}
				}
			}
		}
	}
	f.samples.end = w.nbits

	// Ancillary data. Align to a byte boundary first so that a fake syncword
	// in the ancillary data would look like a real one to a byte-oriented
	// resynchronizer.
	for w.nbits%8 != 0 {
		w.write(0, 1)
	}
	target := crcFrameSize(cfg.layer, cfg.br, cfg.sf, cfg.pad) * 8
	if w.nbits > target {
		panic("buildCRCFrame: encoded data does not fit in frame")
	}
	f.anc.start = w.nbits
	for _, b := range cfg.ancillary {
		w.write(uint32(b), 8)
	}
	for w.nbits < target {
		w.write(0, 1)
	}
	f.anc.end = target

	f.data = w.buf
	if cfg.protected {
		f.crc = crcRef(protected)
		w.patch16(crcPos, f.crc)
	}
	return f
}

func crcFlipBit(data []byte, pos int) []byte {
	out := append([]byte(nil), data...)
	out[pos/8] ^= 1 << uint(7-pos%8)
	return out
}

func crcDecodeOne(t *testing.T, data []byte, strict bool) (*Decoder, error) {
	t.Helper()
	d := &Decoder{Input: bytes.NewReader(data), CheckCRC: strict}
	return d, d.DecodeFrame()
}

func crcCheckNonZero(t *testing.T, d *Decoder) {
	t.Helper()
	for ch := 0; ch < d.NChannels(); ch++ {
		buf := make([]float32, d.NSamples())
		d.ReadSamples(ch, buf)
		nonzero := false
		for _, v := range buf {
			if v != 0 {
				nonzero = true
				break
			}
		}
		if !nonzero {
			t.Fatalf("channel %d is silent", ch)
		}
	}
}

func TestCRCProtectedRegions(t *testing.T) {
	cases := []struct {
		name string
		cfg  crcFrameCfg
	}{
		{"layer1-mono", crcFrameCfg{layer: 1, protected: true, br: 14, sf: 0, mode: ModeMono, allocCount: 4, sfVal: 0, seed: 1}},
		{"layer1-stereo", crcFrameCfg{layer: 1, protected: true, br: 14, sf: 0, mode: ModeStereo, allocCount: 4, sfVal: 0, seed: 2}},
		{"layer1-joint", crcFrameCfg{layer: 1, protected: true, br: 14, sf: 0, mode: ModeJointStereo, modeExt: 1, allocCount: 12, sfVal: 0, seed: 3}},
		{"layer1-32khz-padding", crcFrameCfg{layer: 1, protected: true, br: 14, sf: 2, pad: 1, mode: ModeMono, allocCount: 5, sfVal: 3, seed: 4}},
		{"layer2-mono-tableA", crcFrameCfg{layer: 2, protected: true, br: 4, sf: 0, mode: ModeMono, allocCount: 4, sfVal: 0, seed: 5}},
		{"layer2-mono-tableB", crcFrameCfg{layer: 2, protected: true, br: 10, sf: 0, mode: ModeMono, allocCount: 4, sfVal: 0, seed: 6}},
		{"layer2-stereo-tableA", crcFrameCfg{layer: 2, protected: true, br: 8, sf: 0, mode: ModeStereo, allocCount: 4, sfVal: 0, seed: 7}},
		{"layer2-stereo-tableC", crcFrameCfg{layer: 2, protected: true, br: 4, sf: 0, mode: ModeStereo, allocCount: 4, sfVal: 0, seed: 8}},
		{"layer2-joint-tableB", crcFrameCfg{layer: 2, protected: true, br: 12, sf: 0, mode: ModeJointStereo, modeExt: 1, allocCount: 12, sfVal: 0, seed: 9}},
		{"layer2-joint-tableC", crcFrameCfg{layer: 2, protected: true, br: 4, sf: 0, mode: ModeJointStereo, modeExt: 1, allocCount: 6, sfVal: 0, seed: 10}},
		{"layer2-mono-32khz-tableD", crcFrameCfg{layer: 2, protected: true, br: 2, sf: 2, mode: ModeMono, allocCount: 4, sfVal: 0, seed: 11}},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			f := buildCRCFrame(tc.cfg)

			// A valid protected frame passes in strict mode.
			d, err := crcDecodeOne(t, f.data, true)
			if err != nil {
				t.Fatalf("valid frame rejected: %v", err)
			}
			if d.Layer() != tc.cfg.layer {
				t.Errorf("Layer() = %d, want %d", d.Layer(), tc.cfg.layer)
			}
			if d.Mode() != tc.cfg.mode {
				t.Errorf("Mode() = %d, want %d", d.Mode(), tc.cfg.mode)
			}
			crcCheckNonZero(t, d)

			// The same frame is accepted without strict checking.
			if _, err := crcDecodeOne(t, f.data, false); err != nil {
				t.Fatalf("valid frame rejected in default mode: %v", err)
			}

			// Protected field mutations must be detected.
			mutations := []struct {
				name string
				pos  int
			}{
				{"header", 29}, // original/copy bit: header CRC coverage
				{"allocation", f.alloc.start},
			}
			if !f.scfsi.empty() {
				mutations = append(mutations, struct {
					name string
					pos  int
				}{"scfsi", f.scfsi.start})
			}

			for _, m := range mutations {
				bad := crcFlipBit(f.data, m.pos)
				_, err := crcDecodeOne(t, bad, true)
				var ce CRCError
				if !errors.As(err, &ce) {
					t.Fatalf("%s mutation: got %v, want CRCError", m.name, err)
				}
				if ce.Stored != f.crc {
					t.Errorf("%s mutation: Stored = %#04x, want %#04x", m.name, ce.Stored, f.crc)
				}
				if ce.Computed == ce.Stored {
					t.Errorf("%s mutation: Computed unexpectedly equals Stored", m.name)
				}
				var cep *CRCError
				if !errors.As(err, &cep) {
					t.Errorf("%s mutation: errors.As with *CRCError target failed", m.name)
				}

				// Without strict checking the historical behaviour is to
				// ignore the CRC (the frame may still be syntactically valid).
				if m.name != "allocation" {
					if _, err := crcDecodeOne(t, bad, false); err != nil {
						if _, ok := err.(MalformedStream); !ok {
							t.Logf("%s mutation in default mode: %v", m.name, err)
						}
					}
				}
			}

			// Sample and ancillary mutations are not covered by the CRC.
			if f.samples.empty() || f.anc.empty() {
				t.Fatalf("test frame has no sample/ancillary bits to mutate")
			}
			for _, m := range []struct {
				name string
				pos  int
			}{
				{"samples", f.samples.start},
				{"ancillary", f.anc.start},
			} {
				mutated := crcFlipBit(f.data, m.pos)
				if _, err := crcDecodeOne(t, mutated, true); err != nil {
					t.Fatalf("%s mutation must not fail the CRC: %v", m.name, err)
				}
			}

			// Unprotected frames take the original decoding path even in
			// strict mode, and no bytes are treated as a CRC.
			unprot := tc.cfg
			unprot.protected = false
			uf := buildCRCFrame(unprot)
			if _, err := crcDecodeOne(t, uf.data, true); err != nil {
				t.Fatalf("unprotected frame rejected: %v", err)
			}
			bad := crcFlipBit(uf.data, uf.alloc.start)
			if _, err := crcDecodeOne(t, bad, true); err != nil {
				t.Fatalf("unprotected frame must be decoded without CRC: %v", err)
			}
		})
	}

	t.Run("unsupported", func(t *testing.T) {
		// CRC-protected Layer III: don't guess the protected range.
		h3 := uint32(0xffe00000) | 1<<20 | 1<<19 | 1<<17 | 1<<12 // layer 3, protected
		data3 := []byte{
			byte(h3 >> 24), byte(h3 >> 16), byte(h3 >> 8), byte(h3),
			0, 0,
		}
		_, err := crcDecodeOne(t, data3, true)
		if !errors.Is(err, ErrUnsupportedCRC) {
			t.Fatalf("protected Layer III: got %v, want ErrUnsupportedCRC", err)
		}

		// CRC-protected Layer I free format.
		hf := uint32(0xffe00000) | 1<<20 | 1<<19 | 3<<17 // layer 1, protected, free format
		dataf := []byte{
			byte(hf >> 24), byte(hf >> 16), byte(hf >> 8), byte(hf),
			0, 0,
		}
		_, err = crcDecodeOne(t, dataf, true)
		if !errors.Is(err, ErrUnsupportedCRC) {
			t.Fatalf("protected free format: got %v, want ErrUnsupportedCRC", err)
		}

		// Without strict checking, the protected Layer III frame is still
		// decoded along the original path (it just won't produce a frame here,
		// but the error must not be ErrUnsupportedCRC).
		if _, err := crcDecodeOne(t, data3, false); errors.Is(err, ErrUnsupportedCRC) {
			t.Fatalf("default mode returned ErrUnsupportedCRC")
		}
	})

	t.Run("truncated", func(t *testing.T) {
		cfg := crcFrameCfg{layer: 1, protected: true, br: 14, sf: 0,
			mode: ModeMono, allocCount: 4, sfVal: 0, seed: 42}
		f := buildCRCFrame(cfg)

		// Truncation inside the crc_check field.
		_, err := crcDecodeOne(t, f.data[:5], true)
		if err != io.ErrUnexpectedEOF {
			t.Fatalf("truncated crc_check: got %v, want io.ErrUnexpectedEOF", err)
		}
		// Truncation inside the protected allocation field.
		_, err = crcDecodeOne(t, f.data[:8], true)
		if err != io.ErrUnexpectedEOF {
			t.Fatalf("truncated allocation: got %v, want io.ErrUnexpectedEOF", err)
		}
		// Truncation of the payload in strict mode is still ErrUnexpectedEOF,
		// not a CRCError.
		cut := f.samples.start/8 + (f.samples.end-f.samples.start)/16
		if cut >= len(f.data) {
			cut = len(f.data) - 1
		}
		if cut <= f.samples.start/8 {
			t.Fatal("test frame too small")
		}
		_, err = crcDecodeOne(t, f.data[:cut], true)
		if err != io.ErrUnexpectedEOF {
			t.Fatalf("truncated frame: got %v, want io.ErrUnexpectedEOF", err)
		}
	})

	t.Run("io-error", func(t *testing.T) {
		cfg := crcFrameCfg{layer: 2, protected: true, br: 4, sf: 0,
			mode: ModeStereo, allocCount: 2, sfVal: 0, seed: 43}
		f := buildCRCFrame(cfg)
		boom := errors.New("boom")
		failAt := f.samples.start/8 + (f.samples.end-f.samples.start)/16
		r := &crcErrorReader{r: bytes.NewReader(f.data), failAt: failAt, err: boom}
		d := &Decoder{Input: r, CheckCRC: true}
		if err := d.DecodeFrame(); err != boom {
			t.Fatalf("mid-frame I/O error: got %v, want the original error", err)
		}
	})
}

// crcErrorReader fails with err once failAt bytes have been read.
type crcErrorReader struct {
	r      io.Reader
	failAt int
	read   int
	err    error
}

func (r *crcErrorReader) Read(p []byte) (int, error) {
	if r.read >= r.failAt {
		return 0, r.err
	}
	if rem := r.failAt - r.read; len(p) > rem {
		p = p[:rem]
	}
	n, err := r.r.Read(p)
	r.read += n
	if err == nil && r.read >= r.failAt {
		err = r.err
	}
	return n, err
}

// crcChunkReader returns at most max bytes from the wrapped reader.
type crcChunkReader struct {
	r   io.Reader
	max int
}

func (c *crcChunkReader) Read(p []byte) (int, error) {
	if len(p) > c.max {
		p = p[:c.max]
	}
	return c.r.Read(p)
}

// crcReadAll reads a Reader to EOF, collecting output and any CRC errors.
func crcReadAll(t *testing.T, r *Reader) (out []byte, crcErrs []CRCError, other error) {
	t.Helper()
	buf := make([]byte, 4096)
	for {
		n, err := r.Read(buf)
		out = append(out, buf[:n]...)
		if err != nil {
			var ce CRCError
			if errors.As(err, &ce) {
				crcErrs = append(crcErrs, ce)
				continue
			}
			if err == io.EOF {
				return out, crcErrs, nil
			}
			return out, crcErrs, err
		}
	}
}

func TestCRCRecovery(t *testing.T) {
	base := crcFrameCfg{
		layer: 1, protected: true, br: 14, sf: 0,
		mode: ModeMono, allocCount: 6, sfVal: 0,
	}
	cfg1 := base
	cfg1.seed = 100
	cfg1.original = 0
	cfg2 := base
	cfg2.seed = 200
	cfg2.original = 1
	cfg2.ancillary = []byte{0xff, 0xfb, 0x90, 0x64, 0xff, 0xfb, 0x90, 0x00}

	good1 := buildCRCFrame(cfg1)
	bad := buildCRCFrame(cfg2)
	// The bad frame's ancillary carries fake syncwords. The following valid
	// frame is built without them (decoding it must not depend on scanning
	// ancillary data for headers).
	goodCfg := cfg2
	goodCfg.ancillary = nil
	good2 := buildCRCFrame(goodCfg)

	// Corrupt a CRC-protected bit without updating crc_check, and leave a
	// fake syncword in the ancillary data of the rejected frame.
	badData := crcFlipBit(bad.data, bad.alloc.start)
	stream := append(append(append([]byte(nil), good1.data...), badData...), good2.data...)

	// Reference: decode the stream with the bad frame removed.
	clean := append(append([]byte(nil), good1.data...), good2.data...)
	refD := &Decoder{Input: bytes.NewReader(clean)}
	if err := refD.DecodeFrame(); err != nil {
		t.Fatalf("reference frame 1: %v", err)
	}
	ref1 := make([]float32, refD.NSamples())
	refD.ReadSamples(0, ref1)
	if err := refD.DecodeFrame(); err != nil {
		t.Fatalf("reference frame 2: %v", err)
	}
	ref2 := make([]float32, refD.NSamples())
	refD.ReadSamples(0, ref2)

	for _, chunk := range []int{1, 2, 3, 5, 7, 64, 1 << 20} {
		t.Run("chunk", func(t *testing.T) {
			d := &Decoder{
				Input:    &crcChunkReader{r: bytes.NewReader(stream), max: chunk},
				CheckCRC: true,
			}
			if err := d.DecodeFrame(); err != nil {
				t.Fatalf("chunk %d, frame 1: %v", chunk, err)
			}
			got1 := make([]float32, d.NSamples())
			d.ReadSamples(0, got1)

			err := d.DecodeFrame()
			var ce CRCError
			if !errors.As(err, &ce) {
				t.Fatalf("chunk %d, frame 2: got %v, want CRCError", chunk, err)
			}

			if err := d.DecodeFrame(); err != nil {
				t.Fatalf("chunk %d, frame 3: %v", chunk, err)
			}
			if d.Original() != (cfg2.original != 0) {
				t.Fatalf("chunk %d: resynchronized on the wrong frame", chunk)
			}
			got2 := make([]float32, d.NSamples())
			d.ReadSamples(0, got2)

			for i := range ref1 {
				if got1[i] != ref1[i] {
					t.Fatalf("chunk %d, frame 1 sample %d: %v != %v", chunk, i, got1[i], ref1[i])
				}
			}
			if len(got2) != len(ref2) {
				t.Fatalf("chunk %d: frame 3 sample count %d != %d", chunk, len(got2), len(ref2))
			}
			for i := range ref2 {
				if got2[i] != ref2[i] {
					t.Fatalf("chunk %d, frame 3 sample %d: %v != %v", chunk, i, got2[i], ref2[i])
				}
			}
		})
	}

	// The Reader must surface the CRC error instead of producing PCM for the
	// rejected frame, and its output must match decoding the clean stream.
	refR := &Reader{Decoder: &Decoder{Input: bytes.NewReader(clean)}}
	refOut, _, err := crcReadAll(t, refR)
	if err != nil {
		t.Fatalf("reference Reader: %v", err)
	}
	r := &Reader{Decoder: &Decoder{
		Input:    &crcChunkReader{r: bytes.NewReader(stream), max: 13},
		CheckCRC: true,
	}}
	out, crcErrs, err := crcReadAll(t, r)
	if err != nil {
		t.Fatalf("Reader: %v", err)
	}
	if len(crcErrs) != 1 {
		t.Fatalf("Reader saw %d CRC errors, want 1", len(crcErrs))
	}
	if !bytes.Equal(out, refOut) {
		t.Fatalf("Reader output differs from clean-stream decoding: %d vs %d bytes",
			len(out), len(refOut))
	}

	// Changing Input resets the audio history but keeps CheckCRC.
	d := &Decoder{Input: bytes.NewReader(clean), CheckCRC: true}
	if err := d.DecodeFrame(); err != nil {
		t.Fatalf("config check frame 1: %v", err)
	}
	d.Input = bytes.NewReader(badData)
	if !d.CheckCRC {
		t.Fatal("CheckCRC was cleared by the Input reset")
	}
	var ce CRCError
	if err := d.DecodeFrame(); !errors.As(err, &ce) {
		t.Fatalf("strict mode not preserved across Input change: got %v", err)
	}
}

func TestCRCReader(t *testing.T) {
	base := crcFrameCfg{
		layer: 2, protected: true, br: 4, sf: 0,
		mode: ModeStereo, allocCount: 4, sfVal: 0, seed: 300,
	}
	good := buildCRCFrame(base)
	badCfg := base
	badCfg.seed = 400
	badCfg.ancillary = []byte{0xff, 0xfb, 0x90, 0x44, 0xff, 0xfd}
	bad := buildCRCFrame(badCfg)
	badData := crcFlipBit(bad.data, bad.scfsi.start)
	stream := append(append(append([]byte(nil), good.data...), badData...), good.data...)

	// A Reader over a single bad frame must return a CRCError and no PCM.
	r := &Reader{Decoder: &Decoder{Input: bytes.NewReader(badData), CheckCRC: true}}
	buf := make([]byte, 8192)
	n, err := r.Read(buf)
	var ce CRCError
	if !errors.As(err, &ce) {
		t.Fatalf("Reader over a bad frame: got n=%d err=%v, want CRCError", n, err)
	}
	if n != 0 {
		t.Fatalf("Reader produced %d PCM bytes for a rejected frame", n)
	}
	// The error is not swallowed: a second Read must not pretend success.
	if _, err := r.Read(buf); err == nil {
		t.Fatal("Reader swallowed the CRC error on the next Read")
	}

	// Small reads across a [good, bad, good] stream give exactly the PCM of
	// the clean stream, with the error reported once.
	clean := append(append([]byte(nil), good.data...), good.data...)
	refR := &Reader{Decoder: &Decoder{Input: bytes.NewReader(clean)}}
	refOut, _, err := crcReadAll(t, refR)
	if err != nil {
		t.Fatalf("reference Reader: %v", err)
	}
	r = &Reader{Decoder: &Decoder{
		Input:    &crcChunkReader{r: bytes.NewReader(stream), max: 9},
		CheckCRC: true,
	}}
	out, crcErrs, err := crcReadAll(t, r)
	if err != nil {
		t.Fatalf("Reader: %v", err)
	}
	if len(crcErrs) != 1 {
		t.Fatalf("Reader saw %d CRC errors, want 1", len(crcErrs))
	}
	if !bytes.Equal(out, refOut) {
		t.Fatalf("Reader output differs from clean-stream decoding")
	}

	// Strict mode must not change the output for valid streams.
	plainR := &Reader{Decoder: &Decoder{Input: bytes.NewReader(clean)}}
	plainOut, _, err := crcReadAll(t, plainR)
	if err != nil {
		t.Fatalf("plain Reader: %v", err)
	}
	strictR := &Reader{Decoder: &Decoder{Input: bytes.NewReader(clean), CheckCRC: true}}
	strictOut, crcErrs, err := crcReadAll(t, strictR)
	if err != nil {
		t.Fatalf("strict Reader: %v", err)
	}
	if len(crcErrs) != 0 || !bytes.Equal(plainOut, strictOut) {
		t.Fatal("strict mode changed the output of a valid stream")
	}

	// Unprotected frames still take the original path in strict mode.
	unprot := base
	unprot.protected = false
	uf := buildCRCFrame(unprot)
	uStream := append(append([]byte(nil), uf.data...), uf.data...)
	uR := &Reader{Decoder: &Decoder{Input: bytes.NewReader(uStream), CheckCRC: true}}
	uOut, uErrs, err := crcReadAll(t, uR)
	if err != nil {
		t.Fatalf("unprotected Reader: %v", err)
	}
	nR := &Reader{Decoder: &Decoder{Input: bytes.NewReader(uStream)}}
	nOut, _, err := crcReadAll(t, nR)
	if err != nil {
		t.Fatalf("unprotected plain Reader: %v", err)
	}
	if len(uErrs) != 0 || !bytes.Equal(uOut, nOut) {
		t.Fatal("unprotected frames were not decoded along the original path")
	}
}
