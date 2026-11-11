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
	"bufio"
	"bytes"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/base32"
	"encoding/hex"
	"io"
	"strconv"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const (
	matOriginalID   = "urn:uuid:00000000-0000-0000-0000-000000000001"
	matRevisit1ID   = "urn:uuid:00000000-0000-0000-0000-000000000002"
	matRevisit2ID   = "urn:uuid:00000000-0000-0000-0000-000000000003"
	matOtherID      = "urn:uuid:00000000-0000-0000-0000-000000000099"
	matOriginalDate = "2020-01-01T00:00:00Z"
	matRevisitDate  = "2020-02-02T00:00:00Z"
	matTargetURI    = "http://example.com/"
	matPayload      = "abc"

	// sha256("abc") in base16.
	matPayloadSHA256Base16 = "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
)

// buildHttpResponseRecord builds a cached WARC 1.1 HTTP response record with a single X-Capture
// header and the given payload. All fixtures are constructed in-memory.
func buildHttpResponseRecord(t *testing.T, recordID, date, targetURI, capture, payload string, extra ...WarcRecordOption) WarcRecord {
	t.Helper()
	opts := []WarcRecordOption{
		WithSpecViolationPolicy(ErrFail),
		WithSyntaxErrorPolicy(ErrFail),
		WithAddMissingDigest(true),
		WithDefaultDigestAlgorithm("sha256"),
		WithDefaultDigestEncoding(Base16),
	}
	opts = append(opts, extra...)

	rb := NewRecordBuilder(Response, opts...)
	httpBlock := "HTTP/1.1 200 OK\r\n" +
		"X-Capture: " + capture + "\r\n" +
		"Content-Length: " + strconv.Itoa(len(payload)) + "\r\n" +
		"Content-Type: text/plain\r\n" +
		"\r\n" +
		payload
	_, err := rb.WriteString(httpBlock)
	require.NoError(t, err)
	rb.AddWarcHeader(WarcRecordID, "<"+recordID+">")
	rb.AddWarcHeader(WarcDate, date)
	rb.AddWarcHeader(WarcTargetURI, targetURI)
	rb.AddWarcHeader(ContentType, "application/http;msgtype=response")

	rec, _, err := rb.Build()
	require.NoError(t, err)
	return rec
}

// buildRevisit derives a cached identical-payload-digest revisit from src and points it at
// targetRecordID. The revisit gets its own record id, date and target URI.
func buildRevisit(t *testing.T, src WarcRecord, revisitID, revisitDate, targetRecordID, targetURI, targetDate string) WarcRecord {
	t.Helper()
	ref := &RevisitRef{
		Profile:        ProfileIdenticalPayloadDigestV1_1,
		TargetRecordId: targetRecordID,
		TargetUri:      targetURI,
		TargetDate:     targetDate,
	}
	rev, err := src.ToRevisitRecord(ref)
	require.NoError(t, err)
	rev.WarcHeader().SetId(WarcRecordID, "<"+revisitID+">")
	rev.WarcHeader().Set(WarcDate, revisitDate)
	rev.WarcHeader().Set(WarcTargetURI, targetURI)
	return rev
}

func newMaterializeOriginal(t *testing.T) WarcRecord {
	t.Helper()
	return buildHttpResponseRecord(t, matOriginalID, matOriginalDate, matTargetURI, "old", matPayload)
}

func newMaterializeRevisitPair(t *testing.T, original WarcRecord) (WarcRecord, WarcRecord) {
	t.Helper()
	src1 := buildHttpResponseRecord(t, matRevisit1ID, matRevisitDate, matTargetURI, "first", matPayload)
	t.Cleanup(func() { _ = src1.Close() })
	src2 := buildHttpResponseRecord(t, matRevisit2ID, matRevisitDate, matTargetURI, "second", matPayload)
	t.Cleanup(func() { _ = src2.Close() })

	rev1 := buildRevisit(t, src1, matRevisit1ID, matRevisitDate, matOriginalID, matTargetURI, matOriginalDate)
	rev2 := buildRevisit(t, src2, matRevisit2ID, matRevisitDate, matOriginalID, matTargetURI, matOriginalDate)
	return rev1, rev2
}

