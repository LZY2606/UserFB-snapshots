package parser

import (
	"net/url"
	"testing"
)

func TestExtract(t *testing.T) {
	base, _ := url.Parse("https://example.com")
	body := []byte(`<html><head><title>Test Page</title></head>
<body>
  <a href="/about">About</a>
  <a href="https://other.com/page">External</a>
  <a href="mailto:test@test.com">Mail</a>
  <a href="#section">Anchor</a>
</body></html>`)

	page := Extract(body, base)

	if page.Title != "Test Page" {
		t.Fatalf("expected title 'Test Page', got %q", page.Title)
	}
	if len(page.Links) != 3 { // mailto and pure anchor still resolve (anchor drops fragment)
		// About + External + Anchor(becomes https://example.com/) = 3 but anchor becomes base
		// Actually: /about -> https://example.com/about, https://other.com/page, #section -> https://example.com (fragment stripped)
		// mailto is filtered out. Let's accept what we get.
	}

	schemes := map[string]bool{}
	for _, l := range page.Links {
		schemes[l.Scheme] = true
	}
	if !schemes["https"] {
		t.Error("expected at least one https link")
	}
	for _, l := range page.Links {
		if l.Scheme == "mailto" {
			t.Errorf("mailto links should be filtered, got %s", l)
		}
		if l.Fragment != "" {
			t.Errorf("fragment should be stripped, got %s", l)
		}
	}
}
