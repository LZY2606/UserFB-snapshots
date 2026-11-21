package rtmp

import (
	"bytes"
	"fmt"
	"math"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/gwuhaolin/livego/av"
	"github.com/gwuhaolin/livego/configure"
	"github.com/gwuhaolin/livego/container/flv"
	"github.com/gwuhaolin/livego/protocol/amf"
	"github.com/gwuhaolin/livego/protocol/rtmp/cache"
	"github.com/gwuhaolin/livego/protocol/rtmp/core"
	"github.com/gwuhaolin/livego/task_support/taskcheck"
)

// ---- packet builders -------------------------------------------------------

func videoPacket(ts uint32, key, seq bool, label byte, composition int32) av.Packet {
	flags := byte(0x27) // inter frame, AVC
	if key {
		flags = 0x17 // key frame, AVC
	}
	packetType := byte(av.AVC_NALU)
	if seq {
		packetType = av.AVC_SEQHDR
	}
	data := []byte{flags, packetType,
		byte(composition >> 16), byte(composition >> 8), byte(composition), label}
	p := av.Packet{IsVideo: true, TimeStamp: ts, StreamID: 1, Data: data}
	if err := flv.NewDemuxer().DemuxH(&p); err != nil {
		panic(err)
	}
	return p
}

func aacSeqPacket(ts uint32, label byte) av.Packet {
	p := av.Packet{IsAudio: true, TimeStamp: ts, StreamID: 1,
		Data: []byte{0xAF, av.AAC_SEQHDR, label}}
	if err := flv.NewDemuxer().DemuxH(&p); err != nil {
		panic(err)
	}
	return p
}

func aacPacket(ts uint32, label byte) av.Packet {
	p := av.Packet{IsAudio: true, TimeStamp: ts, StreamID: 1,
		Data: []byte{0xAF, av.AAC_RAW, label}}
	if err := flv.NewDemuxer().DemuxH(&p); err != nil {
		panic(err)
	}
	return p
}

func metadataPacket(ts uint32, label byte) av.Packet {
	var buf bytes.Buffer
	enc := &amf.Encoder{}
	if _, err := enc.Encode(&buf, "onMetaData", amf.AMF0); err != nil {
		panic(err)
	}
	obj := amf.Object{"label": string([]byte{label})}
	if _, err := enc.Encode(&buf, obj, amf.AMF0); err != nil {
		panic(err)
	}
	return av.Packet{IsMetadata: true, TimeStamp: ts, StreamID: 1, Data: buf.Bytes()}
}

func packetLabel(p av.Packet) byte {
	if p.IsMetadata {
		dec := &amf.Decoder{}
		// DecodeBatch reports io.EOF after a successful decode.
		vs, _ := dec.DecodeBatch(bytes.NewReader(p.Data), amf.AMF0)
		if len(vs) < 2 {
			return 0
		}
		obj, ok := vs[1].(amf.Object)
		if !ok {
			return 0
		}
		s, _ := obj["label"].(string)
		if len(s) != 1 {
			return 0
		}
		return s[0]
	}
	if len(p.Data) == 0 {
		return 0
	}
	return p.Data[len(p.Data)-1]
}

// ---- recording writer ------------------------------------------------------

type recordSink struct {
	av.RWBaser
	mu      sync.Mutex
	packets []av.Packet
	closed  bool
	err     error
}

func newRecordSink() *recordSink {
	return &recordSink{RWBaser: av.NewRWBaser(time.Hour)}
}

func (s *recordSink) Info() av.Info      { return av.Info{UID: "sink"} }
func (s *recordSink) CalcBaseTimestamp() {}

func (s *recordSink) Close(err error) {
	s.mu.Lock()
	s.closed = true
	s.err = err
	s.mu.Unlock()
}

func (s *recordSink) Write(p *av.Packet) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	q := *p
	q.Data = append([]byte(nil), p.Data...)
	s.packets = append(s.packets, q)
	return nil
}

func (s *recordSink) labels() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	var b bytes.Buffer
	for _, p := range s.packets {
		b.WriteByte(packetLabel(p))
	}
	return b.String()
}

func (s *recordSink) timestamps() []uint32 {
	s.mu.Lock()
	defer s.mu.Unlock()
	ret := make([]uint32, 0, len(s.packets))
	for _, p := range s.packets {
		ret = append(ret, p.TimeStamp)
	}
	return ret
}

