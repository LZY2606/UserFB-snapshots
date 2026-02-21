// Package checkpoint implements save/restore functionality for crawler state.
package checkpoint

import (
	"encoding/json"
	"fmt"
	"os"
	"time"
)

// Checkpoint represents the complete state of a crawl at a point in time.
type Checkpoint struct {
	Version   string    `json:"version"`
	CreatedAt time.Time `json:"created_at"`
	Config    Config    `json:"config"`
	Stats     Stats     `json:"stats"`
	Frontier  Frontier  `json:"frontier"`
}

// Config stores the crawler configuration.
type Config struct {
	Seeds      []string `json:"seeds"`
	MaxDepth   int      `json:"max_depth"`
	SameDomain bool     `json:"same_domain"`
	UserAgent  string   `json:"user_agent"`
}

// Stats stores crawler statistics.
type Stats struct {
	Fetched  int64 `json:"fetched"`
	Errors   int64 `json:"errors"`
	Enqueued int64 `json:"enqueued"`
	Skipped  int64 `json:"skipped"`
}

// Frontier stores the URL queue and visited state.
type Frontier struct {
	VisitedBloomBase64 string      `json:"visited_bloom_base64"`
	QueueLength        int         `json:"queue_length"`
	Queue              []QueueItem `json:"queue"`
}

// QueueItem represents a URL in the queue with its depth.
type QueueItem struct {
	URL   string `json:"url"`
	Depth int    `json:"depth"`
}

// Save writes a checkpoint to disk atomically.
// Uses a temporary file + rename to ensure atomicity.
func Save(path string, ckpt *Checkpoint) error {
	ckpt.Version = "1.0"
	ckpt.CreatedAt = time.Now()

	// Write to temporary file first
	tmpPath := path + ".tmp"
	f, err := os.Create(tmpPath)
	if err != nil {
		return fmt.Errorf("create temp checkpoint: %w", err)
	}

	enc := json.NewEncoder(f)
	enc.SetIndent("", "  ")
	if err := enc.Encode(ckpt); err != nil {
		f.Close()
		os.Remove(tmpPath)
		return fmt.Errorf("encode checkpoint: %w", err)
	}

	if err := f.Close(); err != nil {
		os.Remove(tmpPath)
		return fmt.Errorf("close temp checkpoint: %w", err)
	}

	// Atomic rename
	if err := os.Rename(tmpPath, path); err != nil {
		os.Remove(tmpPath)
		return fmt.Errorf("rename checkpoint: %w", err)
	}

	return nil
}

// Load reads a checkpoint from disk.
func Load(path string) (*Checkpoint, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open checkpoint: %w", err)
	}
	defer f.Close()

	var ckpt Checkpoint
	if err := json.NewDecoder(f).Decode(&ckpt); err != nil {
		return nil, fmt.Errorf("decode checkpoint: %w", err)
	}

	// Validate version
	if ckpt.Version != "1.0" {
		return nil, fmt.Errorf("unsupported checkpoint version: %s", ckpt.Version)
	}

	return &ckpt, nil
}

// ValidateConfig checks if the checkpoint config is compatible with the current config.
func ValidateConfig(current, saved Config) error {
	if current.MaxDepth != saved.MaxDepth {
		return fmt.Errorf("max_depth mismatch: current=%d, checkpoint=%d", current.MaxDepth, saved.MaxDepth)
	}
	if current.SameDomain != saved.SameDomain {
		return fmt.Errorf("same_domain mismatch: current=%v, checkpoint=%v", current.SameDomain, saved.SameDomain)
	}
	// Seeds can differ (user might want to add more seeds on resume)
	// UserAgent can differ (might want to change)
	return nil
}
