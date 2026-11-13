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
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"
	"sync/atomic"

	"github.com/nlnwa/gowarc/v3/internal/diskbuffer"
)

var (
	// ErrSegmentTooSmall is returned by Marshal when maxSize is too small to hold
	// even a single content byte together with the WARC framing of a segment.
	// No bytes are written and no empty continuation is produced.
	ErrSegmentTooSmall = errors.New("gowarc: maxSize is too small for a WARC record segment")

	// ErrSegmentationNotSupported is returned by Marshal when segmentation is
	// requested for a record that cannot be segmented. Segmentation is supported
	// for WARC/1.1 response and resource records and for continuation records
	// produced by a previous segmented Marshal call.
	ErrSegmentationNotSupported = errors.New("gowarc: segmentation is only supported for cached WARC/1.1 response and resource records")
)

// sha256Base16DigestLength is the fixed length of a WARC-Block-Digest field value
// produced for a segment ("sha256:" + 64 lower-case hex characters).
const sha256Base16DigestLength = len("sha256:") + 64

// segmentState holds the complete, uncut content block of a logically single
// WARC record in an independent, seekable buffer. The state is shared between
// the segments of one segmented record and survives closing of the original
// input record or of any already written continuation record.
type segmentState struct {
	buf      diskbuffer.Buffer // full raw block bytes, backing store independent of the input record
	total    int64             // total size of the logical content block
	originID string            // WARC-Record-ID of the first segment
	refs     atomic.Int32      // number of live continuation records referencing this state
}

// retain registers a new live continuation record.
func (s *segmentState) retain() {
	s.refs.Add(1)
}

// release deregisters a continuation record and frees the backing buffer once
// the last reference goes away.
func (s *segmentState) release() error {
	if s.refs.Add(-1) <= 0 {
		return s.buf.Close()
	}
	return nil
}

// close frees the backing buffer. It is used by the marshaler when it created
// the state but never handed a continuation to the caller, so no references
// have been retained.
func (s *segmentState) close() error {
	return s.buf.Close()
}

// segmentBlock is a Block presenting a byte range of a segmentState as the
// content block of one physical WARC record. Content is cut from the raw block
// bytes of the original record without any rewriting.
type segmentBlock struct {
	state     *segmentState
	off       int64 // absolute offset in the shared buffer
	length    int64 // number of block bytes in this segment
	digestStr string
}

var _ Block = (*segmentBlock)(nil)

func (b *segmentBlock) RawBytes() (io.Reader, error) {
	return io.NewSectionReader(b.state.buf, b.off, b.length), nil
}

func (b *segmentBlock) Size() int64 {
	return b.length
}

func (b *segmentBlock) IsCached() bool {
	return true
}

func (b *segmentBlock) Cache() error {
	return nil
}

func (b *segmentBlock) Close() error {
	// The shared buffer is released through the owning record's closer.
	return nil
}

// BlockDigest returns the SHA-256 base16 digest computed over this segment's
// actual block bytes.
func (b *segmentBlock) BlockDigest() string {
	if b.digestStr == "" {
		d, _ := newDigest("sha256", Base16)
		r := io.NewSectionReader(b.state.buf, b.off, b.length)
		_, _ = io.Copy(io.Discard, newDigestFilterReader(r, d))
		b.digestStr = d.format()
	}
	return b.digestStr
}

// materializeSegmentState caches the record's block and copies its complete raw
// block bytes into a fresh, independent disk buffer so that later segments and
// returned continuation records stay valid after the input record is closed.
// The source content is read exactly once.
func materializeSegmentState(record *warcRecord) (*segmentState, error) {
	block := record.Block()
	if err := block.Cache(); err != nil {
		return nil, err
	}
	raw, err := block.RawBytes()
	if err != nil {
		return nil, err
	}

	buf := diskbuffer.New(record.opts.bufferOptions...)
	if _, err := buf.ReadFrom(raw); err != nil {
		_ = buf.Close()
		return nil, err
	}

	return &segmentState{
		buf:      buf,
		total:    buf.Size(),
		originID: record.RecordId(),
	}, nil
}

