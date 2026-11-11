package geziyor_test

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync/atomic"
	"testing"
	"time"

	"github.com/geziyor/geziyor"
	"github.com/geziyor/geziyor/client"
)

func testHost(raw string) string { u, _ := url.Parse(raw); return u.Host }

// This test records the unmodified starting behavior, not the requested behavior.
func TestRedirectBaseline(t *testing.T) {
	t.Run("normal", func(t *testing.T) {
		var pages int32
		s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&pages, 1)
			fmt.Fprint(w, "catalog")
		}))
		defer s.Close()
		callbacks := 0
		geziyor.NewGeziyor(&geziyor.Options{StartURLs: []string{s.URL}, RobotsTxtDisabled: true, LogDisabled: true,
			ParseFunc: func(_ *geziyor.Geziyor, r *client.Response) {
				callbacks++
				if string(r.Body) != "catalog" {
					t.Error("wrong body")
				}
			},
		}).Start()
		if pages != 1 || callbacks != 1 {
			t.Fatalf("pages=%d callbacks=%d", pages, callbacks)
		}
		fmt.Printf("BASE_NORMAL pages=%d callbacks=%d\n", pages, callbacks)
	})
	t.Run("domain", func(t *testing.T) {
		var targetPages int32
		target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			atomic.AddInt32(&targetPages, 1)
			fmt.Fprint(w, "out-of-scope")
		}))
		defer target.Close()
		origin := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, target.URL+"/private", http.StatusFound)
		}))
		defer origin.Close()
		callbacks := 0
		geziyor.NewGeziyor(&geziyor.Options{StartURLs: []string{origin.URL}, AllowedDomains: []string{testHost(origin.URL)}, RobotsTxtDisabled: true, LogDisabled: true,
			ParseFunc: func(_ *geziyor.Geziyor, _ *client.Response) { callbacks++ },
		}).Start()
		if targetPages != 1 || callbacks != 1 {
			t.Fatalf("starting domain behavior changed: target=%d callbacks=%d", targetPages, callbacks)
		}
		fmt.Printf("BASE_DOMAIN_BYPASS forbidden_pages=%d callbacks=%d\n", targetPages, callbacks)
	})
	t.Run("robots", func(t *testing.T) {
		var robots, pages int32
		target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Path == "/robots.txt" {
				atomic.AddInt32(&robots, 1)
				fmt.Fprint(w, "User-agent: *\nDisallow: /private\n")
				return
			}
			atomic.AddInt32(&pages, 1)
			fmt.Fprint(w, "private")
		}))
		defer target.Close()
		origin := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Path == "/robots.txt" {
				fmt.Fprint(w, "User-agent: *\nAllow: /\n")
				return
			}
			http.Redirect(w, r, target.URL+"/private", http.StatusFound)
		}))
		defer origin.Close()
		callbacks := 0
		geziyor.NewGeziyor(&geziyor.Options{StartURLs: []string{origin.URL}, AllowedDomains: []string{testHost(origin.URL), testHost(target.URL)}, LogDisabled: true,
			ParseFunc: func(_ *geziyor.Geziyor, _ *client.Response) { callbacks++ },
		}).Start()
		if robots != 0 || pages != 1 || callbacks != 1 {
			t.Fatalf("starting robots behavior changed: robots=%d pages=%d callbacks=%d", robots, pages, callbacks)
		}
		fmt.Printf("BASE_ROBOTS_BYPASS target_robots=%d forbidden_pages=%d\n", robots, pages)
	})
	t.Run("convergence", func(t *testing.T) {
		entered := make(chan struct{}, 2)
		release := make(chan struct{})
		var active, peak int32
		target := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			n := atomic.AddInt32(&active, 1)
			for {
				p := atomic.LoadInt32(&peak)
				if n <= p || atomic.CompareAndSwapInt32(&peak, p, n) {
					break
				}
			}
			entered <- struct{}{}
			<-release
			atomic.AddInt32(&active, -1)
			fmt.Fprint(w, "page")
		}))
		defer target.Close()
		redirect := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, target.URL+"/hold", http.StatusFound)
		})
		a := httptest.NewServer(redirect)
		defer a.Close()
		b := httptest.NewServer(redirect)
		defer b.Close()
		done := make(chan struct{})
		var callbacks int32
		go func() {
			defer close(done)
			geziyor.NewGeziyor(&geziyor.Options{StartURLs: []string{a.URL, b.URL}, RobotsTxtDisabled: true, LogDisabled: true, ConcurrentRequests: 2, ConcurrentRequestsPerDomain: 1,
				ParseFunc: func(_ *geziyor.Geziyor, _ *client.Response) { atomic.AddInt32(&callbacks, 1) },
			}).Start()
		}()
		for i := 0; i < 2; i++ {
			select {
			case <-entered:
			case <-time.After(3 * time.Second):
				close(release)
				<-done
				t.Fatal("starting behavior no longer converges concurrently")
			}
		}
		close(release)
		<-done
		if peak != 2 || callbacks != 2 {
			t.Fatalf("peak=%d callbacks=%d", peak, callbacks)
		}
		fmt.Printf("BASE_HOST_BYPASS configured=1 actual_peak=%d callbacks=%d\n", peak, callbacks)
	})
}
