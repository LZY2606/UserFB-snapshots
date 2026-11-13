# Go-Readability [![Go Reference][go-ref-badge]][go-ref] [![PayPal][paypal-badge]][paypal] [![Ko-fi][kofi-badge]][kofi]

Go-Readability is a Go package that find the main readable content and the metadata from a HTML page. It works by removing clutter like buttons, ads, background images, script, etc.

This package is based from [Readability.js] by [Mozilla] and written line by line to make sure it looks and works as similar as possible. This way, hopefully all web page that can be parsed by Readability.js are parse-able by go-readability as well.

> [!WARNING]
> This package is deprecated in favor of [codeberg.org/readeck/go-readability/v2](https://codeberg.org/readeck/go-readability/src/branch/v2).

## Table of Contents

- [Table of Contents](#table-of-contents)
- [Status](#status)
- [Installation](#installation)
- [Example](#example)
- [Debugging the Content Selection](#debugging-the-content-selection)
- [Command Line Usage](#command-line-usage)
- [Licenses](#licenses)

## Status

This package is stable enough for use and up to date with [Readability.js v0.5.0][last-version].

For compatibility with Readability.js v0.6, use [codeberg.org/readeck/go-readability/v2](https://codeberg.org/readeck/go-readability/src/branch/v2).

## Installation

To install this package, just run `go get` :

```
go get -u -v github.com/go-shiori/go-readability
```

## Example

To get the readable content from an URL, you can use `readability.FromURL`. It will fetch the web page from specified url, check if it's readable, then parses the response to find the readable content :

```go
package main

import (
	"fmt"
	"log"
	"os"
	"time"

	readability "github.com/go-shiori/go-readability"
)

var (
	urls = []string{
		// this one is article, so it's parse-able
		"https://www.nytimes.com/2019/02/20/climate/climate-national-security-threat.html",
		// while this one is not an article, so readability will fail to parse.
		"https://www.nytimes.com/",
	}
)

func main() {
	for i, url := range urls {
		article, err := readability.FromURL(url, 30*time.Second)
		if err != nil {
			log.Fatalf("failed to parse %s, %v\n", url, err)
		}

		dstTxtFile, _ := os.Create(fmt.Sprintf("text-%02d.txt", i+1))
		defer dstTxtFile.Close()
		dstTxtFile.WriteString(article.TextContent)

		dstHTMLFile, _ := os.Create(fmt.Sprintf("html-%02d.html", i+1))
		defer dstHTMLFile.Close()
		dstHTMLFile.WriteString(article.Content)

		fmt.Printf("URL     : %s\n", url)
		fmt.Printf("Title   : %s\n", article.Title)
		fmt.Printf("Author  : %s\n", article.Byline)
		fmt.Printf("Length  : %d\n", article.Length)
		fmt.Printf("Excerpt : %s\n", article.Excerpt)
		fmt.Printf("SiteName: %s\n", article.SiteName)
		fmt.Printf("Image   : %s\n", article.Image)
		fmt.Printf("Favicon : %s\n", article.Favicon)
		fmt.Printf("Text content saved to \"text-%02d.txt\"\n", i+1)
		fmt.Printf("HTML content saved to \"html-%02d.html\"\n", i+1)
		fmt.Println()
	}
}
```

However, sometimes you want to parse an URL no matter if it's an article or not. For example is when you only want to get metadata of the page. To do that, you have to download the page manually using `http.Get`, then parse it using `readability.FromReader` :

```go
package main

import (
	"fmt"
	"log"
	"net/http"
	"net/url"

	readability "github.com/go-shiori/go-readability"
)

var (
	urls = []string{
		// Both will be parse-able now
		"https://www.nytimes.com/2019/02/20/climate/climate-national-security-threat.html",
		// But this one will not have any content
		"https://www.nytimes.com/",
	}
)

func main() {
	for _, u := range urls {
		resp, err := http.Get(u)
		if err != nil {
			log.Fatalf("failed to download %s: %v\n", u, err)
		}
		defer resp.Body.Close()

		parsedURL, err := url.Parse(u)
		if err != nil {
			log.Fatalf("error parsing url")
		}

		article, err := readability.FromReader(resp.Body, parsedURL)
		if err != nil {
			log.Fatalf("failed to parse %s: %v\n", u, err)
		}

		fmt.Printf("URL     : %s\n", u)
		fmt.Printf("Title   : %s\n", article.Title)
		fmt.Printf("Author  : %s\n", article.Byline)
		fmt.Printf("Length  : %d\n", article.Length)
		fmt.Printf("Excerpt : %s\n", article.Excerpt)
		fmt.Printf("SiteName: %s\n", article.SiteName)
		fmt.Printf("Image   : %s\n", article.Image)
		fmt.Printf("Favicon : %s\n", article.Favicon)
		fmt.Println()
	}
}

```

## Debugging the Content Selection

When the parser picks the wrong element as the main content, you can set `Parser.ExplainSelection` to record a structured `SelectionTrace` while the selection happens. The trace identifies every element of the original document by a stable ID and a path, and lists every score write (initialization, paragraph contribution, link-density scaling) of every attempt, so you can see exactly which paragraph gave score to which ancestor and which attempt the returned content came from. The original tree and the output HTML are never modified by the tracing.

The example below shows how to resolve a contribution event back to the original element in your own document tree:

```go
package main

import (
	"fmt"
	"log"
	"net/url"
	"strings"

	readability "github.com/go-shiori/go-readability"
	"golang.org/x/net/html"
)

// resolvePath walks the original document tree following the child
// indexes recorded in a SelectionNode path.
func resolvePath(root *html.Node, path []int) *html.Node {
	cur := root
	for _, idx := range path {
		var next *html.Node
		i := 0
		for child := cur.FirstChild; child != nil; child = child.NextSibling {
			if i == idx {
				next = child
				break
			}
			i++
		}
		if next == nil {
			return nil
		}
		cur = next
	}
	return cur
}

func main() {
	raw := `<html><body><div class="article"><p>First paragraph, long enough to be scored.</p></div></body></html>`

	// Keep the parsed document around: the trace refers to its elements.
	doc, err := html.Parse(strings.NewReader(raw))
	if err != nil {
		log.Fatal(err)
	}

	parser := readability.NewParser()
	parser.ExplainSelection = true
	pageURL, _ := url.Parse("https://example.invalid/article")
	article, err := parser.ParseDocument(doc, pageURL)
	if err != nil {
		log.Fatal(err)
	}

	trace := article.SelectionTrace
	if trace == nil {
		log.Fatal("selection tracing was not enabled")
	}

	// Index the node definitions by ID.
	nodes := make(map[int]readability.SelectionNode, len(trace.Nodes))
	for _, n := range trace.Nodes {
		nodes[n.ID] = n
	}

	attempt := trace.Attempts[trace.ChosenAttempt]
	for _, ev := range attempt.Events {
		if ev.Kind != readability.SelectionEventContribution {
			continue
		}

		// Find the original paragraph that produced this contribution.
		src := nodes[ev.SourceID]
		if src.Synthetic {
			fmt.Printf("contribution came from a parser-created node #%d\n", src.ID)
			continue
		}

		el := resolvePath(doc, src.Path)
		tag := ""
		if el != nil && el.Type == html.ElementNode {
			tag = el.Data
		}
		fmt.Printf("paragraph #%d (<%s> at %v) added %.4f to node #%d\n",
			src.ID, tag, src.Path, ev.Operand, ev.NodeID)
	}
}
```

## Command Line Usage

You can also use `go-readability` as command line app. To do that, first install the CLI :

```
go install github.com/go-shiori/go-readability/cmd/go-readability@latest
```

Now you can use it by running `go-readability` in your terminal :

```
$ go-readability -h

go-readability is parser to fetch the readable content of a web page.
The source can be an url or existing file in your storage.

Usage:
  go-readability [flags] source

Flags:
  -h, --help          help for go-readability
  -l, --http string   start the http server at the specified address
  -m, --metadata      only print the page's metadata
  -t, --text          only print the page's text
```

## Licenses

Go-Readability is distributed under [MIT license][mit], which means you can use and modify it however you want. However, if you make an enhancement for it, if possible, please send a pull request. If you like this project, please consider donating to me either via [PayPal][paypal] or [Ko-Fi][kofi].

[go-ref]: https://pkg.go.dev/github.com/go-shiori/go-readability
[go-ref-badge]: https://img.shields.io/static/v1?label=&message=Reference&color=007d9c&logo=go&logoColor=white
[paypal]: https://www.paypal.me/RadhiFadlillah
[paypal-badge]: https://img.shields.io/static/v1?label=&message=PayPal&color=00457C&logo=paypal&logoColor=white
[kofi]: https://ko-fi.com/radhifadlillah
[kofi-badge]: https://img.shields.io/static/v1?label=&message=Ko-fi&color=F16061&logo=ko-fi&logoColor=white
[readability.js]: https://github.com/mozilla/readability
[mozilla]: https://github.com/mozilla
[last-version]: https://github.com/mozilla/readability/tree/0.5.0
[mit]: https://choosealicense.com/licenses/mit/
