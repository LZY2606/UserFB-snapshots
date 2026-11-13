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
	"fmt"
	"io"
	"strconv"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestDefaultMarshaler_Marshal(t *testing.T) {
	tests := []struct {
		name         string
		recordType   RecordType
		headers      *WarcFields
		content      string
		wantSize     int64
		wantInOutput []string // strings that should appear in output
	}{
		{
			name:       "simple warcinfo record",
			recordType: Warcinfo,
			headers: &WarcFields{
				&nameValue{Name: WarcRecordID, Value: "<urn:uuid:12345678-1234-1234-1234-123456789012>"},
				&nameValue{Name: WarcDate, Value: "2024-01-15T10:30:00Z"},
				&nameValue{Name: ContentType, Value: ApplicationWarcFields},
				&nameValue{Name: ContentLength, Value: "13"},
			},
			content: "software: foo",
			wantInOutput: []string{
				"WARC/1.1",
				"WARC-Type: warcinfo",
				"WARC-Record-ID: <urn:uuid:12345678-1234-1234-1234-123456789012>",
				"WARC-Date: 2024-01-15T10:30:00Z",
				"Content-Type: application/warc-fields",
				"Content-Length: 13",
				"software: foo",
			},
		},
		{
			name:       "simple resource record",
			recordType: Resource,
			headers: &WarcFields{
				&nameValue{Name: WarcRecordID, Value: "<urn:uuid:abcdef00-1111-2222-3333-444444444444>"},
				&nameValue{Name: WarcDate, Value: "2024-02-20T14:15:00Z"},
				&nameValue{Name: WarcTargetURI, Value: "http://example.com/page.html"},
				&nameValue{Name: ContentType, Value: "text/html"},
				&nameValue{Name: ContentLength, Value: "12"},
			},
			content: "Hello World!",
			wantInOutput: []string{
				"WARC/1.1",
				"WARC-Type: resource",
				"WARC-Record-ID: <urn:uuid:abcdef00-1111-2222-3333-444444444444>",
				"WARC-Date: 2024-02-20T14:15:00Z",
				"WARC-Target-URI: http://example.com/page.html",
				"Content-Type: text/html",
				"Content-Length: 12",
				"Hello World!",
			},
		},
		{
			name:       "simple metadata record",
			recordType: Metadata,
			headers: &WarcFields{
				&nameValue{Name: WarcRecordID, Value: "<urn:uuid:99999999-aaaa-bbbb-cccc-dddddddddddd>"},
				&nameValue{Name: WarcDate, Value: "2024-03-10T08:45:30Z"},
				&nameValue{Name: WarcTargetURI, Value: "http://example.org/data"},
				&nameValue{Name: ContentType, Value: ApplicationWarcFields},
				&nameValue{Name: ContentLength, Value: "11"},
			},
			content: "author: Bob",
			wantInOutput: []string{
				"WARC/1.1",
				"WARC-Type: metadata",
				"WARC-Record-ID: <urn:uuid:99999999-aaaa-bbbb-cccc-dddddddddddd>",
				"WARC-Date: 2024-03-10T08:45:30Z",
				"WARC-Target-URI: http://example.org/data",
				"Content-Type: application/warc-fields",
				"Content-Length: 11",
				"author: Bob",
			},
		},
		{
			name:       "empty content",
			recordType: Warcinfo,
			headers: &WarcFields{
				&nameValue{Name: WarcRecordID, Value: "<urn:uuid:00000000-0000-0000-0000-000000000000>"},
				&nameValue{Name: WarcDate, Value: "2024-01-01T00:00:00Z"},
				&nameValue{Name: ContentType, Value: ApplicationWarcFields},
				&nameValue{Name: ContentLength, Value: "0"},
			},
			content: "",
			wantInOutput: []string{
				"WARC/1.1",
				"WARC-Type: warcinfo",
				"WARC-Record-ID: <urn:uuid:00000000-0000-0000-0000-000000000000>",
				"WARC-Date: 2024-01-01T00:00:00Z",
				"Content-Type: application/warc-fields",
				"Content-Length: 0",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			// Create record
			record := createMarshalerTestRecord(tt.recordType, tt.headers, tt.content)
			defer func() { assert.NoError(t, record.Close()) }()

			// Marshal record
			marshaler := NewMarshaler()
			var buf bytes.Buffer
			continuation, size, err := marshaler.Marshal(&buf, record, 0)

			// Verify no error
			require.NoError(t, err)

			// Verify no continuation (segmentation not implemented)
			assert.Nil(t, continuation)

			// Verify size is positive
			assert.Greater(t, size, int64(0))

			// Verify size matches actual bytes written
			assert.Equal(t, int64(buf.Len()), size)

			// Get output
			output := buf.String()

			// Verify output contains expected strings
			for _, want := range tt.wantInOutput {
				assert.Contains(t, output, want, "output should contain: %s", want)
			}

			// Verify structure: starts with WARC version
			assert.True(t, strings.HasPrefix(output, "WARC/1.1\r\n"))

			// Verify structure: ends with double CRLF
			assert.True(t, strings.HasSuffix(output, "\r\n\r\n"))

			// Verify structure: has header separator (CRLF between headers and content)
			headerEnd := strings.Index(output, "\r\n\r\n")
			contentStart := headerEnd + 4
			contentEnd := len(output) - 4 // Remove trailing \r\n\r\n

			// Verify content matches
			actualContent := output[contentStart:contentEnd]
			assert.Equal(t, tt.content, actualContent)
		})
	}
}

