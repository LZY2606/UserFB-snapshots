package pubsub

import (
	"errors"
	"io"
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"

	"github.com/notedit/rtmp/av"
	"github.com/notedit/rtmp/task_support/taskcheck"
)

// ---------------------------------------------------------------------------
// Test fixtures: controlled clock, channel reader, in-memory writer.
// ---------------------------------------------------------------------------

type testClock struct {
	mu     sync.Mutex
	now    time.Time
	timers []*testTimer
}

type testTimer struct {
	clk      *testClock
	ch       chan time.Time
	deadline time.Time
	stopped  bool
	fired    bool
}

func newTestClock() *testClock {
	return &testClock{now: time.Unix(1700000000, 0)}
}

func (c *testClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *testClock) NewTimer(d time.Duration) PlaybackTimer {
	c.mu.Lock()
	defer c.mu.Unlock()
	t := &testTimer{
		clk:      c,
		ch:       make(chan time.Time, 1),
		deadline: c.now.Add(d),
	}
	c.timers = append(c.timers, t)
	return t
}

func (c *testClock) Advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.now = c.now.Add(d)
	now := c.now
	var live []*testTimer
	for _, t := range c.timers {
		if t.stopped || t.fired {
			continue
		}
		if !t.deadline.After(now) {
			t.fired = true
			select {
			case t.ch <- now:
			default:
			}
			continue
		}
		live = append(live, t)
	}
	c.timers = live
}

func (c *testClock) activeTimers() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	n := 0
	for _, t := range c.timers {
		if !t.stopped && !t.fired {
			n++
		}
	}
	return n
}

func (c *testClock) waitActive(pred func(int) bool) bool {
	deadline := time.Now().Add(2 * time.Second)
	for {
		if pred(c.activeTimers()) {
			return true
		}
		if time.Now().After(deadline) {
			return false
		}
		time.Sleep(time.Millisecond)
	}
}

func (t *testTimer) C() <-chan time.Time { return t.ch }

func (t *testTimer) Stop() bool {
	t.clk.mu.Lock()
	defer t.clk.mu.Unlock()
	if t.stopped || t.fired {
		return false
	}
	t.stopped = true
	return true
}

// packetFeed is a channel-backed av.PacketReader.
type packetFeed struct {
	ch chan av.Packet
}

func newPacketFeed() *packetFeed {
	return &packetFeed{ch: make(chan av.Packet, 64)}
}

func (f *packetFeed) ReadPacket() (av.Packet, error) {
	p, ok := <-f.ch
	if !ok {
		return av.Packet{}, io.EOF
	}
	return p, nil
}

func (f *packetFeed) send(p av.Packet) { f.ch <- p }

func (f *packetFeed) close() { close(f.ch) }

// testSink is an in-memory av.PacketWriter.
type testSink struct {
	mu     sync.Mutex
	pkts   []av.Packet
	sig    chan struct{}
	err    error
	mutate bool
}

func newTestSink() *testSink {
	return &testSink{sig: make(chan struct{}, 1)}
}

func (s *testSink) WritePacket(p av.Packet) error {
	s.mu.Lock()
	if s.err != nil {
		err := s.err
		s.mu.Unlock()
		return err
	}
	// Keep our own copy so later mutation of the received packet cannot
	// rewrite what the sink recorded.
	s.pkts = append(s.pkts, clonePacket(p))
	if s.mutate {
		for i := range p.Data {
			p.Data[i] ^= 0xff
		}
		for i := range p.Metadata {
			p.Metadata[i] ^= 0xff
		}
		for i := range p.VSeqHdr {
			p.VSeqHdr[i] ^= 0xff
		}
		for i := range p.ASeqHdr {
			p.ASeqHdr[i] ^= 0xff
		}
	}
	s.mu.Unlock()
	select {
	case s.sig <- struct{}{}:
	default:
	}
	return nil
}

func (s *testSink) count() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.pkts)
}

func (s *testSink) snapshot() []av.Packet {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]av.Packet, len(s.pkts))
	copy(out, s.pkts)
	return out
}

