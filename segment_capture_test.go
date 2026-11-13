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
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"

	"github.com/klauspost/compress/gzip"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// physicalRecord is the result of parsing one physical WARC record directly
// from bytes, independently of the package's Unmarshaler.
type physicalRecord struct {
	version string
	headers []nameValue
	block   []byte
}

func (p *physicalRecord) header(name string) string {
	for _, nv := range p.headers {
		if strings.EqualFold(nv.Name, name) {
			return nv.Value
		}
	}
	return ""
}

func (p *physicalRecord) contentLength(t *testing.T) int64 {
	n, err := strconv.ParseInt(p.header(ContentLength), 10, 64)
	require.NoError(t, err)
	return n
}

// parsePhysicalWARC splits a byte stream into physical WARC records by reading
// the version line, header fields and Content-Length delimited block directly.
func parsePhysicalWARC(t *testing.T, data []byte) []physicalRecord {
	t.Helper()
	var records []physicalRecord
	for len(data) > 0 {
		idx := bytes.Index(data, []byte("\r\n"))
		require.NotEqual(t, -1, idx, "missing version line terminator")
		version := string(data[:idx])
		require.True(t, strings.HasPrefix(version, "WARC/"), "unexpected version line %q", version)
		data = data[idx+2:]

		var headers []nameValue
		for {
			idx = bytes.Index(data, []byte("\r\n"))
			require.NotEqual(t, -1, idx, "unterminated header line")
			line := data[:idx]
			data = data[idx+2:]
			if len(line) == 0 {
				break // blank line separates header from block
			}
			colon := bytes.IndexByte(line, ':')
			require.NotEqual(t, -1, colon, "malformed header line %q", line)
			headers = append(headers, nameValue{
				Name:  string(line[:colon]),
				Value: strings.TrimPrefix(string(line[colon:]), ": "),
			})
		}

		cl, err := strconv.ParseInt(physicalHeader(headers, ContentLength), 10, 64)
		require.NoError(t, err)
		require.GreaterOrEqual(t, int64(len(data)), cl+4, "truncated block")
		block := data[:cl]
		data = data[cl:]
		require.True(t, bytes.Equal(data[:4], crlfcrlf), "missing end-of-record marker")
		data = data[4:]

		records = append(records, physicalRecord{
			version: version,
			headers: headers,
			block:   append([]byte(nil), block...),
		})
	}
	return records
}

func physicalHeader(headers []nameValue, name string) string {
	for _, nv := range headers {
		if strings.EqualFold(nv.Name, name) {
			return nv.Value
		}
	}
	return ""
}

// newLocalHTTPResponseRecord builds a cached response record from a local,
// in-memory HTTP response, so no network port is required.
func newLocalHTTPResponseRecord(t *testing.T, body string) (WarcRecord, []byte) {
	t.Helper()
	raw := "HTTP/1.1 200 OK\r\n" +
		"Content-Type: text/plain\r\n" +
		"Content-Length: " + strconv.Itoa(len(body)) + "\r\n" +
		"\r\n" + body
	resp, err := http.ReadResponse(bufio.NewReader(strings.NewReader(raw)), nil)
	require.NoError(t, err)

	builder := NewRecordBuilder(Response,
		WithSpecViolationPolicy(ErrIgnore),
		WithSyntaxErrorPolicy(ErrIgnore),
		WithAddMissingDigest(true))
	builder.AddWarcHeader(WarcRecordID, "<urn:uuid:11111111-1111-1111-1111-111111111111>")
	builder.AddWarcHeader(WarcDate, "2026-01-01T00:00:00Z")
	builder.AddWarcHeader(WarcTargetURI, "http://example.com/capture")
	builder.AddWarcHeader(ContentType, ApplicationHttp+";msgtype=response")
	require.NoError(t, resp.Write(builder))

	record, _, err := builder.Build()
	require.NoError(t, err)
	require.True(t, record.Block().IsCached(), "record must be cached to be segmentable")

	r, err := record.Block().RawBytes()
	require.NoError(t, err)
	rawBlock, err := io.ReadAll(r)
	require.NoError(t, err)
	return record, rawBlock
}

