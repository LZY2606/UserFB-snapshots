# Open Graph Parser for Golang

Go implementation of https://ogp.me/

[![reference](https://pkg.go.dev/badge/github.com/otiai10/opengraph)](https://pkg.go.dev/github.com/otiai10/opengraph/v2)
[![Go](https://github.com/otiai10/opengraph/workflows/Go/badge.svg)](https://github.com/otiai10/opengraph/actions)
[![codecov](https://codecov.io/gh/otiai10/opengraph/branch/main/graph/badge.svg?token=D4mPKqi9fH)](https://codecov.io/gh/otiai10/opengraph)
[![Maintainability](https://api.codeclimate.com/v1/badges/d063131639023e9a8900/maintainability)](https://codeclimate.com/github/otiai10/opengraph/maintainability)
[![Go Report Card](https://goreportcard.com/badge/github.com/otiai10/opengraph)](https://goreportcard.com/report/github.com/otiai10/opengraph)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](https://github.com/otiai10/opengraph/blob/main/LICENSE)
[![FOSSA Status](https://app.fossa.com/api/projects/git%2Bgithub.com%2Fotiai10%2Fopengraph.svg?type=shield)](https://app.fossa.com/projects/git%2Bgithub.com%2Fotiai10%2Fopengraph?ref=badge_shield)
[![GitHub tag (latest SemVer)](https://img.shields.io/github/v/tag/otiai10/opengraph?sort=semver)](https://pkg.go.dev/github.com/otiai10/opengraph/v2)

# Code Example

```go
package main

import (
	"fmt"
	"github.com/otiai10/opengraph/v2"
)

func main() {
	ogp, err := opengraph.Fetch("https://github.com/")
	fmt.Println(ogp, err)
}
```

# You can try CLI as a working example

```sh
% go install github.com/otiai10/opengraph/v2/cmd/ogp@latest
% ogp --help
% ogp -A otiai10.com
```

You can also use custom headers (useful for sites with bot protection):
```sh
% ogp -H "User-Agent: Mozilla/5.0" -H "Accept-Language: en-US" https://example.com
```

Just for fun 😉

# Advanced usage

Set an option for fetching:
```go
intent := opengraph.Intent{
	Context:     ctx,
	HTTPClient:  client,
	Strict:      true,
	TrustedTags: []string{"meta", "title"},
	Headers:     map[string]string{
		"User-Agent": "MyCustomBot/1.0",
		"Accept-Language": "en-US,en;q=0.9",
	},
}
ogp, err := opengraph.Fetch("https://ogp.me", intent)
```

Use any `io.Reader` as a data source:

```go
f, _ := os.Open("my_test.html")
defer f.Close()
ogp := &opengraph.OpenGraph{}
err := ogp.Parse(f)
```

or if you already have parsed `*html.Node`:

```go
err := ogp.Walk(node)
```

Do you wanna make absolute URLs?:
```go
ogp.Image[0].URL // /logo.png
ogp.ToAbs()
ogp.Image[0].URL // https://ogp.me/logo.png
```

# Media collection: repeated roots, aliases and structured properties

Every bare `og:image`, `og:video` or `og:audio` property opens a new record of
that type, in page order. Two roots pointing at the same URL are therefore
kept as separate records, each with its own structured properties:

```html
<meta property="og:image" content="cover.jpg">
<meta property="og:image:url" content="cover.jpg">
<meta property="og:image:width" content="1080">
<meta property="og:image:alt" content="wide banner">
<meta property="og:image" content="cover.jpg">
<meta property="og:image:width" content="200">
<meta property="og:image:alt" content="thumbnail">
```

collects two images with the same URL but independent attributes:

```go
ogp.Image[0] // {URL:"cover.jpg", Width:1080, Alt:"wide banner"}
ogp.Image[1] // {URL:"cover.jpg", Width:200,  Alt:"thumbnail"}
```

The `og:image:url` (and `og:video:url` / `og:audio:url`) form only counts as
an alias of the preceding bare root when it follows that root immediately,
with no other meta tag in between, and carries exactly the same content.
Otherwise it opens a record of its own. Structured properties bind to the
nearest root of their own type; properties appearing before any root are
ignored. The three media types keep independent cursors, so interleaved meta
tags never change each other's binding. When the same sub-property appears
several times, the latest valid value wins.

`width`, `height` and `duration` only accept non-negative decimal integers
that fit into `int`. A bad value returns a `*NumericPropertyError` carrying
the property name and the raw content; the previous value of that field is
kept, and `Parse`/`Walk` stops at the first error while everything collected
earlier stays available:

```go
err := ogp.Parse(reader)
var numErr *opengraph.NumericPropertyError
if errors.As(err, &numErr) {
    fmt.Println(numErr.Property, numErr.Content) // e.g. og:image:width oops
}
```

# Absolute URLs and document base

`ToAbs` resolves both ordinary and secure URLs of every media record with the
relative-reference rules of `net/url`, covering relative paths, queries,
fragments and scheme-relative (`//host/...`) URLs. The base is chosen per
parsed page:

1. the first valid HTTP(S) `<base href>` in that tree (a relative base is
   resolved against `Intent.URL`);
2. otherwise `Intent.URL`;
3. only when both are missing, the page's `og:url`.

An `og:url` therefore never moves collected media onto a different host. When
several pages are parsed into the same `OpenGraph` and `ToAbs` is called once
at the end, every record is expanded against its own page base. Empty fields
stay empty, already absolute URLs keep their meaning, and repeated `ToAbs`
calls do not change the result.

# Issues

- https://github.com/otiai10/opengraph/issues
