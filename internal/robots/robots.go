// Package robots provides robots.txt parsing and compliance checking.
package robots

import (
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

// Checker caches and checks robots.txt rules per host.
type Checker struct {
	mu        sync.RWMutex
	cache     map[string]*rules
	userAgent string
	client    *http.Client
}

type rules struct {
	disallowed []string
	crawlDelay time.Duration
}

// New returns a Checker for the given user-agent string.
func New(userAgent string, client *http.Client) *Checker {
	return &Checker{
		cache:     make(map[string]*rules),
		userAgent: userAgent,
		client:    client,
	}
}

// Allowed returns true if the given URL is allowed to be crawled.
func (c *Checker) Allowed(u *url.URL) bool {
	r := c.getRules(u)
	if r == nil {
		return true
	}
	path := u.RequestURI()
	for _, prefix := range r.disallowed {
		if strings.HasPrefix(path, prefix) {
			return false
		}
	}
	return true
}

// CrawlDelay returns the crawl-delay directive for the host, or 0 if not set.
func (c *Checker) CrawlDelay(u *url.URL) time.Duration {
	r := c.getRules(u)
	if r == nil {
		return 0
	}
	return r.crawlDelay
}

func (c *Checker) getRules(u *url.URL) *rules {
	host := u.Scheme + "://" + u.Host

	c.mu.RLock()
	r, ok := c.cache[host]
	c.mu.RUnlock()
	if ok {
		return r
	}

	r = c.fetch(host)
	c.mu.Lock()
	c.cache[host] = r
	c.mu.Unlock()
	return r
}

func (c *Checker) fetch(hostBase string) *rules {
	robotsURL := fmt.Sprintf("%s/robots.txt", hostBase)
	resp, err := c.client.Get(robotsURL)
	if err != nil || resp.StatusCode != 200 {
		return &rules{} // allow everything if unreachable
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<16)) // 64 KB limit
	if err != nil {
		return &rules{}
	}

	return parse(string(body), c.userAgent)
}

// parse is a minimal robots.txt parser.
func parse(content, userAgent string) *rules {
	r := &rules{}
	ua := strings.ToLower(userAgent)

	lines := strings.Split(content, "\n")
	applicable := false

	for _, line := range lines {
		line = strings.TrimSpace(line)
		if strings.HasPrefix(line, "#") || line == "" {
			continue
		}

		parts := strings.SplitN(line, ":", 2)
		if len(parts) != 2 {
			continue
		}
		field := strings.ToLower(strings.TrimSpace(parts[0]))
		value := strings.TrimSpace(parts[1])
		// strip inline comments
		if idx := strings.Index(value, "#"); idx != -1 {
			value = strings.TrimSpace(value[:idx])
		}

		switch field {
		case "user-agent":
			applicable = value == "*" || strings.ToLower(value) == ua
		case "disallow":
			if applicable && value != "" {
				r.disallowed = append(r.disallowed, value)
			}
		case "crawl-delay":
			if applicable {
				var secs float64
				fmt.Sscanf(value, "%f", &secs)
				r.crawlDelay = time.Duration(secs * float64(time.Second))
			}
		}
	}
	return r
}
