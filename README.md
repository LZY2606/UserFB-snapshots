
[![Go Documentation](http://godoc.org/github.com/raff/godet?status.svg)](http://godoc.org/github.com/raff/godet)
[![Go Report Card](https://goreportcard.com/badge/github.com/raff/godet)](https://goreportcard.com/report/github.com/raff/godet)
[![Actions Status](https://github.com/raff/godet/workflows/Go/badge.svg)](https://github.com/raff/godet/actions)


# godet
Remote client for Chrome DevTools

## Installation

    $ go get github.com/raff/godet
    
## Documentation
http://godoc.org/github.com/raff/godet

## Example
A pretty complete example is available at [`cmd/godet/main.go`](https://github.com/raff/godet/blob/master/cmd/godet/main.go).
This example is available at [`examples/example.go`](https://github.com/raff/godet/blob/master/examples/example.go).

```go
import "github.com/raff/godet"

// connect to Chrome instance
remote, err := godet.Connect("localhost:9222", true)
if err != nil {
    fmt.Println("cannot connect to Chrome instance:", err)
    return
}

// disconnect when done
defer remote.Close()

// get browser and protocol version
version, _ := remote.Version()
fmt.Println(version)

// get list of open tabs
tabs, _ := remote.TabList("")
fmt.Println(tabs)

// install some callbacks
remote.CallbackEvent(godet.EventClosed, func(params godet.Params) {
    fmt.Println("RemoteDebugger connection terminated.")
})

remote.CallbackEvent("Network.requestWillBeSent", func(params godet.Params) {
    fmt.Println("requestWillBeSent",
        params["type"],
        params["documentURL"],
        params["request"].(map[string]interface{})["url"])
})

remote.CallbackEvent("Network.responseReceived", func(params godet.Params) {
    fmt.Println("responseReceived",
        params["type"],
        params["response"].(map[string]interface{})["url"])
})

remote.CallbackEvent("Log.entryAdded", func(params godet.Params) {
    entry := params["entry"].(map[string]interface{})
    fmt.Println("LOG", entry["type"], entry["level"], entry["text"])
})

// block loading of most images
_ = remote.SetBlockedURLs("*.jpg", "*.png", "*.gif")

// create new tab
tab, _ := remote.NewTab("https://www.google.com")
fmt.Println(tab)

// enable event processing
remote.RuntimeEvents(true)
remote.NetworkEvents(true)
remote.PageEvents(true)
remote.DOMEvents(true)
remote.LogEvents(true)

// navigate in existing tab
_ = remote.ActivateTab(tabs[0])

// re-enable events when changing active tab
remote.AllEvents(true) // enable all events

_, _ = remote.Navigate("https://www.google.com")

// evaluate Javascript expression in existing context
res, _ := remote.EvaluateWrap(`
    console.log("hello from godet!")
    return 42;
`)
fmt.Println(res)

// take a screenshot
_ = remote.SaveScreenshot("screenshot.png", 0644, 0, true)

// or save page as PDF
_ = remote.SavePDF("page.pdf", 0644)

```

## Waiting for navigation to complete

`Navigate` returns as soon as Chrome acknowledges the `Page.navigate` command —
it does not mean the new document is ready to drive. `NavigateAndWait` blocks
until the new document, identified by the `frameId` and `loaderId` in the
`Page.navigate` reply, reaches the requested lifecycle stage. Enable Page
events first; the method does not change domain configuration and does not
switch tabs.

```go
import (
    "context"
    "log"

    "github.com/raff/godet"
)

remote, err := godet.Connect("localhost:9222", false)
if err != nil {
    log.Fatal(err)
}
defer remote.Close()

// Page lifecycle events are required for NavigateAndWait.
if err := remote.PageEvents(true); err != nil {
    log.Fatal(err)
}

// Only continue once the next page fires "load".
result, err := remote.NavigateAndWait(
    context.Background(),
    "https://example.com/form/page/2",
    godet.NoTransition,
    godet.LifecycleLoad,
)
if err != nil {
    log.Fatal(err)
}
log.Printf("new document: frame=%s loader=%s sameDocument=%t",
    result.FrameID, result.LoaderID, result.SameDocument)

// The new document is ready: fill the form on the next page.
if _, err := remote.Evaluate(`document.querySelector("#field").value = "42"`); err != nil {
    log.Fatal(err)
}
```

`event` must be `godet.LifecycleDOMContentLoaded` (`"DOMContentLoaded"`) or
`godet.LifecycleLoad` (`"load"`); any other value is rejected before the
command is sent. A reply containing `frameId` but no `loaderId` denotes a
same-document navigation and returns `NavigationResult{SameDocument: true}`
immediately, without waiting for new-document events; a reply containing
`errorText` is returned as a `NavigationError`.

The `context.Context` covers the whole call, including the wait for the
command reply. Canceling it (or letting a deadline expire) removes the pending
request and returns an error that `errors.Is(err, godet.ErrNavigationCanceled)`
recognizes. The navigation itself is not undone, but its late reply and
duplicate events are discarded and cannot block the read loop or complete a
later call. `Close` and websocket disconnects finish pending waits with
`godet.ErrNavigationClosed`. Concurrent calls each track their own reply and
loader, so canceling one never affects another.
