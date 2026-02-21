// Package storage persists crawled page results to JSONL files.
package storage

import (
	"encoding/json"
	"fmt"
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

// Writer writes records to a JSONL file in a thread-safe manner.
type Writer struct {
	mu  sync.Mutex
	f   *os.File
	enc *json.Encoder
}

// NewWriter opens (or creates) path for append-only JSONL output.
func NewWriter(path string) (*Writer, error) {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return nil, fmt.Errorf("storage: open %s: %w", path, err)
	}
	return &Writer{f: f, enc: json.NewEncoder(f)}, nil
}

// Write appends one Record to the file.
func (w *Writer) Write(r Record) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.enc.Encode(r)
}

// Close closes the underlying file.
func (w *Writer) Close() error {
	return w.f.Close()
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
