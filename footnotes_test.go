package readability

import (
	"net/url"
	"strings"
	"testing"
	"unicode/utf8"

	"github.com/go-shiori/dom"
	"golang.org/x/net/html"
)

// longParagraph is a sizable chunk of prose so the synthetic articles
// pass the readability content thresholds.
var longParagraph = strings.Repeat("Long research body text describing measurements, sources and results in enough detail for extraction. ", 10)

var testPageURL = mustParseURL("https://capture.invalid/paper")

func mustParseURL(raw string) *url.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u
}

// parseFootnoteDocument parses source with a KeepFootnotes parser and
// returns the article together with its Content reparsed as a DOM.
func parseFootnoteDocument(t *testing.T, source string, configure func(*Parser)) (Article, *html.Node) {
	t.Helper()

	parser := NewParser()
	parser.KeepFootnotes = true
	if configure != nil {
		configure(&parser)
	}

	article, err := parser.Parse(strings.NewReader(source), testPageURL)
	if err != nil {
		t.Fatalf("failed to parse article: %v", err)
	}

	contentDoc, err := dom.Parse(strings.NewReader(article.Content))
	if err != nil {
		t.Fatalf("failed to reparse article content: %v", err)
	}

	return article, contentDoc
}

// footnoteIDs returns the ids of the restored doc-footnote list items,
// in document order.
func footnoteIDs(doc *html.Node) []string {
	var ids []string
	for _, li := range dom.QuerySelectorAll(doc, `ol > li[role="doc-footnote"]`) {
		ids = append(ids, dom.ID(li))
	}
	return ids
}

// wrapArticle builds a full HTML document with a long article body and
// a footer containing the given footnote markup.
func wrapArticle(bodyExtra, footerNotes string) string {
	return `<html><head><title>A research article with citations</title></head><body>` +
		`<article><h1>A research article with citations</h1>` +
		`<p>` + longParagraph + bodyExtra + `</p>` +
		`</article>` +
		`<footer><ol role="doc-endnotes">` + footerNotes + `</ol></footer>` +
		`</body></html>`
}

// TestKeepFootnotesBasic covers the headline behavior: a referenced
// standalone footnote is restored with its id and text, the unused one
// is not, and Node, Content, TextContent, Length and Excerpt stay
// consistent.
func TestKeepFootnotesBasic(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#fn1">[1]</a>`,
		`<li role="doc-footnote" id="fn1">NOTE_ONE source record.</li>`+
			`<li role="doc-footnote" id="fn2">NOTE_UNUSED must not be rescued.</li>`,
	)

	article, doc := parseFootnoteDocument(t, source, nil)

	notes := dom.QuerySelectorAll(doc, `li[role="doc-footnote"]`)
	if len(notes) != 1 {
		t.Fatalf("want exactly 1 restored footnote, got %d: %v", len(notes), footnoteIDs(doc))
	}
	if dom.ID(notes[0]) != "fn1" {
		t.Errorf("restored footnote id = %q, want fn1", dom.ID(notes[0]))
	}
	if !strings.Contains(dom.TextContent(notes[0]), "NOTE_ONE") {
		t.Errorf("restored footnote text missing NOTE_ONE: %s", dom.OuterHTML(notes[0]))
	}
	if strings.Contains(article.Content, "NOTE_UNUSED") {
		t.Error("unreferenced footnote NOTE_UNUSED must not be restored")
	}
	if !strings.Contains(article.Content, `href="#fn1"`) {
		t.Error("the original citation link must be kept untouched")
	}

	// The restored list is the last element of the single article
	// wrapper, and only the named <li> elements were copied: no footer,
	// no endnotes wrapper.
	wrapper := dom.QuerySelector(doc, `div#readability-page-1`)
	if wrapper == nil {
		t.Fatal("missing readability page wrapper")
	}
	lastChild := wrapper.LastChild
	for lastChild != nil && lastChild.Type != html.ElementNode {
		lastChild = lastChild.PrevSibling
	}
	if lastChild == nil || dom.TagName(lastChild) != "ol" {
		t.Fatalf("last article element must be the footnote <ol>, got %v", lastChild)
	}
	if len(dom.Children(lastChild)) != 1 {
		t.Errorf("footnote list must contain only the referenced li, got %d", len(dom.Children(lastChild)))
	}
	if dom.QuerySelector(doc, "footer") != nil {
		t.Error("the source footer element must not be copied back")
	}

	// Node, Content and TextContent must expose the same restored note.
	if dom.QuerySelector(article.Node, `li#fn1`) == nil {
		t.Error("Article.Node does not contain the restored footnote")
	}
	if !strings.Contains(article.TextContent, "NOTE_ONE") {
		t.Error("Article.TextContent does not contain the restored footnote")
	}
	if article.Length != utf8.RuneCountInString(article.TextContent) {
		t.Errorf("Length = %d, want rune count %d", article.Length, utf8.RuneCountInString(article.TextContent))
	}
	if strings.Contains(article.Excerpt, "NOTE_") {
		t.Errorf("excerpt must come from the main body, got %q", article.Excerpt)
	}
	if !strings.HasPrefix(article.Excerpt, "Long research body text") {
		t.Errorf("unexpected excerpt: %q", article.Excerpt)
	}
}

