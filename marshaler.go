/*
 * Copyright 2021 National Library of Norway.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package gowarc

import (
	"fmt"
	"io"
)

// Marshaler is the interface that wraps the Marshal function.
//
// Marshal converts a WARC record to its serialized form and returns the size of the marshalled record or any error encountered.
//
// When maxSize is positive it denotes the maximum number of complete, uncompressed
// bytes of one physical WARC record (version line, header fields, separators and
// content included). If the record does not fit, it is segmented according to
// WARC 1.1 record segmentation and the returned WarcRecord is the continuation,
// ready to be passed to another Marshal call (possibly with a new maxSize) until
// nil is returned. A maxSize of zero or less writes the complete record without
// segmentation.
//
// See the description of record segmentation at https://iipc.github.io/warc-specifications/specifications/warc-format/warc-1.1/#record-segmentation
type Marshaler interface {
	Marshal(w io.Writer, record WarcRecord, maxSize int64) (WarcRecord, int64, error)
}

type defaultMarshaler struct {
}

func NewMarshaler() Marshaler {
	return &defaultMarshaler{}
}

func (m *defaultMarshaler) Marshal(w io.Writer, record WarcRecord, maxSize int64) (WarcRecord, int64, error) {
	if maxSize <= 0 {
		size, err := m.writeRecord(w, record)
		return nil, size, err
	}
	return m.marshalSegmented(w, record, maxSize)
}

// marshalSegmented writes at most maxSize uncompressed bytes of one physical
// WARC record and returns the continuation record when more segments remain.
// All size planning happens before the first byte is written, so an error never
// results in a partial, unusable segment being reported as success.
func (m *defaultMarshaler) marshalSegmented(w io.Writer, record WarcRecord, maxSize int64) (WarcRecord, int64, error) {
	wr, ok := record.(*warcRecord)
	if !ok {
		return nil, 0, errSegmentationInput(record)
	}

	var (
		state         *segmentState
		base          *WarcFields
		recordType    RecordType
		segmentNumber int64
		off, remain   int64
		ownsState     bool
	)

	if sb, isContinuation := wr.block.(*segmentBlock); isContinuation {
		// A continuation produced by a previous segmented Marshal call.
		state = sb.state
		base = wr.headers
		recordType = Continuation
		segmentNumber, _ = wr.headers.GetInt64(WarcSegmentNumber)
		off = sb.off
		remain = sb.length
	} else {
		// First segment: only cached WARC/1.1 response and resource records are supported.
		if wr.version != V1_1 || (wr.recordType != Response && wr.recordType != Resource) {
			return nil, 0, errSegmentationInput(record)
		}
		if !wr.Block().IsCached() {
			if err := wr.Block().Cache(); err != nil {
				return nil, 0, err
			}
		}

		// If the unchanged record fits, write it in full without any segmentation fields.
		if size := physicalRecordSize(wr.version, wr.headers, wr.Block().Size()); size <= maxSize {
			n, err := m.writeRecord(w, record)
			return nil, n, err
		}

		st, err := materializeSegmentState(wr)
		if err != nil {
			return nil, 0, err
		}
		state = st
		ownsState = true
		base = wr.headers
		recordType = wr.recordType
		segmentNumber = 1
		off = 0
		remain = state.total
	}

	contentLength, final, err := planSegment(wr.version, base, recordType, int(segmentNumber), state, remain, maxSize)
	if err != nil {
		if ownsState {
			_ = state.close()
		}
		return nil, 0, err
	}

	header := renderSegmentHeader(base, recordType, int(segmentNumber), state.originID, state.total, final)
	block := &segmentBlock{state: state, off: off, length: contentLength}
	header.SetInt64(ContentLength, contentLength)
	header.Set(WarcBlockDigest, block.BlockDigest())

	n, werr := writePhysicalSegment(w, wr.version, header, block)
	if werr != nil {
		if ownsState {
			_ = state.close()
		}
		// Keep the original write error and the actual number of bytes written;
		// do not hand back a continuation that could not be fully written.
		return nil, n, fmt.Errorf("marshaling segment %d: %w", segmentNumber, werr)
	}
	if n > maxSize {
		// Invariant check: planning guarantees the segment fits. Fail loudly rather
		// than silently exceeding the caller's budget.
		if ownsState {
			_ = state.close()
		}
		return nil, n, fmt.Errorf("%w: segment of %d bytes exceeds maxSize %d", ErrSegmentTooSmall, n, maxSize)
	}

	if final {
		if ownsState {
			_ = state.close()
		}
		return nil, n, nil
	}

	next, err := newContinuationRecord(wr.opts, base, state, off+contentLength, remain-contentLength, segmentNumber+1)
	if err != nil {
		if ownsState {
			_ = state.close()
		}
		return nil, n, err
	}
	return next, n, nil
}

func (m *defaultMarshaler) writeRecord(w io.Writer, record WarcRecord) (int64, error) {
	var bytesWritten int64

	// Write WARC record version
	n, err := io.WriteString(w, fmt.Sprint(record.Version()))
	bytesWritten += int64(n)
	if err != nil {
		return bytesWritten, err
	}
	n, err = w.Write(crlf)
	bytesWritten += int64(n)
	if err != nil {
		return bytesWritten, err
	}

	// Write WARC header
	k, err := record.WarcHeader().Write(w)
	bytesWritten += k
	if err != nil {
		return bytesWritten, err
	}

	// Write separator
	n, err = w.Write(crlf)
	bytesWritten += int64(n)
	if err != nil {
		return bytesWritten, err
	}

	// Write WARC content
	r, err := record.Block().RawBytes()
	if err != nil {
		return bytesWritten, err
	}
	k, err = io.Copy(w, r)
	bytesWritten += k
	if err != nil {
		return bytesWritten, err
	}

	// Write end of record separator
	n, err = w.Write(crlfcrlf)
	bytesWritten += int64(n)
	if err != nil {
		return bytesWritten, err
	}

	return bytesWritten, err
}
