package warc

import (
	"bytes"
	"compress/gzip"
	"errors"
	"io"
	"math"
	"reflect"
	"strconv"
	"testing"
)

// errSentinel is an injected failure used to check that original errors
// survive the gzip and counting-writer layers.
var errSentinel = errors.New("indexed test: injected failure")

// trackedRead records one ReadAt call as seen by the underlying source.
type trackedRead struct {
	off    int64
	length int64
}

// trackingReaderAt wraps the archive bytes and records every ReadAt
// call, so tests can prove that no read leaves the requested interval.
type trackingReaderAt struct {
	calls  []trackedRead
	source *bytes.Reader
}

func newTrackingReaderAt(data []byte) *trackingReaderAt {
	return &trackingReaderAt{source: bytes.NewReader(data)}
}

func (t *trackingReaderAt) ReadAt(p []byte, off int64) (int, error) {
	t.calls = append(t.calls, trackedRead{off: off, length: int64(len(p))})
	return t.source.ReadAt(p, off)
}

// assertCallsInRange fails the test if any recorded ReadAt touched bytes
// outside the half-open interval described by m.
func assertCallsInRange(t *testing.T, tr *trackingReaderAt, m Member) {
	t.Helper()
	for _, call := range tr.calls {
		if call.off < m.Offset || call.off+call.length > m.Offset+m.Length {
			t.Fatalf("ReadAt call [%d, %d) escapes member interval [%d, %d)",
				call.off, call.off+call.length, m.Offset, m.Offset+m.Length)
		}
	}
}

// gzipMember compresses data into a standalone gzip member using the
// standard library directly, independently of IndexedWriter.
func gzipMember(t *testing.T, data []byte) []byte {
	t.Helper()
	var buffer bytes.Buffer
	compressor := gzip.NewWriter(&buffer)
	if _, err := compressor.Write(data); err != nil {
		t.Fatalf("gzip write failed: %v", err)
	}
	if err := compressor.Close(); err != nil {
		t.Fatalf("gzip close failed: %v", err)
	}
	return buffer.Bytes()
}

// plainRecord encodes a single WARC record without compression.
func plainRecord(t *testing.T, header map[string]string, content []byte) []byte {
	t.Helper()
	var buffer bytes.Buffer
	record := NewRecord()
	record.Header = header
	record.Content = bytes.NewReader(content)
	if _, err := NewWriter(&buffer).WriteRecord(record); err != nil {
		t.Fatalf("plain WriteRecord failed: %v", err)
	}
	return buffer.Bytes()
}

func makeRecord(header map[string]string, content []byte) *Record {
	record := NewRecord()
	record.Header = header
	record.Content = bytes.NewReader(append([]byte(nil), content...))
	return record
}

func readAllContent(t *testing.T, record *Record) []byte {
	t.Helper()
	content, err := io.ReadAll(record.Content)
	if err != nil {
		t.Fatalf("failed to read record content: %v", err)
	}
	return content
}

// countingReader flags whether anything ever consumed its data.
type countingReader struct {
	reader io.Reader
	reads  int
}

func (c *countingReader) Read(p []byte) (int, error) {
	c.reads++
	return c.reader.Read(p)
}

// recordingWriter only records the sizes of the write calls it receives.
type recordingWriter struct {
	sizes []int
}

func (w *recordingWriter) Write(p []byte) (int, error) {
	w.sizes = append(w.sizes, len(p))
	return len(p), nil
}

// failAtWriter fails a predetermined write call. With short set it
// performs a short write while reporting no error; otherwise it returns
// errSentinel.
type failAtWriter struct {
	failAt int
	short  bool
	calls  int
}

func (w *failAtWriter) Write(p []byte) (int, error) {
	w.calls++
	if w.calls == w.failAt {
		if w.short {
			n := len(p) / 2
			if n == 0 || n >= len(p) {
				n = len(p) - 1
			}
			return n, nil
		}
		return 0, errSentinel
	}
	return len(p), nil
}

