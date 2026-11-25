package mp4

import (
	"fmt"
	"math/big"
	"math/bits"
	"time"

	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/format/mp4/mp4io"
)

func errNegativeClockTime(tm time.Duration) error {
	return fmt.Errorf("mp4: clock time %v is negative", tm)
}

type Stream struct {
	av.CodecData

	trackAtom *mp4io.Track
	idx       int

	lastpkt *av.Packet

	timeScale int64
	duration  int64

	muxer   *Muxer
	demuxer *Demuxer

	sample      *mp4io.SampleTable
	sampleIndex int

	sampleOffsetInChunk int64
	syncSampleIndex     int

	dts                    int64
	sttsEntryIndex         int
	sampleIndexInSttsEntry int

	cttsEntryIndex         int
	sampleIndexInCttsEntry int

	chunkGroupIndex    int
	chunkIndex         int
	sampleIndexInChunk int

	sttsEntry *mp4io.TimeToSampleEntry
	cttsEntry *mp4io.CompositionOffsetEntry
}

// mulDivFloor returns floor(a*b/d) without overflowing an intermediate 64-bit
// product. a, b and d are treated as unsigned and d must be non-zero.
func mulDivFloor(a, b, d uint64) uint64 {
	hi, lo := bits.Mul64(a, b)
	if hi < d {
		q, _ := bits.Div64(hi, lo, d)
		return q
	}
	// The quotient does not fit the fast path. math/big keeps the result exact
	// for the (unusual) very large inputs.
	z := new(big.Int).SetUint64(a)
	z.Mul(z, new(big.Int).SetUint64(b))
	z.Quo(z, new(big.Int).SetUint64(d))
	return z.Uint64()
}

// mulDivRoundHalfUp returns round(a*b/d) where an exact half rounds up.
// a, b and d are treated as unsigned and d must be non-zero.
func mulDivRoundHalfUp(a, b, d uint64) uint64 {
	hi, lo := bits.Mul64(a, b)
	half := d / 2
	lo, carry := bits.Add64(lo, half, 0)
	hi += carry
	if hi < d {
		q, _ := bits.Div64(hi, lo, d)
		return q
	}
	z := new(big.Int).SetUint64(a)
	z.Mul(z, new(big.Int).SetUint64(b))
	z.Add(z, new(big.Int).SetUint64(half))
	z.Quo(z, new(big.Int).SetUint64(d))
	return z.Uint64()
}

// ticksToNs converts media ticks to a duration without multiplying ticks by a
// nanosecond-sized constant in 64 bits.
func ticksToNs(ts int64, timeScale int64) time.Duration {
	scale := uint64(timeScale)
	if ts < 0 {
		return -time.Duration(mulDivFloor(uint64(-ts), uint64(time.Second), scale))
	}
	return time.Duration(mulDivFloor(uint64(ts), uint64(time.Second), scale))
}

// nsToTicksTrunc converts a duration to media ticks, truncating towards zero
// exactly like the historical tm*timeScale/time.Second expression.
func nsToTicksTrunc(tm time.Duration, timeScale int64) int64 {
	scale := uint64(timeScale)
	if tm < 0 {
		return -int64(mulDivFloor(uint64(-tm), scale, uint64(time.Second)))
	}
	return int64(mulDivFloor(uint64(tm), scale, uint64(time.Second)))
}

// nsToTicksRound converts an absolute duration endpoint to media ticks,
// rounding to the nearest tick with an exact half rounding up. It is used by
// the clock muxer so consecutive sample durations are derived from quantized
// endpoints rather than by rounding every gap on its own.
func nsToTicksRound(tm time.Duration, timeScale int64) (uint64, error) {
	if tm < 0 {
		return 0, errNegativeClockTime(tm)
	}
	return mulDivRoundHalfUp(uint64(tm), uint64(timeScale), uint64(time.Second)), nil
}

func timeToTs(tm time.Duration, timeScale int64) int64 {
	return nsToTicksTrunc(tm, timeScale)
}

func tsToTime(ts int64, timeScale int64) time.Duration {
	return ticksToNs(ts, timeScale)
}

func (self *Stream) timeToTs(tm time.Duration) int64 {
	return timeToTs(tm, self.timeScale)
}

func (self *Stream) tsToTime(ts int64) time.Duration {
	return tsToTime(ts, self.timeScale)
}
