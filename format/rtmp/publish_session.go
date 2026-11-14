package rtmp

import (
	"fmt"
	"io"
	"time"

	"github.com/nareix/joy4/av"
	"github.com/nareix/joy4/format/flv"
	"github.com/nareix/joy4/format/flv/flvio"
)

// Publish event kinds delivered by PublishSession.ReadEvent.
const (
	// PublishEventStart is delivered once when a publish command is accepted.
	PublishEventStart = "start"
	// PublishEventHeader is delivered once when all tracks declared by the
	// stream's onMetaData have been probed.
	PublishEventHeader = "header"
	// PublishEventPacket is delivered for every cached or live media packet.
	PublishEventPacket = "packet"
	// PublishEventEnd is delivered once when the stream is closed, deleted
	// or the connection terminates.
	PublishEventEnd = "end"
)

// Sentinel errors returned for protocol violations on a multi-publish
// connection.
var (
	// ErrStreamNotCreated means media or a publish targeted a message stream
	// id that no createStream command has allocated in this session.
	ErrStreamNotCreated = fmt.Errorf("rtmp: message stream is not created")
	// ErrStreamAlreadyPublished means a second publish targeted a message
	// stream id that is already in publishing state.
	ErrStreamAlreadyPublished = fmt.Errorf("rtmp: message stream is already publishing")
	// ErrNotPublishing means media data arrived on a message stream id whose
	// publish command has not been accepted yet.
	ErrNotPublishing = fmt.Errorf("rtmp: message stream is not in publishing state")
	// ErrStreamNotFound means closeStream/deleteStream targeted an unknown id.
	ErrStreamNotFound = fmt.Errorf("rtmp: message stream not found")
)

// PublishEvent is a single lifecycle event of one published stream.
type PublishEvent struct {
	// Kind is one of start, header, packet and end.
	Kind string
	// StreamID is the message stream id from the RTMP message header.
	StreamID uint32
	// Name is the raw publish name; only set for start events.
	Name string
	// Streams holds the probed codec data; only set for header events.
	Streams []av.CodecData
	// Packet holds the media packet; only set for packet events.
	Packet av.Packet
}

// PublishSession multiplexes several concurrently published streams on a
// single RTMP connection. The connection-level handshake, chunking and AMF0
// encoding are shared with Conn; only the per-stream probe state and
// publishing state are kept per stream.
type PublishSession struct {
	conn *Conn

	events chan PublishEvent
	// release is closed when the handler has abandoned the session so that
	// emits never block forever.
	release chan struct{}

	// streams maps message stream id to per-stream state.
	streams map[uint32]*publishStreamState
	// nextid hands out fresh, never reused message stream ids.
	nextid uint32
}

type publishStreamState struct {
	id     uint32
	active bool // publish accepted, not yet ended
	// metaSeen records that an onMetaData track declaration was received.
	metaSeen bool
	// probedDelivered marks that the header event has been emitted.
	probedDelivered bool
	prober          *flv.Prober
}

// ReadEvent returns the next event of any stream of this session. When the
// connection has been closed and every already received event has been
// drained, it returns io.EOF.
func (self *PublishSession) ReadEvent() (PublishEvent, error) {
	ev, ok := <-self.events
	if !ok {
		return PublishEvent{}, io.EOF
	}
	return ev, nil
}

// Close shuts down the whole underlying RTMP connection. Events that were
// already received stay readable until ReadEvent returns io.EOF.
func (self *PublishSession) Close() error {
	return self.conn.Close()
}

func (self *Server) handlePublishSession(conn *Conn) (err error) {
	if err = conn.handshakeServer(); err != nil {
		return
	}

	var connectpath string
	var tcurl string
	var connectparams flvio.AMFMap
	if connectpath, tcurl, connectparams, err = conn.readConnectPrologue(); err != nil {
		return
	}

	session := &PublishSession{
		conn:    conn,
		events:  make(chan PublishEvent, 256),
		release: make(chan struct{}),
		streams: make(map[uint32]*publishStreamState),
		nextid:  1,
	}

	handlerDone := make(chan struct{})
	go func() {
		self.HandlePublishSession(session)
		close(handlerDone)
	}()

	loopErr := session.loop(tcurl, connectpath, connectparams)

	// The read loop has ended. If the handler has stopped draining events,
	// the bounded buffer could otherwise block the final flush; release any
	// blocked sender after a grace period. This watchdog only bounds shutdown.
	watchdog := time.AfterFunc(sessionHandlerTimeout, func() {
		select {
		case <-handlerDone:
		default:
			close(session.release)
		}
	})

	// Every still active stream gets exactly one end event; previously
	// closed streams must not produce a second one.
	for _, st := range session.streams {
		if st.active {
			session.drainStreamEnd(st)
		}
	}
	close(session.events)

	// Closing the connection unblocks any reader still stuck on the wire.
	_ = conn.Close()
	watchdog.Stop()
	<-handlerDone

	if loopErr != nil {
		err = loopErr
	}
	return
}