func TestIndexedCapture(t *testing.T) {
	header1 := NewHeader()
	header1.Set("warc-type", "resource")
	header1.Set("content-type", "text/plain")
	header1.Set("warc-record-id", "<urn:test:record-1>")
	content1 := []byte("first capture payload")

	header2 := NewHeader()
	header2.Set("warc-type", "resource")
	header2.Set("content-type", "text/plain")
	header2.Set("warc-record-id", "<urn:test:record-2>")
	content2 := []byte("second capture payload, replayed on its own")

	t.Run("members are contiguous and replay independently", func(t *testing.T) {
		var archive bytes.Buffer
		writer := NewIndexedWriter(&archive)

		member1, err := writer.WriteRecord(makeRecord(header1, content1))
		if err != nil {
			t.Fatalf("first WriteRecord failed: %v", err)
		}
		member2, err := writer.WriteRecord(makeRecord(header2, content2))
		if err != nil {
			t.Fatalf("second WriteRecord failed: %v", err)
		}

		// Intervals start at zero, are contiguous and do not overlap.
		if member1.Offset != 0 {
			t.Fatalf("first member offset = %d, want 0", member1.Offset)
		}
		if member1.Length <= 0 || member2.Length <= 0 {
			t.Fatalf("members must have positive length: %+v %+v", member1, member2)
		}
		if member2.Offset != member1.Offset+member1.Length {
			t.Fatalf("gap or overlap between members: %+v %+v", member1, member2)
		}
		if member2.Offset+member2.Length != int64(archive.Len()) {
			t.Fatalf("members cover %d bytes, archive has %d",
				member2.Offset+member2.Length, archive.Len())
		}
		// Length is the physical gzip size, not the WARC content length.
		if member1.Length == int64(len(content1)) {
			t.Fatalf("member length equals raw content length; gzip header/trailer not counted")
		}
		// Every member starts with the gzip magic number.
		for _, member := range []Member{member1, member2} {
			magic := archive.Bytes()[member.Offset : member.Offset+2]
			if magic[0] != 0x1f || magic[1] != 0x8b {
				t.Fatalf("member at %d does not start with gzip magic: % x", member.Offset, magic)
			}
		}

		// Replay only the second capture, exactly as an archive index would.
		tracker := newTrackingReaderAt(archive.Bytes())
		record2, err := ReadMemberAt(tracker, member2)
		if err != nil {
			t.Fatalf("ReadMemberAt(member2) failed: %v", err)
		}
		assertCallsInRange(t, tracker, member2)
		if got := readAllContent(t, record2); !bytes.Equal(got, content2) {
			t.Fatalf("member2 content = %q, want %q", got, content2)
		}
		if record2.Header.Get("warc-record-id") != "<urn:test:record-2>" {
			t.Fatalf("unexpected member2 header: %v", record2.Header)
		}
		if record2.Header.Get("content-length") != strconv.Itoa(len(content2)) {
			t.Fatalf("content-length = %q, want %d",
				record2.Header.Get("content-length"), len(content2))
		}

		// And the first one still reads correctly through the same ReaderAt.
		tracker2 := newTrackingReaderAt(archive.Bytes())
		record1, err := ReadMemberAt(tracker2, member1)
		if err != nil {
			t.Fatalf("ReadMemberAt(member1) failed: %v", err)
		}
		assertCallsInRange(t, tracker2, member1)
		if got := readAllContent(t, record1); !bytes.Equal(got, content1) {
			t.Fatalf("member1 content = %q, want %q", got, content1)
		}
	})

	t.Run("bad intervals built with stdlib gzip are rejected", func(t *testing.T) {
		plain1 := plainRecord(t, header1, content1)
		plain2 := plainRecord(t, header2, content2)
		good1 := gzipMember(t, plain1)
		good2 := gzipMember(t, plain2)

		cases := []struct {
			name   string
			data   []byte
			member Member
		}{
			{
				name:   "truncated member",
				data:   good1[:len(good1)-3],
				member: Member{0, int64(len(good1) - 3)},
			},
			{
				name:   "interval shorter than the data",
				data:   good1,
				member: Member{0, int64(len(good1) - 2)},
			},
			{
				name: "checksum corrupted",
				data: func() []byte {
					corrupt := append([]byte(nil), good1...)
					corrupt[len(corrupt)-5] ^= 0xff
					return corrupt
				}(),
				member: Member{0, int64(len(good1))},
			},
			{
				name:   "two gzip members in one interval",
				data:   append(append([]byte(nil), good1...), good2...),
				member: Member{0, int64(len(good1) + len(good2))},
			},
			{
				name:   "trailing garbage after member",
				data:   append(append([]byte(nil), good1...), []byte("JUNK")...),
				member: Member{0, int64(len(good1) + 4)},
			},
			{
				name: "interval borrowing the next capture's bytes",
				data: append(append([]byte(nil), good1...), good2...),
				// First member plus six bytes that belong to the second one.
				member: Member{0, int64(len(good1) + 6)},
			},
			{
				name:   "two WARC records inside one gzip member",
				data:   gzipMember(t, append(append([]byte(nil), plain1...), plain2...)),
				member: Member{0, 0}, // filled in below once the size is known
			},
			{
				name:   "extra decompressed bytes after legal record",
				data:   gzipMember(t, append(append([]byte(nil), plain1...), []byte("trailing-bytes")...)),
				member: Member{0, 0},
			},
			{
				name:   "gzip member without any WARC record",
				data:   gzipMember(t, nil),
				member: Member{0, 0},
			},
		}
		cases[6].member = Member{0, int64(len(cases[6].data))}
		cases[7].member = Member{0, int64(len(cases[7].data))}
		cases[8].member = Member{0, int64(len(cases[8].data))}

		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				tracker := newTrackingReaderAt(tc.data)
				record, err := ReadMemberAt(tracker, tc.member)
				if err == nil {
					t.Fatalf("expected error, got record with content %q",
						readAllContent(t, record))
				}
				assertCallsInRange(t, tracker, tc.member)
			})
		}
	})

	t.Run("invalid member descriptors are rejected without reading", func(t *testing.T) {
		invalid := []Member{
			{Offset: -1, Length: 10},
			{Offset: 0, Length: 0},
			{Offset: 5, Length: -1},
			{Offset: math.MaxInt64 - 3, Length: 10},
		}
		for _, member := range invalid {
			tracker := newTrackingReaderAt([]byte("0123456789abcdef"))
			if _, err := ReadMemberAt(tracker, member); err == nil {
				t.Fatalf("expected error for member %+v", member)
			}
			if len(tracker.calls) != 0 {
				t.Fatalf("member %+v triggered %d ReadAt calls, want none", member, len(tracker.calls))
			}
		}

		// An interval that extends beyond the available data must fail
		// instead of being padded with bytes from anywhere.
		shortData := gzipMember(t, plainRecord(t, header1, content1))
		overlong := Member{Offset: 0, Length: int64(len(shortData)) + 16}
		tracker := newTrackingReaderAt(shortData)
		if _, err := ReadMemberAt(tracker, overlong); err == nil {
			t.Fatal("expected error for interval extending past the data")
		}
		assertCallsInRange(t, tracker, Member{Offset: 0, Length: overlong.Length})
	})

	t.Run("short and failing writes poison the writer", func(t *testing.T) {
		// A dry run reveals how the gzip writer splits header, body flushes
		// and trailer across underlying write calls for this record size.
		recorder := &recordingWriter{}
		dryRun := NewIndexedWriter(recorder)
		bigContent := bytes.Repeat([]byte("abcdefghij"), 400)
		if _, err := dryRun.WriteRecord(makeRecord(header1, bigContent)); err != nil {
			t.Fatalf("dry run failed: %v", err)
		}
		headerCall := 1
		trailerCall := len(recorder.sizes)
		bodyCall := len(recorder.sizes) / 2
		if bodyCall == headerCall || bodyCall == trailerCall {
			t.Fatalf("cannot pick a distinct body call from %v", recorder.sizes)
		}
		t.Logf("gzip write call sizes: %v (header=%d body=%d trailer=%d)",
			recorder.sizes, headerCall, bodyCall, trailerCall)

		for _, target := range []struct {
			name string
			call int
		}{
			{"header", headerCall},
			{"body", bodyCall},
			{"trailer", trailerCall},
		} {
			for _, short := range []bool{false, true} {
				name := target.name
				if short {
					name += "-short-write"
				} else {
					name += "-injected-error"
				}
				t.Run(name, func(t *testing.T) {
					failer := &failAtWriter{failAt: target.call, short: short}
					writer := NewIndexedWriter(failer)

					member, err := writer.WriteRecord(makeRecord(header1, bigContent))
					if member != (Member{}) {
						t.Fatalf("failed write returned non-zero member: %+v", member)
					}
					if err == nil {
						t.Fatal("expected write failure, got nil")
					}
					if short {
						if !errors.Is(err, io.ErrShortWrite) {
							t.Fatalf("error = %v, want io.ErrShortWrite", err)
						}
					} else if !errors.Is(err, errSentinel) {
						t.Fatalf("error = %v, want errSentinel", err)
					}
					callsAfterFailure := failer.calls

					// The next record must neither be encoded nor compressed:
					// its content is never read and nothing reaches the sink.
					untouched := &countingReader{reader: bytes.NewReader(content2)}
					next := NewRecord()
					next.Header = NewHeader()
					next.Content = untouched
					member2, err2 := writer.WriteRecord(next)
					if member2 != (Member{}) {
						t.Fatalf("poisoned writer returned member: %+v", member2)
					}
					if err2 != err {
						t.Fatalf("second error = %v, want retained error %v", err2, err)
					}
					if untouched.reads != 0 {
						t.Fatalf("next record content was read %d times after failure", untouched.reads)
					}
					if failer.calls != callsAfterFailure {
						t.Fatalf("poisoned writer performed %d extra underlying writes",
							failer.calls-callsAfterFailure)
					}
				})
			}
		}
	})

	t.Run("reversed reads stay independent", func(t *testing.T) {
		var archive bytes.Buffer
		writer := NewIndexedWriter(&archive)
		member1, err := writer.WriteRecord(makeRecord(header1, content1))
		if err != nil {
			t.Fatalf("first WriteRecord failed: %v", err)
		}
		member2, err := writer.WriteRecord(makeRecord(header2, content2))
		if err != nil {
			t.Fatalf("second WriteRecord failed: %v", err)
		}

		// One shared ReaderAt serves both captures; its shared position
		// must not couple the two independently consumed records.
		tracker := newTrackingReaderAt(archive.Bytes())

		// Read the second capture first and fully consume its content.
		callsBefore2 := len(tracker.calls)
		record2, err := ReadMemberAt(tracker, member2)
		if err != nil {
			t.Fatalf("ReadMemberAt(member2) failed: %v", err)
		}
		assertCallsInRange(t, &trackingReaderAt{calls: tracker.calls[callsBefore2:]}, member2)
		got2 := readAllContent(t, record2)
		if closer, ok := record2.Content.(io.Closer); ok {
			if err := closer.Close(); err != nil {
				t.Fatalf("closing member2 content failed: %v", err)
			}
		}

		// Then read the first capture via the same shared ReaderAt.
		callsBefore1 := len(tracker.calls)
		record1, err := ReadMemberAt(tracker, member1)
		if err != nil {
			t.Fatalf("ReadMemberAt(member1) failed: %v", err)
		}
		assertCallsInRange(t, &trackingReaderAt{calls: tracker.calls[callsBefore1:]}, member1)
		got1 := readAllContent(t, record1)
		if closer, ok := record1.Content.(io.Closer); ok {
			if err := closer.Close(); err != nil {
				t.Fatalf("closing member1 content failed: %v", err)
			}
		}

		// The earlier result must remain valid on its own.
		if !bytes.Equal(got2, content2) {
			t.Fatalf("second capture invalidated: %q", got2)
		}
		if !bytes.Equal(got1, content1) {
			t.Fatalf("first capture mismatch: %q", got1)
		}
		if reflect.DeepEqual(got1, got2) {
			t.Fatal("both reads returned the same content")
		}
	})

	t.Run("plain and whole-stream gzip readers keep working", func(t *testing.T) {
		// Plain, uncompressed WARC.
		var plain bytes.Buffer
		plainWriter := NewWriter(&plain)
		if _, err := plainWriter.WriteRecord(makeRecord(header1, content1)); err != nil {
			t.Fatal(err)
		}
		if _, err := plainWriter.WriteRecord(makeRecord(header2, content2)); err != nil {
			t.Fatal(err)
		}
		plainReader, err := NewReader(bytes.NewReader(plain.Bytes()))
		if err != nil {
			t.Fatalf("NewReader on plain WARC failed: %v", err)
		}
		plainRecord1, err := plainReader.ReadRecord()
		if err != nil {
			t.Fatal(err)
		}
		if got := readAllContent(t, plainRecord1); !bytes.Equal(got, content1) {
			t.Fatalf("plain record 1 mismatch: %q", got)
		}
		plainRecord2, err := plainReader.ReadRecord()
		if err != nil {
			t.Fatal(err)
		}
		if got := readAllContent(t, plainRecord2); !bytes.Equal(got, content2) {
			t.Fatalf("plain record 2 mismatch: %q", got)
		}
		plainReader.Close()

		// The legacy layout: one gzip stream wrapping several records.
		var streamed bytes.Buffer
		compressor := gzip.NewWriter(&streamed)
		streamWriter := NewWriter(compressor)
		if _, err := streamWriter.WriteRecord(makeRecord(header1, content1)); err != nil {
			t.Fatal(err)
		}
		if _, err := streamWriter.WriteRecord(makeRecord(header2, content2)); err != nil {
			t.Fatal(err)
		}
		if err := compressor.Close(); err != nil {
			t.Fatal(err)
		}
		streamReader, err := NewReader(bytes.NewReader(streamed.Bytes()))
		if err != nil {
			t.Fatalf("NewReader on streamed gzip failed: %v", err)
		}
		streamRecord1, err := streamReader.ReadRecord()
		if err != nil {
			t.Fatal(err)
		}
		if got := readAllContent(t, streamRecord1); !bytes.Equal(got, content1) {
			t.Fatalf("stream record 1 mismatch: %q", got)
		}
		streamRecord2, err := streamReader.ReadRecord()
		if err != nil {
			t.Fatal(err)
		}
		if got := readAllContent(t, streamRecord2); !bytes.Equal(got, content2) {
			t.Fatalf("stream record 2 mismatch: %q", got)
		}
		streamReader.Close()
	})
}
