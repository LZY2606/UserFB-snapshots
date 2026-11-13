// Package main is a small, self-contained demonstration of Spider.RunCheckpoint.
//
// It serves a tiny website whose pages link in a cycle, points a spider at it
// and keeps the crawl frontier in a checkpoint directory. Start it like:
//
//	go run ./example/checkpoint_resume -dir ./my-crawl
//
// Kill the process while it is crawling (the server sleeps on every response
// to give you time), then run the same command again. It resumes the
// remaining requests from ./my-crawl instead of re-traversing the entry page,
// and every url is still downloaded at most once. Once the crawl has drained,
// further runs are no-ops: Finish is not called a second time.
package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"time"

	"github.com/hu17889/go_spider/core/common/page"
	"github.com/hu17889/go_spider/core/common/request"
	"github.com/hu17889/go_spider/core/pipeline"
	"github.com/hu17889/go_spider/core/spider"
)

// site describes a small graph full of cycles:
//
//	/        -> /a /b
//	/a       -> /b /            (cycle back to the entry page)
//	/b       -> /c /a
//	/c       -> /               (another cycle)
//	/login   accepts POST and links back to /
var site = map[string][]string{
	"/":      {"/a", "/b"},
	"/a":     {"/b", "/"},
	"/b":     {"/c", "/a"},
	"/c":     {"/"},
	"/login": {"/"},
}

type cyclicProcessor struct{}

func (cyclicProcessor) Process(p *page.Page) {
	req := p.GetRequest()
	fmt.Printf("process %s %s\n", req.GetMethod(), req.GetUrl())
	p.AddField("url", req.GetUrl())
	p.AddField("method", req.GetMethod())

	origin := baseURL(req.GetUrl())
	for _, path := range site[pathOrRoot(req.GetUrl())] {
		p.AddTargetRequest(origin+path, "text")
	}
}

func (cyclicProcessor) Finish() {
	fmt.Println("crawl finished")
}

// newLoginRequest is a POST seed: its method, body and header must survive a
// process restart instead of being resurrected as a plain GET.
func newLoginRequest(url string) *request.Request {
	header := http.Header{}
	header.Set("Content-Type", "application/x-www-form-urlencoded")
	return request.NewRequest(url, "text", "login", "POST", "user=demo&pw=secret",
		header, nil, nil, nil)
}

// pathOrRoot returns the url path, treating an empty path as "/".
func pathOrRoot(rawurl string) string {
	idx := strings.Index(rawurl, "://")
	rest := rawurl
	if idx >= 0 {
		rest = rawurl[idx+3:]
	}
	if slash := strings.Index(rest, "/"); slash >= 0 {
		return rest[slash:]
	}
	return "/"
}

// baseURL strips the path so child urls can be built from the server origin.
func baseURL(rawurl string) string {
	idx := strings.Index(rawurl, "://")
	rest := rawurl
	if idx >= 0 {
		rest = rawurl[idx+3:]
	}
	if slash := strings.Index(rest, "/"); slash >= 0 {
		return rawurl[:idx+3+slash]
	}
	return rawurl
}

func newCyclicServer() *httptest.Server {
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Slow responses make it easy to press Ctrl-C mid-crawl.
		time.Sleep(200 * time.Millisecond)
		if _, ok := site[r.URL.Path]; !ok {
			http.NotFound(w, r)
			return
		}
		fmt.Fprintf(w, "page %s method=%s\n", r.URL.Path, r.Method)
	}))
}

func main() {
	dir := flag.String("dir", "./checkpoint-data", "directory holding the durable crawl frontier")
	flag.Parse()

	server := newCyclicServer()
	defer server.Close()

	sp := spider.NewSpider(cyclicProcessor{}, "checkpoint-resume-demo").
		CloseStrace().
		AddPipeline(pipeline.NewPipelineConsole()).
		SetThreadnum(2)

	// Ordinary GET seed, plus a POST seed proving parameters survive restarts.
	sp.AddUrl(server.URL+"/", "text")
	sp.AddRequest(newLoginRequest(server.URL + "/login"))

	// The same directory on every run: first run crawls, later runs resume.
	if err := sp.RunCheckpoint(context.Background(), *dir); err != nil {
		fmt.Println("crawl stopped, rerun the same command to resume:", err)
		return
	}
	fmt.Println("done; checkpoint directory:", *dir)
}
