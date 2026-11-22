package flac_test

import (
	"bytes"
	"crypto/md5"
	"errors"
	"fmt"
	"io"
	"reflect"
	"testing"

	"github.com/mewkiz/flac"
	"github.com/mewkiz/flac/frame"
	"github.com/mewkiz/flac/meta"
	"github.com/mewkiz/flac/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Test helpers: synthetic frames, reference streams and faulting writers.
// ---------------------------------------------------------------------------

// testParams describes a small synthetic audio configuration.
type testParams struct {
	bps       uint8
	channels  frame.Channels
	rate      uint32
	blockSize uint16
}

func defaultParams() testParams {
	return testParams{bps: 16, channels: frame.ChannelsLR, rate: 44100, blockSize: 64}
}

func (p testParams) streamInfo() *meta.StreamInfo {
	return &meta.StreamInfo{
		BlockSizeMin:  p.blockSize,
		BlockSizeMax:  p.blockSize,
		SampleRate:    p.rate,
		NChannels:     uint8(p.channels.Count()),
		BitsPerSample: p.bps,
	}
}

// genSample produces deterministic, bit-width-legal PCM samples.
func genSample(p testParams, ch, i int) int32 {
	limit := int32(1) << (p.bps - 1)
	v := int32((i*73+ch*1009)%int(limit)) - limit/2
	return v
}

// makeFrame builds a frame with verbatim subframes matching p.
func makeFrame(p testParams) *frame.Frame {
	f := &frame.Frame{
		Header: frame.Header{
			HasFixedBlockSize: true,
			BlockSize:         p.blockSize,
			SampleRate:        p.rate,
			Channels:          p.channels,
			BitsPerSample:     p.bps,
		},
	}
	for ch := 0; ch < p.channels.Count(); ch++ {
		samples := make([]int32, p.blockSize)
		for i := range samples {
			samples[i] = genSample(p, ch, i)
		}
		f.Subframes = append(f.Subframes, &frame.Subframe{
			SubHeader: frame.SubHeader{Pred: frame.PredVerbatim},
			NSamples:  int(p.blockSize),
			Samples:   samples,
		})
	}
	return f
}

// makeFixedFrame builds a frame whose subframes use explicit fixed prediction.
func makeFixedFrame(p testParams, order int, method frame.ResidualCodingMethod) *frame.Frame {
	f := makeFrame(p)
	for _, sf := range f.Subframes {
		sf.Pred = frame.PredFixed
		sf.Order = order
		sf.ResidualCodingMethod = method
		sf.RiceSubframe = &frame.RiceSubframe{
			PartOrder:  0,
			Partitions: []frame.RicePartition{{Param: uint(order)}},
		}
	}
	return f
}

// makeFIRFrame builds a frame with an explicit FIR predictor.
func makeFIRFrame(p testParams, order int, method frame.ResidualCodingMethod) *frame.Frame {
	f := makeFrame(p)
	coeffs := make([]int32, order)
	for i := range coeffs {
		coeffs[i] = int32(i + 1) // small, precision-4 legal coefficients.
	}
	for _, sf := range f.Subframes {
		sf.Pred = frame.PredFIR
		sf.Order = order
		sf.CoeffPrec = 4
		sf.CoeffShift = 0
		sf.Coeffs = coeffs
		sf.ResidualCodingMethod = method
		sf.RiceSubframe = &frame.RiceSubframe{
			PartOrder:  0,
			Partitions: []frame.RicePartition{{Param: 2}},
		}
	}
	return f
}

// makeEscapedFrame builds a fixed order-0 frame whose Rice partition is an
// escaped binary partition of the given width.
func makeEscapedFrame(p testParams, paramSize uint8, escapedWidth uint8) *frame.Frame {
	f := makeFrame(p)
	escape := uint32(1)<<paramSize - 1
	for _, sf := range f.Subframes {
		sf.Pred = frame.PredFixed
		sf.Order = 0
		if paramSize == 4 {
			sf.ResidualCodingMethod = frame.ResidualCodingMethodRice1
		} else {
			sf.ResidualCodingMethod = frame.ResidualCodingMethodRice2
		}
		sf.RiceSubframe = &frame.RiceSubframe{
			PartOrder:  0,
			Partitions: []frame.RicePartition{{Param: uint(escape), EscapedBitsPerSample: uint(escapedWidth)}},
		}
	}
	return f
}

// makeWastedFrame builds a verbatim frame whose samples all have wasted low
// bits set to zero so the shift is reversible.
func makeWastedFrame(p testParams, wasted uint) *frame.Frame {
	f := makeFrame(p)
	for _, sf := range f.Subframes {
		sf.Wasted = wasted
		mask := int32(1)<<wasted - 1
		for i := range sf.Samples {
			sf.Samples[i] &^= mask
		}
	}
	return f
}

// interleavedMD5 computes the reference MD5 over little-endian interleaved PCM.
func interleavedMD5(p testParams, frames []*frame.Frame) []byte {
	h := md5.New()
	var buf [3]byte
	for _, f := range frames {
		for i := 0; i < int(p.blockSize); i++ {
			for _, sf := range f.Subframes {
				s := sf.Samples[i]
				switch {
				case p.bps <= 8:
					buf[0] = uint8(s)
					h.Write(buf[:1])
				case p.bps <= 16:
					buf[0] = uint8(s)
					buf[1] = uint8(s >> 8)
					h.Write(buf[:2])
				default:
					buf[0] = uint8(s)
					buf[1] = uint8(s >> 8)
					buf[2] = uint8(s >> 16)
					h.Write(buf[:])
				}
			}
		}
	}
	return h.Sum(nil)
}

// collectPCM decodes a complete FLAC stream and returns all PCM samples.
func collectPCM(t *testing.T, data []byte) ([][]int32, *flac.Stream) {
	t.Helper()
	s, err := flac.Parse(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("parse encoded stream: %v", err)
	}
	var out [][]int32
	for {
		f, err := s.ParseNext()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatalf("parse frame: %v", err)
		}
		for _, sf := range f.Subframes {
			out = append(out, append([]int32(nil), sf.Samples...))
		}
	}
	return out, s
}

