package main

import (
	"context"
	"flag"
	"fmt"
	"log/slog"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/0xfurai/goliath/internal/crawler"
)

func main() {
	seeds := flag.String("seeds", "", "Comma-separated seed URLs (required)")
	workers := flag.Int("workers", 10, "Number of concurrent workers")
	maxDepth := flag.Int("depth", 3, "Maximum crawl depth (0 = seed only)")
	maxPages := flag.Int64("max-pages", 0, "Maximum pages to fetch (0 = unlimited)")
	sameDomain := flag.Bool("same-domain", true, "Only follow links on the same domain as seeds")
	minDelay := flag.Duration("delay", 500*time.Millisecond, "Minimum politeness delay between requests to the same host")
	timeout := flag.Duration("timeout", 15*time.Second, "HTTP request timeout")
	maxBody := flag.Int("max-body-mb", 10, "Maximum response body size in MB")
	userAgent := flag.String("ua", "goliath/1.0 (+https://github.com/0xfurai/goliath)", "User-Agent string")
	output := flag.String("output", "crawl.jsonl", "Output file path (JSONL format)")
	verbose := flag.Bool("v", false, "Verbose logging (debug level)")

	// Distributed mode (Redis)
	redisAddr := flag.String("redis", "", "Redis address for distributed mode (e.g., localhost:6379). Empty = local mode")
	redisPass := flag.String("redis-pass", "", "Redis password")
	redisDB := flag.Int("redis-db", 0, "Redis database number")
	redisPrefix := flag.String("redis-prefix", "", "Redis key prefix (default: crawl:<first-seed-domain>:)")

	// Checkpoint/Resume (Local mode only)
	checkpointFile := flag.String("checkpoint", "", "Checkpoint file path (local mode only)")
	checkpointInterval := flag.Duration("checkpoint-interval", 5*time.Minute, "How often to save checkpoints")
	resume := flag.Bool("resume", false, "Resume from checkpoint file")

	flag.Parse()

	if *seeds == "" {
		fmt.Fprintln(os.Stderr, "error: -seeds is required")
		flag.Usage()
		os.Exit(1)
	}

	level := slog.LevelInfo
	if *verbose {
		level = slog.LevelDebug
	}
	logger := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))

	seedList := splitTrim(*seeds, ",")

	// Auto-generate Redis prefix from first seed if not specified
	prefix := *redisPrefix
	if *redisAddr != "" && prefix == "" && len(seedList) > 0 {
		if u, err := url.Parse(seedList[0]); err == nil {
			prefix = "crawl:" + u.Hostname() + ":"
		}
	}

	cfg := crawler.Config{
		Seeds:              seedList,
		Workers:            *workers,
		MaxDepth:           *maxDepth,
		MaxPages:           *maxPages,
		SameDomain:         *sameDomain,
		MinDelay:           *minDelay,
		Timeout:            *timeout,
		MaxBodyMB:          *maxBody,
		UserAgent:          *userAgent,
		OutputFile:         *output,
		RedisAddr:          *redisAddr,
		RedisPass:          *redisPass,
		RedisDB:            *redisDB,
		RedisPrefix:        prefix,
		CheckpointFile:     *checkpointFile,
		CheckpointInterval: *checkpointInterval,
	}

	// Validation
	if *resume && *checkpointFile == "" {
		fmt.Fprintln(os.Stderr, "error: -resume requires -checkpoint")
		os.Exit(1)
	}

	if *resume && *redisAddr != "" {
		fmt.Fprintln(os.Stderr, "error: -resume not supported with -redis (distributed mode already persists state)")
		os.Exit(1)
	}

	// Create or restore crawler
	var c *crawler.Crawler
	var err error

	if *resume {
		logger.Info("resuming from checkpoint", "file", *checkpointFile)
		c, err = crawler.NewFromCheckpoint(*checkpointFile, cfg, logger)
		if err != nil {
			logger.Error("failed to resume from checkpoint", "err", err)
			os.Exit(1)
		}
	} else {
		c, err = crawler.New(cfg, logger)
		if err != nil {
			logger.Error("failed to create crawler", "err", err)
			os.Exit(1)
		}
	}

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	// Start periodic checkpointing if configured
	if *checkpointFile != "" && !*resume {
		c.StartCheckpointing(ctx, *checkpointInterval, *checkpointFile)
	} else if *checkpointFile != "" && *resume {
		// Resume mode - start checkpointing to continue saving
		c.StartCheckpointing(ctx, *checkpointInterval, *checkpointFile)
	}

	logger.Info("crawler starting",
		"seeds", seedList,
		"workers", *workers,
		"max_depth", *maxDepth,
		"same_domain", *sameDomain,
		"output", *output,
		"checkpoint", *checkpointFile,
		"resume", *resume,
	)

	stats := c.Run(ctx)
	c.PrintStats(logger)

	// Run has fully drained every worker and closed the output file before
	// returning. A non-nil Err means the JSONL sink failed: the original
	// write/close error is reachable via errors.Is, and the file contains
	// only complete lines. Surface it so downstream ingestion does not
	// silently consume a truncated run.
	if err := c.Err(); err != nil {
		logger.Error("crawl stopped with an output failure",
			"err", err,
			"fetched", stats.Fetched,
			"errors", stats.Errors,
		)
		os.Exit(1)
	}
}

func splitTrim(s, sep string) []string {
	parts := strings.Split(s, sep)
	out := make([]string, 0, len(parts))
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}
