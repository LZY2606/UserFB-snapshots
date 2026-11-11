//
// Copyright (c) 2018- yutopp (yutopp@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at  https://www.boost.org/LICENSE_1_0.txt)
//

package rtmp

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"io"
	"io/ioutil"
	"testing"

	"github.com/stretchr/testify/require"

	"github.com/yutopp/go-rtmp/message"
)

// shortReader A reader which returns at most `max` bytes per Read call.
type shortReader struct {
	r   io.Reader
	max int
}

func (r *shortReader) Read(p []byte) (int, error) {
	if len(p) > r.max {
		p = p[:r.max]
	}
	return r.r.Read(p)
}

// aggregateTestRWC An io.ReadWriteCloser which reads from r and discards writes.
type aggregateTestRWC struct {
	r io.Reader
}

func (c *aggregateTestRWC) Read(p []byte) (int, error) {
	return c.r.Read(p)
}

func (c *aggregateTestRWC) Write(p []byte) (int, error) {
	return len(p), nil
}

func (c *aggregateTestRWC) Close() error {
	return nil
}

// aggregateDeliveryEvent A record of a call to a user handler.
type aggregateDeliveryEvent struct {
	kind    string
	ts      uint32
	payload []byte
}

// aggregateRecordingHandler Records deliveries to user handlers in order.
// It can be configured to return an error at a specific callback invocation.
type aggregateRecordingHandler struct {
	DefaultHandler

	events []aggregateDeliveryEvent

	calls   int
	failAt  int // 0-based index of the callback invocation which returns failErr. Negative disables failures.
	failErr error
}

func (h *aggregateRecordingHandler) tryFail() error {
	call := h.calls
	h.calls++
	if h.failAt >= 0 && call == h.failAt {
		return h.failErr
	}
	return nil
}

func (h *aggregateRecordingHandler) OnAudio(timestamp uint32, payload io.Reader) error {
	b, err := ioutil.ReadAll(payload)
	if err != nil {
		return err
	}
	if err := h.tryFail(); err != nil {
		return err
	}
	h.events = append(h.events, aggregateDeliveryEvent{kind: "audio", ts: timestamp, payload: b})
	return nil
}

func (h *aggregateRecordingHandler) OnVideo(timestamp uint32, payload io.Reader) error {
	b, err := ioutil.ReadAll(payload)
	if err != nil {
		return err
	}
	if err := h.tryFail(); err != nil {
		return err
	}
	h.events = append(h.events, aggregateDeliveryEvent{kind: "video", ts: timestamp, payload: b})
	return nil
}

func (h *aggregateRecordingHandler) OnSetDataFrame(timestamp uint32, data *message.NetStreamSetDataFrame) error {
	if err := h.tryFail(); err != nil {
		return err
	}
	h.events = append(h.events, aggregateDeliveryEvent{kind: "data", ts: timestamp, payload: data.Payload})
	return nil
}

// aggregateWire Builds RTMP chunks from messages using a real ChunkStreamer.
type aggregateWire struct {
	t        *testing.T
	streamer *ChunkStreamer
	buf      *bytes.Buffer
}

func newAggregateWire(t *testing.T, chunkSize uint32) *aggregateWire {
	buf := new(bytes.Buffer)
	streamer := NewChunkStreamer(bytes.NewReader(nil), bufio.NewWriter(buf), (&StreamControlStateConfig{
		DefaultChunkSize: chunkSize,
	}).normalize())

	return &aggregateWire{
		t:        t,
		streamer: streamer,
		buf:      buf,
	}
}

func (w *aggregateWire) close() {
	require.NoError(w.t, w.streamer.Close())
	<-w.streamer.Done()
}

// writeMessage Encodes msg and writes it as chunks.
func (w *aggregateWire) writeMessage(chunkStreamID int, streamID uint32, timestamp uint32, msg message.Message) {
	require.NoError(w.t, w.streamer.Write(context.Background(), chunkStreamID, timestamp, &ChunkMessage{
		StreamID: streamID,
		Message:  msg,
	}))
	w.wait(chunkStreamID)
}

