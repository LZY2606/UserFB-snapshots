// Command mirror_catalog demonstrates mirror racing: the primary address is
// requested first, and only after MirrorDelay do the equivalent mirror
// addresses join the race. The first candidate that delivers a complete HTTP
// 2xx representation which parses as the requested RespType wins; everything
// else is canceled.
//
// Run it against a real site by replacing the URLs below:
//
//	go run ./example/mirror_catalog
package main

import (
	"fmt"
	"net/url"
	"time"

	"github.com/hu17889/go_spider/core/common/page"
	"github.com/hu17889/go_spider/core/common/request"
	"github.com/hu17889/go_spider/core/pipeline"
	"github.com/hu17889/go_spider/core/spider"
)

type CatalogProcessor struct{}

// Process runs exactly once for the winning representation.
func (this *CatalogProcessor) Process(p *page.Page) {
	body := p.GetJson()
	if body == nil {
		return
	}

	title := body.Get("title").MustString()
	p.AddField("title", title)
	p.AddField("fetched_from", p.GetFetchedURL())

	// Resolve a relative "next" link against the address that actually served
	// the winning body, then hand it back to the scheduler.
	if next := body.Get("next").MustString(); next != "" {
		if base, err := url.Parse(p.GetFetchedURL()); err == nil {
			if rel, err := url.Parse(next); err == nil {
				p.AddTargetRequest(base.ResolveReference(rel).String(), "json")
			}
		}
	}
}

func (this *CatalogProcessor) Finish() {}

func main() {
	req := request.NewRequest(
		"https://primary.example.com/api/catalog", // main address, tried first
		"json", "catalog", "GET", "", nil, nil, nil, nil,
	)
	// Equivalent mirror addresses. They are only sent if the primary is
	// still unfinished MirrorDelay after its request went out, or as soon as
	// the primary fails.
	req.MirrorURLs = []string{
		"https://mirror-a.example.com/api/catalog",
		"https://mirror-b.example.com/api/catalog",
	}
	req.MirrorDelay = 2 * time.Second

	// Spider.Run automatically binds its SetThreadnum value to the
	// HttpDownloader's physical request limit. To configure a downloader you
	// own, do it before running:
	//
	//   own := downloader.NewHttpDownloader().SetMaxConcurrentRequests(4)
	//   spider.NewSpider(...).SetDownloader(own).Run()
	//
	// Ordinary requests, the primary and every mirror share those slots.
	sp := spider.NewSpider(&CatalogProcessor{}, "catalog").
		SetThreadnum(4). // also the downloader's physical request cap
		CloseStrace().
		AddPipeline(pipeline.NewPipelineConsole())

	items := sp.GetByRequest(req)
	if items == nil {
		fmt.Println("all addresses failed")
		return
	}
	title, _ := items.GetItem("title")
	source, _ := items.GetItem("fetched_from")
	fmt.Printf("catalog %q served by %s\n", title, source)
}