func readAll(t *testing.T, r io.Reader) []byte {
	t.Helper()
	b, err := io.ReadAll(r)
	require.NoError(t, err)
	return b
}

func blockRaw(t *testing.T, rec WarcRecord) []byte {
	t.Helper()
	r, err := rec.Block().RawBytes()
	require.NoError(t, err)
	return readAll(t, r)
}

func blockPayload(t *testing.T, rec WarcRecord) []byte {
	t.Helper()
	pb, ok := rec.Block().(PayloadBlock)
	require.True(t, ok, "block must be a PayloadBlock")
	r, err := pb.PayloadBytes()
	require.NoError(t, err)
	return readAll(t, r)
}

func sha256Base16(t *testing.T, data []byte) string {
	t.Helper()
	sum := sha256.Sum256(data)
	return "sha256:" + hex.EncodeToString(sum[:])
}

func marshalRecord(t *testing.T, rec WarcRecord) []byte {
	t.Helper()
	var buf bytes.Buffer
	_, _, err := NewMarshaler().Marshal(&buf, rec, 0)
	require.NoError(t, err)
	return buf.Bytes()
}

// TestMaterializeRevisit_SuccessAndIndependence is the minimal scenario: an original carrying
// "X-Capture: old" with body "abc", and two revisits carrying "X-Capture: first"/"second". Both are
// materialized, read back cross-wise, and the original plus the first result are closed. The second
// result must remain its own header plus "abc".
func TestMaterializeRevisit_SuccessAndIndependence(t *testing.T) {
	original := newMaterializeOriginal(t)
	rev1, rev2 := newMaterializeRevisitPair(t, original)

	// Snapshot inputs so we can prove they are never mutated.
	rev1HeaderBefore := append([]byte(nil), rev1.Block().(ProtocolHeaderBlock).ProtocolHeaderBytes()...)
	rev2HeaderBefore := append([]byte(nil), rev2.Block().(ProtocolHeaderBlock).ProtocolHeaderBytes()...)
	originalHeaderBefore := append([]byte(nil), original.Block().(ProtocolHeaderBlock).ProtocolHeaderBytes()...)

	mat1, err := MaterializeRevisit(rev1, original)
	require.NoError(t, err)
	mat2, err := MaterializeRevisit(rev2, original)
	require.NoError(t, err)

	// Each result keeps its own revisit header and the original payload.
	assert.Equal(t, "HTTP/1.1 200 OK\r\nX-Capture: first\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\n",
		string(mat1.Block().(ProtocolHeaderBlock).ProtocolHeaderBytes()))
	assert.Equal(t, "HTTP/1.1 200 OK\r\nX-Capture: second\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\n",
		string(mat2.Block().(ProtocolHeaderBlock).ProtocolHeaderBytes()))
	assert.Equal(t, []byte(matPayload), blockPayload(t, mat1))
	assert.Equal(t, []byte(matPayload), blockPayload(t, mat2))
	assert.Equal(t, "first", mat1.Block().(HttpResponseBlock).HttpHeader().Get("X-Capture"))
	assert.Equal(t, "second", mat2.Block().(HttpResponseBlock).HttpHeader().Get("X-Capture"))

	// Mutating one result's headers must not touch the other.
	mat1.WarcHeader().Set("X-Materialized-Note", "one")
	assert.False(t, mat2.WarcHeader().Has("X-Materialized-Note"))

	// Close the original and the first result; the second must stay fully readable on its own store.
	require.NoError(t, original.Close())
	require.NoError(t, mat1.Close())

	assert.Equal(t, []byte(matPayload), blockPayload(t, mat2), "second result payload after other closures")
	assert.Equal(t, "second", mat2.Block().(HttpResponseBlock).HttpHeader().Get("X-Capture"))
	// Cached block remains repeatably readable.
	assert.Equal(t, []byte(matPayload), blockPayload(t, mat2))

	// The closed first result really released its backing store: reading through fails.
	closedReader, err := mat1.Block().RawBytes()
	require.NoError(t, err)
	_, err = io.ReadAll(closedReader)
	assert.Error(t, err, "reading a closed materialized record should fail")

	// Inputs were not mutated: types, cached state and header bytes are unchanged.
	assert.Equal(t, Revisit, rev1.Type())
	assert.Equal(t, Revisit, rev2.Type())
	assert.True(t, rev1.Block().IsCached())
	assert.True(t, rev2.Block().IsCached())
	assert.Equal(t, rev1HeaderBefore, rev1.Block().(ProtocolHeaderBlock).ProtocolHeaderBytes())
	assert.Equal(t, rev2HeaderBefore, rev2.Block().(ProtocolHeaderBlock).ProtocolHeaderBytes())
	// original was closed above, so compare the snapshot taken earlier against the expected value.
	assert.Equal(t, "HTTP/1.1 200 OK\r\nX-Capture: old\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\n",
		string(originalHeaderBefore))

	require.NoError(t, rev1.Close())
	require.NoError(t, rev2.Close())
	require.NoError(t, mat2.Close())
}

