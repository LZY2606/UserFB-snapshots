package crawler

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/0xfurai/goliath/internal/storage"
)

type taskTransport struct{ pages atomic.Int64 }

func (r *taskTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	body := "User-agent: *\nAllow: /\n"
	contentType := "text/plain"
	if req.URL.Path != "/robots.txt" {
		r.pages.Add(1)
		contentType = "text/html"
		body = "<html><head><title>child</title></head><body>child text</body></html>"
		if req.URL.Path == "/root" {
			body = `<html><head><title>root</title></head><body><p>root text</p><a href="/child">child</a></body></html>`
		}
	}
	return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": {contentType}}, Body: io.NopCloser(strings.NewReader(body)), Request: req}, nil
}

func taskCrawler(t *testing.T) (*Crawler, *taskTransport, string) {
	t.Helper()
	out := filepath.Join(t.TempDir(), "pages.jsonl")
	c, err := New(Config{Seeds: []string{"https://collection.example.invalid/root"}, Workers: 2, MaxDepth: 1, MinDelay: time.Nanosecond, OutputFile: out}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatal(err)
	}
	tr := &taskTransport{}
	c.fetcher.Client().Transport = tr
	return c, tr, out
}

func TestTaskHealthy(t *testing.T) {
	c, tr, path := taskCrawler(t)
	s := c.Run(context.Background())
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	scan := bufio.NewScanner(f)
	var records []storage.Record
	for scan.Scan() {
		var r storage.Record
		if err := json.Unmarshal(scan.Bytes(), &r); err != nil {
			t.Fatal(err)
		}
		records = append(records, r)
	}
	if scan.Err() != nil {
		t.Fatal(scan.Err())
	}
	if s.Fetched != 2 || tr.pages.Load() != 2 || len(records) != 2 || records[0].Title != "root" || records[1].Depth != 1 {
		t.Fatalf("healthy path: stats=%+v pages=%d records=%+v", s, tr.pages.Load(), records)
	}
	fmt.Println("GOLIATH_HEALTHY_OK fetched=2 records=2 child_depth=1")
}

// Closed output now follows the failure path: the failed page is not
// counted as fetched, no child links are dispatched, and Err exposes the
// underlying closed-file error through errors.Is.
func TestTaskClosedOutputProbe(t *testing.T) {
	c, tr, path := taskCrawler(t)
	if err := c.writer.Close(); err != nil {
		t.Fatal(err)
	}
	s := c.Run(context.Background())
	info, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if s.Fetched != 0 || tr.pages.Load() != 1 || info.Size() != 0 {
		t.Fatalf("failure path differs: fetched=%d pages=%d bytes=%d", s.Fetched, tr.pages.Load(), info.Size())
	}
	if c.Err() == nil || !errors.Is(c.Err(), os.ErrClosed) {
		t.Fatalf("Err must wrap os.ErrClosed, got %v", c.Err())
	}
	fmt.Println("GOLIATH_CLOSED_OUTPUT_HANDLED fetched=0 page_requests=1 output_bytes=0 err=os.ErrClosed")
}
