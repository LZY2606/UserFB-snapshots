//
// Copyright (c) 2018- yutopp (yutopp@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at  https://www.boost.org/LICENSE_1_0.txt)
//

package rtmp

import (
	"bytes"
	"io/ioutil"
	"sync"

	"github.com/pkg/errors"
	"github.com/sirupsen/logrus"

	"github.com/yutopp/go-rtmp/internal"
	"github.com/yutopp/go-rtmp/message"
)

type streamState int

const (
	streamStateUnknown streamState = iota
	streamStateServerNotConnected
	streamStateServerConnected
	streamStateServerInactive
	streamStateServerPublish
	streamStateServerPlay
	streamStateClientNotConnected
	streamStateClientConnected
)

func (s streamState) String() string {
	switch s {
	case streamStateServerNotConnected:
		return "NotConnected(Server)"
	case streamStateServerConnected:
		return "Connected(Server)"
	case streamStateServerInactive:
		return "Inactive(Server)"
	case streamStateServerPublish:
		return "Publish(Server)"
	case streamStateServerPlay:
		return "Play(Server)"
	case streamStateClientNotConnected:
		return "NotConnected(Client)"
	case streamStateClientConnected:
		return "Connected(Client)"
	default:
		return "<Unknown>"
	}
}

// streamHandler A handler per streams.
// It holds a handler for each states and processes messages sent to the stream
type streamHandler struct {
	stream  *Stream
	handler stateHandler // A handler for each states
	state   streamState

	loggerEntry *logrus.Entry
	m           sync.Mutex
}

// newEntryHandler Create an incomplete new instance of entryHandler.
// msgHandler fields must be assigned by a caller of this function
func newStreamHandler(s *Stream) *streamHandler {
	return &streamHandler{
		stream: s,
	}
}

func (h *streamHandler) Handle(chunkStreamID int, timestamp uint32, msg message.Message) error {
	l := h.Logger()

	switch msg := msg.(type) {
	case *message.DataMessage:
		return h.handleData(chunkStreamID, timestamp, msg)

	case *message.CommandMessage:
		return h.handleCommand(chunkStreamID, timestamp, msg)

	case *message.AggregateMessage:
		return h.handleAggregate(chunkStreamID, timestamp, msg)

	case *message.SetChunkSize:
		l.Infof("Handle SetChunkSize: Msg = %#v", msg)
		return h.stream.streamer().PeerState().SetChunkSize(msg.ChunkSize)

	case *message.WinAckSize:
		l.Infof("Handle WinAckSize: Msg = %#v", msg)
		return h.stream.streamer().PeerState().SetAckWindowSize(msg.Size)

	default:
		err := h.handler.onMessage(chunkStreamID, timestamp, msg)
		if err == internal.ErrPassThroughMsg {
			return h.stream.userHandler().OnUnknownMessage(timestamp, msg)
		}
		return err
	}
}

func (h *streamHandler) ChangeState(state streamState) {
	h.m.Lock()
	defer h.m.Unlock()

	prevState := h.State()

	switch state {
	case streamStateUnknown:
		return // DO NOTHING
	case streamStateServerNotConnected:
		h.handler = &serverControlNotConnectedHandler{sh: h}
	case streamStateServerConnected:
		h.handler = &serverControlConnectedHandler{sh: h}
	case streamStateServerInactive:
		h.handler = &serverDataInactiveHandler{sh: h}
	case streamStateServerPublish:
		h.handler = &serverDataPublishHandler{sh: h}
	case streamStateServerPlay:
		h.handler = &serverDataPlayHandler{sh: h}
	case streamStateClientNotConnected:
		h.handler = &clientControlNotConnectedHandler{sh: h}
		// 	case streamStateClientConnected:
		// 		h.handler = &serverControlConnectedHandler{sh: h}
	default:
		panic("Unexpected")
	}
	h.state = state

	l := h.Logger()
	l.Infof("Change state: From = %s, To = %s", prevState, h.State())
}

func (h *streamHandler) State() streamState {
	return h.state
}

func (h *streamHandler) Logger() *logrus.Entry {
	if h.loggerEntry == nil {
		h.loggerEntry = h.stream.logger().WithFields(logrus.Fields{
			"stream_id": h.stream.streamID,
		})
	}

	h.loggerEntry.Data["state"] = h.State()

	return h.loggerEntry
}

func (h *streamHandler) handleData(
	chunkStreamID int,
	timestamp uint32,
	dataMsg *message.DataMessage,
) error {
	bodyDecoder := message.DataBodyDecoderFor(dataMsg.Name)

	amfDec := message.NewAMFDecoder(dataMsg.Body, dataMsg.Encoding)
	var value message.AMFConvertible
	if err := bodyDecoder(dataMsg.Body, amfDec, &value); err != nil {
		return err
	}

	err := h.handler.onData(chunkStreamID, timestamp, dataMsg, value)
	if err == internal.ErrPassThroughMsg {
		return h.stream.userHandler().OnUnknownDataMessage(timestamp, dataMsg)
	}
	return err
}

