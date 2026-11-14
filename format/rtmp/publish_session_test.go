package rtmp

import (
	"bytes"
	"io"
	"net"
	"testing"
	"time"

	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/codec/aacparser"
	"github.com/nareix/joy4/codec/h264parser"
	"github.com/nareix/joy4/format/flv/flvio"
	"github.com/nareix/joy4/task_support/taskcheck"
	"github.com/nareix/joy4/utils/bits/pio"
)

// memRTMP is a pair of Conn endpoints joined by net.Pipe. The server side is
// driven by the production Server code while the test side builds raw RTMP
// chunks, exercising the real handshake, chunking and AMF0 code paths.
type memRTMP struct {
	client  *Conn
	server  *Conn
	srvDone chan error
}

func newMemRTMP() *memRTMP {
	c1, c2 := net.Pipe()
	server := NewConn(c1)
	server.isserver = true
	client := NewConn(c2)
	return &memRTMP{client: client, server: server, srvDone: make(chan error, 4)}
}

// writeRaw sends one complete message, splitting it into chunks of the
// client endpoint's current outbound chunk size. The type-0 header always
// carries the full message length even when the payload spans several
// chunks.
func (m *memRTMP) writeRaw(csid byte, sid uint32, typ byte, ms uint32, body []byte) {
	hdr := make([]byte, 12)
	hdr[0] = csid & 0x3f
	pio.PutU24BE(hdr[1:4], ms)
	pio.PutU24BE(hdr[4:7], uint32(len(body)))
	hdr[7] = typ
	pio.PutU32LE(hdr[8:12], sid)

	size := m.client.writeMaxChunkSize
	off := 0
	first := true
	for off < len(body) {
		n := len(body) - off
		if n > size {
			n = size
		}
		if first {
			_, _ = m.client.bufw.Write(hdr)
			first = false
		} else {
			// Type-3 chunk header on the same chunk stream id.
			_, _ = m.client.bufw.Write([]byte{0xc0 | (csid & 0x3f)})
		}
		_, _ = m.client.bufw.Write(body[off : off+n])
		off += n
	}
	_ = m.client.bufw.Flush()
}

func amf0Bytes(vals ...interface{}) []byte {
	n := 0
	for _, v := range vals {
		n += flvio.LenAMF0Val(v)
	}
	b := make([]byte, n)
	off := 0
	for _, v := range vals {
		off += flvio.FillAMF0Val(b[off:], v)
	}
	return b
}

func (m *memRTMP) command(csid byte, sid uint32, name string, trans float64, args ...interface{}) {
	vals := append([]interface{}{name, trans, nil}, args...)
	m.writeRaw(csid, sid, msgtypeidCommandMsgAMF0, 0, amf0Bytes(vals...))
}

func (m *memRTMP) data(csid byte, sid uint32, vals ...interface{}) {
	m.writeRaw(csid, sid, msgtypeidDataMsgAMF0, 0, amf0Bytes(vals...))
}

// setChunkSize sends a connection-level SetChunkSize message and updates the
// client endpoint's own outbound chunk size to match.
func (m *memRTMP) setChunkSize(size int) {
	b := make([]byte, 4)
	pio.PutU32BE(b, uint32(size))
	m.writeRaw(2, 0, msgtypeidSetChunkSize, 0, b)
	m.client.writeMaxChunkSize = size
}

func (m *memRTMP) media(csid byte, sid uint32, typ byte, ms uint32, tag flvio.Tag) {
	hdr := make([]byte, flvio.MaxTagSubHeaderLength)
	n := tag.FillHeader(hdr)
	body := make([]byte, 0, n+len(tag.Data))
	body = append(body, hdr[:n]...)
	body = append(body, tag.Data...)
	m.writeRaw(csid, sid, typ, ms, body)
}

func (m *memRTMP) audio(csid byte, sid uint32, ms uint32, tag flvio.Tag) {
	m.media(csid, sid, msgtypeidAudioMsg, ms, tag)
}

func (m *memRTMP) video(csid byte, sid uint32, ms uint32, tag flvio.Tag) {
	m.media(csid, sid, msgtypeidVideoMsg, ms, tag)
}

