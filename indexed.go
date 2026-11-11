package warc

import (
	"bytes"
	"compress/gzip"
	"errors"
	"fmt"
	"io"
	"math"
	"strconv"
)

// Member describes the physical location of a single compressed capture
// inside the output of an IndexedWriter. Offset and Length form a
// half-open byte interval [Offset, Offset+Length) relative to the start
// of the underlying writer. Length covers the complete gzip member,
// including the gzip header and trailer.
type Member struct {
	Offset int64
	Length int64
}

// IndexedWriter writes WARC records as independent, separately compressed
// gzip members. Every successfully written record is exactly one gzip
// member containing exactly one ordinary WARC record, and the returned
// Member lets an archive index locate that capture without scanning the
// whole file.
//
// After WriteRecord returns an error, the IndexedWriter is spent:
// subsequent calls fail with the same error without touching any new
// record content or the underlying writer.
type IndexedWriter struct {
	target io.Writer
	offset int64
	err    error
}

// countedWriter forwards writes to an underlying writer and counts the
// bytes that were accepted. A short write that reports no error is
// converted to io.ErrShortWrite, so a misbehaving underlying writer
// cannot silently truncate a gzip member.
type countedWriter struct {
	target io.Writer
	count  int64
}

func (w *countedWriter) Write(p []byte) (int, error) {
	n, err := w.target.Write(p)
	w.count += int64(n)
	if err != nil {
		return n, err
	}
	if n < len(p) {
		return n, io.ErrShortWrite
	}
	return n, nil
}

// NewIndexedWriter creates a new IndexedWriter that emits one gzip member
// per record to writer.
func NewIndexedWriter(writer io.Writer) *IndexedWriter {
	return &IndexedWriter{target: writer}
}

// WriteRecord compresses record into a standalone gzip member and writes
// it to the underlying writer. The WARC field encoding is identical to
// Writer.WriteRecord. On success the returned Member covers the complete
// member, including gzip header and trailer; on failure the zero Member
// is returned together with the original error and the IndexedWriter
// stops accepting further records.
func (w *IndexedWriter) WriteRecord(record *Record) (Member, error) {
	if w.err != nil {
		return Member{}, w.err
	}

	// Encode the WARC record exactly the way the plain Writer does.
	// Writing into an in-memory buffer cannot fail for I/O reasons;
	// a failure here stems from reading the record's content.
	var buffer bytes.Buffer
	if _, err := NewWriter(&buffer).WriteRecord(record); err != nil {
		return w.fail(err)
	}

	start := w.offset
	counter := &countedWriter{target: w.target}
	compressor := gzip.NewWriter(counter)
	if _, err := compressor.Write(buffer.Bytes()); err != nil {
		return w.fail(err)
	}
	// Close flushes the gzip trailer; the member is only complete once
	// the trailer has been written successfully.
	if err := compressor.Close(); err != nil {
		return w.fail(err)
	}

	length := counter.count
	if length <= 0 {
		return w.fail(errors.New("warc: wrote empty gzip member"))
	}
	end := w.offset + length
	if end < w.offset || end > math.MaxInt64 {
		return w.fail(errors.New("warc: output offset overflow"))
	}
	w.offset = end
	return Member{Offset: start, Length: length}, nil
}

// fail records the first error on the writer so that all future calls
// fail with that same original error.
func (w *IndexedWriter) fail(err error) (Member, error) {
	if w.err == nil {
		w.err = err
	}
	return Member{}, err
}