// ---- fake connection for VirWriter / ConnServer ----------------------------

// playConn implements core.StreamReadWriteCloser. Read blocks until Close so
// the VirWriter's reader goroutine stays idle.
type playConn struct {
	info    av.Info
	start   uint32
	request bool
	closed  chan struct{}
	once    sync.Once
	written chan core.ChunkStream
	mu      sync.Mutex
	err     error
}

func newPlayConn(key string, start uint32, request bool) *playConn {
	return &playConn{
		info:    av.Info{UID: "play-" + key, Key: key, URL: "rtmp://localhost/" + key, Inter: true},
		start:   start,
		request: request,
		closed:  make(chan struct{}),
		written: make(chan core.ChunkStream, 4096),
	}
}

func (c *playConn) GetInfo() (string, string, string) { return "", "", c.info.URL }
func (c *playConn) PlaybackStart() (uint32, bool)     { return c.start, c.request }
func (c *playConn) Close(err error) {
	c.mu.Lock()
	if c.err == nil {
		c.err = err
	}
	c.mu.Unlock()
	c.once.Do(func() { close(c.closed) })
}

func (c *playConn) closeErr() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.err
}

func (c *playConn) isClosed() bool {
	select {
	case <-c.closed:
		return true
	default:
		return false
	}
}

func (c *playConn) Write(cs core.ChunkStream) error {
	select {
	case c.written <- cs:
	case <-c.closed:
	}
	return nil
}

func (c *playConn) Read(*core.ChunkStream) error {
	<-c.closed
	return fmt.Errorf("closed")
}

// Flush matches the method VirWriter looks up reflectively.
func (c *playConn) Flush() error { return nil }

func (c *playConn) drain() []core.ChunkStream {
	var ret []core.ChunkStream
	for {
		select {
		case cs := <-c.written:
			ret = append(ret, cs)
		default:
			return ret
		}
	}
}

// writeAMF encodes a command into a ChunkStream as a play request.
func writeAMF(streamID uint32, values ...interface{}) core.ChunkStream {
	var buf bytes.Buffer
	enc := &amf.Encoder{}
	for _, v := range values {
		if _, err := enc.Encode(&buf, v, amf.AMF0); err != nil {
			panic(err)
		}
	}
	return core.ChunkStream{
		Format: 0, CSID: 3, TypeID: 20, StreamID: streamID, Data: buf.Bytes(),
	}
}

// ---- helpers ---------------------------------------------------------------

func withGopNum(c *taskcheck.C, n int) {
	// Each test group sets the window size before building its cache; the
	// value is restored once the whole group is done via the outermost call.
	if configure.Config.GetInt("gop_num") != n {
		configure.Config.Set("gop_num", n)
	}
}

// buildTwoConfigStream produces a retained window with two GOPs, a second
// configuration switch and AAC media. The returned packets are the authored
// publication order.
func buildTwoConfigStream() []av.Packet {
	return []av.Packet{
		metadataPacket(0, 'M'),
		videoPacket(0, true, true, 'S', 0),
		aacSeqPacket(0, 'A'),
		videoPacket(0, true, false, 'K', 40),
		aacPacket(0, 'a'),
		videoPacket(100, false, false, 'x', 10),
		// A second configuration, still inside the first growing GOP.
		videoPacket(100, true, true, 'T', 0),
		aacSeqPacket(100, 'B'),
		videoPacket(100, false, false, 'y', 0),
		videoPacket(1000, true, false, 'L', 30),
		aacPacket(1000, 'b'),
		videoPacket(1100, false, false, 'z', 0),
	}
}

func cacheFromPackets(packets []av.Packet) *cache.Cache {
	c := cache.NewCache()
	for _, p := range packets {
		c.Write(p)
	}
	return c
}

func newSeededStream(packets []av.Packet) *Stream {
	s := NewStream()
	s.info = av.Info{Key: "live/movie"}
	for _, p := range packets {
		s.cache.Write(p)
	}
	return s
}

