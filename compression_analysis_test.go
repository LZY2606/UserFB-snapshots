package flac_test

// Automatic compression tests. The independent enumerator at the bottom of this
// file re-implements the FLAC bit accounting from the specification without
// calling any production analysis code, and is used both to predict the
// smallest legal representation of short blocks and to verify every bit the
// encoder writes.

import (
	"bytes"
	"crypto/md5"
	"io"
	"os"
	"testing"

	"github.com/mewkiz/flac"
	"github.com/mewkiz/flac/frame"
	iobits "github.com/mewkiz/flac/internal/bits"
	"github.com/mewkiz/flac/meta"
)

// enumSub is the independent description of one encoded subframe.
type enumSub struct {
	pred      string // "constant", "verbatim" or "fixed"
	order     int
	partOrder int
	ks        []uint8
	bits      uint64
}

// enumPlan is the independently enumerated best plan for one frame.
type enumPlan struct {
	channels frame.Channels
	subs     []enumSub
	bits     uint64
}

// audioCase holds one stereo or mono PCM block under test.
type audioCase struct {
	name string
	bps  uint8
	n    int
	mono bool
	l    []int32
	r    []int32
}

// makeFrame builds a verbatim, zero-wasted-bit frame for the given PCM.
func makeFrame(t *testing.T, tc audioCase) *frame.Frame {
	t.Helper()
	channels := frame.ChannelsLR
	if tc.mono {
		channels = frame.ChannelsMono
	}
	f := &frame.Frame{
		Header: frame.Header{
			HasFixedBlockSize: true,
			BlockSize:         uint16(tc.n),
			SampleRate:        48000,
			Channels:          channels,
			BitsPerSample:     tc.bps,
		},
	}
	add := func(samples []int32) {
		cp := append([]int32(nil), samples...)
		f.Subframes = append(f.Subframes, &frame.Subframe{
			SubHeader: frame.SubHeader{Pred: frame.PredVerbatim},
			NSamples:  tc.n,
			Samples:   cp,
		})
	}
	add(tc.l)
	if !tc.mono {
		add(tc.r)
	}
	return f
}

// snapshotFrame deep-copies the caller-visible state of a frame.
func snapshotFrame(f *frame.Frame) *frame.Frame {
	cp := &frame.Frame{Header: f.Header}
	for _, sf := range f.Subframes {
		cp.Subframes = append(cp.Subframes, &frame.Subframe{
			SubHeader: frame.SubHeader{Pred: sf.Pred, Order: sf.Order, Wasted: sf.Wasted},
			NSamples:  sf.NSamples,
			Samples:   append([]int32(nil), sf.Samples...),
		})
	}
	return cp
}

func framesEqual(a, b *frame.Frame) bool {
	if a.Header != b.Header || len(a.Subframes) != len(b.Subframes) {
		return false
	}
	for i, sa := range a.Subframes {
		sb := b.Subframes[i]
		if sa.SubHeader != sb.SubHeader || sa.NSamples != sb.NSamples || len(sa.Samples) != len(sb.Samples) {
			return false
		}
		for j := range sa.Samples {
			if sa.Samples[j] != sb.Samples[j] {
				return false
			}
		}
	}
	return true
}

// encodeAuto writes the frame with automatic compression enabled and returns
// the resulting file and its raw bytes.
func encodeAuto(t *testing.T, f *frame.Frame, bps uint8, nchannels uint8) (*os.File, []byte) {
	t.Helper()
	file, err := os.CreateTemp(t.TempDir(), "auto-*.flac")
	if err != nil {
		t.Fatal(err)
	}
	info := &meta.StreamInfo{
		BlockSizeMin:  uint16(f.BlockSize),
		BlockSizeMax:  uint16(f.BlockSize),
		SampleRate:    48000,
		NChannels:     nchannels,
		BitsPerSample: bps,
	}
	enc, err := flac.NewEncoder(file, info)
	if err != nil {
		t.Fatal(err)
	}
	enc.EnableAutoCompression(true)
	if err := enc.WriteFrame(f); err != nil {
		t.Fatal(err)
	}
	if err := enc.Close(); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(file.Name())
	if err != nil {
		t.Fatal(err)
	}
	return file, raw
}

// --- Independent bitstream inspection ----------------------------------------

// parsedSubframe records what the independent bitstream walk observed.
type parsedSubframe struct {
	pred      frame.Pred
	order     int
	wasted    uint
	partOrder int
	ks        []uint8
	bits      uint64
	samples   []int32 // reconstructed audio samples (post correlation input)
	maxUnaryQ uint64
}

// signExtendN interprets x as a signed n-bit value.
func signExtendN(x uint64, n uint) int32 {
	if x&(1<<(n-1)) != 0 {
		return int32(x | ^uint64(0)<<n)
	}
	return int32(x)
}

