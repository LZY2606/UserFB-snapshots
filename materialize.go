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
	"time"

	"github.com/nlnwa/gowarc/v3/internal/diskbuffer"
)

// MaterializeRevisit restores a cached WARC 1.1 HTTP response revisit record into an independent,
// fully materialized response record.
//
// The revisit record must use the WARC 1.1 identical-payload-digest profile and reference the
// original record through WARC-Refers-To. The original record must be a complete, cached HTTP
// response record which is not truncated. Neither record may use WARC segmentation fields.
//
// Unlike [WarcRecord.Merge], MaterializeRevisit never modifies its inputs and does not share any
// mutable state with them: the returned record owns private, independently closable copies of the
// revisit's HTTP header bytes and the original's payload. Closing or mutating the returned record
// therefore affects neither the inputs nor any other materialized record.
//
// On success the returned record:
//   - keeps the revisit's WARC-Record-ID, WARC-Date, WARC-Target-URI and raw HTTP header bytes,
//   - takes its payload from the original record,
//   - has WARC-Type "response",
//   - no longer carries WARC-Refers-To, WARC-Refers-To-Target-URI, WARC-Refers-To-Date,
//     WARC-Profile or the revisit's WARC-Truncated field,
//   - has a Content-Length equal to the real size of its WARC block, and
//   - carries freshly computed SHA-256 base16 WARC-Block-Digest and WARC-Payload-Digest values
//     derived from the materialized content (the original's block digest is never copied).
//
// The revisit's declared payload digest is verified against the original's actual payload,
// supporting SHA-1 and SHA-256 digests in both base16 and base32 encoding. A correct reference id
// combined with the wrong payload, or the right payload combined with a wrong reference id, both
// fail. On error the inputs remain fully usable and the returned record is nil.
func MaterializeRevisit(revisit, original WarcRecord) (WarcRecord, error) {
	if revisit == nil || original == nil {
		return nil, fmt.Errorf("%w: nil record", ErrMaterializeUnsupportedType)
	}

	// The revisit is needed as the concrete type to inherit record options for the new block.
	rr, ok := revisit.(*warcRecord)
	if !ok {
		return nil, fmt.Errorf("%w: revisit is not an internally parsed record", ErrMaterializeUnsupportedType)
	}

	// Only revisit records can be materialized.
	if revisit.Type() != Revisit {
		return nil, ErrMaterializeNotRevisit
	}

	// Scope is WARC 1.1 on both sides.
	if revisit.Version() == nil || revisit.Version().id != V1_1.id {
		return nil, fmt.Errorf("%w: revisit record is not WARC/1.1", ErrMaterializeUnsupportedType)
	}
	if original.Version() == nil || original.Version().id != V1_1.id {
		return nil, fmt.Errorf("%w: original record is not WARC/1.1", ErrMaterializeUnsupportedType)
	}

	// Only the WARC 1.1 identical-payload-digest profile is supported.
	if revisit.WarcHeader().Get(WarcProfile) != ProfileIdenticalPayloadDigestV1_1 {
		return nil, fmt.Errorf("%w: %q", ErrMaterializeUnsupportedProfile, revisit.WarcHeader().Get(WarcProfile))
	}

	// Both blocks must be cached so their bytes can be read repeatedly and independently.
	if !revisit.Block().IsCached() || !original.Block().IsCached() {
		return nil, ErrMaterializeNotCached
	}

	// The revisit must carry a parsed revisit block and the original a parsed HTTP response block.
	revBlock, ok := revisit.Block().(*revisitBlock)
	if !ok {
		return nil, fmt.Errorf("%w: revisit block is %T, want *gowarc.revisitBlock", ErrMaterializeUnsupportedType, revisit.Block())
	}
	origBlock, ok := original.Block().(*httpResponseBlock)
	if !ok || original.Type() != Response {
		return nil, fmt.Errorf("%w: original must be a complete HTTP response record", ErrMaterializeUnsupportedType)
	}

	// Segmented records are out of scope.
	for _, f := range []string{WarcSegmentNumber, WarcSegmentOriginID, WarcSegmentTotalLength} {
		if revisit.WarcHeader().Has(f) || original.WarcHeader().Has(f) {
			return nil, fmt.Errorf("%w: field %s present", ErrMaterializeSegmented, f)
		}
	}

	// A truncated original cannot provide a complete payload.
	if original.WarcHeader().Has(WarcTruncated) {
		return nil, fmt.Errorf("%w: reason %q", ErrMaterializeOriginalTruncated, original.WarcHeader().Get(WarcTruncated))
	}

	revisitHeader := revisit.WarcHeader()
	originalHeader := original.WarcHeader()

	// WARC-Refers-To is mandatory and must identify the original record.
	if !revisitHeader.Has(WarcRefersTo) {
		return nil, ErrMaterializeMissingRefersTo
	}
	if revisitHeader.GetId(WarcRefersTo) != originalHeader.GetId(WarcRecordID) {
		return nil, fmt.Errorf("%w: %q does not reference %q",
			ErrMaterializeRefersToMismatch,
			revisitHeader.GetId(WarcRefersTo),
			originalHeader.GetId(WarcRecordID))
	}

	// Optional WARC-Refers-To-Target-URI is compared by raw string.
	if revisitHeader.Has(WarcRefersToTargetURI) {
		if revisitHeader.Get(WarcRefersToTargetURI) != originalHeader.Get(WarcTargetURI) {
			return nil, fmt.Errorf("%w: %q != %q",
				ErrMaterializeTargetUriMismatch,
				revisitHeader.Get(WarcRefersToTargetURI),
				originalHeader.Get(WarcTargetURI))
		}
	}

	// Optional WARC-Refers-To-Date is compared as a point in time.
	if revisitHeader.Has(WarcRefersToDate) {
		refersToDate, err := time.Parse(time.RFC3339, revisitHeader.Get(WarcRefersToDate))
		if err != nil {
			return nil, fmt.Errorf("%w: invalid WARC-Refers-To-Date: %v", ErrMaterializeRefersToDateMismatch, err)
		}
		originalDate, err := time.Parse(time.RFC3339, originalHeader.Get(WarcDate))
		if err != nil {
			return nil, fmt.Errorf("%w: invalid original WARC-Date: %v", ErrMaterializeRefersToDateMismatch, err)
		}
		if !refersToDate.Equal(originalDate) {
			return nil, fmt.Errorf("%w: %s != %s",
				ErrMaterializeRefersToDateMismatch,
				revisitHeader.Get(WarcRefersToDate),
				originalHeader.Get(WarcDate))
		}
	}

	// Snapshot the bytes we keep. Everything below works on private copies, so the inputs are
	// never mutated and no mutable block state is shared.
	httpHeaderBytes := append([]byte(nil), revBlock.ProtocolHeaderBytes()...)

	payload, err := readBlockPayload(origBlock)
	if err != nil {
		return nil, fmt.Errorf("gowarc: materialize: failed to read original payload: %w", err)
	}

	// Verify the revisit's declared payload digest against the original's real payload rather than
	// merely comparing digest strings.
	if err := verifyPayloadDigest(revisitHeader.Get(WarcPayloadDigest), payload); err != nil {
		return nil, err
	}

	// Build a fresh, independent record on top of cloned WARC headers.
	headers := revisitHeader.clone()
	headers.Set(WarcType, Response.String())
	headers.Delete(WarcRefersTo)
	headers.Delete(WarcRefersToTargetURI)
	headers.Delete(WarcRefersToDate)
	headers.Delete(WarcProfile)
	headers.Delete(WarcTruncated)

	block, blockBuffer, err := newMaterializedResponseBlock(rr.opts, headers, httpHeaderBytes, payload)
	if err != nil {
		return nil, err
	}

	blockSize := int64(len(httpHeaderBytes)) + int64(len(payload))
	headers.SetInt64(ContentLength, blockSize)
	headers.Set(WarcBlockDigest, block.BlockDigest())
	headers.Set(WarcPayloadDigest, block.PayloadDigest())

	materialized := &warcRecord{
		opts:       rr.opts,
		version:    revisit.Version(),
		recordType: Response,
		headers:    headers,
		block:      block,
		// The block's payload is a SectionReader over blockBuffer, which has no Close of its own.
		// The record therefore owns the buffer and releases it on Close.
		closer: blockBuffer.Close,
	}
	return materialized, nil
}