// TestKeepFootnotesOrderAndDuplicates checks that notes are ordered by
// first reference (including reverse document order) and that a note
// referenced several times is restored only once.
func TestKeepFootnotesOrderAndDuplicates(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#fn3">[3]</a>`+
			`<a role="doc-noteref" href="#fn1">[1]</a>`+
			`<a role="doc-noteref" href="#fn3">[3 again]</a>`+
			`<a rel="footnote" href="#fn2">[2]</a>`,
		`<li role="doc-footnote" id="fn1">NOTE_ONE</li>`+
			`<li role="doc-footnote" id="fn2">NOTE_TWO</li>`+
			`<li role="doc-footnote" id="fn3">NOTE_THREE</li>`,
	)

	_, doc := parseFootnoteDocument(t, source, nil)

	got := footnoteIDs(doc)
	want := []string{"fn3", "fn1", "fn2"}
	if len(got) != len(want) {
		t.Fatalf("restored %d footnotes %v, want %v", len(got), got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("footnote order = %v, want %v", got, want)
			break
		}
	}

	if len(dom.GetElementsByTagName(doc, "ol")) != 1 {
		t.Error("all restored footnotes must share a single <ol>")
	}
	if strings.Count(dom.OuterHTML(doc), `id="fn3"`) != 1 {
		t.Error("fn3 referenced twice must be restored once")
	}
}

// TestKeepFootnotesDuplicateID makes an ambiguous target (same id on
// two elements) non-restorable, while the citation itself is retained.
func TestKeepFootnotesDuplicateID(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#fn1">[1]</a>`,
		`<li role="doc-footnote" id="fn1">DUP_A</li>`+
			`<li role="doc-footnote" id="fn1">DUP_B</li>`,
	)

	article, doc := parseFootnoteDocument(t, source, nil)

	if notes := dom.QuerySelectorAll(doc, `li[role="doc-footnote"]`); len(notes) != 0 {
		t.Errorf("ambiguous duplicate id must not be restored, got %v", footnoteIDs(doc))
	}
	if !strings.Contains(article.Content, `href="#fn1"`) {
		t.Error("the citation link must be left untouched when the target is ambiguous")
	}
	if strings.Contains(article.Content, "DUP_A") || strings.Contains(article.Content, "DUP_B") {
		t.Error("neither duplicate target may be restored")
	}
}

// TestKeepFootnotesFilteredNavigation ensures references that do not
// survive article extraction (here a navigation landmark) do not bring
// their footnotes back.
func TestKeepFootnotesFilteredNavigation(t *testing.T) {
	source := `<html><head><title>A research article with citations</title></head><body>` +
		`<nav role="navigation">Navigation <a role="doc-noteref" href="#fnnav">[n]</a></nav>` +
		`<article><h1>A research article with citations</h1>` +
		`<p>` + longParagraph + `<a role="doc-noteref" href="#fn1">[1]</a></p>` +
		`</article>` +
		`<footer><ol role="doc-endnotes">` +
		`<li role="doc-footnote" id="fn1">NOTE_ONE</li>` +
		`<li role="doc-footnote" id="fnnav">NOTE_NAV</li>` +
		`</ol></footer></body></html>`

	article, doc := parseFootnoteDocument(t, source, nil)

	if strings.Contains(article.Content, "NOTE_NAV") || strings.Contains(article.Content, `href="#fnnav"`) {
		t.Error("references filtered out with the navigation must not trigger a restore")
	}
	if got := footnoteIDs(doc); len(got) != 1 || got[0] != "fn1" {
		t.Errorf("only fn1 should be restored, got %v", got)
	}
}

