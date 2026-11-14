package main

import (
	"fmt"
	"net/http"
	"time"

	"github.com/PuerkitoBio/fetchbot"
)

// This example shows a long-running crawler that refreshes robots.txt while
// it runs. With RobotsCacheTTL set, the rules for each origin are re-checked
// after the TTL expires, using a conditional request when the previous
// response provided an ETag or Last-Modified validator.
func main() {
	f := fetchbot.New(fetchbot.HandlerFunc(handler))

	// Re-read robots.txt at most once per minute per origin. Workers that go
	// idle are reclaimed, but the rules cache survives them.
	f.RobotsCacheTTL = time.Minute
	f.WorkerIdleTTL = 30 * time.Second

	queue := f.Start()
	queue.SendStringHead("http://google.com", "http://golang.org", "http://golang.org/doc")
	queue.Close()
}

func handler(ctx *fetchbot.Context, res *http.Response, err error) {
	if err != nil {
		fmt.Printf("error: %s\n", err)
		return
	}
	fmt.Printf("[%d] %s %s\n", res.StatusCode, ctx.Cmd.Method(), ctx.Cmd.URL())
}
