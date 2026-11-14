// Package storage persists crawled page results to JSONL files.
package storage

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"sync"
	"time"
)

// Record is the JSON structure written for each crawled page.
type Record struct {
	URL        string            `json:"url"`
	StatusCode int               `json:"status_code"`
	Title      string            `json:"title"`
	Content    string            `json:"content,omitempty"`
	Links      []string          `json:"links"`
	Headers    map[string]string `json:"headers"`
	FetchedAt  time.Time         `json:"fetched_at"`
	Depth      int               `json:"depth"`
	Error      string            `json:"error,omitempty"`
}

// errWriterClosed is returned when Write is called after Close.
var errWriterClosed = fmt.Errorf("storage: writer already closed: %w", os.ErrClosed)

// Writer writes records to a JSONL file in a thread-safe manner.
//
// A Write either appends exactly one complete JSON object followed by a
// newline, or fails and leaves the file at the length it had before the
// attempt. After the first failure the Writer enters a permanent failed
// state: further Writes (including concurrent ones) never touch the file
// again and keep reporting the original cause, which remains reachable via
// errors.Is.
type Writer struct {
	mu      sync.Mutex
	f       *os.File
	failed  bool
	closed  bool
	failErr error // sticky cause of the first failure; always non-nil when failed
}

// NewWriter opens (or creates) path for append-only JSONL output.
// Bytes already present in the file are never modified.
func NewWriter(path string) (*Writer, error) {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return nil, fmt.Errorf("storage: open %s: %w", path, err)
	}
	return &Writer{f: f}, nil
}

// Write appends one Record as a single JSON line (JSON plus trailing
// newline). If the underlying file accepts only part of the line, the
// partial bytes are removed by truncating the file back to its length at
// the start of this Write, and the write error is returned. On the first
// failure the Writer becomes permanently failed and all subsequent Writes
// return the same cause without modifying the file.
func (w *Writer) Write(r Record) error {
	data, err := json.Marshal(r)
	if err != nil {
		// Encoding never touched the file, so the Writer stays usable.
		return fmt.Errorf("storage: encode record for %s: %w", r.URL, err)
	}
	data = append(data, '\n')

	w.mu.Lock()
	defer w.mu.Unlock()

	if w.failed {
		return w.failErr
	}
	if w.closed {
		w.markFailed(errWriterClosed)
		return w.failErr
	}

	info, err := w.f.Stat()
	if err != nil {
		err = fmt.Errorf("storage: stat output before write: %w", err)
		w.markFailed(err)
		return err
	}
	start := info.Size()

	n, werr := w.f.Write(data)
	switch {
	case werr != nil:
		err = fmt.Errorf("storage: write record for %s: %w", r.URL, werr)
	case n < len(data):
		err = fmt.Errorf("storage: short write for %s: wrote %d of %d bytes: %w", r.URL, n, len(data), io.ErrShortWrite)
	default:
		return nil
	}

	// Roll the partial line back so no next record can join a broken line.
	if terr := w.f.Truncate(start); terr != nil {
		// Keep both causes; do not pretend the rollback succeeded.
		err = errors.Join(err, fmt.Errorf("storage: restore file length to %d: %w", start, terr))
	}
	w.markFailed(err)
	return w.failErr
}

// markFailed records the first failure cause. Callers must hold w.mu.
func (w *Writer) markFailed(err error) {
	if !w.failed {
		w.failed = true
		w.failErr = err
	}
}

// Err returns the sticky failure cause, or nil while the Writer is healthy.
func (w *Writer) Err() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.failErr
}

// Close closes the underlying file. A close error is preserved alongside
// any earlier write failure and remains reachable through Err and
// errors.Is. It is safe to call more than once.
func (w *Writer) Close() error {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.closed {
		return nil
	}
	w.closed = true

	cerr := w.f.Close()
	if cerr == nil {
		return nil
	}
	closeErr := fmt.Errorf("storage: close output: %w", cerr)
	if w.failed {
		w.failErr = errors.Join(w.failErr, closeErr)
	} else {
		w.markFailed(closeErr)
	}
	return cerr
}

// URLsToStrings converts a slice of *url.URL to strings.
func URLsToStrings(urls []*url.URL) []string {
	out := make([]string, len(urls))
	for i, u := range urls {
		out[i] = u.String()
	}
	return out
}

// HeadersFlat returns the first value for each header key.
func HeadersFlat(h map[string][]string) map[string]string {
	out := make(map[string]string, len(h))
	for k, vs := range h {
		if len(vs) > 0 {
			out[k] = vs[0]
		}
	}
	return out
}