// memConn is a net.Conn that captures server writes and reports EOF on read.
type memConn struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (m *memConn) Read([]byte) (int, error) { return 0, fmt.Errorf("eof") }
func (m *memConn) Write(p []byte) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.buf.Write(p)
}
func (m *memConn) Close() error                     { return nil }
func (m *memConn) LocalAddr() net.Addr              { return memAddr{} }
func (m *memConn) RemoteAddr() net.Addr             { return memAddr{} }
func (m *memConn) SetDeadline(time.Time) error      { return nil }
func (m *memConn) SetReadDeadline(time.Time) error  { return nil }
func (m *memConn) SetWriteDeadline(time.Time) error { return nil }

type memAddr struct{}

func (memAddr) Network() string { return "mem" }
func (memAddr) String() string  { return "mem" }

func newTestConnServer() *core.ConnServer {
	return core.NewConnServer(core.NewConn(&memConn{}, 1024))
}

func mustStart(c *taskcheck.C, start interface{}) (uint32, bool, error) {
	conn := newTestConnServer()
	values := []interface{}{"play", float64(0), nil, "movie"}
	if start != omitted {
		values = append(values, start)
	}
	chunk := writeAMF(1, values...)
	err := conn.HandleCmdMsgForTest(&chunk)
	got, on := conn.PlaybackStart()
	return got, on, err
}

// omitted marks a play request without a start argument.
type omittedType struct{}

var omitted = omittedType{}

// ---- TestReplayCommand -----------------------------------------------------

func TestReplayCommand(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		testPlaybackStartConversion(c)
		testPlaybackStartRejects(c)
		testAMFPlayReachesStream(c)
	})
}

func testPlaybackStartConversion(c *taskcheck.C) {
	cases := []struct {
		start interface{}
		want  uint32
		on    bool
	}{
		{float64(0), 0, true},
		{float64(0.0019), 1, true},
		{float64(2.5), 2500, true},
		{float64(4294967.295), 4294967295, true},
		{float64(-1), 0, false},
		{float64(-2), 0, false},
		{omitted, 0, false},
	}
	for _, tc := range cases {
		got, on, err := mustStart(c, tc.start)
		c.Check(err == nil, "play start %v returned error: %v", tc.start, err)
		c.Check(on == tc.on, "play start %v requested=%v want %v", tc.start, on, tc.on)
		if tc.on {
			c.Check(got == tc.want, "play start %v -> %d ms, want %d", tc.start, got, tc.want)
		}
	}

	// The transaction id is still parsed as before.
	conn := newTestConnServer()
	chunk := writeAMF(1, "play", float64(7), nil, "movie", float64(1.5))
	if err := conn.HandleCmdMsgForTest(&chunk); err != nil {
		c.Check(false, "play with valid transaction id: %v", err)
	}
	c.Check(conn.TransactionIDForTest() == 7, "transaction id %d, want 7", conn.TransactionIDForTest())
}

func testPlaybackStartRejects(c *taskcheck.C) {
	invalid := []interface{}{
		math.Inf(1),
		math.Inf(-1),
		math.NaN(),
		float64(-3),
		float64(-0.5),
		float64(4294967.296), // 4294967296 ms, one past uint32
		"soon",
		nil, // explicitly present null start is not a finite number
	}
	for _, bad := range invalid {
		conn := newTestConnServer()
		chunk := writeAMF(1, "play", float64(0), nil, "movie", bad)
		err := conn.HandleCmdMsgForTest(&chunk)
		c.Check(err != nil, "invalid start %v must be rejected", bad)
		_, on := conn.PlaybackStart()
		c.Check(!on, "rejected start %v must not request replay", bad)
		c.Check(conn.TransactionIDForTest() == 0,
			"rejected start %v must not become transaction id, got %d", bad, conn.TransactionIDForTest())
	}
}