// countingWriter counts the bytes written to it without storing them.
type countingWriter struct{ n int64 }

func (c *countingWriter) Write(p []byte) (int, error) {
	c.n += int64(len(p))
	return len(p), nil
}

// physicalRecordSize returns the exact number of uncompressed bytes a physical
// WARC record with the given header and content length occupies:
// version line + header fields + blank line + content + end-of-record marker.
func physicalRecordSize(version *WarcVersion, header *WarcFields, contentLength int64) int64 {
	var c countingWriter
	_, _ = header.Write(&c)
	return int64(len(version.String())) + 2 + c.n + 2 + contentLength + 4
}

// probeHeader renders a candidate segment header. The block digest is replaced
// by a placeholder of the exact length a SHA-256 base16 digest will occupy so
// that the size search needs no hashing and accounts for every header byte.
func probeHeader(base *WarcFields, recordType RecordType, segmentNumber int, originID string, totalLength int64, final bool, contentLength int64) *WarcFields {
	h := base.clone()
	h.Set(WarcType, recordType.String())
	h.SetInt(WarcSegmentNumber, segmentNumber)
	h.Delete(WarcSegmentTotalLength)
	if recordType == Continuation {
		h.Delete(WarcPayloadDigest)
		h.Delete(ContentType)
		h.SetId(WarcSegmentOriginID, originID)
	}
	if final {
		h.SetInt64(WarcSegmentTotalLength, totalLength)
	}
	h.SetInt64(ContentLength, contentLength)
	h.Set(WarcBlockDigest, strings.Repeat("0", sha256Base16DigestLength))
	return h
}

// fitsWithin reports whether a segment with the given content length and
// finality stays within maxSize.
func fitsWithin(version *WarcVersion, base *WarcFields, recordType RecordType, segmentNumber int, state *segmentState, final bool, contentLength, maxSize int64) bool {
	h := probeHeader(base, recordType, segmentNumber, state.originID, state.total, final, contentLength)
	return physicalRecordSize(version, h, contentLength) <= maxSize
}

// largestFittingContent returns the largest content length k in [1, upper] for
// which a segment with the given finality stays within maxSize, or 0 if not even
// one content byte fits. Physical size is strictly increasing in k because the
// Content-Length field can only stay equal or grow in decimal width, so binary
// search over the monotone predicate is valid.
func largestFittingContent(version *WarcVersion, base *WarcFields, recordType RecordType, segmentNumber int, state *segmentState, final bool, upper, maxSize int64) int64 {
	if upper <= 0 {
		return 0
	}
	firstTooBig := sort.Search(int(upper), func(i int) bool {
		k := int64(i + 1)
		return !fitsWithin(version, base, recordType, segmentNumber, state, final, k, maxSize)
	})
	return int64(firstTooBig)
}

// planSegment determines the largest non-empty content prefix that fits within
// maxSize together with the segment framing, or returns ErrSegmentTooSmall when
// not even one content byte fits. Nothing is written by this function.
//
// The final segment carries the extra WARC-Segment-Total-Length field and
// therefore has the largest header. The budget is first measured against that
// final header: once the whole remainder fits a final segment it is closed out,
// otherwise a non-final segment is emitted while guaranteeing the leftover can
// always be finalized later, so the chain can neither dead-end nor loop.
func planSegment(version *WarcVersion, base *WarcFields, recordType RecordType, segmentNumber int, state *segmentState, remain, maxSize int64) (contentLength int64, final bool, err error) {
	maxFinal := largestFittingContent(version, base, recordType, segmentNumber, state, true, remain, maxSize)
	if maxFinal < 1 {
		return 0, false, ErrSegmentTooSmall
	}
	if remain <= maxFinal {
		return remain, true, nil
	}

	maxNonFinal := largestFittingContent(version, base, recordType, segmentNumber, state, false, remain-1, maxSize)
	if maxNonFinal < 1 {
		return 0, false, ErrSegmentTooSmall
	}
	return maxNonFinal, false, nil
}