// ReadMemberAt decompresses and parses a single capture located by member
// inside the random-access input ra. Reads never leave the byte interval
// described by member. The returned Record is independently consumable:
// its content lives in memory and does not depend on ra or any gzip
// state after the call returns.
//
// The interval must contain exactly one gzip member, which in turn must
// contain exactly one WARC record with no trailing decompressed bytes.
// Anything else (negative or zero-length intervals, offset overflow,
// truncation, gzip checksum failures, multiple members, multiple WARC
// records, stray trailing bytes) results in an error. The gzip trailer
// is always verified before a record is returned.
func ReadMemberAt(ra io.ReaderAt, member Member) (*Record, error) {
	if member.Offset < 0 {
		return nil, fmt.Errorf("warc: invalid member offset %d", member.Offset)
	}
	if member.Length <= 0 {
		return nil, fmt.Errorf("warc: invalid member length %d", member.Length)
	}
	if member.Offset > math.MaxInt64-member.Length {
		return nil, fmt.Errorf("warc: member interval overflows: offset=%d length=%d",
			member.Offset, member.Length)
	}

	// Copy the whole interval through a SectionReader: every underlying
	// ReadAt call is confined to [Offset, Offset+Length), so no read can
	// borrow bytes from a neighboring capture, and a short underlying
	// source makes ReadFull fail instead of being padded.
	section := io.NewSectionReader(ra, member.Offset, member.Length)
	compressed := make([]byte, member.Length)
	if _, err := io.ReadFull(section, compressed); err != nil {
		return nil, fmt.Errorf("warc: failed to read member interval: %w", err)
	}

	// A *bytes.Reader is used directly by the gzip/flate code without an
	// extra buffering layer, so the reader stops at the exact member
	// boundary and a second member or trailing garbage stays detectable.
	input := bytes.NewReader(compressed)
	decompressor, err := gzip.NewReader(input)
	if err != nil {
		return nil, fmt.Errorf("warc: failed to open gzip member: %w", err)
	}
	// Do not transparently continue into a possible second member.
	decompressor.Multistream(false)

	// ReadAll runs through the gzip trailer, so a checksum mismatch or a
	// truncated member is reported here rather than silently accepted.
	data, err := io.ReadAll(decompressor)
	if err != nil {
		return nil, fmt.Errorf("warc: failed to decompress member: %w", err)
	}

	// Reset positions the gzip reader at the next member. A clean
	// interval has no next member, so this must report io.EOF; nil means
	// a second gzip member is present, any other error means trailing
	// garbage after the member.
	if err := decompressor.Reset(input); err != io.EOF {
		if err == nil {
			return nil, errors.New("warc: interval contains more than one gzip member")
		}
		return nil, fmt.Errorf("warc: trailing bytes after gzip member: %w", err)
	}
	if input.Len() != 0 {
		return nil, fmt.Errorf("warc: %d unconsumed bytes after gzip member", input.Len())
	}

	// Parse the decompressed bytes in AsynchronousMode so the record
	// content is copied into memory and stays valid on its own once the
	// internal reader is closed.
	reader, err := NewReaderMode(bytes.NewReader(data), AsynchronousMode)
	if err != nil {
		return nil, fmt.Errorf("warc: failed to parse member content: %w", err)
	}
	defer reader.Close()

	var result *Record
	count := 0
	for {
		record, err := reader.ReadRecord()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, fmt.Errorf("warc: failed to parse WARC record: %w", err)
		}
		count++
		if count > 1 {
			return nil, errors.New("warc: gzip member contains more than one WARC record")
		}
		result = record
	}
	if count == 0 {
		return nil, errors.New("warc: gzip member contains no WARC record")
	}
	if err := verifySingleWARCFrame(data, result); err != nil {
		return nil, err
	}
	return result, nil
}

// verifySingleWARCFrame makes sure the decompressed payload is exactly one
// WARC record frame: a single version line, one header section terminated
// by CRLF CRLF, then precisely Content-Length content bytes and the two
// trailing CRLFs. The lenient streaming Reader would otherwise swallow a
// second record or stray decompressed bytes after the first record.
func verifySingleWARCFrame(data []byte, record *Record) error {
	const frameEnd = "\r\n\r\n"
	if !bytes.HasPrefix(data, []byte("WARC/")) {
		return errors.New("warc: member content does not start with a WARC version line")
	}
	headerEnd := bytes.Index(data, []byte(frameEnd))
	if headerEnd < 0 {
		return errors.New("warc: malformed WARC header section")
	}
	headerEnd += len(frameEnd)

	contentLength, err := strconv.Atoi(record.Header.Get("content-length"))
	if err != nil {
		return fmt.Errorf("warc: failed to parse field Content-Length: %w", err)
	}
	if contentLength < 0 || headerEnd+contentLength+len(frameEnd) != len(data) {
		return errors.New("warc: extra decompressed bytes after the WARC record")
	}
	if !bytes.HasSuffix(data, []byte(frameEnd)) {
		return errors.New("warc: WARC record is missing its trailing CRLFs")
	}
	return nil
}
