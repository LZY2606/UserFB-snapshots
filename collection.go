package antch

import (
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"sync"
)

const (
	collectionNextHeader  = "X-Crawl-Next"
	collectionDeltaHeader = "X-Crawl-Delta"
)

// CollectionState is the resumable cursor of an interface catalog collection.
//
// SourceURL is the absolute entry URL the collection belongs to. NextURL, when
// non-empty, points at the next page of the collection that has not completed
// yet. DeltaURL is the stable entry URL of the next incremental collection.
type CollectionState struct {
	SourceURL string
	NextURL   string
	DeltaURL  string
}

// CollectionOptions configures a Collect call.
//
// URL is the absolute HTTP(S) entry URL. Full starts a collection at URL.
// State is either the zero value or a state whose SourceURL is equal to URL.
// Save is called with the new state after a page and all of its items have
// completed; collection stops if Save returns an error.
type CollectionOptions struct {
	URL   string
	Full  bool
	State CollectionState
	Save  func(CollectionState) error
}

// Collect crawls one collection page at a time and persists the cursor through
// options.Save. A page is complete only after its registered Handler has
// returned and every Item that Handler sent during the call has returned from
// the Crawler's Pipeline. The next page is requested only after Save succeeds.
//
// Collect returns the last successfully saved state. Transport, protocol,
// Handler/Pipeline panic, and Save errors stop the collection; the returned
// error can be inspected with errors.Is when Save was the failing step.
func (c *Crawler) Collect(options CollectionOptions) (CollectionState, error) {
	state := options.State

	if options.Save == nil {
		return state, errors.New("crawler: collection requires a Save function")
	}
	entry, err := parseCollectionURL(options.URL)
	if err != nil {
		return state, fmt.Errorf("crawler: collection URL: %w", err)
	}
	if state != (CollectionState{}) && state.SourceURL != options.URL {
		return state, fmt.Errorf("crawler: collection state source URL %q does not match entry URL %q",
			state.SourceURL, options.URL)
	}

	pageURL := entry.String()
	if !options.Full {
		switch {
		case state.NextURL != "":
			pageURL = state.NextURL
		case state.DeltaURL != "":
			pageURL = state.DeltaURL
		}
	}

	visited := make(map[string]struct{})
	for {
		page, err := parseCollectionURL(pageURL)
		if err != nil {
			return state, fmt.Errorf("crawler: collection page URL: %w", err)
		}
		visited[collectionURLKey(page)] = struct{}{}

		req, err := http.NewRequest(http.MethodGet, pageURL, nil)
		if err != nil {
			return state, fmt.Errorf("crawler: collection request: %w", err)
		}
		res, err := c.doCollectionRequest(req)
		if err != nil {
			return state, err
		}
		baseURL := page
		if res.Request != nil && res.Request.URL != nil {
			baseURL = res.Request.URL
			visited[collectionURLKey(baseURL)] = struct{}{}
		}

		if res.StatusCode != http.StatusOK {
			status := res.StatusCode
			closeResponse(res)
			return state, fmt.Errorf("crawler: collection page %s returned HTTP %d", pageURL, status)
		}

		nextRaw := res.Header.Get(collectionNextHeader)
		deltaRaw := res.Header.Get(collectionDeltaHeader)
		if (nextRaw != "") == (deltaRaw != "") {
			closeResponse(res)
			return state, fmt.Errorf(
				"crawler: collection page %s must provide exactly one of %s or %s",
				pageURL, collectionNextHeader, collectionDeltaHeader)
		}

		var nextURL, deltaURL string
		if nextRaw != "" {
			next, err := resolveCollectionURL(baseURL, nextRaw)
			if err != nil {
				closeResponse(res)
				return state, fmt.Errorf("crawler: collection next URL: %w", err)
			}
			if _, ok := visited[collectionURLKey(next)]; ok {
				closeResponse(res)
				return state, fmt.Errorf("crawler: collection continuation cycle at %s", next)
			}
			nextURL = next.String()
		} else {
			delta, err := resolveCollectionURL(baseURL, deltaRaw)
			if err != nil {
				closeResponse(res)
				return state, fmt.Errorf("crawler: collection delta URL: %w", err)
			}
			deltaURL = delta.String()
		}

		// The page is not complete until the Handler has returned and every
		// Item it sent has come back from the original Pipeline chain.
		if err := c.serveCollectionPage(res); err != nil {
			closeResponse(res)
			return state, err
		}
		closeResponse(res)

		nextState := state
		nextState.SourceURL = options.URL
		if nextRaw != "" {
			// An intermediate page keeps the previous stable delta entry.
			nextState.NextURL = nextURL
		} else {
			// The final page of this run hands over the next delta entry.
			nextState.NextURL = ""
			nextState.DeltaURL = deltaURL
		}

		if err := options.Save(nextState); err != nil {
			return state, fmt.Errorf("crawler: collection save: %w", err)
		}
		state = nextState
		if nextRaw == "" {
			return state, nil
		}
		pageURL = nextURL
	}
}

