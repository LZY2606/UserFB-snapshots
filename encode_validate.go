package flac

import (
	"math"

	"github.com/mewkiz/flac/frame"
	iobits "github.com/mewkiz/flac/internal/bits"
	"github.com/mewkiz/pkg/errutil"
)

// This file contains the pre-flight validation and exact bit accounting run
// inside the single-frame workspace of WriteFrame. Nothing here touches the
// underlying writer, so a rejected frame leaves both the output stream and the
// caller's Frame untouched.

// validateFrame verifies that the frame header, channel layout and raw sample
// data are compatible with the encoder's STREAMINFO and internally consistent.
func (enc *Encoder) validateFrame(f *frame.Frame) error {
	nchannels := int(enc.Info.NChannels)
	if len(f.Subframes) == 0 {
		return errutil.Newf("frame contains no subframes")
	}
	if len(f.Subframes) != nchannels {
		return errutil.Newf("subframe and channel count mismatch; expected %d, got %d", nchannels, len(f.Subframes))
	}
	if f.Channels.Count() != nchannels {
		return errutil.Newf("channel count mismatch; expected %d, got %d", nchannels, f.Channels.Count())
	}
	if !validChannelAssignment(f.Channels) {
		return errutil.Newf("unsupported channel assignment %v", f.Channels)
	}

	bps := uint(enc.Info.BitsPerSample)
	switch f.BitsPerSample {
	case 0:
		// 0 means "get from STREAMINFO".
	case 8, 16, 24:
		if uint(f.BitsPerSample) != bps {
			return errutil.Newf("bits-per-sample mismatch; StreamInfo specifies %d, frame specifies %d", bps, f.BitsPerSample)
		}
	default:
		return errutil.Newf("unsupported bits-per-sample %d; expected 8, 16 or 24", f.BitsPerSample)
	}

	if f.SampleRate != 0 && f.SampleRate != enc.Info.SampleRate {
		return errutil.Newf("sample rate mismatch; StreamInfo specifies %d Hz, frame specifies %d Hz", enc.Info.SampleRate, f.SampleRate)
	}

	if f.BlockSize < 16 || f.BlockSize > 65535 {
		return errutil.Newf("invalid block size %d; expected 16..65535 samples", f.BlockSize)
	}

	for i, subframe := range f.Subframes {
		if subframe == nil {
			return errutil.Newf("subframe %d is nil", i)
		}
		if subframe.NSamples != int(f.BlockSize) {
			return errutil.Newf("subframe %d NSamples mismatch; expected %d, got %d", i, f.BlockSize, subframe.NSamples)
		}
		if len(subframe.Samples) != int(f.BlockSize) {
			return errutil.Newf("subframe %d sample count mismatch; expected %d, got %d", i, f.BlockSize, len(subframe.Samples))
		}
		subBPS := bps
		if isSideChannel(f.Channels, i) {
			subBPS++
		}
		if subframe.Wasted >= subBPS {
			return errutil.Newf("subframe %d has %d wasted bits but only %d bits-per-sample", i, subframe.Wasted, subBPS)
		}
		for j, sample := range subframe.Samples {
			if !sampleFits(sample, subBPS) {
				return errutil.Newf("subframe %d sample %d (%d) does not fit in %d bits", i, j, sample, subBPS)
			}
			// The wasted low bits must be zero so the production right/left
			// shift round-trip restores the original sample.
			if subframe.Wasted > 0 {
				mask := int32(1)<<subframe.Wasted - 1
				if sample&mask != 0 {
					return errutil.Newf("subframe %d sample %d has non-zero wasted low %d bits", i, j, subframe.Wasted)
				}
				if !sampleFits(sample>>subframe.Wasted, subBPS-subframe.Wasted) {
					return errutil.Newf("subframe %d sample %d does not fit in %d bits after removing %d wasted bits", i, j, subBPS-subframe.Wasted, subframe.Wasted)
				}
			}
		}
	}
	return nil
}

