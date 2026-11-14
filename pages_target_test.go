package readability

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
	"unicode/utf8"

	"github.com/go-shiori/dom"
	"github.com/go-shiori/go-readability/task_support/taskcheck"
	"golang.org/x/net/html"
)

// pageFiller is enough readable text for the default parser to keep the
// paragraphs that carry the test markers and links.
func pageFiller() string {
	return strings.Repeat("A long field report, with details from the observed landscape, weather and collection process. ", 12)
}

// pageHTML builds a simple HTML document.
func pageHTML(title, headExtra, bodyExtra string, paragraphs ...string) string {
	var body strings.Builder
	body.WriteString("<article>")
	for _, p := range paragraphs {
		body.WriteString("<p>" + p + "</p>")
	}
	body.WriteString("</article>")
	body.WriteString(bodyExtra)

	return fmt.Sprintf("<!doctype html><html><head><title>%s</title>%s</head><body>%s</body></html>",
		title, headExtra, body.String())
}

// countingServer wraps a mux and records how many times every path has
// been requested, including paths that end up as 404.
func countingServer(t *testing.T, register func(*http.ServeMux)) (*httptest.Server, func(string) int) {
	t.Helper()

	mux := http.NewServeMux()
	register(mux)

	var mu sync.Mutex
	counts := make(map[string]int)
	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		counts[r.URL.Path]++
		mu.Unlock()
		mux.ServeHTTP(w, r)
	})

	// Recover so an environment that forbids local port binding fails this
	// test cleanly instead of panicking the whole test binary.
	var server *httptest.Server
	func() {
		defer func() {
			if recovered := recover(); recovered != nil {
				t.Fatalf("httptest server could not start in this environment: %v", recovered)
			}
		}()
		server = httptest.NewServer(handler)
	}()
	t.Cleanup(func() {
		if server != nil {
			server.Close()
		}
	})

	hits := func(path string) int {
		mu.Lock()
		defer mu.Unlock()
		return counts[path]
	}
	return server, hits
}

// findAnchor returns the first anchor whose text equals marker.
func findAnchor(root *html.Node, marker string) *html.Node {
	for _, anchor := range dom.GetElementsByTagName(root, "a") {
		if strings.TrimSpace(dom.TextContent(anchor)) == marker {
			return anchor
		}
	}
	return nil
}

// deepestElementWithText returns the deepest element whose text content
// still contains marker.
func deepestElementWithText(root *html.Node, marker string) *html.Node {
	var found *html.Node
	for _, el := range dom.GetElementsByTagName(root, "*") {
		if strings.Contains(dom.TextContent(el), marker) {
			found = el
		}
	}
	return found
}

func allIDs(root *html.Node) []string {
	var ids []string
	for _, el := range dom.GetElementsByTagName(root, "*") {
		if id := dom.ID(el); id != "" {
			ids = append(ids, id)
		}
	}
	return ids
}

