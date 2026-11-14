rss
=====

 [![GoDoc](https://godoc.org/github.com/SlyMarbo/rss?status.svg)](https://pkg.go.dev/github.com/SlyMarbo/rss/v2)

RSS is a small library for simplifying the parsing of RSS and Atom feeds.
The package conforms to the [RSS 1.0], [RSS 2.0], and [Atom 1.0] specifications.

If you encounter any problems with feeds being parsed incorrectly, please
open an issue on GitHub.

Example usage:

```go
package main

import (
	"context"

	"github.com/SlyMarbo/rss/v2"
)

func main() {
	reader, err := rss.NewReader()
	if err != nil {
		// handle error.
	}

	feed, err := reader.Fetch(context.Background(), "https://example.com/rss")
	if err != nil {
		// handle error.
	}

	// ... Some time later ...

	err = reader.UpdatePatiently(feed)
	if err != nil {
		// handle error.
	}
}
```

The library does its best to follow the appropriate specifications and not to set the `NextUpdate` time
too soon. It currently follows all update time management methods in the RSS 1.0, 2.0, and Atom 1.0
specifications. If one is not provided, it defaults to 12 hour intervals. If you are having issues
with feed providors dropping connections, use `WithDefaultUpdateInterval` to set the default update
interval.

Atom archive capture
--------------------

Atom feeds that keep only recent entries can link older entries through the RFC 5005 `prev-archive`
chain. Use `Reader.CaptureHistory` to collect the subscription entry and everything reachable through
that chain:

```go
reader, err := rss.NewReader()
if err != nil {
	// handle error.
}

ctx := context.Background()
history, err := reader.CaptureHistory(ctx, "https://example.com/atom", nil, "full")
if err != nil {
	// handle error.
}

fmt.Println(history.Feed.Title, len(history.Feed.Items))
for _, document := range history.Documents {
	fmt.Println(document)
}

// Later, reuse successfully fetched archives and follow the chain
// from where it ended last time.
next, err := reader.CaptureHistory(ctx, "https://example.com/atom", history, "incremental")
if err != nil {
	// handle error.
}
```

`mode` accepts `"full"` and `"incremental"`. A full capture fetches the entry and the entire archive
chain again. An incremental capture still fetches the entry unconditionally, but may reuse archives
from the previous capture that were fetched successfully, carry the `fh:archive` marker, and are still
reachable from the entry. Only the entry and the archives reached in the current capture contribute to
the result, so archives that have dropped off the chain are not merged in.

`history.Documents` lists the final response URLs of the documents that contributed, from the entry
to the oldest archive. Entries are merged by `atom:id`: when an ID occurs in more than one document,
the document with the newer feed-level `updated` time wins, and the entry keeps the position of its
first occurrence. Items that were read in `previous.Feed` stay read, new items are unread, and
`Feed.Unread` is recalculated. Channel metadata comes from the entry document.

CaptureHistory performs a fresh, unconditional GET and never follows `next`, `next-archive`, or
`current` links. It returns an error, leaving the previous result untouched, if any document returns a
non-200 status, cannot be read or closed, has an invalid RFC 3339 feed-level `updated` time, or if the
chain requests the same address twice or forms a cycle. Only document links that are direct children
of the feed and in the Atom namespace are considered; `href` values are resolved from the document's
final response URL, applying the feed's and then the link's `xml:base`.

The project is not proactively maintained, but I'll respond to issues and PRs as soon as I can.

[Atom 1.0]: https://datatracker.ietf.org/doc/html/rfc4287
[RSS 1.0]: https://web.resource.org/rss/1.0/spec
[RSS 2.0]: https://www.rssboard.org/rss-specification