// TestMaterializeRevisit_ResultFields checks the header transformation, digests and content length
// of a successfully materialized record.
func TestMaterializeRevisit_ResultFields(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, original)
	defer func() { require.NoError(t, rev1.Close()) }()

	mat, err := MaterializeRevisit(rev1, original)
	require.NoError(t, err)
	defer func() { require.NoError(t, mat.Close()) }()

	h := mat.WarcHeader()
	// Identity fields come from the revisit.
	assert.Equal(t, Response, mat.Type())
	assert.Equal(t, V1_1, mat.Version())
	assert.Equal(t, matRevisit1ID, mat.RecordId())
	assert.Equal(t, matRevisitDate, h.Get(WarcDate))
	assert.Equal(t, matTargetURI, h.Get(WarcTargetURI))

	// Revisit-only fields are gone, including the truncation that only existed because the body was omitted.
	assert.False(t, h.Has(WarcRefersTo))
	assert.False(t, h.Has(WarcRefersToTargetURI))
	assert.False(t, h.Has(WarcRefersToDate))
	assert.False(t, h.Has(WarcProfile))
	assert.False(t, h.Has(WarcTruncated))

	// The revisit itself is untouched.
	assert.Equal(t, Revisit, rev1.Type())
	assert.True(t, rev1.WarcHeader().Has(WarcProfile))
	assert.True(t, rev1.WarcHeader().Has(WarcTruncated))

	// Content-Length is the real WARC block size.
	raw := blockRaw(t, mat)
	contentLength, err := mat.ContentLength()
	require.NoError(t, err)
	assert.Equal(t, int64(len(raw)), contentLength)

	// Digests are SHA-256 base16, freshly computed over the new header + original payload, and are
	// not copied from the original.
	blockDigest := h.Get(WarcBlockDigest)
	payloadDigest := h.Get(WarcPayloadDigest)
	assert.True(t, strings.HasPrefix(blockDigest, "sha256:"))
	assert.True(t, strings.HasPrefix(payloadDigest, "sha256:"))
	assert.Equal(t, sha256Base16(t, raw), blockDigest)
	assert.Equal(t, matPayloadSHA256Base16, payloadDigest)
	assert.Equal(t, sha256Base16(t, blockPayload(t, mat)), payloadDigest)
	assert.NotEqual(t, original.Block().BlockDigest(), blockDigest, "block digest must be recomputed, not copied")
}

// TestMaterializeRevisit_NotRevisit ensures a non-revisit first argument is rejected.
func TestMaterializeRevisit_NotRevisit(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()

	_, err := MaterializeRevisit(original, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeNotRevisit)
	assert.Equal(t, Response, original.Type(), "input must be unchanged after failure")
}

// TestMaterializeRevisit_UnsupportedProfile rejects anything but the WARC 1.1
// identical-payload-digest profile.
func TestMaterializeRevisit_UnsupportedProfile(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, original)
	defer func() { require.NoError(t, rev1.Close()) }()
	rev1.WarcHeader().Set(WarcProfile, ProfileServerNotModifiedV1_1)

	_, err := MaterializeRevisit(rev1, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeUnsupportedProfile)
}

