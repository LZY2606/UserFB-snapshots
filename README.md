Antch
====

[![Build Status](https://travis-ci.org/antchfx/antch.svg?branch=master)](https://travis-ci.org/antchfx/antch)
[![Coverage Status](https://coveralls.io/repos/github/antchfx/antch/badge.svg?branch=master)](https://coveralls.io/github/antchfx/antch?branch=master)
[![Go Report Card](https://goreportcard.com/badge/github.com/antchfx/antch)](https://goreportcard.com/report/github.com/antchfx/antch)
[![GoDoc](https://godoc.org/github.com/antchfx/antch?status.svg)](https://godoc.org/github.com/antchfx/antch)

**Antch**, inspired by [Scrapy](https://scrapy.org/). If you're familiar with scrapy, 
you can quickly get started.

Antch is a fast, powerful and extensible web crawling & scraping framework for Go, used 
to crawl websites and extract structured data from their pages.

Get Started
====

[Getting Started](https://github.com/antchfx/antch-getstarted)

Follow the Getting Started instructions to start your first spider.

- [English](https://github.com/antchfx/antch/wiki/Getting-Started)
- [简体中文](https://github.com/antchfx/antch/wiki/%E5%BF%AB%E9%80%9F%E5%85%A5%E9%97%A8)

Features
====

- Polite, highly concurrent web crawler.
- Powerful and customizable HTTP middleware.
- Item data pipeline for the web spider.
- Built-in proxy support (HTTP, HTTPS, SOCKS5).
- Built-in XPath query support for HTML/XML documents.
- Easy to use and integrate with your project.

Examples
===

[BingWallpaper](https://github.com/zhengchun/bingwallpaper) - Bing daily wallpaper.

Documentation
====

See [https://github.com/antchfx/antch/wiki](https://github.com/antchfx/antch/wiki)

Collection crawls
====

`Crawler.Collect` supports full and incremental interface-catalog crawls. A
page announces the next full page with `X-Crawl-Next` or, on the last page,
the entry of the next incremental run with `X-Crawl-Delta`. Both headers are
relative to the final response URL after redirects.

`Collect` only saves a page after its registered Handler returned and every
Item that Handler sent during the call finished in the existing Pipeline. The
next page is requested after `Save` succeeds, so a failed `Save` leaves the
previous cursor in place and the page can be replayed (business code should
deduplicate its output).

```go
crawler := antch.NewCrawler()
crawler.UsePipeline(myPipeline)
crawler.Handle("example.com", myHandler)

state := antch.CollectionState{}

err := crawler.Collect(antch.CollectionOptions{
	URL:  "https://example.com/items?page=1",
	Full: true, // start at URL; false resumes at State.NextURL/DeltaURL
	State: state,
	Save: func(s antch.CollectionState) error {
		state = s
		return store.SaveCollectionState(s) // must support the next run
	},
})
if err != nil {
	log.Fatalf("collection failed, resume from %+v: %v", state, err)
}

// The returned state is the last successfully saved cursor. Run the next
// incremental collection from state.DeltaURL:
state = antch.CollectionState{
	SourceURL: state.SourceURL,
	DeltaURL:  state.DeltaURL,
}
```

The same `Crawler` may run several `Collect` calls concurrently and ordinary
`Crawl` requests can interleave with them; each collection only settles pages
it fetched itself.