// validChannelAssignment reports whether the channel assignment is one of the
// assignments the frame header encoder supports.
func validChannelAssignment(channels frame.Channels) bool {
	switch channels {
	case frame.ChannelsMono, frame.ChannelsLR, frame.ChannelsLRC, frame.ChannelsLRLsRs,
		frame.ChannelsLRCLsRs, frame.ChannelsLRCLfeLsRs, frame.ChannelsLRCLfeCsSlSr,
		frame.ChannelsLRCLfeLsRsSlSr, frame.ChannelsLeftSide, frame.ChannelsSideRight,
		frame.ChannelsMidSide:
		return true
	}
	return false
}

// isSideChannel reports whether subframe channel carries a difference signal
// requiring one extra bit-per-sample.
func isSideChannel(channels frame.Channels, channel int) bool {
	switch channels {
	case frame.ChannelsSideRight:
		return channel == 0
	case frame.ChannelsLeftSide, frame.ChannelsMidSide:
		return channel == 1
	}
	return false
}

// sampleFits reports whether s is a valid signed bps-bit sample.
func sampleFits(s int32, bps uint) bool {
	limit := int64(1) << (bps - 1)
	v := int64(s)
	return v >= -limit && v <= limit-1
}

// validatePrediction verifies prediction metadata, Rice partition coverage and
// residual ranges after inter-channel decorrelation and prediction analysis.
// It returns the wide-integer residuals of each subframe, which are reused for
// exact encoded-size accounting.
func (enc *Encoder) validatePrediction(f *frame.Frame) (residuals [][]int64, err error) {
	residuals = make([][]int64, len(f.Subframes))
	for channel, subframe := range f.Subframes {
		subBPS := enc.subframeBPS(f, channel)

		// Decorrelation may widen the side channel; re-check sample ranges and
		// wasted-bit reversibility on the actual encoded samples.
		for j, sample := range subframe.Samples {
			if !sampleFits(sample, subBPS) {
				return nil, errutil.Newf("subframe %d sample %d (%d) does not fit in %d bits after inter-channel decorrelation", channel, j, sample, subBPS)
			}
			if subframe.Wasted > 0 {
				mask := int32(1)<<subframe.Wasted - 1
				if sample&mask != 0 {
					return nil, errutil.Newf("subframe %d sample %d has non-zero wasted low %d bits after decorrelation", channel, j, subframe.Wasted)
				}
			}
		}

		switch subframe.Pred {
		case frame.PredConstant, frame.PredVerbatim:
			// Constant and verbatim subframes carry no residual section. Any
			// stale Rice metadata left behind by prediction analysis is simply
			// ignored by the production encoder, so it is not an error here.
			if subframe.Pred == frame.PredConstant {
				first := subframe.Samples[0] >> subframe.Wasted
				for _, sample := range subframe.Samples[1:] {
					if sample>>subframe.Wasted != first {
						return nil, errutil.Newf("subframe %d marked constant contains differing samples", channel)
					}
				}
			}
			continue
		case frame.PredFixed:
			if subframe.Order < 0 || subframe.Order > 4 {
				return nil, errutil.Newf("subframe %d fixed predictor order %d out of range 0..4", channel, subframe.Order)
			}
			if subframe.Order > subframe.NSamples {
				return nil, errutil.Newf("subframe %d predictor order %d exceeds sample count %d", channel, subframe.Order, subframe.NSamples)
			}
		case frame.PredFIR:
			if subframe.Order < 1 || subframe.Order > 32 {
				return nil, errutil.Newf("subframe %d FIR predictor order %d out of range 1..32", channel, subframe.Order)
			}
			if subframe.Order > subframe.NSamples {
				return nil, errutil.Newf("subframe %d predictor order %d exceeds sample count %d", channel, subframe.Order, subframe.NSamples)
			}
			if len(subframe.Coeffs) != subframe.Order {
				return nil, errutil.Newf("subframe %d FIR coefficient count %d differs from predictor order %d", channel, len(subframe.Coeffs), subframe.Order)
			}
			if subframe.CoeffPrec < 1 || subframe.CoeffPrec > 15 {
				return nil, errutil.Newf("subframe %d FIR coefficient precision %d out of range 1..15 bits", channel, subframe.CoeffPrec)
			}
			// The coefficient shift is a signed 5-bit field; the production
			// residual computation rejects negative shifts, so the encodable
			// range is 0..15.
			if subframe.CoeffShift < 0 || subframe.CoeffShift > 15 {
				return nil, errutil.Newf("subframe %d FIR coefficient shift %d out of range 0..15", channel, subframe.CoeffShift)
			}
			for i, coeff := range subframe.Coeffs {
				if !sampleFits(coeff, subframe.CoeffPrec) {
					return nil, errutil.Newf("subframe %d coefficient %d (%d) does not fit in %d bits", channel, i, coeff, subframe.CoeffPrec)
				}
			}
		default:
			return nil, errutil.Newf("subframe %d uses unsupported prediction method %v", channel, subframe.Pred)
		}

		rice := subframe.RiceSubframe
		if rice == nil {
			return nil, errutil.Newf("subframe %d is missing Rice coding metadata", channel)
		}
		paramSize, err := residualParamSize(subframe.ResidualCodingMethod)
		if err != nil {
			return nil, errutil.Newf("subframe %d: %v", channel, err)
		}
		if rice.PartOrder < 0 || rice.PartOrder > 15 {
			return nil, errutil.Newf("subframe %d Rice partition order %d out of range 0..15", channel, rice.PartOrder)
		}
		nparts := 1 << rice.PartOrder
		if len(rice.Partitions) != nparts {
			return nil, errutil.Newf("subframe %d has %d Rice partitions for partition order %d; expected %d", channel, len(rice.Partitions), rice.PartOrder, nparts)
		}
		if subframe.NSamples%nparts != 0 {
			return nil, errutil.Newf("subframe %d block size %d is not divisible into %d Rice partitions", channel, subframe.NSamples, nparts)
		}

		// Compute residuals with 64-bit prediction arithmetic and verify that
		// every residual is representable as the signed 32-bit value the
		// production encoder emits. No truncation is allowed.
		coeffs := predictorCoeffs(subframe)
		res, err := lpcResidualsWide(subframe.Samples, subframe.Order, subframe.Wasted, coeffs, subframe.CoeffShift)
		if err != nil {
			return nil, errutil.Newf("subframe %d: %v", channel, err)
		}
		if len(res) != subframe.NSamples-subframe.Order {
			return nil, errutil.Newf("subframe %d produced %d residuals; expected %d", channel, len(res), subframe.NSamples-subframe.Order)
		}

		// Verify the partitions cover every residual exactly, with legal
		// parameters and escape widths.
		cur := 0
		for i, partition := range rice.Partitions {
			nsamples := partitionSampleCount(subframe, rice.PartOrder, i)
			if cur+nsamples > len(res) {
				return nil, errutil.Newf("subframe %d Rice partition %d overruns the residual count", channel, i)
			}
			escapeParam := uint(1)<<paramSize - 1
			if partition.Param > escapeParam {
				return nil, errutil.Newf("subframe %d partition %d Rice parameter %d exceeds %d-bit field", channel, i, partition.Param, paramSize)
			}
			if partition.Param == escapeParam {
				if partition.EscapedBitsPerSample > 32 {
					return nil, errutil.Newf("subframe %d escaped partition %d uses invalid width %d bits", channel, i, partition.EscapedBitsPerSample)
				}
				if partition.EscapedBitsPerSample == 0 && nsamples > 0 {
					return nil, errutil.Newf("subframe %d escaped partition %d declares 0 bits for %d residuals", channel, i, nsamples)
				}
				for j := 0; j < nsamples; j++ {
					if !sampleFits(int32(res[cur+j]), partition.EscapedBitsPerSample) {
						return nil, errutil.Newf("subframe %d escaped partition %d residual %d does not fit in %d bits", channel, i, j, partition.EscapedBitsPerSample)
					}
				}
			}
			cur += nsamples
		}
		if cur != len(res) {
			return nil, errutil.Newf("subframe %d Rice partitions cover %d of %d residuals", channel, cur, len(res))
		}
		residuals[channel] = res
	}
	return residuals, nil
}