// testAMFPlayReachesStream drives the real AMF play command through a
// ConnServer, then hands the resulting VirWriter to the stream dispatcher and
// verifies the player receives rebased cached media.
func testAMFPlayReachesStream(c *taskcheck.C) {
	withGopNum(c, 2)
	s := newSeededStream(buildTwoConfigStream())

	server := newTestConnServer()
	chunk := writeAMF(1, "play", float64(0), nil, "movie", float64(1))
	if err := server.HandleCmdMsgForTest(&chunk); err != nil {
		c.Check(false, "AMF play command failed: %v", err)
		return
	}
	start, requested := server.PlaybackStart()
	c.Check(requested && start == 1000, "AMF play start (1s) -> (%d,%v)", start, requested)

	conn := newPlayConn("live/movie", start, requested)
	vw := NewVirWriter(conn)
	c.Check(func() bool { s, o := vw.PlaybackStart(); return o && s == 1000 }(),
		"VirWriter carries the playback request")

	s.AddWriter(vw)
	// One published packet triggers seeding and then live delivery.
	s.writePacket(videoPacket(1200, false, false, 'n', 0))
	waitIdle(c, vw)
	vw.Close(fmt.Errorf("done"))

	packets := conn.drain()
	c.Check(len(packets) > 0, "AMF play path delivered %d packets", len(packets))
	if len(packets) > 0 {
		// The selected GOP's config head (metadata, AVC seq, AAC seq) precedes
		// media, and the keyframe is rebased to zero.
		c.Check(packets[0].TypeID == av.TAG_SCRIPTDATAAMF0, "first packet is metadata, type %d", packets[0].TypeID)
		c.Check(packets[0].Timestamp == 0, "first delivered packet starts at 0, got %d", packets[0].Timestamp)
		var media *core.ChunkStream
		for i := range packets {
			if packets[i].TypeID != av.TAG_VIDEO {
				continue
			}
			if len(packets[i].Data) > 1 && packets[i].Data[1] == av.AVC_SEQHDR {
				continue
			}
			media = &packets[i]
			break
		}
		c.Check(media != nil, "AMF play path delivered no video")
		if media != nil {
			c.Check(media.Timestamp == 0, "selected keyframe rebased to 0, got %d", media.Timestamp)
			c.Check(len(media.Data) > 0 && media.Data[len(media.Data)-1] == 'L',
				"selected keyframe is L, got %q", media.Data)
		}
	}
}

// ---- TestReplayGOPs --------------------------------------------------------

func TestReplayGOPs(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		testReplaySelection(c)
		testReplayConfigHead(c)
		testReplayIsolation(c)
		testReplayEviction(c)
	})
}

func testReplaySelection(c *taskcheck.C) {
	withGopNum(c, 3)
	cc := cacheFromPackets(buildTwoConfigStream())

	sink := newRecordSink()
	offset, err := cc.SendFrom(sink, 500, true)
	c.Check(err == nil, "replay start 500ms: %v", err)
	c.Check(offset == 0, "replay start 500ms offset %d, want 0", offset)
	labels := sink.labels()
	c.Check(len(labels) > 0 && labels[0] == 'M', "config head starts with metadata, got %q", labels)
	c.Check(bytes.IndexByte([]byte(labels), 'K') == 3,
		"start 500 selects first keyframe K after the three config packets, got %q", labels)
	if len(sink.packets) > 3 {
		c.Check(sink.packets[3].TimeStamp == 0, "selected keyframe rebased to 0, got %d", sink.packets[3].TimeStamp)
	}
	// Every media timestamp is shifted by the same offset.
	for i, p := range sink.packets {
		if i > 0 {
			c.Check(p.TimeStamp >= sink.packets[i-1].TimeStamp,
				"replay timestamps not ordered at %d: %v", i, sink.timestamps())
		}
	}

	sink = newRecordSink()
	offset, err = cc.SendFrom(sink, 1000, true)
	c.Check(err == nil && offset == 1000, "start 1000 offset %d err %v", offset, err)
	labels = sink.labels()
	c.Check(len(labels) > 3 && labels[3] == 'L',
		"start 1000 selects L after its config head, got %q", labels)
	if len(sink.packets) > 3 {
		c.Check(sink.packets[3].TimeStamp == 0, "start 1000 keyframe ts %d", sink.packets[3].TimeStamp)
	}
	// The replay ends exactly at the newest cached media time.
	if n := len(sink.packets); n > 0 {
		wantLast := uint32(1100) - offset
		c.Check(sink.packets[n-1].TimeStamp == wantLast,
			"replay tail ts %d, want %d", sink.packets[n-1].TimeStamp, wantLast)
	}
	// A start before the earliest keyframe selects the earliest.
	sink = newRecordSink()
	offset, err = cc.SendFrom(sink, 0, true)
	c.Check(err == nil && offset == 0, "start 0 offset %d err %v", offset, err)
}

