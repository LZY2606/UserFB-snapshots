package opengraph

import (
	"reflect"
	"strings"
	"testing"

	"golang.org/x/net/html"
)

// TestMediaCollectionGrouping covers root/alias handling, interleaved media
// kinds, orphan properties, repeated sub-properties and the full field sets
// of image, video and audio records. Every case goes through Parse on real
// HTML.
func TestMediaCollectionGrouping(t *testing.T) {
	page := `<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta property="og:image:width" content="999">
<meta property="og:image" content="same.jpg">
<meta property="og:image:url" content="same.jpg">
<meta property="og:image:width" content="100">
<meta property="og:image:height" content="200">
<meta property="og:image:alt" content="first">
<meta property="og:image:type" content="image/jpeg">
<meta property="og:image:secure_url" content="https://cdn.example.com/secure/same.jpg">
<meta property="og:audio" content="song.mp3">
<meta property="og:audio:type" content="audio/mpeg">
<meta property="og:audio:secure_url" content="https://cdn.example.com/secure/song.mp3">
<meta property="og:image" content="same.jpg">
<meta property="og:image:width" content="300">
<meta property="og:image:height" content="400">
<meta property="og:image:alt" content="second">
<meta property="og:video" content="clip.mp4">
<meta property="og:video:url" content="clip.mp4">
<meta property="og:video:type" content="video/mp4">
<meta property="og:video:width" content="640">
<meta property="og:video:height" content="480">
<meta property="og:video:duration" content="12">
<meta property="og:image:url" content="same.jpg">
<meta property="og:image:alt" content="third">
<meta property="og:image:width" content="700">
<meta property="og:image:width" content="800">
</head>
</html>`

	t.Run("same URL roots stay separate with their own properties", func(t *testing.T) {
		og := New("https://example.com/dir/page.html")
		if err := og.Parse(strings.NewReader(page)); err != nil {
			t.Fatalf("Parse returned error: %v", err)
		}

		wantImages := []Image{
			{
				URL:       "same.jpg",
				SecureURL: "https://cdn.example.com/secure/same.jpg",
				Type:      "image/jpeg",
				Width:     100,
				Height:    200,
				Alt:       "first",
			},
			{
				URL:    "same.jpg",
				Width:  300,
				Height: 400,
				Alt:    "second",
			},
			{
				URL:   "same.jpg",
				Width: 800,
				Alt:   "third",
			},
		}
		if !reflect.DeepEqual(og.Image, wantImages) {
			t.Fatalf("images mismatch:\n got  %+v\n want %+v", og.Image, wantImages)
		}

		wantAudio := []Audio{
			{
				URL:       "song.mp3",
				SecureURL: "https://cdn.example.com/secure/song.mp3",
				Type:      "audio/mpeg",
			},
		}
		if !reflect.DeepEqual(og.Audio, wantAudio) {
			t.Fatalf("audio mismatch:\n got  %+v\n want %+v", og.Audio, wantAudio)
		}

		wantVideo := []Video{
			{
				URL:      "clip.mp4",
				Type:     "video/mp4",
				Width:    640,
				Height:   480,
				Duration: 12,
			},
		}
		if !reflect.DeepEqual(og.Video, wantVideo) {
			t.Fatalf("video mismatch:\n got  %+v\n want %+v", og.Video, wantVideo)
		}
	})

	t.Run("Walk and Parse collect identically", func(t *testing.T) {
		parsed := New("https://example.com/dir/page.html")
		if err := parsed.Parse(strings.NewReader(page)); err != nil {
			t.Fatalf("Parse returned error: %v", err)
		}

		node, err := html.Parse(strings.NewReader(page))
		if err != nil {
			t.Fatalf("html.Parse returned error: %v", err)
		}
		walked := New("https://example.com/dir/page.html")
		if err := walked.Walk(node); err != nil {
			t.Fatalf("Walk returned error: %v", err)
		}

		if !reflect.DeepEqual(parsed.Image, walked.Image) {
			t.Fatalf("image mismatch: Parse=%+v Walk=%+v", parsed.Image, walked.Image)
		}
		if !reflect.DeepEqual(parsed.Audio, walked.Audio) {
			t.Fatalf("audio mismatch: Parse=%+v Walk=%+v", parsed.Audio, walked.Audio)
		}
		if !reflect.DeepEqual(parsed.Video, walked.Video) {
			t.Fatalf("video mismatch: Parse=%+v Walk=%+v", parsed.Video, walked.Video)
		}
	})

	t.Run("strict mode keeps trusting meta tags only", func(t *testing.T) {
		strictPage := `<!DOCTYPE html><html><head>
<title>title tag must be ignored in strict mode</title>
<meta name="description" content="name description must be ignored in strict mode">
<meta property="og:title" content="strict title">
<meta property="og:image" content="strict.jpg">
<meta property="og:image:alt" content="strict alt">
</head></html>`
		og := New("https://example.com/")
		og.Intent.Strict = true
		if err := og.Parse(strings.NewReader(strictPage)); err != nil {
			t.Fatalf("Parse returned error: %v", err)
		}
		if og.Title != "strict title" {
			t.Fatalf("strict title not collected: %q", og.Title)
		}
		if og.Description != "" {
			t.Fatalf("non-meta description leaked into strict parse: %q", og.Description)
		}
		if len(og.Image) != 1 || og.Image[0].Alt != "strict alt" {
			t.Fatalf("strict image collection wrong: %+v", og.Image)
		}
	})
}

