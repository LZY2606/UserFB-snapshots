package ant

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/url"

	"github.com/yields/ant/internal/normalize"
	"github.com/yields/ant/internal/robots"
	"golang.org/x/sync/errgroup"
	"golang.org/x/sync/semaphore"
)

// EngineConfig configures the engine.
type EngineConfig struct {
	// Scraper is the scraper to use.
	//
	// If nil, NewEngine returns an error.
	Scraper Scraper

	// Deduper is the URL de-duplicator to use.
	//
	// If nil, DedupeMap is used.
	Deduper Deduper

	// Fetcher is the page fetcher to use.
	//
	// If nil, the default HTTP fetcher is used.
	Fetcher *Fetcher

	// Queue is the URL queue to use.
	//
	// If nil, the default in-memory queue is used.
	Queue Queue

	// Limiter is the rate limiter to use.
	//
	// The limiter is called with each URL before
	// it is fetched.
	//
	// If nil, no limits are used.
	Limiter Limiter

	// Matcher is the URL matcher to use.
	//
	// The matcher is called with a URL before it is queued
	// if it returns false the URL is discarded.
	//
	// If nil, all URLs are queued.
	Matcher Matcher

	// Impolite skips any robots.txt checking.
	//
	// Note that it does not affect any configured
	// ratelimiters or matchers.
	//
	// By default the engine checks robots.txt, it uses
	// the default ant.UserAgent.
	Impolite bool

	// Workers specifies the amount of workers to use.
	//
	// Every worker the engine start consumes URLs from the queue
	// and starts a goroutine for each URL.
	//
	// If <= 0, defaults to 1.
	Workers int

	// Concurrency is the maximum amount of URLs to process
	// at any given time.
	//
	// The engine uses a global semaphore to limit the amount
	// of goroutines started by the workers.
	//
	// If <= 0, there's no limit.
	Concurrency int
}

// Engine implements web crawler engine.
type Engine struct {
	deduper  Deduper
	scraper  Scraper
	fetcher  *Fetcher
	queue    Queue
	matcher  Matcher
	limiter  Limiter
	robots   *robots.Cache
	impolite bool
	workers  int
	sema     *semaphore.Weighted
}

// NewEngine returns a new engine.
func NewEngine(c EngineConfig) (*Engine, error) {
	if c.Scraper == nil {
		return nil, errors.New("ant: scraper is required")
	}

	if c.Deduper == nil {
		c.Deduper = DedupeMap()
	}

	if c.Fetcher == nil {
		c.Fetcher = &Fetcher{}
	}

	if c.Workers <= 0 {
		c.Workers = 1
	}

	if c.Queue == nil {
		c.Queue = MemoryQueue(c.Workers)
	}

	var sema *semaphore.Weighted
	if n := int64(c.Concurrency); n > 0 {
		sema = semaphore.NewWeighted(n)
	}

	return &Engine{
		scraper:  c.Scraper,
		deduper:  c.Deduper,
		fetcher:  c.Fetcher,
		queue:    c.Queue,
		matcher:  c.Matcher,
		limiter:  c.Limiter,
		robots:   robots.NewCache(DefaultClient, 1000),
		impolite: c.Impolite,
		workers:  c.Workers,
		sema:     sema,
	}, nil
}

// Run runs the engine with the given start urls.
//
// Run blocks until every worker, in-flight HTTP request and
// scraper call has returned. URLs that are still queued when
// the round shuts down are discarded through Queue.Close;
// every URL that was dequeued is acknowledged with Done exactly
// once, whether its request started or not.
//
// The round shuts down when the queue is drained, the given
// context is canceled, or a worker reports an unrecoverable
// error. In the error case the first non-context error is
// returned and can be inspected with errors.Is; when the
// shutdown was caused by the given context its error wins.
func (eng *Engine) Run(ctx context.Context, urls ...string) error {
	// Enqueue initial URLs.
	if err := eng.Enqueue(ctx, urls...); err != nil {
		return fmt.Errorf("ant: enqueue - %w", err)
	}

	runCtx, stop := context.WithCancel(ctx)
	defer stop()

	// A single errgroup tracks both the worker goroutines and
	// the per-URL processing goroutines, so the first failure
	// cancels every worker and in-flight request immediately,
	// and Run returns only once all of them have exited.
	eg, egCtx := errgroup.WithContext(runCtx)
	for i := 0; i < eng.workers; i++ {
		eg.Go(func() error {
			return eng.run(eg, egCtx)
		})
	}

	// queue.Wait blocks until every enqueued URL has been
	// acknowledged or discarded, run it on its own goroutine
	// so shutdown can be triggered by cancellation or a
	// worker error as well.
	waitExited := make(chan struct{})
	go func() {
		eng.queue.Wait()
		close(waitExited)
	}()

	select {
	case <-waitExited:
		// All URLs were handled.
	case <-ctx.Done():
		// External cancellation: stop dispatching new work.
		stop()
	case <-egCtx.Done():
		// A worker failed: errgroup already canceled egCtx,
		// which cancels every sibling worker.
	}

	// Discard URLs that no worker picked up and wake up the
	// workers blocked on Dequeue. The context is intentionally
	// backgrounded so teardown cannot be aborted by the
	// canceled run context.
	closeErr := eng.queue.Close(context.Background())

	// Wait until every worker and in-flight request exits.
	err := eg.Wait()

	// The caller's context is the root cause: keep its error
	// even if the cancellation surfaced context errors inside
	// the workers.
	if ctx.Err() != nil {
		return fmt.Errorf("ant: run - %w", ctx.Err())
	}
	if err != nil {
		return fmt.Errorf("ant: run - %w", err)
	}
	if closeErr != nil {
		return closeErr
	}

	return nil
}