// residualParamSize returns the width in bits of the Rice parameter field for
// the given residual coding method.
func residualParamSize(method frame.ResidualCodingMethod) (uint, error) {
	switch method {
	case frame.ResidualCodingMethodRice1:
		return 4, nil
	case frame.ResidualCodingMethodRice2:
		return 5, nil
	default:
		return 0, errutil.Newf("reserved residual coding method bit pattern (%02b)", uint8(method))
	}
}

// predictorCoeffs returns the LPC coefficients of the subframe's predictor.
func predictorCoeffs(subframe *frame.Subframe) []int32 {
	if subframe.Pred == frame.PredFixed {
		return frame.FixedCoeffs[subframe.Order]
	}
	return subframe.Coeffs
}

// partitionSampleCount mirrors the production partition sizing rules exactly.
func partitionSampleCount(subframe *frame.Subframe, partOrder, partition int) int {
	nparts := 1 << partOrder
	if partOrder == 0 {
		return subframe.NSamples - subframe.Order
	}
	if partition != 0 {
		return subframe.NSamples / nparts
	}
	return subframe.NSamples/nparts - subframe.Order
}

// lpcResidualsWide computes the LPC residuals using 64-bit arithmetic on the
// wasted-bit shifted samples, and rejects residuals that do not fit in a
// signed 32-bit integer.
func lpcResidualsWide(samples []int32, order int, wasted uint, coeffs []int32, shift int32) ([]int64, error) {
	residuals := make([]int64, 0, len(samples)-order)
	for i := order; i < len(samples); i++ {
		var sum int64
		for j, coeff := range coeffs {
			sum += int64(coeff) * int64(samples[i-j-1]>>wasted)
		}
		predicted := sum >> uint(shift)
		residual := int64(samples[i]>>wasted) - predicted
		if residual > math.MaxInt32 || residual < math.MinInt32 {
			return nil, errutil.Newf("residual at sample %d (%d) overflows signed 32 bits", i, residual)
		}
		residuals = append(residuals, residual)
	}
	return residuals, nil
}