// readBlockPayload reads and returns the full payload bytes of a cached payload block.
// Reading does not consume a cached block, so the input stays readable afterwards.
func readBlockPayload(block PayloadBlock) ([]byte, error) {
	r, err := block.PayloadBytes()
	if err != nil {
		return nil, err
	}
	return io.ReadAll(r)
}

// verifyPayloadDigest checks a WARC digest-field value against the actual payload bytes.
// SHA-1 and SHA-256 algorithms in base16 or base32 encoding are supported. The declared encoding
// is deduced from the value itself, so equivalent digests using different encodings are accepted.
func verifyPayloadDigest(digestField string, payload []byte) error {
	if digestField == "" {
		return fmt.Errorf("%w: revisit record is missing WARC-Payload-Digest", ErrMaterializePayloadDigestMismatch)
	}

	d, err := newDigest(digestField, unknown)
	if err != nil {
		return fmt.Errorf("%w: %v", ErrMaterializePayloadDigestMismatch, err)
	}
	if d.name != "sha1" && d.name != "sha256" {
		return fmt.Errorf("%w: unsupported digest algorithm %q", ErrMaterializePayloadDigestMismatch, d.name)
	}
	if d.encoding != Base16 && d.encoding != Base32 {
		return fmt.Errorf("%w: unsupported digest encoding for %q (only base16 and base32 are supported)", ErrMaterializePayloadDigestMismatch, digestField)
	}

	if _, err := d.Write(payload); err != nil {
		return fmt.Errorf("%w: %v", ErrMaterializePayloadDigestMismatch, err)
	}
	if err := d.validate(); err != nil {
		return fmt.Errorf("%w: %v", ErrMaterializePayloadDigestMismatch, err)
	}
	return nil
}