// zigZagDecode is a local copy of the FLAC signed residual folding inverse.
func zigZagDecode(u uint32) int32 {
	return int32(u>>1) ^ -int32(u&1)
}

// parseFrameHeader independently walks a frame header and returns the parsed
// header. The reader is positioned at the first subframe on return.
func parseFrameHeader(t *testing.T, br *iobits.Reader) frame.Header {
	t.Helper()
	read := func(n uint) uint64 {
		x, err := br.Read(n)
		if err != nil {
			t.Fatalf("read %d bits: %v", n, err)
		}
		return x
	}
	if read(14) != 0x3FFE {
		t.Fatal("invalid frame sync code")
	}
	read(1) // reserved
	variable := read(1) == 1
	blockCode := read(4)
	rateCode := read(4)
	channels := frame.Channels(read(4))
	var bps uint8
	switch read(3) {
	case 0x1:
		bps = 8
	case 0x2:
		bps = 12
	case 0x4:
		bps = 16
	case 0x5:
		bps = 20
	case 0x6:
		bps = 24
	default:
		t.Fatal("unexpected bits-per-sample code")
	}
	read(1) // reserved

	// FLAC "UTF-8" coded frame/sample number.
	first := read(8)
	total := 1
	for b := first; b&0x80 != 0; b <<= 1 {
		total++
	}
	for i := 1; i < total; i++ {
		read(8)
	}

	var blockSize uint16
	switch {
	case blockCode == 0x1:
		blockSize = 192
	case blockCode >= 0x2 && blockCode <= 0x5:
		blockSize = uint16(576 * (1 << (blockCode - 2)))
	case blockCode == 0x6:
		blockSize = uint16(read(8)) + 1
	case blockCode == 0x7:
		blockSize = uint16(read(16)) + 1
	default:
		blockSize = uint16(256 * (1 << (blockCode - 8)))
	}
	switch rateCode {
	case 0xC:
		read(8)
	case 0xD, 0xE:
		read(16)
	}
	read(8) // CRC-8

	return frame.Header{
		HasFixedBlockSize: !variable,
		BlockSize:         blockSize,
		SampleRate:        48000,
		Channels:          channels,
		BitsPerSample:     bps,
	}
}

// parseSubframes walks the subframes of a frame whose header has already been
// consumed, independently decoding every field the automatic encoder may
// choose. It returns the parsed subframes and the total number of bits read.
func parseSubframes(t *testing.T, br *iobits.Reader, hdr frame.Header) []parsedSubframe {
	t.Helper()
	read := func(n uint) uint64 {
		x, err := br.Read(n)
		if err != nil {
			t.Fatalf("read %d bits: %v", n, err)
		}
		return x
	}
	out := make([]parsedSubframe, hdr.Channels.Count())
	totalBits := uint64(0)
	for ch := range out {
		start := totalBits
		bps := uint(hdr.BitsPerSample)
		switch hdr.Channels {
		case frame.ChannelsSideRight:
			if ch == 0 {
				bps++
			}
		case frame.ChannelsLeftSide, frame.ChannelsMidSide:
			if ch == 1 {
				bps++
			}
		}

		// Subframe header: zero padding, 6 prediction bits, wasted-bits flag.
		if read(1) != 0 {
			t.Fatalf("subframe %d: non-zero padding", ch)
		}
		predBits := read(6)
		wasted := uint(0)
		if read(1) != 0 {
			u, err := br.ReadUnary()
			if err != nil {
				t.Fatalf("wasted bits unary: %v", err)
			}
			wasted = uint(u) + 1
			bps -= wasted
		}
		totalBits += 8

		p := parsedSubframe{wasted: wasted}
		switch {
		case predBits == 0:
			p.pred = frame.PredConstant
			p.samples = make([]int32, hdr.BlockSize)
			v := signExtendN(read(bps), bps)
			totalBits += uint64(bps)
			for i := range p.samples {
				p.samples[i] = v
			}
		case predBits == 1:
			p.pred = frame.PredVerbatim
			p.samples = make([]int32, hdr.BlockSize)
			for i := range p.samples {
				p.samples[i] = signExtendN(read(bps), bps)
			}
			totalBits += uint64(hdr.BlockSize) * uint64(bps)
		case predBits >= 0x08 && predBits <= 0x0C:
			p.pred = frame.PredFixed
			p.order = int(predBits & 0x07)
			// Warm-up samples.
			warm := make([]int32, p.order)
			for i := range warm {
				warm[i] = signExtendN(read(bps), bps)
			}
			totalBits += uint64(p.order) * uint64(bps)
			// Residual coding method: expect Rice1 (00).
			if read(2) != 0 {
				t.Fatalf("subframe %d: expected Rice1 residual method", ch)
			}
			p.partOrder = int(read(4))
			totalBits += 6
			nparts := 1 << p.partOrder
			partSize := int(hdr.BlockSize) / nparts
			residuals := make([]int32, 0, int(hdr.BlockSize)-p.order)
			for part := 0; part < nparts; part++ {
				k := uint8(read(4))
				if k == 0xF {
					t.Fatalf("subframe %d: unexpected Rice escape code", ch)
				}
				p.ks = append(p.ks, k)
				totalBits += 4
				count := partSize
				if part == 0 {
					count -= p.order
				}
				for j := 0; j < count; j++ {
					q, err := br.ReadUnary()
					if err != nil {
						t.Fatalf("unary: %v", err)
					}
					totalBits += q + 1
					if q > p.maxUnaryQ {
						p.maxUnaryQ = q
					}
					var low uint64
					if k > 0 {
						low = read(uint(k))
						totalBits += uint64(k)
					}
					u := uint32(q<<k | low)
					residuals = append(residuals, zigZagDecode(u))
				}
			}
			// Reconstruct audio samples with the fixed predictors.
			p.samples = append(p.samples, warm...)
			for i := p.order; i < int(hdr.BlockSize); i++ {
				r := residuals[i-p.order]
				var pred int64
				switch p.order {
				case 0:
				case 1:
					pred = int64(p.samples[i-1])
				case 2:
					pred = 2*int64(p.samples[i-1]) - int64(p.samples[i-2])
				case 3:
					pred = 3*int64(p.samples[i-1]) - 3*int64(p.samples[i-2]) + int64(p.samples[i-3])
				case 4:
					pred = 4*int64(p.samples[i-1]) - 6*int64(p.samples[i-2]) + 4*int64(p.samples[i-3]) - int64(p.samples[i-4])
				}
				p.samples = append(p.samples, int32(pred)+r)
			}
		default:
			t.Fatalf("subframe %d: unexpected prediction bits %06b", ch, predBits)
		}
		p.bits = totalBits - start
		out[ch] = p
	}
	return out
}