// TestMaterializeRevisit_OriginalNotResponse rejects an original that is not a complete HTTP
// response record even though the reference id matches.
func TestMaterializeRevisit_OriginalNotResponse(t *testing.T) {
	// A resource record is a valid record but not an HTTP response.
	rb := NewRecordBuilder(Resource,
		WithSpecViolationPolicy(ErrFail),
		WithSyntaxErrorPolicy(ErrFail),
		WithAddMissingDigest(true),
		WithDefaultDigestEncoding(Base16),
	)
	_, err := rb.WriteString(matPayload)
	require.NoError(t, err)
	rb.AddWarcHeader(WarcRecordID, "<"+matOriginalID+">")
	rb.AddWarcHeader(WarcDate, matOriginalDate)
	rb.AddWarcHeader(WarcTargetURI, matTargetURI)
	rb.AddWarcHeader(ContentType, "text/plain")
	resource, _, err := rb.Build()
	require.NoError(t, err)
	defer func() { require.NoError(t, resource.Close()) }()

	cachedOriginal := newMaterializeOriginal(t)
	defer func() { require.NoError(t, cachedOriginal.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, cachedOriginal)
	defer func() { require.NoError(t, rev1.Close()) }()

	_, err = MaterializeRevisit(rev1, resource)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeUnsupportedType)
	assert.Equal(t, Revisit, rev1.Type(), "revisit input must be unchanged")
}

// TestMaterializeRevisit_NotCached rejects an uncached original.
func TestMaterializeRevisit_NotCached(t *testing.T) {
	original := newMaterializeOriginal(t)
	rawOriginal := marshalRecord(t, original)
	require.NoError(t, original.Close())

	// io.NopCloser hides the Seek method, so the parsed block is not cached.
	nonSeekable := io.NopCloser(strings.NewReader(string(rawOriginal)))
	u := NewUnmarshaler(WithSpecViolationPolicy(ErrIgnore), WithSyntaxErrorPolicy(ErrIgnore))
	uncachedOriginal, _, _, err := u.Unmarshal(bufio.NewReader(nonSeekable))
	require.NoError(t, err)
	defer func() { require.NoError(t, uncachedOriginal.Close()) }()
	require.False(t, uncachedOriginal.Block().IsCached())

	cachedOriginal := newMaterializeOriginal(t)
	defer func() { require.NoError(t, cachedOriginal.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, cachedOriginal)
	defer func() { require.NoError(t, rev1.Close()) }()

	_, err = MaterializeRevisit(rev1, uncachedOriginal)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeNotCached)
}

// TestMaterializeRevisit_Segmented rejects records carrying segmentation fields.
func TestMaterializeRevisit_Segmented(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, original)
	defer func() { require.NoError(t, rev1.Close()) }()
	rev1.WarcHeader().Set(WarcSegmentNumber, "1")

	_, err := MaterializeRevisit(rev1, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeSegmented)
}

// TestMaterializeRevisit_OriginalTruncated rejects an original declaring WARC-Truncated.
func TestMaterializeRevisit_OriginalTruncated(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	original.WarcHeader().Set(WarcTruncated, "length")

	rev1, _ := newMaterializeRevisitPair(t, newMaterializeOriginal(t))
	defer func() { require.NoError(t, rev1.Close()) }()

	_, err := MaterializeRevisit(rev1, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeOriginalTruncated)
}

// TestMaterializeRevisit_MissingRefersTo rejects a revisit without WARC-Refers-To.
func TestMaterializeRevisit_MissingRefersTo(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, original)
	defer func() { require.NoError(t, rev1.Close()) }()
	rev1.WarcHeader().Delete(WarcRefersTo)

	_, err := MaterializeRevisit(rev1, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeMissingRefersTo)
}

// TestMaterializeRevisit_RefersToMismatch uses the correct body but a wrong reference id and must fail.
func TestMaterializeRevisit_RefersToMismatch(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, original)
	defer func() { require.NoError(t, rev1.Close()) }()
	rev1.WarcHeader().SetId(WarcRefersTo, "<"+matOtherID+">")

	_, err := MaterializeRevisit(rev1, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeRefersToMismatch)
}