func testReplayConfigHead(c *taskcheck.C) {
	withGopNum(c, 2)
	cc := cacheFromPackets(buildTwoConfigStream())

	// Starting at the second GOP emits that GOP's config first: metadata M,
	// the newer video seq T, the newer audio seq B, then keyframe L.
	sink := newRecordSink()
	if _, err := cc.SendFrom(sink, 1000, true); err != nil {
		c.Check(false, "replay second gop: %v", err)
		return
	}
	labels := sink.labels()
	if len(labels) < 4 {
		c.Check(false, "second gop replay too short: %q", labels)
		return
	}
	c.Check(labels[0] == 'M', "config order metadata first, got %q", labels)
	c.Check(labels[1] == 'T', "config order newer video seq second, got %q", labels)
	c.Check(labels[2] == 'B', "config order newer audio seq third, got %q", labels)
	c.Check(labels[3] == 'L', "media keyframe after config, got %q", labels)

	// Starting at the first GOP replays the mid-GOP switch: T and B come out
	// after the media already buffered in that GOP and before the media of the
	// following GOP, keyframe L.
	sink = newRecordSink()
	if _, err := cc.SendFrom(sink, 0, true); err != nil {
		c.Check(false, "replay first gop: %v", err)
		return
	}
	labels = sink.labels()
	ti := bytes.IndexByte([]byte(labels), 'T')
	bi := bytes.IndexByte([]byte(labels), 'B')
	li := bytes.LastIndexByte([]byte(labels), 'L')
	yi := bytes.IndexByte([]byte(labels), 'y')
	c.Check(ti >= 0 && bi >= 0 && li >= 0 && yi >= 0, "mid-gop config missing from %q", labels)
	if ti >= 0 && bi >= 0 && li >= 0 && yi >= 0 {
		// The inline audio sequence header B precedes the media y that
		// followed it, and the next GOP's snapshot T precedes keyframe L.
		c.Check(bi < yi, "inline config precedes its following media in %q", labels)
		c.Check(ti < li, "next gop config precedes its keyframe in %q", labels)
	}
	// GOP 1 keeps its original configuration: T and B do not appear in the
	// group's own config head, and the anchor keyframe K still comes after
	// the original S/A.
	c.Check(bytes.IndexByte([]byte(labels), 'K') == 3,
		"first gop head is M,S,A,K, got %q", labels)

	// Composition time and compressed bytes survive replay untouched.
	sink = newRecordSink()
	if _, err := cc.SendFrom(sink, 0, true); err != nil {
		c.Check(false, "replay composition: %v", err)
		return
	}
	wantComposition := map[byte]int32{'K': 40, 'x': 10, 'y': 0, 'z': 0, 'L': 30}
	for _, p := range sink.packets {
		if !p.IsVideo || len(p.Data) < 2 || p.Data[1] != av.AVC_NALU {
			continue
		}
		vh := p.Header.(av.VideoPacketHeader)
		if want, ok := wantComposition[packetLabel(p)]; ok {
			c.Check(vh.CompositionTime() == want,
				"composition time for %c is %d, want %d", packetLabel(p), vh.CompositionTime(), want)
		}
	}

	// AAC media stays in the GOP it was published in.
	sink = newRecordSink()
	if _, err := cc.SendFrom(sink, 1000, true); err != nil {
		c.Check(false, "replay aac: %v", err)
		return
	}
	c.Check(bytes.IndexByte([]byte(sink.labels()), 'b') > 0,
		"AAC media retained in second gop: %q", sink.labels())
}

