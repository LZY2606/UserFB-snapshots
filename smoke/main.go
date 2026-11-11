package main

import (
    "fmt"
    "net/url"
    "strings"
    readability "github.com/go-shiori/go-readability"
    "github.com/go-shiori/dom"
)

func main() {
    paragraph := strings.Repeat("The captured research article explains the measurements, the sources, and the results in enough detail for normal extraction. ", 12)
    source := `<html><head><title>A research article with citations</title></head><body><nav>Navigation only</nav><article><h1>A research article with citations</h1><p>` + paragraph + `<a role="doc-noteref" href="#fn1">[1]</a></p><p>` + paragraph + `</p></article><footer><ol role="doc-endnotes"><li role="doc-footnote" id="fn1">NOTE_ONE source record.</li><li role="doc-footnote" id="fn2">NOTE_UNUSED must not be rescued.</li></ol></footer></body></html>`
    page, _ := url.Parse("https://capture.invalid/paper")
    article, err := readability.FromReader(strings.NewReader(source), page)
    if err != nil { panic(err) }
    if !strings.Contains(article.Content, `href="#fn1"`) { panic("citation missing") }
    if strings.Contains(article.Content, "NOTE_ONE") { panic("note unexpectedly kept") }
    if !strings.Contains(article.TextContent, "measurements") { panic("body missing") }
    parser := readability.NewParser()
    doc, err := dom.Parse(strings.NewReader(source))
    if err != nil { panic(err) }
    other, err := parser.ParseDocument(doc, page)
    if err != nil || other.Content != article.Content { panic("entry mismatch") }
    fmt.Println("BASELINE OK: main body and #fn1 citation retained; footer note absent; reader/document agree")
}
