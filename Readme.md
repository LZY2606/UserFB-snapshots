
<br>
<br>
<br>

<p align=center>
  ant (<em>alpha</em>) is a web crawler for Go.
</p>

<br>
<br>
<br>

<p align=center>
  <a href="https://github.com/yields/ant/workflows/test">
    <img src="https://github.com/yields/ant/workflows/test/badge.svg?event=push" />
  </a>
  <a href="https://pkg.go.dev/github.com/yields/ant">
    <img src="https://pkg.go.dev/badge/github.com/yields/ant" />
  </a>
  <a href="https://goreportcard.com/report/github.com/yields/ant">
    <img src="https://goreportcard.com/badge/github.com/yields/ant" />
  </a>
</p>

<br>
<br>
<br>


<br>

#### Declarative

  The package includes functions that can scan data from the page into your structs
  or slice of structs, this allows you to reduce the noise and complexity in your source-code.

  You can also use a jQuery-like API that allows you to scrape complex HTML pages if needed.

  ```go

  var data struct { Title string `css:"title"` }
  page, _ := ant.Fetch(ctx, "https://apple.com")
  page.Scan(&data)
  data.Title // => Apple
  ```

<br>

#### Headless

  By default the crawler uses `http.Client`, however if you're crawling SPAs
  youc an use the `antcdp.Client` implementation which allows you to use chrome
  headless browser to crawl pages.

  ```go
  eng, err := ant.Engine(ant.EngineConfig{
    Fetcher: &ant.Fetcher{
      Client: antcdp.Client{},
    },
  })
  ```

<br>

#### Polite

  The crawler automatically fetches and caches `robots.txt`, making sure that
  it never causes issues to small website owners. Of-course you can disable
  this behavior.

  ```go
  eng, err := ant.NewEngine(ant.EngineConfig{
    Impolite: true,
  })
  eng.Run(ctx)
  ```

<br>

#### Concurrent

  The crawler maintains a configurable amount of "worker" goroutines that read
  URLs off the queue, and spawn a goroutine for each URL.

  Depending on your configuration, you may want to increase the number of workers
  to speed up URL reads, of-course if you don't have enough resources you can reduce
  the number of workers too.

  ```go
  eng, err := ant.NewEngine(ant.EngineConfig{
    // Spawn 5 worker goroutines that dequeue
    // URLs and spawn a new goroutine for each URL.
    Workers: 5,
  })
  eng.Run(ctx)
  ```

<br>

#### Rate limits

  The package includes a powerful `ant.Limiter` interface that allows you to
  define rate limits per URL. There are some built-in limiters as well.

  ```go
  ant.Limit(1) // 1 rps on all URLs.
  ant.LimitHostname(5, "amazon.com") // 5 rps on amazon.com hostname.
  ant.LimitPattern(5, "amazon.com.*") // 5 rps on URLs starting with `amazon.co.`.
  ant.LimitRegexp(5, "^apple.com\/iphone\/*") // 5 rps on URLs that match the regex.
  ```
  
  Note that `LimitPattern` and `LimitRegexp` only match on the host and path of the URL.

<br>

#### Matchers

  Another powerful interface is `ant.Matcher` which allows you to define URL
  matchers, the matchers are called before URLs are queued.

  ```go
  ant.MatchHostname("amazon.com") // scrape amazon.com URLs only.
  ant.MatchPattern("amazon.com/help/*")
  ant.MatchRegexp("amazon\.com\/help/.+")
  ```

<br>

#### Robust

  The crawl engine automatically retries any errors that implement `Temporary()`
  error that returns true.

  Becuase the standard library returns errors that implement that interface
  the engine will retry most temporary network and HTTP errors.

  ```go
  eng, err := ant.NewEngine(ant.EngineConfig{
    Scraper: myscraper{},
    MaxAttempts: 5,
  })

  // Blocks until one of the following is true:
  //
  // 1. No more URLs to crawl (the scraper stops returning URLs)
  // 2. A non-temporary error occured.
  // 3. MaxAttempts was reached.
  //
  err = eng.Run(ctx)
  ```

