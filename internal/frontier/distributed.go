// Package frontier implements both local and distributed URL frontiers.
package frontier

import (
	"context"
	"encoding/json"
	"fmt"
	"net/url"
	"time"

	"github.com/redis/go-redis/v9"
)

// DistributedFrontier is a Redis-backed frontier for multi-machine crawling.
// It provides a shared queue and visited set across multiple crawler instances.
type DistributedFrontier struct {
	client     *redis.Client
	queueKey   string // Redis list key for the queue
	visitedKey string // Redis set key for visited URLs
	ctx        context.Context
	closed     bool
}

// RedisConfig holds Redis connection parameters.
type RedisConfig struct {
	Addr     string // Redis server address (e.g., "localhost:6379")
	Password string // Redis password (empty if no auth)
	DB       int    // Redis database number
	Prefix   string // Key prefix for this crawl (e.g., "crawl:example.com:")
}

// NewDistributed creates a distributed frontier backed by Redis.
func NewDistributed(ctx context.Context, cfg RedisConfig) (*DistributedFrontier, error) {
	if cfg.Prefix == "" {
		cfg.Prefix = "crawl:"
	}

	client := redis.NewClient(&redis.Options{
		Addr:     cfg.Addr,
		Password: cfg.Password,
		DB:       cfg.DB,
	})

	// Test connection
	if err := client.Ping(ctx).Err(); err != nil {
		return nil, fmt.Errorf("redis ping failed: %w", err)
	}

	return &DistributedFrontier{
		client:     client,
		queueKey:   cfg.Prefix + "queue",
		visitedKey: cfg.Prefix + "visited",
		ctx:        ctx,
	}, nil
}

// Push adds a URL to the distributed queue if it hasn't been visited.
// Returns true if the URL was added.
// Thread-safe across multiple machines via Redis atomic operations.
func (f *DistributedFrontier) Push(u *url.URL, depth int) bool {
	if f.closed {
		return false
	}

	key := normalizeURL(u)

	// Use SADD which returns 1 if added, 0 if already exists (atomic operation)
	added, err := f.client.SAdd(f.ctx, f.visitedKey, key).Result()
	if err != nil || added == 0 {
		return false
	}

	// Serialize the item (URL as string for JSON compatibility)
	itemData := struct {
		URL   string `json:"url"`
		Depth int    `json:"depth"`
	}{
		URL:   u.String(),
		Depth: depth,
	}
	data, err := json.Marshal(itemData)
	if err != nil {
		return false
	}

	// Push to the queue (right side = FIFO with LPOP)
	if err := f.client.RPush(f.ctx, f.queueKey, data).Err(); err != nil {
		// If queue push fails, remove from visited set to maintain consistency
		f.client.SRem(f.ctx, f.visitedKey, key)
		return false
	}

	return true
}

// Pop blocks until an item is available or context is done.
// Returns the item and true, or zero-value and false when closed/error.
func (f *DistributedFrontier) Pop() (Item, bool) {
	if f.closed {
		return Item{}, false
	}

	// BLPOP blocks until an item is available or timeout
	// Using 1 second timeout to allow checking f.closed periodically
	for {
		if f.closed {
			return Item{}, false
		}

		result, err := f.client.BLPop(f.ctx, 1*time.Second, f.queueKey).Result()
		if err == redis.Nil {
			// Timeout, continue loop
			continue
		}
		if err != nil {
			// Context canceled or other error
			return Item{}, false
		}

		// result[0] is the key, result[1] is the value
		var itemData struct {
			URL   string `json:"url"`
			Depth int    `json:"depth"`
		}
		if err := json.Unmarshal([]byte(result[1]), &itemData); err != nil {
			continue // Skip malformed items
		}

		u, err := url.Parse(itemData.URL)
		if err != nil {
			continue // Skip invalid URLs
		}

		return Item{URL: u, Depth: itemData.Depth}, true
	}
}

// PopContext blocks like Pop but also returns when ctx is done.
func (f *DistributedFrontier) PopContext(ctx context.Context) (Item, bool) {
	for {
		if f.closed {
			return Item{}, false
		}
		if err := ctx.Err(); err != nil {
			return Item{}, false
		}

		// Bound the BLPOP wait so ctx cancellation is observed promptly.
		result, err := f.client.BLPop(f.ctx, 250*time.Millisecond, f.queueKey).Result()
		if err == redis.Nil {
			continue // timeout: re-check ctx and closed before retrying
		}
		if err != nil {
			return Item{}, false
		}

		// result[0] is the key, result[1] is the value
		var itemData struct {
			URL   string `json:"url"`
			Depth int    `json:"depth"`
		}
		if err := json.Unmarshal([]byte(result[1]), &itemData); err != nil {
			continue // Skip malformed items
		}

		u, err := url.Parse(itemData.URL)
		if err != nil {
			continue // Skip invalid URLs
		}

		return Item{URL: u, Depth: itemData.Depth}, true
	}
}

// Len returns the approximate number of queued URLs.
// Note: In distributed mode, this reflects the global queue length.
func (f *DistributedFrontier) Len() int {
	length, err := f.client.LLen(f.ctx, f.queueKey).Result()
	if err != nil {
		return 0
	}
	return int(length)
}

// VisitedCount returns the total number of unique URLs visited globally.
func (f *DistributedFrontier) VisitedCount() uint32 {
	count, err := f.client.SCard(f.ctx, f.visitedKey).Result()
	if err != nil {
		return 0
	}
	return uint32(count)
}

// Close signals that this instance is done.
// Does NOT clear the Redis data (other instances may still be running).
func (f *DistributedFrontier) Close() {
	f.closed = true
}

// Clear removes all data for this crawl from Redis.
// WARNING: This affects all crawler instances using the same prefix.
// Only call this when you're sure no other instances are running.
func (f *DistributedFrontier) Clear() error {
	pipe := f.client.Pipeline()
	pipe.Del(f.ctx, f.queueKey)
	pipe.Del(f.ctx, f.visitedKey)
	_, err := pipe.Exec(f.ctx)
	return err
}

// Stats returns statistics about the distributed crawl.
func (f *DistributedFrontier) Stats() (queueLen int64, visitedCount int64, err error) {
	pipe := f.client.Pipeline()
	queueLenCmd := pipe.LLen(f.ctx, f.queueKey)
	visitedCountCmd := pipe.SCard(f.ctx, f.visitedKey)

	if _, err := pipe.Exec(f.ctx); err != nil {
		return 0, 0, err
	}

	return queueLenCmd.Val(), visitedCountCmd.Val(), nil
}
