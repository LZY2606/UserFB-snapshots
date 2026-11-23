package pubsub

import (
	"bytes"
	"fmt"
	"sync/atomic"
	"time"

	"github.com/notedit/rtmp/av"
)

// PlaybackClock is the wall clock used by AddSubTimed. Tests can provide an
// in-memory implementation so playback can be advanced without sleeping.
type PlaybackClock interface {
	Now() time.Time
	NewTimer(time.Duration) PlaybackTimer
}

// PlaybackTimer is a one-shot timer created by a PlaybackClock.
type PlaybackTimer interface {
	C() <-chan time.Time
	Stop() bool
}

// PlaybackOptions configures media-time playback for AddSubTimed.
type PlaybackOptions struct {
	// MaxLag is the maximum amount of media time a subscriber is allowed to
	// fall behind. A stale next packet is dropped and playback restarts from
	// a cached keyframe. MaxLag must be positive.
	MaxLag time.Duration
	// Clock is the playback clock. A nil Clock uses the real clock.
	Clock PlaybackClock
}

type realClock struct{}

func (realClock) Now() time.Time { return time.Now() }

func (realClock) NewTimer(d time.Duration) PlaybackTimer {
	return &realTimer{timer: time.NewTimer(d)}
}

type realTimer struct {
	timer *time.Timer
}

func (t *realTimer) C() <-chan time.Time { return t.timer.C }
func (t *realTimer) Stop() bool          { return t.timer.Stop() }

func isTimedKeyFrame(pkt av.Packet) bool {
	return pkt.Type == av.H264 && pkt.IsKeyFrame
}

func maxDuration(a, b time.Duration) time.Duration {
	if a > b {
		return a
	}
	return b
}

func cloneBytes(b []byte) []byte {
	if b == nil {
		return nil
	}
	return append([]byte(nil), b...)
}

// clonePacket copies every byte slice so a publisher reusing its buffers, or
// one writer mutating a received packet, cannot affect the cache or another
// subscriber.
func clonePacket(pkt av.Packet) av.Packet {
	pkt.Data = cloneBytes(pkt.Data)
	pkt.ASeqHdr = cloneBytes(pkt.ASeqHdr)
	pkt.VSeqHdr = cloneBytes(pkt.VSeqHdr)
	pkt.Metadata = cloneBytes(pkt.Metadata)
	return pkt
}

// timedHeaderState tracks the metadata and sequence headers already sent to a
// single subscriber. Resetting it forces the next publication's configuration
// to be written even when the bytes are identical to the previous stream.
type timedHeaderState struct {
	metadata []byte
	vseq     []byte
	aseq     []byte

	metadataSet bool
	vseqSet     bool
	aseqSet     bool
}

func (h *timedHeaderState) reset() {
	*h = timedHeaderState{}
}

func shouldEmitHeader(cur []byte, sent []byte, set bool) bool {
	if !set {
		// After a publication change, emit a non-empty header regardless of
		// whether an identical header was sent for the previous publication.
		return len(cur) > 0
	}
	return !bytes.Equal(sent, cur)
}

func (h *timedHeaderState) deliver(w av.PacketWriter, pkt av.Packet, outTime time.Duration) error {
	switch pkt.Type {
	case av.H264:
		if shouldEmitHeader(pkt.Metadata, h.metadata, h.metadataSet) {
			if err := w.WritePacket(av.Packet{
				Type: av.Metadata,
				Data: cloneBytes(pkt.Metadata),
				Time: outTime,
			}); err != nil {
				return err
			}
			h.metadata = cloneBytes(pkt.Metadata)
			h.metadataSet = true
		}
		if pkt.IsKeyFrame && shouldEmitHeader(pkt.VSeqHdr, h.vseq, h.vseqSet) {
			if err := w.WritePacket(av.Packet{
				Type: av.H264DecoderConfig,
				Data: cloneBytes(pkt.VSeqHdr),
				Time: outTime,
			}); err != nil {
				return err
			}
			h.vseq = cloneBytes(pkt.VSeqHdr)
			h.vseqSet = true
		}
	case av.AAC:
		if shouldEmitHeader(pkt.Metadata, h.metadata, h.metadataSet) {
			if err := w.WritePacket(av.Packet{
				Type: av.Metadata,
				Data: cloneBytes(pkt.Metadata),
				Time: outTime,
			}); err != nil {
				return err
			}
			h.metadata = cloneBytes(pkt.Metadata)
			h.metadataSet = true
		}
		if shouldEmitHeader(pkt.ASeqHdr, h.aseq, h.aseqSet) {
			if err := w.WritePacket(av.Packet{
				Type: av.AACDecoderConfig,
				Data: cloneBytes(pkt.ASeqHdr),
				Time: outTime,
			}); err != nil {
				return err
			}
			h.aseq = cloneBytes(pkt.ASeqHdr)
			h.aseqSet = true
		}
	}

	out := clonePacket(pkt)
	out.Time = outTime
	return w.WritePacket(out)
}

// waitTimed blocks until close is signalled, notify is delivered, or the
// optional timer fires. It reports whether close was signalled. The caller
// must not hold any publication lock while calling it.
func waitTimed(closeCh <-chan bool, notify <-chan struct{}, timer PlaybackTimer) (closed bool) {
	if timer == nil {
		select {
		case <-closeCh:
			return true
		case <-notify:
			return false
		}
	}

	defer timer.Stop()
	select {
	case <-closeCh:
		return true
	case <-notify:
		return false
	case <-timer.C():
		return false
	}
}

