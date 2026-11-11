package main

import (
	"fmt"
	"net/url"
	"strings"

	"github.com/go-shiori/dom"
	readability "github.com/go-shiori/go-readability"
)

func main() {
	paragraph := strings.Repeat("The captured research article explains the measurements, the sources, and the results in enough detail for normal extraction. ", 12)
	source := `<html><head><title>A research article with citations</title></head><body><nav>Navigation only</nav><article><h1>A research article with citations</h1><p>` + paragraph + `<a role="doc-noteref" href="#fn1">[1]</a></p><p>` + paragraph + `</p></article><footer><ol role="doc-endnotes"><li role="doc-footnote" id="fn1">NOTE_ONE source record.</li><li role="doc-footnote" id="fn2">NOTE_UNUSED must not be rescued.</li></ol></footer></body></html>`
	page, _ := url.Parse("https://capture.invalid/paper")

	// Default configuration: footnotes stay dropped, exactly as before.
	article, err := readability.FromReader(strings.NewReader(source), page)
	if err != nil {
		panic(err)
	}
	if !strings.Contains(article.Content, `href="#fn1"`) {
		panic("citation missing")
	}
	if strings.Contains(article.Content, "NOTE_ONE") {
		panic("note unexpectedly kept")
	}
	if !strings.Contains(article.TextContent, "measurements") {
		panic("body missing")
	}
	parser := readability.NewParser()
	doc, err := dom.Parse(strings.NewReader(source))
	if err != nil {
		panic(err)
	}
	other, err := parser.ParseDocument(doc, page)
	if err != nil || other.Content != article.Content {
		panic("entry mismatch")
	}
	fmt.Println("BASELINE OK: main body and #fn1 citation retained; footer note absent; reader/document agree")

	// KeepFootnotes enabled: the referenced standalone footnote is
	// restored at the end of the article, the unused one is not.
	keepParser := readability.NewParser()
	keepParser.KeepFootnotes = true

	kept, err := keepParser.Parse(strings.NewReader(source), page)
	if err != nil {
		panic(err)
	}
	if !strings.Contains(kept.Content, `id="fn1"`) || !strings.Contains(kept.Content, "NOTE_ONE") {
		panic("referenced footnote not restored")
	}
	if strings.Contains(kept.Content, "NOTE_UNUSED") {
		panic("unreferenced footnote unexpectedly restored")
	}
	if !strings.Contains(kept.Content, `href="#fn1"`) {
		panic("citation missing after restore")
	}
	if !strings.Contains(kept.TextContent, "NOTE_ONE") {
		panic("restored footnote missing from text content")
	}
	if kept.Length != len([]rune(kept.TextContent)) {
		panic("length does not match text content")
	}
	if kept.Excerpt != other.Excerpt {
		panic("excerpt should still come from the main body")
	}

	keepDoc, err := dom.Parse(strings.NewReader(source))
	if err != nil {
		panic(err)
	}
	keptFromDoc, err := keepParser.ParseDocument(keepDoc, page)
	if err != nil {
		panic(err)
	}
	if keptFromDoc.Content != kept.Content {
		panic("reader/document mismatch with KeepFootnotes")
	}
	if strings.Contains(dom.OuterHTML(keepDoc), `id="fn1"`) == false {
		panic("caller document was modified")
	}
	fmt.Println("KEEPFOOTNOTES OK: referenced #fn1 footnote restored; NOTE_UNUSED absent; reader/document agree; caller tree untouched")
}