// estimateSubframesBits computes the exact number of bits occupied by all
// subframes of the staged frame, including subframe headers, warm-up samples,
// Rice unary/binary codes, escape partitions and wasted-bit unary codes. The
// count is arithmetic: no unary code is generated while sizing the frame.
func (enc *Encoder) estimateSubframesBits(f *frame.Frame, residuals [][]int64) (int64, error) {
	var total int64
	for channel, subframe := range f.Subframes {
		bps := enc.subframeBPS(f, channel) - subframe.Wasted

		// Subframe header: 1 zero-padding bit, 6 prediction bits, 1 wasted
		// flag bit, plus the unary coded (Wasted-1) value.
		total += 1 + 6 + 1
		if subframe.Wasted > 0 {
			total += int64(subframe.Wasted) // unary of (Wasted-1): Wasted bits.
		}

		switch subframe.Pred {
		case frame.PredConstant:
			total += int64(bps)
			continue
		case frame.PredVerbatim:
			total += int64(subframe.NSamples) * int64(bps)
			continue
		case frame.PredFixed:
			// Order warm-up samples, then the residual section.
			total += int64(subframe.Order) * int64(bps)
		case frame.PredFIR:
			total += int64(subframe.Order) * int64(bps)
			// 4-bit (precision-1), 5-bit shift, one coefficient per order.
			total += 4 + 5 + int64(subframe.Order)*int64(subframe.CoeffPrec)
		}

		paramSize, err := residualParamSize(subframe.ResidualCodingMethod)
		if err != nil {
			return 0, errutil.Newf("subframe %d: %v", channel, err)
		}
		rice := subframe.RiceSubframe
		// 2-bit residual method, 4-bit partition order.
		total += 2 + 4

		res := residuals[channel]
		cur := 0
		for i, partition := range rice.Partitions {
			nsamples := partitionSampleCount(subframe, rice.PartOrder, i)
			total += int64(paramSize)
			escapeParam := uint(1)<<paramSize - 1
			if partition.Param == escapeParam {
				// 5-bit escape width followed by raw two's-complement samples.
				total += 5 + int64(nsamples)*int64(partition.EscapedBitsPerSample)
				cur += nsamples
				continue
			}
			k := partition.Param
			for j := 0; j < nsamples; j++ {
				// ZigZag folding of the validated 32-bit residual.
				folded := uint64(iobits.EncodeZigZag(int32(res[cur+j])))
				high := folded >> k
				// Unary code of `high` takes high+1 bits, followed by k low bits.
				total += int64(high) + 1 + int64(k)
			}
			cur += nsamples
		}
		if cur != len(res) {
			return 0, errutil.Newf("subframe %d size estimate covered %d of %d residuals", channel, cur, len(res))
		}
	}
	return total, nil
}
