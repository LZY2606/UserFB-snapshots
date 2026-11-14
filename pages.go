package readability

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	nurl "net/url"
	"strconv"
	"strings"
	"time"

	"github.com/go-shiori/dom"
	"golang.org/x/net/html"
)

// fetchedPage holds everything collected from a single fetched document:
// the parsed article, the addresses involved in the request, the base
// address used to resolve that page's relative references and the raw
// href of the page's own rel="next" declaration.
type fetchedPage struct {
	requestURL *nurl.URL
	finalURL   *nurl.URL
	baseURL    *nurl.URL
	nextHref   string
	article    Article
	// ids maps each original (non-empty) id kept in the article node to
	// its new page-scoped id.
	ids map[string]string
	// idCounts records how many kept elements carried each original id,
	// so links to duplicated ids can be left untouched.
	idCounts map[string]int
}

// FromURLPages fetches a paginated article, following the pages' own
// rel="next" declarations, and merges the readable content of every page
// into a single Article. Each page is extracted independently with the
// default Parser, so the navigation, titles and metadata of the following
// pages never participate in the first page's content selection.
//
// Pagination stops when a page has no rel="next" link, when maxPages has
// been collected or when an error occurs. Only the first link element in
// the original head with a non-empty href and a rel attribute whose
// whitespace-separated tokens include "next" is followed; page numbers are
// never guessed and in-article recommendation links are never followed.
func FromURLPages(pageURL string, timeout time.Duration, maxPages int, requestModifiers ...RequestWith) (Article, error) {
	if maxPages <= 0 {
		return Article{}, fmt.Errorf("maxPages must be greater than zero")
	}

	parsedURL, err := nurl.ParseRequestURI(pageURL)
	if err != nil {
		return Article{}, fmt.Errorf("failed to parse URL: %v", err)
	} else if !isHTTPURL(parsedURL) {
		return Article{}, fmt.Errorf("failed to parse URL: %q is not an HTTP(S) address", pageURL)
	}

	client := &http.Client{Timeout: timeout}
	var pages []fetchedPage
	visited := make(map[string]struct{})
	nextURL := parsedURL

	for len(pages) < maxPages {
		page, err := fetchArticlePage(client, nextURL, requestModifiers)
		if err != nil {
			return Article{}, err
		}

		// The redirected response URL is the page identity. A page already
		// collected (by request or final address, fragments ignored) means
		// the same content would be merged twice, which is an error.
		requestKey := pageURLKey(page.requestURL)
		finalKey := pageURLKey(page.finalURL)
		if _, seen := visited[requestKey]; seen {
			return Article{}, fmt.Errorf("page %q has already been collected", requestKey)
		} else if _, seen := visited[finalKey]; seen {
			return Article{}, fmt.Errorf("page %q has already been collected", finalKey)
		}
		visited[requestKey] = struct{}{}
		visited[finalKey] = struct{}{}

		pages = append(pages, page)

		// The cap is inclusive: once reached, return what has been
		// collected without requesting another page.
		if len(pages) >= maxPages {
			break
		}

		if page.nextHref == "" {
			break
		}

		nextURL, err = resolvePageURL(page.baseURL, page.nextHref)
		if err != nil {
			return Article{}, err
		}
	}

	return mergeArticlePages(pages)
}

