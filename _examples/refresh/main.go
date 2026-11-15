package main

import (
	"context"
	"log"
	"time"

	"github.com/yields/ant"
)

// Scraper refreshes the index page every time it is scraped and
// follows all article links it finds.
type scraper struct {
	eng *ant.Engine
}

func (s *scraper) Scrape(ctx context.Context, p *ant.Page) (ant.URLs, error) {
	if p.URL.Path == "/" {
		// Notify the active round that the index should be
		// refreshed, the notification coalesces into a single
		// successor fetch once this callback returns.
		if err := s.eng.Refresh(ctx, p.URL.String(), ant.RefreshIncremental); err != nil {
			return nil, err
		}
	}
	return p.Next("article a")
}

func main() {
	var ctx = context.Background()
	var s = new(scraper)

	eng, err := ant.NewEngine(ant.EngineConfig{
		Scraper: s,
	})
	if err != nil {
		log.Fatalf("new engine: %s", err)
	}
	s.eng = eng

	// The first round performs a full crawl and saves the
	// representations (ETag, Last-Modified and outlinks) of all
	// successfully processed pages on the engine.
	if err := eng.RunRefresh(ctx, ant.RefreshFull, "https://example.com"); err != nil {
		log.Fatal(err)
	}

	// Later rounds re-use the saved representations, incremental
	// rounds send conditional requests and a 304 response keeps
	// the old outlinks without calling the scraper.
	for range time.Tick(time.Hour) {
		if err := eng.RunRefresh(ctx, ant.RefreshIncremental, "https://example.com"); err != nil {
			log.Fatal(err)
		}
	}
}
