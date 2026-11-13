package browser_test

import (
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"

	surf "github.com/headzoo/surf"
)

// ExampleBrowser_Frame walks a two-level iframe portal: the root document
// holds the navigation frame, that frame holds the query form, and a named
// sibling "result" frame receives the form response.
//
// Three rules make the frame workflow predictable:
//
//   - Lazy loading: Frame performs its HTTP request only the first time an
//     element is selected. Selecting the same element again returns the same
//     *browser.Browser without another request, and elements never passed to
//     Frame are never downloaded.
//   - Target addressing: a form target resolves only among contexts already
//     open in the same frame tree. "_self", "_parent" and "_top" are
//     case-insensitive keywords, while ordinary names are case-sensitive and
//     must match exactly one context. Unknown or duplicated names, and
//     "_blank", fail with browser.ErrFrameTarget instead of guessing.
//   - Handle lifetime: replacing any context's document permanently detaches
//     its old descendants. The stale handles still expose their old DOM
//     snapshot, but any request from them returns browser.ErrFrameDetached;
//     a same-name frame in the new document is a brand new handle.
func Example_frameWorkflow() {
	portal := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/":
			io.WriteString(w, `<html><head><title>portal</title></head><body>`+
				`<iframe name="nav" src="/nav"></iframe>`+
				`<iframe name="result" src="/result/empty"></iframe>`+
				`</body></html>`)
		case "/nav":
			io.WriteString(w, `<html><head><title>nav</title></head><body>`+
				`<iframe name="query" src="/query"></iframe></body></html>`)
		case "/query":
			io.WriteString(w, `<html><head><title>query</title></head><body>`+
				`<form method="POST" action="/search" target="result">`+
				`<input name="station" value="7">`+
				`</form></body></html>`)
		case "/search":
			http.Redirect(w, r, "/result/7", http.StatusSeeOther)
		case "/result/empty":
			io.WriteString(w, `<html><head><title>empty</title></head><body></body></html>`)
		case "/result/7":
			io.WriteString(w, `<html><head><title>result</title></head><body>`+
				`<p id="record">record=7</p></body></html>`)
		}
	}))
	defer portal.Close()

	bow := surf.NewBrowser()
	if err := bow.Open(portal.URL + "/"); err != nil {
		panic(err)
	}

	// First Frame call opens the navigation document through the normal
	// request/DOM pipeline; repeating it returns the same context, no request.
	nav, err := bow.Frame(`iframe[name="nav"]`)
	if err != nil {
		panic(err)
	}
	if same, err := bow.Frame(`iframe[name="nav"]`); err != nil || same != nav {
		panic("same frame element must return the same Browser")
	}

	// Nested frames are opened from the context that contains them.
	query, err := nav.Frame(`iframe[name="query"]`)
	if err != nil {
		panic(err)
	}

	// The result frame is a sibling of nav, addressed by its element name.
	result, err := bow.Frame(`iframe[name="result"]`)
	if err != nil {
		panic(err)
	}

	form, err := query.Form("form")
	if err != nil {
		panic(err)
	}
	if err := form.Input("station", "7"); err != nil {
		panic(err)
	}
	// The response lands in the named result frame; the query document and
	// every other context keep their current pages.
	if err := form.Submit(); err != nil {
		panic(err)
	}

	fmt.Println(strings.TrimSpace(result.Title()))
	fmt.Println(strings.TrimSpace(result.Find("#record").Text()))
	fmt.Println(strings.TrimSpace(query.Title()))

	// After the root navigates, the old descendants are detached: reading the
	// snapshot still works, but issuing a request fails with ErrFrameDetached.
	if err := bow.Open(portal.URL + "/"); err != nil {
		panic(err)
	}
	fmt.Println(query.Title()) // old DOM snapshot remains readable
	if err := query.Reload(); err != nil {
		fmt.Println("reload rejected")
	}
	// Output:
	// result
	// record=7
	// query
	// query
	// reload rejected
}