var errFault = errors.New("synthetic writer fault")

// faultMode selects how a faulting writer misbehaves once its budget runs out.
type faultMode int

const (
	faultError faultMode = iota // return (0, errFault)
	faultShort                  // short write with nil error
	faultZero                   // zero write with nil error
)

// faultWriter cuts the stream at a fixed absolute byte offset, in three
// different ways. It also records every write after the fault.
type faultWriter struct {
	buf             bytes.Buffer
	limit           int
	mode            faultMode
	faulted         bool
	postFaultWrites int
}

func (w *faultWriter) Write(p []byte) (int, error) {
	if !w.faulted {
		// Re-cut at the fixed absolute offset even when the whole frame
		// arrives in a single Write call.
		if w.buf.Len() < w.limit && w.buf.Len()+len(p) >= w.limit {
			w.faulted = true
			room := w.limit - w.buf.Len()
			if room > 0 {
				w.buf.Write(p[:room])
			}
			switch w.mode {
			case faultError:
				return room, errFault
			case faultShort:
				if room == 0 {
					return 0, nil
				}
				return room, nil
			case faultZero:
				return 0, nil
			}
		}
	} else {
		w.postFaultWrites++
	}
	return w.buf.Write(p)
}

// memoryFile is a seekable in-memory file.
type memoryFile struct {
	data []byte
	pos  int64
}

func (m *memoryFile) Write(p []byte) (int, error) {
	end := m.pos + int64(len(p))
	if end > int64(len(m.data)) {
		m.data = append(m.data, make([]byte, end-int64(len(m.data)))...)
	}
	copy(m.data[m.pos:end], p)
	m.pos = end
	return len(p), nil
}

func (m *memoryFile) Seek(offset int64, whence int) (int64, error) {
	switch whence {
	case io.SeekCurrent:
		offset += m.pos
	case io.SeekEnd:
		offset += int64(len(m.data))
	}
	if offset < 0 {
		return m.pos, errors.New("negative position")
	}
	m.pos = offset
	return offset, nil
}

// closeCounter records Close calls and may fail on close.
type closeCounter struct {
	closes int
	fail   bool
}

func (c *closeCounter) Close() error {
	c.closes++
	if c.fail {
		return errFault
	}
	return nil
}

// faultWriteSeeker is a seekable memory file whose writes can fail and whose
// seeks can be disabled, with a close counter.
type faultWriteSeeker struct {
	memoryFile
	closeCounter
	writeLimit      int
	seeksAllowed    bool
	seekCalls       int
	faulted         bool
	postFaultWrites int
}

func (f *faultWriteSeeker) Write(p []byte) (int, error) {
	if !f.faulted && f.writeLimit > 0 && f.pos+int64(len(p)) >= int64(f.writeLimit) {
		f.faulted = true
		room := int64(f.writeLimit) - f.pos
		if room < 0 {
			room = 0
		}
		if room > 0 {
			f.memoryFile.Write(p[:room])
		}
		return int(room), errFault
	}
	if f.faulted {
		f.postFaultWrites++
	}
	return f.memoryFile.Write(p)
}

func (f *faultWriteSeeker) Seek(offset int64, whence int) (int64, error) {
	f.seekCalls++
	if !f.seeksAllowed {
		return f.pos, errFault
	}
	return f.memoryFile.Seek(offset, whence)
}