// TestMaterializeRevisit_TargetUriMismatch rejects a mismatched optional target URI.
func TestMaterializeRevisit_TargetUriMismatch(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, original)
	defer func() { require.NoError(t, rev1.Close()) }()
	rev1.WarcHeader().Set(WarcRefersToTargetURI, "http://different.example/")

	_, err := MaterializeRevisit(rev1, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeTargetUriMismatch)
}

// TestMaterializeRevisit_RefersToDate covers a mismatched reference date and an equivalent instant
// expressed differently.
func TestMaterializeRevisit_RefersToDate(t *testing.T) {
	t.Run("mismatched date", func(t *testing.T) {
		original := newMaterializeOriginal(t)
		defer func() { require.NoError(t, original.Close()) }()
		rev1, _ := newMaterializeRevisitPair(t, original)
		defer func() { require.NoError(t, rev1.Close()) }()
		rev1.WarcHeader().Set(WarcRefersToDate, "2019-12-31T00:00:00Z")

		_, err := MaterializeRevisit(rev1, original)
		require.Error(t, err)
		assert.ErrorIs(t, err, ErrMaterializeRefersToDateMismatch)
	})

	t.Run("same instant different textual representation", func(t *testing.T) {
		original := newMaterializeOriginal(t)
		defer func() { require.NoError(t, original.Close()) }()
		rev1, _ := newMaterializeRevisitPair(t, original)
		defer func() { require.NoError(t, rev1.Close()) }()
		// 2020-01-01T00:00:00Z expressed with a non-UTC offset denotes the same instant.
		rev1.WarcHeader().Set(WarcRefersToDate, "2020-01-01T02:00:00+02:00")

		mat, err := MaterializeRevisit(rev1, original)
		require.NoError(t, err)
		require.NoError(t, mat.Close())
	})
}

// TestMaterializeRevisit_ForgedPayloadDigest covers a correct reference id paired with the wrong
// body: the declared digest cannot be verified against the original's real payload.
func TestMaterializeRevisit_ForgedPayloadDigest(t *testing.T) {
	// Original carries a different body than the digest the revisit claims.
	original := buildHttpResponseRecord(t, matOriginalID, matOriginalDate, matTargetURI, "old", "xyz")
	defer func() { require.NoError(t, original.Close()) }()

	src := buildHttpResponseRecord(t, matRevisit1ID, matRevisitDate, matTargetURI, "first", matPayload)
	defer func() { require.NoError(t, src.Close()) }()
	rev := buildRevisit(t, src, matRevisit1ID, matRevisitDate, matOriginalID, matTargetURI, matOriginalDate)
	defer func() { require.NoError(t, rev.Close()) }()
	// Forge the declared digest to describe "abc" while the original body is "xyz".
	rev.WarcHeader().Set(WarcPayloadDigest, matPayloadSHA256Base16)

	_, err := MaterializeRevisit(rev, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializePayloadDigestMismatch)
}

// TestMaterializeRevisit_TamperedDigestText rejects a correct reference and correct body when the
// revisit's declared digest string itself does not match the real payload.
func TestMaterializeRevisit_TamperedDigestText(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, original)
	defer func() { require.NoError(t, rev1.Close()) }()

	// Same algorithm/encoding and body, but a non-matching hash.
	rev1.WarcHeader().Set(WarcPayloadDigest, "sha256:"+strings.Repeat("0", 64))

	_, err := MaterializeRevisit(rev1, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializePayloadDigestMismatch)
	// Inputs stay usable after the failure.
	assert.Equal(t, []byte(matPayload), blockPayload(t, original))
}