const sessionHandlerTimeout = 5 * time.Second

// emit hands one event to the session consumer. It only drops the event if
// the handler has provably abandoned the session.
func (self *PublishSession) emit(ev PublishEvent) {
	select {
	case self.events <- ev:
	case <-self.release:
	}
}

// loop reads every message of the connection and dispatches it by the
// message stream id carried in the message header.
func (self *PublishSession) loop(tcurl, connectpath string, connectparams flvio.AMFMap) (err error) {
	for {
		if err = self.conn.pollMsg(); err != nil {
			return
		}

		msgsid := self.conn.curmsgsid
		switch self.conn.msgtypeid {
		case msgtypeidCommandMsgAMF0, msgtypeidCommandMsgAMF3:
			if err = self.handleCommand(msgsid, tcurl, connectpath, connectparams); err != nil {
				return
			}

		case msgtypeidDataMsgAMF0:
			if err = self.handleData(msgsid); err != nil {
				return
			}

		case msgtypeidVideoMsg, msgtypeidAudioMsg:
			if err = self.handleMedia(msgsid); err != nil {
				return
			}
		}
	}
}

func (self *PublishSession) handleCommand(msgsid uint32, tcurl, connectpath string, connectparams flvio.AMFMap) (err error) {
	conn := self.conn
	switch conn.commandname {
	case "createStream":
		id := self.nextid
		self.nextid++
		self.streams[id] = &publishStreamState{
			id:     id,
			prober: &flv.Prober{},
		}
		// > _result(streamid), each with its own transaction id and a
		// fresh non-zero stream id; ids are never reused in this session.
		if err = conn.writeCommandMsg(3, 0, "_result", conn.commandtransid, nil, id); err != nil {
			return
		}
		if err = conn.flushWrite(); err != nil {
			return
		}

	case "publish":
		if len(conn.commandparams) < 1 {
			return fmt.Errorf("rtmp: publish params invalid")
		}
		publishpath, _ := conn.commandparams[0].(string)

		st := self.streams[msgsid]
		if st == nil {
			return ErrStreamNotCreated
		}
		if st.active {
			return ErrStreamAlreadyPublished
		}

		var cberr error
		if conn.OnPlayOrPublish != nil {
			cberr = conn.OnPlayOrPublish(conn.commandname, connectparams)
		}

		// > onStatus(NetStream.Publish.Start) on this stream's own id.
		if err = conn.writeCommandMsg(5, msgsid,
			"onStatus", conn.commandtransid, nil,
			flvio.AMFMap{
				"level":       "status",
				"code":        "NetStream.Publish.Start",
				"description": "Start publishing",
			},
		); err != nil {
			return
		}
		if err = conn.flushWrite(); err != nil {
			return
		}

		if cberr != nil {
			return fmt.Errorf("rtmp: OnPlayOrPublish check failed")
		}

		st.active = true
		conn.URL = createURL(tcurl, connectpath, publishpath)
		self.emit(PublishEvent{
			Kind:     PublishEventStart,
			StreamID: msgsid,
			Name:     publishpath,
		})

	case "closeStream":
		// closeStream addresses the stream of its message header.
		if err = self.endStream(msgsid); err != nil {
			return
		}

	case "deleteStream":
		// deleteStream addresses the stream id passed as its first parameter.
		if len(conn.commandparams) < 1 {
			return fmt.Errorf("rtmp: deleteStream params invalid")
		}
		idf, _ := conn.commandparams[0].(float64)
		if err = self.endStream(uint32(idf)); err != nil {
			return
		}
	}

	return
}

