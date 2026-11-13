//go:build baselinecheck

package opengraph

import (
	"fmt"
	"strings"
	"testing"
)

func TestBaseline(t *testing.T) {
	og := New("https://page.invalid/dir/index.html")
	e := og.Parse(strings.NewReader(`<meta property="og:title" content="ok"><meta property="og:image" content="same.jpg"><meta property="og:image:width" content="100"><meta property="og:image" content="same.jpg"><meta property="og:image:width" content="200"><meta property="og:image:alt" content="second">`))
	if e != nil || og.Title != "ok" || len(og.Image) != 1 || og.Image[0].Width != 200 || og.Image[0].Alt != "" {
		t.Fatalf("unexpected baseline: %+v %v", og, e)
	}
	fmt.Printf("baseline groups=%d width=%d alt=%q\n", len(og.Image), og.Image[0].Width, og.Image[0].Alt)
	bad := New("")
	e = bad.Parse(strings.NewReader(`<meta property="og:image" content="a"><meta property="og:image:width" content="100"><meta property="og:image:width" content="oops"><meta property="og:title" content="after">`))
	if e != nil || bad.Image[0].Width != 0 || bad.Title != "after" {
		t.Fatal("unexpected numeric behavior", e, bad)
	}
	fmt.Printf("baseline numeric-error=%v width=%d later-title=%s\n", e, bad.Image[0].Width, bad.Title)
}
