package browser_test

import (
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"

	"github.com/headzoo/surf"
	"github.com/headzoo/surf/browser"
	"golang.org/x/text/encoding/charmap"
)

// roundTripFunc dispatches requests to an http.Handler without opening a
// network socket. Real applications just use the default transport.
type roundTripFunc func(*http.Request) (*http.Response, error)

func (fn roundTripFunc) RoundTrip(req *http.Request) (*http.Response, error) {
	return fn(req)
}

// charsetHandler serves hand-written Windows-1252 bytes and records the
// urlencoded form body it receives.
func charsetHandler(capturedBody *string) http.Handler {
	// 0xe9 is "é" encoded in Windows-1252.
	page := []byte(`<!doctype html><html><head><title>caf` + "\xe9" + `</title></head>` +
		`<body><form method="post" action="/submit">` +
		`<input type="text" name="q" value="caf` + "\xe9" + `">` +
		`<input type="hidden" name="_charset_" value="ignored">` +
		`</form></body></html>`)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/":
			w.Header().Set("Content-Type", "text/html; charset=windows-1252")
			w.Write(page)
		default:
			body, _ := io.ReadAll(r.Body)
			if capturedBody != nil {
				*capturedBody = string(body)
			}
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			fmt.Fprint(w, "<!doctype html><title>ok</title>")
		}
	})
}

func dispatch(handler http.Handler) roundTripFunc {
	return func(req *http.Request) (*http.Response, error) {
		rec := httptest.NewRecorder()
		handler.ServeHTTP(rec, req)
		res := rec.Result()
		res.Request = req
		return res, nil
	}
}

// ExampleBrowser_DocumentCharset shows how a legacy charset page is decoded
// into a Unicode DOM while DocumentCharset reports the charset that was used.
func ExampleBrowser_DocumentCharset() {
	bow := surf.NewBrowser()
	bow.SetTransport(dispatch(charsetHandler(nil)))

	if err := bow.Open("http://example.test/"); err != nil {
		panic(err)
	}
	fmt.Println(bow.DocumentCharset())
	fmt.Println(bow.Title())
	// Output:
	// windows-1252
	// café
}

// ExampleForm_charsetSubmission shows a form read from a Windows-1252 page
// being submitted back in the same charset. Field names and values are
// transcoded before percent-encoding, and the hidden _charset_ control
// reports the canonical charset name without changing Form.Get.
func ExampleForm_charsetSubmission() {
	var capturedBody string
	bow := surf.NewBrowser()
	bow.SetTransport(dispatch(charsetHandler(&capturedBody)))

	if err := bow.Open("http://example.test/"); err != nil {
		panic(err)
	}
	form, err := bow.Form("form")
	if err != nil {
		panic(err)
	}
	if err := form.Input("q", "café"); err != nil {
		panic(err)
	}
	if err := form.Submit(); err != nil {
		panic(err)
	}

	// 0xe9 is the Windows-1252 byte for "é".
	fmt.Println(capturedBody)
	fmt.Println(strings.Join(form.(*browser.Form).Get("_charset_"), ","))

	// A Windows-1252 server decodes the submitted bytes back to Unicode.
	values, _ := url.ParseQuery(capturedBody)
	decoded, err := charmap.Windows1252.NewDecoder().String(values.Get("q"))
	if err != nil {
		panic(err)
	}
	fmt.Println(decoded)
	// Output:
	// _charset_=windows-1252&q=caf%E9
	// ignored
	// café
}