// TestKeepFootnotesAlreadyInBody verifies that a footnote already part
// of the extracted article is not copied a second time.
func TestKeepFootnotesAlreadyInBody(t *testing.T) {
	source := `<html><head><title>A research article with citations</title></head><body>` +
		`<article><h1>A research article with citations</h1>` +
		`<p>` + longParagraph + `<a role="doc-noteref" href="#fn1">[1]</a></p>` +
		`<ol><li role="doc-footnote" id="fn1">NOTE_BODY already inside.</li></ol>` +
		`</article>` +
		`<footer><ol role="doc-endnotes"><li role="doc-footnote" id="fn2">NOTE_OUTSIDE</li></ol></footer>` +
		`</body></html>`

	article, doc := parseFootnoteDocument(t, source, nil)

	if count := strings.Count(article.Content, `id="fn1"`); count != 1 {
		t.Errorf("in-body footnote fn1 must appear once, got %d", count)
	}
	if strings.Contains(article.Content, "NOTE_OUTSIDE") {
		t.Error("unreferenced outside footnote must not be restored")
	}
	if got := footnoteIDs(doc); len(got) != 1 || got[0] != "fn1" {
		t.Errorf("only the in-body fn1 should be present, got %v", got)
	}
}

// TestKeepFootnotesMutualReferences confirms links inside footnotes do
// not start another restore round, so mutually referring notes cannot
// expand the result.
func TestKeepFootnotesMutualReferences(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#fn1">[1]</a>`,
		`<li role="doc-footnote" id="fn1">NOTE_ONE see <a role="doc-noteref" href="#fn2">[2]</a></li>`+
			`<li role="doc-footnote" id="fn2">NOTE_TWO back <a role="doc-noteref" href="#fn1">[1]</a></li>`,
	)

	article, doc := parseFootnoteDocument(t, source, nil)

	if got := footnoteIDs(doc); len(got) != 1 || got[0] != "fn1" {
		t.Fatalf("only the body-referenced fn1 may be restored, got %v", got)
	}
	if strings.Contains(article.Content, "NOTE_TWO") {
		t.Error("fn2 is only referenced from a footnote and must not be restored")
	}
}

// TestKeepFootnotesReferenceRules exercises the definition of a
// reference: exact role value, whitespace-split case-insensitive rel
// tokens, same-page fragment/URL resolution, and the non-triggering
// cases.
func TestKeepFootnotesReferenceRules(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#fn1">[1]</a>`+
			`<a rel="cite footnote" href="#fn2">[2]</a>`+
			`<a rel="FOOTNOTE" href="https://capture.invalid/paper#fn3">[3]</a>`+
			`<a rel="cite	footnote" href="paper#fn4">[4]</a>`+
			`<a rel="xfootnote" href="#fn5">[5]</a>`+
			`<a role="doc-noteref extra" href="#fn6">[6]</a>`+
			`<a role="doc-noteref" href="https://elsewhere.invalid/paper#fn7">[7]</a>`+
			`<a role="doc-noteref" href="#bad%zz">[8]</a>`+
			`<a href="#fn9">[9]</a>`+
			`<a role="doc-noteref" href="#fn10">[10]</a>`+
			`<a role="doc-noteref" href="#fn11">[11]</a>`+
			`<a role="doc-noteref" href="#fnmissing">[x]</a>`,
		`<li role="doc-footnote" id="fn1">N1</li>`+
			`<li role="doc-footnote" id="fn2">N2</li>`+
			`<li role="doc-footnote" id="fn3">N3</li>`+
			`<li role="doc-footnote" id="fn4">N4</li>`+
			`<li role="doc-footnote" id="fn5">N5</li>`+
			`<li role="doc-footnote" id="fn6">N6</li>`+
			`<li role="doc-footnote" id="fn7">N7</li>`+
			`<div role="doc-footnote" id="fn10">N10 is not an li</div>`+
			`<li id="fn11">N11 has no footnote role</li>`,
	)

	article, doc := parseFootnoteDocument(t, source, nil)

	got := footnoteIDs(doc)
	want := []string{"fn1", "fn2", "fn3", "fn4"}
	if len(got) != len(want) {
		t.Fatalf("restored %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("restored %v, want %v", got, want)
		}
	}

	// Non-triggering references stay in the text exactly as authored.
	// Note that fixRelativeURIs rewrites relative/absolute hrefs, but
	// the cross-article link is preserved with its full URL.
	for _, href := range []string{
		`href="#fn5"`, `href="#fn6"`,
		`href="https://elsewhere.invalid/paper#fn7"`,
		`href="#bad%zz"`, `href="#fn9"`, `href="#fn10"`,
		`href="#fn11"`, `href="#fnmissing"`,
	} {
		if !strings.Contains(article.Content, href) {
			t.Errorf("non-triggering reference %s must be preserved untouched", href)
		}
	}
	for _, marker := range []string{">N5<", ">N6<", ">N7<", "N10", "N11"} {
		if strings.Contains(article.Content, marker) {
			t.Errorf("footnote for marker %s must not be restored", marker)
		}
	}
}