// AddSubTimed subscribes to the current publication and writes packets in
// media time. The first delivered video packet is anchored at Time 0; later
// packets are scheduled by their DTS delta. If a subscriber falls more than
// opts.MaxLag behind, stale media is skipped and playback restarts from the
// newest cached keyframe that has not been delivered to this subscriber yet.
//
// Every subscriber keeps its own cursor, anchor and header state. Notifications
// are only hints to re-read the cache, so duplicate notifications and stale
// timer wakes never replay a packet that was already written.
func (s *PubSub) AddSubTimed(closeCh <-chan bool, w av.PacketWriter, opts PlaybackOptions) error {
	if opts.MaxLag <= 0 {
		return fmt.Errorf("pubsub: playback MaxLag must be positive")
	}
	clock := opts.Clock
	if clock == nil {
		clock = realClock{}
	}

	ss := &streamSub{
		notify: make(chan struct{}, 1),
	}
	s.sub.Store(ss, nil)
	defer s.sub.Delete(ss)

	var (
		lastsp *streamPub

		// consumed is the absolute cache index of the next packet this
		// subscriber still has to deliver.
		consumed        int
		waitingKeyframe bool

		haveAnchor bool
		anchorDTS  time.Duration
		anchorWall time.Time
		anchorOut  time.Duration
		firstStart time.Time

		lastMediaTime time.Duration
		headers       timedHeaderState
	)

	resetPublication := func() {
		consumed = 0
		waitingKeyframe = true
		headers.reset()
	}

	for {
		select {
		case <-closeCh:
			return nil
		default:
		}

		// A publisher change must be detected even if the subscriber is
		// still waiting on the previous publication's packets. SetPub also
		// notifies subscribers so a pending timer is cancelled immediately.
		spPtr := atomic.LoadPointer(&s.pub)
		sp := (*streamPub)(spPtr)
		if sp != lastsp {
			lastsp = sp
			resetPublication()
		}

		var cur *gopCacheSnapshot
		if sp != nil {
			cur = sp.gc.curSnapshot()
		}
		if cur == nil || len(cur.pkts) == 0 {
			if waitTimed(closeCh, ss.notify, nil) {
				return nil
			}
			continue
		}

		firstPos := cur.idx - len(cur.pkts)
		if firstPos < 0 {
			firstPos = 0
		}

		if waitingKeyframe {
			if firstPos >= consumed && isTimedKeyFrame(cur.pkts[0]) {
				consumed = firstPos
				waitingKeyframe = false
				if haveAnchor {
					// Restarting (catch-up or a new publication) from a
					// keyframe: continue the subscriber's output timeline
					// instead of jumping backwards.
					now := clock.Now()
					anchorOut = maxDuration(lastMediaTime, now.Sub(firstStart))
					anchorDTS = cur.pkts[0].Time
					anchorWall = now
				}
			} else {
				// Audio before the keyframe must not be written.
				consumed = cur.idx
				if waitTimed(closeCh, ss.notify, nil) {
					return nil
				}
				continue
			}
		} else if consumed < firstPos {
			// A newer keyframe replaced the GOP before we consumed the rest
			// of the older one. Continue from the cache start.
			consumed = firstPos
		} else if consumed > cur.idx {
			consumed = cur.idx
		}

		if consumed >= cur.idx {
			if waitTimed(closeCh, ss.notify, nil) {
				return nil
			}
			continue
		}

		pkt := cur.pkts[consumed-firstPos]
		if pkt.Type != av.H264 && pkt.Type != av.AAC {
			consumed++
			continue
		}

		if !haveAnchor {
			if pkt.Type != av.H264 || !pkt.IsKeyFrame {
				consumed++
				continue
			}
			now := clock.Now()
			anchorDTS = pkt.Time
			anchorWall = now
			anchorOut = 0
			firstStart = now
			haveAnchor = true
		}

		now := clock.Now()
		target := anchorWall.Add(pkt.Time - anchorDTS)

		if now.Sub(target) > opts.MaxLag {
			// The next packet is too old. Drop it and restart from the
			// newest cached keyframe this subscriber has not seen.
			if firstPos >= consumed && isTimedKeyFrame(cur.pkts[0]) {
				consumed = firstPos
				pkt = cur.pkts[0]
				now = clock.Now()
				anchorOut = maxDuration(lastMediaTime, now.Sub(firstStart))
				anchorDTS = pkt.Time
				anchorWall = now
				target = now
			} else {
				// No undelivered keyframe is cached. Skip everything up to
				// the next keyframe instead of replaying the old GOP.
				consumed = cur.idx
				waitingKeyframe = true
				continue
			}
		} else if now.Before(target) {
			d := target.Sub(now)
			if d <= 0 {
				d = time.Nanosecond
			}
			timer := clock.NewTimer(d)
			if waitTimed(closeCh, ss.notify, timer) {
				return nil
			}
			// Re-read the publisher and cache; a notification may have
			// replaced either while this subscriber was waiting.
			continue
		}

		outTime := anchorOut + (pkt.Time - anchorDTS)

		// Final identity check: a packet from a replaced publication must
		// not be written. Packets already inside WritePacket are allowed to
		// finish.
		if atomic.LoadPointer(&s.pub) != spPtr {
			continue
		}

		if err := headers.deliver(w, pkt, outTime); err != nil {
			return err
		}
		consumed++
		lastMediaTime = outTime
	}
}
