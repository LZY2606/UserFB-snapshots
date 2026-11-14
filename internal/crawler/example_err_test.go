package crawler

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"time"
)

// ExampleCrawler_Err shows how to detect that a crawl ended because the
// JSONL output failed. Run has already drained every worker and closed the
// file by the time Err is consulted; the original cause stays reachable
// through errors.Is.
func ExampleCrawler_Err() {
	out := filepath.Join(os.TempDir(), "goliath-example-err.jsonl")
	defer os.Remove(out)

	c, err := New(Config{
		Seeds:      []string{"https://example.invalid/root"},
		Workers:    1,
		MinDelay:   time.Nanosecond,
		OutputFile: out,
	}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		fmt.Println("new:", err)
		return
	}
	// In-memory transport defined in task_baseline_test.go: no network.
	c.fetcher.Client().Transport = &taskTransport{}

	// Simulate the output file becoming unavailable before the crawl runs.
	_ = c.writer.Close()

	c.Run(context.Background())

	switch err := c.Err(); {
	case errors.Is(err, os.ErrClosed):
		fmt.Println("output failed: os.ErrClosed")
	case err != nil:
		fmt.Println("output failed:", err)
	default:
		fmt.Println("clean run")
	}
	// Output:
	// output failed: os.ErrClosed
}