// endStream finishes one stream. If the stream was never published it is
// simply removed; otherwise its deliverable events are flushed and one end
// event is emitted. A second call for the same stream fails with
// ErrStreamNotFound, so a deleted stream never produces a second end.
func (self *PublishSession) endStream(id uint32) (err error) {
	st := self.streams[id]
	if st == nil {
		return ErrStreamNotFound
	}
	if st.active {
		self.drainStreamEnd(st)
	}
	delete(self.streams, id)
	return
}

// drainStreamEnd delivers every packet the prober has already cached, then
// appends a single end event.
func (self *PublishSession) drainStreamEnd(st *publishStreamState) {
	self.flushCached(st)
	self.emit(PublishEvent{
		Kind:     PublishEventEnd,
		StreamID: st.id,
	})
	st.active = false
}

// handleData parses an onMetaData (or @setDataFrame-prefixed) data message
// and records the hasAudio/hasVideo track declaration of this stream.
func (self *PublishSession) handleData(msgsid uint32) (err error) {
	st := self.streams[msgsid]
	if st == nil || !st.active {
		return ErrNotPublishing
	}

	vals := self.conn.datamsgvals
	if len(vals) == 0 {
		return nil
	}
	name, ok := vals[0].(string)
	if !ok {
		return nil
	}
	// ffmpeg style: "@setDataFrame", "onMetaData", {...}
	if name == "@setDataFrame" {
		if len(vals) < 2 {
			return nil
		}
		if name, ok = vals[1].(string); !ok || name != "onMetaData" {
			return nil
		}
		vals = vals[1:]
	}
	if name != "onMetaData" {
		return nil
	}
	if len(vals) < 2 {
		return nil
	}
	meta, ok := vals[1].(flvio.AMFMap)
	if !ok {
		return nil
	}

	hasAudio, _ := meta["hasAudio"].(bool)
	hasVideo, _ := meta["hasVideo"].(bool)
	if !hasAudio && !hasVideo {
		return fmt.Errorf("rtmp: onMetaData of stream %d declares no tracks", msgsid)
	}

	st.metaSeen = true
	st.prober.HasAudio = hasAudio
	st.prober.HasVideo = hasVideo
	return
}

// handleMedia feeds one audio or video tag into the stream's own prober and
// delivers header/packet events without ever borrowing another stream's
// identity.
func (self *PublishSession) handleMedia(msgsid uint32) (err error) {
	st := self.streams[msgsid]
	if st == nil {
		return ErrStreamNotCreated
	}
	if !st.active {
		return ErrNotPublishing
	}
	if !st.metaSeen {
		return fmt.Errorf("rtmp: media before onMetaData on stream %d", msgsid)
	}

	tag := self.conn.avtag
	timestamp := int32(self.conn.timestamp)

	if !st.prober.Probed() {
		if err = st.prober.PushTag(tag, timestamp); err != nil {
			return
		}
		if st.prober.Probed() {
			self.deliverHeader(st)
			self.flushCached(st)
		}
		return
	}

	// After probing, bypass the prober's probe-only packet counter. Config
	// tags (sequence headers) are not packets and are simply ignored here.
	if pkt, ok := st.prober.TagToPacket(tag, timestamp); ok {
		self.emit(PublishEvent{
			Kind:     PublishEventPacket,
			StreamID: st.id,
			Packet:   pkt,
		})
	}
	return
}

// flushCached emits every packet cached while probing, in receive order and
// with the stream's own track indices.
func (self *PublishSession) flushCached(st *publishStreamState) {
	if !st.prober.Probed() {
		return
	}
	if !st.probedDelivered {
		self.deliverHeader(st)
	}
	for !st.prober.Empty() {
		pkt := st.prober.PopPacket()
		self.emit(PublishEvent{
			Kind:     PublishEventPacket,
			StreamID: st.id,
			Packet:   pkt,
		})
	}
}

func (self *PublishSession) deliverHeader(st *publishStreamState) {
	streams := make([]av.CodecData, len(st.prober.Streams))
	copy(streams, st.prober.Streams)
	self.emit(PublishEvent{
		Kind:     PublishEventHeader,
		StreamID: st.id,
		Streams:  streams,
	})
	st.probedDelivered = true
}