// correlate independently reverts inter-channel decorrelation.
func correlate(t *testing.T, channels frame.Channels, subs []parsedSubframe) [][]int32 {
	t.Helper()
	n := len(subs[0].samples)
	a := append([]int32(nil), subs[0].samples...)
	if channels == frame.ChannelsMono {
		return [][]int32{a}
	}
	b := append([]int32(nil), subs[1].samples...)
	switch channels {
	case frame.ChannelsLR:
		return [][]int32{a, b}
	case frame.ChannelsLeftSide:
		side := b
		right := make([]int32, n)
		for i := range side {
			right[i] = a[i] - side[i]
		}
		return [][]int32{a, right}
	case frame.ChannelsSideRight:
		side := a
		left := make([]int32, n)
		for i := range side {
			left[i] = side[i] + b[i]
		}
		return [][]int32{left, b}
	case frame.ChannelsMidSide:
		mid, side := a, b
		left := make([]int32, n)
		right := make([]int32, n)
		for i := range side {
			m := mid[i] * 2
			m |= side[i] & 1
			left[i] = (m + side[i]) / 2
			right[i] = (m - side[i]) / 2
		}
		return [][]int32{left, right}
	default:
		t.Fatalf("unexpected channels %v", channels)
		return nil
	}
}

// --- Test cases --------------------------------------------------------------

func buildCases() []audioCase {
	const n = 256
	ramp := make([]int32, n)
	for i := range ramp {
		ramp[i] = int32(i*17 - 2000)
	}

	// Identical channels.
	identicalR := append([]int32(nil), ramp...)

	// Inverse phase signals.
	inverse := make([]int32, n)
	for i := range inverse {
		inverse[i] = -ramp[i]
	}

	// Negative odd mid: every left+right sum is a negative odd number.
	negOddL := make([]int32, n)
	negOddR := make([]int32, n)
	for i := range negOddL {
		negOddL[i] = int32(-3 - 2*i)
		negOddR[i] = 0
	}

	// 24-bit extremes, including -2^23 and 2^23-1.
	ext24L := make([]int32, n)
	ext24R := make([]int32, n)
	for i := range ext24L {
		if i%2 == 0 {
			ext24L[i] = 1<<23 - 1
			ext24R[i] = -(1 << 23)
		} else {
			ext24L[i] = -(1 << 23)
			ext24R[i] = 1<<23 - 1
		}
	}

	// Segmented signal: silence followed by a loud constant level, which only
	// Rice partitions can encode cheaply.
	segmented := make([]int32, n)
	for i := n / 2; i < n; i++ {
		segmented[i] = 10000
	}
	segmentedR := append([]int32(nil), segmented...)

	// Isolated transient in an otherwise silent 24-bit block forces a unary
	// prefix far longer than 64 bits: with 16-bit PCM the k<=14 parameter cap
	// alone keeps prefixes short, so a 24-bit residual is required.
	spike := make([]int32, 64)
	spike[7] = 1 << 22

	// High-entropy pseudo-random block spanning the full 16-bit range. The two
	// channels use independent seeds so inter-channel decorrelation cannot
	// collapse either one.
	noise := make([]int32, n)
	noiseR := make([]int32, n)
	var seedL uint32 = 0x1234abcd
	var seedR uint32 = 0xdeadbeef
	for i := range noise {
		seedL = seedL*1103515245 + 12345
		noise[i] = int32(int16(seedL >> 16))
		seedR = seedR*1103515245 + 12345
		noiseR[i] = int32(int16(seedR >> 16))
	}

	// A short mono block for the smallest enumeration.
	mono := make([]int32, 64)
	for i := range mono {
		mono[i] = int32(i*7 - 100)
	}

	return []audioCase{
		{name: "identical", bps: 16, n: n, l: ramp, r: identicalR},
		{name: "inverse", bps: 16, n: n, l: ramp, r: inverse},
		{name: "negative_odd_mid", bps: 16, n: n, l: negOddL, r: negOddR},
		{name: "extremes24", bps: 24, n: n, l: ext24L, r: ext24R},
		{name: "segmented", bps: 16, n: n, l: segmented, r: segmentedR},
		{name: "long_unary", bps: 24, n: 64, mono: true, l: spike},
		{name: "high_entropy", bps: 16, n: n, l: noise, r: noiseR},
		{name: "mono_ramp", bps: 16, n: 64, mono: true, l: mono},
	}
}

