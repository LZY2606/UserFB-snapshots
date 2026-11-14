Agouti
======

[![Build Status](https://api.travis-ci.org/sclevine/agouti.png?branch=master)](http://travis-ci.org/sclevine/agouti)
[![GoDoc](https://godoc.org/github.com/sclevine/agouti?status.svg)](https://godoc.org/github.com/sclevine/agouti)

**NOTE: Agouti is no longer actively maintained. This was one of my earliest Go projects, and I am no longer happy with the API design. I recommend selecting an alternative WebDriver client for Go.**

Agouti is a library for writing browser-based acceptance tests in Google Go. It provides [Gomega](https://github.com/onsi/gomega) matchers and plays nicely with [Ginkgo](https://github.com/onsi/ginkgo) or [Spec](https://github.com/sclevine/spec). See [agouti.org](http://agouti.org) and the [GoDoc](https://godoc.org/github.com/sclevine/agouti) for documentation. Have questions? Check out the [Agouti mailing list](https://groups.google.com/d/forum/agouti) or the #agouti IRC channel on Freenode.

The [integration tests](https://github.com/sclevine/agouti/blob/master/internal/integration/) are a great place to see everything in action and get started quickly!

## Filling fields across nested iframes

`Selection.SwitchToFrame` changes the focus of the whole session, so a saved
`Selection` can silently start addressing a different frame. `Page.InFrames`
returns a `FrameScope` that remembers a path of nested iframes and applies it
afresh, from the root frame, for every terminal operation (`Count`, `Text`,
`Click` and `Fill`). Deriving scopes and selections never sends a request;
each operation returns to the root frame, descends the path one iframe at a
time, runs the field action, and returns to the root again.

```go
page, err := agouti.NewPage("http://example.com")
if err != nil {
    panic(err)
}

// Fill a field two iframes deep: #order on the root page, #contact inside it.
err = page.InFrames("#order", "#contact").Find("input.name").Fill("林")
if err != nil {
    panic(err)
}

// Scopes can be nested further and reuse the same path. The returned
// selections stay ordinary *Selection / *MultiSelection values, so Find,
// All and At continue to carry the same frame path.
order := page.InFrames("#order")
contact := order.InFrames("#contact")

if count, err := contact.All("input.cell").Count(); err != nil {
    panic(err)
} else {
    fmt.Println("cells:", count)
}

if err := contact.All("input.cell").At(0).Click(); err != nil {
    panic(err)
}

// An empty argument list describes the root page itself.
text, err := page.InFrames().Find("#summary").Text()
if err != nil {
    panic(err)
}
fmt.Println(text)
```

If an operation cannot return to the root frame, the Page's frame context is
marked uncertain. Further frame-aware operations then fail with
`ErrFrameContextUnknown` (recognizable by `errors.Is`) without sending a
request, until an explicit `Page.SwitchToRootFrame` call succeeds:

```go
if err := field.Fill("林"); err != nil {
    if errors.Is(err, agouti.ErrFrameContextUnknown) {
        // Recover the frame focus before using this Page again.
        if recoverErr := page.SwitchToRootFrame(); recoverErr != nil {
            panic(recoverErr)
        }
    } else {
        panic(err)
    }
}
```

<p align="center"><a href=http://agouti.org><img src="http://agouti.org/images/agouti_small.png" /></a></p>