func TestDefaultMarshaler_MarshalWithMaxSize(t *testing.T) {
	// Build a resource record large enough that no single physical record can
	// hold the whole thing within a small maxSize.
	content := strings.Repeat("abcdefgh", 256) // 2048 bytes
	headers := &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-record-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T00:00:00Z"},
		&nameValue{Name: WarcTargetURI, Value: "http://example.com/resource"},
		&nameValue{Name: ContentType, Value: "text/plain"},
	}
	record := createMarshalerTestRecord(Resource, headers, content)
	defer func() { assert.NoError(t, record.Close()) }()

	marshaler := NewMarshaler()

	// Measure the size of the unsegmented record.
	var full bytes.Buffer
	_, fullSize, err := marshaler.Marshal(&full, record, 0)
	require.NoError(t, err)
	require.Equal(t, int64(full.Len()), fullSize)

	// Choose a budget that forces segmentation but comfortably fits every
	// segment, including the larger continuation and final headers.
	const maxSize int64 = 600
	require.Less(t, maxSize, fullSize)

	// Re-create the record because the first Marshal consumed the uncached block.
	record2 := createMarshalerTestRecord(Resource, headers, content)
	defer func() { assert.NoError(t, record2.Close()) }()

	var (
		out          bytes.Buffer
		current      = record2
		segments     int
		contentTotal int64
		originID     string
	)
	for current != nil {
		var seg bytes.Buffer
		next, size, mErr := marshaler.Marshal(&seg, current, maxSize)
		require.NoError(t, mErr)

		// The physical record must never exceed the budget and must be non-empty.
		assert.Equal(t, int64(seg.Len()), size)
		assert.LessOrEqual(t, size, maxSize)
		assert.Greater(t, size, int64(0))

		parsed, off, _, uErr := NewUnmarshaler(WithSpecViolationPolicy(ErrIgnore), WithSyntaxErrorPolicy(ErrIgnore)).
			Unmarshal(bufio.NewReader(bytes.NewReader(seg.Bytes())))
		require.NoError(t, uErr)
		assert.Equal(t, int64(0), off)

		segments++
		cl, cErr := parsed.ContentLength()
		require.NoError(t, cErr)
		contentTotal += cl

		if segments == 1 {
			assert.Equal(t, Resource, parsed.Type())
			assert.Equal(t, "1", parsed.WarcHeader().Get(WarcSegmentNumber))
			assert.False(t, parsed.WarcHeader().Has(WarcSegmentOriginID))
			originID = parsed.RecordId()
		} else {
			assert.Equal(t, Continuation, parsed.Type())
			assert.Equal(t, strconv.Itoa(segments), parsed.WarcHeader().Get(WarcSegmentNumber))
			assert.Equal(t, originID, parsed.WarcHeader().GetId(WarcSegmentOriginID))
			assert.NotEqual(t, originID, parsed.RecordId())
			// Payload digest is not copied onto continuation segments.
			assert.False(t, parsed.WarcHeader().Has(WarcPayloadDigest))
		}

		if next == nil {
			// Only the last segment carries the total logical block length.
			assert.True(t, parsed.WarcHeader().Has(WarcSegmentTotalLength))
			total, tErr := parsed.WarcHeader().GetInt64(WarcSegmentTotalLength)
			require.NoError(t, tErr)
			assert.Equal(t, int64(len(content)), total)
		} else {
			assert.False(t, parsed.WarcHeader().Has(WarcSegmentTotalLength))
			if current != record2 {
				_ = current.Close()
			}
		}
		_ = parsed.Close()

		_, _ = out.Write(seg.Bytes())
		current = next
	}

	assert.Greater(t, segments, 1)
	// The concatenated segment content blocks equal the original raw block.
	assert.Equal(t, int64(len(content)), contentTotal)
}

