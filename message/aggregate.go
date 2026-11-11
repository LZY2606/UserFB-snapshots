//
// Copyright (c) 2018- yutopp (yutopp@gmail.com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at  https://www.boost.org/LICENSE_1_0.txt)
//

package message

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"

	"github.com/pkg/errors"
)

const (
	aggregateTagHeaderSize      = 11
	aggregateTagBackpointerSize = 4

	// aggregateMaxTimestampSpan The span of timestamps in an aggregate
	// message must be less than 2^31 milliseconds.
	aggregateMaxTimestampSpan = 1 << 31
)

// AggregateMessageFormatError Reports a malformed FLV tag in an aggregate
// message body.
type AggregateMessageFormatError struct {
	TagIndex int   // Index of the broken tag in the body
	Offset   int64 // Byte offset in the body where the error was detected
	Err      error
}

func (e *AggregateMessageFormatError) Error() string {
	return fmt.Sprintf("Invalid aggregate message body: TagIndex = %d, Offset = %d: %s",
		e.TagIndex,
		e.Offset,
		e.Err,
	)
}

func (e *AggregateMessageFormatError) Unwrap() error {
	return e.Err
}

// decodeAggregateMessageBody Decodes a sequence of FLV tags (without the FLV
// file header and PreviousTagSize0) from r until EOF. An empty body is
// decoded as an aggregate message with zero members.
func decodeAggregateMessageBody(r io.Reader, m *AggregateMessage) error {
	var offset int64
	tagIndex := 0

	var firstTimestamp uint32
	var prevDiff uint32

	for {
		tagOffset := offset

		header := make([]byte, aggregateTagHeaderSize)
		n, err := io.ReadFull(r, header)
		offset += int64(n)
		if err != nil {
			if err == io.EOF {
				// No more tags
				return nil
			}
			if err == io.ErrUnexpectedEOF {
				err = errors.New("tag header is truncated")
			}
			return &AggregateMessageFormatError{
				TagIndex: tagIndex,
				Offset:   tagOffset,
				Err:      err,
			}
		}

		tagType := header[0]
		dataSize := int64(header[1])<<16 | int64(header[2])<<8 | int64(header[3])
		timestamp := uint32(header[4])<<16 | uint32(header[5])<<8 | uint32(header[6]) | uint32(header[7])<<24
		streamID := uint32(header[8])<<16 | uint32(header[9])<<8 | uint32(header[10])

		if streamID != 0 {
			return &AggregateMessageFormatError{
				TagIndex: tagIndex,
				Offset:   tagOffset,
				Err:      errors.Errorf("StreamID in a tag must be 0: Got = %d", streamID),
			}
		}

		switch TypeID(tagType) {
		case TypeIDAudioMessage, TypeIDVideoMessage, TypeIDDataMessageAMF0:
			// OK
		case TypeIDAggregateMessage:
			return &AggregateMessageFormatError{
				TagIndex: tagIndex,
				Offset:   tagOffset,
				Err:      errors.New("nested aggregate messages are not allowed"),
			}
		default:
			return &AggregateMessageFormatError{
				TagIndex: tagIndex,
				Offset:   tagOffset,
				Err:      errors.Errorf("tag type is not allowed in an aggregate message: TypeID = %d", tagType),
			}
		}

		payloadOffset := offset
		payload := make([]byte, dataSize)
		n, err = io.ReadFull(r, payload)
		offset += int64(n)
		if err != nil {
			if err == io.EOF || err == io.ErrUnexpectedEOF {
				err = errors.New("tag payload is truncated")
			}
			return &AggregateMessageFormatError{
				TagIndex: tagIndex,
				Offset:   payloadOffset,
				Err:      err,
			}
		}

		backpointerOffset := offset
		backpointer := make([]byte, aggregateTagBackpointerSize)
		n, err = io.ReadFull(r, backpointer)
		offset += int64(n)
		if err != nil {
			if err == io.EOF || err == io.ErrUnexpectedEOF {
				err = errors.New("tag backpointer is truncated")
			}
			return &AggregateMessageFormatError{
				TagIndex: tagIndex,
				Offset:   backpointerOffset,
				Err:      err,
			}
		}
		if expect, got := uint32(aggregateTagHeaderSize+dataSize), binary.BigEndian.Uint32(backpointer); expect != got {
			return &AggregateMessageFormatError{
				TagIndex: tagIndex,
				Offset:   backpointerOffset,
				Err:      errors.Errorf("invalid backpointer: Expect = %d, Got = %d", expect, got),
			}
		}

		// Timestamps of tags must never go backward relative to the first
		// tag, and the span of an aggregate must be less than 2^31
		// milliseconds. Differences are calculated in uint32, so a wrap
		// around of raw timestamps is supported.
		diff := timestamp - firstTimestamp
		if tagIndex > 0 {
			if diff < prevDiff {
				return &AggregateMessageFormatError{
					TagIndex: tagIndex,
					Offset:   tagOffset,
					Err: errors.Errorf(
						"timestamps must not go backward: PrevDiff = %d, Diff = %d",
						prevDiff,
						diff,
					),
				}
			}
			if diff >= aggregateMaxTimestampSpan {
				return &AggregateMessageFormatError{
					TagIndex: tagIndex,
					Offset:   tagOffset,
					Err: errors.Errorf(
						"timestamp span of an aggregate message is too large: Diff = %d",
						diff,
					),
				}
			}
		}
		prevDiff = diff

		// Decode a member message from its own buffer so that members can
		// be read independently after decoding.
		var member Message
		memberDec := NewDecoder(bytes.NewReader(payload))
		if err := memberDec.Decode(TypeID(tagType), &member); err != nil {
			return &AggregateMessageFormatError{
				TagIndex: tagIndex,
				Offset:   tagOffset,
				Err:      errors.Wrap(err, "Failed to decode a member message"),
			}
		}

		m.Messages = append(m.Messages, &AggregateMessageEntry{
			Timestamp: timestamp,
			Message:   member,
		})
		tagIndex++
	}
}