// doCollectionRequest sends a request through the Crawler's existing spider,
// which applies the site scheduling and uses the shared HTTP client (middleware
// and redirect policy included).
func (c *Crawler) doCollectionRequest(req *http.Request) (*http.Response, error) {
	c.once.Do(c.init)
	if req.Header.Get("User-Agent") == "" && c.UserAgent != "" {
		req.Header.Set("User-Agent", c.UserAgent)
	}

	resc := make(chan responseAndError, 1)
	s := c.getSpider(req.URL)
	select {
	case s.reqch <- requestAndChan{req: req, ch: resc}:
	case <-s.done:
		// The spider retired between lookup and hand-off; retry on a fresh one.
		return c.doCollectionRequest(req)
	case <-c.Exit:
		return nil, errors.New("crawler: collection stopped before request")
	}

	select {
	case re := <-resc:
		closeRequest(req)
		if re.err != nil {
			if re.res != nil {
				closeResponse(re.res)
			}
			return nil, fmt.Errorf("crawler: collection request %s: %w", req.URL, re.err)
		}
		return re.res, nil
	case <-c.Exit:
		closeRequest(req)
		return nil, errors.New("crawler: collection stopped while waiting for response")
	}
}

// serveCollectionPage invokes the Crawler's registered Handler for res and
// waits until the Handler returns and all Items it sent have returned from the
// Pipeline. Items are handed to the original Pipeline chain directly, using the
// Crawler's configured item concurrency. Panics are reported as errors so the
// collection can stop.
func (c *Crawler) serveCollectionPage(res *http.Response) error {
	items := make(chan Item)
	var (
		wg        sync.WaitGroup
		mu        sync.Mutex
		panicked  interface{}
		stopItems bool
	)

	workers := c.maxConcurrentItems()
	wg.Add(workers)
	for i := 0; i < workers; i++ {
		go func() {
			defer wg.Done()
			for item := range items {
				mu.Lock()
				skip := stopItems
				mu.Unlock()
				if skip {
					// Keep draining so a Handler blocked on a send can return.
					continue
				}
				func() {
					defer func() {
						if r := recover(); r != nil {
							mu.Lock()
							if panicked == nil {
								panicked = r
								stopItems = true
							}
							mu.Unlock()
						}
					}()
					c.pipeHandler.ServePipeline(item)
				}()
			}
		}()
	}

	var handlerPanic interface{}
	func() {
		defer func() {
			handlerPanic = recover()
		}()
		h, _ := c.Handler(res)
		h.ServeSpider(items, res)
	}()
	close(items)
	wg.Wait()

	mu.Lock()
	pipelinePanic := panicked
	mu.Unlock()

	if handlerPanic != nil {
		return fmt.Errorf("crawler: collection handler panic: %v", handlerPanic)
	}
	if pipelinePanic != nil {
		return fmt.Errorf("crawler: collection pipeline panic: %v", pipelinePanic)
	}
	return nil
}

func parseCollectionURL(raw string) (*url.URL, error) {
	if raw == "" {
		return nil, errors.New("URL is empty")
	}
	u, err := url.Parse(raw)
	if err != nil {
		return nil, err
	}
	if !isCollectionHTTPURL(u) {
		return nil, fmt.Errorf("%q is not an absolute HTTP(S) URL", raw)
	}
	return u, nil
}

func resolveCollectionURL(base *url.URL, raw string) (*url.URL, error) {
	ref, err := url.Parse(raw)
	if err != nil {
		return nil, err
	}
	u := base.ResolveReference(ref)
	if !isCollectionHTTPURL(u) {
		return nil, fmt.Errorf("%q does not resolve to an absolute HTTP(S) URL", raw)
	}
	return u, nil
}

func isCollectionHTTPURL(u *url.URL) bool {
	if u == nil || u.Host == "" {
		return false
	}
	return strings.EqualFold(u.Scheme, "http") || strings.EqualFold(u.Scheme, "https")
}

// collectionURLKey identifies a URL for continuation-cycle detection. A
// fragment does not change which page is fetched, so it is ignored.
func collectionURLKey(u *url.URL) string {
	key := *u
	key.Fragment = ""
	return key.String()
}
