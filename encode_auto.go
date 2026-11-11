package flac

import (
	"github.com/icza/bitio"
	"github.com/mewkiz/flac/frame"
	"github.com/mewkiz/pkg/errutil"
)

// Auto compression selects, per frame, the channel representation and the
// per-subframe prediction method and Rice partition layout that minimize the
// number of bits actually written. It is opt-in through
// Encoder.EnableAutoCompression and only engages with frames whose subframes
// are all stored verbatim without wasted bits.
//
// The implemented search space is intentionally narrow:
//   - mono, or stereo represented as independent (L,R), left/side, side/right
//     or mid/side;
//   - constant, verbatim or fixed prediction of order 0 through 4;
//   - Rice1 residuals with partition order 0 through 8 and Rice parameter
//     k in 0 through 14, evaluated independently per partition.
//
// Rice2, escape codes, LPC prediction and wasted-bit search are not used.

// maxRiceParameter is the largest Rice1 parameter tried per partition. The
// value 15 (0xF) is the Rice1 escape code and is never emitted.
const maxRiceParameter = 14

// maxPartitionOrder is the largest partition order tried.
const maxPartitionOrder = 8

// autoSubframePlan describes the selected encoding of one subframe.
type autoSubframePlan struct {
	// Prediction method of the subframe.
	pred frame.Pred
	// Fixed prediction order; only used when pred is frame.PredFixed.
	order int
	// Container bit width of the subframe (the frame header still declares the
	// original audio bit depth; side channels use one extra bit).
	bps uint8
	// Samples in the selected channel representation; never aliases the
	// caller's PCM.
	samples []int32
	// Fixed prediction residuals; only used when pred is frame.PredFixed.
	residuals []int32
	// Rice partition order and per-partition Rice parameters.
	partOrder uint8
	ks        []uint8
	// Total effective size of the subframe in bits, including the subframe
	// header, warm-up samples, residual method and partition fields and every
	// partition parameter.
	bits uint64
}

// autoPlan describes the selected encoding of a whole frame.
type autoPlan struct {
	// Selected channel assignment.
	channels frame.Channels
	// Selected encoding of each subframe, in stream order.
	subs []autoSubframePlan
	// Total effective size of all subframes in bits.
	bits uint64
}

// autoEligible reports whether the frame may be analyzed automatically. Frames
// in which any subframe explicitly selects a prediction method other than
// verbatim (or uses wasted bits) keep going through the original write path
// unchanged.
func autoEligible(f *frame.Frame) bool {
	for _, subframe := range f.Subframes {
		if subframe.Pred != frame.PredVerbatim || subframe.Wasted != 0 {
			return false
		}
	}
	return true
}

// signedRange returns the inclusive signed integer range of a bps-bit sample.
func signedRange(bps uint8) (lo, hi int32) {
	hi = int32(1<<(bps-1)) - 1
	lo = -hi - 1
	return lo, hi
}

// inRange reports whether every sample fits in a signed bps-bit container.
func inRange(samples []int32, bps uint8) bool {
	lo, hi := signedRange(bps)
	for _, s := range samples {
		if s < lo || s > hi {
			return false
		}
	}
	return true
}

// validateAutoFrame verifies the input range documented on
// EnableAutoCompression. It runs before any byte of the frame header is
// written.
func validateAutoFrame(f *frame.Frame) error {
	bps := f.Header.BitsPerSample
	switch bps {
	case 8, 16, 24:
	default:
		return errutil.Newf("auto compression supports 8, 16 or 24 bits-per-sample; got %d", bps)
	}

	nchannels := len(f.Subframes)
	switch f.Channels {
	case frame.ChannelsMono:
		if nchannels != 1 {
			return errutil.Newf("auto compression mono frame requires 1 subframe; got %d", nchannels)
		}
	case frame.ChannelsLR:
		if nchannels != 2 {
			return errutil.Newf("auto compression stereo frame requires 2 subframes; got %d", nchannels)
		}
	default:
		return errutil.Newf("auto compression supports mono or left/right channel assignment; got %v", f.Channels)
	}

	nsamples := f.Subframes[0].NSamples
	if int(f.Header.BlockSize) != nsamples {
		return errutil.Newf("auto compression block size mismatch; header declares %d, subframes hold %d samples", f.Header.BlockSize, nsamples)
	}

	lo, hi := signedRange(bps)
	for i, subframe := range f.Subframes {
		if subframe.NSamples != len(subframe.Samples) {
			return errutil.Newf("auto compression subframe %d has NSamples %d but %d samples", i, subframe.NSamples, len(subframe.Samples))
		}
		for j, sample := range subframe.Samples {
			if sample < lo || sample > hi {
				return errutil.Newf("auto compression sample %d of subframe %d (%d) exceeds declared %d-bit range [%d, %d]", j, i, sample, bps, lo, hi)
			}
		}
	}
	return nil
}