// renderSegmentHeader builds the concrete header for a segment about to be
// written. Content-Length and WARC-Block-Digest are filled in by the caller
// once the segment block is known.
func renderSegmentHeader(base *WarcFields, recordType RecordType, segmentNumber int, originID string, totalLength int64, final bool) *WarcFields {
	h := base.clone()
	h.Set(WarcType, recordType.String())
	h.SetInt(WarcSegmentNumber, segmentNumber)
	h.Delete(ContentLength)
	h.Delete(WarcBlockDigest)
	h.Delete(WarcSegmentTotalLength)
	if recordType == Continuation {
		// The payload digest describes the complete logical response and stays
		// on the first segment only; HTTP headers are not copied to continuations.
		h.Delete(WarcPayloadDigest)
		h.Delete(ContentType)
		h.SetId(WarcSegmentOriginID, originID)
	}
	if final {
		h.SetInt64(WarcSegmentTotalLength, totalLength)
	}
	return h
}

// writePhysicalSegment writes one fully determined physical WARC record and
// returns the number of bytes written.
func writePhysicalSegment(w io.Writer, version *WarcVersion, header *WarcFields, block *segmentBlock) (int64, error) {
	var bytesWritten int64

	n, err := io.WriteString(w, version.String())
	bytesWritten += int64(n)
	if err != nil {
		return bytesWritten, err
	}
	n, err = w.Write(crlf)
	bytesWritten += int64(n)
	if err != nil {
		return bytesWritten, err
	}

	k, err := header.Write(w)
	bytesWritten += k
	if err != nil {
		return bytesWritten, err
	}

	n, err = w.Write(crlf)
	bytesWritten += int64(n)
	if err != nil {
		return bytesWritten, err
	}

	r, err := block.RawBytes()
	if err != nil {
		return bytesWritten, err
	}
	k, err = io.Copy(w, r)
	bytesWritten += k
	if err != nil {
		return bytesWritten, err
	}

	n, err = w.Write(crlfcrlf)
	bytesWritten += int64(n)
	return bytesWritten, err
}

// newContinuationRecord builds the record representing the not-yet-written
// suffix of a segmented record. It has its own record id, the same target and
// date as the first segment, an Origin-ID pointing back to it, and no payload
// digest. Its closer releases the shared backing buffer.
func newContinuationRecord(opts *warcRecordOptions, base *WarcFields, state *segmentState, off, length, segmentNumber int64) (*warcRecord, error) {
	h := base.clone()
	h.Set(WarcType, Continuation.String())
	h.Delete(WarcBlockDigest)
	h.Delete(WarcPayloadDigest)
	h.Delete(ContentType)
	h.Delete(WarcSegmentTotalLength)
	h.SetId(WarcSegmentOriginID, state.originID)
	h.SetInt64(WarcSegmentNumber, segmentNumber)
	h.SetInt64(ContentLength, length)

	id, err := opts.recordIdFunc()
	if err != nil {
		return nil, err
	}
	h.SetId(WarcRecordID, id)

	rec := &warcRecord{
		opts:       opts,
		version:    V1_1,
		recordType: Continuation,
		headers:    h,
		block: &segmentBlock{
			state:  state,
			off:    off,
			length: length,
		},
	}
	state.retain()
	rec.closer = func() error {
		return state.release()
	}
	return rec, nil
}

// errSegmentationInput wraps ErrSegmentationNotSupported with the actual
// version and record type received.
func errSegmentationInput(record WarcRecord) error {
	return fmt.Errorf("%w: got version %s, type %s", ErrSegmentationNotSupported, record.Version(), record.Type())
}
