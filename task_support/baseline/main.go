package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"time"

	rss "github.com/SlyMarbo/rss/v2"
)

type transport func(*http.Request) (*http.Response, error)

func (f transport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

type countedBody struct {
	io.Reader
	closed *int
}

func (b *countedBody) Close() error { *b.closed++; return nil }
func require(ok bool, message string) {
	if !ok {
		fmt.Fprintln(os.Stderr, "BASELINE FAILED:", message)
		os.Exit(1)
	}
}

func main() {
	now := time.Date(2026, 6, 1, 12, 0, 0, 0, time.UTC)
	requests, closed := 0, 0
	client := &http.Client{Transport: transport(func(req *http.Request) (*http.Response, error) {
		requests++
		require(req.Method == "GET" && req.URL.String() == "https://source.invalid/feed", "actual source request")
		items := `<item><guid>a</guid><title>original</title></item>`
		if requests == 2 {
			items = `<item><guid>a</guid><title>edited</title></item><item><guid>b</guid><title>new</title></item><item><guid>b</guid><title>duplicate</title></item>`
		}
		xml := `<rss version="2.0"><channel><title>source</title><description>data</description><ttl>2</ttl>` + items + `</channel></rss>`
		return &http.Response{StatusCode: 200, Header: make(http.Header), Request: req,
			Body: &countedBody{Reader: strings.NewReader(xml), closed: &closed}}, nil
	})}
	reader, err := rss.NewReader(rss.WithNow(func() time.Time { return now }), rss.WithHTTPClient(client))
	require(err == nil, "reader")
	feed, err := reader.Fetch(context.Background(), "https://source.invalid/feed")
	require(err == nil && len(feed.Items) == 1 && feed.Items[0].ID == "a" && feed.NextUpdate.Equal(now.Add(2*time.Minute)), "Fetch -> Parse -> ttl")
	fmt.Println("BASELINE single-source-fetch=ok")
	feed.Items[0].Read = true
	feed.Unread = 0
	err = reader.Update(context.Background(), feed)
	require(errors.Is(err, rss.ErrTooSoon) && requests == 1, "early update must avoid request")
	fmt.Println("BASELINE due-gate=ok")
	now = now.Add(2 * time.Minute)
	err = reader.Update(context.Background(), feed)
	require(err == nil && requests == 2 && len(feed.Items) == 2, "due update appends only unseen IDs")
	require(feed.Items[0].Title == "original" && feed.Items[0].Read && feed.Items[1].ID == "b" && feed.Items[1].Title == "new" && !feed.Items[1].Read && feed.Unread == 1, "existing merge/read/unread behavior")
	fmt.Println("BASELINE duplicate-read-unread=ok")
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	err = reader.UpdatePatiently(ctx, feed)
	require(errors.Is(err, context.Canceled) && requests == 2, "waiting cancellation avoids request")
	fmt.Println("BASELINE waiting-context=ok")
	require(closed == 2, "response body ownership")
	fmt.Printf("BASELINE response-bodies=%d/%d\n", closed, requests)
}