// marshalAllSegments drives Marshal until the chain is fully written, closing
// each consumed continuation. It returns the concatenated output and per
// segment byte counts.
func marshalAllSegments(t *testing.T, m Marshaler, first WarcRecord, maxSize int64) ([]byte, []int64) {
	t.Helper()
	var out bytes.Buffer
	var sizes []int64
	current := first
	for current != nil {
		var seg bytes.Buffer
		next, n, err := m.Marshal(&seg, current, maxSize)
		require.NoError(t, err)
		require.Equal(t, int64(seg.Len()), n)
		require.LessOrEqual(t, n, maxSize)
		require.Greater(t, n, int64(0))
		sizes = append(sizes, n)
		_, _ = out.Write(seg.Bytes())
		if current != first {
			require.NoError(t, current.Close())
		}
		current = next
	}
	return out.Bytes(), sizes
}

// sha256Base16 returns the lower-case hex SHA-256 digest of data.
func sha256Base16(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// TestCaptureSegment_MarshalResponse drives a local HTTP response through
// segmented Marshal and independently parses the physical WARC records,
// checking byte budgets, maximal prefixes, per-segment digests, segment header
// semantics and byte-exact reassembly across decimal-width boundaries.
func TestCaptureSegment_MarshalResponse(t *testing.T) {
	body := strings.Repeat("capture-", 600) // 4800 payload bytes
	record, rawBlock := newLocalHTTPResponseRecord(t, body)
	defer func() { assert.NoError(t, record.Close()) }()

	const maxSize int64 = 1500
	out, sizes := marshalAllSegments(t, NewMarshaler(), record, maxSize)
	require.Greater(t, len(sizes), 1, "record should have been segmented")

	// Marshal must not mutate the caller's header: the input record still shows
	// no segmentation fields and keeps its original type and identity.
	assert.False(t, record.WarcHeader().Has(WarcSegmentNumber))
	assert.False(t, record.WarcHeader().Has(WarcSegmentOriginID))
	assert.False(t, record.WarcHeader().Has(WarcSegmentTotalLength))
	assert.Equal(t, Response.String(), record.WarcHeader().Get(WarcType))

	records := parsePhysicalWARC(t, out)
	require.Len(t, records, len(sizes))

	var reassembled bytes.Buffer
	digitWidths := map[int]bool{}
	originID := ""

	for i, pr := range records {
		// Every physical record stays within the uncompressed byte budget.
		assert.LessOrEqual(t, sizes[i], maxSize)
		assert.Equal(t, "WARC/1.1", pr.version)

		cl := pr.contentLength(t)
		assert.Equal(t, int64(len(pr.block)), cl)
		digitWidths[len(strconv.FormatInt(cl, 10))] = true
		reassembled.Write(pr.block)

		// Block-Digest is SHA-256 base16 over this segment's real block bytes.
		digest := pr.header(WarcBlockDigest)
		assert.True(t, strings.HasPrefix(digest, "sha256:"))
		assert.Equal(t, "sha256:"+sha256Base16(pr.block), digest)

		assert.Equal(t, strconv.Itoa(i+1), pr.header(WarcSegmentNumber))

		if i == 0 {
			// First segment keeps the original type and identity.
			assert.Equal(t, Response.String(), pr.header(WarcType))
			assert.Equal(t, "<urn:uuid:11111111-1111-1111-1111-111111111111>", pr.header(WarcRecordID))
			assert.Equal(t, "http://example.com/capture", pr.header(WarcTargetURI))
			assert.Equal(t, "2026-01-01T00:00:00Z", pr.header(WarcDate))
			assert.False(t, physicalHas(pr.headers, WarcSegmentOriginID))
			// The original payload digest still describes the full logical response.
			assert.NotEmpty(t, pr.header(WarcPayloadDigest))
			// The HTTP header appears only in the first segment.
			assert.True(t, bytes.HasPrefix(pr.block, []byte("HTTP/1.1 200 OK")))
			originID = strings.Trim(pr.header(WarcRecordID), "<>")
		} else {
			// Continuations have their own ids but the same target and date.
			assert.Equal(t, Continuation.String(), pr.header(WarcType))
			assert.NotEqual(t, "<urn:uuid:11111111-1111-1111-1111-111111111111>", pr.header(WarcRecordID))
			assert.Equal(t, "http://example.com/capture", pr.header(WarcTargetURI))
			assert.Equal(t, "2026-01-01T00:00:00Z", pr.header(WarcDate))
			assert.Equal(t, originID, strings.Trim(pr.header(WarcSegmentOriginID), "<>"))
			// Payload digest and HTTP headers are never copied to continuations.
			assert.False(t, physicalHas(pr.headers, WarcPayloadDigest))
			assert.False(t, bytes.HasPrefix(pr.block, []byte("HTTP/1.1")))
		}

		if i == len(records)-1 {
			// Only the final segment carries the total logical block length.
			total, err := strconv.ParseInt(pr.header(WarcSegmentTotalLength), 10, 64)
			require.NoError(t, err)
			assert.Equal(t, int64(len(rawBlock)), total)
		} else {
			assert.False(t, physicalHas(pr.headers, WarcSegmentTotalLength))

			// Maximal-prefix check: one more content byte must exceed maxSize for
			// the same non-final header framing.
			grown := cl + 1
			grownSize := physicalSizeWithContentLength(pr, grown, false)
			assert.Greater(t, grownSize, maxSize,
				"segment %d was not the largest fitting prefix", i+1)
		}
	}

	// The raw block bytes are cut contiguously and reassemble byte-for-byte.
	assert.Equal(t, rawBlock, reassembled.Bytes())

	// The chain must cross at least one decimal-width boundary of Content-Length.
	assert.GreaterOrEqual(t, len(digitWidths), 2, "expected Content-Length widths to vary: %v", digitWidths)
}

// physicalHas reports whether a parsed physical header contains the field.
func physicalHas(headers []nameValue, name string) bool {
	return physicalHeader(headers, name) != ""
}

// physicalSizeWithContentLength recomputes the physical size of a non-final
// segment if its Content-Length were grownTo bytes, keeping every other header
// field. The block digest keeps the fixed SHA-256 base16 width. Non-final
// segments carry no WARC-Segment-Total-Length.
func physicalSizeWithContentLength(pr physicalRecord, grownTo int64, final bool) int64 {
	var headerBytes int64
	for _, nv := range pr.headers {
		switch {
		case strings.EqualFold(nv.Name, ContentLength):
			headerBytes += int64(len(nv.Name)) + 2 + int64(len(strconv.FormatInt(grownTo, 10))) + 2
		case strings.EqualFold(nv.Name, WarcSegmentTotalLength) && !final:
			// not present in a non-final segment
		default:
			headerBytes += int64(len(nv.Name)) + 2 + int64(len(nv.Value)) + 2
		}
	}
	return int64(len(pr.version)) + 2 + headerBytes + 2 + grownTo + 4
}

// physicalRecordLen returns the total serialized length of a parsed physical
// record: version line, header fields, blank line, block and end marker.
func physicalRecordLen(pr physicalRecord) int64 {
	return physicalRecordOverhead(pr) + int64(len(pr.block))
}

// TestCaptureSegment_CloseAndShortWrite verifies that closing the input record
// or an already written continuation does not invalidate unwritten suffixes,
// and that a failing writer surfaces the original error, the real byte count
// and no continuation.
func TestCaptureSegment_CloseAndShortWrite(t *testing.T) {
	body := strings.Repeat("abcdefgh", 500) // 4000 bytes
	record, rawBlock := newLocalHTTPResponseRecord(t, body)

	m := NewMarshaler()
	const maxSize int64 = 800

	// First segment, then close the original input record.
	var first bytes.Buffer
	next, n1, err := m.Marshal(&first, record, maxSize)
	require.NoError(t, err)
	require.NotNil(t, next)
	assert.NoError(t, record.Close())

	// The suffix stays valid after the input record was closed.
	var second bytes.Buffer
	next2, n2, err := m.Marshal(&second, next, maxSize)
	require.NoError(t, err)
	assert.NoError(t, next.Close()) // closing an already written continuation is safe

	var rest bytes.Buffer
	var sizes []int64
	sizes = append(sizes, n1, n2)
	current := next2
	for current != nil {
		var seg bytes.Buffer
		cont, n, mErr := m.Marshal(&seg, current, maxSize)
		require.NoError(t, mErr)
		sizes = append(sizes, n)
		_, _ = rest.Write(seg.Bytes())
		assert.NoError(t, current.Close())
		current = cont
	}

	// Reassemble everything and compare against the original block.
	all := append(append(append([]byte(nil), first.Bytes()...), second.Bytes()...), rest.Bytes()...)
	records := parsePhysicalWARC(t, all)
	var reassembled bytes.Buffer
	for _, pr := range records {
		reassembled.Write(pr.block)
	}
	assert.Equal(t, rawBlock, reassembled.Bytes())
	assert.Greater(t, len(records), 2)

	// Short write: the writer accepts exactly the 10-byte version line, then
	// fails on the header write. No continuation may be handed back and the
	// reported count must equal the bytes actually accepted.
	failingRecord, _ := newLocalHTTPResponseRecord(t, body)
	defer func() { _ = failingRecord.Close() }()
	fw := &errWriter{n: 10, err: fmt.Errorf("disk full")}
	continuation, written, werr := m.Marshal(fw, failingRecord, maxSize)
	require.Error(t, werr)
	assert.Contains(t, werr.Error(), "disk full")
	assert.Nil(t, continuation, "no continuation on failed write")
	assert.Equal(t, int64(10), written)

	// A budget smaller than any segment header writes nothing at all and never
	// produces an empty continuation.
	tinyRecord, _ := newLocalHTTPResponseRecord(t, body)
	defer func() { _ = tinyRecord.Close() }()
	var ignored bytes.Buffer
	cont, zero, tErr := m.Marshal(&ignored, tinyRecord, 8)
	require.ErrorIs(t, tErr, ErrSegmentTooSmall)
	assert.Nil(t, cont)
	assert.Equal(t, int64(0), zero)
	assert.Equal(t, 0, ignored.Len())
}

// TestCaptureSegment_Eligibility verifies that only cached WARC/1.1 response
// and resource records (and their continuations) can be segmented, while a
// WARC/1.0 record and an unsupported record type are rejected without writing.
func TestCaptureSegment_Eligibility(t *testing.T) {
	content := "some content"

	// WARC/1.0 response is not segmentable.
	v10 := createMarshalerTestRecord(Response, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:v10>"},
		&nameValue{Name: WarcDate, Value: "2026-01-01T00:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
	}, content)
	defer func() { assert.NoError(t, v10.Close()) }()
	// Force the builder's version to 1.0 by constructing through an option.
	v10.(*warcRecord).version = V1_0

	var buf bytes.Buffer
	next, n, err := NewMarshaler().Marshal(&buf, v10, 400)
	require.ErrorIs(t, err, ErrSegmentationNotSupported)
	assert.Nil(t, next)
	assert.Equal(t, int64(0), n)
	assert.Equal(t, 0, buf.Len())

	// A metadata record type is not segmentable either.
	metadata := createMarshalerTestRecord(Metadata, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:meta>"},
		&nameValue{Name: WarcDate, Value: "2026-01-01T00:00:00Z"},
		&nameValue{Name: ContentType, Value: ApplicationWarcFields},
	}, content)
	defer func() { assert.NoError(t, metadata.Close()) }()

	next, n, err = NewMarshaler().Marshal(&buf, metadata, 400)
	require.ErrorIs(t, err, ErrSegmentationNotSupported)
	assert.Nil(t, next)
	assert.Equal(t, int64(0), n)
	assert.Equal(t, 0, buf.Len())

	// A resource record under WARC/1.1 is segmentable.
	resource := createMarshalerTestRecord(Resource, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:res>"},
		&nameValue{Name: WarcDate, Value: "2026-01-01T00:00:00Z"},
		&nameValue{Name: WarcTargetURI, Value: "http://example.com/r"},
		&nameValue{Name: ContentType, Value: "text/plain"},
	}, strings.Repeat("r", 2000))
	defer func() { assert.NoError(t, resource.Close()) }()
	out, _ := marshalAllSegments(t, NewMarshaler(), resource, 700)
	records := parsePhysicalWARC(t, out)
	require.Greater(t, len(records), 1)
	assert.Equal(t, Resource.String(), records[0].header(WarcType))
	assert.Equal(t, "1", records[0].header(WarcSegmentNumber))
}

