package cache

import (
	"bytes"
	"fmt"
	"sync"

	"github.com/gwuhaolin/livego/av"
)

var (
	maxGOPCap        int = 1024
	ErrGopTooBig         = fmt.Errorf("gop to big")
	ErrPlaybackRange     = fmt.Errorf("playback start out of retained cache range")
)

// configUpdate is a metadata / sequence header that arrived in the middle of a
// GOP. index is the position in the GOP media list it has to be emitted before.
type configUpdate struct {
	index int
	p     *av.Packet
}

// gop is one complete group of pictures. The configuration snapshots are the
// metadata / AVC sequence header / AAC sequence header that were current when
// the group's first keyframe arrived.
type gop struct {
	metadata *av.Packet
	videoSeq *av.Packet
	audioSeq *av.Packet

	media    []*av.Packet
	updates  []configUpdate
	keyframe uint32
}

type GopCache struct {
	mu       sync.Mutex
	capacity int
	head     int
	count    int
	gops     []*gop

	pendingMetadata *av.Packet
	pendingVideoSeq *av.Packet
	pendingAudioSeq *av.Packet

	started     bool
	lastMediaTs uint32
}

func NewGopCache(num int) *GopCache {
	if num < 1 {
		num = 1
	}
	return &GopCache{
		capacity: num,
		gops:     make([]*gop, num),
	}
}

func clonePacket(p *av.Packet) *av.Packet {
	if p == nil {
		return nil
	}
	q := *p
	if p.Data != nil {
		q.Data = append([]byte(nil), p.Data...)
	}
	return &q
}

func appendMedia(g *gop, p *av.Packet) {
	if len(g.media) >= maxGOPCap {
		return
	}
	g.media = append(g.media, p)
}

func (g *GopCache) current() *gop {
	if g.count == 0 {
		return nil
	}
	return g.gops[(g.head+g.count-1)%g.capacity]
}

func (g *GopCache) startGop(keyframe uint32) *gop {
	ng := &gop{
		metadata: g.pendingMetadata,
		videoSeq: g.pendingVideoSeq,
		audioSeq: g.pendingAudioSeq,
		keyframe: keyframe,
	}
	if g.count < g.capacity {
		g.gops[(g.head+g.count)%g.capacity] = ng
		g.count++
	} else {
		// Circular eviction: the oldest group makes room for the new one.
		g.gops[g.head] = ng
		g.head = (g.head + 1) % g.capacity
	}
	g.started = true
	return ng
}

func (g *GopCache) Write(p *av.Packet) {
	g.mu.Lock()
	defer g.mu.Unlock()

	if p == nil || p.IsMetadata {
		if p != nil {
			cp := clonePacket(p)
			g.pendingMetadata = cp
			if cur := g.current(); cur != nil {
				cur.updates = append(cur.updates, configUpdate{index: len(cur.media), p: cp})
			}
		}
		return
	}

	if p.IsVideo {
		vh, ok := p.Header.(av.VideoPacketHeader)
		if !ok || vh.CodecID() != av.VIDEO_H264 {
			// The replay cache is scoped to H.264 video.
			return
		}
		if vh.IsSeq() {
			// A new video decoder configuration always precedes the next
			// keyframe, so it becomes that GOP's snapshot. It must never be
			// attached to already buffered pictures.
			g.pendingVideoSeq = clonePacket(p)
			return
		}
		if vh.IsKeyFrame() {
			cur := g.startGop(p.TimeStamp)
			appendMedia(cur, clonePacket(p))
			g.lastMediaTs = p.TimeStamp
			return
		}
		if !g.started {
			return
		}
		if cur := g.current(); cur != nil {
			appendMedia(cur, clonePacket(p))
			g.lastMediaTs = p.TimeStamp
		}
		return
	}

	if p.IsAudio {
		ah, ok := p.Header.(av.AudioPacketHeader)
		if !ok {
			return
		}
		if ah.SoundFormat() != av.SOUND_AAC {
			return
		}
		if ah.AACPacketType() == av.AAC_SEQHDR {
			cp := clonePacket(p)
			g.pendingAudioSeq = cp
			if cur := g.current(); cur != nil {
				cur.updates = append(cur.updates, configUpdate{index: len(cur.media), p: cp})
			}
			return
		}
		// Plain AAC media stays in the GOP it was published in.
		if !g.started {
			return
		}
		if cur := g.current(); cur != nil {
			appendMedia(cur, clonePacket(p))
			g.lastMediaTs = p.TimeStamp
		}
	}
}

func (g *GopCache) chrono() []*gop {
	ret := make([]*gop, 0, g.count)
	for i := 0; i < g.count; i++ {
		ret = append(ret, g.gops[(g.head+i)%g.capacity])
	}
	return ret
}