// TestAutoCompressionEnumeration independently enumerates every legal channel
// representation, prediction method and Rice partition layout, and checks that
// the actual bitstream selects exactly the minimum-bit candidate with the
// documented tie-breaking order.
func TestAutoCompressionEnumeration(t *testing.T) {
	for _, tc := range buildCases() {
		t.Run(tc.name, func(t *testing.T) {
			f := makeFrame(t, tc)
			want := enumerateBest(t, tc)

			file, raw := encodeAuto(t, f, tc.bps, uint8(len(f.Subframes)))
			defer file.Close()

			br := iobits.NewReader(bytes.NewReader(raw[streamPosition(t, raw):]))
			hdr := parseFrameHeader(t, br)
			if hdr.Channels != want.channels {
				t.Fatalf("channels: expected %v, got %v", want.channels, hdr.Channels)
			}
			if hdr.BlockSize != uint16(tc.n) || hdr.BitsPerSample != tc.bps {
				t.Fatalf("frame header changed block size or bit depth: %d @ %d bits", hdr.BlockSize, hdr.BitsPerSample)
			}
			parsed := parseSubframes(t, br, hdr)

			if len(parsed) != len(want.subs) {
				t.Fatalf("subframe count: expected %d, got %d", len(want.subs), len(parsed))
			}
			for i, ws := range want.subs {
				got := parsed[i]
				if predName(got.pred) != ws.pred {
					t.Fatalf("subframe %d pred: expected %s, got %s", i, ws.pred, predName(got.pred))
				}
				if got.order != ws.order {
					t.Fatalf("subframe %d order: expected %d, got %d", i, ws.order, got.order)
				}
				if got.partOrder != ws.partOrder {
					t.Fatalf("subframe %d partition order: expected %d, got %d", i, ws.partOrder, got.partOrder)
				}
				if len(got.ks) != len(ws.ks) {
					t.Fatalf("subframe %d Rice parameter count: expected %d, got %d", i, len(ws.ks), len(got.ks))
				}
				for j := range ws.ks {
					if got.ks[j] != ws.ks[j] {
						t.Fatalf("subframe %d partition %d k: expected %d, got %d", i, j, ws.ks[j], got.ks[j])
					}
				}
				if got.bits != ws.bits {
					t.Fatalf("subframe %d bits: expected %d, got %d", i, ws.bits, got.bits)
				}
			}

			// Reconstruct the original PCM from the raw bitstream and compare
			// every sample of every channel.
			pcm := correlate(t, hdr.Channels, parsed)
			wantPCM := [][]int32{tc.l}
			if !tc.mono {
				wantPCM = append(wantPCM, tc.r)
			}
			if len(pcm) != len(wantPCM) {
				t.Fatalf("correlated channel count: expected %d, got %d", len(wantPCM), len(pcm))
			}
			for ch := range wantPCM {
				for i, s := range pcm[ch] {
					if s != wantPCM[ch][i] {
						t.Fatalf("bitstream channel %d sample %d: expected %d, got %d", ch, i, wantPCM[ch][i], s)
					}
				}
			}
			var totalBits uint64
			for _, p := range parsed {
				totalBits += p.bits
			}
			if totalBits != want.bits {
				t.Fatalf("frame subframe bits: expected %d, got %d", want.bits, totalBits)
			}

			// Semantic checks specific to the input shapes.
			switch tc.name {
			case "identical":
				if hdr.Channels == frame.ChannelsLR {
					t.Fatal("identical channels were not decorrelated")
				}
				if parsed[1].pred != frame.PredConstant {
					t.Fatalf("identical channels: side subframe should be constant, got %v", parsed[1].pred)
				}
			case "inverse":
				if hdr.Channels != frame.ChannelsMidSide || parsed[0].pred != frame.PredConstant {
					t.Fatalf("inverse phase should select mid/side with constant mid, got %v / %v", hdr.Channels, parsed[0].pred)
				}
			case "negative_odd_mid":
				// mid = (-3 + 0) >> 1 must be arithmetic: -2, never -1.
				rep := buildRepresentations(t, tc)
				var mid []int32
				for _, cand := range rep {
					if cand.channels == frame.ChannelsMidSide {
						mid = cand.containers[0]
					}
				}
				if mid[0] != -2 {
					t.Fatalf("negative odd mid: expected -2, got %d", mid[0])
				}
			case "segmented":
				found := false
				for _, p := range parsed {
					if p.pred == frame.PredFixed && p.partOrder > 0 {
						found = true
					}
				}
				if !found {
					t.Fatal("segmented signal did not benefit from Rice partitions")
				}
			case "long_unary":
				if parsed[0].maxUnaryQ <= 64 {
					t.Fatalf("expected unary prefix longer than 64 bits, got %d", parsed[0].maxUnaryQ)
				}
			case "high_entropy":
				for i, p := range parsed {
					if p.pred != frame.PredVerbatim {
						t.Fatalf("high-entropy subframe %d should fall back to verbatim, got %v", i, p.pred)
					}
				}
			}
		})
	}
}