// TestMaterializeRevisit_DigestEncodings verifies SHA-1/SHA-256 digests in base16 and base32,
// including the same digest represented in different encodings, and rejects out-of-scope encodings
// and algorithms.
func TestMaterializeRevisit_DigestEncodings(t *testing.T) {
	sha1Sum := sha1.Sum([]byte(matPayload))
	sha256Sum := sha256.Sum256([]byte(matPayload))
	sha1Base16 := "sha1:" + hex.EncodeToString(sha1Sum[:])
	sha1Base32 := "sha1:" + base32.StdEncoding.WithPadding(base32.NoPadding).EncodeToString(sha1Sum[:])
	sha256Base32 := "sha256:" + base32.StdEncoding.WithPadding(base32.NoPadding).EncodeToString(sha256Sum[:])
	sha256Base64 := func() string {
		d, err := newDigest("sha256", Base64)
		require.NoError(t, err)
		_, err = d.Write([]byte(matPayload))
		require.NoError(t, err)
		return d.format()
	}()
	md5Base16 := func() string {
		d, err := newDigest("md5", Base16)
		require.NoError(t, err)
		_, err = d.Write([]byte(matPayload))
		require.NoError(t, err)
		return d.format()
	}()

	tests := []struct {
		name        string
		digestField string
		wantErr     bool
		wantErrIs   error
	}{
		{"sha1 base16", sha1Base16, false, nil},
		{"sha1 base32", sha1Base32, false, nil},
		{"sha256 base16", matPayloadSHA256Base16, false, nil},
		{"sha256 base32", sha256Base32, false, nil},
		{"sha256 base64 rejected", sha256Base64, true, ErrMaterializePayloadDigestMismatch},
		{"md5 rejected", md5Base16, true, ErrMaterializePayloadDigestMismatch},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			original := newMaterializeOriginal(t)
			defer func() { require.NoError(t, original.Close()) }()
			rev, _ := newMaterializeRevisitPair(t, original)
			defer func() { require.NoError(t, rev.Close()) }()
			rev.WarcHeader().Set(WarcPayloadDigest, tt.digestField)

			mat, err := MaterializeRevisit(rev, original)
			if tt.wantErr {
				require.Error(t, err)
				assert.ErrorIs(t, err, tt.wantErrIs)
				assert.Nil(t, mat)
				return
			}
			require.NoError(t, err)
			// Output digests are always SHA-256 base16 regardless of the input encoding.
			assert.Equal(t, matPayloadSHA256Base16, mat.WarcHeader().Get(WarcPayloadDigest))
			require.NoError(t, mat.Close())
		})
	}
}

// TestMaterializeRevisit_InputsSurviveFailureAndRetry proves a failed materialization leaves both
// inputs fully usable, and that a correct attempt on the same inputs succeeds afterwards.
func TestMaterializeRevisit_InputsSurviveFailureAndRetry(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, original)
	defer func() { require.NoError(t, rev1.Close()) }()

	// First attempt fails because of a wrong reference id.
	rev1.WarcHeader().SetId(WarcRefersTo, "<"+matOtherID+">")
	_, err := MaterializeRevisit(rev1, original)
	require.ErrorIs(t, err, ErrMaterializeRefersToMismatch)

	// The inputs are unchanged and still readable after the failure.
	assert.Equal(t, Revisit, rev1.Type())
	assert.Equal(t, Response, original.Type())
	assert.True(t, original.Block().IsCached())
	assert.Equal(t, []byte(matPayload), blockPayload(t, original))
	assert.True(t, rev1.Block().IsCached())
	assert.Contains(t, string(rev1.Block().(ProtocolHeaderBlock).ProtocolHeaderBytes()), "X-Capture: first")

	// Correct the reference and retry successfully.
	rev1.WarcHeader().SetId(WarcRefersTo, "<"+matOriginalID+">")
	mat, err := MaterializeRevisit(rev1, original)
	require.NoError(t, err)
	defer func() { require.NoError(t, mat.Close()) }()
	assert.Equal(t, []byte(matPayload), blockPayload(t, mat))
}

// TestMaterializeRevisit_UnsupportedVersion rejects a revisit that is not WARC/1.1.
func TestMaterializeRevisit_UnsupportedVersion(t *testing.T) {
	original := newMaterializeOriginal(t)
	defer func() { require.NoError(t, original.Close()) }()
	rev1, _ := newMaterializeRevisitPair(t, original)
	defer func() { require.NoError(t, rev1.Close()) }()

	rev1.(*warcRecord).version = V1_0
	_, err := MaterializeRevisit(rev1, original)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeUnsupportedType)
}

