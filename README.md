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

### WebSub subscriptions

Feeds that advertise a [WebSub] hub — a `rel="hub"` link alongside a
`rel="self"` link — can be monitored with push notifications in
addition to polling. `Reader.NewSubscription` builds a `Subscription`
from a fetched feed and a callback base URL; the subscription is an
`http.Handler` that receives the hub's verification requests and
notifications:

```go
feed, err := reader.Fetch(ctx, "https://example.com/rss")
if err != nil {
	// handle error.
}

sub, err := reader.NewSubscription(feed, "https://subscriber.example/callbacks")
if err != nil {
	// handle error.
}

// Route requests under the callback base URL to sub, then subscribe.
if err := sub.Subscribe(ctx); err != nil {
	// handle error.
}
```

`Subscribe` is also used to renew the lease; each call generates a
fresh callback path and secret, and a failed renewal never revokes
the previous lease. Notifications revise items in place by ID and
append new ones, but pushes generally only carry new or changed
items, so deletions are found by `Resync`, which replaces the state
with a full fetch of the topic and then replays any notifications
received while the fetch was in flight. `Snapshot` returns a detached
copy of the current state and `SetRead` tracks read state locally.

The project is not proactively maintained, but I'll respond to issues and PRs as soon as I can.

[Atom 1.0]: https://datatracker.ietf.org/doc/html/rfc4287
[RSS 1.0]: https://web.resource.org/rss/1.0/spec
[RSS 2.0]: https://www.rssboard.org/rss-specification
[WebSub]: https://www.w3.org/TR/websub/