// startSession boots the production multi-publish server loop and completes
// the full RTMP handshake plus connect command/reply exchange.
func startSession(t *testing.T) (*memRTMP, *PublishSession) {
	t.Helper()
	m := newMemRTMP()
	sessCh := make(chan *PublishSession, 1)
	server := &Server{
		HandlePublishSession: func(sess *PublishSession) {
			sessCh <- sess
		},
	}
	go func() {
		m.srvDone <- server.handlePublishSession(m.server)
	}()

	if err := m.client.handshakeClient(); err != nil {
		t.Fatalf("handshake: %v", err)
	}
	// connect carries the command object as its third AMF0 argument rather
	// than a null marker.
	m.writeRaw(3, 0, msgtypeidCommandMsgAMF0, 0, amf0Bytes("connect", float64(1),
		flvio.AMFMap{"app": "live", "tcUrl": "rtmp://local/live"}))
	m.waitCommand(t, "_result", 1)

	select {
	case sess := <-sessCh:
		return m, sess
	case <-time.After(5 * time.Second):
		t.Fatal("HandlePublishSession was not invoked")
		return nil, nil
	}
}

func (m *memRTMP) kickDeadline() {
	_ = m.client.netconn.SetReadDeadline(time.Now().Add(5 * time.Second))
}

// waitCommand polls server messages until a command with the given name and
// transaction id arrives.
func (m *memRTMP) waitCommand(t *testing.T, name string, trans float64) {
	t.Helper()
	for {
		m.kickDeadline()
		if err := m.client.pollMsg(); err != nil {
			t.Fatalf("wait command %s trans %v: %v", name, trans, err)
		}
		if m.client.gotcommand && m.client.commandname == name && m.client.commandtransid == trans {
			return
		}
	}
}

// waitCreateStreamResult validates a createStream _result and returns the
// freshly allocated stream id.
func (m *memRTMP) waitCreateStreamResult(t *testing.T, trans float64) uint32 {
	t.Helper()
	m.waitCommand(t, "_result", trans)
	if len(m.client.commandparams) < 1 {
		t.Fatalf("createStream trans %v returned no stream id", trans)
	}
	id, _ := m.client.commandparams[0].(float64)
	return uint32(id)
}

// waitPublishStart validates a NetStream.Publish.Start onStatus reply and
// reports both its transaction id target and its message stream id.
func (m *memRTMP) waitPublishStart(t *testing.T, trans float64) (msgsid uint32, code string) {
	t.Helper()
	m.waitCommand(t, "onStatus", trans)
	if len(m.client.commandparams) < 1 {
		t.Fatalf("onStatus trans %v has no status object", trans)
	}
	obj, _ := m.client.commandparams[0].(flvio.AMFMap)
	code, _ = obj["code"].(string)
	return m.client.curmsgsid, code
}

func mustReadEvent(t *testing.T, sess *PublishSession) PublishEvent {
	t.Helper()
	ev, err := sess.ReadEvent()
	if err != nil {
		t.Fatalf("ReadEvent: %v", err)
	}
	return ev
}

// doPublish sends a publish command, drains its onStatus reply (the server
// blocks on the in-memory pipe until this is read) and then returns the
// corresponding start event together with the reply's routing fields.
func doPublish(t *testing.T, m *memRTMP, sess *PublishSession, csid byte, sid uint32, trans float64, name string) (PublishEvent, uint32, string) {
	t.Helper()
	m.command(csid, sid, "publish", trans, name)
	replySid, code := m.waitPublishStart(t, trans)
	return mustReadEvent(t, sess), replySid, code
}

