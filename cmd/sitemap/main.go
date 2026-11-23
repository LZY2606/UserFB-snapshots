package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/url"
	"os"
	"time"

	"github.com/frobware/grawler"
)

var (
	concurrency int
	perOrigin   int
	originGap   time.Duration
)

func init() {
	flag.IntVar(&concurrency, "j", 50, "number of concurrent downloads")
	flag.IntVar(&perOrigin, "per-origin", 0, "maximum concurrent downloads per origin (defaults to -j)")
	flag.DurationVar(&originGap, "origin-gap", 0, "minimum delay between responses and the next request at one origin (e.g. 250ms)")
}

var usage = func() {
	fmt.Fprintf(os.Stderr, "%s [ -j <concurrency> ] [ -per-origin <n> ] [ -origin-gap <duration> ] URL\n", os.Args[0])
	flag.PrintDefaults()
}

func sameHost(base *url.URL, URL string) bool {
	u, err := url.Parse(URL)

	if err != nil {
		return false
	}

	return base.Hostname() == u.Hostname()
}

func main() {
	flag.Parse()

	if len(flag.Args()) < 1 {
		usage()
		os.Exit(1)
	}

	if perOrigin < 1 {
		perOrigin = concurrency
	}

	baseURL, err := url.Parse(flag.Arg(0))

	if err != nil {
		log.Fatal(err)
	}

	filter := func(URL string) bool {
		return sameHost(baseURL, URL)
	}

	pages, err := grawler.CrawlHTTP(
		[]string{flag.Arg(0)},
		grawler.HTTPOptions{
			MaxWorkers:        concurrency,
			MaxPerOrigin:      perOrigin,
			MinOriginInterval: originGap,
		},
		filter,
	)

	if err != nil {
		log.Fatal("error:", err)
	}

	b, err := json.MarshalIndent(pages, "", "  ")

	if err != nil {
		log.Fatal("error:", err)
	}

	fmt.Println(string(b))
}
