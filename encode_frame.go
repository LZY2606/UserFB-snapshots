package flac

import (
	"bytes"
	"encoding/binary"
	"io"
	"math"

	"github.com/icza/bitio"
	"github.com/mewkiz/flac/frame"
	"github.com/mewkiz/flac/internal/hashutil/crc16"
	"github.com/mewkiz/flac/internal/hashutil/crc8"
	"github.com/mewkiz/flac/internal/utf8"
	"github.com/mewkiz/pkg/errutil"
)

// --- [ Frame ] ---------------------------------------------------------------

// WriteFrame encodes the given audio frame to the output stream.
//
// The frame is fully validated and encoded into a bounded, single-frame
// workspace before the underlying writer is touched: invalid metadata never
// produces a partial frame, never panics, and leaves the caller's Frame (its
// header, subframe metadata, nested partitions and samples) unmodified. The
// only caller-visible mutation on success is the automatic assignment of the
// frame Num field.
//
// State is committed only after the complete frame has been written to the
// underlying writer: frame numbering, block/frame size statistics and the
// running MD5 hash of the unencoded audio samples advance together. A short
// write or any other I/O error poisons the encoder; subsequent WriteFrame and
// Close calls return the first error without writing any more data. A frame
// rejected before the commit point may be fixed and retried, producing a
// byte-identical stream as if the failed attempt had never happened.
func (enc *Encoder) WriteFrame(f *frame.Frame) error {
	// A poisoned or closed encoder performs no further writes.
	if enc.err != nil {
		return enc.err
	}
	if enc.closed {
		return ErrEncoderClosed
	}
	if f == nil {
		return errutil.Newf("WriteFrame: nil frame")
	}

	// Validate and encode the complete frame into an in-memory workspace. The
	// working copy absorbs decorrelation, prediction analysis and wasted-bit
	// shifts, so the caller's frame is never mutated by a failed attempt.
	buf, num, nextNum, blockSize, err := enc.stageFrame(f)
	if err != nil {
		return errutil.Err(err)
	}

	// Commit point: write the complete frame in a single operation. Nothing in
	// the encoder state may advance until the whole frame is accepted.
	n, werr := enc.w.Write(buf)
	if werr == nil && n != len(buf) {
		// A writer must return an error on a short write; tolerate writers
		// that violate the io.Writer contract instead of silently truncating.
		werr = io.ErrShortWrite
	}
	if werr != nil {
		// The output may now be incomplete. Record the first I/O error and
		// leave numbering, statistics and MD5 exactly where they were.
		enc.fail(werr)
		return errutil.Err(werr)
	}

	// The complete frame is durable: commit all derived state atomically.
	f.Num = num
	enc.curNum = nextNum
	enc.nsamples += uint64(blockSize)
	if enc.blockSizeMin == 0 || blockSize < enc.blockSizeMin {
		enc.blockSizeMin = blockSize
	}
	if enc.blockSizeMax == 0 || blockSize > enc.blockSizeMax {
		enc.blockSizeMax = blockSize
	}
	frameSize := uint32(len(buf))
	if enc.frameSizeMin == 0 || frameSize < enc.frameSizeMin {
		enc.frameSizeMin = frameSize
	}
	if frameSize > enc.frameSizeMax {
		enc.frameSizeMax = frameSize
	}
	// Hash the caller's untouched, inter-channel correlated PCM samples using
	// the effective bits-per-sample, so a frame header that defers to
	// STREAMINFO (BitsPerSample == 0) still contributes to the MD5.
	enc.hashInterleavedPCM(f)
	return nil
}

// hashInterleavedPCM writes the unencoded, inter-channel correlated PCM of the
// frame to the running MD5 hash as little-endian samples, one subframe after
// another per sample index. For an explicit frame bits-per-sample this is
// byte-identical to frame.Frame.Hash; a zero value defers to STREAMINFO.
func (enc *Encoder) hashInterleavedPCM(f *frame.Frame) {
	bps := f.BitsPerSample
	if bps == 0 {
		bps = enc.Info.BitsPerSample
	}
	if len(f.Subframes) == 0 {
		return
	}
	var buf [3]byte
	nsamples := len(f.Subframes[0].Samples)
	h := enc.md5sum
	for i := 0; i < nsamples; i++ {
		for _, subframe := range f.Subframes {
			sample := subframe.Samples[i]
			switch {
			case bps <= 8:
				buf[0] = uint8(sample)
				h.Write(buf[:1])
			case bps <= 16:
				buf[0] = uint8(sample)
				buf[1] = uint8(sample >> 8)
				h.Write(buf[:2])
			default:
				buf[0] = uint8(sample)
				buf[1] = uint8(sample >> 8)
				buf[2] = uint8(sample >> 16)
				h.Write(buf[:])
			}
		}
	}
}

