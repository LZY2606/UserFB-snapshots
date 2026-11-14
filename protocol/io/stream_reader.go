package io

import (
	"bytes"
	"context"
	"encoding/base64"
	"io"
	"strings"
	"sync"
	"time"
)

// StreamReader represents a stream reader.
type StreamReader struct {
	next  func(pos, size int) (*ReadReply, error)
	close func() error
	r     io.Reader
	pos   int
	eof   bool

	// Serialize Read calls (sequential handles must not be read
	// concurrently) and make Close idempotent.
	readMu  sync.Mutex
	closeMu sync.Mutex
	closed  bool
}

var _ io.ReadCloser = (*StreamReader)(nil)

// NewStreamReader returns a reader for io.Streams that implements io.Reader
// from the standard library.
//
// Every IO.read request carries an explicit offset, seeking to the current
// position in the stream.
func NewStreamReader(ctx context.Context, ioClient interface {
	Read(context.Context, *ReadArgs) (*ReadReply, error)
	Close(context.Context, *CloseArgs) error
}, handle StreamHandle,
) *StreamReader {
	args := NewReadArgs(handle)
	return &StreamReader{
		next: func(pos, size int) (*ReadReply, error) {
			args.SetOffset(pos).SetSize(size)
			return ioClient.Read(ctx, args)
		},
		close: newStreamCloseFunc(ioClient, handle),
	}
}

// NewSequentialStreamReader returns a reader for io.Streams that implements
// io.Reader from the standard library.
//
// Unlike NewStreamReader, the offset is omitted on every IO.read request.
// This is required for sequential streams (e.g. those returned by
// Fetch.takeResponseBodyAsStream) that reject positioned reads. Plain text
// and base64 encoded chunks may be interleaved by the remote.
func NewSequentialStreamReader(ctx context.Context, ioClient interface {
	Read(context.Context, *ReadArgs) (*ReadReply, error)
	Close(context.Context, *CloseArgs) error
}, handle StreamHandle,
) *StreamReader {
	args := NewReadArgs(handle)
	return &StreamReader{
		next: func(pos, size int) (*ReadReply, error) {
			// Deliberately do not set Offset: sequential streams
			// forbid positioned IO.read requests.
			args.SetSize(size)
			return ioClient.Read(ctx, args)
		},
		close: newStreamCloseFunc(ioClient, handle),
	}
}

func newStreamCloseFunc(ioClient interface {
	Close(context.Context, *CloseArgs) error
}, handle StreamHandle,
) func() error {
	return func() error {
		// TODO(mafredri): We should ideally allow the user to define a timeout here.
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		return ioClient.Close(ctx, NewCloseArgs(handle))
	}
}

func (r *StreamReader) read(p []byte) (n int, err error) {
	n, err = r.r.Read(p)
	r.pos += n
	if !r.eof && err == io.EOF {
		r.r = nil
		err = nil
	}
	return n, err
}

// Read a chunk of the stream.
func (r *StreamReader) Read(p []byte) (n int, err error) {
	r.readMu.Lock()
	defer r.readMu.Unlock()

	if r.r != nil {
		// Continue reading from buffer.
		return r.read(p)
	}
	if r.eof {
		return 0, io.EOF
	}
	if len(p) == 0 {
		return 0, nil
	}

	// Chrome might have an off-by-one when deciding the maximum
	// size (at least for base64 encoded data), usually it will
	// overflow. We subtract one to make sure it fits into p.
	size := len(p) - 1
	if size < 1 {
		// Safety-check to avoid crashing Chrome (e.g. via SetSize(-1)).
		size = 1
	}

	reply, err := r.next(r.pos, size)
	if err != nil {
		return 0, err
	}

	r.eof = reply.EOF

	switch {
	case reply.Base64Encoded != nil && *reply.Base64Encoded:
		b := []byte(reply.Data)
		size := base64.StdEncoding.DecodedLen(len(b))

		// Validate up front so corrupt base64 is reported as an
		// error instead of being silently truncated by the decoder.
		if _, err := base64.StdEncoding.DecodeString(reply.Data); err != nil {
			return 0, err
		}

		// Safety-check for fast-path to avoid panics.
		if len(p) >= size {
			n, err = base64.StdEncoding.Decode(p, b)
			r.pos += n
			return n, err
		}

		r.r = base64.NewDecoder(base64.StdEncoding, bytes.NewReader(b))
	default:
		r.r = strings.NewReader(reply.Data)
	}

	return r.read(p)
}

// Close the stream, discard any temporary backing storage.
//
// Close is safe to call multiple times; only the first call invokes
// IO.close on the remote.
func (r *StreamReader) Close() error {
	r.closeMu.Lock()
	defer r.closeMu.Unlock()
	if r.closed {
		return nil
	}
	r.closed = true
	return r.close()
}