// encodeAggregateMessageBody Encodes members of m as a sequence of FLV tags
// (without the FLV file header and PreviousTagSize0).
func encodeAggregateMessageBody(w io.Writer, m *AggregateMessage) error {
	var firstTimestamp uint32
	var prevDiff uint32

	for i, entry := range m.Messages {
		if entry == nil || entry.Message == nil {
			return errors.Errorf("Invalid aggregate member: Index = %d: message is nil", i)
		}

		var tagType TypeID
		switch member := entry.Message.(type) {
		case *AudioMessage:
			tagType = TypeIDAudioMessage
		case *VideoMessage:
			tagType = TypeIDVideoMessage
		case *DataMessage:
			if member.Encoding != EncodingTypeAMF0 {
				return errors.Errorf(
					"Invalid aggregate member: Index = %d: only AMF0 data messages are supported: Encoding = %d",
					i,
					member.Encoding,
				)
			}
			tagType = TypeIDDataMessageAMF0
		default:
			return errors.Errorf(
				"Invalid aggregate member: Index = %d: unsupported message type: %T",
				i,
				entry.Message,
			)
		}

		// Apply the same timestamp rules as the decoder so that an encoded
		// message is always decodable.
		diff := entry.Timestamp - firstTimestamp
		if i > 0 {
			if diff < prevDiff {
				return errors.Errorf(
					"Invalid aggregate member: Index = %d: timestamps must not go backward: PrevDiff = %d, Diff = %d",
					i,
					prevDiff,
					diff,
				)
			}
			if diff >= aggregateMaxTimestampSpan {
				return errors.Errorf(
					"Invalid aggregate member: Index = %d: timestamp span of an aggregate message is too large: Diff = %d",
					i,
					diff,
				)
			}
		}
		prevDiff = diff

		// Encode the member payload using the existing message encoder
		payload := new(bytes.Buffer)
		memberEnc := NewEncoder(payload)
		if err := memberEnc.Encode(entry.Message); err != nil {
			return errors.Wrapf(err, "Failed to encode an aggregate member: Index = %d", i)
		}

		dataSize := payload.Len()
		if dataSize > 0xffffff {
			return errors.Errorf(
				"Invalid aggregate member: Index = %d: payload is too large: Size = %d",
				i,
				dataSize,
			)
		}

		header := make([]byte, aggregateTagHeaderSize)
		header[0] = byte(tagType)
		header[1] = byte(dataSize >> 16) // DataSize (24bits BE)
		header[2] = byte(dataSize >> 8)
		header[3] = byte(dataSize)
		header[4] = byte(entry.Timestamp >> 16) // Timestamp (24bits BE)
		header[5] = byte(entry.Timestamp >> 8)
		header[6] = byte(entry.Timestamp)
		header[7] = byte(entry.Timestamp >> 24) // TimestampExtended (8bits)
		// header[8:11] StreamID: always 0

		if _, err := w.Write(header); err != nil {
			return err
		}
		if _, err := payload.WriteTo(w); err != nil {
			return err
		}

		backpointer := make([]byte, aggregateTagBackpointerSize)
		binary.BigEndian.PutUint32(backpointer, uint32(aggregateTagHeaderSize+dataSize))
		if _, err := w.Write(backpointer); err != nil {
			return err
		}
	}

	return nil
}