// analyzeAutoFrame enumerates every legal channel representation and, for each
// subframe, every legal prediction method and Rice partition layout, and
// returns the plan with the smallest total effective bit count. Ties are
// resolved by enumeration order: channel representation, then prediction
// method (constant, verbatim, fixed orders 0 through 4), then partition order
// and Rice parameter, so repeated encoding of the same frame is stable.
func analyzeAutoFrame(f *frame.Frame) (*autoPlan, error) {
	if err := validateAutoFrame(f); err != nil {
		return nil, err
	}
	bps := f.Header.BitsPerSample
	n := f.Subframes[0].NSamples

	var best *autoPlan
	consider := func(channels frame.Channels, containers [][]int32, widths []uint8) error {
		for i, samples := range containers {
			if !inRange(samples, widths[i]) {
				return errutil.Newf("auto compression cannot represent subframe %d of %v in %d bits", i, channels, widths[i])
			}
		}
		plan := &autoPlan{channels: channels, subs: make([]autoSubframePlan, len(containers))}
		for i, samples := range containers {
			plan.subs[i] = analyzeSubframe(samples, widths[i])
			plan.bits += plan.subs[i].bits
		}
		if best == nil || plan.bits < best.bits {
			best = plan
		}
		return nil
	}

	if f.Channels == frame.ChannelsMono {
		// Mono frames only analyze their own samples.
		if err := consider(frame.ChannelsMono, [][]int32{f.Subframes[0].Samples}, []uint8{bps}); err != nil {
			return nil, err
		}
		return best, nil
	}

	left := f.Subframes[0].Samples
	right := f.Subframes[1].Samples

	// 1. Independent channels: left and right.
	if err := consider(frame.ChannelsLR, [][]int32{left, right}, []uint8{bps, bps}); err != nil {
		return nil, err
	}

	// Side and mid are computed with wide integers first.
	side := make([]int32, n)
	mid := make([]int32, n)
	for i := 0; i < n; i++ {
		l := int64(left[i])
		r := int64(right[i])
		// side = left - right; the side container is one bit wider than the
		// declared audio bit depth.
		side64 := l - r
		// mid is the arithmetic (signed) right shift of the average. Truncating
		// a negative odd sum toward zero would be wrong: the decoder reconstructs
		// the discarded least significant bit from the side parity.
		mid64 := (l + r) >> 1
		if side64 < mathMinInt32 || side64 > mathMaxInt32 || mid64 < mathMinInt32 || mid64 > mathMaxInt32 {
			return nil, errutil.Newf("auto compression inter-channel transform overflows at sample %d", i)
		}
		side[i] = int32(side64)
		mid[i] = int32(mid64)
	}
	sideBPS := bps + 1

	// 2. Left/side stereo.
	if err := consider(frame.ChannelsLeftSide, [][]int32{left, side}, []uint8{bps, sideBPS}); err != nil {
		return nil, err
	}
	// 3. Side/right stereo.
	if err := consider(frame.ChannelsSideRight, [][]int32{side, right}, []uint8{sideBPS, bps}); err != nil {
		return nil, err
	}
	// 4. Mid/side stereo.
	if err := consider(frame.ChannelsMidSide, [][]int32{mid, side}, []uint8{bps, sideBPS}); err != nil {
		return nil, err
	}

	return best, nil
}

// Integer bounds used by the wide-integer transform checks.
const (
	mathMaxInt32 = int64(1<<31 - 1)
	mathMinInt32 = -int64(1 << 31)
)

