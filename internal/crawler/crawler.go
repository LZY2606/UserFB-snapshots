// Package crawler ties together the frontier, fetcher, parser, robots checker,
// and storage into a concurrent crawl engine.
package crawler

import (
	"context"
	"fmt"
	"log/slog"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/0xfurai/goliath/internal/checkpoint"
	"github.com/0xfurai/goliath/internal/fetcher"
	"github.com/0xfurai/goliath/internal/frontier"
	"github.com/0xfurai/goliath/internal/parser"
	"github.com/0xfurai/goliath/internal/robots"
	"github.com/0xfurai/goliath/internal/storage"
)

// Config holds all tunable parameters for the crawler.
type Config struct {
	Seeds             []string
	Workers           int
	MaxDepth          int
	MaxPages          int64
	SameDomain        bool          // restrict crawl to seed domains
	MinDelay          time.Duration // politeness delay between requests to same host
	Timeout           time.Duration // per-request HTTP timeout
	MaxBodyMB         int
	UserAgent         string
	OutputFile        string
	RedisAddr         string        // Redis address for distributed mode (e.g., "localhost:6379"). Empty = local mode
	RedisPass         string        // Redis password
	RedisDB           int           // Redis database number
	RedisPrefix       string        // Redis key prefix for this crawl
	CheckpointFile    string        // Checkpoint file path for local mode
	CheckpointInterval time.Duration // How often to save checkpoints
}

// Stats are updated atomically during the crawl.
type Stats struct {
	Fetched  atomic.Int64
	Errors   atomic.Int64
	Skipped  atomic.Int64
	Enqueued atomic.Int64
}

// Crawler orchestrates the concurrent crawl.
type Crawler struct {
	cfg      Config
	frontier frontier.Queue // supports both local and distributed modes
	fetcher  *fetcher.Fetcher
	robots   *robots.Checker
	writer   *storage.Writer
	stats    Stats
	logger   *slog.Logger

	allowedHosts      map[string]struct{} // non-nil when SameDomain=true
	checkpointCancel  context.CancelFunc  // cancel periodic checkpointing
	localFrontier     *frontier.Frontier  // reference to local frontier for checkpointing (nil if distributed)
}

// New creates a Crawler from cfg. Call Run to start crawling.
func New(cfg Config, logger *slog.Logger) (*Crawler, error) {
	if cfg.UserAgent == "" {
		cfg.UserAgent = "goliath/1.0 (+https://github.com/0xfurai/goliath)"
	}
	if cfg.Timeout <= 0 {
		cfg.Timeout = 15 * time.Second
	}
	if cfg.Workers <= 0 {
		cfg.Workers = 10
	}
	if cfg.MinDelay <= 0 {
		cfg.MinDelay = 500 * time.Millisecond
	}

	f := fetcher.New(fetcher.Config{
		UserAgent: cfg.UserAgent,
		Timeout:   cfg.Timeout,
		MaxBodyMB: cfg.MaxBodyMB,
		MinDelay:  cfg.MinDelay,
	})

	w, err := storage.NewWriter(cfg.OutputFile)
	if err != nil {
		return nil, err
	}

	// Create frontier (local or distributed based on config)
	var fq frontier.Queue
	var localFrontier *frontier.Frontier
	if cfg.RedisAddr != "" {
		logger.Info("using distributed mode", "redis", cfg.RedisAddr, "prefix", cfg.RedisPrefix)
		if cfg.CheckpointFile != "" {
			logger.Warn("checkpoint file ignored in distributed mode (Redis already provides persistence)")
		}
		ctx := context.Background()
		distFrontier, err := frontier.NewDistributed(ctx, frontier.RedisConfig{
			Addr:     cfg.RedisAddr,
			Password: cfg.RedisPass,
			DB:       cfg.RedisDB,
			Prefix:   cfg.RedisPrefix,
		})
		if err != nil {
			return nil, fmt.Errorf("failed to connect to redis: %w", err)
		}
		fq = distFrontier
	} else {
		logger.Info("using local mode")
		localFrontier = frontier.New()
		fq = localFrontier
	}

	c := &Crawler{
		cfg:           cfg,
		frontier:      fq,
		fetcher:       f,
		robots:        robots.New(cfg.UserAgent, f.Client()),
		writer:        w,
		logger:        logger,
		localFrontier: localFrontier,
	}

	if cfg.SameDomain {
		c.allowedHosts = make(map[string]struct{})
		for _, seed := range cfg.Seeds {
			if u, err := url.Parse(seed); err == nil {
				c.allowedHosts[u.Hostname()] = struct{}{}
			}
		}
	}

	// Seed the frontier
	for _, seed := range cfg.Seeds {
		u, err := url.Parse(seed)
		if err != nil {
			logger.Warn("invalid seed URL", "url", seed, "err", err)
			continue
		}
		if c.frontier.Push(u, 0) {
			c.stats.Enqueued.Add(1)
		}
	}

	return c, nil
}