// streamPosition returns the byte offset of the first frame by walking the
// metadata block headers in the raw stream.
func streamPosition(t *testing.T, raw []byte) int64 {
	t.Helper()
	// 4-byte "fLaC" signature followed by metadata blocks, each prefixed by a
	// 4-byte header whose low 24 bits hold the body length.
	pos := 4
	for {
		if pos+4 > len(raw) {
			t.Fatal("truncated metadata while locating first frame")
		}
		last := raw[pos]&0x80 != 0
		bodyLen := int(raw[pos+1])<<16 | int(raw[pos+2])<<8 | int(raw[pos+3])
		pos += 4 + bodyLen
		if last {
			break
		}
	}
	return int64(pos)
}

func predName(p frame.Pred) string {
	switch p {
	case frame.PredConstant:
		return "constant"
	case frame.PredVerbatim:
		return "verbatim"
	case frame.PredFixed:
		return "fixed"
	default:
		return "unknown"
	}
}

// TestAutoCompressionRoundTrip encodes real files, decodes them with the
// production decoder, and checks PCM, MD5 and StreamInfo descriptions.
func TestAutoCompressionRoundTrip(t *testing.T) {
	for _, tc := range buildCases() {
		t.Run(tc.name, func(t *testing.T) {
			f := makeFrame(t, tc)
			before := snapshotFrame(f)
			file, raw := encodeAuto(t, f, tc.bps, uint8(len(f.Subframes)))
			defer file.Close()

			// The caller's frame must be untouched, including its channel
			// assignment and verbatim prediction metadata.
			if !framesEqual(before, f) {
				t.Fatal("WriteFrame modified the caller-provided frame")
			}

			stream, err := flac.ParseFile(file.Name())
			if err != nil {
				t.Fatal(err)
			}
			defer stream.Close()
			if stream.Info.NChannels != uint8(len(f.Subframes)) {
				t.Fatalf("StreamInfo channel count changed: %d", stream.Info.NChannels)
			}
			if stream.Info.NSamples != uint64(tc.n) {
				t.Fatalf("StreamInfo sample count: expected %d, got %d", tc.n, stream.Info.NSamples)
			}
			if stream.Info.BitsPerSample != tc.bps {
				t.Fatalf("StreamInfo bits-per-sample: expected %d, got %d", tc.bps, stream.Info.BitsPerSample)
			}

			want := [][]int32{tc.l}
			if !tc.mono {
				want = append(want, tc.r)
			}
			md5sum := md5.New()
			frames := 0
			for {
				got, err := stream.ParseNext()
				if err == io.EOF {
					break
				}
				if err != nil {
					t.Fatal(err)
				}
				if got.BlockSize != uint16(tc.n) {
					t.Fatalf("block size: expected %d, got %d", tc.n, got.BlockSize)
				}
				if len(got.Subframes) != len(want) {
					t.Fatalf("decoded channel count: expected %d, got %d", len(want), len(got.Subframes))
				}
				for ch := range want {
					if len(got.Subframes[ch].Samples) != tc.n {
						t.Fatalf("decoded sample count: expected %d, got %d", tc.n, len(got.Subframes[ch].Samples))
					}
					for i, s := range got.Subframes[ch].Samples {
						if s != want[ch][i] {
							t.Fatalf("frame %d channel %d sample %d: expected %d, got %d", frames, ch, i, want[ch][i], s)
						}
					}
				}
				got.Hash(md5sum)
				frames++
			}
			if frames != 1 {
				t.Fatalf("expected 1 frame, decoded %d", frames)
			}
			if got := md5sum.Sum(nil); !bytes.Equal(got, stream.Info.MD5sum[:]) {
				t.Fatalf("MD5 mismatch: expected %x, got %x", stream.Info.MD5sum, got)
			}

			// Re-encoding the same frame must be byte-for-byte stable.
			file2, raw2 := encodeAuto(t, makeFrame(t, tc), tc.bps, uint8(len(f.Subframes)))
			defer file2.Close()
			if !bytes.Equal(raw, raw2) {
				t.Fatal("repeated encoding produced different bytes")
			}
		})
	}
}

