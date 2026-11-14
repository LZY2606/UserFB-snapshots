package crawler

import (
	"io"
	"net/http"
	"net/http/httptest"
	"slices"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/s0rg/crawley/task_support/taskcheck"
)

// reqLog records actual HTTP requests hitting a test server.
type reqLog struct {
	mu     sync.Mutex
	counts map[string]int
}

func newReqLog() *reqLog {
	return &reqLog{counts: make(map[string]int)}
}

func (l *reqLog) record(r *http.Request) {
	l.mu.Lock()
	defer l.mu.Unlock()

	l.counts[r.Method+" "+r.URL.RequestURI()]++
}

func (l *reqLog) count(method, uri string) (rv int) {
	l.mu.Lock()
	defer l.mu.Unlock()

	return l.counts[method+" "+uri]
}

func htmlReply(w http.ResponseWriter, body string) {
	w.Header().Set(contentType, contentHTML)
	_, _ = io.WriteString(w, body)
}

func canonicalTestCrawler(opts ...Option) *Crawler {
	base := []Option{
		WithWorkersCount(4),
		WithMaxCrawlDepth(-1),
		WithTimeout(30 * time.Second),
		WithCanonicalLinks(true),
	}

	return New(append(base, opts...)...)
}

func runCollect(c *Crawler, uri string) (out []string, err error) {
	err = c.Run(uri, func(s string) {
		out = append(out, s)
	})

	return
}

func prefixed(base string, paths ...string) (rv []string) {
	rv = make([]string, len(paths))

	for i, p := range paths {
		rv[i] = base + p
	}

	return
}

