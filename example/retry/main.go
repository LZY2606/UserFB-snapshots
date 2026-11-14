// Command example: retrying replayable commands.
//
// A Fetcher sends every Command exactly once by default. A Command that
// explicitly opts in to replays - by implementing RetryableCommand - may be
// retried on 429 (Too Many Requests) and 503 (Service Unavailable)
// responses, up to Fetcher.RetryMax extra attempts. This is useful for
// idempotent POST queries against rate-limited APIs.
package main

import (
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/PuerkitoBio/fetchbot"
)

// queryCmd is a POST command that explicitly allows its request to be
// replayed. The Fetcher reads the body once, before the first attempt, and
// replays the exact same bytes on every retry.
type queryCmd struct {
	*fetchbot.Cmd
	payload string
}

// Retryable opts this command in to retries. It is called exactly once,
// after the robots.txt admission and before the first attempt.
func (c *queryCmd) Retryable() bool { return true }

// Reader provides the body of the POST request.
func (c *queryCmd) Reader() io.Reader { return strings.NewReader(c.payload) }

func main() {
	f := fetchbot.New(fetchbot.HandlerFunc(handler))
	// Allow up to 2 extra attempts. The wait between attempts is the
	// largest of the host's crawl delay, RetryDelay and the Retry-After
	// response header, capped by RetryMaxDelay (the crawl delay itself is
	// never shortened by that cap).
	f.RetryMax = 2
	f.RetryDelay = 500 * time.Millisecond
	f.RetryMaxDelay = 10 * time.Second
	// Bodies larger than this cannot be replayed: the command is not sent
	// at all and its handler receives fetchbot.ErrRetryBodyTooLarge.
	f.RetryBodyLimit = 1 << 20

	q := f.Start()
	u, err := url.Parse("https://example.org/api/query")
	if err != nil {
		panic(err)
	}
	if err := q.Send(&queryCmd{Cmd: &fetchbot.Cmd{U: u, M: "POST"}, payload: `{"q":"golang"}`}); err != nil {
		panic(err)
	}
	// Close blocks until every admitted command - including all of its
	// retry attempts - has been delivered to its handler.
	q.Close()
}

func handler(ctx *fetchbot.Context, res *http.Response, err error) {
	if err != nil {
		fmt.Printf("error: %s\n", err)
		return
	}
	fmt.Printf("[%d] %s %s\n", res.StatusCode, ctx.Cmd.Method(), ctx.Cmd.URL())
}
