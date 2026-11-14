package mp4

import (
	"math"
	"math/big"
	"time"

	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/format/mp4/mp4io"
)

type Stream struct {
	av.CodecData

	trackAtom *mp4io.Track
	idx       int

	lastpkt *av.Packet

	timeScale int64
	duration  int64

	muxer *Muxer
	demuxer *Demuxer

	sample      *mp4io.SampleTable
	sampleIndex int

	sampleOffsetInChunk int64
	syncSampleIndex     int

	dts                    uint64
	sttsEntryIndex         int
	sampleIndexInSttsEntry int

	cttsEntryIndex         int
	sampleIndexInCttsEntry int

	chunkGroupIndex    int
	chunkIndex         int
	sampleIndexInChunk int

	sttsEntry *mp4io.TimeToSampleEntry
	cttsEntry *mp4io.CompositionOffsetEntry

	// clock mode only: quantized absolute endpoint (in track ticks) of
	// every accepted packet, the packets themselves and the absolute Time
	// of the last one. clockEndTicks is the quantized trailer end time.
	clockTicks    []int64
	clockPkts     []av.Packet
	clockLastTime time.Duration
	clockEndTicks int64
}

func timeToTs(tm time.Duration, timeScale int64) int64 {
	return int64(tm*time.Duration(timeScale) / time.Second)
}

func tsToTime(ts int64, timeScale int64) time.Duration {
	return time.Duration(ts)*time.Second / time.Duration(timeScale)
}

func (self *Stream) timeToTs(tm time.Duration) int64 {
	// Split off whole seconds before multiplying so a multi-day duration at
	// a high time scale cannot overflow the intermediate product.
	neg := tm < 0
	if neg {
		tm = -tm
	}
	ns := uint64(tm)
	scale := uint64(self.timeScale)
	ticks := ns/nanosPerSecond*scale + (ns%nanosPerSecond)*scale/nanosPerSecond
	if neg {
		return -int64(ticks)
	}
	return int64(ticks)
}

func (self *Stream) tsToTime(ts uint64) time.Duration {
	// Divide before multiplying by 1e9 to keep the nanosecond result inside
	// int64 even when the tick count is far beyond 32 bits.
	scale := uint64(self.timeScale)
	secs := ts / scale
	rem := ts % scale
	return time.Duration(secs*nanosPerSecond + rem*nanosPerSecond/scale)
}

const nanosPerSecond uint64 = 1000000000

// quantizeClockTicks converts an absolute time endpoint to track ticks.
// Rounding is round-half-away-from-zero (the half tick rounds up) and is
// applied to the absolute endpoint, never to per-segment nanosecond deltas.
// The intermediate product must not overflow: a uint64 fast path is used when
// it cannot, and math/big otherwise. Inputs are non-negative.
func quantizeClockTicks(t time.Duration, timeScale uint32) (int64, error) {
	scale := uint64(timeScale)
	ns := uint64(t)

	// Fast path: ns*scale plus the rounding bias must fit in uint64.
	if ns <= (math.MaxUint64-nanosPerSecond/2)/scale {
		num := ns*scale + nanosPerSecond/2
		return int64(num / nanosPerSecond), nil
	}

	num := new(big.Int).Mul(new(big.Int).SetUint64(ns), new(big.Int).SetUint64(scale))
	num.Add(num, new(big.Int).SetUint64(nanosPerSecond/2))
	num.Div(num, new(big.Int).SetUint64(nanosPerSecond))
	if !num.IsInt64() {
		return 0, errClockTicksOverflow
	}
	return num.Int64(), nil
}

// mulDivFloor returns floor(a*b/c) without overflowing the intermediate
// product, and rejects results that do not fit in uint64.
func mulDivFloor(a, b, c uint64) (uint64, error) {
	if a <= math.MaxUint64/b {
		return (a * b) / c, nil
	}
	out := new(big.Int).Mul(new(big.Int).SetUint64(a), new(big.Int).SetUint64(b))
	out.Div(out, new(big.Int).SetUint64(c))
	if !out.IsUint64() {
		return 0, errClockTicksOverflow
	}
	return out.Uint64(), nil
}