func expectEOF(t *testing.T, sess *PublishSession) {
	t.Helper()
	done := make(chan error, 1)
	go func() {
		_, err := sess.ReadEvent()
		done <- err
	}()
	select {
	case err := <-done:
		if err != io.EOF {
			t.Fatalf("expected io.EOF, got %v", err)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("ReadEvent did not return io.EOF after event exhaustion")
	}
}

func (m *memRTMP) waitServerErr(t *testing.T) error {
	t.Helper()
	select {
	case err := <-m.srvDone:
		return err
	case <-time.After(5 * time.Second):
		t.Fatal("server session did not terminate")
		return nil
	}
}

func aacTag(packetType uint8, config []byte) flvio.Tag {
	return flvio.Tag{
		Type:          flvio.TAG_AUDIO,
		SoundFormat:   flvio.SOUND_AAC,
		SoundRate:     flvio.SOUND_44Khz,
		SoundSize:     flvio.SOUND_16BIT,
		SoundType:     flvio.SOUND_STEREO,
		AACPacketType: packetType,
		Data:          config,
	}
}

func h264Tag(frameType, packetType uint8, compTime int32, data []byte) flvio.Tag {
	return flvio.Tag{
		Type:            flvio.TAG_VIDEO,
		FrameType:       frameType,
		CodecID:         flvio.VIDEO_H264,
		AVCPacketType:   packetType,
		CompositionTime: compTime,
		Data:            data,
	}
}

func mustH264Record(t *testing.T) []byte {
	sps := []byte{0x67, 0x42, 0x00, 0x0a, 0xe9, 0x40, 0x40, 0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0xc8, 0x40}
	pps := []byte{0x68, 0xce, 0x38, 0x80}
	cd, err := h264parser.NewCodecDataFromSPSAndPPS(sps, pps)
	if err != nil {
		t.Fatalf("h264 config: %v", err)
	}
	return cd.AVCDecoderConfRecordBytes()
}

// TestPublishSessionLifecycle covers createStream/publish transaction and
// stream id handling, start/end ordering, closeStream vs deleteStream
// targeting, stream id non-reuse and the recognizable protocol errors.
func TestPublishSessionLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		m, sess := startSession(t)

		// Two createStream calls; each gets its own transaction id back and a
		// fresh non-zero stream id.
		m.command(3, 0, "createStream", 2)
		idA := m.waitCreateStreamResult(t, 2)
		m.command(4, 0, "createStream", 3)
		idB := m.waitCreateStreamResult(t, 3)
		c.Check(idA != 0 && idB != 0, "created stream ids must be non-zero: %d %d", idA, idB)
		c.Check(idA != idB, "created stream ids must differ: %d %d", idA, idB)

		// publish targets come from the message header, independent of CSID.
		m.command(5, idA, "publish", 10, "stream/a")
		sid, code := m.waitPublishStart(t, 10)
		c.Check(sid == idA && code == "NetStream.Publish.Start",
			"publish A reply: sid=%d want=%d code=%s", sid, idA, code)
		ev := mustReadEvent(t, sess)
		c.Check(ev.Kind == "start" && ev.StreamID == idA && ev.Name == "stream/a",
			"start A event: %+v", ev)

		m.command(5, idB, "publish", 11, "stream/b")
		sid, code = m.waitPublishStart(t, 11)
		c.Check(sid == idB && code == "NetStream.Publish.Start",
			"publish B reply: sid=%d want=%d code=%s", sid, idB, code)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "start" && ev.StreamID == idB && ev.Name == "stream/b",
			"start B event: %+v", ev)

		// closeStream acts on the message header stream id.
		m.command(5, idA, "closeStream", 12)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "end" && ev.StreamID == idA, "closeStream end A: %+v", ev)

		// deleteStream acts on the parameter stream id; header id is 0 here.
		m.command(3, 0, "deleteStream", 13, float64(idB))
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "end" && ev.StreamID == idB, "deleteStream end B: %+v", ev)

		// A new stream is allocated a fresh id, never reused within the session.
		m.command(3, 0, "createStream", 4)
		idC := m.waitCreateStreamResult(t, 4)
		c.Check(idC != 0 && idC != idA && idC != idB,
			"new stream id must not reuse old ids: %d (%d,%d)", idC, idA, idB)
		m.command(5, idC, "publish", 14, "stream/c")
		sid, code = m.waitPublishStart(t, 14)
		c.Check(sid == idC && code == "NetStream.Publish.Start",
			"publish C reply: sid=%d want=%d code=%s", sid, idC, code)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "start" && ev.StreamID == idC && ev.Name == "stream/c",
			"start C event: %+v", ev)
		m.command(5, idC, "closeStream", 15)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "end" && ev.StreamID == idC, "closeStream end C: %+v", ev)

		// Deleting an already removed stream is a recognizable error and must
		// not produce a second end event.
		m.command(3, 0, "deleteStream", 16, float64(idA))
		err := m.waitServerErr(t)
		c.Check(err == ErrStreamNotFound, "double delete error: %v", err)
		expectEOF(t, sess)
	})

	// Duplicate publish on an active stream.
	taskcheck.Run(t, func(c *taskcheck.C) {
		m, sess := startSession(t)
		m.command(3, 0, "createStream", 2)
		id := m.waitCreateStreamResult(t, 2)
		m.command(5, id, "publish", 3, "dup")
		_, code := m.waitPublishStart(t, 3)
		c.Check(code == "NetStream.Publish.Start", "first publish accepted: %s", code)
		m.command(5, id, "publish", 4, "dup-again")
		err := m.waitServerErr(t)
		c.Check(err == ErrStreamAlreadyPublished, "duplicate publish error: %v", err)

		ev := mustReadEvent(t, sess)
		c.Check(ev.Kind == "start" && ev.StreamID == id, "first start retained: %+v", ev)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "end" && ev.StreamID == id, "single end on teardown: %+v", ev)
		expectEOF(t, sess)
	})

	// publish on a stream that was never created.
	taskcheck.Run(t, func(c *taskcheck.C) {
		m, sess := startSession(t)
		m.command(5, 4242, "publish", 2, "ghost")
		err := m.waitServerErr(t)
		c.Check(err == ErrStreamNotCreated, "publish unknown stream error: %v", err)
		expectEOF(t, sess)
	})

	// Media on a created-but-unpublished stream must not borrow any identity.
	taskcheck.Run(t, func(c *taskcheck.C) {
		m, sess := startSession(t)
		m.command(3, 0, "createStream", 2)
		id := m.waitCreateStreamResult(t, 2)
		m.audio(6, id, 0, aacTag(flvio.AAC_RAW, []byte{0x01}))
		err := m.waitServerErr(t)
		c.Check(err == ErrNotPublishing, "media before publish error: %v", err)
		expectEOF(t, sess)
	})
}

