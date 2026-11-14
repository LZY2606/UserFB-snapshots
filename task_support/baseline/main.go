package main

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"github.com/headzoo/surf"
	"github.com/headzoo/surf/browser"
)

func main() {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/submit" {
			if err := r.ParseForm(); err != nil { panic(err) }
			fmt.Fprintf(w, "<title>receipt</title><body>name=%s</body>", r.Form.Get("name"))
			return
		}
		fmt.Fprint(w, `<title>application</title><form method="post" action="/submit"><input name="name" value="seed"></form>`)
	}))
	defer ts.Close()
	b := surf.NewBrowser()
	b.SetAttribute(browser.MetaRefreshHandling, false)
	if err := b.Open(ts.URL); err != nil { panic(err) }
	legacy := b.NewTab()
	if legacy.State() != b.State() || legacy.HistoryJar() != b.HistoryJar() { panic("legacy sharing changed") }
	f, err := b.Form("form"); if err != nil { panic(err) }
	if err = f.Input("name", "baseline"); err != nil { panic(err) }
	if err = f.Submit(); err != nil { panic(err) }
	if b.Title() != "receipt" || b.Find("body").Text() != "name=baseline" { panic("form submission failed") }
	fmt.Println("BASELINE_OK form POST name=baseline; NewTab State and History shared")
}
