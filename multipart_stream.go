package graphql

import (
	"bufio"
	"bytes"
	stderrors "errors"
	"fmt"
	"io"
	"net/textproto"
	"strings"
)

// errMultipartTruncated marks a response whose stream ended before the MIME
// framing was complete. It wraps io.ErrUnexpectedEOF so callers can detect
// truncation either way.
var errMultipartTruncated = fmt.Errorf("multipart stream truncated: %w", io.ErrUnexpectedEOF)

// mixedStreamReader is a streaming parser for multipart/mixed response bodies.
//
// It exists instead of mime/multipart.Reader because the incremental protocol
// requires distinguishing a properly closed stream (the "--boundary--" line)
// from a truncated one after the final part: mime/multipart reports io.EOF in
// both cases. Parts are read one at a time, so the server can block between
// flushes and wait for the client callback.
type mixedStreamReader struct {
	br       *bufio.Reader
	tr       *textproto.Reader
	boundary string
	open     string // "--boundary"
	close    string // "--boundary--"
	started  bool   // The first open boundary has been consumed.
	closed   bool   // The closing boundary has been consumed.
	pending  int    // Marker already consumed at the end of the previous part.
}

func newMixedStreamReader(r io.Reader, boundary string) *mixedStreamReader {
	br := bufio.NewReader(r)
	return &mixedStreamReader{
		br:       br,
		tr:       textproto.NewReader(br),
		boundary: boundary,
		open:     "--" + boundary,
		close:    "--" + boundary + "--",
	}
}

// markerClass classifies a boundary line. 0 is not a boundary, 1 opens the
// next part, 2 closes the stream.
func (m *mixedStreamReader) markerClass(line string) int {
	line = strings.TrimRight(line, " \t") // Strip transport padding.
	switch line {
	case m.close:
		return 2
	case m.open:
		return 1
	default:
		return 0
	}
}

// readMarker returns the next boundary marker line, skipping the optional
// preamble before the first one. An EOF anywhere a marker is required is a
// protocol error rather than a clean end of stream.
func (m *mixedStreamReader) readMarker() (int, error) {
	for {
		line, err := m.tr.ReadLine()
		if err != nil {
			if stderrors.Is(err, io.EOF) {
				return 0, errMultipartTruncated
			}
			return 0, err
		}
		switch m.markerClass(line) {
		case 1:
			m.started = true
			return 1, nil
		case 2:
			m.closed = true
			return 2, nil
		default:
			if m.started {
				return 0, fmt.Errorf("expected MIME boundary %q, got %q", m.open, line)
			}
			// Still in the preamble; ignore its contents.
		}
	}
}

// NextPart returns the body of the next part. It returns io.EOF only after
// the closing boundary line has been read and verified.
func (m *mixedStreamReader) NextPart() ([]byte, error) {
	if m.closed {
		return nil, io.EOF
	}
	// The marker at the end of the previous part's body was already consumed.
	if m.pending != 0 {
		marker := m.pending
		m.pending = 0
		if marker == 2 {
			m.closed = true
			return nil, io.EOF
		}
	} else if marker, err := m.readMarker(); err != nil {
		return nil, err
	} else if marker == 2 {
		return nil, io.EOF
	}

	// Header block, terminated by an empty line.
	if _, err := m.tr.ReadMIMEHeader(); err != nil {
		if stderrors.Is(err, io.EOF) {
			err = errMultipartTruncated
		}
		return nil, fmt.Errorf("reading MIME part headers: %w", err)
	}

	// Body: everything up to the next boundary line. The line is matched at
	// column 0, so JSON content (which cannot contain raw newlines inside
	// strings) can never be mistaken for a boundary.
	var body bytes.Buffer
	for {
		line, err := m.br.ReadBytes('\n')
		if len(line) > 0 {
			stripped := strings.TrimRight(string(line), "\r\n")
			if marker := m.markerClass(stripped); marker != 0 {
				m.pending = marker
				return body.Bytes(), nil
			}
			body.Write(line)
		}
		if err != nil {
			if stderrors.Is(err, io.EOF) {
				return nil, errMultipartTruncated
			}
			return nil, err
		}
	}
}