// Run starts the worker pool and blocks until the crawl completes or ctx is done.
func (c *Crawler) Run(ctx context.Context) Stats {
	defer c.writer.Close()
	defer c.StopCheckpointing()

	var wg sync.WaitGroup
	// Channel to drain workers when frontier is closed
	done := make(chan struct{})

	// Watchdog: close frontier when queue empties and all workers are idle
	idleWorkers := atomic.Int64{}
	idleWorkers.Store(0)

	for i := 0; i < c.cfg.Workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				select {
				case <-ctx.Done():
					return
				case <-done:
					return
				default:
				}

				idleWorkers.Add(1)
				item, ok := c.frontier.Pop()
				idleWorkers.Add(-1)

				if !ok {
					return // frontier closed
				}
				c.process(ctx, item)
			}
		}()
	}

	// Watchdog goroutine: closes frontier when all workers idle and queue empty
	go func() {
		for {
			select {
			case <-ctx.Done():
				c.frontier.Close()
				return
			default:
			}
			time.Sleep(200 * time.Millisecond)
			if c.frontier.Len() == 0 && idleWorkers.Load() == int64(c.cfg.Workers) {
				c.frontier.Close()
				return
			}
			if c.cfg.MaxPages > 0 && c.stats.Fetched.Load() >= c.cfg.MaxPages {
				c.frontier.Close()
				return
			}
		}
	}()

	wg.Wait()
	close(done)
	return c.stats
}

// process fetches one URL, parses it, and enqueues discovered links.
func (c *Crawler) process(ctx context.Context, item frontier.Item) {
	if c.cfg.MaxPages > 0 && c.stats.Fetched.Load() >= c.cfg.MaxPages {
		return
	}

	u := item.URL
	log := c.logger.With("url", u.String(), "depth", item.Depth)

	// Check robots.txt
	if !c.robots.Allowed(u) {
		log.Debug("blocked by robots.txt")
		c.stats.Skipped.Add(1)
		return
	}

	crawlDelay := c.robots.CrawlDelay(u)
	result := c.fetcher.Fetch(ctx, u, crawlDelay)

	rec := storage.Record{
		URL:       u.String(),
		FetchedAt: result.FetchedAt,
		Depth:     item.Depth,
	}

	if result.Error != nil {
		rec.Error = result.Error.Error()
		log.Warn("fetch error", "err", result.Error)
		c.stats.Errors.Add(1)
		_ = c.writer.Write(rec)
		return
	}

	rec.StatusCode = result.StatusCode
	rec.Headers = storage.HeadersFlat(map[string][]string(result.Headers))

	c.stats.Fetched.Add(1)
	log.Info("fetched", "status", result.StatusCode)

	// Only parse HTML
	ct := result.Headers.Get("Content-Type")
	if !strings.Contains(ct, "text/html") && !strings.Contains(ct, "application/xhtml") {
		_ = c.writer.Write(rec)
		return
	}

	page := parser.Extract(result.Body, u)
	rec.Title = page.Title
	rec.Content = page.Text
	rec.Links = storage.URLsToStrings(page.Links)

	_ = c.writer.Write(rec)

	// Enqueue discovered links
	if item.Depth >= c.cfg.MaxDepth {
		return
	}
	for _, link := range page.Links {
		if !c.isAllowed(link) {
			continue
		}
		if c.frontier.Push(link, item.Depth+1) {
			c.stats.Enqueued.Add(1)
		}
	}
}

