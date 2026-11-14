package middleware

import (
	"github.com/geziyor/geziyor/client"
	"github.com/geziyor/geziyor/internal"
	"sync"
)

// DuplicateRequests checks for already visited URLs
type DuplicateRequests struct {
	RevisitEnabled bool
	visitedURLs    sync.Map
	logOnlyOnce    sync.Map
}

func (a *DuplicateRequests) ProcessRequest(r *client.Request) {
	if !a.RevisitEnabled && r.Request.Method == "GET" {
		requestURL := r.Request.URL.String()
		dedupKey := requestURL
		if r.DedupScope != "" {
			dedupKey = r.DedupScope + "\x00" + requestURL
		}
		if _, visited := a.visitedURLs.LoadOrStore(dedupKey, struct{}{}); visited {
			if _, logged := a.logOnlyOnce.LoadOrStore(dedupKey, struct{}{}); !logged {
				internal.Logger.Printf("URL already visited %s\n", requestURL)
			}
			r.Cancel()
		}
	}
}