func (s *testSink) waitCount(n int, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for {
		if s.count() >= n {
			return true
		}
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return false
		}
		select {
		case <-s.sig:
		case <-time.After(remaining):
		}
	}
}

// settled reports whether no more than n packets were written during d.
func (s *testSink) settled(n int, d time.Duration) bool {
	deadline := time.Now().Add(d)
	for {
		if s.count() > n {
			return false
		}
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return s.count() <= n
		}
		select {
		case <-s.sig:
		case <-time.After(remaining):
		}
	}
}

type timedSub struct {
	closeCh chan bool
	done    chan error
}

func startTimedSub(s *PubSub, opts PlaybackOptions, w av.PacketWriter) *timedSub {
	h := &timedSub{
		closeCh: make(chan bool),
		done:    make(chan error, 1),
	}
	go func() { h.done <- s.AddSubTimed(h.closeCh, w, opts) }()
	return h
}

func (h *timedSub) stop() error {
	close(h.closeCh)
	select {
	case err := <-h.done:
		return err
	case <-time.After(2 * time.Second):
		return errors.New("subscriber did not stop after close")
	}
}

func (h *timedSub) waitDone(d time.Duration) (error, bool) {
	select {
	case err := <-h.done:
		return err, true
	case <-time.After(d):
		return nil, false
	}
}

func startPublication(s *PubSub, f *packetFeed) {
	go s.SetPub(f)
}

func waitCacheIdx(s *PubSub, idx int) bool {
	deadline := time.Now().Add(2 * time.Second)
	for {
		if cur := s.curGopCacheSnapshot(); cur != nil && cur.idx >= idx {
			return true
		}
		if time.Now().After(deadline) {
			return false
		}
		time.Sleep(time.Millisecond)
	}
}

func waitPubChange(s *PubSub, previous unsafe.Pointer) bool {
	deadline := time.Now().Add(2 * time.Second)
	for {
		if atomic.LoadPointer(&s.pub) != previous {
			return true
		}
		if time.Now().After(deadline) {
			return false
		}
		time.Sleep(time.Millisecond)
	}
}