// Enqueue enqueues the given set of URLs.
//
// The method blocks until all URLs are queued
// or the given context is canceled.
//
// The method will also de-duplicate the URLs, ensuring
// that URLs will not be visited more than once.
func (eng *Engine) Enqueue(ctx context.Context, rawurls ...string) error {
	var batch = make(URLs, 0, len(rawurls))

	for _, rawurl := range rawurls {
		u, err := url.Parse(rawurl)
		if err != nil {
			return fmt.Errorf("ant: parse url %q - %w", rawurl, err)
		}

		switch u.Scheme {
		case "https", "http":
		default:
			return fmt.Errorf("ant: cannot enqueue invalid URL %q", u)
		}

		batch = append(batch, u)
	}

	return eng.enqueue(ctx, batch)
}

// Enqueue enqueues the given parsed urls.
func (eng *Engine) enqueue(ctx context.Context, batch URLs) error {
	for j := range batch {
		batch[j] = normalize.URL(batch[j])
	}

	next, err := eng.dedupe(ctx, eng.matches(batch))
	if err != nil {
		return err
	}

	if err := eng.queue.Enqueue(ctx, next); err != nil {
		return err
	}

	return nil
}

// Run runs a single crawl worker.
//
// The worker is in charge of fetching a url from
// the queue, creating a page and then calling the scraper.
//
// All per-URL work is registered on the engine-wide errgroup so
// that the first failure cancels every sibling worker through
// egCtx, while the engine still waits for every in-flight
// request and scraper before Run returns.
func (eng *Engine) run(eg *errgroup.Group, egCtx context.Context) error {
	var fatal error

	for {
		url, err := eng.queue.Dequeue(egCtx)

		if errors.Is(err, io.EOF) {
			// The queue was closed: nothing more will be
			// dispatched.
			break
		}
		if err != nil {
			// A context error means the round is shutting
			// down and must not mask the real failure, which
			// is tracked by the shared errgroup. Any other
			// queue error is itself unrecoverable.
			if !errors.Is(err, context.Canceled) &&
				!errors.Is(err, context.DeadlineExceeded) {
				fatal = err
			}
			break
		}

		if eng.sema != nil {
			if err := eng.sema.Acquire(egCtx, 1); err != nil {
				// The URL was already dequeued, so it must
				// still be acknowledged exactly once even if
				// its request never started.
				if dErr := eng.queue.Done(context.Background(), url); dErr != nil && fatal == nil {
					fatal = dErr
				}
				break
			}
		}

		eg.Go(func() error {
			if eng.sema != nil {
				defer eng.sema.Release(1)
			}
			return eng.process(egCtx, url)
		})
	}

	// Returning from the worker never cancels the shared
	// context by itself: the engine keeps waiting for every
	// per-URL goroutine already registered on the errgroup.
	return fatal
}

// Process processes a single url.
func (eng *Engine) process(ctx context.Context, url *URL) (err error) {
	// Acknowledgement is unconditional: once the URL left the
	// queue it is accounted for exactly once, regardless of how
	// the processing ends. A Queue error while acknowledging is
	// still surfaced when processing otherwise succeeded.
	defer func() {
		if dErr := eng.queue.Done(context.Background(), url); dErr != nil && err == nil {
			err = dErr
		}
	}()

	// Check robots.txt.
	if !eng.impolite {
		allowed, err := eng.robots.Allowed(ctx, robots.Request{
			URL:       url,
			UserAgent: UserAgent.String(),
		})
		if err != nil {
			return err
		}
		if !allowed {
			return nil
		}
	}

	// Potential limits.
	if err := eng.limit(ctx, url); err != nil {
		return err
	}

	// Scrape the URL.
	urls, err := eng.scrape(ctx, url)
	if err != nil {
		return err
	}

	// Enqueue URLs.
	if err := eng.enqueue(ctx, urls); err != nil {
		return fmt.Errorf("ant: enqueue - %w", err)
	}

	return nil
}

// Scrape scrapes the given URL and returns the next URLs.
func (eng *Engine) scrape(ctx context.Context, url *URL) (URLs, error) {
	page, err := eng.fetcher.Fetch(ctx, url)

	if err != nil {
		return nil, fmt.Errorf("ant: fetch %q - %w", url, err)
	}

	if page == nil {
		return nil, nil
	}

	defer page.close()

	urls, err := eng.scraper.Scrape(ctx, page)
	if err != nil {
		return nil, fmt.Errorf("ant: scrape %q - %w", url, err)
	}

	return urls, nil
}

// Dedupe de-duplicates the given slice of URLs.
func (eng *Engine) dedupe(ctx context.Context, urls URLs) (URLs, error) {
	deduped, err := eng.deduper.Dedupe(ctx, urls)
	if err != nil {
		return nil, fmt.Errorf("ant: dedupe - %w", err)
	}

	return deduped, nil
}

// Limit runs all configured limiters.
func (eng *Engine) limit(ctx context.Context, url *URL) error {
	if eng.limiter != nil {
		if err := eng.limiter.Limit(ctx, url); err != nil {
			return fmt.Errorf("limit %q - %w", url, err)
		}
	}

	if eng.impolite {
		return nil
	}

	err := eng.robots.Wait(ctx, robots.Request{
		URL:       url,
		UserAgent: UserAgent.String(),
	})
	if err != nil {
		return fmt.Errorf("ant: robots wait - %w", err)
	}

	return nil
}

// Matches returns all URLs that match the matcher.
func (eng *Engine) matches(urls URLs) URLs {
	if eng.matcher != nil {
		ret := make(URLs, 0, len(urls))
		for _, u := range urls {
			if eng.matcher.Match(u) {
				ret = append(ret, u)
			}
		}
		return ret
	}
	return urls
}