func testReplayIsolation(c *taskcheck.C) {
	withGopNum(c, 3)
	shared := buildTwoConfigStream()
	cc := cacheFromPackets(shared)

	// Two viewers with different starts rebase independently from one cache.
	a := newRecordSink()
	b := newRecordSink()
	if _, err := cc.SendFrom(a, 0, true); err != nil {
		c.Check(false, "viewer A replay: %v", err)
	}
	if _, err := cc.SendFrom(b, 1000, true); err != nil {
		c.Check(false, "viewer B replay: %v", err)
	}
	c.Check(len(a.timestamps()) > 0 && a.timestamps()[0] == 0, "viewer A starts at zero")
	c.Check(len(b.timestamps()) > 0 && b.timestamps()[0] == 0, "viewer B starts at zero")
	c.Check(bytes.IndexByte([]byte(a.labels()), 'K') == 3 && bytes.IndexByte([]byte(b.labels()), 'L') == 3,
		"viewers select different keyframes: %q %q", a.labels(), b.labels())
	for _, v := range [][]uint32{a.timestamps(), b.timestamps()} {
		for i := 1; i < len(v); i++ {
			c.Check(v[i] >= v[i-1], "viewer timestamps not continuous: %v", v)
		}
	}

	// Mutating the publisher's input Data after buffering must not affect the
	// retained copies.
	mutated := shared[3]
	for i := range mutated.Data {
		mutated.Data[i] = 0xEE
	}
	after := newRecordSink()
	if _, err := cc.SendFrom(after, 0, true); err != nil {
		c.Check(false, "replay after input mutation: %v", err)
	}
	c.Check(bytes.IndexByte([]byte(after.labels()), 'K') >= 0,
		"cache polluted by publisher reuse of input Data: %q", after.labels())
	c.Check(after.labels() == a.labels(), "input mutation changed cached output: %q vs %q", after.labels(), a.labels())

	// A downstream writer mutating its packets must not pollute another.
	mutating := &mutatingSink{}
	if _, err := cc.SendFrom(mutating, 0, true); err != nil {
		c.Check(false, "mutating viewer replay: %v", err)
	}
	clean := newRecordSink()
	if _, err := cc.SendFrom(clean, 0, true); err != nil {
		c.Check(false, "clean viewer replay: %v", err)
	}
	c.Check(clean.labels() == after.labels(), "downstream mutation leaked: %q vs %q", clean.labels(), after.labels())
}

type mutatingSink struct{ av.RWBaser }

func (m *mutatingSink) Info() av.Info      { return av.Info{} }
func (m *mutatingSink) Close(error)        {}
func (m *mutatingSink) CalcBaseTimestamp() {}
func (m *mutatingSink) Write(p *av.Packet) error {
	for i := range p.Data {
		p.Data[i] = 0xDD
	}
	return nil
}

func testReplayEviction(c *taskcheck.C) {
	withGopNum(c, 3)
	cc := cacheFromPackets([]av.Packet{
		videoPacket(0, true, true, 'S', 0),
		videoPacket(0, true, false, '1', 0),
		videoPacket(100, true, false, '2', 0),
		videoPacket(200, true, false, '3', 0),
		videoPacket(300, true, false, '4', 0),
		videoPacket(400, true, false, '5', 0),
	})
	// Three slots retain GOPs 3, 4, 5; GOPs 1 and 2 are evicted. The newest
	// media time is 400, so 450 is past the end.
	for _, start := range []uint32{0, 100, 199} {
		sink := newRecordSink()
		_, err := cc.SendFrom(sink, start, true)
		c.Check(err == cache.ErrPlaybackRange, "evicted start %d error %v", start, err)
		c.Check(len(sink.packets) == 0, "evicted start %d wrote media", start)
	}
	for _, tc := range []struct {
		start  uint32
		label  byte
		offset uint32
	}{
		{200, '3', 200},
		{250, '3', 200},
		{300, '4', 300},
		{400, '5', 400},
	} {
		sink := newRecordSink()
		offset, err := cc.SendFrom(sink, tc.start, true)
		c.Check(err == nil, "retained start %d error %v", tc.start, err)
		c.Check(offset == tc.offset, "retained start %d offset %d, want %d", tc.start, offset, tc.offset)
		labels := sink.labels()
		if len(labels) > 0 {
			c.Check(bytes.IndexByte([]byte(labels), tc.label) >= 0,
				"retained start %d selected %q, want keyframe %c", tc.start, labels, tc.label)
			// The first media packet (after config) is the selected keyframe.
			var firstMedia *av.Packet
			for i := range sink.packets {
				if !sink.packets[i].IsMetadata &&
					!(sink.packets[i].IsVideo && sink.packets[i].Data[1] == av.AVC_SEQHDR) &&
					!(sink.packets[i].IsAudio && sink.packets[i].Data[1] == av.AAC_SEQHDR) {
					firstMedia = &sink.packets[i]
					break
				}
			}
			c.Check(firstMedia != nil && packetLabel(*firstMedia) == tc.label,
				"retained start %d first media %v, want %c", tc.start, firstMedia, tc.label)
		}
	}
	sink := newRecordSink()
	_, err := cc.SendFrom(sink, 450, true)
	c.Check(err == cache.ErrPlaybackRange, "past-end start 450 error %v", err)
	c.Check(len(sink.packets) == 0, "past-end start 450 wrote media")
	// The newest, still-growing GOP is retained too: its config head (the
	// pending video sequence header S) plus keyframe 5.
	sink = newRecordSink()
	_, err = cc.SendFrom(sink, 400, true)
	c.Check(err == nil && len(sink.packets) == 2,
		"newest growing gop packets=%d err=%v", len(sink.packets), err)
}