// stageFrame validates and encodes a single frame into an in-memory buffer,
// returning the frame bytes together with the assigned frame number, the next
// frame number and the block size. The caller's frame is not modified.
//
// All failures (including panics caused by malformed metadata) are returned as
// errors before any data reaches the underlying writer.
func (enc *Encoder) stageFrame(f *frame.Frame) (buf []byte, num, nextNum uint64, blockSize uint16, err error) {
	defer func() {
		if r := recover(); r != nil {
			buf = nil
			err = errutil.Newf("internal encoder error while staging frame: %v", r)
		}
	}()

	// Work on a private copy so analysis and decorrelation cannot dirty the
	// caller's frame, even on an error mid-way.
	work := cloneFrame(f)

	if err = enc.validateFrame(work); err != nil {
		return nil, 0, 0, 0, err
	}

	num = enc.curNum
	blockSize = work.Header.BlockSize
	if work.HasFixedBlockSize {
		if num > (1<<31)-1 {
			return nil, 0, 0, 0, errutil.Newf("frame number %d exceeds the 31-bit fixed-blocksize limit", num)
		}
		nextNum = num + 1
	} else {
		if num > (1<<36)-1 {
			return nil, 0, 0, 0, errutil.Newf("sample number %d exceeds the 36-bit variable-blocksize limit", num)
		}
		nextNum = num + uint64(blockSize)
	}
	work.Header.Num = num

	// Inter-channel decorrelation of the working copy only.
	work.Decorrelate()

	// Optional prediction analysis, using the production implementation.
	for channel, subframe := range work.Subframes {
		if !enc.AnalysisEnabled {
			break
		}
		if subframe.Pred == frame.PredVerbatim {
			analyzeSubframe(subframe, enc.subframeBPS(work, channel))
		}
	}

	// Re-validate metadata after analysis, and compute the exact encoded bit
	// count arithmetically. This rejects over-long Rice unary runs before a
	// single unary code is generated.
	residuals, err := enc.validatePrediction(work)
	if err != nil {
		return nil, 0, 0, 0, err
	}

	var workspace bytes.Buffer
	// Create a new CRC-16 hash writer which adds the data from all write
	// operations to a running hash.
	h := crc16.NewIBM()
	hw := io.MultiWriter(h, &workspace)

	// Encode frame header.
	if err = enc.encodeFrameHeader(hw, work.Header); err != nil {
		return nil, 0, 0, 0, err
	}
	headerBytes := workspace.Len()

	subframeBits, err := enc.estimateSubframesBits(work, residuals)
	if err != nil {
		return nil, 0, 0, 0, err
	}
	totalBits := int64(headerBytes)*8 + subframeBits
	frameBytes := (totalBits+7)/8 + 2 // byte alignment plus CRC-16.
	if frameBytes > maxFrameSize {
		return nil, 0, 0, 0, ErrFrameTooLarge
	}

	// Encode subframes using the unchanged production routines.
	bw := bitio.NewWriter(hw)
	for channel, subframe := range work.Subframes {
		if err = encodeSubframe(bw, work.Header, subframe, enc.subframeBPS(work, channel)); err != nil {
			return nil, 0, 0, 0, err
		}
	}

	// Zero-padding to byte alignment. Flush pending writes to subframes.
	if _, err = bw.Align(); err != nil {
		return nil, 0, 0, 0, err
	}

	// CRC-16 of everything before the crc, back to and including the frame
	// header sync code.
	crc := h.Sum16()
	if err = binary.Write(&workspace, binary.BigEndian, crc); err != nil {
		return nil, 0, 0, 0, err
	}

	// The arithmetic estimate and the real encoder must agree exactly.
	if int64(workspace.Len()) != frameBytes {
		return nil, 0, 0, 0, errutil.Newf("frame size estimate mismatch; estimated %d bytes, encoded %d bytes", frameBytes, workspace.Len())
	}
	return workspace.Bytes(), num, nextNum, blockSize, nil
}

// subframeBPS returns the bits-per-sample used for the given subframe,
// accounting for the extra bit required by inter-channel side channels.
func (enc *Encoder) subframeBPS(f *frame.Frame, channel int) uint {
	bps := uint(f.BitsPerSample)
	if bps == 0 {
		bps = uint(enc.Info.BitsPerSample)
	}
	switch f.Channels {
	case frame.ChannelsSideRight:
		// channel 0 is the side channel.
		if channel == 0 {
			bps++
		}
	case frame.ChannelsLeftSide, frame.ChannelsMidSide:
		// channel 1 is the side channel.
		if channel == 1 {
			bps++
		}
	}
	return bps
}