func bytesEqual(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// ---------------------------------------------------------------------------
// TestTimedSchedule: first packet, anchor, exact due boundary, ordering.
// ---------------------------------------------------------------------------

func TestTimedSchedule(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Invalid MaxLag must be reported before anything is written.
		bad := newTestSink()
		err := (&PubSub{}).AddSubTimed(make(chan bool), bad, PlaybackOptions{MaxLag: 0})
		c.Check(err != nil, "MaxLag=0 must be rejected")
		c.Check(bad.count() == 0, "invalid options must not write packets")
		err = (&PubSub{}).AddSubTimed(make(chan bool), bad, PlaybackOptions{MaxLag: -time.Millisecond})
		c.Check(err != nil, "negative MaxLag must be rejected")

		clk := newTestClock()
		s := &PubSub{}
		feed := newPacketFeed()
		defer feed.close()
		startPublication(s, feed)

		feed.send(av.Packet{Type: av.Metadata, Data: []byte("meta")})
		feed.send(av.Packet{Type: av.H264DecoderConfig, Data: []byte{1, 2}})
		feed.send(av.Packet{Type: av.AACDecoderConfig, Data: []byte{3, 4}})
		feed.send(av.Packet{
			Type:       av.H264,
			IsKeyFrame: true,
			Time:       100 * time.Millisecond,
			CTime:      -5 * time.Millisecond,
			Data:       []byte{5},
		})
		feed.send(av.Packet{Type: av.AAC, Time: 120 * time.Millisecond, Data: []byte{6}})
		c.Check(waitCacheIdx(s, 2), "publisher should cache two media packets")

		sink := newTestSink()
		sub := startTimedSub(s, PlaybackOptions{MaxLag: 50 * time.Millisecond, Clock: clk}, sink)

		c.Check(sink.waitCount(3, 2*time.Second), "expected metadata, sequence header and first video")
		pkts := sink.snapshot()
		if len(pkts) >= 3 {
			c.Check(pkts[0].Type == av.Metadata && bytesEqual(pkts[0].Data, []byte("meta")),
				"metadata should precede the keyframe, got %v", pkts[0])
			c.Check(pkts[0].Time == 0, "metadata time should match its media, got %v", pkts[0].Time)
			c.Check(pkts[1].Type == av.H264DecoderConfig && bytesEqual(pkts[1].Data, []byte{1, 2}),
				"video sequence header should precede the keyframe, got %v", pkts[1])
			c.Check(pkts[1].Time == 0, "sequence header time should match its media, got %v", pkts[1].Time)
			c.Check(pkts[2].Type == av.H264 && pkts[2].IsKeyFrame,
				"third packet should be the keyframe, got %v", pkts[2])
			c.Check(pkts[2].Time == 0, "first video packet must be anchored at Time 0, got %v", pkts[2].Time)
			c.Check(pkts[2].CTime == -5*time.Millisecond, "CTime must be preserved, got %v", pkts[2].CTime)
			c.Check(bytesEqual(pkts[2].Data, []byte{5}), "payload must be preserved")
		}

		c.Check(sink.settled(3, 30*time.Millisecond), "audio must not be written before its media time")
		clk.Advance(19 * time.Millisecond)
		c.Check(sink.settled(3, 20*time.Millisecond), "audio must not be written 1ms before due")
		clk.Advance(1 * time.Millisecond)
		c.Check(sink.waitCount(5, 2*time.Second), "audio config and audio should be written once due")
		pkts = sink.snapshot()
		if len(pkts) >= 5 {
			c.Check(pkts[3].Type == av.AACDecoderConfig && bytesEqual(pkts[3].Data, []byte{3, 4}),
				"audio sequence header should sit right before its audio, got %v", pkts[3])
			c.Check(pkts[3].Time == 20*time.Millisecond,
				"audio sequence header time should match its media, got %v", pkts[3].Time)
			c.Check(pkts[4].Type == av.AAC, "fifth packet should be audio, got %v", pkts[4])
			c.Check(pkts[4].Time == 20*time.Millisecond, "audio time should be 20ms, got %v", pkts[4].Time)
			c.Check(bytesEqual(pkts[4].Data, []byte{6}), "audio payload must be preserved")
		}
		c.Check(sub.stop() == nil, "close should stop the subscriber without error")

		// A nil Clock means the real clock is used.
		s2 := &PubSub{}
		feed2 := newPacketFeed()
		defer feed2.close()
		startPublication(s2, feed2)
		feed2.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 5 * time.Second, Data: []byte{1}})
		c.Check(waitCacheIdx(s2, 1), "second publisher should cache a keyframe")
		sink2 := newTestSink()
		sub2 := startTimedSub(s2, PlaybackOptions{MaxLag: time.Second}, sink2)
		c.Check(sink2.waitCount(1, 2*time.Second), "nil Clock must fall back to the real clock")
		pk2 := sink2.snapshot()
		if len(pk2) == 1 {
			c.Check(pk2[0].Time == 0, "real-clock first video must be Time 0")
		}
		c.Check(sub2.stop() == nil, "real-clock subscriber should stop cleanly")
	})
}

// ---------------------------------------------------------------------------
// TestTimedCatchup: MaxLag boundaries and restarting from keyframes.
// ---------------------------------------------------------------------------