// TestAutoCompressionValidation checks the input-range errors and the rules
// that keep explicit encoder choices on the original write path.
func TestAutoCompressionValidation(t *testing.T) {
	t.Run("out_of_range", func(t *testing.T) {
		tc := audioCase{name: "oor", bps: 24, n: 64, l: make([]int32, 64), r: make([]int32, 64)}
		tc.l[3] = 1 << 23 // one beyond the signed 24-bit maximum
		f := makeFrame(t, tc)
		file, err := os.CreateTemp(t.TempDir(), "oor-*.flac")
		if err != nil {
			t.Fatal(err)
		}
		defer file.Close()
		info := &meta.StreamInfo{BlockSizeMin: 64, BlockSizeMax: 64, SampleRate: 48000, NChannels: 2, BitsPerSample: 24}
		enc, err := flac.NewEncoder(file, info)
		if err != nil {
			t.Fatal(err)
		}
		enc.EnableAutoCompression(true)
		offset, _ := file.Seek(0, io.SeekEnd)
		if err := enc.WriteFrame(f); err == nil {
			t.Fatal("expected error for sample exceeding declared bit depth")
		}
		after, _ := file.Seek(0, io.SeekEnd)
		if after != offset {
			t.Fatalf("frame header was written before validation: %d -> %d bytes", offset, after)
		}
	})

	t.Run("unsupported_bps", func(t *testing.T) {
		tc := audioCase{name: "bps12", bps: 12, n: 64, mono: true, l: make([]int32, 64)}
		f := makeFrame(t, tc)
		file, err := os.CreateTemp(t.TempDir(), "bps12-*.flac")
		if err != nil {
			t.Fatal(err)
		}
		defer file.Close()
		info := &meta.StreamInfo{BlockSizeMin: 64, BlockSizeMax: 64, SampleRate: 48000, NChannels: 1, BitsPerSample: 12}
		enc, err := flac.NewEncoder(file, info)
		if err != nil {
			t.Fatal(err)
		}
		enc.EnableAutoCompression(true)
		if err := enc.WriteFrame(f); err == nil {
			t.Fatal("expected error for unsupported 12-bit auto compression input")
		}
	})

	t.Run("explicit_prediction_kept", func(t *testing.T) {
		// A subframe that explicitly selects constant prediction must stay on
		// the original path even with auto compression enabled.
		n := 64
		samples := make([]int32, n)
		for i := range samples {
			samples[i] = 4242
		}
		f := &frame.Frame{
			Header:    frame.Header{HasFixedBlockSize: true, BlockSize: uint16(n), SampleRate: 48000, Channels: frame.ChannelsMono, BitsPerSample: 16},
			Subframes: []*frame.Subframe{{SubHeader: frame.SubHeader{Pred: frame.PredConstant}, NSamples: n, Samples: samples}},
		}
		file, err := os.CreateTemp(t.TempDir(), "const-*.flac")
		if err != nil {
			t.Fatal(err)
		}
		defer file.Close()
		info := &meta.StreamInfo{BlockSizeMin: 64, BlockSizeMax: 64, SampleRate: 48000, NChannels: 1, BitsPerSample: 16}
		enc, err := flac.NewEncoder(file, info)
		if err != nil {
			t.Fatal(err)
		}
		enc.EnableAutoCompression(true)
		if err := enc.WriteFrame(f); err != nil {
			t.Fatal(err)
		}
		if err := enc.Close(); err != nil {
			t.Fatal(err)
		}
		stream, err := flac.ParseFile(file.Name())
		if err != nil {
			t.Fatal(err)
		}
		defer stream.Close()
		got, err := stream.ParseNext()
		if err != nil {
			t.Fatal(err)
		}
		if got.Channels != frame.ChannelsMono || got.Subframes[0].Pred != frame.PredConstant {
			t.Fatalf("explicit constant subframe was replaced: %v / %v", got.Channels, got.Subframes[0].Pred)
		}
	})

	t.Run("wasted_bits_kept", func(t *testing.T) {
		n := 64
		samples := make([]int32, n)
		for i := range samples {
			samples[i] = int32(i*8) << 2 // multiples of 4, two wasted bits
		}
		write := func(auto bool) []byte {
			f := &frame.Frame{
				Header: frame.Header{HasFixedBlockSize: true, BlockSize: uint16(n), SampleRate: 48000, Channels: frame.ChannelsMono, BitsPerSample: 16},
				Subframes: []*frame.Subframe{{
					SubHeader: frame.SubHeader{Pred: frame.PredVerbatim, Wasted: 2},
					NSamples:  n,
					Samples:   append([]int32(nil), samples...),
				}},
			}
			file, err := os.CreateTemp(t.TempDir(), "wasted-*.flac")
			if err != nil {
				t.Fatal(err)
			}
			defer file.Close()
			info := &meta.StreamInfo{BlockSizeMin: 64, BlockSizeMax: 64, SampleRate: 48000, NChannels: 1, BitsPerSample: 16}
			enc, err := flac.NewEncoder(file, info)
			if err != nil {
				t.Fatal(err)
			}
			enc.EnableAutoCompression(auto)
			if err := enc.WriteFrame(f); err != nil {
				t.Fatal(err)
			}
			if err := enc.Close(); err != nil {
				t.Fatal(err)
			}
			raw, err := os.ReadFile(file.Name())
			if err != nil {
				t.Fatal(err)
			}
			return raw
		}
		off := write(false)
		on := write(true)
		// Automatic analysis must not replace an explicit wasted-bit frame:
		// the bytes stay on the original write path.
		if !bytes.Equal(off, on) {
			t.Fatal("wasted-bit frame was modified by automatic compression")
		}
		// And the bitstream still carries a verbatim subframe with a wasted-bit
		// flag set.
		br := iobits.NewReader(bytes.NewReader(on[streamPosition(t, on):]))
		hdr := parseFrameHeader(t, br)
		parsed := parseSubframes(t, br, hdr)
		if len(parsed) != 1 || parsed[0].pred != frame.PredVerbatim || parsed[0].wasted == 0 {
			t.Fatalf("wasted-bit verbatim subframe was replaced: %+v", parsed)
		}
	})
}