// cloneFrame returns a deep copy of the frame structure and its sample slices.
// Read-only metadata such as coefficient and partition slices is shared, since
// encoding never mutates them.
func cloneFrame(f *frame.Frame) *frame.Frame {
	c := &frame.Frame{
		Header:    f.Header,
		Subframes: make([]*frame.Subframe, len(f.Subframes)),
	}
	for i, sf := range f.Subframes {
		if sf == nil {
			c.Subframes[i] = nil
			continue
		}
		cp := *sf // value copy of SubHeader, including slice headers.
		cp.Samples = append([]int32(nil), sf.Samples...)
		c.Subframes[i] = &cp
	}
	return c
}

// --- [ Frame header ] --------------------------------------------------------

// encodeFrameHeader encodes the given frame header, writing to w.
func (enc *Encoder) encodeFrameHeader(w io.Writer, hdr frame.Header) error {
	// Create a new CRC-8 hash writer which adds the data from all write
	// operations to a running hash.
	h := crc8.NewATM()
	hw := io.MultiWriter(h, w)
	bw := bitio.NewWriter(hw)

	// Closing the *bitio.Writer will not close the underlying writer
	defer bw.Close()

	//  Sync code: 11111111111110
	if err := bw.WriteBits(0x3FFE, 14); err != nil {
		return errutil.Err(err)
	}

	// Reserved: 0
	if err := bw.WriteBits(0x0, 1); err != nil {
		return errutil.Err(err)
	}

	// Blocking strategy:
	//    0 : fixed-blocksize stream; frame header encodes the frame number
	//    1 : variable-blocksize stream; frame header encodes the sample number
	if err := bw.WriteBool(!hdr.HasFixedBlockSize); err != nil {
		return errutil.Err(err)
	}

	// Encode block size.
	nblockSizeSuffixBits, err := encodeFrameHeaderBlockSize(bw, hdr.BlockSize)
	if err != nil {
		return errutil.Err(err)
	}

	// Encode sample rate.
	sampleRateSuffixBits, nsampleRateSuffixBits, err := encodeFrameHeaderSampleRate(bw, hdr.SampleRate)
	if err != nil {
		return errutil.Err(err)
	}

	// Encode channels assignment.
	if err := encodeFrameHeaderChannels(bw, hdr.Channels); err != nil {
		return errutil.Err(err)
	}

	// Encode bits-per-sample.
	if err := encodeFrameHeaderBitsPerSample(bw, hdr.BitsPerSample); err != nil {
		return errutil.Err(err)
	}

	// Reserved: 0
	if err := bw.WriteBits(0x0, 1); err != nil {
		return errutil.Err(err)
	}

	//    if (variable blocksize)
	//       <8-56>:"UTF-8" coded sample number (decoded number is 36 bits)
	//    else
	//       <8-48>:"UTF-8" coded frame number (decoded number is 31 bits)
	if err := utf8.Encode(bw, hdr.Num); err != nil {
		return errutil.Err(err)
	}

	// Write block size after the frame header (used for uncommon block sizes).
	if nblockSizeSuffixBits > 0 {
		// 0110 : get 8 bit (blocksize-1) from end of header
		// 0111 : get 16 bit (blocksize-1) from end of header
		if err := bw.WriteBits(uint64(hdr.BlockSize-1), nblockSizeSuffixBits); err != nil {
			return errutil.Err(err)
		}
	}

	// Write sample rate after the frame header (used for uncommon sample rates).
	if nsampleRateSuffixBits > 0 {
		if err := bw.WriteBits(sampleRateSuffixBits, nsampleRateSuffixBits); err != nil {
			return errutil.Err(err)
		}
	}

	// Flush pending writes to frame header.
	if _, err := bw.Align(); err != nil {
		return errutil.Err(err)
	}

	// CRC-8 (polynomial = x^8 + x^2 + x^1 + x^0, initialized with 0) of
	// everything before the crc, including the sync code.
	crc := h.Sum8()
	if err := binary.Write(w, binary.BigEndian, crc); err != nil {
		return errutil.Err(err)
	}

	return nil
}

// ~~~ [ Block size ] ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