// countingBlock wraps a Block and counts the total number of bytes ever served
// through its RawBytes reader, so a test can prove the source content is read
// once rather than re-copied for every segment.
type countingBlock struct {
	Block
	readBytes int64
}

func (c *countingBlock) RawBytes() (io.Reader, error) {
	r, err := c.Block.RawBytes()
	if err != nil {
		return nil, err
	}
	return &countingReader{r: r, cb: c}, nil
}

type countingReader struct {
	r  io.Reader
	cb *countingBlock
}

func (c *countingReader) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.cb.readBytes += int64(n)
	return n, err
}

// TestCaptureSegment_LinearSourceReads ensures the complete source block is
// streamed into the segment buffer once and subsequent segments read from that
// buffer, so total reads from the original content grow linearly with its size.
func TestCaptureSegment_LinearSourceReads(t *testing.T) {
	body := strings.Repeat("linear-", 1000) // 7000 payload bytes
	record, rawBlock := newLocalHTTPResponseRecord(t, body)

	// Wrap the record's block with a byte counter before segmenting.
	wr := record.(*warcRecord)
	src := &countingBlock{Block: wr.block}
	wr.block = src

	out, sizes := marshalAllSegments(t, NewMarshaler(), record, 1000)
	require.Greater(t, len(sizes), 1)

	// The original block is fully read exactly once during materialization;
	// every later segment is served from the independent segment buffer.
	assert.Equal(t, int64(len(rawBlock)), src.readBytes,
		"source block must be read once, not once per segment")

	records := parsePhysicalWARC(t, out)
	var reassembled bytes.Buffer
	for _, pr := range records {
		reassembled.Write(pr.block)
	}
	assert.Equal(t, rawBlock, reassembled.Bytes())
}

