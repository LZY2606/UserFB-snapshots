package main

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"sync/atomic"

	"github.com/gocolly/colly/v2"
)

const manualHeader = "Digest username=\"collector\", realm=\"local reports\", nonce=\"baseline-nonce\", uri=\"/reports?month=11\", response=\"21d73a46c34f3eab83f8fb991af7a945aea2bb8b7bab08fb237ceaae0da9740d\", algorithm=SHA-256, qop=auth-int, nc=00000001, cnonce=\"baseline-cnonce\""

type transport func(*http.Request) (*http.Response, error)

func (f transport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

type countedBody struct {
	*bytes.Reader
	closes *atomic.Int32
}

func (b *countedBody) Close() error { b.closes.Add(1); return nil }
func check(ok bool, msg string) {
	if !ok {
		panic(msg)
	}
}

func main() {
	entity := []byte{0, 'r', 'e', 'p', 'o', 'r', 't', '=', 'e', 'l', 'e', 'v', 'e', 'n', 255}
	var sends, errorsSeen, headersSeen, responses, scraped, requests, requestHeaders atomic.Int32
	var unauthorizedClosed, successClosed atomic.Int32
	c := colly.NewCollector(colly.IgnoreRobotsTxt(), colly.AllowURLRevisit())
	c.WithTransport(transport(func(r *http.Request) (*http.Response, error) {
		sends.Add(1)
		check(r.Method == "POST" && r.URL.RequestURI() == "/reports?month=11", "actual method and URI")
		check(r.GetBody != nil, "standard reader produces replay factory")
		body, err := io.ReadAll(r.Body)
		check(err == nil && bytes.Equal(body, entity), "actual entity bytes")
		check(r.Body.Close() == nil, "request body close")
		if r.Header.Get("Authorization") == "" {
			return &http.Response{StatusCode: 401, Header: http.Header{
				"Www-Authenticate": {`Digest realm="local reports", nonce="baseline-nonce", algorithm=SHA-256, qop="auth-int"`},
				"Content-Type":     {"text/plain"},
			}, Body: &countedBody{bytes.NewReader([]byte("challenge")), &unauthorizedClosed}, Request: r}, nil
		}
		check(r.Header.Get("Authorization") == manualHeader, "single fixed manual Digest header preserved")
		return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": {"text/plain"}},
			Body: &countedBody{bytes.NewReader([]byte("report-ready")), &successClosed}, Request: r}, nil
	}))
	c.OnRequest(func(r *colly.Request) { requests.Add(1) })
	c.OnRequestHeaders(func(r *colly.Request) { requestHeaders.Add(1) })
	c.OnResponseHeaders(func(r *colly.Response) { headersSeen.Add(1) })
	c.OnError(func(r *colly.Response, err error) {
		check(r.StatusCode == 401, "401 reaches current OnError")
		errorsSeen.Add(1)
	})
	c.OnResponse(func(r *colly.Response) { check(string(r.Body) == "report-ready", "control body"); responses.Add(1) })
	c.OnScraped(func(r *colly.Response) { scraped.Add(1) })
	ctx := colly.NewContext()
	ctx.Put("source", "local-fixture")
	err := c.Request("POST", "http://digest.test/reports?month=11", bytes.NewReader(entity), ctx, nil)
	check(err != nil && sends.Load() == 1 && errorsSeen.Load() == 1, "no automatic challenge retry")
	check(responses.Load() == 0 && scraped.Load() == 0 && headersSeen.Load() == 1, "current failure callbacks")
	check(unauthorizedClosed.Load() == 1, "401 body closes once")
	fmt.Println("BASELINE one unauthenticated POST reaches 401 and OnError; no automatic Digest retry")
	hdr := http.Header{"Authorization": {manualHeader}}
	err = c.Request("POST", "http://digest.test/reports?month=11", bytes.NewReader(entity), ctx, hdr)
	check(err == nil && sends.Load() == 2 && responses.Load() == 1 && scraped.Load() == 1, "manual authenticated control")
	check(requests.Load() == 2 && requestHeaders.Load() == 2 && headersSeen.Load() == 2, "real callback boundaries")
	check(successClosed.Load() == 1 && unauthorizedClosed.Load() == 1, "all response bodies close once")
	fmt.Println("CONTROL fixed manual Digest header and binary POST entity survive real Collector transport")
	fmt.Println("CONTROL successful response and OnScraped delivered once; each response body closed once")
}