// fetchArticlePage performs a single GET request, validates the HTML
// response, parses the document and extracts the readable content using
// the page's effective base address.
func fetchArticlePage(client *http.Client, pageURL *nurl.URL, requestModifiers []RequestWith) (fetchedPage, error) {
	req, err := http.NewRequest("GET", pageURL.String(), nil)
	if err != nil {
		return fetchedPage{}, fmt.Errorf("failed to fetch the page: %v", err)
	}
	for _, modifier := range requestModifiers {
		modifier(req)
	}

	resp, err := client.Do(req)
	if err != nil {
		return fetchedPage{}, fmt.Errorf("failed to fetch the page: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return fetchedPage{}, fmt.Errorf("failed to fetch the page: HTTP status %d", resp.StatusCode)
	}

	contentType := resp.Header.Get("Content-Type")
	if !strings.Contains(contentType, "text/html") {
		return fetchedPage{}, fmt.Errorf("URL is not a HTML document")
	}

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return fetchedPage{}, fmt.Errorf("failed to read the page: %v", err)
	}

	// Parse with the charset-aware parser used elsewhere in the package,
	// then inspect the original head for the base and next-page link.
	doc, err := dom.Parse(bytes.NewReader(body))
	if err != nil {
		return fetchedPage{}, fmt.Errorf("failed to parse input: %v", err)
	}

	// The final response URL, after any redirect, is the page identity.
	finalURL := cloneURL(pageURL)
	if resp.Request != nil && resp.Request.URL != nil {
		finalURL = cloneURL(resp.Request.URL)
	}

	baseURL := effectiveBaseURL(doc, finalURL)
	nextHref := findHeadNextHref(doc)

	parser := NewParser()
	article, err := parser.ParseDocument(doc, baseURL)
	if err != nil {
		return fetchedPage{}, err
	} else if article.Node == nil {
		return fetchedPage{}, fmt.Errorf("failed to extract readable content from %q", finalURL.String())
	}

	return fetchedPage{
		requestURL: pageURL,
		finalURL:   finalURL,
		baseURL:    baseURL,
		nextHref:   nextHref,
		article:    article,
	}, nil
}

// effectiveBaseURL resolves the first head/base[href] whose resolution
// against the page's final response URL yields an HTTP(S) address. When
// there is no usable base element, the final response URL itself is the
// base address.
func effectiveBaseURL(doc *html.Node, finalURL *nurl.URL) *nurl.URL {
	head := firstElement(doc, "head")
	if head != nil {
		for _, base := range dom.GetElementsByTagName(head, "base") {
			href := strings.TrimSpace(dom.GetAttribute(base, "href"))
			if href == "" {
				continue
			}

			ref, err := nurl.Parse(href)
			if err != nil {
				continue
			}

			resolved := finalURL.ResolveReference(ref)
			if isHTTPURL(resolved) {
				return resolved
			}
		}
	}
	return cloneURL(finalURL)
}

// findHeadNextHref returns the href of the first link element inside the
// original head that has a non-empty href and a rel attribute whose
// ASCII-whitespace-separated, case-insensitive tokens include "next".
func findHeadNextHref(doc *html.Node) string {
	head := firstElement(doc, "head")
	if head == nil {
		return ""
	}

	for _, link := range dom.GetElementsByTagName(head, "link") {
		href := dom.GetAttribute(link, "href")
		if strings.TrimSpace(href) == "" {
			continue
		}

		rel := dom.GetAttribute(link, "rel")
		for _, token := range splitASCIIWhitespace(rel) {
			if strings.EqualFold(token, "next") {
				return href
			}
		}
	}
	return ""
}

// splitASCIIWhitespace splits s on the HTML-defined ASCII whitespace
// characters (tab, LF, FF, CR and space).
func splitASCIIWhitespace(s string) []string {
	var tokens []string
	start := -1
	for i, r := range s {
		if r == '\t' || r == '\n' || r == '\f' || r == '\r' || r == ' ' {
			if start >= 0 {
				tokens = append(tokens, s[start:i])
				start = -1
			}
		} else if start < 0 {
			start = i
		}
	}
	if start >= 0 {
		tokens = append(tokens, s[start:])
	}
	return tokens
}

// resolvePageURL resolves a pagination reference against the page's base
// address and makes sure the result is an HTTP(S) address.
func resolvePageURL(baseURL *nurl.URL, href string) (*nurl.URL, error) {
	ref, err := nurl.Parse(href)
	if err != nil {
		return nil, fmt.Errorf("failed to parse next page URL %q: %v", href, err)
	}

	resolved := baseURL.ResolveReference(ref)
	if !isHTTPURL(resolved) {
		return nil, fmt.Errorf("next page %q is not an HTTP(S) address", resolved.String())
	}
	return resolved, nil
}

// isHTTPURL reports whether u carries an http or https scheme.
func isHTTPURL(u *nurl.URL) bool {
	if u == nil {
		return false
	}
	scheme := strings.ToLower(u.Scheme)
	return (scheme == "http" || scheme == "https") && u.Host != ""
}

// pageURLKey renders an address for visited-page comparison, dropping the
// fragment so the same document reached via different fragments is treated
// as identical.
func pageURLKey(u *nurl.URL) string {
	clone := cloneURL(u)
	clone.Fragment = ""
	return clone.String()
}