func segmentNameGenerator(dir, prefix string) *PatternNameGenerator {
	return &PatternNameGenerator{
		Directory: dir,
		Prefix:    prefix,
		Pattern:   "%{prefix}s-%04{serial}d.warc",
		Extension: "warc",
	}
}

func listWARCFiles(t *testing.T, dir string) []string {
	t.Helper()
	entries, err := os.ReadDir(dir)
	require.NoError(t, err)
	var names []string
	for _, e := range entries {
		if !e.IsDir() && regexp.MustCompile(`\.warc(\.gz)?$`).MatchString(e.Name()) {
			names = append(names, e.Name())
		}
	}
	sort.Strings(names)
	return names
}

// TestCaptureSegment_WriterRotationWithWarcinfo checks that a segmented writer
// rotates across files, that every file keeps its own warcinfo, that the
// response location points at the first segment and that BytesWritten sums all
// segments without counting warcinfo records.
func TestCaptureSegment_WriterRotationWithWarcinfo(t *testing.T) {
	freezeClockAndHost(t)
	dir := t.TempDir()

	w := NewWarcFileWriter(
		WithCompression(false),
		WithFileNameGenerator(segmentNameGenerator(dir, "rot")),
		WithMaxFileSize(1500),
		WithSegmentation(),
		WithWarcInfoFunc(func(rb WarcRecordBuilder) error {
			rb.AddWarcHeader(WarcRecordID, "<urn:uuid:warcinfo-record>")
			return nil
		}),
	)
	defer func() { assert.NoError(t, w.Close()) }()

	body := strings.Repeat("rotation-", 900) // 8100 payload bytes
	record, rawBlock := newLocalHTTPResponseRecord(t, body)

	res := w.Write(record)
	require.Len(t, res, 1)
	require.NoError(t, res[0].Err)
	require.NoError(t, w.Close())

	files := listWARCFiles(t, dir)
	require.GreaterOrEqual(t, len(files), 2, "record should span multiple files")

	// First response location is the first segment, right after the warcinfo.
	assert.Equal(t, files[0], res[0].FileName)
	firstData, err := os.ReadFile(filepath.Join(dir, files[0]))
	require.NoError(t, err)
	firstFileRecords := parsePhysicalWARC(t, firstData)
	require.GreaterOrEqual(t, len(firstFileRecords), 2)
	assert.Equal(t, Warcinfo.String(), firstFileRecords[0].header(WarcType))
	assert.Equal(t, physicalRecordLen(firstFileRecords[0]), res[0].FileOffset)
	assert.Equal(t, Response.String(), firstFileRecords[1].header(WarcType))
	assert.Equal(t, "1", firstFileRecords[1].header(WarcSegmentNumber))

	var (
		allBlocks      bytes.Buffer
		segmentCount   int
		warcinfoCount  int
		onDiskTotal    int64
		warcinfoBytes  int64
		segmentNumbers []int
	)
	for _, name := range files {
		data, rErr := os.ReadFile(filepath.Join(dir, name))
		require.NoError(t, rErr)
		onDiskTotal += int64(len(data))
		prs := parsePhysicalWARC(t, data)
		require.NotEmpty(t, prs)
		assert.Equal(t, Warcinfo.String(), prs[0].header(WarcType), "each file starts with warcinfo")
		for _, pr := range prs {
			if pr.header(WarcType) == Warcinfo.String() {
				warcinfoCount++
				warcinfoBytes += physicalRecordLen(pr)
				continue
			}
			segmentCount++
			n, pErr := strconv.Atoi(pr.header(WarcSegmentNumber))
			require.NoError(t, pErr)
			segmentNumbers = append(segmentNumbers, n)
			allBlocks.Write(pr.block)
		}
	}

	// Segment numbers form a contiguous 1..N sequence across files.
	for i, n := range segmentNumbers {
		assert.Equal(t, i+1, n)
	}
	assert.Equal(t, rawBlock, allBlocks.Bytes())

	// BytesWritten covers all segments but never the warcinfo records.
	assert.Greater(t, res[0].BytesWritten, int64(0))
	assert.Less(t, res[0].BytesWritten, onDiskTotal)
	assert.Equal(t, onDiskTotal-warcinfoBytes, res[0].BytesWritten)
	assert.Equal(t, len(files), warcinfoCount)
	assert.Greater(t, segmentCount, 1)
}

