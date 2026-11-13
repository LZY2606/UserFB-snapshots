package main

import (
	"fmt"
	surf "github.com/headzoo/surf"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
)

func main() {
	var posts int
	s := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/":
			io.WriteString(w, `<html><head><title>source</title></head><body><iframe name="result" src="/old"></iframe><form method="POST" action="/submit" target="result"><input name="station" value="7"></form></body></html>`)
		case "/submit":
			if r.Method != "POST" {
				panic("method")
			}
			if err := r.ParseForm(); err != nil {
				panic(err)
			}
			if r.Form.Get("station") != "7" {
				panic("field")
			}
			posts++
			http.Redirect(w, r, "/receipt", http.StatusSeeOther)
		case "/receipt":
			io.WriteString(w, `<html><head><title>receipt</title></head><body>record=7</body></html>`)
		case "/old":
			io.WriteString(w, `<html><head><title>old-result</title></head><body>old</body></html>`)
		default:
			http.NotFound(w, r)
		}
	}))
	defer s.Close()
	b := surf.NewBrowser()
	if err := b.Open(s.URL); err != nil {
		panic(err)
	}
	form, err := b.Form("form")
	if err != nil {
		panic(err)
	}
	if err = form.Submit(); err != nil {
		panic(err)
	}
	if posts != 1 {
		panic("post count")
	}
	fmt.Printf("SOURCE_TITLE_AFTER_TARGET_SUBMIT=%s POST_COUNT=%d\n", b.Title(), posts)
	if !strings.Contains(b.Body(), "record=7") {
		panic("missing receipt")
	}
	fmt.Println("BASELINE_OK real Surf form POST and redirect; no frame implementation in support")
}