// ---- TestReplayHandoff -----------------------------------------------------

func TestReplayHandoff(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		testHandoffSeam(c)
		testHandoffConcurrentPublisher(c)
		testHandoffRangeError(c)
		testHandoffHTTPFLV(c)
	})
}

func testHandoffSeam(c *taskcheck.C) {
	withGopNum(c, 2)
	s := newSeededStream(buildTwoConfigStream())
	conn := newPlayConn("live/movie", 1000, true)
	vw := NewVirWriter(conn)
	s.AddWriter(vw)

	live := []av.Packet{
		videoPacket(1200, false, false, 'n', 0),
		aacPacket(1250, 'c'),
		videoPacket(1300, false, false, 'o', 0),
	}
	for _, p := range live {
		s.writePacket(p)
	}
	waitIdle(c, vw)
	vw.Close(fmt.Errorf("done"))

	packets := conn.drain()
	// Config head comes first: metadata M, newer video seq T, newer audio seq B.
	if len(packets) < 3 {
		c.Check(false, "handoff got %d packets %v, want config head plus media", len(packets), chunkTimestamps(packets))
		return
	}
	c.Check(packets[0].TypeID == av.TAG_SCRIPTDATAAMF0, "handoff head metadata first")
	media := packets[3:]
	// Replay media: L@0, b@0, z@100. Live: 1200->200, 1250->250, 1300->300.
	want := []uint32{0, 0, 100, 200, 250, 300}
	if len(media) != len(want) {
		c.Check(false, "handoff got %d media packets %v, want %d", len(media), chunkTimestamps(media), len(want))
		return
	}
	for i := range want {
		c.Check(media[i].Timestamp == want[i],
			"handoff media packet %d timestamp %d, want %d", i, media[i].Timestamp, want[i])
	}
	// Tail of replay and first live packet meet without gap or duplicate.
	for i := 1; i < len(media); i++ {
		c.Check(media[i].Timestamp >= media[i-1].Timestamp,
			"handoff timestamps not monotonic: %v", chunkTimestamps(media))
	}
	// The live tail bytes are forwarded unchanged.
	if n := len(media); n > 0 {
		last := media[n-1]
		c.Check(len(last.Data) > 0 && last.Data[len(last.Data)-1] == 'o',
			"last live packet bytes forwarded, got %v", last.Data)
	}
}

func testHandoffConcurrentPublisher(c *taskcheck.C) {
	withGopNum(c, 3)
	cc := cacheFromPackets(buildTwoConfigStream())
	s := &Stream{cache: cc, ws: &sync.Map{}}
	conn := newPlayConn("live/movie", 0, true)
	vw := NewVirWriter(conn)
	s.AddWriter(vw)

	// A second subscriber downstream-mutates every packet it receives. The
	// publisher and the replaying viewer must be unaffected.
	mutating := &mutatingSink{}
	s.AddWriter(&fakeWriter{inner: mutating})

	// Publishing continues while the subscriber is served. Races here would
	// trip the race detector or corrupt the shared cache.
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := uint32(0); i < 50; i++ {
			s.writePacket(videoPacket(1200+i*40, i == 0, false, 'q', 0))
			s.writePacket(aacPacket(1210+i*40, 'r'))
		}
	}()
	wg.Wait()

	waitIdle(c, vw)
	vw.Close(fmt.Errorf("done"))
	packets := conn.drain()
	c.Check(len(packets) > 0, "concurrent handoff delivered no packets")
	for i := 1; i < len(packets); i++ {
		c.Check(packets[i].Timestamp >= packets[i-1].Timestamp,
			"concurrent handoff timestamps not monotonic at %d", i)
	}
	// The viewer's live tail bytes were not rewritten by the other subscriber.
	if n := len(packets); n > 0 {
		c.Check(packets[n-1].Data[len(packets[n-1].Data)-1] == 'r',
			"other subscriber's mutation leaked into live fan-out")
	}
	// The first viewer must still get the whole retained window afterwards.
	other := newRecordSink()
	if _, err := cc.SendFrom(other, 0, true); err != nil {
		c.Check(false, "concurrent publisher corrupted cache: %v", err)
	}
	c.Check(len(other.packets) > len(buildTwoConfigStream()), "cache did not keep growing during handoff")
}