// analyzeSubframe selects the best prediction method and Rice partition layout
// for one subframe. Ties prefer constant, then verbatim, then lower fixed
// orders, matching the documented stability rules.
func analyzeSubframe(samples []int32, bps uint8) autoSubframePlan {
	n := len(samples)

	// Constant prediction: a single unencoded sample replicated blockSize times.
	constant := autoSubframePlan{pred: frame.PredConstant, bps: bps, samples: samples, bits: subframeHeaderBits + uint64(bps)}
	allEqual := true
	for _, s := range samples[1:] {
		if s != samples[0] {
			allEqual = false
			break
		}
	}
	best := constant
	if !allEqual {
		// Constant is not representable; mark it unavailable by giving verbatim
		// the first viable slot below.
		best = autoSubframePlan{bits: ^uint64(0)}
	}

	// Verbatim prediction.
	verbatim := autoSubframePlan{pred: frame.PredVerbatim, bps: bps, samples: samples, bits: subframeHeaderBits + uint64(n)*uint64(bps)}
	if verbatim.bits < best.bits {
		best = verbatim
	}

	// Fixed prediction, orders 0 through 4.
	for order := 0; order <= 4; order++ {
		// The first partition must still hold residuals once the warm-up samples
		// are removed.
		if n <= order {
			continue
		}
		residuals, ok := fixedResiduals(samples, order)
		if !ok {
			continue
		}
		riceBits, partOrder, ks := bestRiceLayout(n, order, residuals)
		plan := autoSubframePlan{
			pred:      frame.PredFixed,
			order:     order,
			bps:       bps,
			samples:   samples,
			residuals: residuals,
			partOrder: partOrder,
			ks:        ks,
			bits:      subframeHeaderBits + uint64(order)*uint64(bps) + riceBits,
		}
		if plan.bits < best.bits {
			best = plan
		}
	}
	return best
}

// subframeHeaderBits is the size of the subframe header without wasted bits:
// one zero padding bit, six prediction bits and one wasted-bits flag.
const subframeHeaderBits = 8

// fixedResiduals computes the fixed-prediction residuals in wide integers and
// reports whether the candidate is representable. Residuals are folded onto
// uint32 by the Rice coder, so a residual outside the int32 range rejects the
// candidate rather than truncating silently.
func fixedResiduals(samples []int32, order int) (residuals []int32, ok bool) {
	n := len(samples)
	residuals = make([]int32, n-order)
	for i := order; i < n; i++ {
		var r int64
		s := func(j int) int64 { return int64(samples[j]) }
		switch order {
		case 0:
			r = s(i)
		case 1:
			// x[n] - x[n-1]
			r = s(i) - s(i-1)
		case 2:
			// x[n] - 2*x[n-1] + x[n-2]
			r = s(i) - 2*s(i-1) + s(i-2)
		case 3:
			// x[n] - 3*x[n-1] + 3*x[n-2] - x[n-3]
			r = s(i) - 3*s(i-1) + 3*s(i-2) - s(i-3)
		case 4:
			// x[n] - 4*x[n-1] + 6*x[n-2] - 4*x[n-3] + x[n-4]
			r = s(i) - 4*s(i-1) + 6*s(i-2) - 4*s(i-3) + s(i-4)
		}
		if r < mathMinInt32 || r > mathMaxInt32 {
			return nil, false
		}
		residuals[i-order] = int32(r)
	}
	return residuals, true
}

// partitionCounts returns the number of residuals in each partition for the
// given block size, fixed order and partition order.
func partitionCounts(n, order, partOrder int) (counts []int, ok bool) {
	nparts := 1 << partOrder
	if n%nparts != 0 {
		return nil, false
	}
	counts = make([]int, nparts)
	for i := range counts {
		counts[i] = n / nparts
	}
	// The first partition additionally loses the warm-up samples.
	counts[0] -= order
	if counts[0] <= 0 {
		return nil, false
	}
	return counts, true
}

// riceResidualBits returns the Rice1 code length of one residual: a unary
// prefix of (q+1) bits followed by k remainder bits, where q is the folded
// residual shifted right by k.
func riceResidualBits(r int32, k uint8) uint64 {
	u := foldSigned(r)
	return uint64(u>>k) + 1 + uint64(k)
}

// foldSigned maps a signed residual onto an unsigned value using the FLAC
// signed folding (the inverse of internal/bits.ZigZag).
func foldSigned(r int32) uint32 {
	return uint32(r<<1) ^ uint32(r>>31)
}