// TestMaterializeRevisit_NilInputs rejects nil arguments.
func TestMaterializeRevisit_NilInputs(t *testing.T) {
	_, err := MaterializeRevisit(nil, nil)
	require.Error(t, err)
	assert.ErrorIs(t, err, ErrMaterializeUnsupportedType)
}

// TestMaterializeRevisit_RoundTrip serializes both materialized records with the repository's
// marshaler, reads them back through the unmarshaler and verifies the stored block and payload
// digests against the actual bytes.
func TestMaterializeRevisit_RoundTrip(t *testing.T) {
	original := newMaterializeOriginal(t)
	rev1, rev2 := newMaterializeRevisitPair(t, original)

	mat1, err := MaterializeRevisit(rev1, original)
	require.NoError(t, err)
	mat2, err := MaterializeRevisit(rev2, original)
	require.NoError(t, err)

	wantBlockDigest1 := mat1.WarcHeader().Get(WarcBlockDigest)
	wantPayloadDigest1 := mat1.WarcHeader().Get(WarcPayloadDigest)
	wantBlockDigest2 := mat2.WarcHeader().Get(WarcBlockDigest)
	wantPayloadDigest2 := mat2.WarcHeader().Get(WarcPayloadDigest)

	stream := append(marshalRecord(t, mat1), marshalRecord(t, mat2)...)
	require.NoError(t, mat1.Close())
	require.NoError(t, mat2.Close())
	require.NoError(t, original.Close())
	require.NoError(t, rev1.Close())
	require.NoError(t, rev2.Close())

	reader, err := NewWarcFileReaderFromStream(bytes.NewReader(stream), 0)
	require.NoError(t, err)
	defer func() { require.NoError(t, reader.Close()) }()

	want := []struct {
		id, capture, blockDigest, payloadDigest string
	}{
		{matRevisit1ID, "first", wantBlockDigest1, wantPayloadDigest1},
		{matRevisit2ID, "second", wantBlockDigest2, wantPayloadDigest2},
	}
	for i, w := range want {
		rec, err := reader.Next()
		require.NoError(t, err, "record %d", i)
		require.NotNil(t, rec.WarcRecord)
		readback := rec.WarcRecord
		require.NoError(t, readback.Block().Cache())

		// Identity and transformed headers survive serialization.
		assert.Equal(t, Response, readback.Type())
		assert.Equal(t, w.id, readback.RecordId())
		assert.Equal(t, matRevisitDate, readback.WarcHeader().Get(WarcDate))
		assert.Equal(t, matTargetURI, readback.WarcHeader().Get(WarcTargetURI))
		assert.False(t, readback.WarcHeader().Has(WarcProfile))
		assert.False(t, readback.WarcHeader().Has(WarcRefersTo))
		assert.False(t, readback.WarcHeader().Has(WarcTruncated))
		assert.Equal(t, w.capture, readback.Block().(HttpResponseBlock).HttpHeader().Get("X-Capture"))

		// Digests in the header match both the expected values and the actual read-back bytes.
		raw := blockRaw(t, readback)
		payload := blockPayload(t, readback)
		assert.Equal(t, []byte(matPayload), payload)
		assert.Equal(t, w.blockDigest, readback.WarcHeader().Get(WarcBlockDigest))
		assert.Equal(t, w.payloadDigest, readback.WarcHeader().Get(WarcPayloadDigest))
		assert.Equal(t, sha256Base16(t, raw), readback.WarcHeader().Get(WarcBlockDigest))
		assert.Equal(t, sha256Base16(t, payload), readback.WarcHeader().Get(WarcPayloadDigest))

		contentLength, err := readback.ContentLength()
		require.NoError(t, err)
		assert.Equal(t, int64(len(raw)), contentLength)

		validation, vErr := readback.ValidateDigest()
		assert.NoError(t, vErr)
		assert.Empty(t, validation)
		require.NoError(t, readback.Close())
	}
}