func (c *Crawler) isAllowed(u *url.URL) bool {
	if c.allowedHosts == nil {
		return true
	}
	_, ok := c.allowedHosts[u.Hostname()]
	return ok
}

// PrintStats logs a summary of the crawl stats.
func (c *Crawler) PrintStats(logger *slog.Logger) {
	logger.Info("crawl complete",
		"fetched", c.stats.Fetched.Load(),
		"errors", c.stats.Errors.Load(),
		"skipped", c.stats.Skipped.Load(),
		"enqueued", c.stats.Enqueued.Load(),
		"visited_approx", c.frontier.VisitedCount(),
	)
}

// HTTPClient returns the underlying *http.Client for external use.
func HTTPClient(cfg Config) *http.Client {
	return &http.Client{Timeout: cfg.Timeout}
}

// SaveCheckpoint saves the current crawler state to a checkpoint file.
// Only works in local mode (not distributed/Redis mode).
func (c *Crawler) SaveCheckpoint(path string) error {
	if c.localFrontier == nil {
		return fmt.Errorf("checkpoints not supported in distributed mode")
	}

	// Create frontier snapshot
	frontierSnap, err := c.localFrontier.Save()
	if err != nil {
		return fmt.Errorf("save frontier: %w", err)
	}

	// Build checkpoint
	ckpt := &checkpoint.Checkpoint{
		Config: checkpoint.Config{
			Seeds:      c.cfg.Seeds,
			MaxDepth:   c.cfg.MaxDepth,
			SameDomain: c.cfg.SameDomain,
			UserAgent:  c.cfg.UserAgent,
		},
		Stats: checkpoint.Stats{
			Fetched:  c.stats.Fetched.Load(),
			Errors:   c.stats.Errors.Load(),
			Enqueued: c.stats.Enqueued.Load(),
			Skipped:  c.stats.Skipped.Load(),
		},
		Frontier: checkpoint.Frontier{
			VisitedBloomBase64: frontierSnap.VisitedBloomBase64,
			QueueLength:        len(frontierSnap.Queue),
			Queue:              make([]checkpoint.QueueItem, len(frontierSnap.Queue)),
		},
	}

	// Convert frontier queue items
	for i, item := range frontierSnap.Queue {
		ckpt.Frontier.Queue[i] = checkpoint.QueueItem{
			URL:   item.URL,
			Depth: item.Depth,
		}
	}

	// Save to disk
	if err := checkpoint.Save(path, ckpt); err != nil {
		return fmt.Errorf("save checkpoint: %w", err)
	}

	c.logger.Info("checkpoint saved",
		"path", path,
		"queue_length", ckpt.Frontier.QueueLength,
		"fetched", ckpt.Stats.Fetched,
	)

	return nil
}

// StartCheckpointing starts a goroutine that periodically saves checkpoints.
// Call this after creating the crawler but before calling Run.
func (c *Crawler) StartCheckpointing(ctx context.Context, interval time.Duration, path string) {
	if c.localFrontier == nil {
		c.logger.Warn("checkpointing not available in distributed mode")
		return
	}

	if interval <= 0 {
		c.logger.Warn("checkpoint interval must be > 0, checkpointing disabled")
		return
	}

	checkpointCtx, cancel := context.WithCancel(ctx)
	c.checkpointCancel = cancel

	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()

		for {
			select {
			case <-ticker.C:
				if err := c.SaveCheckpoint(path); err != nil {
					c.logger.Error("failed to save checkpoint", "err", err)
				}
			case <-checkpointCtx.Done():
				// Save final checkpoint on exit
				if err := c.SaveCheckpoint(path); err != nil {
					c.logger.Error("failed to save final checkpoint", "err", err)
				}
				return
			}
		}
	}()

	c.logger.Info("periodic checkpointing started", "interval", interval, "path", path)
}