// encodeFrameHeaderBlockSize encodes the block size of the frame header,
// writing to bw. It returns the number of bits used to store block size after
// the frame header.
func encodeFrameHeaderBlockSize(bw *bitio.Writer, blockSize uint16) (nblockSizeSuffixBits byte, err error) {
	// Block size in inter-channel samples:
	//    0000 : reserved
	//    0001 : 192 samples
	//    0010-0101 : 576 * (2^(n-2)) samples, i.e. 576/1152/2304/4608
	//    0110 : get 8 bit (blocksize-1) from end of header
	//    0111 : get 16 bit (blocksize-1) from end of header
	//    1000-1111 : 256 * (2^(n-8)) samples, i.e. 256/512/1024/2048/4096/8192/16384/32768
	var bits uint64
	switch blockSize {
	case 192:
		// 0001
		bits = 0x1
	case 576, 1152, 2304, 4608:
		// 0010-0101 : 576 * (2^(n-2)) samples, i.e. 576/1152/2304/4608
		bits = 0x2 + uint64(math.Log2(float64(blockSize/576)))
	case 256, 512, 1024, 2048, 4096, 8192, 16384, 32768:
		// 1000-1111 : 256 * (2^(n-8)) samples, i.e. 256/512/1024/2048/4096/8192/16384/32768
		bits = 0x8 + uint64(math.Log2(float64(blockSize/256)))
	default:
		if blockSize <= 256 {
			// 0110 : get 8 bit (blocksize-1) from end of header
			bits = 0x6
			nblockSizeSuffixBits = 8
		} else {
			// 0111 : get 16 bit (blocksize-1) from end of header
			bits = 0x7
			nblockSizeSuffixBits = 16
		}
	}
	if err := bw.WriteBits(bits, 4); err != nil {
		return 0, errutil.Err(err)
	}
	return nblockSizeSuffixBits, nil
}

// ~~~ [ Sample rate ] ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

// encodeFrameHeaderSampleRate encodes the sample rate of the frame header,
// writing to bw. It returns the bits and the number of bits used to store
// sample rate after the frame header.
func encodeFrameHeaderSampleRate(bw *bitio.Writer, sampleRate uint32) (sampleRateSuffixBits uint64, nsampleRateSuffixBits byte, err error) {
	// Sample rate:
	//    0000 : get from STREAMINFO metadata block
	//    0001 : 88.2kHz
	//    0010 : 176.4kHz
	//    0011 : 192kHz
	//    0100 : 8kHz
	//    0101 : 16kHz
	//    0110 : 22.05kHz
	//    0111 : 24kHz
	//    1000 : 32kHz
	//    1001 : 44.1kHz
	//    1010 : 48kHz
	//    1011 : 96kHz
	//    1100 : get 8 bit sample rate (in kHz) from end of header
	//    1101 : get 16 bit sample rate (in Hz) from end of header
	//    1110 : get 16 bit sample rate (in tens of Hz) from end of header
	//    1111 : invalid, to prevent sync-fooling string of 1s
	var bits uint64
	switch sampleRate {
	case 0:
		// 0000 : get from STREAMINFO metadata block
		bits = 0
	case 88200:
		// 0001 : 88.2kHz
		bits = 0x1
	case 176400:
		// 0010 : 176.4kHz
		bits = 0x2
	case 192000:
		// 0011 : 192kHz
		bits = 0x3
	case 8000:
		// 0100 : 8kHz
		bits = 0x4
	case 16000:
		// 0101 : 16kHz
		bits = 0x5
	case 22050:
		// 0110 : 22.05kHz
		bits = 0x6
	case 24000:
		// 0111 : 24kHz
		bits = 0x7
	case 32000:
		// 1000 : 32kHz
		bits = 0x8
	case 44100:
		// 1001 : 44.1kHz
		bits = 0x9
	case 48000:
		// 1010 : 48kHz
		bits = 0xA
	case 96000:
		// 1011 : 96kHz
		bits = 0xB
	default:
		switch {
		case sampleRate <= 255000 && sampleRate%1000 == 0:
			// 1100 : get 8 bit sample rate (in kHz) from end of header
			bits = 0xC
			sampleRateSuffixBits = uint64(sampleRate / 1000)
			nsampleRateSuffixBits = 8
		case sampleRate <= 65535:
			// 1101 : get 16 bit sample rate (in Hz) from end of header
			bits = 0xD
			sampleRateSuffixBits = uint64(sampleRate)
			nsampleRateSuffixBits = 16
		case sampleRate <= 655350 && sampleRate%10 == 0:
			// 1110 : get 16 bit sample rate (in tens of Hz) from end of header
			bits = 0xE
			sampleRateSuffixBits = uint64(sampleRate / 10)
			nsampleRateSuffixBits = 16
		default:
			return 0, 0, errutil.Newf("unable to encode sample rate %v", sampleRate)
		}
	}
	if err := bw.WriteBits(bits, 4); err != nil {
		return 0, 0, errutil.Err(err)
	}
	return sampleRateSuffixBits, nsampleRateSuffixBits, nil
}