// TestKeepFootnotesNonASCIIID covers percent-encoded fragments,
// including a non-ASCII id and the rule that "+" is not a space.
func TestKeepFootnotesNonASCIIID(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#f%C3%B6note">[a]</a>`+
			`<a role="doc-noteref" href="#fn%2B1">[b]</a>`+
			`<a role="doc-noteref" href="#fn+plus">[c]</a>`,
		`<li role="doc-footnote" id="fönote">NONASCII NOTE</li>`+
			`<li role="doc-footnote" id="fn+1">PLUS NOTE</li>`+
			`<li role="doc-footnote" id="fn plus">SPACED NOTE</li>`,
	)

	article, doc := parseFootnoteDocument(t, source, nil)

	got := footnoteIDs(doc)
	want := []string{"fönote", "fn+1"}
	if len(got) != len(want) {
		t.Fatalf("restored %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("restored %v, want %v", got, want)
		}
	}
	if !strings.Contains(article.TextContent, "NONASCII NOTE") {
		t.Error("non-ASCII footnote text missing from TextContent")
	}
	if strings.Contains(article.Content, "SPACED NOTE") {
		t.Error(`"+" must not be decoded as a space, so "fn plus" must not be restored`)
	}
}

// TestKeepFootnotesShortRetry forces every short-content retry by
// setting an unreachable character threshold, and checks the footnotes
// are still restored with concrete DOM, text and length assertions,
// without changing the article chosen by the retry logic.
func TestKeepFootnotesShortRetry(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#fn1">[1]</a>`,
		`<li role="doc-footnote" id="fn1">NOTE_RETRY</li>`,
	)

	article, doc := parseFootnoteDocument(t, source, func(p *Parser) {
		p.CharThresholds = 1000000
	})

	if got := footnoteIDs(doc); len(got) != 1 || got[0] != "fn1" {
		t.Fatalf("footnote must survive the retry path, got %v", got)
	}
	if !strings.Contains(article.TextContent, "NOTE_RETRY") {
		t.Error("restored footnote text missing after retry")
	}
	if article.Length != utf8.RuneCountInString(article.TextContent) {
		t.Errorf("Length = %d, want rune count %d", article.Length, utf8.RuneCountInString(article.TextContent))
	}
	if strings.Contains(article.Excerpt, "NOTE_RETRY") {
		t.Error("excerpt must still come from the main body after retry")
	}

	// With the same unreadable threshold, the disabled parser picks the
	// exact same article through the same retries: apart from the
	// appended footnote list the output must be identical, proving the
	// option never influences candidate selection or the retry logic.
	offParser := NewParser()
	offParser.KeepFootnotes = false
	offParser.CharThresholds = 1000000
	offArticle, err := offParser.Parse(strings.NewReader(source), testPageURL)
	if err != nil {
		t.Fatalf("disabled retry parse failed: %v", err)
	}
	if offArticle.Excerpt != article.Excerpt {
		t.Error("excerpt must not depend on KeepFootnotes during retries")
	}

	lastList := strings.LastIndex(article.Content, "<ol>")
	if lastList == -1 {
		t.Fatal("missing restored footnote list")
	}
	bodyWithoutNotes := article.Content[:lastList] + "</div>"
	if bodyWithoutNotes != offArticle.Content {
		t.Error("footnote restore changed the article selected by the short-content retry path")
	}
	if article.Length <= offArticle.Length {
		t.Errorf("restored footnote must add text: on=%d off=%d", article.Length, offArticle.Length)
	}
}