// TestPublishSessionMedia verifies per-stream metadata declarations, AAC/AVC
// probing, header delivery, packet caching and exact packet field fidelity.
func TestPublishSessionMedia(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		m, sess := startSession(t)

		m.command(3, 0, "createStream", 2)
		idA := m.waitCreateStreamResult(t, 2)
		ev, sid, code := doPublish(t, m, sess, 5, idA, 3, "cam")
		c.Check(sid == idA && code == "NetStream.Publish.Start" && ev.Name == "cam",
			"publish cam reply/start: sid=%d code=%s event=%+v", sid, code, ev)

		// Stream A declares audio and video.
		m.data(4, idA, "onMetaData", flvio.AMFMap{"hasAudio": true, "hasVideo": true})

		aac44, err := aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x12, 0x10})
		if err != nil {
			t.Fatal(err)
		}
		record := mustH264Record(t)

		// Audio config then audio media while the video config is still
		// missing: the packet must be cached, not delivered or misindexed.
		m.audio(6, idA, 0, aacTag(flvio.AAC_SEQHDR, aac44.MPEG4AudioConfigBytes()))
		m.audio(6, idA, 40, aacTag(flvio.AAC_RAW, []byte{0x11, 0x22}))

		// No event may be delivered before every declared track is configured.
		m.kickDeadline()
		select {
		case ev := <-sess.events:
			t.Fatalf("unexpected early event: %+v", ev)
		default:
		}

		// Video config completes probing: one header then the cached packets.
		m.video(7, idA, 0, h264Tag(flvio.FRAME_KEY, flvio.AVC_SEQHDR, 0, record))

		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "header" && ev.StreamID == idA && len(ev.Streams) == 2,
			"header A event: %+v", ev)
		if len(ev.Streams) == 2 {
			c.Check(ev.Streams[0].Type() == av.AAC, "first track is AAC")
			c.Check(ev.Streams[1].Type() == av.H264, "second track is H264")
			audioCd, ok := ev.Streams[0].(av.AudioCodecData)
			c.Check(ok && audioCd.SampleRate() == 44100, "AAC rate 44100: %+v", ev.Streams[0])
			videoCd, ok := ev.Streams[1].(av.VideoCodecData)
			c.Check(ok && videoCd.Width() == 2048 && videoCd.Height() == 512,
				"H264 dimensions: %+v", ev.Streams[1])
		}

		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idA && ev.Packet.Idx == 0 &&
			ev.Packet.Time == 40*time.Millisecond && bytes.Equal(ev.Packet.Data, []byte{0x11, 0x22}),
			"cached audio packet: %+v", ev.Packet)
		c.Check(!ev.Packet.IsKeyFrame, "audio packet is not keyframe")

		// A video NALU after the sequence header is delivered immediately,
		// carrying the video track's own index.
		m.video(7, idA, 50, h264Tag(flvio.FRAME_KEY, flvio.AVC_NALU, 0, []byte{0xaa, 0xbb}))
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idA && ev.Packet.Idx == 1 &&
			ev.Packet.Time == 50*time.Millisecond && ev.Packet.IsKeyFrame &&
			bytes.Equal(ev.Packet.Data, []byte{0xaa, 0xbb}),
			"video packet: %+v", ev.Packet)

		// Live packets keep DTS, composition time and keyframe flag; a second
		// sequence header must not masquerade as a packet.
		m.video(7, idA, 0, h264Tag(flvio.FRAME_KEY, flvio.AVC_SEQHDR, 0, record))
		m.video(7, idA, 70, h264Tag(flvio.FRAME_INTER, flvio.AVC_NALU, 20, []byte{0xcc}))
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idA && ev.Packet.Idx == 1 &&
			ev.Packet.Time == 70*time.Millisecond &&
			ev.Packet.CompositionTime == 20*time.Millisecond &&
			!ev.Packet.IsKeyFrame && bytes.Equal(ev.Packet.Data, []byte{0xcc}),
			"live inter video packet: %+v", ev.Packet)

		// Second stream: audio only, different AAC sample rate, using the
		// @setDataFrame-prefixed metadata form.
		m.command(3, 0, "createStream", 4)
		idB := m.waitCreateStreamResult(t, 4)
		ev, sid, code = doPublish(t, m, sess, 5, idB, 5, "mic")
		c.Check(sid == idB && code == "NetStream.Publish.Start" && ev.Name == "mic",
			"publish mic reply/start: sid=%d code=%s event=%+v", sid, code, ev)
		m.data(4, idB, "@setDataFrame", "onMetaData",
			flvio.AMFMap{"hasAudio": true, "hasVideo": false})
		aac22, err := aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x13, 0x88})
		if err != nil {
			t.Fatal(err)
		}
		m.audio(6, idB, 0, aacTag(flvio.AAC_SEQHDR, aac22.MPEG4AudioConfigBytes()))
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "header" && ev.StreamID == idB && len(ev.Streams) == 1 &&
			ev.Streams[0].Type() == av.AAC, "audio-only header B: %+v", ev)
		if cd, ok := ev.Streams[0].(av.AudioCodecData); ok {
			c.Check(cd.SampleRate() == 22050, "AAC rate 22050 on stream B")
		}

		m.audio(6, idB, 30, aacTag(flvio.AAC_RAW, []byte{0x33}))
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idB && ev.Packet.Idx == 0 &&
			ev.Packet.Time == 30*time.Millisecond && bytes.Equal(ev.Packet.Data, []byte{0x33}),
			"stream B packet keeps its own track index: %+v", ev.Packet)

		m.command(5, idB, "closeStream", 6)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "end" && ev.StreamID == idB, "end B: %+v", ev)
		m.command(5, idA, "closeStream", 7)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "end" && ev.StreamID == idA, "end A: %+v", ev)

		// Both streams are gone; closing the connection ends the read loop and
		// marks the event channel exhausted without emitting any extra events.
		if err := sess.Close(); err != nil {
			t.Fatalf("session close: %v", err)
		}
		_ = m.waitServerErr(t)
		expectEOF(t, sess)
	})
}