func TestTimedCatchup(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Lag exactly MaxLag is still allowed.
		clk := newTestClock()
		s := &PubSub{}
		feed := newPacketFeed()
		defer feed.close()
		startPublication(s, feed)
		feed.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 100 * time.Millisecond, Data: []byte{1}})
		feed.send(av.Packet{Type: av.AAC, Time: 120 * time.Millisecond, Data: []byte{2}})
		c.Check(waitCacheIdx(s, 2), "cache should hold keyframe and audio")

		sink := newTestSink()
		sub := startTimedSub(s, PlaybackOptions{MaxLag: 50 * time.Millisecond, Clock: clk}, sink)
		c.Check(sink.waitCount(1, 2*time.Second), "first video should be written")
		clk.Advance(70 * time.Millisecond) // 20ms media delta + MaxLag exactly
		c.Check(sink.waitCount(2, 2*time.Second), "lag equal to MaxLag must not drop the packet")
		pk := sink.snapshot()
		if len(pk) >= 2 {
			c.Check(pk[1].Type == av.AAC && pk[1].Time == 20*time.Millisecond,
				"audio at the MaxLag boundary should be Time 20ms, got %v", pk[1])
		}
		c.Check(sub.stop() == nil, "boundary subscriber should stop cleanly")

		// Lag greater than MaxLag with no undelivered keyframe: wait for the
		// next keyframe instead of replaying the old GOP.
		clk2 := newTestClock()
		s2 := &PubSub{}
		feed2 := newPacketFeed()
		defer feed2.close()
		startPublication(s2, feed2)
		feed2.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 0, Data: []byte{10}})
		feed2.send(av.Packet{Type: av.AAC, Time: 20 * time.Millisecond, Data: []byte{11}})
		c.Check(waitCacheIdx(s2, 2), "second cache should hold keyframe and audio")

		sink2 := newTestSink()
		sub2 := startTimedSub(s2, PlaybackOptions{MaxLag: 50 * time.Millisecond, Clock: clk2}, sink2)
		c.Check(sink2.waitCount(1, 2*time.Second), "second first video should be written")
		clk2.Advance(71 * time.Millisecond) // 20ms delta + MaxLag + 1ms
		c.Check(sink2.settled(1, 50*time.Millisecond), "stale audio must be dropped past MaxLag")

		feed2.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 200 * time.Millisecond, Data: []byte{12}})
		c.Check(sink2.waitCount(2, 2*time.Second), "playback should resume at the next keyframe")
		pk2 := sink2.snapshot()
		if len(pk2) >= 2 {
			c.Check(pk2[1].Type == av.H264 && pk2[1].IsKeyFrame,
				"resume packet should be a keyframe, got %v", pk2[1])
			c.Check(pk2[1].Time == 71*time.Millisecond,
				"resumed keyframe time should be max(last, now-firstStart)=71ms, got %v", pk2[1].Time)
			c.Check(bytesEqual(pk2[1].Data, []byte{12}), "resumed payload should be preserved")
		}
		c.Check(sink2.settled(2, 30*time.Millisecond), "old GOP packets must not be replayed")
		feed2.send(av.Packet{Type: av.AAC, Time: 210 * time.Millisecond, Data: []byte{13}})
		clk2.Advance(10 * time.Millisecond)
		c.Check(sink2.waitCount(3, 2*time.Second), "audio after resume should follow its DTS delta")
		pk2 = sink2.snapshot()
		if len(pk2) >= 3 {
			c.Check(pk2[2].Time == 81*time.Millisecond,
				"audio after resume should be 81ms, got %v", pk2[2].Time)
		}
		c.Check(sub2.stop() == nil, "wait-for-keyframe subscriber should stop cleanly")

		// Lag past MaxLag while a newer keyframe is already cached: restart
		// from that new keyframe immediately.
		clk3 := newTestClock()
		s3 := &PubSub{}
		feed3 := newPacketFeed()
		defer feed3.close()
		startPublication(s3, feed3)
		feed3.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 0, Data: []byte{20}})
		feed3.send(av.Packet{Type: av.AAC, Time: 10 * time.Millisecond, Data: []byte{21}})
		c.Check(waitCacheIdx(s3, 2), "third cache should hold first GOP")

		sink3 := newTestSink()
		sub3 := startTimedSub(s3, PlaybackOptions{MaxLag: 50 * time.Millisecond, Clock: clk3}, sink3)
		c.Check(sink3.waitCount(1, 2*time.Second), "third first video should be written")

		feed3.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 100 * time.Millisecond, Data: []byte{22}})
		feed3.send(av.Packet{Type: av.AAC, Time: 110 * time.Millisecond, Data: []byte{23}})
		c.Check(waitCacheIdx(s3, 4), "third cache should hold the new GOP")

		clk3.Advance(200 * time.Millisecond)
		c.Check(sink3.waitCount(2, 2*time.Second), "catch-up should start at the newest cached keyframe")
		pk3 := sink3.snapshot()
		if len(pk3) >= 2 {
			c.Check(bytesEqual(pk3[1].Data, []byte{22}), "catch-up should pick the newest keyframe")
			c.Check(pk3[1].Time == 200*time.Millisecond,
				"catch-up keyframe should use now-firstStart=200ms, got %v", pk3[1].Time)
		}
		clk3.Advance(10 * time.Millisecond)
		c.Check(sink3.waitCount(3, 2*time.Second), "audio after catch-up should be delivered")
		pk3 = sink3.snapshot()
		if len(pk3) >= 3 {
			c.Check(pk3[2].Time == 210*time.Millisecond,
				"audio after catch-up should be re-anchored to 210ms, got %v", pk3[2].Time)
		}
		c.Check(sub3.stop() == nil, "catch-up subscriber should stop cleanly")
	})
}

