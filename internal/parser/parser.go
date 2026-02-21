// Package parser extracts links and metadata from HTML pages.
package parser

import (
	"bytes"
	"net/url"
	"strings"

	"golang.org/x/net/html"
)

// Page holds data extracted from an HTML document.
type Page struct {
	Title string
	Links []*url.URL
	Text  string // semantic markdown-like plain text
}

// Extract parses the HTML body and returns links resolved against base.
func Extract(body []byte, base *url.URL) Page {
	doc, err := html.Parse(bytes.NewReader(body))
	if err != nil {
		return Page{}
	}

	p := Page{}
	var walk func(*html.Node)
	walk = func(n *html.Node) {
		if n.Type == html.ElementNode {
			switch n.Data {
			case "title":
				if n.FirstChild != nil {
					p.Title = strings.TrimSpace(n.FirstChild.Data)
				}
			case "a":
				if href := attr(n, "href"); href != "" {
					if u := resolve(base, href); u != nil {
						p.Links = append(p.Links, u)
					}
				}
			}
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			walk(c)
		}
	}
	walk(doc)

	p.Text = extractMarkdown(doc)
	return p
}

// extractMarkdown converts an HTML document to a markdown-like plain text
// representation: headings become # / ## / etc., paragraphs become plain text.
func extractMarkdown(doc *html.Node) string {
	var sb strings.Builder
	walkBlocks(doc, &sb)
	return strings.TrimSpace(sb.String())
}

// skip lists elements whose content should be omitted entirely.
var skipTags = map[string]bool{
	"script": true, "style": true, "head": true,
	"noscript": true, "svg": true, "canvas": true,
}

// walkBlocks writes markdown-formatted content to sb by walking the DOM.
func walkBlocks(n *html.Node, sb *strings.Builder) {
	if n.Type == html.ElementNode {
		if skipTags[n.Data] {
			return
		}
		switch n.Data {
		case "h1", "h2", "h3", "h4", "h5", "h6":
			level := int(n.Data[1] - '0')
			text := normalizeSpace(inlineText(n))
			if text != "" {
				sb.WriteString(strings.Repeat("#", level))
				sb.WriteByte(' ')
				sb.WriteString(text)
				sb.WriteByte('\n')
			}
			return
		case "p":
			text := normalizeSpace(inlineText(n))
			if text != "" {
				sb.WriteString(text)
				sb.WriteByte('\n')
			}
			return
		case "li":
			text := normalizeSpace(inlineText(n))
			if text != "" {
				sb.WriteString(text)
				sb.WriteByte('\n')
			}
			return
		case "br":
			sb.WriteByte('\n')
			return
		}
	}
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		walkBlocks(c, sb)
	}
}

// inlineText collects all visible text from n as a flat string.
func inlineText(n *html.Node) string {
	if n.Type == html.TextNode {
		return n.Data
	}
	if n.Type != html.ElementNode || skipTags[n.Data] {
		return ""
	}
	var sb strings.Builder
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		sb.WriteString(inlineText(c))
	}
	return sb.String()
}

// normalizeSpace collapses runs of whitespace to a single space and trims.
func normalizeSpace(s string) string {
	return strings.Join(strings.Fields(s), " ")
}

func attr(n *html.Node, key string) string {
	for _, a := range n.Attr {
		if a.Key == key {
			return strings.TrimSpace(a.Val)
		}
	}
	return ""
}

func resolve(base *url.URL, href string) *url.URL {
	ref, err := url.Parse(href)
	if err != nil {
		return nil
	}
	resolved := base.ResolveReference(ref)

	// Only follow http/https
	if resolved.Scheme != "http" && resolved.Scheme != "https" {
		return nil
	}
	// Drop fragment
	resolved.Fragment = ""
	return resolved
}
