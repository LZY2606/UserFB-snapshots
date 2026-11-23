package grawler

import (
	"io"
	"net/url"
	"strings"
	"sync"
)

type request struct {
	Fetcher
	OriginURL string
	URL       string
}

// result captures all of the state after downloading request.URL.
type result struct {
	request
	error  error
	links  []string
	assets []string
}

func resolveLink(base *url.URL, link string) (string, error) {
	url, err := url.Parse(link)

	if err != nil {
		return "", err
	}

	return strings.Split(base.ResolveReference(url).String(), "#")[0], nil
}

func dedupeAssets(assets []string) []string {
	seen := map[string]bool{}
	result := []string{}

	for _, asset := range assets {
		if !seen[asset] {
			seen[asset] = true
			result = append(result, asset)
		}
	}

	return result
}

func dedupeAndFixLinks(baseURL *url.URL, links []string) []string {
	result := []string{}
	seen := map[string]bool{}

	for _, link := range links {
		var err error
		link, err = resolveLink(baseURL, link)
		if err != nil {
			continue
		}
		if !seen[link] {
			seen[link] = true
			result = append(result, link)
		}
	}

	return result
}

// errorRecordingReader remembers the first non-EOF read error so that
// a body which fails halfway through parsing cannot silently yield a
// truncated document.
type errorRecordingReader struct {
	r   io.Reader
	err error
}

func (e *errorRecordingReader) Read(p []byte) (int, error) {
	n, err := e.r.Read(p)
	if err != nil && err != io.EOF && e.err == nil {
		e.err = err
	}
	return n, err
}

// parseDocument is the shared HTML production pipeline: it tokenizes
// the body, extracts links/assets and resolves links against baseURL.
// A failure while reading the body is returned as an error and no
// links or assets are exposed for it.
func parseDocument(body io.Reader, baseURL *url.URL) (links []string, assets []string, err error) {
	rec := &errorRecordingReader{r: body}
	rawLinks, rawAssets := findLinksAndAssets(rec)

	if rec.err != nil {
		return nil, nil, rec.err
	}

	return dedupeAndFixLinks(baseURL, rawLinks), dedupeAssets(rawAssets), nil
}

func fetch(req request) *result {
	result := &result{request: req}
	body, err := req.Fetcher.Fetch(req.URL)

	if err != nil {
		result.error = err
		return result
	}

	defer body.Close()
	pageURL, err := url.Parse(req.URL)

	if err != nil {
		result.error = err
		return result
	}

	links, assets, err := parseDocument(body, pageURL)
	if err != nil {
		result.error = err
		return result
	}

	result.links = links
	result.assets = assets
	return result
}

func startWorkers(maxWorkers int, done <-chan struct{}, requests <-chan request, results chan<- *result) *sync.WaitGroup {
	wg := &sync.WaitGroup{}

	for i := 0; i < maxWorkers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				select {
				case req := <-requests:
					results <- fetch(req)
				case <-done:
					return
				}
			}
		}()
	}

	return wg
}

// Crawl from URL, using concurrent download workers. Each discovered
// link is added to the queue based on the result of applying it to
// the followLink filter.
func Crawl(URL string, maxWorkers int, fetcher Fetcher, followLink FollowLink) []*Page {
	if maxWorkers < 1 {
		maxWorkers = 1
	}

	done := make(chan struct{})
	pages := []*Page{}
	requests := make(chan request)
	results := make(chan *result)
	seen := make(map[string]bool)

	pending := []request{{
		Fetcher: fetcher,
		URL:     URL,
	}}

	outstandingDownloads := 0

	wg := startWorkers(maxWorkers, done, requests, results)

	// Queue requests which are picked up by a worker. The result
	// is analyzed and discovered links are added to the queue.
	// Exit when there are zero outstanding downloads and the
	// pending queue is empty.

	for {
		var send chan<- request
		var link request

		if len(pending) > 0 {
			send = requests
			link = pending[0]
		} else if outstandingDownloads == 0 {
			break
		}

		select {
		case send <- link:
			outstandingDownloads++
			pending = pending[1:]
		case result := <-results:
			outstandingDownloads--

			page := &Page{
				URL:    result.request.URL,
				Links:  []string{},
				Assets: []string{},
			}

			pages = append(pages, page)

			if result.error != nil {
				page.FetcherError = result.error
				continue
			}

			page.Links = result.links
			page.Assets = result.assets

			for _, link := range result.links {
				if seen[link] {
					continue
				}
				seen[link] = true
				if followLink(link) {
					pending = append(pending, request{
						Fetcher:   fetcher,
						OriginURL: result.request.URL,
						URL:       link,
					})
				}
			}
		}
	}

	close(done)
	wg.Wait()

	return pages
}
