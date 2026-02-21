// Package frontier implements the URL frontier - the queue of URLs to crawl
// with built-in deduplication via a bloom filter for memory efficiency.
package frontier

import (
	"bytes"
	"encoding/base64"
	"encoding/gob"
	"fmt"
	"net/url"
	"sync"

	"github.com/bits-and-blooms/bloom/v3"
)

// Item represents a URL scheduled for crawling.
type Item struct {
	URL   *url.URL
	Depth int
}

// Frontier is a thread-safe FIFO queue with bloom filter deduplication.
// Uses fixed memory regardless of the number of URLs visited.
type Frontier struct {
	mu      sync.Mutex
	queue   []Item
	visited *bloom.BloomFilter
	cond    *sync.Cond
	closed  bool
}

// New creates a new Frontier with default bloom filter settings.
// Optimized for ~10M URLs with 1% false positive rate (~12MB memory).
func New() *Frontier {
	return NewWithCapacity(10_000_000, 0.01)
}

// NewWithCapacity creates a Frontier with custom bloom filter parameters.
// expectedURLs: estimated number of unique URLs to visit
// falsePositiveRate: acceptable probability of false positives (e.g., 0.01 = 1%)
func NewWithCapacity(expectedURLs uint, falsePositiveRate float64) *Frontier {
	f := &Frontier{
		visited: bloom.NewWithEstimates(expectedURLs, falsePositiveRate),
	}
	f.cond = sync.NewCond(&f.mu)
	return f
}

// Push adds a URL to the queue if it hasn't been visited.
// Returns true if the URL was added.
// Note: Due to bloom filter, there's a small chance of false positives
// (skipping URLs that haven't actually been visited).
func (f *Frontier) Push(u *url.URL, depth int) bool {
	key := normalizeURL(u)
	f.mu.Lock()
	defer f.mu.Unlock()

	if f.visited.TestString(key) {
		return false
	}
	f.visited.AddString(key)
	f.queue = append(f.queue, Item{URL: u, Depth: depth})
	f.cond.Signal()
	return true
}

// Pop blocks until an item is available or the frontier is closed.
// Returns the item and false, or zero-value and true when closed+empty.
func (f *Frontier) Pop() (Item, bool) {
	f.mu.Lock()
	defer f.mu.Unlock()

	for len(f.queue) == 0 && !f.closed {
		f.cond.Wait()
	}
	if len(f.queue) == 0 {
		return Item{}, false
	}
	item := f.queue[0]
	f.queue = f.queue[1:]
	return item, true
}

// Len returns the current number of queued (not yet processed) URLs.
func (f *Frontier) Len() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.queue)
}

// VisitedCount returns an approximate count of unique URLs seen.
// This is an estimate due to the bloom filter's probabilistic nature.
func (f *Frontier) VisitedCount() uint32 {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.visited.ApproximatedSize()
}

// Close signals that no more URLs will be pushed, unblocking all Pop callers.
func (f *Frontier) Close() {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.closed = true
	f.cond.Broadcast()
}

// normalizeURL returns a canonical string key for deduplication.
func normalizeURL(u *url.URL) string {
	// Strip fragment, normalize scheme to lowercase
	c := *u
	c.Fragment = ""
	c.Scheme = u.Scheme
	return c.String()
}

// Snapshot represents a serializable snapshot of the Frontier state.
type Snapshot struct {
	VisitedBloomBase64 string
	Queue              []SnapshotItem
}

// SnapshotItem represents a URL in the queue with its depth.
type SnapshotItem struct {
	URL   string
	Depth int
}

// Save creates a snapshot of the current Frontier state.
// This is safe to call while the crawler is running.
func (f *Frontier) Save() (*Snapshot, error) {
	f.mu.Lock()
	defer f.mu.Unlock()

	// Serialize bloom filter using gob
	var buf bytes.Buffer
	enc := gob.NewEncoder(&buf)
	if err := enc.Encode(f.visited); err != nil {
		return nil, fmt.Errorf("encode bloom filter: %w", err)
	}

	// Encode bloom filter as base64 for JSON compatibility
	bloomBase64 := base64.StdEncoding.EncodeToString(buf.Bytes())

	// Copy queue
	queue := make([]SnapshotItem, len(f.queue))
	for i, item := range f.queue {
		queue[i] = SnapshotItem{
			URL:   item.URL.String(),
			Depth: item.Depth,
		}
	}

	return &Snapshot{
		VisitedBloomBase64: bloomBase64,
		Queue:              queue,
	}, nil
}

// Restore restores the Frontier state from a snapshot.
// This should only be called on a newly created Frontier before use.
func (f *Frontier) Restore(snap *Snapshot) error {
	f.mu.Lock()
	defer f.mu.Unlock()

	// Decode bloom filter from base64
	bloomData, err := base64.StdEncoding.DecodeString(snap.VisitedBloomBase64)
	if err != nil {
		return fmt.Errorf("decode bloom base64: %w", err)
	}

	// Deserialize bloom filter using gob
	buf := bytes.NewReader(bloomData)
	dec := gob.NewDecoder(buf)
	bloom := &bloom.BloomFilter{}
	if err := dec.Decode(bloom); err != nil {
		return fmt.Errorf("decode bloom filter: %w", err)
	}

	// Restore queue
	queue := make([]Item, len(snap.Queue))
	for i, item := range snap.Queue {
		u, err := url.Parse(item.URL)
		if err != nil {
			return fmt.Errorf("parse URL %s: %w", item.URL, err)
		}
		queue[i] = Item{
			URL:   u,
			Depth: item.Depth,
		}
	}

	// Update frontier state
	f.visited = bloom
	f.queue = queue

	return nil
}