// seekFailingFile allows the initial SeekCurrent (in NewEncoder) but fails on
// every later seek, simulating a back-fill seek failure on Close.
type seekFailingFile struct {
	memoryFile
	closeCounter
	seekCalls int
}

func (s *seekFailingFile) Seek(offset int64, whence int) (int64, error) {
	s.seekCalls++
	if s.seekCalls == 1 && whence == io.SeekCurrent {
		return s.memoryFile.Seek(offset, whence)
	}
	return s.pos, errFault
}

// backfillFailSeeker accepts metadata and frame writes while healthy, then
// fails every write once armed, simulating a STREAMINFO rewrite failure.
type backfillFailSeeker struct {
	memoryFile
	closeCounter
	armed bool
}

func (b *backfillFailSeeker) Write(p []byte) (int, error) {
	if b.armed {
		return 0, errFault
	}
	return b.memoryFile.Write(p)
}

// buildReferenceStream encodes nframes good frames and returns the bytes and
// the size of the metadata prefix (everything before the first frame).
func buildReferenceStream(t *testing.T, p testParams, nframes int, analysis bool) ([]byte, int, []*frame.Frame) {
	t.Helper()
	cw := &countingFile{}
	enc, err := flac.NewEncoder(cw, p.streamInfo())
	if err != nil {
		t.Fatalf("NewEncoder: %v", err)
	}
	enc.EnablePredictionAnalysis(analysis)
	var frames []*frame.Frame
	metaLen := 0
	for i := 0; i < nframes; i++ {
		f := makeFrame(p)
		if i == 0 {
			metaLen = cw.total
		}
		if err := enc.WriteFrame(f); err != nil {
			t.Fatalf("WriteFrame %d: %v", i, err)
		}
		frames = append(frames, f)
	}
	if err := enc.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	return cw.data, metaLen, frames
}

// countingFile is a seekable in-memory file that also counts accepted bytes.
type countingFile struct {
	memoryFile
	total int
}

func (c *countingFile) Write(p []byte) (int, error) {
	c.total += len(p)
	return c.memoryFile.Write(p)
}

// ---------------------------------------------------------------------------
// TestEncoderCommit: validation, atomic commit and retry semantics.
// ---------------------------------------------------------------------------