func testHandoffRangeError(c *taskcheck.C) {
	withGopNum(c, 2)
	s := newSeededStream(buildTwoConfigStream())

	// Past the newest media time: subscriber is closed with a distinguishable
	// range error and receives no media.
	conn := newPlayConn("live/movie", 99999, true)
	vw := NewVirWriter(conn)
	s.AddWriter(vw)
	s.writePacket(videoPacket(1200, false, false, 'n', 0))
	c.Check(conn.isClosed(), "out-of-range subscriber was not closed")
	c.Check(len(conn.drain()) == 0, "out-of-range subscriber received media")
	c.Check(conn.closeErr() != nil, "out-of-range subscriber closed with nil error")

	// The publisher and other live subscribers keep working.
	live := newRecordSink()
	s.AddWriter(&fakeWriter{inner: live})
	s.writePacket(videoPacket(1300, false, false, 'p', 0))
	c.Check(len(live.packets) > 0, "live subscribers stopped after a range error")

	// A start before the earliest retained keyframe is also out of range.
	s2 := newSeededStream([]av.Packet{
		videoPacket(500, true, false, 'K', 0),
		videoPacket(600, false, false, 'x', 0),
	})
	conn2 := newPlayConn("live/movie", 100, true)
	vw2 := NewVirWriter(conn2)
	s2.AddWriter(vw2)
	s2.writePacket(videoPacket(700, false, false, 'y', 0))
	c.Check(conn2.closeErr() != nil, "early start subscriber closed with error")
	c.Check(len(conn2.drain()) == 0, "early start subscriber received media")
}

// fakeWriter wraps a sink as an av.WriteCloser without playback info.
type fakeWriter struct {
	inner av.WriteCloser
}

func (f *fakeWriter) Info() av.Info      { return av.Info{UID: fmt.Sprintf("live-%p", f)} }
func (f *fakeWriter) Close(error)        {}
func (f *fakeWriter) Alive() bool        { return true }
func (f *fakeWriter) CalcBaseTimestamp() {}
func (f *fakeWriter) Write(p *av.Packet) error {
	return f.inner.Write(p)
}

func testHandoffHTTPFLV(c *taskcheck.C) {
	withGopNum(c, 2)
	s := newSeededStream(buildTwoConfigStream())

	// HTTP-FLV writers are not PlaybackStarter, so they still join live and
	// receive the retained cache in its live form (metadata first).
	live := newRecordSink()
	s.AddWriter(&fakeWriter{inner: live})
	s.writePacket(videoPacket(1200, false, false, 'n', 0))
	c.Check(len(live.packets) > 0, "HTTP-FLV style subscriber got no packets")
	if len(live.packets) > 0 {
		c.Check(live.packets[0].IsMetadata, "HTTP-FLV subscriber starts with metadata")
		c.Check(live.labels()[0] == 'M', "HTTP-FLV subscriber cache head %q", live.labels())
	}
}

func chunkTimestamps(packets []core.ChunkStream) []uint32 {
	ret := make([]uint32, len(packets))
	for i, p := range packets {
		ret[i] = p.Timestamp
	}
	return ret
}

// waitIdle blocks until the VirWriter has forwarded every queued packet.
func waitIdle(c *taskcheck.C, vw *VirWriter) {
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		if len(vw.packetQueue) == 0 {
			return
		}
		time.Sleep(time.Millisecond)
	}
	c.Check(false, "timed out waiting for writer queue to drain")
}
