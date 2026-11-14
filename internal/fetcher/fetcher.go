// Package fetcher performs HTTP requests with rate limiting and retries.
package fetcher

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"sync"
	"time"
)

// Result holds the outcome of fetching a URL.
type Result struct {
	URL        *url.URL
	StatusCode int
	Body       []byte
	Headers    http.Header
	FetchedAt  time.Time
	Error      error
}

// Fetcher performs HTTP GETs with per-host rate limiting.
type Fetcher struct {
	client    *http.Client
	userAgent string
	maxBody   int64

	mu       sync.Mutex
	hostLast map[string]time.Time // last fetch time per host
	minDelay time.Duration        // minimum politeness delay
}

// Config configures the Fetcher.
type Config struct {
	UserAgent  string
	Timeout    time.Duration
	MaxBodyMB  int
	MinDelay   time.Duration // minimum delay between requests to same host
	MaxRetries int
}

// New creates a Fetcher with the provided config.
func New(cfg Config) *Fetcher {
	transport := &http.Transport{
		MaxIdleConns:        200,
		MaxIdleConnsPerHost: 10,
		IdleConnTimeout:     90 * time.Second,
		DisableCompression:  false,
	}
	client := &http.Client{
		Timeout:   cfg.Timeout,
		Transport: transport,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			if len(via) >= 5 {
				return fmt.Errorf("too many redirects")
			}
			return nil
		},
	}
	if cfg.MaxBodyMB <= 0 {
		cfg.MaxBodyMB = 10
	}
	return &Fetcher{
		client:    client,
		userAgent: cfg.UserAgent,
		maxBody:   int64(cfg.MaxBodyMB) << 20,
		hostLast:  make(map[string]time.Time),
		minDelay:  cfg.MinDelay,
	}
}

// Client returns the underlying http.Client (e.g. for robots.txt fetching).
func (f *Fetcher) Client() *http.Client {
	return f.client
}

// Fetch retrieves the given URL, respecting the per-host politeness delay.
// extraDelay is added on top of minDelay (e.g. from robots.txt crawl-delay).
// Both the delay wait and the HTTP request honor ctx, so a canceled or
// failed crawl stops without sleeping through the whole delay.
func (f *Fetcher) Fetch(ctx context.Context, u *url.URL, extraDelay time.Duration) Result {
	if err := f.Wait(ctx, u.Host, extraDelay); err != nil {
		return Result{URL: u, Error: err, FetchedAt: time.Now()}
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u.String(), nil)
	if err != nil {
		return Result{URL: u, Error: err}
	}
	req.Header.Set("User-Agent", f.userAgent)
	req.Header.Set("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8")
	req.Header.Set("Accept-Language", "en-US,en;q=0.5")

	resp, err := f.client.Do(req)
	if err != nil {
		return Result{URL: u, Error: err, FetchedAt: time.Now()}
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, f.maxBody))
	return Result{
		URL:        u,
		StatusCode: resp.StatusCode,
		Body:       body,
		Headers:    resp.Header,
		FetchedAt:  time.Now(),
		Error:      err,
	}
}

// Wait blocks until the per-host politeness delay has elapsed or ctx is
// done. It returns ctx.Err() when interrupted before the delay is over,
// otherwise nil.
func (f *Fetcher) Wait(ctx context.Context, host string, extra time.Duration) error {
	if err := ctx.Err(); err != nil {
		return err
	}

	delay := f.minDelay
	if extra > delay {
		delay = extra
	}

	f.mu.Lock()
	last, ok := f.hostLast[host]
	now := time.Now()
	f.hostLast[host] = now
	f.mu.Unlock()

	if !ok {
		return nil
	}
	wait := delay - now.Sub(last)
	if wait <= 0 {
		return nil
	}

	timer := time.NewTimer(wait)
	defer timer.Stop()
	select {
	case <-timer.C:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}