// TestMediaCollectionNumericErrorsAndCrossPage checks that bad numeric values
// surface as errors carrying property and raw content, that processing stops
// at the first error while keeping earlier data, and that a second page starts
// with fresh root bindings while previously collected arrays are preserved.
func TestMediaCollectionNumericErrorsAndCrossPage(t *testing.T) {
	t.Run("first error stops the walk and keeps earlier values", func(t *testing.T) {
		page := `<!DOCTYPE html><html><head>
<meta property="og:url" content="https://elsewhere.example.com/canonical">
<meta property="og:image" content="a.jpg">
<meta property="og:image:width" content="100">
<meta property="og:image:width" content="oops">
<meta property="og:title" content="after error">
</head></html>`
		og := New("https://one.example.com/path/page1.html")
		err := og.Parse(strings.NewReader(page))
		if err == nil {
			t.Fatal("expected error for non-numeric width, got nil")
		}
		numErr, ok := err.(*NumericPropertyError)
		if !ok {
			t.Fatalf("expected *NumericPropertyError, got %T: %v", err, err)
		}
		if numErr.Property != "og:image:width" || numErr.Content != "oops" {
			t.Fatalf("error does not carry property and content: %+v", numErr)
		}
		if len(og.Image) != 1 || og.Image[0].URL != "a.jpg" || og.Image[0].Width != 100 {
			t.Fatalf("earlier image data not preserved: %+v", og.Image)
		}
		if og.Title != "" {
			t.Fatalf("nodes after the error must not be processed, title=%q", og.Title)
		}
	})

	t.Run("negative and out-of-range integers are rejected", func(t *testing.T) {
		cases := map[string]string{
			"og:image:width":    "-1",
			"og:video:height":   "99999999999999999999999",
			"og:video:duration": "1.5",
			"og:image:height":   "",
		}
		for property, content := range cases {
			og := New("https://example.com/")
			htmlPage := `<meta property="og:image" content="x.jpg">` +
				`<meta property="og:video" content="x.mp4">` +
				`<meta property="` + property + `" content="` + content + `">`
			err := og.Parse(strings.NewReader(htmlPage))
			if err == nil {
				t.Fatalf("expected error for %s=%q", property, content)
			}
			numErr, ok := err.(*NumericPropertyError)
			if !ok {
				t.Fatalf("%s=%s: expected *NumericPropertyError, got %T", property, content, err)
			}
			if numErr.Property != property || numErr.Content != content {
				t.Fatalf("%s=%s: wrong error payload %+v", property, content, numErr)
			}
		}
	})

	t.Run("second page resets bindings but keeps collected arrays", func(t *testing.T) {
		og := New("https://one.example.com/path/page1.html")
		page1 := `<!DOCTYPE html><html><head>
<meta property="og:url" content="https://elsewhere.example.com/canonical">
<meta property="og:image" content="a.jpg">
<meta property="og:image:width" content="100">
<meta property="og:audio" content="a.mp3">
</head></html>`
		if err := og.Parse(strings.NewReader(page1)); err != nil {
			t.Fatalf("page1 Parse error: %v", err)
		}

		// The second page starts with an orphan property: it must not bind to
		// the last record of page one.
		og.Intent.URL = "https://two.example.com/x/page2.html"
		page2 := `<!DOCTYPE html><html><head>
<meta property="og:image:width" content="50">
<meta property="og:image" content="b.jpg">
<meta property="og:image:height" content="60">
<meta property="og:video" content="b.mp4">
</head></html>`
		if err := og.Parse(strings.NewReader(page2)); err != nil {
			t.Fatalf("page2 Parse error: %v", err)
		}

		if len(og.Image) != 2 {
			t.Fatalf("expected two images across pages, got %+v", og.Image)
		}
		if og.Image[0].Width != 100 {
			t.Fatalf("page one width changed: %+v", og.Image[0])
		}
		if og.Image[1].URL != "b.jpg" || og.Image[1].Width != 0 || og.Image[1].Height != 60 {
			t.Fatalf("orphan width leaked into page two record: %+v", og.Image[1])
		}
		if len(og.Audio) != 1 || og.Audio[0].URL != "a.mp3" {
			t.Fatalf("page one audio lost: %+v", og.Audio)
		}
		if len(og.Video) != 1 || og.Video[0].URL != "b.mp4" {
			t.Fatalf("page two video missing: %+v", og.Video)
		}

		// ToAbs resolves each record against its own page base, and og:url of
		// page one must never move media onto another host.
		if err := og.ToAbs(); err != nil {
			t.Fatalf("ToAbs error: %v", err)
		}
		if got := og.Image[0].URL; got != "https://one.example.com/path/a.jpg" {
			t.Fatalf("page one image resolved against wrong base: %s", got)
		}
		if got := og.Audio[0].URL; got != "https://one.example.com/path/a.mp3" {
			t.Fatalf("page one audio resolved against wrong base: %s", got)
		}
		if got := og.Image[1].URL; got != "https://two.example.com/x/b.jpg" {
			t.Fatalf("page two image resolved against wrong base: %s", got)
		}
		if got := og.Video[0].URL; got != "https://two.example.com/x/b.mp4" {
			t.Fatalf("page two video resolved against wrong base: %s", got)
		}
	})

	t.Run("direct Contribute calls remain one continuous stream", func(t *testing.T) {
		og := New("https://example.com/")
		metas := []Meta{
			{Property: "og:image", Content: "stream.jpg"},
			{Property: "og:image:url", Content: "stream.jpg"},
			{Property: "og:image:width", Content: "120"},
			{Property: "og:video", Content: "stream.mp4"},
			{Property: "og:video:duration", Content: "9"},
			{Property: "og:image:alt", Content: "streamed"},
		}
		for _, m := range metas {
			if err := m.Contribute(og); err != nil {
				t.Fatalf("Contribute error: %v", err)
			}
		}
		if len(og.Image) != 1 {
			t.Fatalf("alias :url opened a second record: %+v", og.Image)
		}
		if og.Image[0].Width != 120 || og.Image[0].Alt != "streamed" {
			t.Fatalf("streamed image properties wrong: %+v", og.Image[0])
		}
		if len(og.Video) != 1 || og.Video[0].Duration != 9 {
			t.Fatalf("streamed video wrong: %+v", og.Video)
		}
	})
}