func (h *streamHandler) handleCommand(
	chunkStreamID int,
	timestamp uint32,
	cmdMsg *message.CommandMessage,
) error {
	switch cmdMsg.CommandName {
	case "_result", "_error":
		t, err := h.stream.transactions.At(cmdMsg.TransactionID)
		if err != nil {
			return errors.Wrap(err, "Got response to the unexpected transaction")
		}

		// Set result (NOTE: should use a mutex for it?)
		t.Reply(cmdMsg.CommandName, cmdMsg.Encoding, cmdMsg.Body)

		// Remove transacaction because this transaction is resolved
		if err := h.stream.transactions.Delete(cmdMsg.TransactionID); err != nil {
			return errors.Wrap(err, "Unexpected behavior: transaction is not found")
		}

		return nil

		// TODO: Support onStatus
	}

	amfDec := message.NewAMFDecoder(cmdMsg.Body, cmdMsg.Encoding)
	bodyDecoder := message.CmdBodyDecoderFor(cmdMsg.CommandName, cmdMsg.TransactionID)

	var value message.AMFConvertible
	if err := bodyDecoder(cmdMsg.Body, amfDec, &value); err != nil {
		return err
	}

	err := h.handler.onCommand(chunkStreamID, timestamp, cmdMsg, value)
	if err == internal.ErrPassThroughMsg {
		return h.stream.userHandler().OnUnknownCommandMessage(timestamp, cmdMsg)
	}

	return err
}

// handleAggregate Delivers members of an aggregate message to the same
// handlers as their standalone counterparts.
func (h *streamHandler) handleAggregate(
	chunkStreamID int,
	timestamp uint32,
	aggMsg *message.AggregateMessage,
) error {
	// Validate the whole message before delivering anything to user
	// handlers, so that a broken member is never detected after some
	// members have already been published.
	for i, entry := range aggMsg.Messages {
		if err := h.validateAggregateMember(entry.Message); err != nil {
			return errors.Wrapf(err, "Invalid aggregate member: Index = %d", i)
		}
	}

	var firstTimestamp uint32
	for i, entry := range aggMsg.Messages {
		if i == 0 {
			firstTimestamp = entry.Timestamp
		}
		// Align the timestamp of the member to the outer RTMP clock: the
		// first tag corresponds to the outer timestamp, and subsequent tags
		// are offset by their unsigned differences from the first tag.
		// Both the subtraction and the addition are done in uint32, so
		// wrapping around is preserved as-is.
		ts := timestamp + (entry.Timestamp - firstTimestamp)

		if err := h.Handle(chunkStreamID, ts, entry.Message); err != nil {
			switch err.(type) {
			case *message.UnknownDataBodyDecodeError, *message.UnknownCommandBodyDecodeError:
				// Ignore unknown message bodies and continue to deliver
				// the remaining members, as done for standalone messages.
				h.Logger().Warnf("Ignored unknown message body in an aggregate message: Err = %+v", err)
				continue
			}
			return err
		}
	}

	return nil
}

// validateAggregateMember Checks that a member of an aggregate message is
// well-formed before it is delivered to user handlers.
func (h *streamHandler) validateAggregateMember(msg message.Message) error {
	switch msg := msg.(type) {
	case *message.AudioMessage, *message.VideoMessage:
		// Nothing to check: the structure was already validated when the
		// aggregate message was decoded.
		return nil

	case *message.DataMessage:
		if msg.Encoding != message.EncodingTypeAMF0 {
			return errors.Errorf(
				"Unsupported encoding of a data message in an aggregate message: Encoding = %d",
				msg.Encoding,
			)
		}

		// Buffer the body to validate it here and to make it readable
		// again for the actual delivery.
		body, err := ioutil.ReadAll(msg.Body)
		if err != nil {
			return errors.Wrap(err, "Failed to read a data message body")
		}
		msg.Body = bytes.NewReader(body)

		amfDec := message.NewAMFDecoder(bytes.NewReader(body), msg.Encoding)
		bodyDecoder := message.DataBodyDecoderFor(msg.Name)

		var value message.AMFConvertible
		if err := bodyDecoder(bytes.NewReader(body), amfDec, &value); err != nil {
			if _, ok := err.(*message.UnknownDataBodyDecodeError); ok {
				// Unknown data names are not fatal: they are reported as
				// unknown data messages on delivery.
				return nil
			}
			return err
		}

		return nil

	default:
		return errors.Errorf("Unexpected message type in an aggregate message: %T", msg)
	}
}