// TestPagesBase verifies redirects, head/base resolution, per-page
// resource bases, page ordering and the merged document structure.
func TestPagesBase(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		server, hits := countingServer(t, func(mux *http.ServeMux) {
			mux.HandleFunc("/entry", func(w http.ResponseWriter, r *http.Request) {
				http.Redirect(w, r, "/one/start", http.StatusFound)
			})

			mux.HandleFunc("/one/start", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				head := `<base href="/assets/"><link rel="Next" href="part2">`
				paragraphs := []string{
					`FIRST_PAGE_MARKER ` + pageFiller(),
					`<span id="section1">SECTION_ONE</span> ` + pageFiller(),
					`<a href="pic.jpg">PIC_LINK</a> ` + pageFiller(),
					`<img src="cover.png" alt="cover"/> ` + pageFiller(),
				}
				fmt.Fprint(w, pageHTML("Collected article", head, "", paragraphs...))
			})

			mux.HandleFunc("/assets/part2", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				// No base element: relative refs resolve against this URL.
				head := `<link rel="chapter next" href="part3">`
				paragraphs := []string{
					`SECOND_PAGE_MARKER ` + pageFiller(),
					`<span id="section1">SECTION_ONE_AGAIN</span> ` + pageFiller(),
					`<img src="figure.png" alt="figure"/> ` + pageFiller(),
				}
				fmt.Fprint(w, pageHTML("Second page title", head, "", paragraphs...))
			})

			mux.HandleFunc("/assets/part3", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				head := `<base href="/cdn/">`
				paragraphs := []string{
					`THIRD_PAGE_MARKER ` + pageFiller(),
					`<img src="x.png" alt="x"/> ` + pageFiller(),
				}
				fmt.Fprint(w, pageHTML("Third page title", head, "", paragraphs...))
			})
		})

		article, err := FromURLPages(server.URL+"/entry", 5*time.Second, 10)
		c.Check(err == nil, "FromURLPages returned error: %v", err)
		c.Check(article.Node != nil, "merged article node is nil")
		if article.Node == nil {
			return
		}

		// Real requests: redirect followed, next links resolved via bases,
		// and no guessed/root-relative request was made.
		c.Check(hits("/entry") == 1, "want 1 /entry request, got %d", hits("/entry"))
		c.Check(hits("/one/start") == 1, "want 1 /one/start request, got %d", hits("/one/start"))
		c.Check(hits("/assets/part2") == 1, "want 1 /assets/part2 request, got %d", hits("/assets/part2"))
		c.Check(hits("/assets/part3") == 1, "want 1 /assets/part3 request, got %d", hits("/assets/part3"))
		c.Check(hits("/part2") == 0, "next link must resolve via base, but /part2 was requested %d times", hits("/part2"))

		// Final DOM: one div wrapper per page inside the returned div.
		c.Check(dom.TagName(article.Node) == "div", "root node must be a div, got %q", dom.TagName(article.Node))
		pageWrappers := dom.Children(article.Node)
		c.Check(len(pageWrappers) == 3, "want 3 page wrappers, got %d", len(pageWrappers))
		for i, wrapper := range pageWrappers {
			c.Check(dom.TagName(wrapper) == "div", "page wrapper %d must be a div", i+1)
			c.Check(dom.GetAttribute(wrapper, "class") == "page", "page wrapper %d must keep the page class", i+1)
		}

		// Every page-scoped id is unique and follows the p<page>-<ordinal>
		// scheme; the parser's own id must not survive.
		ids := allIDs(article.Node)
		seenIDs := make(map[string]struct{})
		hasUniqueIDs := true
		for _, id := range ids {
			if _, dup := seenIDs[id]; dup {
				hasUniqueIDs = false
			}
			seenIDs[id] = struct{}{}
		}
		c.Check(hasUniqueIDs, "rewritten ids must be unique, got %v", ids)
		c.Check(len(ids) == 5, "want 5 rewritten ids (2 per page with a span id, 1 wrapper-only), got %v", ids)
		for _, expected := range []string{"p1-1", "p1-2", "p2-1", "p2-2", "p3-1"} {
			_, exist := seenIDs[expected]
			c.Check(exist, "expected rewritten id %q in %v", expected, ids)
		}
		c.Check(!strings.Contains(article.Content, "readability-page-1"), "parser-generated id must be renamed")

		// Relative resources use the effective base of their own page.
		c.Check(strings.Contains(article.Content, server.URL+"/assets/pic.jpg"),
			"page 1 anchor must resolve against /assets/, content: %s", article.Content)
		c.Check(strings.Contains(article.Content, server.URL+"/assets/cover.png"),
			"page 1 image must resolve against /assets/")
		c.Check(strings.Contains(article.Content, server.URL+"/assets/figure.png"),
			"page 2 image must resolve against its final URL base")
		c.Check(strings.Contains(article.Content, server.URL+"/cdn/x.png"),
			"page 3 image must resolve against its own /cdn/ base")

		// All three pages' body text is present, text is trimmed and the
		// length counts Unicode characters.
		c.Check(strings.Contains(article.TextContent, "FIRST_PAGE_MARKER"), "missing page 1 text")
		c.Check(strings.Contains(article.TextContent, "SECOND_PAGE_MARKER"), "missing page 2 text")
		c.Check(strings.Contains(article.TextContent, "THIRD_PAGE_MARKER"), "missing page 3 text")
		c.Check(article.TextContent == strings.TrimSpace(article.TextContent), "TextContent must be trimmed")
		c.Check(article.Length == utf8.RuneCountInString(article.TextContent),
			"Length must be the Unicode character count")
		c.Check(article.Content == dom.OuterHTML(article.Node), "Content must be the root outer HTML")

		// Metadata comes from the first page only.
		c.Check(article.Title == "Collected article", "title must come from the first page, got %q", article.Title)
	})
}

