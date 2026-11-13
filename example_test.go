package opengraph

import (
	"fmt"
	"strings"
)

func ExampleFetch() {
	ogp, err := Fetch("https://ogp.me/")
	fmt.Println("title:", ogp.Title)
	fmt.Println("type:", ogp.Type)
	fmt.Println("error:", err)
	// Output:
	// title: Open Graph protocol
	// type: website
	// error: <nil>
}

// ExampleOpenGraph_mediaCollection shows that image roots sharing the same URL
// are still kept as separate records, each carrying its own structured
// properties. An immediately following og:image:url with identical content is
// treated as an alias of the root and does not open another record.
func ExampleOpenGraph_mediaCollection() {
	ogp := New("https://example.com/posts/1")
	page := strings.NewReader(`
<meta property="og:image" content="cover.jpg">
<meta property="og:image:url" content="cover.jpg">
<meta property="og:image:width" content="1080">
<meta property="og:image:alt" content="wide banner">
<meta property="og:image" content="cover.jpg">
<meta property="og:image:width" content="200">
<meta property="og:image:alt" content="thumbnail">`)
	if err := ogp.Parse(page); err != nil {
		fmt.Println("error:", err)
		return
	}
	for i, img := range ogp.Image {
		fmt.Printf("%d: url=%s width=%d alt=%s\n", i, img.URL, img.Width, img.Alt)
	}
	if err := ogp.ToAbs(); err != nil {
		fmt.Println("error:", err)
		return
	}
	fmt.Println("abs:", ogp.Image[0].URL, ogp.Image[1].URL)
	// Output:
	// 0: url=cover.jpg width=1080 alt=wide banner
	// 1: url=cover.jpg width=200 alt=thumbnail
	// abs: https://example.com/posts/cover.jpg https://example.com/posts/cover.jpg
}

// ExampleNumericPropertyError shows that malformed numeric properties stop the
// parse with an error naming the property and its raw content, while values
// collected earlier remain available.
func ExampleNumericPropertyError() {
	ogp := New("https://example.com/")
	page := strings.NewReader(`
<meta property="og:image" content="cover.jpg">
<meta property="og:image:width" content="1080">
<meta property="og:image:width" content="not-a-number">
<meta property="og:title" content="never reached">`)
	err := ogp.Parse(page)
	fmt.Println("error:", err)
	fmt.Printf("kept: url=%s width=%d title=%q\n", ogp.Image[0].URL, ogp.Image[0].Width, ogp.Title)
	// Output:
	// error: opengraph: invalid non-negative integer for og:image:width: "not-a-number"
	// kept: url=cover.jpg width=1080 title=""
}