func cloneURL(u *nurl.URL) *nurl.URL {
	if u == nil {
		return nil
	}
	clone := *u
	return &clone
}

func firstElement(doc *html.Node, tagName string) *html.Node {
	nodes := dom.GetElementsByTagName(doc, tagName)
	if len(nodes) > 0 {
		return nodes[0]
	}
	return nil
}

// mergeArticlePages combines independently extracted pages into one
// Article. The returned Node is a plain div holding one extracted article
// node per page, in collection order.
func mergeArticlePages(pages []fetchedPage) (Article, error) {
	if len(pages) == 0 {
		return Article{}, fmt.Errorf("no page collected")
	}

	root := dom.CreateElement("div")
	for pageIndex := range pages {
		page := &pages[pageIndex]
		rewritePageIDs(page, pageIndex+1)
		dom.AppendChild(root, page.article.Node)
	}

	rewritePageAnchors(pages)

	first := pages[0].article
	content := dom.OuterHTML(root)
	textContent := strings.TrimSpace(dom.TextContent(root))

	merged := first
	merged.Node = root
	merged.Content = content
	merged.TextContent = textContent
	merged.Length = charCount(textContent)
	return merged, nil
}

// rewritePageIDs renames every non-empty id kept in a page's article node
// to a page-scoped id of the form "p<page>-<ordinal>" and records the
// mapping for fragment-link rewriting.
func rewritePageIDs(page *fetchedPage, pageNumber int) {
	page.ids = make(map[string]string)
	page.idCounts = make(map[string]int)
	ordinal := 0

	var walk func(*html.Node)
	walk = func(node *html.Node) {
		if node.Type == html.ElementNode {
			if id := dom.ID(node); id != "" {
				ordinal++
				newID := "p" + strconv.Itoa(pageNumber) + "-" + strconv.Itoa(ordinal)
				page.ids[id] = newID
				page.idCounts[id]++
				dom.SetAttribute(node, "id", newID)
			}
		}
		for child := node.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}
	walk(page.article.Node)
}

// rewritePageAnchors resolves every link against its own page's base
// address, then rewrites fragment links that point at a uniquely
// identified element of a collected page. Everything else keeps its
// absolute URL untouched; link text is never modified.
func rewritePageAnchors(pages []fetchedPage) {
	for i := range pages {
		page := &pages[i]
		anchors := dom.GetElementsByTagName(page.article.Node, "a")
		for _, anchor := range anchors {
			href := dom.GetAttribute(anchor, "href")
			if href == "" || strings.HasPrefix(strings.TrimSpace(href), "javascript:") {
				continue
			}

			absolute, err := absoluteLinkURL(page.baseURL, href)
			if err != nil || absolute == nil {
				continue
			}

			target, fragment, ok := locateCollectedTarget(pages, absolute)
			if !ok || fragment == "" {
				// External page, collected page without fragment or any
				// other unresolvable reference: keep the absolute address.
				dom.SetAttribute(anchor, "href", absolute.String())
				continue
			}

			// url.Parse already percent-decodes Fragment exactly once, so
			// it is used as-is. A literal "+" therefore survives and is
			// never turned into a space (QueryUnescape would do that).
			newID, exist := target.ids[fragment]
			if !exist || target.idCounts[fragment] != 1 {
				// Duplicated ids and targets cleaned out of the article
				// keep their absolute address; no target is picked.
				dom.SetAttribute(anchor, "href", absolute.String())
				continue
			}

			dom.SetAttribute(anchor, "href", "#"+newID)
		}
	}
}

// absoluteLinkURL resolves href against the page's base address. Hash-only
// references are resolved against the base itself.
func absoluteLinkURL(baseURL *nurl.URL, href string) (*nurl.URL, error) {
	ref, err := nurl.Parse(href)
	if err != nil {
		return nil, err
	}
	return baseURL.ResolveReference(ref), nil
}

// locateCollectedTarget finds the collected page addressed by u, matching
// either its request or final response URL with fragments ignored. It
// returns the page and the link's raw fragment.
func locateCollectedTarget(pages []fetchedPage, u *nurl.URL) (*fetchedPage, string, bool) {
	key := pageURLKey(u)
	for i := range pages {
		page := &pages[i]
		if pageURLKey(page.requestURL) == key || pageURLKey(page.finalURL) == key {
			return page, u.Fragment, true
		}
	}
	return nil, "", false
}