// StopCheckpointing stops the periodic checkpointing goroutine.
func (c *Crawler) StopCheckpointing() {
	if c.checkpointCancel != nil {
		c.checkpointCancel()
		c.checkpointCancel = nil
	}
}

// NewFromCheckpoint creates a Crawler from a saved checkpoint.
func NewFromCheckpoint(ckptPath string, cfg Config, logger *slog.Logger) (*Crawler, error) {
	// Load checkpoint
	ckpt, err := checkpoint.Load(ckptPath)
	if err != nil {
		return nil, fmt.Errorf("load checkpoint: %w", err)
	}

	logger.Info("loaded checkpoint",
		"created_at", ckpt.CreatedAt,
		"queue_length", ckpt.Frontier.QueueLength,
		"fetched", ckpt.Stats.Fetched,
	)

	// Validate config compatibility
	savedCfg := checkpoint.Config{
		Seeds:      cfg.Seeds,
		MaxDepth:   cfg.MaxDepth,
		SameDomain: cfg.SameDomain,
		UserAgent:  cfg.UserAgent,
	}
	if err := checkpoint.ValidateConfig(savedCfg, ckpt.Config); err != nil {
		return nil, fmt.Errorf("config mismatch: %w", err)
	}

	// Set defaults
	if cfg.UserAgent == "" {
		cfg.UserAgent = "goliath/1.0 (+https://github.com/0xfurai/goliath)"
	}
	if cfg.Timeout <= 0 {
		cfg.Timeout = 15 * time.Second
	}
	if cfg.Workers <= 0 {
		cfg.Workers = 10
	}
	if cfg.MinDelay <= 0 {
		cfg.MinDelay = 500 * time.Millisecond
	}

	f := fetcher.New(fetcher.Config{
		UserAgent: cfg.UserAgent,
		Timeout:   cfg.Timeout,
		MaxBodyMB: cfg.MaxBodyMB,
		MinDelay:  cfg.MinDelay,
	})

	w, err := storage.NewWriter(cfg.OutputFile)
	if err != nil {
		return nil, err
	}

	// Distributed mode not supported for resume
	if cfg.RedisAddr != "" {
		return nil, fmt.Errorf("cannot resume from checkpoint in distributed mode")
	}

	// Create frontier and restore state
	localFrontier := frontier.New()

	// Convert checkpoint queue items to frontier snapshot
	frontierSnap := &frontier.Snapshot{
		VisitedBloomBase64: ckpt.Frontier.VisitedBloomBase64,
		Queue:              make([]frontier.SnapshotItem, len(ckpt.Frontier.Queue)),
	}
	for i, item := range ckpt.Frontier.Queue {
		frontierSnap.Queue[i] = frontier.SnapshotItem{
			URL:   item.URL,
			Depth: item.Depth,
		}
	}

	if err := localFrontier.Restore(frontierSnap); err != nil {
		return nil, fmt.Errorf("restore frontier: %w", err)
	}

	c := &Crawler{
		cfg:           cfg,
		frontier:      localFrontier,
		fetcher:       f,
		robots:        robots.New(cfg.UserAgent, f.Client()),
		writer:        w,
		logger:        logger,
		localFrontier: localFrontier,
	}

	// Restore stats
	c.stats.Fetched.Store(ckpt.Stats.Fetched)
	c.stats.Errors.Store(ckpt.Stats.Errors)
	c.stats.Enqueued.Store(ckpt.Stats.Enqueued)
	c.stats.Skipped.Store(ckpt.Stats.Skipped)

	// Restore allowed hosts
	if cfg.SameDomain {
		c.allowedHosts = make(map[string]struct{})
		for _, seed := range ckpt.Config.Seeds {
			if u, err := url.Parse(seed); err == nil {
				c.allowedHosts[u.Hostname()] = struct{}{}
			}
		}
	}

	logger.Info("crawler restored from checkpoint", "queue", localFrontier.Len())

	return c, nil
}