func TestEncoderCommit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		p := defaultParams()

		// --- Rejected frames must not touch output or caller state. -------
		badCases := []struct {
			name string
			mut  func(*frame.Frame)
		}{
			{"channel count mismatch", func(f *frame.Frame) { f.Channels = frame.ChannelsMono }},
			{"subframe count mismatch", func(f *frame.Frame) { f.Subframes = f.Subframes[:1] }},
			{"block size too small", func(f *frame.Frame) {
				f.BlockSize = 4
				for _, sf := range f.Subframes {
					sf.NSamples, sf.Samples = 4, sf.Samples[:4]
				}
			}},
			{"NSamples differs from block size", func(f *frame.Frame) {
				f.Subframes[0].NSamples = 32
			}},
			{"unsupported bps", func(f *frame.Frame) { f.BitsPerSample = 12 }},
			{"sample length mismatch", func(f *frame.Frame) {
				f.Subframes[0].Samples = f.Subframes[0].Samples[:10]
			}},
			{"FIR coefficient count mismatch", func(f *frame.Frame) {
				sf := f.Subframes[0]
				sf.Pred, sf.Order = frame.PredFIR, 2
				sf.CoeffPrec, sf.CoeffShift, sf.Coeffs = 4, 0, []int32{1}
				sf.ResidualCodingMethod = frame.ResidualCodingMethodRice1
				sf.RiceSubframe = &frame.RiceSubframe{Partitions: []frame.RicePartition{{Param: 2}}}
			}},
			{"Rice partition count mismatch", func(f *frame.Frame) {
				sf := f.Subframes[0]
				sf.Pred, sf.Order = frame.PredFixed, 1
				sf.ResidualCodingMethod = frame.ResidualCodingMethodRice1
				sf.RiceSubframe = &frame.RiceSubframe{PartOrder: 2, Partitions: []frame.RicePartition{{Param: 1}, {Param: 1}}}
			}},
			{"non-reversible wasted bits", func(f *frame.Frame) {
				f.Subframes[0].Wasted = 1
			}},
			{"escaped partition too narrow", func(f *frame.Frame) {
				sf := f.Subframes[0]
				for i := range sf.Samples {
					sf.Samples[i] = 1000
				}
				sf.Pred, sf.Order = frame.PredFixed, 0
				sf.ResidualCodingMethod = frame.ResidualCodingMethodRice1
				sf.RiceSubframe = &frame.RiceSubframe{Partitions: []frame.RicePartition{{Param: 0xF, EscapedBitsPerSample: 1}}}
			}},
			{"residual overflows signed 32 bits", func(f *frame.Frame) {
				sf := f.Subframes[0]
				for i := range sf.Samples {
					sf.Samples[i] = 32767 // maximum legal 16-bit sample
				}
				const order = 32
				coeffs := make([]int32, order)
				for i := range coeffs {
					coeffs[i] = 16383 // maximum legal 15-bit coefficient
				}
				// 32 terms of 16383*32767 exceed the signed 32-bit residual.
				sf.Pred, sf.Order = frame.PredFIR, order
				sf.CoeffPrec, sf.CoeffShift = 15, 0
				sf.Coeffs = coeffs
				sf.ResidualCodingMethod = frame.ResidualCodingMethodRice1
				sf.RiceSubframe = &frame.RiceSubframe{Partitions: []frame.RicePartition{{Param: 4}}}
			}},
		}
		for _, bc := range badCases {
			out := new(bytes.Buffer)
			enc, err := flac.NewEncoder(out, p.streamInfo())
			c.Check(err == nil, fmt.Sprintf("%s: NewEncoder error: %v", bc.name, err))

			bad := makeFrame(p)
			bc.mut(bad)
			snapshot := *bad
			before := out.Len()
			err = enc.WriteFrame(bad)
			c.Check(err != nil, bc.name+": expected rejection error")
			c.Check(out.Len() == before, bc.name+": rejected frame wrote bytes")
			c.Check(bad.Num == snapshot.Num, bc.name+": frame Num mutated on failure")
			c.Check(reflect.DeepEqual(bad.Subframes, snapshot.Subframes), bc.name+": subframes mutated on failure")
			c.Check(reflect.DeepEqual(bad.Header, snapshot.Header), bc.name+": header mutated on failure")

			// The encoder must remain usable after a rejected frame.
			good := makeFrame(p)
			err = enc.WriteFrame(good)
			c.Check(err == nil, fmt.Sprintf("%s: encoder unusable after rejection: %v", bc.name, err))
			c.Check(good.Num == 0, bc.name+": rejected frame advanced frame numbering")
			c.Check(enc.Close() == nil, bc.name+": Close after retry failed")
		}

		// --- Retry after correction is byte-identical to a clean run. ------
		ref, _, refFrames := buildReferenceStream(t, p, 3, false)

		mf := &memoryFile{}
		enc, err := flac.NewEncoder(mf, p.streamInfo())
		c.Check(err == nil, "retry: NewEncoder error")
		enc.EnablePredictionAnalysis(false) // match the reference stream's setting
		// Submit and reject a bad frame, then fix it and continue.
		bad := makeFrame(p)
		bad.Channels = frame.ChannelsMono
		c.Check(enc.WriteFrame(bad) != nil, "retry: bad frame not rejected")
		var retryFrames []*frame.Frame
		for i := 0; i < 3; i++ {
			f := makeFrame(p)
			c.Check(enc.WriteFrame(f) == nil, "retry: good frame rejected")
			retryFrames = append(retryFrames, f)
		}
		c.Check(enc.Close() == nil, "retry: Close failed")
		c.Check(bytes.Equal(mf.data, ref), "retry: output differs from clean encoder")
		for i := range retryFrames {
			c.Check(retryFrames[i].Num == refFrames[i].Num, "retry: frame number mismatch")
		}

		// --- Over-long unary is rejected before any unary is generated. ----
		hp := testParams{bps: 24, channels: frame.ChannelsMono, rate: p.rate, blockSize: 16}
		huge := makeFrame(hp)
		for _, sf := range huge.Subframes {
			for i := range sf.Samples {
				sf.Samples[i] = -(int32(1) << 23) // zig-zag folds to ~2^24.
			}
			sf.Pred, sf.Order = frame.PredFixed, 0
			sf.ResidualCodingMethod = frame.ResidualCodingMethodRice1
			sf.RiceSubframe = &frame.RiceSubframe{Partitions: []frame.RicePartition{{Param: 0}}}
		}
		hugeOut := new(bytes.Buffer)
		hugeEnc, err := flac.NewEncoder(hugeOut, hp.streamInfo())
		c.Check(err == nil, "huge: NewEncoder error")
		before := hugeOut.Len()
		err = hugeEnc.WriteFrame(huge)
		c.Check(errors.Is(err, flac.ErrFrameTooLarge), "huge: expected ErrFrameTooLarge, got %v", err)
		c.Check(hugeOut.Len() == before, "huge: frame bytes leaked before size rejection")
		c.Check(huge.Num == 0, "huge: Num assigned on rejected frame")
		c.Check(hugeEnc.WriteFrame(makeFrame(hp)) == nil,
			"huge: encoder unusable after size rejection")
		c.Check(hugeEnc.Close() == nil, "huge: Close failed")

		// --- All production coding paths survive the workspace round trip. -
		codingFrames := []struct {
			name string
			f    func() *frame.Frame
		}{
			{"verbatim", func() *frame.Frame { return makeFrame(p) }},
			{"fixed rice1", func() *frame.Frame { return makeFixedFrame(p, 2, frame.ResidualCodingMethodRice1) }},
			{"fixed rice2", func() *frame.Frame { return makeFixedFrame(p, 3, frame.ResidualCodingMethodRice2) }},
			{"fir rice1", func() *frame.Frame { return makeFIRFrame(p, 2, frame.ResidualCodingMethodRice1) }},
			{"fir rice2", func() *frame.Frame { return makeFIRFrame(p, 3, frame.ResidualCodingMethodRice2) }},
			{"escape rice1", func() *frame.Frame { return makeEscapedFrame(p, 4, p.bps) }},
			{"escape rice2", func() *frame.Frame { return makeEscapedFrame(p, 5, p.bps) }},
			{"wasted bits", func() *frame.Frame { return makeWastedFrame(p, 2) }},
		}
		for _, cf := range codingFrames {
			mf2 := &memoryFile{}
			e, err := flac.NewEncoder(mf2, p.streamInfo())
			c.Check(err == nil, cf.name+": NewEncoder error")
			f := cf.f()
			c.Check(e.WriteFrame(f) == nil, cf.name+": WriteFrame failed")
			c.Check(e.Close() == nil, cf.name+": Close failed")

			pcm, s := collectPCM(t, mf2.data)
			c.Check(len(pcm) == len(f.Subframes), cf.name+": decoded channel count mismatch")
			ok := true
			for ch := range f.Subframes {
				if !reflect.DeepEqual(pcm[ch], f.Subframes[ch].Samples) {
					ok = false
				}
			}
			c.Check(ok, cf.name+": decoded PCM differs from input PCM")
			c.Check(s.Info.NSamples == uint64(p.blockSize), cf.name+": NSamples mismatch")
		}

		// --- Inter-channel decorrelation assignments. ----------------------
		for _, ch := range []frame.Channels{frame.ChannelsLeftSide, frame.ChannelsSideRight, frame.ChannelsMidSide} {
			cp := testParams{bps: 16, channels: ch, rate: p.rate, blockSize: 32}
			buf := new(bytes.Buffer)
			e, err := flac.NewEncoder(buf, cp.streamInfo())
			c.Check(err == nil, "decorrelate: NewEncoder error")
			f := makeFrame(cp)
			want := makeFrame(cp)
			c.Check(e.WriteFrame(f) == nil, "decorrelate: WriteFrame failed")
			c.Check(e.Close() == nil, "decorrelate: Close failed")
			pcm, _ := collectPCM(t, buf.Bytes())
			ok := len(pcm) == len(want.Subframes)
			if ok {
				for idx := range want.Subframes {
					if !reflect.DeepEqual(pcm[idx], want.Subframes[idx].Samples) {
						ok = false
					}
				}
			}
			c.Check(ok, fmt.Sprintf("decorrelate %v: PCM mismatch", ch))
			// Analysis must not dirty the caller's verbatim marker.
			c.Check(f.Subframes[0].Pred == frame.PredVerbatim, "decorrelate: analysis mutated caller frame")
		}

		// --- Analysis on/off both decode to identical PCM. -----------------
		for _, analysis := range []bool{false, true} {
			buf := new(bytes.Buffer)
			e, err := flac.NewEncoder(buf, p.streamInfo())
			c.Check(err == nil, "analysis: NewEncoder error")
			e.EnablePredictionAnalysis(analysis)
			f := makeFrame(p)
			c.Check(e.WriteFrame(f) == nil, "analysis: WriteFrame failed")
			c.Check(e.Close() == nil, "analysis: Close failed")
			pcm, _ := collectPCM(t, buf.Bytes())
			want := makeFrame(p)
			ok := true
			for ch := range want.Subframes {
				if !reflect.DeepEqual(pcm[ch], want.Subframes[ch].Samples) {
					ok = false
				}
			}
			c.Check(ok, fmt.Sprintf("analysis=%v: PCM mismatch", analysis))
		}

		// --- Continuous frames: numbering, MD5, output length accounting. ---
		const nframes = 4
		cw := &countingFile{}
		e, err := flac.NewEncoder(cw, p.streamInfo())
		c.Check(err == nil, "continuous: NewEncoder error")
		var frames []*frame.Frame
		frameSizes := []int{}
		for i := 0; i < nframes; i++ {
			f := makeFrame(p)
			before := cw.total
			c.Check(e.WriteFrame(f) == nil, "continuous: WriteFrame failed")
			frameSizes = append(frameSizes, cw.total-before)
			c.Check(f.Num == uint64(i), "continuous: auto Num mismatch")
			frames = append(frames, f)
		}
		c.Check(e.Close() == nil, "continuous: Close failed")

		_, s := collectPCM(t, cw.data)
		c.Check(s.Info.NSamples == uint64(nframes)*uint64(p.blockSize), "continuous: total NSamples")
		c.Check(bytes.Equal(s.Info.MD5sum[:], interleavedMD5(p, frames)), "continuous: MD5 mismatch")
		minSize, maxSize := frameSizes[0], frameSizes[0]
		for _, sz := range frameSizes[1:] {
			if sz < minSize {
				minSize = sz
			}
			if sz > maxSize {
				maxSize = sz
			}
		}
		c.Check(s.Info.FrameSizeMin == uint32(minSize), "continuous: FrameSizeMin mismatch")
		c.Check(s.Info.FrameSizeMax == uint32(maxSize), "continuous: FrameSizeMax mismatch")
		c.Check(s.Info.BlockSizeMin == p.blockSize && s.Info.BlockSizeMax == p.blockSize, "continuous: block size extrema mismatch")

		// Decoded frame numbers must be sequential.
		dec, err := flac.Parse(bytes.NewReader(cw.data))
		c.Check(err == nil, "continuous: re-parse failed")
		i := 0
		for {
			f, err := dec.ParseNext()
			if err == io.EOF {
				break
			}
			c.Check(err == nil, "continuous: frame parse error")
			c.Check(f.Num == uint64(i), "continuous: decoded frame number")
			i++
		}
		c.Check(i == nframes, "continuous: decoded frame count")
	})
}

