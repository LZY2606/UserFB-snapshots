# GRequests

GRequests provides a clean wrapper around Go's `net/http` package.  It mimics the convenience of the Python Requests library while keeping the power and safety of Go.

[![Join the chat at https://gitter.im/levigross/grequests](https://badges.gitter.im/Join%20Chat.svg)](https://gitter.im/levigross/grequests?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge&utm_content=badge)

## Features

- Simple helpers for every HTTP verb
- Context aware request functions for easy cancellation
- RequestOptions for headers, query parameters, proxies, cookies and more
- Built in support for JSON and XML responses
- File uploads and convenient download helpers
- Session type for reusing cookies between requests

## Installation

```bash
go get github.com/levigross/grequests/v2
```

## Quick start

```go
package main

import (
    "context"
    "log"

    "github.com/levigross/grequests/v2"
)

func main() {
    resp, err := grequests.Get(context.Background(), "https://httpbin.org/get",
        grequests.UserAgent("MyAgent"))
    if err != nil {
        log.Fatal(err)
    }

    var data map[string]any
    if err := resp.JSON(&data); err != nil {
        log.Fatal(err)
    }
    log.Println(data)
}
```

### Uploading a file

```go
fd, _ := grequests.FileUploadFromDisk("testdata/file.txt")
ro := &grequests.RequestOptions{
    Files: fd,
    Data:  map[string]string{"desc": "test"},
}
resp, err := grequests.Post(context.Background(), "https://httpbin.org/post",
    grequests.FromRequestOptions(ro))
```

### Using a session

```go
sess := grequests.NewSession(nil)
_, _ = sess.Get(context.Background(), "https://httpbin.org/cookies/set?one=two", nil)
resp, _ := sess.Get(context.Background(), "https://httpbin.org/cookies", nil)
log.Println(resp.String())
```

See the documentation for a full list of available options.

### Redirect policies and custom redirect callbacks

Every request gets its own redirect policy, even when requests share a
`Session`. The session only provides defaults - a `RedirectLimit` or
`SensitiveHTTPHeaders` value set on an individual `RequestOptions` overrides
the session default for that request alone, and the caller's
`RequestOptions` is never modified:

```go
sess := grequests.NewSession(&grequests.RequestOptions{RedirectLimit: 10})

// Returns the 302 response itself, without following it.
resp, _ := sess.Get(context.Background(), "https://example.com/start",
    &grequests.RequestOptions{RedirectLimit: -1})

// The next request on the same session is unaffected and follows up to
// 4 redirects. The underlying connection pool and cookie jar are shared.
resp, _ = sess.Get(context.Background(), "https://example.com/start",
    &grequests.RequestOptions{RedirectLimit: 4})
```

`RedirectLimit` keeps its usual meaning: a negative value returns the first
redirect response, zero falls back to the global `RequestRedirectLimit`,
and a positive value limits the number of followed redirects.

If you supply your own `http.Client` with a `CheckRedirect` callback, the
library policy runs first (limit check and sensitive header handling) and
your callback is only invoked when the library allows the redirect. Your
client is wrapped in a shallow per-request copy - its `CheckRedirect`,
`Transport`, `Jar` and `Timeout` are never permanently changed:

```go
client := &http.Client{
    CheckRedirect: func(req *http.Request, via []*http.Request) error {
        if len(via) >= 2 {
            return http.ErrUseLastResponse
        }
        return nil
    },
}
resp, err := grequests.Get(context.Background(), "https://example.com/start",
    grequests.HTTPClient(client), grequests.RedirectLimit(5))
```

## License

GRequests is licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
