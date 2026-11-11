package colly

import (
    "io"
    "net/http"
    "strings"
    "sync/atomic"
    "testing"
    "time"
)
type stubTransport func(*http.Request) (*http.Response, error)
func (f stubTransport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }
func TestRobotsBaseline(t *testing.T) {
    var calls atomic.Int32
    entered, release := make(chan struct{}, 2), make(chan struct{})
    c := NewCollector()
    c.IgnoreRobotsTxt = false
    c.WithTransport(stubTransport(func(r *http.Request) (*http.Response, error) {
        body := "ok"
        if r.URL.Path == "/robots.txt" {
            calls.Add(1); entered <- struct{}{}; <-release
            body = "User-agent: *\nDisallow: /blocked\n"
        }
        return &http.Response{StatusCode:200, Header:make(http.Header), Body:io.NopCloser(strings.NewReader(body)), Request:r}, nil
    }))
    result := make(chan error, 2)
    go func(){ result <- c.Visit("http://sample.invalid/a") }()
    go func(){ result <- c.Visit("http://sample.invalid/b") }()
    for i:=0; i<2; i++ { select { case <-entered: case <-time.After(2*time.Second): close(release); t.Fatal("expected two concurrent discovery fetches") } }
    close(release)
    for i:=0; i<2; i++ { if err:= <-result; err!=nil {t.Fatal(err)} }
    if calls.Load()!=2 {t.Fatal(calls.Load())}
    if err:=c.Visit("http://sample.invalid/blocked"); err!=ErrRobotsTxtBlocked {t.Fatalf("unexpected policy result %v",err)}
    if calls.Load()!=2 {t.Fatal("cache miss after discovery")}
    t.Log("BASELINE: two same-origin robots misses fetched twice; cache blocks later path")
}