// ---------------------------------------------------------------------------
// TestEncoderIO: fault injection at header, subframe and CRC boundaries.
// ---------------------------------------------------------------------------

func TestEncoderIO(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		p := testParams{bps: 16, channels: frame.ChannelsLR, rate: 44100, blockSize: 512}
		ref, metaLen, _ := buildReferenceStream(t, p, 1, false)
		frameLen := len(ref) - metaLen
		c.Check(frameLen > 8, "reference frame too small for fault injection")

		// Absolute offsets of the frame header, a subframe midpoint and the
		// two CRC-16 bytes.
		cuts := map[string]int{
			"frame header": metaLen + 3,
			"subframe":     metaLen + frameLen/2,
			"crc first":    metaLen + frameLen - 2,
			"crc last":     metaLen + frameLen - 1,
		}
		modes := map[string]faultMode{
			"error": faultError, "short": faultShort, "zero": faultZero,
		}

		for cutName, cut := range cuts {
			for modeName, mode := range modes {
				label := cutName + "/" + modeName
				fw := &faultWriter{limit: cut, mode: mode}
				enc, err := flac.NewEncoder(fw, p.streamInfo())
				c.Check(err == nil, label+": NewEncoder error")
				enc.EnablePredictionAnalysis(false) // match the reference frame layout
				f := makeFrame(p)
				werr := enc.WriteFrame(f)
				c.Check(werr != nil, label+": expected WriteFrame error")
				c.Check(f.Num == 0, label+": Num committed despite failed write")

				// Every later operation returns the stored error and writes
				// nothing further.
				c.Check(enc.WriteFrame(makeFrame(p)) != nil, label+": WriteFrame after fault accepted")
				closeErr := enc.Close()
				c.Check(closeErr != nil, label+": Close after fault succeeded")
				c.Check(errors.Is(enc.Close(), closeErr) && errors.Is(closeErr, enc.Close()), label+": repeated Close changed result")
				c.Check(fw.postFaultWrites == 0, label+": bytes written after the fault")
				c.Check(f.Num == 0, label+": frame numbering advanced after fault")
			}
		}

		// --- Faulting WriteSeeker: no back-fill after an I/O fault. --------
		f := &faultWriteSeeker{
			memoryFile:   memoryFile{data: make([]byte, 0, 4096)},
			seeksAllowed: true,
			writeLimit:   metaLen + frameLen/2,
		}
		enc, err := flac.NewEncoder(f, p.streamInfo())
		c.Check(err == nil, "seeker fault: NewEncoder error")
		enc.EnablePredictionAnalysis(false)
		seeksAtStart := f.seekCalls
		werr := enc.WriteFrame(makeFrame(p))
		c.Check(werr != nil, "seeker fault: WriteFrame accepted")
		closeErr := enc.Close()
		// Both results wrap the same first I/O error even if their wrapper
		// identities differ.
		c.Check(errors.Is(closeErr, errFault) && errors.Is(werr, errFault), "seeker fault: Close lost the first error")
		c.Check(f.seekCalls == seeksAtStart, "seeker fault: STREAMINFO back-fill attempted")
		c.Check(f.closes == 1, "seeker fault: underlying Close call count")
		c.Check(enc.Close() != nil, "seeker fault: repeated Close succeeded")
		c.Check(f.closes == 1, "seeker fault: underlying Close called twice")
		c.Check(enc.WriteFrame(makeFrame(p)) != nil, "seeker fault: WriteFrame after Close accepted")
		c.Check(f.postFaultWrites == 0, "seeker fault: writes after fault")

		// --- Underlying Close failure is retained and reported once. -------
		failFile := &faultWriteSeeker{
			memoryFile:   memoryFile{data: make([]byte, 0, 4096)},
			seeksAllowed: true,
		}
		failFile.closeCounter.fail = true
		enc, err = flac.NewEncoder(failFile, p.streamInfo())
		c.Check(err == nil, "close-fail: NewEncoder error")
		enc.EnablePredictionAnalysis(false)
		c.Check(enc.WriteFrame(makeFrame(p)) == nil, "close-fail: WriteFrame failed")
		closeErr = enc.Close()
		c.Check(closeErr != nil, "close-fail: Close succeeded")
		c.Check(failFile.closes == 1, "close-fail: Close call count")
		c.Check(errors.Is(enc.Close(), closeErr), "close-fail: repeated Close changed result")
		c.Check(failFile.closes == 1, "close-fail: Close invoked twice")
	})
}