// snapshot returns a consistent view of the retained window together with the
// last media timestamp, so a concurrent publisher cannot shift indices while a
// subscriber is being seeded. The GOP slice headers are copied so later appends
// cannot race with the replay loop.
func (g *GopCache) snapshot() ([]*gop, uint32, bool) {
	g.mu.Lock()
	defer g.mu.Unlock()
	list := g.chrono()
	for i, gp := range list {
		cp := *gp
		cp.media = append([]*av.Packet(nil), gp.media...)
		cp.updates = append([]configUpdate(nil), gp.updates...)
		list[i] = &cp
	}
	return list, g.lastMediaTs, g.started
}

type configState struct {
	metadata *av.Packet
	videoSeq *av.Packet
	audioSeq *av.Packet
}

func samePacket(a, b *av.Packet) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	return bytes.Equal(a.Data, b.Data)
}

func writePacket(w av.WriteCloser, p *av.Packet, offset uint32) error {
	q := *p
	if p.Data != nil {
		q.Data = append([]byte(nil), p.Data...)
	}
	if offset != 0 {
		// Rebase the replay on the selected keyframe. Clamp at zero so
		// configuration packets that carry timestamp 0 stay at the start.
		if q.TimeStamp >= offset {
			q.TimeStamp -= offset
		} else {
			q.TimeStamp = 0
		}
	}
	return w.Write(&q)
}

func emitConfig(w av.WriteCloser, sent *configState, slot **av.Packet, p *av.Packet, offset uint32) error {
	if p == nil || samePacket(*slot, p) {
		return nil
	}
	if err := writePacket(w, p, offset); err != nil {
		return err
	}
	*slot = p
	return nil
}

// configSlot returns the state slot matching the kind of the given config
// packet so inline configuration updates are de-duplicated correctly.
func configSlot(sent *configState, p *av.Packet) **av.Packet {
	if p.IsMetadata {
		return &sent.metadata
	}
	if p.IsVideo {
		return &sent.videoSeq
	}
	return &sent.audioSeq
}

func emitInlineConfig(w av.WriteCloser, sent *configState, p *av.Packet, offset uint32) error {
	return emitConfig(w, sent, configSlot(sent, p), p, offset)
}

func emitGroupConfig(w av.WriteCloser, sent *configState, gp *gop, offset uint32) error {
	if err := emitConfig(w, sent, &sent.metadata, gp.metadata, offset); err != nil {
		return err
	}
	if err := emitConfig(w, sent, &sent.videoSeq, gp.videoSeq, offset); err != nil {
		return err
	}
	return emitConfig(w, sent, &sent.audioSeq, gp.audioSeq, offset)
}

// SendFrom writes the retained cache to w. When requested is false it behaves
// like the historical cache send (configuration followed by every retained
// GOP). When requested is true, start is a millisecond position: the most
// recent retained keyframe at or before it is selected, its timestamp is
// rebased to zero and the rest of the cache is shifted by the same offset.
// Out-of-range positions return ErrPlaybackRange before any packet is written.
func (g *GopCache) SendFrom(w av.WriteCloser, start uint32, requested bool) (uint32, error) {
	list, lastMediaTs, started := g.snapshot()
	var offset uint32

	if requested {
		if !started || len(list) == 0 {
			return 0, ErrPlaybackRange
		}
		if start > lastMediaTs {
			return 0, ErrPlaybackRange
		}
		selected := -1
		for i := len(list) - 1; i >= 0; i-- {
			if list[i].keyframe <= start {
				selected = i
				break
			}
		}
		if selected < 0 {
			return 0, ErrPlaybackRange
		}
		offset = list[selected].keyframe
		list = list[selected:]
	}

	if len(list) == 0 {
		return offset, nil
	}

	var sent configState
	for _, gp := range list {
		if err := emitGroupConfig(w, &sent, gp, offset); err != nil {
			return offset, err
		}

		next := 0
		for i := 0; i < len(gp.media); i++ {
			for next < len(gp.updates) && gp.updates[next].index == i {
				if err := emitInlineConfig(w, &sent, gp.updates[next].p, offset); err != nil {
					return offset, err
				}
				next++
			}
			if err := writePacket(w, gp.media[i], offset); err != nil {
				return offset, err
			}
		}
		for ; next < len(gp.updates); next++ {
			if err := emitInlineConfig(w, &sent, gp.updates[next].p, offset); err != nil {
				return offset, err
			}
		}
	}

	return offset, nil
}

func (g *GopCache) Send(w av.WriteCloser) error {
	_, err := g.SendFrom(w, 0, false)
	return err
}