// TestPublishSessionInterleave interleaves two streams' configs and data,
// switches chunk size mid-stream, reuses one CSID for different streams and
// verifies that closing one stream never blocks or misroutes the other.
func TestPublishSessionInterleave(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		m, sess := startSession(t)

		m.command(3, 0, "createStream", 2)
		idA := m.waitCreateStreamResult(t, 2)
		m.command(3, 0, "createStream", 3)
		idB := m.waitCreateStreamResult(t, 3)
		c.Check(idA == 1 && idB == 2, "interleave stream ids: %d %d", idA, idB)

		avStart, sid, code := doPublish(t, m, sess, 5, idA, 4, "av")
		c.Check(sid == idA && code == "NetStream.Publish.Start" && avStart.StreamID == idA,
			"publish av reply/start: sid=%d code=%s event=%+v", sid, code, avStart)
		audStart, sid, code := doPublish(t, m, sess, 5, idB, 5, "audio")
		c.Check(sid == idB && code == "NetStream.Publish.Start" && audStart.StreamID == idB,
			"publish audio reply/start: sid=%d code=%s event=%+v", sid, code, audStart)

		m.data(4, idA, "onMetaData", flvio.AMFMap{"hasAudio": true, "hasVideo": true})
		m.data(4, idB, "onMetaData", flvio.AMFMap{"hasAudio": true, "hasVideo": false})

		aac44, _ := aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x12, 0x10})
		aac22, _ := aacparser.NewCodecDataFromMPEG4AudioConfigBytes([]byte{0x13, 0x88})

		// A gets audio config but no video config yet, so it must stay silent.
		m.audio(6, idA, 0, aacTag(flvio.AAC_SEQHDR, aac44.MPEG4AudioConfigBytes()))
		// B completes probing immediately; it must not wait for A.
		m.audio(6, idB, 0, aacTag(flvio.AAC_SEQHDR, aac22.MPEG4AudioConfigBytes()))
		m.audio(6, idA, 10, aacTag(flvio.AAC_RAW, []byte{0xa1}))
		m.audio(6, idB, 10, aacTag(flvio.AAC_RAW, []byte{0xb1}))

		ev := mustReadEvent(t, sess)
		c.Check(ev.Kind == "header" && ev.StreamID == idB && len(ev.Streams) == 1,
			"B header delivered while A unprobed: %+v", ev)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idB &&
			bytes.Equal(ev.Packet.Data, []byte{0xb1}), "B packet not blocked by A: %+v", ev)
		m.kickDeadline()
		select {
		case ev := <-sess.events:
			t.Fatalf("stream A produced event before probing finished: %+v", ev)
		default:
		}

		// Completing A's video config delivers A's header and its cached audio.
		m.video(7, idA, 0, h264Tag(flvio.FRAME_KEY, flvio.AVC_SEQHDR, 0, mustH264Record(t)))
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "header" && ev.StreamID == idA && len(ev.Streams) == 2,
			"A header after B already flowing: %+v", ev)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idA && ev.Packet.Idx == 0 &&
			bytes.Equal(ev.Packet.Data, []byte{0xa1}) && ev.Packet.Time == 10*time.Millisecond,
			"A cached audio preserved: %+v", ev.Packet)

		// Switch chunk size to 64 and send a packet spanning several chunks.
		m.setChunkSize(64)
		big := make([]byte, 200)
		for i := range big {
			big[i] = byte(i)
		}
		m.video(7, idA, 90, h264Tag(flvio.FRAME_KEY, flvio.AVC_NALU, 0, big))
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idA && ev.Packet.Idx == 1 &&
			ev.Packet.IsKeyFrame && len(ev.Packet.Data) == 200 && bytes.Equal(ev.Packet.Data, big),
			"chunked video packet reassembled: len=%d", len(ev.Packet.Data))

		// The same CSID 6 must serve both streams in turn, routed by stream id.
		m.audio(6, idB, 20, aacTag(flvio.AAC_RAW, []byte{0xb2}))
		m.audio(6, idA, 20, aacTag(flvio.AAC_RAW, []byte{0xa2}))
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idB && bytes.Equal(ev.Packet.Data, []byte{0xb2}),
			"shared CSID routes to B: %+v", ev)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idA && bytes.Equal(ev.Packet.Data, []byte{0xa2}),
			"shared CSID routes to A: %+v", ev)

		// Close A: its events are already delivered, and exactly one end follows.
		m.command(5, idA, "closeStream", 6)
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "end" && ev.StreamID == idA, "end A while B alive: %+v", ev)

		// B keeps reading and writing after A is gone.
		m.audio(6, idB, 40, aacTag(flvio.AAC_RAW, []byte{0xb3}))
		ev = mustReadEvent(t, sess)
		c.Check(ev.Kind == "packet" && ev.StreamID == idB && bytes.Equal(ev.Packet.Data, []byte{0xb3}),
			"B continues after A closed: %+v", ev)

		// A new stream can still be created on the same connection.
		m.command(3, 0, "createStream", 7)
		idC := m.waitCreateStreamResult(t, 7)
		c.Check(idC != idA && idC != idB && idC != 0, "new id after close: %d", idC)

		// Connection close: every already received B event is drained, then one
		// end for B, never a second end for A.
		if err := sess.Close(); err != nil {
			t.Fatalf("session close: %v", err)
		}
		gotEndB := false
		for {
			ev, err := sess.ReadEvent()
			if err == io.EOF {
				break
			}
			c.Check(ev.StreamID != idA, "closed stream A must emit no further events: %+v", ev)
			if ev.Kind == "end" {
				c.Check(ev.StreamID == idB && !gotEndB, "single end for B on close: %+v", ev)
				gotEndB = true
			}
		}
		c.Check(gotEndB, "stream B received an end event when connection closed")
		_ = m.waitServerErr(t)
	})
}