// ~~~ [ Channels assignment ] ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

// encodeFrameHeaderChannels encodes the channels assignment of the frame
// header, writing to bw.
func encodeFrameHeaderChannels(bw *bitio.Writer, channels frame.Channels) error {
	// Channel assignment.
	//    0000-0111 : (number of independent channels)-1. Where defined, the channel order follows SMPTE/ITU-R recommendations. The assignments are as follows:
	//        1 channel: mono
	//        2 channels: left, right
	//        3 channels: left, right, center
	//        4 channels: front left, front right, back left, back right
	//        5 channels: front left, front right, front center, back/surround left, back/surround right
	//        6 channels: front left, front right, front center, LFE, back/surround left, back/surround right
	//        7 channels: front left, front right, front center, LFE, back center, side left, side right
	//        8 channels: front left, front right, front center, LFE, back left, back right, side left, side right
	//    1000 : left/side stereo: channel 0 is the left channel, channel 1 is the side(difference) channel
	//    1001 : right/side stereo: channel 0 is the side(difference) channel, channel 1 is the right channel
	//    1010 : mid/side stereo: channel 0 is the mid(average) channel, channel 1 is the side(difference) channel
	//    1011-1111 : reserved
	var bits uint64
	switch channels {
	case frame.ChannelsMono, frame.ChannelsLR, frame.ChannelsLRC, frame.ChannelsLRLsRs, frame.ChannelsLRCLsRs, frame.ChannelsLRCLfeLsRs, frame.ChannelsLRCLfeCsSlSr, frame.ChannelsLRCLfeLsRsSlSr:
		// 1 channel: mono.
		// 2 channels: left, right.
		// 3 channels: left, right, center.
		// 4 channels: left, right, left surround, right surround.
		// 5 channels: left, right, center, left surround, right surround.
		// 6 channels: left, right, center, LFE, left surround, right surround.
		// 7 channels: left, right, center, LFE, center surround, side left, side right.
		// 8 channels: left, right, center, LFE, left surround, right surround, side left, side right.
		bits = uint64(channels.Count() - 1)
	case frame.ChannelsLeftSide:
		// 2 channels: left, side; using inter-channel decorrelation.
		// 1000 : left/side stereo: channel 0 is the left channel, channel 1 is the side(difference) channel
		bits = 0x8
	case frame.ChannelsSideRight:
		// 2 channels: side, right; using inter-channel decorrelation.
		// 1001 : right/side stereo: channel 0 is the side(difference) channel, channel 1 is the right channel
		bits = 0x9
	case frame.ChannelsMidSide:
		// 2 channels: mid, side; using inter-channel decorrelation.
		// 1010 : mid/side stereo: channel 0 is the mid(average) channel, channel 1 is the side(difference) channel
		bits = 0xA
	default:
		return errutil.Newf("support for channel assignment %v not yet implemented", channels)
	}
	if err := bw.WriteBits(bits, 4); err != nil {
		return errutil.Err(err)
	}
	return nil
}

// ~~~ [ Bits-per-sample ] ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

// encodeFrameHeaderBitsPerSample encodes the bits-per-sample of the frame
// header, writing to bw.
func encodeFrameHeaderBitsPerSample(bw *bitio.Writer, bps uint8) error {
	// Sample size in bits:
	//    000 : get from STREAMINFO metadata block
	//    001 : 8 bits per sample
	//    010 : 12 bits per sample
	//    011 : reserved
	//    100 : 16 bits per sample
	//    101 : 20 bits per sample
	//    110 : 24 bits per sample
	//    111 : reserved
	var bits uint64
	switch bps {
	case 0:
		// 000 : get from STREAMINFO metadata block
		bits = 0x0
	case 8:
		// 001 : 8 bits per sample
		bits = 0x1
	case 12:
		// 010 : 12 bits per sample
		bits = 0x2
	case 16:
		// 100 : 16 bits per sample
		bits = 0x4
	case 20:
		// 101 : 20 bits per sample
		bits = 0x5
	case 24:
		// 110 : 24 bits per sample
		bits = 0x6
	default:
		return errutil.Newf("support for sample size %v not yet implemented", bps)
	}
	if err := bw.WriteBits(bits, 3); err != nil {
		return errutil.Err(err)
	}
	return nil
}