// TestPagesAnchors verifies forward/backward fragment links, duplicate and
// cleaned targets, percent-decoding and the literal "+" rule.
func TestPagesAnchors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		server, _ := countingServer(t, func(mux *http.ServeMux) {
			mux.HandleFunc("/a1", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				head := `<link rel="next" href="/a2">`
				paragraphs := []string{
					`PAGE_ONE_MARKER ` + pageFiller(),
					`<span id="mine">MINE_TARGET</span> ` + pageFiller(),
					`<a href="/a2#sec2">FORWARD_MARK</a> ` + pageFiller(),
					`<a href="/a1#mine">SAME_MARK</a> ` + pageFiller(),
					`<a href="/a2#dup">DUP_MARK</a> ` + pageFiller(),
					`<a href="/a2#gone">GONE_MARK</a> ` + pageFiller(),
					`<a href="/a2#a%2Bb">PLUS_MARK</a> ` + pageFiller(),
					`<a href="/a2#sec%20two">SPACE_MARK</a> ` + pageFiller(),
					`<a href="/a2#c+d">LITERAL_PLUS_MARK</a> ` + pageFiller(),
					`<a href="https://example.com/keep">EXT_MARK</a> ` + pageFiller(),
				}
				// The aside is cleaned by the parser, so id="gone" is not
				// a valid fragment target anymore.
				extra := `<aside id="gone">GONE_SHOULD_BE_REMOVED</aside>`
				fmt.Fprint(w, pageHTML("Anchor article one", head, extra, paragraphs...))
			})

			mux.HandleFunc("/a2", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				head := `<link rel="next" href="/a3">`
				paragraphs := []string{
					`PAGE_TWO_MARKER ` + pageFiller(),
					`<span id="sec2">SEC2_TARGET</span> ` + pageFiller(),
					`<span id="dup">DUP_TARGET_A</span> ` + pageFiller(),
					`<span id="dup">DUP_TARGET_B</span> ` + pageFiller(),
					`<span id="a+b">PLUS_TARGET</span> ` + pageFiller(),
					`<span id="sec two">SPACED_TARGET</span> ` + pageFiller(),
					`<a href="/a1#mine">BACK_MARK</a> ` + pageFiller(),
				}
				fmt.Fprint(w, pageHTML("Anchor article two", head, "", paragraphs...))
			})

			mux.HandleFunc("/a3", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				paragraphs := []string{
					`PAGE_THREE_MARKER ` + pageFiller(),
					`<a href="/a2#sec2">LATER_MARK</a> ` + pageFiller(),
					// In-article recommendation must never drive traversal.
					`<a href="/a2">RECOMMEND_MARK</a> ` + pageFiller(),
				}
				fmt.Fprint(w, pageHTML("Anchor article three", "", "", paragraphs...))
			})
		})

		article, err := FromURLPages(server.URL+"/a1", 5*time.Second, 10)
		c.Check(err == nil, "FromURLPages returned error: %v", err)
		c.Check(article.Node != nil, "merged article node is nil")
		c.Check(len(dom.Children(article.Node)) == 3, "want 3 page wrappers")
		if article.Node == nil {
			return
		}

		sec2 := deepestElementWithText(article.Node, "SEC2_TARGET")
		mine := deepestElementWithText(article.Node, "MINE_TARGET")
		plus := deepestElementWithText(article.Node, "PLUS_TARGET")
		spaced := deepestElementWithText(article.Node, "SPACED_TARGET")
		c.Check(sec2 != nil && mine != nil && plus != nil && spaced != nil,
			"target elements must survive extraction")

		expectHref := func(marker, href string) {
			anchor := findAnchor(article.Node, marker)
			c.Check(anchor != nil, "anchor %q not found", marker)
			if anchor != nil {
				c.Check(dom.GetAttribute(anchor, "href") == href,
					"anchor %q: want href %q, got %q", marker, href, dom.GetAttribute(anchor, "href"))
			}
		}

		// Forward, same-page and backward fragment references are rewritten
		// to page-scoped fragment links.
		expectHref("FORWARD_MARK", "#"+dom.ID(sec2))
		expectHref("SAME_MARK", "#"+dom.ID(mine))
		expectHref("BACK_MARK", "#"+dom.ID(mine))
		expectHref("LATER_MARK", "#"+dom.ID(sec2))

		// Percent-decoding happens once; "+" keeps its literal meaning.
		expectHref("PLUS_MARK", "#"+dom.ID(plus))
		expectHref("SPACE_MARK", "#"+dom.ID(spaced))

		// Duplicate ids, cleaned targets, missing ids and external pages
		// keep their absolute address instead of picking a target.
		expectHref("DUP_MARK", server.URL+"/a2#dup")
		expectHref("GONE_MARK", server.URL+"/a2#gone")
		expectHref("LITERAL_PLUS_MARK", server.URL+"/a2#c+d")
		expectHref("EXT_MARK", "https://example.com/keep")

		// Link text is never modified.
		c.Check(dom.TextContent(findAnchor(article.Node, "FORWARD_MARK")) == "FORWARD_MARK",
			"anchor text must be preserved")
		c.Check(dom.TextContent(findAnchor(article.Node, "BACK_MARK")) == "BACK_MARK",
			"anchor text must be preserved")

		// The recommendation link is an ordinary absolute link, not a
		// pagination instruction.
		expectHref("RECOMMEND_MARK", server.URL+"/a2")
	})
}