// bestRiceLayout evaluates partition orders 0 through 8 for a residual stream
// and returns the cheapest layout. Lower partition orders and lower Rice
// parameters win ties.
func bestRiceLayout(n, order int, residuals []int32) (bits uint64, partOrder uint8, ks []uint8) {
	bestBits := ^uint64(0)
	for p := 0; p <= maxPartitionOrder; p++ {
		counts, ok := partitionCounts(n, order, p)
		if !ok {
			continue
		}
		layoutBits := uint64(2 + 4) // residual coding method (Rice1) and partition order.
		layoutKS := make([]uint8, len(counts))
		offset := 0
		for part, count := range counts {
			partitionBits := ^uint64(0)
			var bestK uint8
			for k := uint8(0); k <= maxRiceParameter; k++ {
				var b uint64 = 4 // four-bit Rice parameter.
				for _, r := range residuals[offset : offset+count] {
					b += riceResidualBits(r, k)
				}
				if b < partitionBits {
					partitionBits = b
					bestK = k
				}
			}
			layoutKS[part] = bestK
			layoutBits += partitionBits
			offset += count
		}
		if layoutBits < bestBits {
			bestBits = layoutBits
			partOrder = uint8(p)
			ks = layoutKS
		}
	}
	return bestBits, partOrder, ks
}

// encodeAutoSubframe writes one subframe described by an auto compression plan.
func encodeAutoSubframe(bw *bitio.Writer, plan *autoSubframePlan) error {
	if err := encodeSubframeHeader(bw, frame.SubHeader{Pred: plan.pred, Order: plan.order, Wasted: 0}); err != nil {
		return errutil.Err(err)
	}
	switch plan.pred {
	case frame.PredConstant:
		if err := bw.WriteBits(uint64(plan.samples[0]), plan.bps); err != nil {
			return errutil.Err(err)
		}
	case frame.PredVerbatim:
		for _, sample := range plan.samples {
			if err := bw.WriteBits(uint64(sample), plan.bps); err != nil {
				return errutil.Err(err)
			}
		}
	case frame.PredFixed:
		if err := encodeFixedSamples(bw, plan); err != nil {
			return err
		}
	default:
		return errutil.Newf("support for prediction method %v not yet implemented in auto compression", plan.pred)
	}
	return nil
}

// encodeFixedSamples writes the warm-up samples and Rice1 coded residuals of a
// fixed-prediction subframe.
func encodeFixedSamples(bw *bitio.Writer, plan *autoSubframePlan) error {
	// Unencoded warm-up samples, one per prediction coefficient.
	for i := 0; i < plan.order; i++ {
		if err := bw.WriteBits(uint64(plan.samples[i]), plan.bps); err != nil {
			return errutil.Err(err)
		}
	}

	// 2 bits: residual coding method 00 (Rice coding with a 4-bit parameter).
	if err := bw.WriteBits(0x0, 2); err != nil {
		return errutil.Err(err)
	}
	// 4 bits: partition order.
	if err := bw.WriteBits(uint64(plan.partOrder), 4); err != nil {
		return errutil.Err(err)
	}

	counts, ok := partitionCounts(len(plan.samples), plan.order, int(plan.partOrder))
	if !ok {
		return errutil.Newf("invalid partition order %d for block of %d samples", plan.partOrder, len(plan.samples))
	}
	offset := 0
	for part, count := range counts {
		k := plan.ks[part]
		// 4 bits: Rice parameter; 0xF would be the escape code and is never used.
		if err := bw.WriteBits(uint64(k), 4); err != nil {
			return errutil.Err(err)
		}
		for _, r := range plan.residuals[offset : offset+count] {
			if err := writeRice1Residual(bw, r, k); err != nil {
				return err
			}
		}
		offset += count
	}
	return nil
}

// writeRice1Residual writes one Rice1 coded residual. Unary prefixes longer
// than 64 bits are emitted in chunks, so isolated transients are encoded
// completely rather than truncated.
func writeRice1Residual(bw *bitio.Writer, r int32, k uint8) error {
	u := foldSigned(r)
	q := uint64(u >> k)
	// Unary prefix: q zero bits followed by a one bit.
	for q > 0 {
		n := uint8(64)
		if q < 64 {
			n = uint8(q)
		}
		if err := bw.WriteBits(0, n); err != nil {
			return errutil.Err(err)
		}
		q -= uint64(n)
	}
	if err := bw.WriteBits(1, 1); err != nil {
		return errutil.Err(err)
	}
	if k > 0 {
		remainder := uint64(u) & (1<<k - 1)
		if err := bw.WriteBits(remainder, k); err != nil {
			return errutil.Err(err)
		}
	}
	return nil
}