// ---------------------------------------------------------------------------
// TestTimedPublisher: publisher changes, config re-emission, ownership, isolation.
// ---------------------------------------------------------------------------

func TestTimedPublisher(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clk := newTestClock()
		s := &PubSub{}
		feed1 := newPacketFeed()
		defer feed1.close()
		startPublication(s, feed1)

		feed1.send(av.Packet{Type: av.Metadata, Data: []byte("meta")})
		feed1.send(av.Packet{Type: av.H264DecoderConfig, Data: []byte{1, 2}})
		feed1.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 0, Data: []byte{0x11}})
		feed1.send(av.Packet{Type: av.AAC, Time: time.Second, Data: []byte{0x99}})
		c.Check(waitCacheIdx(s, 2), "publisher should cache the first GOP")

		sink := newTestSink()
		sub := startTimedSub(s, PlaybackOptions{MaxLag: 100 * time.Millisecond, Clock: clk}, sink)
		c.Check(sink.waitCount(3, 2*time.Second), "first publication should emit config and video")
		c.Check(clk.waitActive(func(n int) bool { return n >= 1 }),
			"subscriber should be waiting for the old audio packet")

		// Switching publishers while the subscriber waits must cancel the
		// old wait immediately, before the new publisher produces packets.
		before := atomic.LoadPointer(&s.pub)
		feed2 := newPacketFeed()
		defer feed2.close()
		startPublication(s, feed2)
		c.Check(waitPubChange(s, before), "new publisher should be installed")
		c.Check(clk.waitActive(func(n int) bool { return n == 0 }),
			"publisher change must cancel the pending playback wait")
		c.Check(sink.settled(3, 30*time.Millisecond), "no packet from the old publisher may be delivered")

		// Identical config bytes and DTS starting at 0 must still re-emit the
		// configuration and restart this subscriber's playback.
		feed2.send(av.Packet{Type: av.Metadata, Data: []byte("meta")})
		feed2.send(av.Packet{Type: av.H264DecoderConfig, Data: []byte{1, 2}})
		feed2.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 0, Data: []byte{0x22}})
		feed2.send(av.Packet{Type: av.AAC, Time: 10 * time.Millisecond, Data: []byte{0x33}})
		c.Check(sink.waitCount(6, 2*time.Second), "new publication should re-emit metadata and sequence header")
		pk := sink.snapshot()
		if len(pk) >= 6 {
			c.Check(pk[3].Type == av.Metadata && bytesEqual(pk[3].Data, []byte("meta")),
				"metadata must be re-emitted for the new publisher, got %v", pk[3])
			c.Check(pk[4].Type == av.H264DecoderConfig && bytesEqual(pk[4].Data, []byte{1, 2}),
				"sequence header must be re-emitted even if its bytes are identical, got %v", pk[4])
			c.Check(pk[5].Type == av.H264 && bytesEqual(pk[5].Data, []byte{0x22}),
				"new publisher's video should follow its config, got %v", pk[5])
			c.Check(pk[5].Time == 0, "new publisher's first video should restart at Time 0, got %v", pk[5].Time)
		}
		for i, p := range pk {
			c.Check(!bytesEqual(p.Data, []byte{0x99}), "old publisher audio must never be replayed (index %d)", i)
		}
		clk.Advance(10 * time.Millisecond)
		c.Check(sink.waitCount(7, 2*time.Second), "new publication audio should follow its DTS delta")
		pk = sink.snapshot()
		if len(pk) >= 7 {
			c.Check(pk[6].Type == av.AAC && pk[6].Time == 10*time.Millisecond,
				"new publisher audio should be Time 10ms, got %v", pk[6])
		}
		c.Check(sub.stop() == nil, "publisher-change subscriber should stop cleanly")

		// Writer errors are returned unchanged.
		s2 := &PubSub{}
		feed3 := newPacketFeed()
		defer feed3.close()
		startPublication(s2, feed3)
		feed3.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 0, Data: []byte{1}})
		c.Check(waitCacheIdx(s2, 1), "error publisher should cache a keyframe")
		boom := errors.New("boom")
		failSink := newTestSink()
		failSink.err = boom
		failSub := startTimedSub(s2, PlaybackOptions{MaxLag: 50 * time.Millisecond, Clock: newTestClock()}, failSink)
		if err, ok := failSub.waitDone(2 * time.Second); !ok {
			c.Check(false, "writer error should stop the subscriber")
		} else {
			c.Check(err == boom, "WritePacket error must be returned unchanged, got %v", err)
		}

		// close ends a pending wait immediately.
		s3 := &PubSub{}
		feed4 := newPacketFeed()
		defer feed4.close()
		startPublication(s3, feed4)
		feed4.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 0, Data: []byte{1}})
		feed4.send(av.Packet{Type: av.AAC, Time: time.Second, Data: []byte{2}})
		c.Check(waitCacheIdx(s3, 2), "close publisher should cache a GOP")
		clk3 := newTestClock()
		sink3 := newTestSink()
		sub3 := startTimedSub(s3, PlaybackOptions{MaxLag: 100 * time.Millisecond, Clock: clk3}, sink3)
		c.Check(sink3.waitCount(1, 2*time.Second), "close scenario first video should be written")
		c.Check(clk3.waitActive(func(n int) bool { return n >= 1 }), "close scenario should be waiting")
		c.Check(sub3.stop() == nil, "close must end a pending wait with a nil error")

		// Publisher buffer reuse must not pollute the cache.
		gc := &gopCache{}
		buf := []byte{7, 8, 9}
		gc.put(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 0, Data: buf})
		buf[0] = 0xee
		snap := gc.curSnapshot()
		c.Check(snap != nil && len(snap.pkts) == 1 && snap.pkts[0].Data[0] == 7,
			"cache must copy publisher buffers")

		// Two subscribers keep independent cursors/anchors and byte ownership.
		clk4 := newTestClock()
		s4 := &PubSub{}
		feed5 := newPacketFeed()
		defer feed5.close()
		startPublication(s4, feed5)
		feed5.send(av.Packet{Type: av.Metadata, Data: []byte("meta")})
		feed5.send(av.Packet{Type: av.H264DecoderConfig, Data: []byte{1, 2}})
		feed5.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 0, Data: []byte{7, 8}})
		feed5.send(av.Packet{Type: av.AAC, Time: 10 * time.Millisecond, Data: []byte{9}})
		c.Check(waitCacheIdx(s4, 2), "isolation publisher should cache its GOP")

		mutSink := newTestSink()
		mutSink.mutate = true
		plainSink := newTestSink()
		mutSub := startTimedSub(s4, PlaybackOptions{MaxLag: 50 * time.Millisecond, Clock: clk4}, mutSink)
		plainSub := startTimedSub(s4, PlaybackOptions{MaxLag: 50 * time.Millisecond, Clock: clk4}, plainSink)
		c.Check(mutSink.waitCount(3, 2*time.Second), "mutating subscriber should receive config and video")
		c.Check(plainSink.waitCount(3, 2*time.Second), "plain subscriber should receive config and video")
		clk4.Advance(10 * time.Millisecond)
		c.Check(mutSink.waitCount(4, 2*time.Second), "mutating subscriber should receive audio")
		c.Check(plainSink.waitCount(4, 2*time.Second), "plain subscriber should receive audio")
		plain := plainSink.snapshot()
		if len(plain) >= 4 {
			c.Check(plain[0].Type == av.Metadata && bytesEqual(plain[0].Data, []byte("meta")),
				"a writer mutating its packets must not corrupt another subscriber's metadata")
			c.Check(plain[1].Type == av.H264DecoderConfig && bytesEqual(plain[1].Data, []byte{1, 2}),
				"a writer mutating its packets must not corrupt another subscriber's sequence header")
			c.Check(plain[2].Type == av.H264 && bytesEqual(plain[2].Data, []byte{7, 8}),
				"a writer mutating its packets must not corrupt another subscriber's video")
			c.Check(plain[3].Type == av.AAC && bytesEqual(plain[3].Data, []byte{9}),
				"a writer mutating its packets must not corrupt another subscriber's audio")
		}
		c.Check(mutSub.stop() == nil, "mutating subscriber should stop cleanly")
		c.Check(plainSub.stop() == nil, "plain subscriber should stop cleanly")

		// A later subscriber gets its own anchor; one subscriber cannot push
		// another's cursor.
		clk5 := newTestClock()
		s5 := &PubSub{}
		feed6 := newPacketFeed()
		defer feed6.close()
		startPublication(s5, feed6)
		feed6.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 0, Data: []byte{0x41}})
		feed6.send(av.Packet{Type: av.AAC, Time: 10 * time.Millisecond, Data: []byte{0x42}})
		c.Check(waitCacheIdx(s5, 2), "isolation publisher should cache first GOP")

		firstSink := newTestSink()
		firstSub := startTimedSub(s5, PlaybackOptions{MaxLag: 50 * time.Millisecond, Clock: clk5}, firstSink)
		c.Check(firstSink.waitCount(1, 2*time.Second), "first subscriber should start at the keyframe")
		clk5.Advance(10 * time.Millisecond)
		c.Check(firstSink.waitCount(2, 2*time.Second), "first subscriber should advance independently")

		feed6.send(av.Packet{Type: av.H264, IsKeyFrame: true, Time: 20 * time.Millisecond, Data: []byte{0x43}})
		feed6.send(av.Packet{Type: av.AAC, Time: 30 * time.Millisecond, Data: []byte{0x44}})
		c.Check(waitCacheIdx(s5, 4), "isolation publisher should cache the second GOP")

		secondSink := newTestSink()
		secondSub := startTimedSub(s5, PlaybackOptions{MaxLag: 50 * time.Millisecond, Clock: clk5}, secondSink)
		c.Check(secondSink.waitCount(1, 2*time.Second), "late subscriber should start from the cached keyframe")
		secondPk := secondSink.snapshot()
		if len(secondPk) >= 1 {
			c.Check(bytesEqual(secondPk[0].Data, []byte{0x43}),
				"late subscriber must start at the newest keyframe")
			c.Check(secondPk[0].Time == 0,
				"late subscriber must anchor its first video at Time 0, got %v", secondPk[0].Time)
		}

		clk5.Advance(10 * time.Millisecond)
		c.Check(firstSink.waitCount(3, 2*time.Second), "first subscriber should continue with the new GOP")
		c.Check(secondSink.waitCount(2, 2*time.Second), "late subscriber should follow its own anchor")
		firstPk := firstSink.snapshot()
		if len(firstPk) >= 3 {
			c.Check(firstPk[2].Time == 20*time.Millisecond,
				"first subscriber's new keyframe should be Time 20ms, got %v", firstPk[2].Time)
			c.Check(bytesEqual(firstPk[2].Data, []byte{0x43}), "first subscriber should get the new keyframe")
		}
		secondPk = secondSink.snapshot()
		if len(secondPk) >= 2 {
			c.Check(secondPk[1].Time == 10*time.Millisecond,
				"late subscriber's audio should be Time 10ms under its own anchor, got %v", secondPk[1].Time)
			c.Check(bytesEqual(secondPk[1].Data, []byte{0x44}), "late subscriber should get the new audio")
		}
		c.Check(firstSub.stop() == nil, "first isolation subscriber should stop cleanly")
		c.Check(secondSub.stop() == nil, "late isolation subscriber should stop cleanly")
	})
}