// TestPagesTraversal verifies the page cap, visited-page/alias cycles and
// request-level error handling.
func TestPagesTraversal(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		server, hits := countingServer(t, func(mux *http.ServeMux) {
			for i := 1; i <= 4; i++ {
				i := i
				next := i + 1
				mux.HandleFunc(fmt.Sprintf("/t%d", i), func(w http.ResponseWriter, r *http.Request) {
					w.Header().Set("Content-Type", "text/html; charset=utf-8")
					head := ""
					if next <= 4 {
						head = fmt.Sprintf(`<link rel="next" href="/t%d">`, next)
					}
					paragraphs := []string{
						fmt.Sprintf("CHAIN_PAGE_%d_MARKER ", i) + pageFiller(),
					}
					if i == 2 {
						paragraphs = append(paragraphs,
							`<a href="/t4">BEYOND_CAP_MARK</a> `+pageFiller())
					}
					fmt.Fprint(w, pageHTML(fmt.Sprintf("Chain %d", i), head, "", paragraphs...))
				})
			}

			mux.HandleFunc("/stop", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				// "nextpage" is a single token that must not match "next".
				head := `<link rel="nextpage" href="/t2">`
				fmt.Fprint(w, pageHTML("Stop page", head, "", pageFiller()))
			})

			mux.HandleFunc("/ring1", func(w http.ResponseWriter, r *http.Request) {
				http.Redirect(w, r, "/ring2", http.StatusFound)
			})
			mux.HandleFunc("/ring2", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				// Fragment must be ignored when comparing visited pages.
				head := `<link rel="next" href="/ring1#part">`
				fmt.Fprint(w, pageHTML("Ring page", head, "", pageFiller()))
			})

			mux.HandleFunc("/fail", func(w http.ResponseWriter, r *http.Request) {
				http.Error(w, "boom", http.StatusInternalServerError)
			})
			mux.HandleFunc("/jsonpage", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				fmt.Fprint(w, `{"ok":true}`)
			})
			mux.HandleFunc("/ftpnext", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "text/html; charset=utf-8")
				head := `<link rel="next" href="ftp://example.org/x">`
				fmt.Fprint(w, pageHTML("FTP next", head, "", pageFiller()))
			})
		})

		// The cap is inclusive: no request beyond the second page.
		capped, err := FromURLPages(server.URL+"/t1", 5*time.Second, 2)
		c.Check(err == nil, "capped fetch returned error: %v", err)
		c.Check(capped.Node != nil, "capped article node is nil")
		c.Check(hits("/t1") == 1, "want 1 /t1 request, got %d", hits("/t1"))
		c.Check(hits("/t2") == 1, "want 1 /t2 request, got %d", hits("/t2"))
		c.Check(hits("/t3") == 0, "page beyond cap must not be requested, got %d", hits("/t3"))
		c.Check(hits("/t4") == 0, "page beyond cap must not be requested, got %d", hits("/t4"))
		c.Check(len(dom.Children(capped.Node)) == 2, "want 2 merged pages, got %d", len(dom.Children(capped.Node)))
		c.Check(strings.Contains(capped.TextContent, "CHAIN_PAGE_1_MARKER"), "missing page 1 text")
		c.Check(strings.Contains(capped.TextContent, "CHAIN_PAGE_2_MARKER"), "missing page 2 text")
		c.Check(!strings.Contains(capped.TextContent, "CHAIN_PAGE_3_MARKER"), "page 3 text must be absent")

		if capped.Node != nil {
			beyond := findAnchor(capped.Node, "BEYOND_CAP_MARK")
			c.Check(beyond != nil, "anchor to page beyond cap not found")
			if beyond != nil {
				c.Check(dom.GetAttribute(beyond, "href") == server.URL+"/t4",
					"link beyond the cap must stay absolute, got %q", dom.GetAttribute(beyond, "href"))
			}
		}

		// maxPages must be positive.
		zeroArticle, zeroErr := FromURLPages(server.URL+"/t1", 5*time.Second, 0)
		c.Check(zeroErr != nil, "maxPages=0 must return an error")
		c.Check(zeroArticle.Node == nil, "maxPages=0 must return an empty article")

		// A rel token that merely starts with "next" does not paginate.
		t2Before := hits("/t2")
		stopped, err := FromURLPages(server.URL+"/stop", 5*time.Second, 5)
		c.Check(err == nil, "stop-page fetch returned error: %v", err)
		c.Check(len(dom.Children(stopped.Node)) == 1, "want a single page when no rel=next exists")
		c.Check(hits("/t2") == t2Before, "stop page must not follow its link: before=%d after=%d", t2Before, hits("/t2"))

		// Alias ring through a redirect: the second encounter is an error
		// and an empty article, never a duplicated merge.
		ringArticle, ringErr := FromURLPages(server.URL+"/ring1", 5*time.Second, 10)
		c.Check(ringErr != nil, "alias cycle must return an error")
		c.Check(ringArticle.Node == nil, "alias cycle must return an empty article")
		c.Check(hits("/ring1") == 2, "want 2 /ring1 requests, got %d", hits("/ring1"))
		c.Check(hits("/ring2") == 2, "want 2 /ring2 requests before cycle detection, got %d", hits("/ring2"))

		// Request and content failures surface as errors with empty
		// articles.
		failArticle, failErr := FromURLPages(server.URL+"/fail", 5*time.Second, 5)
		c.Check(failErr != nil, "HTTP 500 must return an error")
		c.Check(failArticle.Node == nil, "HTTP 500 must return an empty article")

		jsonArticle, jsonErr := FromURLPages(server.URL+"/jsonpage", 5*time.Second, 5)
		c.Check(jsonErr != nil, "non-HTML content must return an error")
		c.Check(jsonArticle.Node == nil, "non-HTML content must return an empty article")

		ftpArticle, ftpErr := FromURLPages(server.URL+"/ftpnext", 5*time.Second, 5)
		c.Check(ftpErr != nil, "non-HTTP(S) next link must return an error")
		c.Check(ftpArticle.Node == nil, "non-HTTP(S) next link must return an empty article")
	})
}