// TestMediaCollectionURLAndBase covers ordinary and secure URL resolution,
// absolute and relative <base href>, the og:url fallback, net/url
// relative-reference edge cases and ToAbs idempotency.
func TestMediaCollectionURLAndBase(t *testing.T) {
	t.Run("base href, query, fragment and scheme-relative references", func(t *testing.T) {
		page := `<!DOCTYPE html><html><head>
<base href="/rootbase/">
<base href="https://ignored.example.com/never/">
<meta property="og:image" content="img.png?q=1#frag">
<meta property="og:image:secure_url" content="secure.jpg">
<meta property="og:audio" content="//cdn.example.com/song.mp3">
<meta property="og:audio:secure_url" content="https://cdn.example.com/safe-song.mp3">
<meta property="og:video" content="https://video.example.com/clip.mp4">
<meta property="og:video:secure_url" content="">
</head></html>`
		og := New("https://example.com/dir/page.html")
		if err := og.Parse(strings.NewReader(page)); err != nil {
			t.Fatalf("Parse error: %v", err)
		}
		if err := og.ToAbs(); err != nil {
			t.Fatalf("first ToAbs error: %v", err)
		}

		if got := og.Image[0].URL; got != "https://example.com/rootbase/img.png?q=1#frag" {
			t.Fatalf("relative image with query/fragment wrong: %s", got)
		}
		if got := og.Image[0].SecureURL; got != "https://example.com/rootbase/secure.jpg" {
			t.Fatalf("relative secure_url wrong: %s", got)
		}
		if got := og.Audio[0].URL; got != "https://cdn.example.com/song.mp3" {
			t.Fatalf("scheme-relative URL wrong: %s", got)
		}
		if got := og.Audio[0].SecureURL; got != "https://cdn.example.com/safe-song.mp3" {
			t.Fatalf("absolute secure_url changed: %s", got)
		}
		if got := og.Video[0].URL; got != "https://video.example.com/clip.mp4" {
			t.Fatalf("absolute URL changed: %s", got)
		}
		if og.Video[0].SecureURL != "" {
			t.Fatalf("empty secure_url must stay empty: %q", og.Video[0].SecureURL)
		}

		snapshot := *og
		if err := og.ToAbs(); err != nil {
			t.Fatalf("second ToAbs error: %v", err)
		}
		if !reflect.DeepEqual(og.Image, snapshot.Image) ||
			!reflect.DeepEqual(og.Audio, snapshot.Audio) ||
			!reflect.DeepEqual(og.Video, snapshot.Video) {
			t.Fatalf("ToAbs is not idempotent:\n first  %+v %+v %+v\n second %+v %+v %+v",
				snapshot.Image, snapshot.Audio, snapshot.Video,
				og.Image, og.Audio, og.Video)
		}
	})

	t.Run("relative base href resolves against Intent.URL", func(t *testing.T) {
		page := `<!DOCTYPE html><html><head>
<base href="sub/">
<meta property="og:image" content="pic.png">
<meta property="og:image:secure_url" content="../safe/pic.jpg">
</head></html>`
		og := New("https://example.com/a/b/page.html")
		if err := og.Parse(strings.NewReader(page)); err != nil {
			t.Fatalf("Parse error: %v", err)
		}
		if err := og.ToAbs(); err != nil {
			t.Fatalf("ToAbs error: %v", err)
		}
		if got := og.Image[0].URL; got != "https://example.com/a/b/sub/pic.png" {
			t.Fatalf("relative base resolution wrong: %s", got)
		}
		if got := og.Image[0].SecureURL; got != "https://example.com/a/b/safe/pic.jpg" {
			t.Fatalf("secure path traversal wrong: %s", got)
		}
	})

	t.Run("og:url is only the fallback when base and Intent.URL are absent", func(t *testing.T) {
		page := `<!DOCTYPE html><html><head>
<meta property="og:url" content="https://ogurl.example.com/post/1">
<meta property="og:image" content="cover.png">
</head></html>`

		withIntent := New("https://intent.example.com/news/page")
		if err := withIntent.Parse(strings.NewReader(page)); err != nil {
			t.Fatalf("Parse error: %v", err)
		}
		if err := withIntent.ToAbs(); err != nil {
			t.Fatalf("ToAbs error: %v", err)
		}
		if got := withIntent.Image[0].URL; got != "https://intent.example.com/news/cover.png" {
			t.Fatalf("og:url must not override Intent.URL base: %s", got)
		}

		withoutIntent := New("")
		if err := withoutIntent.Parse(strings.NewReader(page)); err != nil {
			t.Fatalf("Parse error: %v", err)
		}
		if err := withoutIntent.ToAbs(); err != nil {
			t.Fatalf("ToAbs error: %v", err)
		}
		if got := withoutIntent.Image[0].URL; got != "https://ogurl.example.com/post/cover.png" {
			t.Fatalf("og:url fallback not used: %s", got)
		}
	})

	t.Run("non-HTTP base href is ignored", func(t *testing.T) {
		page := `<!DOCTYPE html><html><head>
<base href="file:///etc/">
<meta property="og:image" content="cover.png">
</head></html>`
		og := New("https://example.com/dir/page.html")
		if err := og.Parse(strings.NewReader(page)); err != nil {
			t.Fatalf("Parse error: %v", err)
		}
		if err := og.ToAbs(); err != nil {
			t.Fatalf("ToAbs error: %v", err)
		}
		if got := og.Image[0].URL; got != "https://example.com/dir/cover.png" {
			t.Fatalf("file: base should have been ignored: %s", got)
		}
	})
}
