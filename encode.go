package flac

import (
	"crypto/md5"
	"errors"
	"hash"
	"io"

	"github.com/icza/bitio"
	"github.com/mewkiz/flac/meta"
	"github.com/mewkiz/pkg/errutil"
)

// maxFrameSize is the largest frame size, in bytes, representable in the
// 24-bit FrameSizeMin/FrameSizeMax fields of a STREAMINFO metadata block.
const maxFrameSize = 1<<24 - 1 // 16777215

// Sentinel errors returned by the encoder.
var (
	// ErrFrameTooLarge reports that the encoded frame would exceed the
	// 24-bit maximum frame size representable in STREAMINFO. It is returned
	// before any data is written to the underlying writer, so the call is
	// safe to retry after adjusting the frame (e.g. using a smaller block
	// size or a different residual coding).
	ErrFrameTooLarge = errors.New("flac: encoded frame exceeds the maximum frame size (16777215 bytes) representable in STREAMINFO")

	// ErrEncoderClosed reports that the encoder has already been closed.
	ErrEncoderClosed = errors.New("flac: encoder is closed")
)

// An Encoder represents a FLAC encoder.
type Encoder struct {
	// FLAC stream of encoder.
	*Stream
	// Underlying io.Writer or io.WriteCloser to the output stream.
	w io.Writer
	// Minimum and maximum block size (in samples) of frames written by encoder.
	blockSizeMin, blockSizeMax uint16
	// Minimum and maximum frame size (in bytes) of frames written by encoder.
	frameSizeMin, frameSizeMax uint32
	// MD5 running hash of unencoded audio samples.
	md5sum hash.Hash
	// Total number of samples (per channel) written by encoder.
	nsamples uint64
	// Current frame number if block size is fixed, and the first sample number
	// of the current frame otherwise.
	curNum uint64
	// AnalysisEnabled indicates whether analysis is enabled for the encoder.
	AnalysisEnabled bool

	// streamStart is the offset of the first byte ("fLaC" signature) of this
	// FLAC stream within the underlying writer. A single writer may host a
	// prefix of unrelated data, or several independent FLAC streams appended
	// one after another.
	streamStart int64
	// canSeek reports whether the underlying writer supports seeking. Only
	// seekers get their STREAMINFO back-filled on Close.
	canSeek bool
	// err is the first I/O or finalization error encountered by the encoder.
	// Once set, the encoder is poisoned: subsequent WriteFrame and Close calls
	// return it without writing any more data.
	err error
	// closed reports whether Close has been called.
	closed bool
	// closeErr is the result returned by Close; repeated Close calls return it.
	closeErr error
}

// NewEncoder returns a new FLAC encoder for the given metadata StreamInfo block
// and optional metadata blocks.
//
// The FLAC stream is written at the underlying writer's current position. If
// the writer implements io.WriteSeeker the position is recorded and used on
// Close to back-fill this stream's STREAMINFO; bytes preceding the stream are
// never modified. A plain io.Writer is fully supported, in which case the
// STREAMINFO fields unknown at creation time are left as given.
//
// By default prediction analysis is enabled. For more information, see
// Encoder.EnablePredictionAnalysis.
func NewEncoder(w io.Writer, info *meta.StreamInfo, blocks ...*meta.Block) (*Encoder, error) {
	if w == nil {
		return nil, errutil.Newf("NewEncoder: nil io.Writer")
	}
	if info == nil {
		return nil, errutil.Newf("NewEncoder: nil StreamInfo")
	}
	if err := validateStreamInfo(info); err != nil {
		return nil, errutil.Err(err)
	}

	enc := &Encoder{
		Stream: &Stream{
			Info:   info,
			Blocks: blocks,
		},
		w:               w,
		md5sum:          md5.New(),
		AnalysisEnabled: true, // enable prediction analysis by default.
	}

	// Record the origin of this independent FLAC stream. A non-seeking writer
	// is used as-is: back-filling is simply unavailable.
	if ws, ok := w.(io.WriteSeeker); ok {
		pos, err := ws.Seek(0, io.SeekCurrent)
		if err != nil {
			return nil, errutil.Err(err)
		}
		enc.streamStart = pos
		enc.canSeek = true
	}

	bw := bitio.NewWriter(w)
	if _, err := bw.Write(flacSignature); err != nil {
		return nil, errutil.Err(err)
	}
	// Encode metadata blocks.
	// TODO: consider using bufio.NewWriter.
	if err := encodeStreamInfo(bw, info, len(blocks) == 0); err != nil {
		return nil, errutil.Err(err)
	}
	for i, block := range blocks {
		if err := encodeBlock(bw, block, i == len(blocks)-1); err != nil {
			return nil, errutil.Err(err)
		}
	}
	// Flush pending writes of metadata blocks.
	if _, err := bw.Align(); err != nil {
		return nil, errutil.Err(err)
	}
	// Return encoder to be used for encoding audio samples.
	return enc, nil
}