func TestDefaultMarshaler_MarshalWithMaxSize_FitsUnsegmented(t *testing.T) {
	// When the whole record fits within maxSize it is written unchanged,
	// without any segmentation fields.
	headers := &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-record-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T00:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
		&nameValue{Name: ContentLength, Value: "5"},
	}
	record := createMarshalerTestRecord(Resource, headers, "Hello")
	defer func() { assert.NoError(t, record.Close()) }()

	marshaler := NewMarshaler()
	var buf bytes.Buffer
	continuation, size, err := marshaler.Marshal(&buf, record, 100_000)

	require.NoError(t, err)
	assert.Nil(t, continuation)
	assert.Equal(t, int64(buf.Len()), size)
	assert.NotContains(t, buf.String(), WarcSegmentNumber)
}

func TestDefaultMarshaler_MarshalWithMaxSize_TooSmall(t *testing.T) {
	// A budget that cannot hold even one content byte plus framing must write
	// nothing and return ErrSegmentTooSmall, never an empty continuation.
	headers := &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-record-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T00:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
		&nameValue{Name: ContentLength, Value: "5"},
	}
	record := createMarshalerTestRecord(Resource, headers, "Hello")
	defer func() { assert.NoError(t, record.Close()) }()

	marshaler := NewMarshaler()
	var buf bytes.Buffer
	continuation, size, err := marshaler.Marshal(&buf, record, 5)

	require.ErrorIs(t, err, ErrSegmentTooSmall)
	assert.Nil(t, continuation)
	assert.Equal(t, int64(0), size)
	assert.Equal(t, 0, buf.Len())
}

func TestDefaultMarshaler_WriteRecord(t *testing.T) {
	tests := []struct {
		name         string
		recordType   RecordType
		headers      *WarcFields
		content      string
		wantContains []string
	}{
		{
			name:       "verify complete record structure",
			recordType: Warcinfo,
			headers: &WarcFields{
				&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-id>"},
				&nameValue{Name: WarcDate, Value: "2024-01-01T12:00:00Z"},
				&nameValue{Name: ContentType, Value: "text/plain"},
				&nameValue{Name: ContentLength, Value: "4"},
			},
			content: "test",
			wantContains: []string{
				"WARC/1.1\r\n",
				"WARC-Type: warcinfo\r\n",
				"WARC-Record-ID: <urn:uuid:test-id>\r\n",
				"WARC-Date: 2024-01-01T12:00:00Z\r\n",
				"Content-Type: text/plain\r\n",
				"Content-Length: 4\r\n",
				"\r\n\r\n", // separator between headers and content
				"test",
				"\r\n\r\n", // end of record
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			record := createMarshalerTestRecord(tt.recordType, tt.headers, tt.content)
			defer func() { assert.NoError(t, record.Close()) }()

			marshaler := &defaultMarshaler{}
			var buf bytes.Buffer
			size, err := marshaler.writeRecord(&buf, record)

			require.NoError(t, err)
			assert.Greater(t, size, int64(0))
			assert.Equal(t, int64(buf.Len()), size)

			output := buf.String()
			for _, want := range tt.wantContains {
				assert.Contains(t, output, want)
			}
		})
	}
}

// createMarshalerTestRecord is a helper function to create records for testing
func createMarshalerTestRecord(recordType RecordType, headers *WarcFields, content string) WarcRecord {
	rb := NewRecordBuilder(
		recordType,
		WithSpecViolationPolicy(ErrIgnore),
		WithSyntaxErrorPolicy(ErrIgnore),
		WithUnknownRecordTypePolicy(ErrIgnore),
		WithFixDigest(false),
		WithAddMissingDigest(false),
	)

	for _, nv := range *headers {
		rb.AddWarcHeader(nv.Name, nv.Value)
	}

	if _, err := rb.WriteString(content); err != nil {
		panic(err)
	}

	record, _, err := rb.Build()
	if err != nil {
		panic(err)
	}

	return record
}

// errWriter fails after writing n bytes
type errWriter struct {
	n   int
	err error
}

func (w *errWriter) Write(p []byte) (int, error) {
	if w.n <= 0 {
		return 0, w.err
	}
	if len(p) > w.n {
		w.n = 0
		return 0, w.err
	}
	w.n -= len(p)
	return len(p), nil
}

func TestDefaultMarshaler_writeRecord_ErrorAtVersion(t *testing.T) {
	record := createMarshalerTestRecord(Warcinfo, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T12:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
		&nameValue{Name: ContentLength, Value: "4"},
	}, "test")
	defer func() { assert.NoError(t, record.Close()) }()

	m := &defaultMarshaler{}
	w := &errWriter{n: 0, err: fmt.Errorf("write version failed")}
	_, err := m.writeRecord(w, record)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "write version failed")
}

func TestDefaultMarshaler_writeRecord_ErrorAtVersionCRLF(t *testing.T) {
	record := createMarshalerTestRecord(Warcinfo, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T12:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
		&nameValue{Name: ContentLength, Value: "4"},
	}, "test")
	defer func() { assert.NoError(t, record.Close()) }()

	m := &defaultMarshaler{}
	// Allow version string ("WARC/1.1" = 8 bytes) but fail on CRLF
	w := &errWriter{n: 8, err: fmt.Errorf("write crlf failed")}
	_, err := m.writeRecord(w, record)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "write crlf failed")
}