// newMaterializedResponseBlock builds an independently owned, cached HTTP response block from
// private copies of the HTTP header bytes and payload. The block is produced through the same
// constructor used when parsing WARC records and computes fresh SHA-256 base16 block and payload
// digests over the materialized content.
//
// The returned diskbuffer.Buffer is the backing store for the block's SectionReader payload; the
// caller owns it and must keep it open while the block is in use and close it to release the
// underlying resources.
func newMaterializedResponseBlock(opts *warcRecordOptions, wf *WarcFields, httpHeaderBytes, payload []byte) (*httpResponseBlock, diskbuffer.Buffer, error) {
	blockDigest, err := newDigest("sha256", Base16)
	if err != nil {
		return nil, nil, err
	}
	payloadDigest, err := newDigest("sha256", Base16)
	if err != nil {
		return nil, nil, err
	}

	// A private, seekable buffer guarantees repeated reads and an independent Close. It is owned
	// by the caller; it is closed here only on a construction failure.
	buf := diskbuffer.New(opts.bufferOptions...)
	if _, err := buf.Write(httpHeaderBytes); err != nil {
		_ = buf.Close()
		return nil, nil, err
	}
	if _, err := buf.Write(payload); err != nil {
		_ = buf.Close()
		return nil, nil, err
	}
	if _, err := buf.Seek(0, io.SeekStart); err != nil {
		_ = buf.Close()
		return nil, nil, err
	}

	block, _, err := newHttpBlock(opts, wf, buf, blockDigest, payloadDigest)
	if err != nil {
		_ = buf.Close()
		return nil, nil, err
	}
	respBlock, ok := block.(*httpResponseBlock)
	if !ok {
		_ = buf.Close()
		return nil, nil, fmt.Errorf("%w: materialized block is %T, want *gowarc.httpResponseBlock", ErrMaterializeUnsupportedType, block)
	}
	return respBlock, buf, nil
}