// writeRawMessage Writes an already encoded message body as chunks.
func (w *aggregateWire) writeRawMessage(chunkStreamID int, streamID uint32, typeID byte, timestamp uint32, body []byte) {
	writer, err := w.streamer.NewChunkWriter(context.Background(), chunkStreamID)
	require.NoError(w.t, err)

	writer.messageLength = uint32(len(body))
	writer.messageTypeID = typeID
	writer.messageStreamID = streamID
	writer.timestamp = timestamp
	writer.buf.Write(body)

	require.NoError(w.t, w.streamer.Sched(writer))
	w.wait(chunkStreamID)
}

func (w *aggregateWire) wait(chunkStreamID int) {
	_, err := w.streamer.NewChunkWriter(context.Background(), chunkStreamID) // wait for writing
	require.NoError(w.t, err)
}

func (w *aggregateWire) bytes() []byte {
	return w.buf.Bytes()
}

// deliverWire Feeds wire bytes to a fresh connection which has a stream in
// the publish state, and dispatches nMessages messages. Reading is done
// through a reader which returns at most maxRead bytes per Read call.
// Returns the first error occurred while reading or handling a message.
func deliverWire(t *testing.T, wire []byte, maxRead int, handler Handler, nMessages int) error {
	t.Helper()

	rwc := &aggregateTestRWC{
		r: &shortReader{r: bytes.NewReader(wire), max: maxRead},
	}
	conn := newConn(rwc, &ConnConfig{
		Handler: handler,
	})
	defer func() {
		require.NoError(t, conn.Close())
	}()

	stream, err := conn.streams.Create(1)
	require.NoError(t, err)
	stream.handler.ChangeState(streamStateServerPublish)

	var cmsg ChunkMessage
	for i := 0; i < nMessages; i++ {
		chunkStreamID, timestamp, err := conn.streamer.Read(&cmsg)
		if err != nil {
			return err
		}
		if err := conn.handleMessage(chunkStreamID, timestamp, &cmsg); err != nil {
			return err
		}
	}

	return nil
}

// flvTag Builds a FLV tag (11 bytes header + payload + 4 bytes backpointer).
func flvTag(kind byte, ts uint32, payload []byte) []byte {
	n := len(payload)
	b := []byte{
		kind,
		byte(n >> 16), byte(n >> 8), byte(n),
		byte(ts >> 16), byte(ts >> 8), byte(ts), byte(ts >> 24),
		0, 0, 0, // StreamID
	}
	b = append(b, payload...)
	return binary.BigEndian.AppendUint32(b, uint32(11+n))
}

// newSetDataFrameMessage Creates a `@setDataFrame` data message. Also returns
// the expected body bytes (AMF0 encoded `onMetaData` arguments).
func newSetDataFrameMessage(t *testing.T) (*message.DataMessage, []byte) {
	t.Helper()

	buf := new(bytes.Buffer)
	amfEnc := message.NewAMFEncoder(buf, message.EncodingTypeAMF0)
	require.NoError(t, message.EncodeBodyAnyValues(amfEnc, &message.NetStreamSetDataFrame{
		AmfData: map[string]interface{}{
			"title":    "aggregate",
			"duration": float64(1234),
		},
	}))

	return &message.DataMessage{
		Name:     "@setDataFrame",
		Encoding: message.EncodingTypeAMF0,
		Body:     bytes.NewReader(buf.Bytes()),
	}, buf.Bytes()
}