// TestAutoCompressionDisabledByDefault verifies byte compatibility with the
// original verbatim encoder when automatic compression is not enabled.
func TestAutoCompressionDisabledByDefault(t *testing.T) {
	tc := audioCase{name: "default", bps: 16, n: 256, l: make([]int32, 256), r: make([]int32, 256)}
	for i := range tc.l {
		tc.l[i] = int32(i*17 - 2000)
		tc.r[i] = tc.l[i]
	}
	write := func(auto bool) []byte {
		f := makeFrame(t, tc)
		file, err := os.CreateTemp(t.TempDir(), "compat-*.flac")
		if err != nil {
			t.Fatal(err)
		}
		defer file.Close()
		info := &meta.StreamInfo{BlockSizeMin: 256, BlockSizeMax: 256, SampleRate: 48000, NChannels: 2, BitsPerSample: 16}
		enc, err := flac.NewEncoder(file, info)
		if err != nil {
			t.Fatal(err)
		}
		enc.EnableAutoCompression(auto)
		if err := enc.WriteFrame(f); err != nil {
			t.Fatal(err)
		}
		if err := enc.Close(); err != nil {
			t.Fatal(err)
		}
		raw, err := os.ReadFile(file.Name())
		if err != nil {
			t.Fatal(err)
		}
		return raw
	}
	off := write(false)
	alsoOff := write(false)
	if !bytes.Equal(off, alsoOff) {
		t.Fatal("default encoding is not deterministic")
	}
	on := write(true)
	if bytes.Equal(off, on) {
		t.Fatal("automatic compression produced no change for a compressible frame")
	}
}

// --- Independent enumerator (must not use production analysis code) ----------

type repCand struct {
	channels   frame.Channels
	containers [][]int32
	widths     []uint8
}

func foldSigned32(r int32) uint32 {
	return uint32(r<<1) ^ uint32(r>>31)
}

func riceLength(r int32, k uint8) uint64 {
	u := foldSigned32(r)
	return uint64(u>>k) + 1 + uint64(k)
}