func TestCanonicalHead(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("real head vs fake declarations", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/page">p</a></body></html>`)
				case "/page":
					htmlReply(w, `<html><head>
<!-- <link rel="canonical" href="/fake-comment"> -->
<script>var s = '<link rel="canonical" href="/fake-script">';</script>
<template><link rel="canonical" href="/fake-template"></template>
<link rel="canonical" href="http://other.test/cross">
<link rel="canonical" href="/real">
<link rel="canonical" href="/second">
</head><body>
<link rel="canonical" href="/fake-body">
<a href="/other">o</a>
</body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/other", "/real")), "output: %v", out)

			for _, p := range []string{"/real", "/second", "/fake-comment", "/fake-script", "/fake-template", "/fake-body"} {
				c.Check(log.count(http.MethodGet, p) == 0, "GET %s happened", p)
				c.Check(log.count(http.MethodHead, p) == 0, "HEAD %s happened", p)
			}

			c.Check(log.count(http.MethodGet, "/page") == 1, "GET /page count")
			c.Check(log.count(http.MethodGet, "/other") == 1, "GET /other count")
		})

		t.Run("head base drives resolution", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/dir/page">a</a><a href="/p2">b</a></body></html>`)
				case "/dir/page":
					htmlReply(w, `<html><head>
<base href="/root/">
<base href="/ignored/">
<link rel="canonical" href="canon">
</head><body><a href="rel">r</a></body></html>`)
				case "/p2":
					htmlReply(w, `<html><head>
<base href="ftp://example.invalid/x/">
<link rel="canonical" href="c2">
</head><body><a href="rel2">r</a></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/c2", "/rel2", "/root/canon", "/root/rel")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/root/canon") == 0, "GET /root/canon happened")
			c.Check(log.count(http.MethodGet, "/c2") == 0, "GET /c2 happened")
			c.Check(log.count(http.MethodGet, "/root/rel") == 1, "GET /root/rel count")
			c.Check(log.count(http.MethodGet, "/rel2") == 1, "GET /rel2 count")
		})

		t.Run("redirect then relative canonical", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/redir">r</a></body></html>`)
				case "/redir":
					http.Redirect(w, r, "/final", http.StatusFound)
				case "/final":
					htmlReply(w, `<html><head><link rel="canonical" href="canon"></head><body></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/canon")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/redir") == 1, "GET /redir count")
			c.Check(log.count(http.MethodGet, "/final") == 1, "GET /final count")
			c.Check(log.count(http.MethodGet, "/canon") == 0, "GET /canon happened")
			c.Check(log.count(http.MethodHead, "/canon") == 0, "HEAD /canon happened")
		})

		t.Run("no canonical from error pages or non-HTML", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/err">e</a><a href="/plain">p</a></body></html>`)
				case "/err":
					w.Header().Set(contentType, contentHTML)
					w.WriteHeader(http.StatusInternalServerError)
					_, _ = io.WriteString(w, `<html><head><link rel="canonical" href="/err-canon"></head><body><a href="/err-link">l</a></body></html>`)
				case "/plain":
					w.Header().Set(contentType, "text/plain")
					_, _ = io.WriteString(w, `<link rel="canonical" href="/plain-canon"><a href="/plain-link">l</a>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(WithoutHeads(true)), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/err", "/err-link", "/plain")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/err-canon") == 0, "GET /err-canon happened")
			c.Check(log.count(http.MethodGet, "/plain-canon") == 0, "GET /plain-canon happened")
			c.Check(log.count(http.MethodGet, "/plain-link") == 0, "GET /plain-link happened")
			c.Check(log.count(http.MethodGet, "/err-link") == 1, "GET /err-link count")
			c.Check(log.count(http.MethodHead, "/err") == 0, "HEAD /err happened")
		})

		t.Run("tag filter cannot block canonical declaration", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/p">p</a></body></html>`)
				case "/p":
					htmlReply(w, `<html><head><link rel="canonical" href="/c"></head><body><a href="/x">x</a><img src="/y"></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(WithTagsFilter([]string{"a"})), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/c", "/x")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/c") == 0, "GET /c happened")
			c.Check(log.count(http.MethodGet, "/x") == 1, "GET /x count")
			c.Check(log.count(http.MethodGet, "/y") == 0, "GET /y happened")
		})
	})
}

func TestCanonicalGraph(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("alias chain resolves to the end", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/a">a</a><a href="/b">b</a></body></html>`)
				case "/a":
					htmlReply(w, `<html><head><link rel="canonical" href="/b"></head><body></body></html>`)
				case "/b":
					htmlReply(w, `<html><head><link rel="canonical" href="/c"></head><body></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/c")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/a") == 1, "GET /a count")
			c.Check(log.count(http.MethodGet, "/b") == 1, "GET /b count")
			c.Check(log.count(http.MethodGet, "/c") == 0, "GET /c happened")
		})

		t.Run("cycles, predecessors and self-loops", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/x">x</a><a href="/y">y</a><a href="/p">p</a><a href="/s">s</a></body></html>`)
				case "/x":
					htmlReply(w, `<html><head><link rel="canonical" href="/y"></head><body></body></html>`)
				case "/y":
					htmlReply(w, `<html><head><link rel="canonical" href="/x"></head><body></body></html>`)
				case "/p":
					htmlReply(w, `<html><head><link rel="canonical" href="/y"></head><body></body></html>`)
				case "/s":
					htmlReply(w, `<html><head><link rel="canonical" href="/s"></head><body></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/s", "/x")), "output: %v", out)

			for _, p := range []string{"/x", "/y", "/p", "/s"} {
				c.Check(log.count(http.MethodGet, p) == 1, "GET %s count", p)
			}
		})

		t.Run("query order, path case and trailing slash stay significant", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.RequestURI() {
				case "/":
					htmlReply(w, `<html><body>`+
						`<a href="/q?a=1&amp;b=2">1</a><a href="/q?b=2&amp;a=1">2</a>`+
						`<a href="/Page">3</a><a href="/page">4</a>`+
						`<a href="/dir">5</a><a href="/dir/">6</a>`+
						`<a href="/p1">7</a></body></html>`)
				case "/p1":
					htmlReply(w, `<html><head><link rel="canonical" href="/q?a=1&amp;b=2"></head><body></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL,
				"/Page", "/dir", "/dir/", "/page", "/q?a=1&b=2", "/q?b=2&a=1",
			)), "output: %v", out)

			for _, u := range []string{"/q?a=1&b=2", "/q?b=2&a=1", "/Page", "/page", "/dir", "/dir/", "/p1"} {
				c.Check(log.count(http.MethodGet, u) == 1, "GET %s count", u)
			}
		})

		t.Run("redirect and canonical edges chain", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/r2">r</a><a href="/f2">f</a></body></html>`)
				case "/r2":
					http.Redirect(w, r, "/f2", http.StatusFound)
				case "/f2":
					htmlReply(w, `<html><head><link rel="canonical" href="/c2"></head><body></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/c2")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/r2") == 1, "GET /r2 count")
			c.Check(log.count(http.MethodGet, "/f2") >= 1, "GET /f2 count")
			c.Check(log.count(http.MethodGet, "/c2") == 0, "GET /c2 happened")
		})

		t.Run("cross-origin and userinfo canonicals are ignored", func(t *testing.T) {
			log := newReqLog()

			var ts *httptest.Server

			ts = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/p">p</a><a href="/p2">q</a></body></html>`)
				case "/p":
					htmlReply(w, `<html><head><link rel="canonical" href="http://other.test/canon"></head><body></body></html>`)
				case "/p2":
					htmlReply(w, `<html><head><link rel="canonical" href="http://user@`+ts.URL[len("http://"):]+`/uc"></head><body></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/p", "/p2")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/uc") == 0, "GET /uc happened")
		})
	})
}

func TestCanonicalLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		aliasSite := func(log *reqLog) http.HandlerFunc {
			return func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/a">a</a><a href="/b">b</a></body></html>`)
				case "/a", "/b":
					htmlReply(w, `<html><head><link rel="canonical" href="/article"></head><body><a href="/detail">d</a></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}
		}

		t.Run("default off keeps old behavior", func(t *testing.T) {
			log := newReqLog()
			ts := httptest.NewServer(aliasSite(log))
			defer ts.Close()

			crawler := New(
				WithWorkersCount(4),
				WithMaxCrawlDepth(-1),
				WithTimeout(30*time.Second),
			)

			out, err := runCollect(crawler, ts.URL+"/")
			c.Check(err == nil, "run: %v", err)

			slices.Sort(out)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/a", "/article", "/b", "/detail")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/a") == 1, "GET /a count")
			c.Check(log.count(http.MethodGet, "/b") == 1, "GET /b count")
			c.Check(log.count(http.MethodGet, "/detail") == 1, "GET /detail count")
			c.Check(log.count(http.MethodGet, "/article") == 0, "GET /article happened")
		})

		t.Run("explicit off keeps old behavior", func(t *testing.T) {
			log := newReqLog()
			ts := httptest.NewServer(aliasSite(log))
			defer ts.Close()

			crawler := New(
				WithWorkersCount(4),
				WithMaxCrawlDepth(-1),
				WithTimeout(30*time.Second),
				WithCanonicalLinks(false),
			)

			out, err := runCollect(crawler, ts.URL+"/")
			c.Check(err == nil, "run: %v", err)

			slices.Sort(out)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/a", "/article", "/b", "/detail")), "output: %v", out)
		})

		t.Run("on merges aliases", func(t *testing.T) {
			log := newReqLog()
			ts := httptest.NewServer(aliasSite(log))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/article", "/detail")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/a") == 1, "GET /a count")
			c.Check(log.count(http.MethodGet, "/b") == 1, "GET /b count")
			c.Check(log.count(http.MethodGet, "/detail") == 1, "GET /detail count")
			c.Check(log.count(http.MethodGet, "/article") == 0, "GET /article happened")
			c.Check(log.count(http.MethodHead, "/article") == 0, "HEAD /article happened")
		})

		t.Run("late declaration dedups in-flight discoveries", func(t *testing.T) {
			log := newReqLog()

			var (
				startedB    = make(chan struct{})
				edgeApplied = make(chan struct{})
			)

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				if r.Method == http.MethodHead {
					w.Header().Set(contentType, contentHTML)

					return
				}

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/a">a</a><a href="/b">b</a></body></html>`)
				case "/a":
					<-startedB // respond only while /b is in-flight
					htmlReply(w, `<html><head><link rel="canonical" href="/c"></head><body><a href="/sync">s</a></body></html>`)
				case "/sync":
					// by the time /sync is fetched, the scheduler has
					// already applied the /a -> /c edge
					close(edgeApplied)
					htmlReply(w, `<html><body></body></html>`)
				case "/b":
					close(startedB)
					<-edgeApplied
					htmlReply(w, `<html><body><a href="/c">c</a></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/b", "/c", "/sync")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/a") == 1, "GET /a count")
			c.Check(log.count(http.MethodGet, "/b") == 1, "GET /b count")
			c.Check(log.count(http.MethodGet, "/sync") == 1, "GET /sync count")
			c.Check(log.count(http.MethodGet, "/c") == 0, "GET /c happened")
			c.Check(log.count(http.MethodHead, "/c") == 0, "HEAD /c happened")
		})

		t.Run("output-only resource keeps its crawl chance", func(t *testing.T) {
			log := newReqLog()

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/p1">p</a></body></html>`)
				case "/p1":
					htmlReply(w, `<html><body><img src="/shared"><a href="/p2">p2</a></body></html>`)
				case "/p2":
					htmlReply(w, `<html><body><a href="/shared">s</a></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			out, err := runCollect(canonicalTestCrawler(), ts.URL+"/")
			c.Check(err == nil, "run: %v", err)
			c.Check(slices.Equal(out, prefixed(ts.URL, "/p1", "/p2", "/shared")), "output: %v", out)
			c.Check(log.count(http.MethodGet, "/shared") == 1, "GET /shared count")
			c.Check(log.count(http.MethodHead, "/shared") == 1, "HEAD /shared count")
		})

		t.Run("sequential runs rebuild identity state", func(t *testing.T) {
			log := newReqLog()

			var declare atomic.Bool

			declare.Store(true)

			ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				log.record(r)

				switch r.URL.Path {
				case "/":
					htmlReply(w, `<html><body><a href="/a">a</a></body></html>`)
				case "/a":
					head := ""

					if declare.Load() {
						head = `<head><link rel="canonical" href="/b"></head>`
					}

					htmlReply(w, `<html>`+head+`<body><a href="/b">b</a></body></html>`)
				default:
					htmlReply(w, `<html><body></body></html>`)
				}
			}))
			defer ts.Close()

			crawler := canonicalTestCrawler()

			out1, err := runCollect(crawler, ts.URL+"/")
			c.Check(err == nil, "run1: %v", err)
			c.Check(slices.Equal(out1, prefixed(ts.URL, "/b")), "output1: %v", out1)
			c.Check(log.count(http.MethodGet, "/b") == 0, "GET /b after run1")

			declare.Store(false)

			out2, err := runCollect(crawler, ts.URL+"/")
			c.Check(err == nil, "run2: %v", err)
			c.Check(slices.Equal(out2, prefixed(ts.URL, "/a", "/b")), "output2: %v", out2)
			c.Check(log.count(http.MethodGet, "/b") == 1, "GET /b after run2")
			c.Check(log.count(http.MethodGet, "/a") == 2, "GET /a total")
			c.Check(log.count(http.MethodGet, "/") == 2, "GET / total")
		})
	})
}