func TestDefaultMarshaler_writeRecord_ErrorAtHeaders(t *testing.T) {
	record := createMarshalerTestRecord(Warcinfo, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T12:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
		&nameValue{Name: ContentLength, Value: "4"},
	}, "test")
	defer func() { assert.NoError(t, record.Close()) }()

	m := &defaultMarshaler{}
	// Allow version + CRLF (10 bytes) but fail on headers
	w := &errWriter{n: 10, err: fmt.Errorf("write headers failed")}
	_, err := m.writeRecord(w, record)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "write headers failed")
}

func TestDefaultMarshaler_writeRecord_ErrorAtContent(t *testing.T) {
	record := createMarshalerTestRecord(Warcinfo, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T12:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
		&nameValue{Name: ContentLength, Value: "4"},
	}, "test")
	defer func() { assert.NoError(t, record.Close()) }()

	// Write the record to measure total size
	var countBuf bytes.Buffer
	m := &defaultMarshaler{}
	totalSize, err := m.writeRecord(&countBuf, record)
	require.NoError(t, err)

	// Create a new identical record (the first one's block has been consumed)
	record2 := createMarshalerTestRecord(Warcinfo, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T12:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
		&nameValue{Name: ContentLength, Value: "4"},
	}, "test")
	defer func() { assert.NoError(t, record2.Close()) }()

	// Allow all bytes except content(4) + end marker(4) = 8 bytes
	w := &errWriter{n: int(totalSize) - 8, err: fmt.Errorf("write content failed")}
	_, err = m.writeRecord(w, record2)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "write content failed")
}

func TestDefaultMarshaler_writeRecord_ErrorAtEndMarker(t *testing.T) {
	record := createMarshalerTestRecord(Warcinfo, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T12:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
		&nameValue{Name: ContentLength, Value: "0"},
	}, "")
	defer func() { assert.NoError(t, record.Close()) }()

	// Write the record successfully to determine total size minus the end marker
	var countBuf bytes.Buffer
	m := &defaultMarshaler{}
	totalSize, err := m.writeRecord(&countBuf, record)
	require.NoError(t, err)

	// Now create an identical record and fail at the end marker
	record2 := createMarshalerTestRecord(Warcinfo, &WarcFields{
		&nameValue{Name: WarcRecordID, Value: "<urn:uuid:test-id>"},
		&nameValue{Name: WarcDate, Value: "2024-01-01T12:00:00Z"},
		&nameValue{Name: ContentType, Value: "text/plain"},
		&nameValue{Name: ContentLength, Value: "0"},
	}, "")
	defer func() { assert.NoError(t, record2.Close()) }()

	// Allow all bytes except the last 4 (end marker \r\n\r\n)
	w := &errWriter{n: int(totalSize) - 4, err: fmt.Errorf("write end marker failed")}
	_, err = m.writeRecord(w, record2)
	require.Error(t, err)
	assert.Contains(t, err.Error(), "write end marker failed")
}

func Test_writeRecord_RawBytesError(t *testing.T) {
	// Exercise the writeRecord error path where Block().RawBytes() fails.
	// Use the Unmarshaler with a non-seekable reader to get an uncached HTTP response block,
	// then consume it to make RawBytes() fail on re-access.
	rawWARC := "WARC/1.1\r\n" +
		"WARC-Type: response\r\n" +
		"WARC-Target-URI: http://example.com\r\n" +
		"WARC-Date: 2016-09-19T18:03:53Z\r\n" +
		"WARC-Record-ID: <urn:uuid:fff0cecc-0221-11e7-adb1-0242ac120008>\r\n" +
		"Content-Type: application/http;msgtype=response\r\n" +
		"Content-Length: 42\r\n" +
		"\r\n" +
		"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ntest" +
		"\r\n\r\n"

	u := NewUnmarshaler(WithSpecViolationPolicy(ErrIgnore), WithSyntaxErrorPolicy(ErrIgnore))
	rec, _, _, err := u.Unmarshal(bufio.NewReader(io.NopCloser(strings.NewReader(rawWARC))))
	require.NoError(t, err)
	defer func() { assert.NoError(t, rec.Close()) }()

	// Consume the block to make it inaccessible
	r, err := rec.Block().RawBytes()
	require.NoError(t, err)
	_, _ = io.ReadAll(r)

	// Now marshal should fail because RawBytes() returns errContentReAccessed
	m := &defaultMarshaler{}
	var buf bytes.Buffer
	_, err = m.writeRecord(&buf, rec)
	assert.Error(t, err)
}
