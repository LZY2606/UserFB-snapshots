# Goliath 🕷️

[![Go Version](https://img.shields.io/badge/Go-1.24%2B-00ADD8?logo=go)](https://go.dev/)
[![License](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Test](https://github.com/0xfurai/goliath/workflows/Test/badge.svg)](https://github.com/0xfurai/goliath/actions)

A powerful, production-ready web crawler written in Go. Built for scale with distributed crawling, checkpoint/resume, and memory-efficient bloom filter deduplication.

> **Goliath** scales from single machines to distributed clusters - crawl millions of pages with fixed memory usage.

## ✨ Key Features

- 🚀 **Distributed Crawling**: Scale horizontally with Redis-backed coordination
- 💾 **Checkpoint/Resume**: Save progress and resume long-running crawls
- 🧠 **Memory Efficient**: Fixed ~12MB bloom filter regardless of crawl size
- ⚡ **High Performance**: Concurrent workers with configurable parallelism
- 🤖 **Polite**: Respects `robots.txt` and enforces politeness delays
- 📊 **JSONL Output**: Structured output for easy processing
- 🎯 **Flexible**: Domain filtering, depth control, and fine-grained configuration

## Installation

```bash
go install github.com/0xfurai/goliath/cmd/crawler@latest
```

Or build from source:

```bash
git clone https://github.com/0xfurai/goliath.git
cd goliath
go build -o goliath ./cmd/crawler
```

## Quick Start

Basic crawl of a single domain:

```bash
./goliath -seeds "https://example.com"
```

Crawl multiple seeds with custom settings:

```bash
./goliath \
  -seeds "https://example.com,https://another.com" \
  -workers 20 \
  -depth 5 \
  -max-pages 10000 \
  -output results.jsonl
```

## Configuration

| Flag | Default | Description |
|------|---------|-------------|
| `-seeds` | *required* | Comma-separated list of seed URLs |
| `-workers` | `10` | Number of concurrent fetcher workers |
| `-depth` | `3` | Maximum crawl depth (0 = seeds only) |
| `-max-pages` | `0` | Maximum pages to fetch (0 = unlimited) |
| `-same-domain` | `true` | Only follow links on the same domain as seeds |
| `-delay` | `500ms` | Minimum politeness delay between requests to same host |
| `-timeout` | `15s` | HTTP request timeout |
| `-max-body-mb` | `10` | Maximum response body size in MB |
| `-ua` | `goliath/1.0 ...` | User-Agent string |
| `-output` | `crawl.jsonl` | Output file path (JSONL format) |
| `-v` | `false` | Verbose logging (debug level) |
| `-redis` | `""` | Redis address for distributed mode (e.g., `localhost:6379`) |
| `-redis-pass` | `""` | Redis password |
| `-redis-db` | `0` | Redis database number |
| `-redis-prefix` | auto | Redis key prefix (auto-generated from first seed domain) |
| `-checkpoint` | `""` | Checkpoint file path (local mode only) |
| `-checkpoint-interval` | `5m` | How often to save checkpoints |
| `-resume` | `false` | Resume from checkpoint file |

## Checkpoint and Resume

Save crawl progress and resume later (local mode only).

### Quick Start

**Start with checkpointing:**
```bash
./goliath \
  -seeds "https://example.com" \
  -checkpoint crawl.ckpt.json \
  -checkpoint-interval 10m
```

The crawler will save its state every 10 minutes. Press Ctrl+C to stop anytime.

**Resume from checkpoint:**
```bash
./goliath -resume -checkpoint crawl.ckpt.json
```

The crawler continues from where it left off, preserving:
- ✅ Queue of pending URLs
- ✅ Visited set (bloom filter)
- ✅ Statistics (pages fetched, errors, etc.)

### How It Works

**Checkpoint file** contains:
- Frontier state (queue + bloom filter)
- Statistics (fetched, errors, enqueued)
- Configuration (seeds, depth, same-domain)

**Automatic saves:**
- Periodic (every `-checkpoint-interval`)
- On graceful shutdown (Ctrl+C)

**Resume validation:**
- Checks config compatibility (max-depth, same-domain)
- Warns about mismatches
- Seeds can differ (you can add more on resume)

### Example: Long-Running Crawl

```bash
# Day 1: Crawl 10K pages, checkpoint every hour
./goliath \
  -seeds "https://large-site.com" \
  -max-pages 10000 \
  -checkpoint crawl.ckpt.json \
  -checkpoint-interval 1h

# Stop for the night (Ctrl+C)
# Checkpoint saved automatically

# Day 2: Resume and continue
./goliath \
  -resume \
  -checkpoint crawl.ckpt.json \
  -max-pages 20000  # Increase limit
```

### Checkpoint File Example

```json
{
  "version": "1.0",
  "created_at": "2026-02-21T10:30:00Z",
  "config": {
    "seeds": ["https://example.com"],
    "max_depth": 3,
    "same_domain": true
  },
  "stats": {
    "fetched": 5420,
    "errors": 103,
    "enqueued": 12500
  },
  "frontier": {
    "visited_bloom_base64": "...",
    "queue_length": 7080,
    "queue": [...]
  }
}
```

### Limitations

- **Local mode only**: Checkpoints don't work with `-redis` (distributed mode already persists to Redis)
- **Bloom filter constraints**: Very long crawls may exceed bloom filter capacity (10M URLs default)
- **File size**: Checkpoint grows with queue size (~100 bytes per URL)

### Performance

| Queue Size | Checkpoint Size | Save Time |
|------------|-----------------|-----------|
| 1K URLs    | ~200 KB        | <10ms     |
| 10K URLs   | ~2 MB          | ~50ms     |
| 100K URLs  | ~20 MB         | ~500ms    |

Recommended checkpoint interval: 5-10 minutes for typical crawls.

## Distributed Crawling

Run the crawler across **multiple machines** by using Redis as a shared queue and visited set.

### Setup

1. **Start Redis** using Docker Compose (recommended):
   ```bash
   docker-compose up -d
   ```

   Or manually with Docker:
   ```bash
   docker run -d -p 6379:6379 redis:latest
   ```

   Or use a managed Redis service (Redis Cloud, AWS ElastiCache, etc.)

2. **Run crawlers on multiple machines** pointing to the same Redis instance:

   **Machine 1:**
   ```bash
   ./goliath -seeds "https://example.com" -redis "redis-host:6379" -workers 20
   ```

   **Machine 2:**
   ```bash
   ./goliath -seeds "https://example.com" -redis "redis-host:6379" -workers 20
   ```

   **Machine 3:**
   ```bash
   ./goliath -seeds "https://example.com" -redis "redis-host:6379" -workers 20
   ```

All machines will:
- Share the same URL queue (no duplicate fetches)
- Share the same visited set (global deduplication)
- Work on different URLs concurrently
- Write to their own local output files

### How It Works

- **Queue**: Redis LIST (FIFO) for pending URLs
- **Visited**: Redis SET for exact deduplication (no bloom filter false positives in distributed mode)
- **Atomicity**: All operations use Redis atomic commands (SADD, LPOP, etc.)

### Advantages over Local Mode

| Feature | Local Mode | Distributed Mode |
|---------|------------|------------------|
| **Machines** | Single machine | Unlimited machines |
| **Visited Set** | Bloom filter (~12MB, 1% false positives) | Redis SET (exact, but uses more memory) |
| **Queue** | In-memory | Redis (persistent) |
| **Scalability** | Limited by single machine resources | Horizontal scaling |
| **Crash Recovery** | Lost state | Persistent in Redis |

### When to Use Distributed Mode

- Crawling **large sites** (millions of pages)
- Need to **scale horizontally** (more machines = faster crawl)
- Want **crash recovery** (Redis persists state)
- Running **long crawls** where you might need to restart machines

### Redis Memory Usage

Distributed mode uses Redis SETs for exact deduplication instead of bloom filters:

- ~100 bytes per URL in Redis
- 1M URLs = ~100MB Redis memory
- 10M URLs = ~1GB Redis memory
- 100M URLs = ~10GB Redis memory

For very large crawls (100M+ URLs), consider using Redis with persistence or Redis Cluster for high availability.

### Example: Distributed Crawl with Authentication

```bash
# With Redis password
./goliath \
  -seeds "https://example.com" \
  -redis "redis.example.com:6379" \
  -redis-pass "your-password" \
  -redis-prefix "crawl:myproject:" \
  -workers 50
```

## Output Format

Results are written to a JSONL (JSON Lines) file where each line is a JSON object:

```json
{
  "url": "https://example.com/page",
  "status_code": 200,
  "title": "Example Page",
  "content": "Extracted text content...",
  "links": ["https://example.com/other", "https://example.com/another"],
  "headers": {"content-type": "text/html; charset=utf-8"},
  "fetched_at": "2026-02-21T10:30:00Z",
  "depth": 1,
  "error": ""
}
```

Failed fetches include an `error` field with the error message.

## Architecture

The crawler consists of several modular components:

- **Frontier**: Thread-safe URL queue with bloom filter deduplication
- **Fetcher**: HTTP client with per-host politeness delays
- **Parser**: Extracts links and text content from HTML
- **Robots Checker**: Validates URLs against robots.txt rules
- **Storage**: Writes crawl results to JSONL files

### Deduplication Strategy

The crawler uses a **bloom filter** for URL deduplication instead of a traditional map:

- **Memory**: Fixed ~12MB for 10M URLs (vs. potentially gigabytes with a map)
- **Performance**: O(1) lookups and inserts
- **Trade-off**: ~1% false positive rate (some URLs may be incorrectly marked as visited)

This design enables **long-running crawls** without memory growth from URL tracking.

## Example Use Cases

**Crawl a blog with unlimited pages:**
```bash
./goliath -seeds "https://blog.example.com" -max-pages 0 -depth 10
```

**Fast shallow crawl of multiple sites:**
```bash
./goliath -seeds "https://site1.com,https://site2.com" -depth 1 -workers 50
```

**Respectful deep crawl with longer delays:**
```bash
./goliath -seeds "https://example.com" -depth 10 -delay 2s -workers 5
```

**Cross-domain crawl (follow external links):**
```bash
./goliath -seeds "https://example.com" -same-domain=false -max-pages 5000
```

## Performance

On a typical machine with good network connectivity:

- **Speed**: 100-500+ pages/minute (depends on workers, delays, and target sites)
- **Memory**: ~50-100MB baseline + ~12MB for bloom filter (largely independent of crawl size)
- **Disk**: ~10-50KB per page (varies with content size)

## Stopping a Crawl

The crawler gracefully handles interruption:

```bash
# Press Ctrl+C to stop
# Or send SIGTERM: kill <pid>
```

All fetched pages are written to the output file before shutdown.

## License

MIT License - see LICENSE file for details

## Contributing

Contributions welcome! Please open an issue or PR.

## Roadmap

- [x] Distributed crawling support (Redis-based)
- [x] Resume from checkpoint ([design doc](CHECKPOINT_DESIGN.md))
- [ ] Configurable bloom filter parameters via CLI
- [ ] Metrics/monitoring endpoint
- [ ] Custom link extractors (JS rendering, PDFs, etc.)
- [ ] Redis Cluster support for ultra-large crawls

## Documentation

- [README.md](README.md) - Main documentation (you are here)
- [DISTRIBUTED.md](DISTRIBUTED.md) - Distributed crawling guide
- [CHECKPOINT_DESIGN.md](CHECKPOINT_DESIGN.md) - Checkpoint/resume design
- [LICENSE](LICENSE) - MIT License
