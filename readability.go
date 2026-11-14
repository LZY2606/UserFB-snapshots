// Package readability is a Go package that find the main readable
// content from a HTML page. It works by removing clutter like buttons,
// ads, background images, script, etc.
//
// This package is based from Readability.js by Mozilla, and written line
// by line to make sure it looks and works as similar as possible. This
// way, hopefully all web page that can be parsed by Readability.js
// are parse-able by go-readability as well.
package readability

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	nurl "net/url"
	"strings"
	"time"

	"github.com/go-shiori/dom"
	"golang.org/x/net/html"
)

// FromReader parses an `io.Reader` and returns the readable content. It's the wrapper
// or `Parser.Parse()` and useful if you only want to use the default parser.
func FromReader(input io.Reader, pageURL *nurl.URL) (Article, error) {
	parser := NewParser()
	return parser.Parse(input, pageURL)
}

// FromDocument parses an document and returns the readable content. It's the wrapper
// or `Parser.ParseDocument()` and useful if you only want to use the default parser.
func FromDocument(doc *html.Node, pageURL *nurl.URL) (Article, error) {
	parser := NewParser()
	return parser.ParseDocument(doc, pageURL)
}

type RequestWith func(r *http.Request)

// FromURL fetch the web page from specified url then parses the response to find
// the readable content. Relative references are resolved against the final
// response URL (the address after any redirect), itself possibly overridden
// by the first parseable HTTP(S) head/base[href], exactly like FromURLPages.
func FromURL(pageURL string, timeout time.Duration, requestModifiers ...RequestWith) (Article, error) {
	// Make sure URL is valid
	parsedURL, err := nurl.ParseRequestURI(pageURL)
	if err != nil {
		return Article{}, fmt.Errorf("failed to parse URL: %v", err)
	}

	// Fetch page from URL
	client := &http.Client{Timeout: timeout}
	req, err := http.NewRequest("GET", pageURL, nil)
	if err != nil {
		return Article{}, fmt.Errorf("failed to fetch the page: %v", err)
	}
	for _, modifier := range requestModifiers {
		modifier(req)
	}
	resp, err := client.Do(req)
	if err != nil {
		return Article{}, fmt.Errorf("failed to fetch the page: %v", err)
	}
	defer resp.Body.Close()

	// Make sure content type is HTML
	cp := resp.Header.Get("Content-Type")
	if !strings.Contains(cp, "text/html") {
		return Article{}, fmt.Errorf("URL is not a HTML document")
	}

	// Read the body so the document can both be inspected for its base
	// element and parsed by the readability parser.
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return Article{}, fmt.Errorf("failed to read the page: %v", err)
	}

	doc, err := dom.Parse(bytes.NewReader(body))
	if err != nil {
		return Article{}, fmt.Errorf("failed to parse input: %v", err)
	}

	// The redirected response URL is the page identity, and the first
	// parseable HTTP(S) base element resolves against it.
	finalURL := cloneURL(parsedURL)
	if resp.Request != nil && resp.Request.URL != nil {
		finalURL = cloneURL(resp.Request.URL)
	}
	baseURL := effectiveBaseURL(doc, finalURL)

	// Parse content
	parser := NewParser()
	return parser.ParseDocument(doc, baseURL)
}

// Check checks whether the input is readable without parsing the whole thing. It's the
// wrapper for `Parser.Check()` and useful if you only use the default parser.
func Check(input io.Reader) bool {
	parser := NewParser()
	return parser.Check(input)
}

// CheckDocument checks whether the document is readable without parsing the whole thing.
// It's the wrapper for `Parser.CheckDocument()` and useful if you only use the default
// parser.
func CheckDocument(doc *html.Node) bool {
	parser := NewParser()
	return parser.CheckDocument(doc)
}