// independentFixedResiduals mirrors the FLAC fixed predictor definitions.
func independentFixedResiduals(samples []int32, order int) ([]int32, bool) {
	n := len(samples)
	out := make([]int32, 0, n-order)
	for i := order; i < n; i++ {
		s := func(j int) int64 { return int64(samples[j]) }
		var r int64
		switch order {
		case 0:
			r = s(i)
		case 1:
			r = s(i) - s(i-1)
		case 2:
			r = s(i) - 2*s(i-1) + s(i-2)
		case 3:
			r = s(i) - 3*s(i-1) + 3*s(i-2) - s(i-3)
		case 4:
			r = s(i) - 4*s(i-1) + 6*s(i-2) - 4*s(i-3) + s(i-4)
		}
		if r < -(1<<31) || r > (1<<31-1) {
			return nil, false
		}
		out = append(out, int32(r))
	}
	return out, true
}

func independentPartitionBits(n, order, partOrder int, residuals []int32) (uint64, []uint8, bool) {
	nparts := 1 << partOrder
	if n%nparts != 0 {
		return 0, nil, false
	}
	ks := make([]uint8, nparts)
	bits := uint64(2 + 4) // Rice1 method indicator and partition order field.
	offset := 0
	for part := 0; part < nparts; part++ {
		count := n / nparts
		if part == 0 {
			count -= order
		}
		if count <= 0 {
			return 0, nil, false
		}
		best := ^uint64(0)
		var bestK uint8
		for k := uint8(0); k <= 14; k++ {
			b := uint64(4) // per-partition Rice parameter.
			for _, r := range residuals[offset : offset+count] {
				b += riceLength(r, k)
			}
			if b < best {
				best = b
				bestK = k
			}
		}
		ks[part] = bestK
		bits += best
		offset += count
	}
	return bits, ks, true
}

func enumerateSubframe(samples []int32, bps uint8) enumSub {
	n := len(samples)
	options := make([]enumSub, 0, 7)

	allEqual := true
	for _, s := range samples[1:] {
		if s != samples[0] {
			allEqual = false
			break
		}
	}
	if allEqual {
		options = append(options, enumSub{pred: "constant", bits: 8 + uint64(bps)})
	}
	options = append(options, enumSub{pred: "verbatim", bits: 8 + uint64(n)*uint64(bps)})

	for order := 0; order <= 4; order++ {
		if n <= order {
			continue
		}
		residuals, ok := independentFixedResiduals(samples, order)
		if !ok {
			continue
		}
		bestBits := ^uint64(0)
		bestOrder := -1
		var bestKS []uint8
		for p := 0; p <= 8; p++ {
			b, ks, ok := independentPartitionBits(n, order, p, residuals)
			if !ok {
				continue
			}
			total := 8 + uint64(order)*uint64(bps) + b
			if total < bestBits {
				bestBits = total
				bestOrder = p
				bestKS = ks
			}
		}
		if bestOrder >= 0 {
			options = append(options, enumSub{
				pred:      "fixed",
				order:     order,
				partOrder: bestOrder,
				ks:        bestKS,
				bits:      bestBits,
			})
		}
	}

	best := options[0]
	for _, opt := range options[1:] {
		if opt.bits < best.bits {
			best = opt
		}
	}
	return best
}

func buildRepresentations(t *testing.T, tc audioCase) []repCand {
	t.Helper()
	if tc.mono {
		return []repCand{{channels: frame.ChannelsMono, containers: [][]int32{tc.l}, widths: []uint8{tc.bps}}}
	}
	n := tc.n
	side := make([]int32, n)
	mid := make([]int32, n)
	for i := 0; i < n; i++ {
		l, r := int64(tc.l[i]), int64(tc.r[i])
		side[i] = int32(l - r)
		mid[i] = int32((l + r) >> 1) // arithmetic signed shift
	}
	sideBPS := tc.bps + 1
	return []repCand{
		{frame.ChannelsLR, [][]int32{tc.l, tc.r}, []uint8{tc.bps, tc.bps}},
		{frame.ChannelsLeftSide, [][]int32{tc.l, side}, []uint8{tc.bps, sideBPS}},
		{frame.ChannelsSideRight, [][]int32{side, tc.r}, []uint8{sideBPS, tc.bps}},
		{frame.ChannelsMidSide, [][]int32{mid, side}, []uint8{tc.bps, sideBPS}},
	}
}

func enumerateBest(t *testing.T, tc audioCase) enumPlan {
	t.Helper()
	reps := buildRepresentations(t, tc)
	best := enumPlan{bits: ^uint64(0)}
	for _, rep := range reps {
		plan := enumPlan{channels: rep.channels}
		for i, samples := range rep.containers {
			lo := -int64(1) << (rep.widths[i] - 1)
			hi := int64(1)<<(rep.widths[i]-1) - 1
			for _, s := range samples {
				if int64(s) < lo || int64(s) > hi {
					t.Fatalf("representation %v subframe %d exceeds %d-bit container", rep.channels, i, rep.widths[i])
				}
			}
			sub := enumerateSubframe(samples, rep.widths[i])
			plan.subs = append(plan.subs, sub)
			plan.bits += sub.bits
		}
		if plan.bits < best.bits {
			best = plan
		}
	}
	return best
}
