# cascadia

[![](https://travis-ci.org/andybalholm/cascadia.svg)](https://travis-ci.org/andybalholm/cascadia)

The Cascadia package implements CSS selectors for use with the parse trees produced by the html package.

To test CSS selectors without writing Go code, check out [cascadia](https://github.com/suntong/cascadia) the command line tool, a thin wrapper around this package.

[Refer to godoc here](https://godoc.org/github.com/andybalholm/cascadia).

## Example

The following is an example of how you can use Cascadia.

```go
package main

import (
	"fmt"
	"log"
	"strings"

	"github.com/andybalholm/cascadia"
	"golang.org/x/net/html"
)

var pricingHtml string = `
<div class="card mb-4 box-shadow">
	<div class="card-header">
		<h4 class="my-0 font-weight-normal">Free</h4>
	</div>
	<div class="card-body">
		<h1 class="card-title pricing-card-title">$0/mo</h1>
		<ul class="list-unstyled mt-3 mb-4">
			<li>10 users included</li>
			<li>2 GB of storage</li>
			<li><a href="https://example.com">See more</a></li>
		</ul>
	</div>
</div>

<div class="card mb-4 box-shadow">
	<div class="card-header">
		<h4 class="my-0 font-weight-normal">Pro</h4>
	</div>
	<div class="card-body">
		<h1 class="card-title pricing-card-title">$15/mo</h1>
		<ul class="list-unstyled mt-3 mb-4">
			<li>20 users included</li>
			<li>10 GB of storage</li>
			<li><a href="https://example.com">See more</a></li>
		</ul>
	</div>
</div>

<div class="card mb-4 box-shadow">
	<div class="card-header">
		<h4 class="my-0 font-weight-normal">Enterprise</h4>
	</div>
	<div class="card-body">
		<h1 class="card-title pricing-card-title">$29/mo</h1>
		<ul class="list-unstyled mt-3 mb-4">
			<li>30 users included</li>
			<li>15 GB of storage</li>
			<li><a>See more</a></li>
		</ul>
	</div>
</div>
`

func Query(n *html.Node, query string) *html.Node {
	sel, err := cascadia.Parse(query)
	if err != nil {
		return &html.Node{}
	}
	return cascadia.Query(n, sel)
}

func QueryAll(n *html.Node, query string) []*html.Node {
	sel, err := cascadia.Parse(query)
	if err != nil {
		return []*html.Node{}
	}
	return cascadia.QueryAll(n, sel)
}

func AttrOr(n *html.Node, attrName, or string) string {
	for _, a := range n.Attr {
		if a.Key == attrName {
			return a.Val
		}
	}
	return or
}

func main() {
	doc, err := html.Parse(strings.NewReader(pricingHtml))
	if err != nil {
		log.Fatal(err)
	}
	fmt.Printf("List of pricing plans:\n\n")
	for i, p := range QueryAll(doc, "div.card.mb-4.box-shadow") {
		planName := Query(p, "h4").FirstChild.Data
		price := Query(p, ".pricing-card-title").FirstChild.Data
		usersIncluded := Query(p, "li:first-child").FirstChild.Data
		storage := Query(p, "li:nth-child(2)").FirstChild.Data
		detailsUrl := AttrOr(Query(p, "li:last-child a"), "href", "(No link available)")
		fmt.Printf(
			"Plan #%d\nName: %s\nPrice: %s\nUsers: %s\nStorage: %s\nDetails: %s\n\n",
			i+1,
			planName,
			price,
			usersIncluded,
			storage,
			detailsUrl,
		)
	}
}
```
The output is:
```
List of pricing plans:

Plan #1
Name: Free
Price: $0/mo
Users: 10 users included
Storage: 2 GB of storage
Details: https://example.com

Plan #2
Name: Pro
Price: $15/mo
Users: 20 users included
Storage: 10 GB of storage
Details: https://example.com

Plan #3
Name: Enterprise
Price: $29/mo
Users: 30 users included
Storage: 15 GB of storage
Details: (No link available)
```

## Relative selectors: `:has()` and `:haschild()`

The argument of `:has()` (and `:haschild()`) is a comma-separated list of
relative selectors. Each branch is evaluated starting from the element the
pseudo-class is tested on — the anchor — and the anchor is only in charge of
the branch's first step:

- a branch with no leading combinator takes a **descendant** first step
  (`:haschild()` instead defaults to a **child** first step);
- `>` takes a **child** first step;
- `+` takes the **adjacent element sibling** first step;
- `~` takes a **subsequent element sibling** first step.

Whitespace text nodes and comments are not element siblings, so they do not
occupy a `+` or `~` position. The anchor itself and its ancestors can never
be the targets of the descendant or child first step.

After the first step, the rest of the branch is an ordinary selector chain.
Only the first step is constrained to the anchor's subtree: `+` and `~` can
reach the anchor's siblings, and later combinators may follow descendants,
children or siblings from there.

```go
// Matches articles that contain a .flag somewhere underneath them.
// The implicit first step is a descendant search.
`article:has(.flag)`

// Matches only when an h2 is a direct child of the article, and a .flag is
// its descendant. An h2 nested deeper does not satisfy the child step.
`article:has(> h2 .flag)`

// The adjacent sibling may live outside the article's subtree; the trailing
// descendant combinator then searches inside that sibling.
`article:has(+ aside .flag)`

// A two-step chain after the anchored child step: the direct h2 must be
// immediately followed by a paragraph that contains a link.
`article:has(> h2 + p a)`

// :haschild() keeps its implicit child first step, but supports the same
// complex chains after it.
`article:haschild(h2 + p a)`
```

A selector matches when any comma branch succeeds; an element matched by
several branches is still returned only once. The usual compound conditions,
`:not()`, `:is()` and the positional pseudo-classes can be used anywhere in
the chain, and the compiled selector always evaluates against the tree as it
currently stands, so the same compiled object can be reused across documents
and after the document's sibling order changes.