// ---------------------------------------------------------------------------
// TestEncoderFinalize: STREAMINFO back-fill, stream ownership and Close.
// ---------------------------------------------------------------------------

func TestEncoderFinalize(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		p := defaultParams()

		// --- Non-zero origin: prefix untouched, cursor restored. -----------
		const prefixLen = 37
		prefix := make([]byte, prefixLen)
		for i := range prefix {
			prefix[i] = byte(0xA0 + i)
		}
		mf := &memoryFile{data: append([]byte(nil), prefix...), pos: int64(prefixLen)}
		enc, err := flac.NewEncoder(mf, p.streamInfo())
		c.Check(err == nil, "origin: NewEncoder error")
		const nframes = 3
		var frames []*frame.Frame
		for i := 0; i < nframes; i++ {
			f := makeFrame(p)
			c.Check(enc.WriteFrame(f) == nil, "origin: WriteFrame failed")
			frames = append(frames, f)
		}
		audioEnd := mf.pos
		c.Check(enc.Close() == nil, "origin: Close failed")
		c.Check(bytes.Equal(mf.data[:prefixLen], prefix), "origin: prefix modified")
		c.Check(mf.pos == audioEnd, "origin: cursor not restored to audio end")

		// The embedded slice is an independent, decodable FLAC stream.
		embedded := mf.data[prefixLen:]
		s, err := flac.Parse(bytes.NewReader(embedded))
		c.Check(err == nil, "origin: embedded stream parse failed")
		c.Check(s.Info.NSamples == uint64(nframes)*uint64(p.blockSize), "origin: NSamples back-fill")
		c.Check(bytes.Equal(s.Info.MD5sum[:], interleavedMD5(p, frames)), "origin: MD5 back-fill")
		c.Check(s.Info.BlockSizeMin == p.blockSize && s.Info.BlockSizeMax == p.blockSize, "origin: block extrema back-fill")
		c.Check(s.Info.FrameSizeMin > 0 && s.Info.FrameSizeMax >= s.Info.FrameSizeMin, "origin: frame extrema back-fill")

		// --- Plain io.Writer works without seeking/back-fill. --------------
		buf := new(bytes.Buffer)
		plainEnc, err := flac.NewEncoder(buf, p.streamInfo())
		c.Check(err == nil, "plain: NewEncoder error")
		plainFrame := makeFrame(p)
		c.Check(plainEnc.WriteFrame(plainFrame) == nil, "plain: WriteFrame failed")
		c.Check(plainEnc.Close() == nil, "plain: Close failed")
		plainStream, err := flac.Parse(bytes.NewReader(buf.Bytes()))
		c.Check(err == nil, "plain: parse failed")
		// Without a seeker the unknown-at-creation STREAMINFO fields are left
		// untouched: NSamples stays 0, but the audio itself decodes correctly.
		c.Check(plainStream.Info.NSamples == 0, "plain: STREAMINFO unexpectedly back-filled without a seeker")
		plainPCM, _ := collectPCM(t, buf.Bytes())
		c.Check(reflect.DeepEqual(plainPCM[0], plainFrame.Subframes[0].Samples), "plain: decoded PCM mismatch")

		// --- Seek failure during back-fill is retained. --------------------
		sff := &seekFailingFile{memoryFile: memoryFile{data: make([]byte, 0, 2048)}}
		seekEnc, err := flac.NewEncoder(sff, p.streamInfo())
		c.Check(err == nil, "seek-fail: NewEncoder error")
		c.Check(seekEnc.WriteFrame(makeFrame(p)) == nil, "seek-fail: WriteFrame failed")
		seekErr := seekEnc.Close()
		c.Check(seekErr != nil, "seek-fail: Close succeeded")
		c.Check(errors.Is(seekEnc.Close(), seekErr), "seek-fail: repeated Close changed result")
		c.Check(sff.closes == 1, "seek-fail: underlying Close call count")
		c.Check(seekEnc.WriteFrame(makeFrame(p)) != nil, "seek-fail: WriteFrame after Close accepted")

		// --- Back-fill write failure is retained, prefix still intact. -----
		bwf := &backfillFailSeeker{memoryFile: memoryFile{data: append([]byte(nil), prefix...)}}
		bwf.pos = int64(prefixLen)
		backEnc, err := flac.NewEncoder(bwf, p.streamInfo())
		c.Check(err == nil, "backfill-fail: NewEncoder error")
		c.Check(backEnc.WriteFrame(makeFrame(p)) == nil, "backfill-fail: WriteFrame failed")
		// Arm the fault only for the STREAMINFO rewrite on Close.
		bwf.armed = true
		backErr := backEnc.Close()
		c.Check(backErr != nil, "backfill-fail: Close succeeded")
		c.Check(errors.Is(backEnc.Close(), backErr), "backfill-fail: repeated Close changed result")
		c.Check(bytes.Equal(bwf.data[:prefixLen], prefix), "backfill-fail: prefix modified")
		c.Check(bwf.closes == 1, "backfill-fail: underlying Close call count")

		// --- Repeated Close and WriteFrame-after-Close on a healthy stream.
		healthy := &memoryFile{data: make([]byte, 0, 2048)}
		healthyEnc, err := flac.NewEncoder(healthy, p.streamInfo())
		c.Check(err == nil, "healthy: NewEncoder error")
		c.Check(healthyEnc.WriteFrame(makeFrame(p)) == nil, "healthy: WriteFrame failed")
		c.Check(healthyEnc.Close() == nil, "healthy: first Close failed")
		c.Check(healthyEnc.Close() == nil, "healthy: repeated Close failed")
		c.Check(errors.Is(healthyEnc.WriteFrame(makeFrame(p)), flac.ErrEncoderClosed),
			"healthy: WriteFrame after Close not rejected")

		// --- Continuous legal frames with varying channel representations. -
		for _, ch := range []frame.Channels{frame.ChannelsMono, frame.ChannelsLR, frame.ChannelsLRC, frame.ChannelsMidSide} {
			cp := testParams{bps: 16, channels: ch, rate: p.rate, blockSize: 48}
			out := &memoryFile{data: make([]byte, 0, 4096)}
			e, err := flac.NewEncoder(out, cp.streamInfo())
			c.Check(err == nil, "multi: NewEncoder error")
			var want [][][]int32
			for i := 0; i < 3; i++ {
				f := makeFrame(cp)
				c.Check(e.WriteFrame(f) == nil, "multi: WriteFrame failed")
				var chs [][]int32
				for _, sf := range f.Subframes {
					chs = append(chs, append([]int32(nil), sf.Samples...))
				}
				want = append(want, chs)
			}
			c.Check(e.Close() == nil, "multi: Close failed")
			s, err := flac.Parse(bytes.NewReader(out.data))
			c.Check(err == nil, "multi: parse failed")
			c.Check(s.Info.NSamples == 3*uint64(cp.blockSize), "multi: NSamples")
			got := [][][]int32{}
			for {
				f, err := s.ParseNext()
				if err == io.EOF {
					break
				}
				c.Check(err == nil, "multi: frame parse error")
				var chs [][]int32
				for _, sf := range f.Subframes {
					chs = append(chs, append([]int32(nil), sf.Samples...))
				}
				got = append(got, chs)
			}
			c.Check(reflect.DeepEqual(got, want), fmt.Sprintf("multi %v: PCM round trip mismatch", ch))
		}

		// --- 8-bit and 24-bit streams finalize correctly. ------------------
		for _, bps := range []uint8{8, 24} {
			cp := testParams{bps: bps, channels: frame.ChannelsLR, rate: p.rate, blockSize: 32}
			mf2 := &memoryFile{data: make([]byte, 0, 2048)}
			e, err := flac.NewEncoder(mf2, cp.streamInfo())
			c.Check(err == nil, "bps: NewEncoder error")
			f := makeFrame(cp)
			c.Check(e.WriteFrame(f) == nil, "bps: WriteFrame failed")
			c.Check(e.Close() == nil, "bps: Close failed")
			s, err := flac.Parse(bytes.NewReader(mf2.data))
			c.Check(err == nil, "bps: parse failed")
			c.Check(s.Info.BitsPerSample == bps, "bps: StreamInfo bps")
			c.Check(s.Info.NSamples == uint64(cp.blockSize), "bps: NSamples")
		}
	})
}
