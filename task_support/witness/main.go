package main

import (
 "fmt"
 "net/http"
 "net/http/httptest"
 "os"
 "reflect"
 "sync"
 "time"

 crawl "github.com/PuerkitoBio/gocrawl"
)

func main() {
 slowStarted, release, child := make(chan struct{}), make(chan struct{}), make(chan struct{})
 var releaseOnce, childOnce sync.Once
 unblock := func() { releaseOnce.Do(func() { close(release) }) }
 defer unblock()
 slow := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
  if r.URL.Path == "/robots.txt" { fmt.Fprint(w, "User-agent: *\nAllow: /\n"); return }
  close(slowStarted)
  <-release
  fmt.Fprint(w, "<html>peer</html>")
 }))
 defer slow.Close()
 fast := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
  switch r.URL.Path {
  case "/robots.txt": fmt.Fprint(w, "User-agent: *\nAllow: /\n")
  case "/root": <-slowStarted; fmt.Fprint(w, `<a href="/child">child</a>`)
  case "/child": childOnce.Do(func() { close(child) }); fmt.Fprint(w, "<html>child</html>")
  default: http.NotFound(w, r)
  }
 }))
 defer fast.Close()
 opts := crawl.NewOptions(new(crawl.DefaultExtender))
 opts.CrawlDelay, opts.LogFlags, opts.SameHostOnly = 0, 0, false
 _, present := reflect.TypeOf(*opts).FieldByName("Layered")
 fmt.Printf("LAYERED_OPTION_PRESENT=%t\n", present)
 crawler := crawl.NewCrawlerWithOptions(opts)
 done := make(chan error, 1)
 go func() { done <- crawler.Run([]string{fast.URL+"/root", slow.URL+"/peer"}) }()
 timer := time.NewTimer(8*time.Second)
 defer timer.Stop()
 select {
 case <-child:
  fmt.Println("BASELINE_OK child_before_peer_release=true")
 case <-timer.C:
  unblock()
  fmt.Fprintln(os.Stderr, "legacy crawl did not reach child")
  os.Exit(1)
 }
 unblock()
 select {
 case err := <-done:
  if err != nil { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
 case <-time.After(8*time.Second):
  fmt.Fprintln(os.Stderr, "crawl did not finish"); os.Exit(1)
 }
 fmt.Println("BASELINE_DRAIN_OK")
}
