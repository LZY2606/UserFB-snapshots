package crawler

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
)

func TestBaselineDiscovery(t *testing.T) {
	var mu sync.Mutex
	hits := map[string]int{}
	out := map[string]int{}
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		if r.Method != "GET" {
			return
		}
		mu.Lock()
		hits[r.URL.Path]++
		mu.Unlock()
		if r.URL.Path == "/" {
			fmt.Fprint(w, `<html><head><meta name="robots" content="nofollow"></head><body><a href="/leaf" rel="nofollow">x</a><a href="/leaf">again</a><img src="/asset.png"></body></html>`)
		} else {
			fmt.Fprint(w, `<html><body>leaf</body></html>`)
		}
	}))
	defer s.Close()
	c := New(WithRobotsPolicy(RobotsIgnore), WithWorkersCount(2), WithMaxCrawlDepth(-1))
	err := c.Run(s.URL+"/", func(u string) { out[strings.TrimPrefix(u, s.URL)]++ })
	if err != nil {
		t.Fatal(err)
	}
	if hits["/"] != 1 || hits["/leaf"] != 1 || hits["/asset.png"] != 0 || out["/leaf"] != 1 || out["/asset.png"] != 1 {
		t.Fatalf("CONTENT discovery GET=%v callbacks=%v", hits, out)
	}
	t.Log("BASE_DISCOVERY legacy nofollow ignored, duplicate leaf fetched/output once, image output only")
}
