package mp4

import (
	"errors"
	"fmt"
	"time"

	"github.com/nareix/joy4/av"
)

var (
	errClockTicksOverflow = errors.New("mp4: clock tick conversion overflows int64")
	errClockBadFirstTime  = errors.New("mp4: clock mode requires the first packet Time of every stream to be zero")
	errClockTimeNotStrict = errors.New("mp4: clock mode requires strictly increasing packet Time")
	errClockSameTick      = errors.New("mp4: clock mode rejected packet: endpoints quantize to the same tick")
	errClockDurationWide  = errors.New("mp4: clock mode rejected packet: sample duration does not fit in uint32")
)

// writeClockPacket validates and buffers one packet in clock mode. A rejected
// packet is not consumed: no payload is written and no pending sample table
// entry changes, so a later legal packet can still be committed.
func (self *Muxer) writeClockPacket(pkt av.Packet) error {
	if int(pkt.Idx) < 0 || int(pkt.Idx) >= len(self.streams) {
		return fmt.Errorf("mp4: packet index %d out of range", pkt.Idx)
	}
	stream := self.streams[pkt.Idx]

	if len(stream.clockTicks) == 0 {
		if pkt.Time != 0 {
			return errClockBadFirstTime
		}
	} else if pkt.Time <= stream.clockLastTime {
		return errClockTimeNotStrict
	}

	ticks, err := quantizeClockTicks(pkt.Time, uint32(stream.timeScale))
	if err != nil {
		return err
	}

	if len(stream.clockTicks) > 0 {
		duration := ticks - stream.clockTicks[len(stream.clockTicks)-1]
		if duration <= 0 {
			return errClockSameTick
		}
		if duration > int64(^uint32(0)) {
			return errClockDurationWide
		}
	}

	// Commit only after every check passed.
	if _, err := self.bufw.Write(pkt.Data); err != nil {
		return err
	}

	if pkt.IsKeyFrame && stream.sample.SyncSample != nil {
		stream.sample.SyncSample.Entries = append(
			stream.sample.SyncSample.Entries, uint32(stream.sampleIndex+1))
	}

	// Sample duration is finalized when the next endpoint arrives (or at
	// trailer time); record the packet itself in arrival order.
	stream.clockTicks = append(stream.clockTicks, ticks)
	stream.clockLastTime = pkt.Time
	stream.clockPkts = append(stream.clockPkts, pkt)
	stream.sampleIndex++
	stream.sample.ChunkOffset.Entries = append(
		stream.sample.ChunkOffset.Entries, uint32(self.wpos))
	stream.sample.SampleSize.Entries = append(
		stream.sample.SampleSize.Entries, uint32(len(pkt.Data)))
	self.wpos += int64(len(pkt.Data))
	return nil
}

// WriteTrailerAt finishes a clock-mode file. ends holds the absolute end time
// of the last packet of every stream and is copied before validation. All
// streams are validated before any pending data is flushed, so a bad argument
// leaves the muxer usable for a corrected retry.
func (self *Muxer) WriteTrailerAt(ends []time.Duration) error {
	if !self.clock {
		return errors.New("mp4: WriteTrailerAt is only valid for a clock muxer")
	}
	if len(ends) != len(self.streams) {
		return fmt.Errorf("mp4: WriteTrailerAt got %d end times for %d streams",
			len(ends), len(self.streams))
	}

	endsCopy := make([]time.Duration, len(ends))
	copy(endsCopy, ends)

	type clockPending struct {
		stream   *Stream
		endTicks int64
	}
	pending := make([]clockPending, len(self.streams))

	// Validate every stream first; nothing is mutated on failure.
	for i, stream := range self.streams {
		end := endsCopy[i]
		if len(stream.clockTicks) == 0 {
			if end != 0 {
				return fmt.Errorf("mp4: stream#%d is empty but got non-zero end time %v", i, end)
			}
			pending[i] = clockPending{stream: stream, endTicks: 0}
			continue
		}

		lastTime := stream.clockLastTime
		if end <= lastTime {
			return fmt.Errorf("mp4: stream#%d end time %v must be later than last packet time %v",
				i, end, lastTime)
		}

		endTicks, err := quantizeClockTicks(end, uint32(stream.timeScale))
		if err != nil {
			return err
		}
		lastEndTick := stream.clockTicks[len(stream.clockTicks)-1]
		if endTicks <= lastEndTick {
			return fmt.Errorf("mp4: stream#%d quantized end tick %d must be greater than last tick %d",
				i, endTicks, lastEndTick)
		}
		if endTicks-lastEndTick > int64(^uint32(0)) {
			return errClockDurationWide
		}
		pending[i] = clockPending{stream: stream, endTicks: endTicks}
	}

	// All streams validated: build stts/ctts and finalize durations from the
	// single shared set of quantized absolute endpoints, where the final
	// boundary is the quantized trailer end time. Sample j lasts from
	// boundary j to boundary j+1, so N packets plus the end boundary yield
	// exactly N sample durations.
	for _, p := range pending {
		stream := p.stream
		if len(stream.clockTicks) == 0 {
			stream.clockEndTicks = 0
			continue
		}

		boundaries := make([]int64, 0, len(stream.clockTicks)+1)
		boundaries = append(boundaries, stream.clockTicks...)
		boundaries = append(boundaries, p.endTicks)

		prev := boundaries[0]
		for j := 1; j < len(boundaries); j++ {
			duration := uint32(boundaries[j] - prev)
			stream.appendSampleTiming(stream.clockPkts[j-1], duration)
			prev = boundaries[j]
		}
		stream.clockEndTicks = p.endTicks
	}

	return self.writeMovieAtom(true)
}