func newAudioVideoDataAggregate(t *testing.T, audio, video []byte, base uint32) (*message.AggregateMessage, []byte) {
	t.Helper()

	dataMsg, dataBytes := newSetDataFrameMessage(t)

	return &message.AggregateMessage{
		Messages: []*message.AggregateMessageEntry{
			{Timestamp: base + 0, Message: &message.AudioMessage{Payload: bytes.NewReader(audio)}},
			{Timestamp: base + 20, Message: &message.VideoMessage{Payload: bytes.NewReader(video)}},
			{Timestamp: base + 40, Message: dataMsg},
		},
	}, dataBytes
}

func TestAggregateDelivery(t *testing.T) {
	audioPayload := []byte{0xaf, 0x01, 0x12, 0x34}
	// AVC payload with a non-zero composition time offset. These bytes are
	// not a tag clock offset and must be delivered as-is.
	videoPayload := []byte{0x17, 0x01, 0x00, 0x00, 0x28, 0x65}

	t.Run("AudioVideoAndDataAreDeliveredInOrder", func(t *testing.T) {
		agg, dataBytes := newAudioVideoDataAggregate(t, audioPayload, videoPayload, 1000)

		wire := newAggregateWire(t, 16) // small chunks to force fragmentation
		wire.writeMessage(4, 1, 5000, agg)
		wire.close()

		handler := &aggregateRecordingHandler{failAt: -1}
		require.NoError(t, deliverWire(t, wire.bytes(), 2, handler, 1))

		require.Equal(t, []aggregateDeliveryEvent{
			{kind: "audio", ts: 5000, payload: audioPayload},
			{kind: "video", ts: 5020, payload: videoPayload},
			{kind: "data", ts: 5040, payload: dataBytes},
		}, handler.events)
	})

	t.Run("ScriptFirstTagAnchorsClock", func(t *testing.T) {
		dataMsg, dataBytes := newSetDataFrameMessage(t)
		agg := &message.AggregateMessage{
			Messages: []*message.AggregateMessageEntry{
				{Timestamp: 2000, Message: dataMsg},
				{Timestamp: 2020, Message: &message.AudioMessage{Payload: bytes.NewReader(audioPayload)}},
				{Timestamp: 2040, Message: &message.VideoMessage{Payload: bytes.NewReader(videoPayload)}},
			},
		}

		wire := newAggregateWire(t, 16)
		wire.writeMessage(4, 1, 9000, agg)
		wire.close()

		handler := &aggregateRecordingHandler{failAt: -1}
		require.NoError(t, deliverWire(t, wire.bytes(), 2, handler, 1))

		require.Equal(t, []aggregateDeliveryEvent{
			{kind: "data", ts: 9000, payload: dataBytes},
			{kind: "audio", ts: 9020, payload: audioPayload},
			{kind: "video", ts: 9040, payload: videoPayload},
		}, handler.events)
	})

	t.Run("TimestampWraparound", func(t *testing.T) {
		p0 := []byte{0xaf, 0x00}
		p1 := []byte{0xaf, 0x01}
		p2 := []byte{0xaf, 0x02}

		// Inner timestamps cross 2^32, and delivered timestamps wrap around
		// as uint32 values.
		agg := &message.AggregateMessage{
			Messages: []*message.AggregateMessageEntry{
				{Timestamp: 0xfffffff0, Message: &message.AudioMessage{Payload: bytes.NewReader(p0)}},
				{Timestamp: 0x0000000a, Message: &message.AudioMessage{Payload: bytes.NewReader(p1)}}, // +26
				{Timestamp: 0x00000028, Message: &message.AudioMessage{Payload: bytes.NewReader(p2)}}, // +56
			},
		}

		wire := newAggregateWire(t, 16)
		wire.writeMessage(4, 1, 0xffffffe0, agg) // requires the extended timestamp on the wire
		wire.close()

		handler := &aggregateRecordingHandler{failAt: -1}
		require.NoError(t, deliverWire(t, wire.bytes(), 2, handler, 1))

		require.Equal(t, []aggregateDeliveryEvent{
			{kind: "audio", ts: 0xffffffe0, payload: p0},
			{kind: "audio", ts: 0xfffffffa, payload: p1},
			{kind: "audio", ts: 0x00000018, payload: p2}, // wrapped around
		}, handler.events)
	})

	t.Run("SameTimestampDifferentPayloads", func(t *testing.T) {
		p0 := []byte{0xaa, 0x01}
		p1 := []byte{0xbb, 0x02}

		agg := &message.AggregateMessage{
			Messages: []*message.AggregateMessageEntry{
				{Timestamp: 1500, Message: &message.AudioMessage{Payload: bytes.NewReader(p0)}},
				{Timestamp: 1500, Message: &message.AudioMessage{Payload: bytes.NewReader(p1)}},
			},
		}

		wire := newAggregateWire(t, 16)
		wire.writeMessage(4, 1, 7000, agg)
		wire.close()

		handler := &aggregateRecordingHandler{failAt: -1}
		require.NoError(t, deliverWire(t, wire.bytes(), 2, handler, 1))

		require.Equal(t, []aggregateDeliveryEvent{
			{kind: "audio", ts: 7000, payload: p0},
			{kind: "audio", ts: 7000, payload: p1},
		}, handler.events)
	})

	t.Run("EncodeDecodeRoundtrip", func(t *testing.T) {
		dataMsg, dataBytes := newSetDataFrameMessage(t)

		// Encode a data message to get a raw tag payload for it
		dataTagPayload := new(bytes.Buffer)
		require.NoError(t, message.NewEncoder(dataTagPayload).Encode(dataMsg))

		body := append(
			flvTag(8, 1000, audioPayload),
			append(
				flvTag(9, 1020, videoPayload),
				flvTag(18, 1040, dataTagPayload.Bytes())...,
			)...,
		)

		var msg message.Message
		require.NoError(t, message.NewDecoder(bytes.NewReader(body)).Decode(message.TypeIDAggregateMessage, &msg))

		agg, ok := msg.(*message.AggregateMessage)
		require.True(t, ok)
		require.Len(t, agg.Messages, 3)

		// Tag order and timestamps are preserved
		require.Equal(t, uint32(1000), agg.Messages[0].Timestamp)
		require.Equal(t, uint32(1020), agg.Messages[1].Timestamp)
		require.Equal(t, uint32(1040), agg.Messages[2].Timestamp)
		require.Equal(t, message.TypeIDAudioMessage, agg.Messages[0].Message.TypeID())
		require.Equal(t, message.TypeIDVideoMessage, agg.Messages[1].Message.TypeID())
		require.Equal(t, message.TypeIDDataMessageAMF0, agg.Messages[2].Message.TypeID())
		require.Equal(t, "@setDataFrame", agg.Messages[2].Message.(*message.DataMessage).Name)

		// Re-encoding reproduces the same body (note: this consumes the
		// payloads of the members)
		out := new(bytes.Buffer)
		require.NoError(t, message.NewEncoder(out).Encode(agg))
		require.Equal(t, body, out.Bytes())

		// Decode again: members can be read independently, in any order
		var msg2 message.Message
		require.NoError(t, message.NewDecoder(bytes.NewReader(body)).Decode(message.TypeIDAggregateMessage, &msg2))
		agg2 := msg2.(*message.AggregateMessage)
		require.Len(t, agg2.Messages, 3)

		dataGot, err := ioutil.ReadAll(agg2.Messages[2].Message.(*message.DataMessage).Body)
		require.NoError(t, err)
		require.Equal(t, dataBytes, dataGot)
		audioGot, err := ioutil.ReadAll(agg2.Messages[0].Message.(*message.AudioMessage).Payload)
		require.NoError(t, err)
		require.Equal(t, audioPayload, audioGot)
		videoGot, err := ioutil.ReadAll(agg2.Messages[1].Message.(*message.VideoMessage).Payload)
		require.NoError(t, err)
		require.Equal(t, videoPayload, videoGot)
	})

	t.Run("EmptyBody", func(t *testing.T) {
		// An empty body is decoded as an aggregate message with zero members
		var msg message.Message
		require.NoError(t, message.NewDecoder(bytes.NewReader(nil)).Decode(message.TypeIDAggregateMessage, &msg))

		agg, ok := msg.(*message.AggregateMessage)
		require.True(t, ok)
		require.Len(t, agg.Messages, 0)

		// Encoding a zero-member aggregate produces an empty body
		out := new(bytes.Buffer)
		require.NoError(t, message.NewEncoder(out).Encode(agg))
		require.Equal(t, 0, out.Len())

		// Delivering a zero-member aggregate is a no-op
		handler := &aggregateRecordingHandler{failAt: -1}
		conn := newConn(&aggregateTestRWC{r: bytes.NewReader(nil)}, &ConnConfig{
			Handler: handler,
		})
		defer func() {
			require.NoError(t, conn.Close())
		}()

		stream, err := conn.streams.Create(1)
		require.NoError(t, err)
		stream.handler.ChangeState(streamStateServerPublish)

		require.NoError(t, stream.handle(4, 5000, &message.AggregateMessage{}))
		require.Empty(t, handler.events)
	})

	t.Run("CorruptTags", func(t *testing.T) {
		validBody := func() []byte {
			return append(
				flvTag(8, 1000, audioPayload),
				flvTag(9, 1020, videoPayload)...,
			)
		}

		tag0Len := 11 + len(audioPayload) + 4

		tcs := []struct {
			name     string
			body     func() []byte
			tagIndex int
			offset   int64
		}{
			{
				name: "TruncatedHeader",
				body: func() []byte {
					return append(validBody(), 0x09, 0x00, 0x00) // broken 2nd tag header
				},
				tagIndex: 2,
				offset:   int64(tag0Len + 11 + len(videoPayload) + 4),
			},
			{
				name: "TruncatedPayload",
				body: func() []byte {
					b := validBody()
					return b[:len(b)-6] // cut into the payload of the 2nd tag
				},
				tagIndex: 1,
				offset:   int64(tag0Len + 11),
			},
			{
				name: "TruncatedBackpointer",
				body: func() []byte {
					b := validBody()
					return b[:len(b)-2] // cut into the backpointer of the 2nd tag
				},
				tagIndex: 1,
				offset:   int64(tag0Len + 11 + len(videoPayload)),
			},
			{
				name: "BadBackpointer",
				body: func() []byte {
					b := validBody()
					b[len(b)-1] ^= 0xff // break the backpointer of the 2nd tag
					return b
				},
				tagIndex: 1,
				offset:   int64(tag0Len + 11 + len(videoPayload)),
			},
			{
				name: "NonZeroStreamID",
				body: func() []byte {
					b := validBody()
					b[10] = 1 // StreamID of the 1st tag
					return b
				},
				tagIndex: 0,
				offset:   0,
			},
			{
				name: "ForbiddenType",
				body: func() []byte {
					return flvTag(20, 1000, []byte{0x00}) // command messages are not allowed
				},
				tagIndex: 0,
				offset:   0,
			},
			{
				name: "NestedAggregate",
				body: func() []byte {
					return flvTag(22, 1000, nil)
				},
				tagIndex: 0,
				offset:   0,
			},
			{
				name: "BackwardTimestamp",
				body: func() []byte {
					return append(
						flvTag(8, 2000, audioPayload),
						flvTag(8, 1000, audioPayload)...,
					)
				},
				tagIndex: 1,
				offset:   int64(tag0Len),
			},
		}

		for _, tc := range tcs {
			t.Run(tc.name, func(t *testing.T) {
				body := tc.body()

				wire := newAggregateWire(t, 16)
				wire.writeRawMessage(4, 1, byte(message.TypeIDAggregateMessage), 5000, body)
				wire.close()

				handler := &aggregateRecordingHandler{failAt: -1}
				err := deliverWire(t, wire.bytes(), 2, handler, 1)
				require.Error(t, err)

				var aggErr *message.AggregateMessageFormatError
				require.True(t, errors.As(err, &aggErr), "unexpected error type: %T (%v)", err, err)
				require.Equal(t, tc.tagIndex, aggErr.TagIndex)
				require.Equal(t, tc.offset, aggErr.Offset)

				// No user handler must be called for a broken aggregate
				require.Empty(t, handler.events)
			})
		}
	})

	t.Run("CallbackErrorStopsDelivery", func(t *testing.T) {
		p0 := []byte{0xaf, 0x00}
		p1 := []byte{0xaf, 0x01}
		p2 := []byte{0xaf, 0x02}

		agg := &message.AggregateMessage{
			Messages: []*message.AggregateMessageEntry{
				{Timestamp: 1000, Message: &message.AudioMessage{Payload: bytes.NewReader(p0)}},
				{Timestamp: 1010, Message: &message.AudioMessage{Payload: bytes.NewReader(p1)}},
				{Timestamp: 1020, Message: &message.AudioMessage{Payload: bytes.NewReader(p2)}},
			},
		}

		wire := newAggregateWire(t, 16)
		wire.writeMessage(4, 1, 5000, agg)
		wire.close()

		sentinel := errors.New("user handler failed")
		handler := &aggregateRecordingHandler{failAt: 1, failErr: sentinel}

		err := deliverWire(t, wire.bytes(), 2, handler, 1)
		require.Equal(t, sentinel, err) // the original error is preserved

		// The 1st callback succeeded, the 2nd one failed, and the 3rd one
		// was never delivered.
		require.Equal(t, 2, handler.calls)
		require.Equal(t, []aggregateDeliveryEvent{
			{kind: "audio", ts: 5000, payload: p0},
		}, handler.events)
	})

	t.Run("ShortReader", func(t *testing.T) {
		for _, maxRead := range []int{1, 2} {
			agg, dataBytes := newAudioVideoDataAggregate(t, audioPayload, videoPayload, 1000)

			wire := newAggregateWire(t, 8) // also fragment heavily
			wire.writeMessage(4, 1, 5000, agg)
			wire.close()

			handler := &aggregateRecordingHandler{failAt: -1}
			require.NoError(t, deliverWire(t, wire.bytes(), maxRead, handler, 1))

			require.Equal(t, []aggregateDeliveryEvent{
				{kind: "audio", ts: 5000, payload: audioPayload},
				{kind: "video", ts: 5020, payload: videoPayload},
				{kind: "data", ts: 5040, payload: dataBytes},
			}, handler.events)
		}
	})

	t.Run("MessageAfterAggregate", func(t *testing.T) {
		standalone := []byte{0xaf, 0x01, 0x99, 0x88}

		agg := &message.AggregateMessage{
			Messages: []*message.AggregateMessageEntry{
				{Timestamp: 1000, Message: &message.AudioMessage{Payload: bytes.NewReader(audioPayload)}},
				{Timestamp: 1020, Message: &message.VideoMessage{Payload: bytes.NewReader(videoPayload)}},
			},
		}

		wire := newAggregateWire(t, 16)
		wire.writeMessage(4, 1, 5000, agg)
		// An ordinary message which immediately follows the aggregate must
		// not be swallowed nor corrupted.
		wire.writeMessage(4, 1, 6000, &message.AudioMessage{Payload: bytes.NewReader(standalone)})
		wire.close()

		handler := &aggregateRecordingHandler{failAt: -1}
		require.NoError(t, deliverWire(t, wire.bytes(), 2, handler, 2))

		require.Equal(t, []aggregateDeliveryEvent{
			{kind: "audio", ts: 5000, payload: audioPayload},
			{kind: "video", ts: 5020, payload: videoPayload},
			{kind: "audio", ts: 6000, payload: standalone},
		}, handler.events)
	})
}