// physicalRecordOverhead returns everything but the block of a parsed physical
// record: version line, header fields, blank line and end-of-record marker.
func physicalRecordOverhead(pr physicalRecord) int64 {
	var headerBytes int64
	for _, nv := range pr.headers {
		headerBytes += int64(len(nv.Name)) + 2 + int64(len(nv.Value)) + 2
	}
	return int64(len(pr.version)) + 2 + headerBytes + 2 + 4
}

// TestCaptureSegment_GzipMembers writes a segmented compressed file and reads
// it back one independent gzip member at a time, expecting one WARC record per
// member.
func TestCaptureSegment_GzipMembers(t *testing.T) {
	freezeClockAndHost(t)
	dir := t.TempDir()

	w := NewWarcFileWriter(
		WithCompression(true),
		WithCompressionLevel(gzip.DefaultCompression),
		WithExpectedCompressionRatio(1.0),
		WithFileNameGenerator(segmentNameGenerator(dir, "gz")),
		WithMaxFileSize(2000),
		WithSegmentation(),
	)
	defer func() { assert.NoError(t, w.Close()) }()

	body := strings.Repeat("gzip-segment-", 800) // 11200 highly compressible bytes
	record, rawBlock := newLocalHTTPResponseRecord(t, body)

	res := w.Write(record)
	require.Len(t, res, 1)
	require.NoError(t, res[0].Err)
	require.NoError(t, w.Close())

	files := listWARCFiles(t, dir)
	require.NotEmpty(t, files)

	var memberCount int
	var reassembled bytes.Buffer
	for _, name := range files {
		f, err := os.Open(filepath.Join(dir, name))
		require.NoError(t, err)

		// Each physical record is its own gzip member. Multistream(false) makes
		// reading stop at the member boundary instead of silently chaining into
		// the next member; Reset then positions the reader at the next header.
		br := bufio.NewReader(f)
		gz, err := gzip.NewReader(br)
		require.NoError(t, err)
		for {
			// Reset restores multistream mode, so disable it on every iteration.
			gz.Multistream(false)
			member, mErr := io.ReadAll(gz)
			require.NoError(t, mErr)
			if len(member) == 0 {
				// Underlying stream exhausted with no further member header.
				break
			}

			prs := parsePhysicalWARC(t, member)
			require.Len(t, prs, 1, "each gzip member holds exactly one WARC record")
			for _, pr := range prs {
				memberCount++
				if pr.header(WarcType) != Warcinfo.String() {
					reassembled.Write(pr.block)
				}
			}

			// io.EOF means there are no further members in this file.
			if rErr := gz.Reset(br); rErr != nil {
				require.ErrorIs(t, rErr, io.EOF)
				break
			}
		}
		assert.NoError(t, gz.Close())
		assert.NoError(t, f.Close())
	}

	assert.Greater(t, memberCount, 2, "expected independent gzip members for every record")
	assert.Equal(t, rawBlock, reassembled.Bytes())
	// All segments are reported, warcinfo members excluded from the byte total.
	assert.Greater(t, res[0].BytesWritten, int64(len(rawBlock)))
}
