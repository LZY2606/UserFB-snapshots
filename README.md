Agouti
======

[![Build Status](https://api.travis-ci.org/sclevine/agouti.png?branch=master)](http://travis-ci.org/sclevine/agouti)
[![GoDoc](https://godoc.org/github.com/sclevine/agouti?status.svg)](https://godoc.org/github.com/sclevine/agouti)

**NOTE: Agouti is no longer actively maintained. This was one of my earliest Go projects, and I am no longer happy with the API design. I recommend selecting an alternative WebDriver client for Go.**

Agouti is a library for writing browser-based acceptance tests in Google Go. It provides [Gomega](https://github.com/onsi/gomega) matchers and plays nicely with [Ginkgo](https://github.com/onsi/ginkgo) or [Spec](https://github.com/sclevine/spec). See [agouti.org](http://agouti.org) and the [GoDoc](https://godoc.org/github.com/sclevine/agouti) for documentation. Have questions? Check out the [Agouti mailing list](https://groups.google.com/d/forum/agouti) or the #agouti IRC channel on Freenode.

The [integration tests](https://github.com/sclevine/agouti/blob/master/internal/integration/) are a great place to see everything in action and get started quickly!

Tracked-origin Reset
--------------------

By default, `page.Reset()` only clears cookies and Web Storage for the domain
the browser is currently on, so a flow that crosses sites leaves state behind.
Calling `page.EnableOriginReset()` opts the session into origin tracking:
navigation (including redirects, history traversal, refreshes, and element
clicks/submits) records each visited origin, and `page.Reset()` then clears
cookies, local storage, and session storage for every recorded origin in
first-observed order before navigating to `about:blank`:

```go
page, err := agouti.NewPage("http://localhost:9515")
if err != nil {
	log.Fatal(err)
}
defer page.Destroy()

if err := page.EnableOriginReset(); err != nil {
	log.Fatal(err)
}

// A multi-origin form flow: order on one site, pay on another.
if err := page.Navigate("https://shop.example.com/order"); err != nil {
	log.Fatal(err)
}
if err := page.Find("#sku").Fill("item-42"); err != nil {
	log.Fatal(err)
}
if err := page.Find("#checkout").Click(); err != nil { // redirects to the payment origin
	log.Fatal(err)
}
if err := page.Find("#confirm").Submit(); err != nil {
	log.Fatal(err)
}

// Reset visits each observed origin root and clears its cookies,
// localStorage, and sessionStorage. Note that visiting an origin root may
// execute page code and may fail or redirect; only origins already observed
// during the flow are cleanup targets.
if err := page.Reset(); err != nil {
	// Cleanup stopped at the first failing origin. The remaining
	// origins stay pending and the next Reset resumes there.
	log.Printf("reset incomplete, still pending: %v (%s)", page.OriginResetPending(), err)
}
```

`page.OriginResetPending()` returns an independent copy of the origins still
waiting to be cleaned, so it can be inspected at any time without disturbing
progress. Without `EnableOriginReset()`, `Reset()` keeps its original
current-domain-only behavior.

<p align="center"><a href=http://agouti.org><img src="http://agouti.org/images/agouti_small.png" /></a></p>