// validateStreamInfo verifies the fields of the STREAMINFO block that frames
// written by this encoder must conform to.
func validateStreamInfo(info *meta.StreamInfo) error {
	if info.SampleRate == 0 || info.SampleRate > 655350 {
		return errutil.Newf("NewEncoder: invalid sample rate %d; expected 1..655350 Hz", info.SampleRate)
	}
	if info.NChannels < 1 || info.NChannels > 8 {
		return errutil.Newf("NewEncoder: invalid channel count %d; expected 1..8 channels", info.NChannels)
	}
	switch info.BitsPerSample {
	case 8, 16, 24:
	default:
		return errutil.Newf("NewEncoder: unsupported bits-per-sample %d; expected 8, 16 or 24", info.BitsPerSample)
	}
	// Block sizes of 0 mean "unknown" and are legal at creation time; any
	// explicit value must be within the legal 16..65535 range.
	if info.BlockSizeMin != 0 && info.BlockSizeMin < 16 {
		return errutil.Newf("NewEncoder: invalid minimum block size %d; expected >= 16", info.BlockSizeMin)
	}
	if info.BlockSizeMax != 0 && info.BlockSizeMax < 16 {
		return errutil.Newf("NewEncoder: invalid maximum block size %d; expected >= 16", info.BlockSizeMax)
	}
	if info.BlockSizeMin != 0 && info.BlockSizeMax != 0 && info.BlockSizeMin > info.BlockSizeMax {
		return errutil.Newf("NewEncoder: minimum block size (%d) exceeds maximum block size (%d)", info.BlockSizeMin, info.BlockSizeMax)
	}
	return nil
}

// Close flushes the encoder and, when the underlying writer implements
// io.WriteSeeker, back-fills this stream's STREAMINFO block with the total
// number of samples, the MD5 checksum of the unencoded audio samples, and the
// minimum and maximum frame and block sizes. The seek cursor is restored to
// the end of this stream's audio; bytes preceding the stream are left
// untouched.
//
// If the underlying writer implements io.Closer, it is closed at most once,
// including when finalization fails. Repeated calls to Close return the result
// of the first finalization.
func (enc *Encoder) Close() error {
	if enc.closed {
		return enc.closeErr
	}
	enc.closed = true

	// A poisoned encoder must not attempt any further writes, including the
	// STREAMINFO back-fill.
	if enc.err == nil && enc.canSeek {
		if err := enc.finalizeStreamInfo(); err != nil {
			enc.fail(err)
		}
	}

	enc.closeUnderlyingWriter()
	enc.closeErr = enc.err
	return enc.closeErr
}

// finalizeStreamInfo back-fills the STREAMINFO block of this FLAC stream and
// restores the seek cursor to the end of the stream's audio data.
func (enc *Encoder) finalizeStreamInfo() error {
	ws := enc.w.(io.WriteSeeker)

	// Record the end of this stream's audio so the cursor can be restored.
	audioEnd, err := ws.Seek(0, io.SeekCurrent)
	if err != nil {
		return errutil.Err(err)
	}

	// Seek to the STREAMINFO body, immediately after the "fLaC" signature of
	// this stream. The seek is relative to the stream origin, so any prefix in
	// the underlying writer is never modified.
	if _, err := ws.Seek(enc.streamStart+int64(len(flacSignature)), io.SeekStart); err != nil {
		return errutil.Err(err)
	}

	// Update minimum and maximum block size (in samples) of FLAC stream.
	enc.Info.BlockSizeMin = enc.blockSizeMin
	enc.Info.BlockSizeMax = enc.blockSizeMax
	// Update minimum and maximum frame size (in bytes) of FLAC stream.
	enc.Info.FrameSizeMin = enc.frameSizeMin
	enc.Info.FrameSizeMax = enc.frameSizeMax
	// Update total number of samples (per channel) of FLAC stream.
	enc.Info.NSamples = enc.nsamples
	// Update MD5 checksum of the unencoded audio samples.
	sum := enc.md5sum.Sum(nil)
	for i := range sum {
		enc.Info.MD5sum[i] = sum[i]
	}

	bw := bitio.NewWriter(ws)
	// Write updated StreamInfo metadata block to output stream.
	if err := encodeStreamInfo(bw, enc.Info, len(enc.Blocks) == 0); err != nil {
		return errutil.Err(err)
	}
	if _, err := bw.Align(); err != nil {
		return errutil.Err(err)
	}

	// Restore the cursor to the end of this stream's audio.
	if _, err := ws.Seek(audioEnd, io.SeekStart); err != nil {
		return errutil.Err(err)
	}
	return nil
}

// closeUnderlyingWriter closes the underlying writer once, if it implements
// io.Closer. The first error of the encoder is preserved.
func (enc *Encoder) closeUnderlyingWriter() {
	closer, ok := enc.w.(io.Closer)
	if !ok {
		return
	}
	if err := closer.Close(); err != nil && enc.err == nil {
		enc.fail(err)
	}
}

// fail records the first error encountered by the encoder. Once poisoned, the
// encoder performs no further writes to the underlying writer.
func (enc *Encoder) fail(err error) {
	if err == nil || enc.err != nil {
		return
	}
	enc.err = err
}

// EnablePredictionAnalysis specifies whether to enable analysis for the
// encoder. When analysis is enabled, subframes that are currently marked as
// PredVerbatim will be analyzed to use the best prediction method
// (constant, fixed or verbatim) based on size.
func (enc *Encoder) EnablePredictionAnalysis(enable bool) {
	enc.AnalysisEnabled = enable
}