// TestKeepFootnotesCleaningAndClasses checks restored notes go through
// the normal cleaning rules, relative link conversion and the
// KeepClasses setting, without disabling article-wide cleanup.
func TestKeepFootnotesCleaningAndClasses(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#fn1">[1]</a>`,
		`<li role="doc-footnote" id="fn1" class="keepme junk">NOTE_ONE `+
			`<script>alert(1)</script><style>a{}</style>`+
			`<form><input value="x"></form>`+
			`<a href="detail.html">relative</a></li>`,
	)

	stripped, strippedDoc := parseFootnoteDocument(t, source, nil)
	if strings.Contains(stripped.Content, "<script") || strings.Contains(stripped.Content, "<style") ||
		strings.Contains(stripped.Content, "<form") || strings.Contains(stripped.Content, "<input") {
		t.Error("junk inside a restored footnote must be cleaned out")
	}
	if !strings.Contains(stripped.Content, `href="https://capture.invalid/detail.html"`) {
		t.Error("relative links inside restored footnotes must be converted to absolute")
	}
	if li := dom.QuerySelector(strippedDoc, `li#fn1`); li == nil || dom.ClassName(li) != "" {
		t.Error("classes must be stripped when KeepClasses is false")
	}

	kept, keptDoc := parseFootnoteDocument(t, source, func(p *Parser) {
		p.KeepClasses = true
	})
	if strings.Contains(kept.Content, "<script") || strings.Contains(kept.Content, "<form") {
		t.Error("KeepClasses must not disable the other cleaning rules")
	}
	if li := dom.QuerySelector(keptDoc, `li#fn1`); li == nil || dom.ClassName(li) != "keepme junk" {
		t.Errorf("KeepClasses should preserve note classes, got %q", dom.ClassName(li))
	}
}

// TestKeepFootnotesDisabled proves the default configuration leaves
// the output byte-identical to a parser with the option explicitly off,
// including through FromReader and FromDocument.
func TestKeepFootnotesDisabled(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#fn1">[1]</a>`,
		`<li role="doc-footnote" id="fn1">NOTE_ONE</li>`+
			`<li role="doc-footnote" id="fn2">NOTE_UNUSED</li>`,
	)

	defaultParser := NewParser()
	defaultArticle, err := defaultParser.Parse(strings.NewReader(source), testPageURL)
	if err != nil {
		t.Fatalf("default parse failed: %v", err)
	}

	explicitOff := NewParser()
	explicitOff.KeepFootnotes = false
	offArticle, err := explicitOff.Parse(strings.NewReader(source), testPageURL)
	if err != nil {
		t.Fatalf("explicit-off parse failed: %v", err)
	}

	if defaultArticle.Content != offArticle.Content {
		t.Error("default and KeepFootnotes=false output must be identical")
	}
	if strings.Contains(defaultArticle.Content, "NOTE_ONE") {
		t.Error("footnote must be dropped with the default configuration")
	}
	if strings.Contains(defaultArticle.TextContent, "NOTE_ONE") {
		t.Error("footnote text must not leak into TextContent with the default configuration")
	}

	fromReader, err := FromReader(strings.NewReader(source), testPageURL)
	if err != nil {
		t.Fatalf("FromReader failed: %v", err)
	}
	if fromReader.Content != defaultArticle.Content {
		t.Error("FromReader must keep using the default disabled configuration")
	}

	doc, err := dom.Parse(strings.NewReader(source))
	if err != nil {
		t.Fatalf("failed to parse source document: %v", err)
	}
	fromDoc, err := FromDocument(doc, testPageURL)
	if err != nil {
		t.Fatalf("FromDocument failed: %v", err)
	}
	if fromDoc.Content != defaultArticle.Content {
		t.Error("FromDocument must keep using the default disabled configuration")
	}
}

