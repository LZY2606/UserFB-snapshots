[![Travis CI](https://travis-ci.org/frobware/grawler.svg?branch=master)](https://travis-ci.org/frobware/grawler)
[![GoDoc](https://img.shields.io/badge/godoc-reference-blue.svg?style=flat-square)](https://godoc.org/github.com/frobware/grawler)
[![Coverage Status](http://codecov.io/github/frobware/grawler/coverage.svg?branch=master)](http://codecov.io/github/frobware/grawler?branch=master)
[![Report Card](https://goreportcard.com/badge/github.com/frobware/grawler)](https://goreportcard.com/report/github.com/frobware/grawler)

# Web Crawler

A webcrawler library written in Go.

## Installation

	$ go get -u golang.org/x/net/html
	$ go get -u github.com/frobware/grawler/...

The binary `sitemap` is an example of using the library.

Given a URL it will print a basic sitemap for the given domain,
listing the links each page has, together with a list of assets found
on each page. At the moment only `img`, `script` and `link` elements
are considered an asset. `sitemap` will also only download links from
the same domain. And downloads are, by default, concurrent, governed
by the `-j <N>` argument.

	$ sitemap -j 42 http://gopl.io

```json
{
  "URL": "http://gopl.io",
  "Links": [
	"http://www.informit.com/store/go-programming-language-9780134190440",
	"http://www.amazon.com/dp/0134190440",
	"http://www.barnesandnoble.com/w/1121601944",
	"http://gopl.io/ch1.pdf",
	"https://github.com/adonovan/gopl.io/",
	"http://gopl.io/reviews.html",
	"http://gopl.io/translations.html",
	"http://gopl.io/errata.html",
	"http://golang.org/s/oracle-user-manual",
	"http://golang.org/lib/godoc/analysis/help.html",
	"https://github.com/golang/tools/blob/master/refactor/eg/eg.go",
	"https://github.com/golang/tools/blob/master/refactor/rename/rename.go",
	"http://www.amazon.com/dp/0131103628?tracking_id=disfordig-20",
	"http://www.amazon.com/dp/020161586X?tracking_id=disfordig-20"
  ],
  "Assets": [
	"style.css",
	"cover.png",
	"buyfromamazon.png",
	"informit.png",
	"barnesnoble.png"
  ]
},
{
  "URL": "http://gopl.io/errata.html",
  "Links": [
	"https://github.com/golang/proposal/blob/master/design/12416-cgo-pointers.md"
  ],
  "Assets": [
	"style.css"
  ]
},
{
  "URL": "http://gopl.io/reviews.html",
  "Links": [
	"https://www.usenix.org/system/files/login/articles/login_dec15_17_books.pdf",
	"http://lpar.ath0.com/2015/12/03/review-go-programming-language-book",
	"http://www.computingreviews.com/index_dynamic.cfm?CFID=15675338\u0026CFTOKEN=37047869",
	"http://www.infoq.com/articles/the-go-programming-language-book-review",
	"http://www.onebigfluke.com/2016/03/book-review-go-programming-language.html",
	"http://eli.thegreenplace.net/2016/book-review-the-go-programming-language-by-alan-donovan-and-brian-kernighan",
	"http://www.amazon.com/Programming-Language-Addison-Wesley-Professional-Computing/product-reviews/0134190440/ref=cm_cr_dp_see_all_summary"
  ],
  "Assets": [
	"style.css",
	"5stars.png"
  ]
},
{
  "URL": "http://gopl.io/translations.html",
  "Links": [
	"http://www.acornpub.co.kr/book/go-programming",
	"http://www.williamspublishing.com/Books/978-5-8459-2051-5.html",
	"http://helion.pl/ksiazki/jezyk-go-poznaj-i-programuj-alan-a-a-donovan-brian-w-kernighan,jgopop.htm",
	"http://helion.pl/",
	"http://www.amazon.co.jp/exec/obidos/ASIN/4621300253",
	"http://www.maruzen.co.jp/corp/en/services/publishing.html",
	"http://novatec.com.br/",
	"http://www.gotop.com.tw/",
	"http://www.pearsonapac.com/"
  ],
  "Assets": [
	"style.css"
  ]
},
{
  "URL": "http://gopl.io/ch1.pdf"
}
```

## Crawling several sites at once

`CrawlHTTP` takes several seed URLs and schedules every physical GET
under both a global worker limit and a per-origin limit
(`scheme`/`host`/effective port). `MinOriginInterval` additionally
forces a quiet period at each origin between the end of one response
and the start of the next request. Because the limits are tracked per
origin, an origin waiting out its interval never blocks another
origin, and redirect hops — including hops that cross to a different
host — are scheduled through the same gates instead of being followed
transparently by `net/http`.

```go
pages, err := grawler.CrawlHTTP(
    []string{
        "http://example.com/index.html",
        "http://example.org/start.html",
    },
    grawler.HTTPOptions{
        MaxWorkers:        8,
        MaxPerOrigin:      2,
        MinOriginInterval: 250 * time.Millisecond,
    },
    func(URL string) bool {
        // Ordinary discovered links must pass this filter; seeds and
        // redirect destinations bypass it.
        return true
    },
)
if err != nil {
    log.Fatal(err)
}
for _, page := range pages {
    if page.FetcherError != nil {
        log.Printf("%s failed: %v", page.URL, page.FetcherError)
        continue
    }
    fmt.Println(page.URL, page.Links)
}
```

Each accepted seed or discovered link yields one `Page` carrying its
own logical URL, even when several entries converge (through redirect
chains or ordinary links) on the same physical document: that
document is fetched only once per call and its terminal content or
error is shared. Redirect-only intermediates produce no `Page`,
relative links are resolved against the final response URL, and a
failure on one page never stops the independent pages. The `sitemap`
command exposes the same scheduling through `-per-origin` and
`-origin-gap`, keeping `-j` and its JSON output:

	$ sitemap -j 8 -per-origin 2 -origin-gap 250ms http://gopl.io
