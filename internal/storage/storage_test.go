package storage

import (
	"bufio"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

func sampleRecord(url string) Record {
	return Record{
		URL:       url,
		Title:     "t",
		Links:     []string{},
		FetchedAt: time.Unix(1700000000, 0).UTC(),
		Depth:     0,
	}
}

func readLines(t *testing.T, path string) []string {
	t.Helper()
	f, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	var lines []string
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 64*1024), 4*1024*1024)
	for sc.Scan() {
		lines = append(lines, sc.Text())
	}
	if err := sc.Err(); err != nil {
		t.Fatal(err)
	}
	return lines
}

func TestWriterAppendsAndKeepsPrefixIntact(t *testing.T) {
	path := filepath.Join(t.TempDir(), "pages.jsonl")

	first, err := NewWriter(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := first.Write(sampleRecord("https://example.invalid/seed")); err != nil {
		t.Fatal(err)
	}
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}

	prefix, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}

	// A second writer must append, never overwrite or alter the prefix.
	second, err := NewWriter(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := second.Write(sampleRecord("https://example.invalid/next")); err != nil {
		t.Fatal(err)
	}
	if err := second.Close(); err != nil {
		t.Fatal(err)
	}

	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) <= len(prefix) || string(got[:len(prefix)]) != string(prefix) {
		t.Fatalf("existing prefix bytes changed:\nprefix=%x\ngot   =%x", prefix, got)
	}

	lines := readLines(t, path)
	if len(lines) != 2 {
		t.Fatalf("want 2 complete lines, got %d", len(lines))
	}
	for i, line := range lines {
		var r Record
		if err := json.Unmarshal([]byte(line), &r); err != nil {
			t.Fatalf("line %d is not valid JSON: %v", i, err)
		}
		if line[len(line)-1] != '}' {
			t.Fatalf("line %d has trailing bytes after the JSON object", i)
		}
	}
}

func TestWriterFailsStickyAfterCloseAndPreservesFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "pages.jsonl")
	w, err := NewWriter(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := w.Write(sampleRecord("https://example.invalid/one")); err != nil {
		t.Fatal(err)
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}

	info, _ := os.Stat(path)
	before := info.Size()

	for i := 0; i < 3; i++ {
		err := w.Write(sampleRecord("https://example.invalid/rejected"))
		if !errors.Is(err, os.ErrClosed) {
			t.Fatalf("write %d after Close: want os.ErrClosed, got %v", i, err)
		}
	}
	if !errors.Is(w.Err(), os.ErrClosed) {
		t.Fatalf("Err() must wrap os.ErrClosed, got %v", w.Err())
	}
	info, _ = os.Stat(path)
	if info.Size() != before {
		t.Fatalf("failed writes changed the file: before=%d after=%d", before, info.Size())
	}

	// Closing twice is harmless and does not fabricate an error.
	if err := w.Close(); err != nil {
		t.Fatalf("second Close returned error: %v", err)
	}
	if lines := readLines(t, path); len(lines) != 1 {
		t.Fatalf("want exactly one intact line, got %d", len(lines))
	}
}

func TestWriterConcurrentWritesAreCompleteLines(t *testing.T) {
	path := filepath.Join(t.TempDir(), "pages.jsonl")
	w, err := NewWriter(path)
	if err != nil {
		t.Fatal(err)
	}

	const n = 50
	var wg sync.WaitGroup
	errs := make(chan error, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			errs <- w.Write(sampleRecord("https://example.invalid/page-" + string(rune('a'+i%26)) + string(rune('0'+i/26))))
		}(i)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Fatal(err)
		}
	}
	if err := w.Close(); err != nil {
		t.Fatal(err)
	}

	lines := readLines(t, path)
	if len(lines) != n {
		t.Fatalf("want %d complete lines, got %d", n, len(lines))
	}
	seen := map[string]bool{}
	for _, line := range lines {
		var r Record
		if err := json.Unmarshal([]byte(line), &r); err != nil {
			t.Fatalf("corrupt line %q: %v", line, err)
		}
		if seen[r.URL] {
			t.Fatalf("duplicated record %s", r.URL)
		}
		seen[r.URL] = true
	}
}