// TestKeepFootnotesParserReuse reuses one parser across two pages,
// toggles the option and drives a failed parse in between, verifying no
// target index or reference order leaks between runs.
func TestKeepFootnotesParserReuse(t *testing.T) {
	pageA := wrapArticle(
		`<a role="doc-noteref" href="#fnA">[a]</a>`,
		`<li role="doc-footnote" id="fnA">NOTE_A</li>`,
	)
	pageB := wrapArticle(
		`<a role="doc-noteref" href="#fnB">[b]</a>`,
		`<li role="doc-footnote" id="fnB">NOTE_B</li>`,
	)

	parser := NewParser()
	parser.KeepFootnotes = true

	articleA, err := parser.Parse(strings.NewReader(pageA), testPageURL)
	if err != nil {
		t.Fatalf("first parse failed: %v", err)
	}
	if !strings.Contains(articleA.Content, "NOTE_A") {
		t.Fatal("first parse should restore NOTE_A")
	}

	articleB, err := parser.Parse(strings.NewReader(pageB), testPageURL)
	if err != nil {
		t.Fatalf("second parse failed: %v", err)
	}
	if strings.Contains(articleB.Content, "NOTE_A") || !strings.Contains(articleB.Content, "NOTE_B") {
		t.Error("footnote state from page A leaked into page B")
	}

	// Toggle the option off: no restore should happen.
	parser.KeepFootnotes = false
	offArticle, err := parser.Parse(strings.NewReader(pageA), testPageURL)
	if err != nil {
		t.Fatalf("disabled parse failed: %v", err)
	}
	if strings.Contains(offArticle.Content, "NOTE_A") {
		t.Error("toggling KeepFootnotes off must stop restoring footnotes")
	}

	// Drive a failed parse (document larger than the configured limit).
	parser.MaxElemsToParse = 1
	if _, err := parser.Parse(strings.NewReader(pageA), testPageURL); err == nil {
		t.Error("expected the over-limit parse to fail")
	}
	parser.MaxElemsToParse = 0

	// Re-enable and parse page A again: fresh state, same result as the
	// very first parse, with nothing carried over from page B or the
	// failed run.
	parser.KeepFootnotes = true
	retryA, err := parser.Parse(strings.NewReader(pageA), testPageURL)
	if err != nil {
		t.Fatalf("final parse failed: %v", err)
	}
	if retryA.Content != articleA.Content {
		t.Error("parser reuse altered the output of the repeated page A parse")
	}
	if strings.Contains(retryA.Content, "NOTE_B") {
		t.Error("page B footnote index leaked into the repeated page A parse")
	}
}

// TestKeepFootnotesDoesNotMutateDocument ensures ParseDocument never
// modifies the caller's own document tree.
func TestKeepFootnotesDoesNotMutateDocument(t *testing.T) {
	source := wrapArticle(
		`<a role="doc-noteref" href="#fn1">[1]</a>`,
		`<li role="doc-footnote" id="fn1">NOTE_ONE</li>`,
	)

	doc, err := dom.Parse(strings.NewReader(source))
	if err != nil {
		t.Fatalf("failed to parse source: %v", err)
	}
	before := dom.OuterHTML(doc)

	parser := NewParser()
	parser.KeepFootnotes = true
	article, err := parser.ParseDocument(doc, testPageURL)
	if err != nil {
		t.Fatalf("ParseDocument failed: %v", err)
	}

	after := dom.OuterHTML(doc)
	if before != after {
		t.Error("ParseDocument modified the caller's document")
	}

	// Original footer still owns the footnote in the caller's tree.
	footer := dom.QuerySelector(doc, "footer")
	if footer == nil || dom.QuerySelector(footer, `li#fn1`) == nil {
		t.Error("original footnote must remain in the caller's footer")
	}
	if !strings.Contains(article.Content, "NOTE_ONE") {
		t.Error("the article copy should still contain the restored footnote")
	}
}