<br>

#### Built-in Scrapers

  The whole point of scraping is to extract data from websites into a machine readable
  format such as CSV or JSON, ant comes with built-in scrapers to make this ridiculously
  easy, here's a full cralwer that extracts quotes into stdout.


[embedmd]:# (_examples/jsonquotes/main.go /func main/ $)
```go
func main() {
	var url = "http://quotes.toscrape.com"
	var ctx = context.Background()
	var start = time.Now()

	type quote struct {
		Text string   `css:".text"   json:"text"`
		By   string   `css:".author" json:"by"`
		Tags []string `css:".tag"    json:"tags"`
	}

	type page struct {
		Quotes []quote `css:".quote" json:"quotes"`
	}

	eng, err := ant.NewEngine(ant.EngineConfig{
		Scraper: ant.JSON(os.Stdout, page{}, `li.next > a`),
		Matcher: ant.MatchHostname("quotes.toscrape.com"),
	})
	if err != nil {
		log.Fatalf("new engine: %s", err)
	}

	if err := eng.Run(ctx, url); err != nil {
		log.Fatal(err)
	}

	log.Printf("scraped in %s :)", time.Since(start))
}
```
<br>

#### Graceful shutdown

  `Run` always waits for the whole crawl round to wind down before it
  returns: every worker goroutine, in-flight HTTP request and `Scraper`
  call has to exit. It never returns early by skipping `Queue.Wait` or by
  resetting the queue's counters.

  A round shuts down for one of three reasons:

  - There are no more URLs to crawl and every dequeued URL was handled.
  - The `context.Context` passed to `Run` was canceled.
  - A worker hit an unrecoverable `Scraper`, `Limiter`, `Deduper`,
    `Queue` or fetch error.

  On shutdown the queue is closed through `Queue.Close`, which discards
  the entries no worker had dequeued yet. Every entry a worker *did*
  dequeue is acknowledged with `Done` exactly once, whether its HTTP
  request started, it was still waiting for a `Concurrency` slot, or the
  round was canceled while it ran.

  When a worker fails, new dispatching stops immediately and the sibling
  workers are canceled through the shared context; a scraper already
  running is still awaited, so it can finish cleaning its resources up
  after observing cancellation. The first non-context failure is returned
  from `Run` wrapped with `%w`, so it stays reachable through
  `errors.Is` and is not masked by the `context.Canceled` or queue-close
  errors it triggered. If the root cause was the caller canceling the
  context, the caller's context error wins instead.

  ```go
  ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
  defer cancel()

  // Returns after every worker, request and scraper has exited.
  // On timeout the error satisfies errors.Is(err, context.DeadlineExceeded);
  // on a scraper failure it satisfies errors.Is(err, scraperErr).
  err := eng.Run(ctx, "https://example.com")
  ```

  `MemoryQueue` follows the same contract on its own. A `Dequeue` parked
  on an empty queue returns the context's error as soon as that context
  is canceled, without needing another `Enqueue` or `Close` call to wake
  it. After `Close`, `Dequeue` keeps returning `io.EOF`, enqueuing fails
  with `io.EOF`, and calling `Close` more than once is a no-op that does
  not account for the discarded entries a second time. Custom `Queue`
  implementations only need the public interface; the engine's teardown
  never relies on the internals of the in-memory queue.

<br>

#### Testing

  `anttest` package makes it easy to test your scraper implementation
  it fetches a page by a URL, caches it in the OS's temporary directory and re-uses it.

  The func depends on the file's modtime, the file expires daily, you can adjust
  the TTL by setting `antttest.FetchTTL`.

  ```Go
  // Fetch calls `t.Fatal` on errors.
  page := anttest.Fetch(t, "https://apple.com")
  _, err := myscraper.Scrape(ctx, page)
  assert.NoError(err)
  ```

<br>
<br>
