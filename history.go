package rss

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/SlyMarbo/rss/v2/atom1"
)

// History is the result of capturing an Atom feed together with its
// [RFC 5005] archive chain.
//
// Feed holds the merged channel metadata (taken from the entry
// document) and all items reachable through the chain. Documents
// lists the final response URLs of every document that contributed
// to the result, starting with the entry and ending with the oldest
// archive.
//
// [RFC 5005]: https://datatracker.ietf.org/doc/html/rfc5005
type History struct {
	Feed      *Feed
	Documents []string

	// entry is the normalized subscription entry this History
	// was captured from. It is used to reject histories that
	// belong to a different entry.
	entry string
}

// historyDocument is a single parsed document in an archive chain.
// Values of this type are immutable once parsed and may be shared
// through the Reader's cache.
type historyDocument struct {
	requestURL string // Normalized address that was requested.
	finalURL   string // Final response address, without a fragment.
	feed       *atom1.Feed
	updated    time.Time // Feed-level updated time.
	archive    bool      // Whether the feed declares fh:archive.
	prevURL    string    // Resolved prev-archive request address, if any.
}

// historyModes are the accepted values for the mode argument of
// [Reader.CaptureHistory].
const (
	historyModeFull        = "full"
	historyModeIncremental = "incremental"
)

// CaptureHistory downloads an Atom subscription entry and its
// [RFC 5005] prev-archive chain.
//
// The entry is always requested with a fresh, unconditional GET.
// When mode is "full", the entire archive chain is downloaded
// again. When mode is "incremental" and previous is not nil, archive
// documents from the previous capture that were fetched successfully
// and carry the fh:archive marker may be reused, and their saved
// prev-archive links followed. Only the entry and the archives
// actually reachable in this capture contribute to the result.
//
// Documents are returned in entry-to-oldest order. Items are merged
// by atom:id; when the same ID occurs in multiple documents the
// document with the newer feed-level updated time wins, with ties
// going to the earlier document. The position of an item is fixed at
// its first occurrence. Items retain their read state from
// previous.Feed, and Unread is recalculated.
//
// Any failure (a non-200 response, a read or close error, an invalid
// updated time, a duplicate request address, or an archive cycle)
// returns nil and an error, leaving previous and the Reader's cache
// untouched.
//
// [RFC 5005]: https://datatracker.ietf.org/doc/html/rfc5005
func (r *Reader) CaptureHistory(ctx context.Context, start string, previous *History, mode string) (*History, error) {
	if ctx == nil {
		return nil, errors.New("rss: nil context")
	}

	switch mode {
	case historyModeFull, historyModeIncremental:
	default:
		return nil, fmt.Errorf("rss: invalid history mode %q", mode)
	}

	entry, err := historyEntryURL(start)
	if err != nil {
		return nil, err
	}

	if previous != nil {
		if previous.Feed == nil {
			return nil, errors.New("rss: previous history has no feed")
		}
		if previous.entry != "" && previous.entry != entry {
			return nil, fmt.Errorf("rss: previous history was captured from %q, not %q", previous.entry, entry)
		}
	}

	// The cache only holds archives of the entry being captured, and
	// entries never change, so a previous History from the same entry
	// is all an incremental capture needs.
	reuse := mode == historyModeIncremental && previous != nil
	var cached map[string]*historyDocument
	if reuse {
		r.historyMu.Lock()
		cached = r.history[entry]
		r.historyMu.Unlock()
	}

	var allowed map[string]bool
	if reuse {
		allowed = make(map[string]bool, len(previous.Documents))
		for _, document := range previous.Documents {
			allowed[historyStripFragment(document)] = true
		}
	}

	requested := make(map[string]bool)
	final := make(map[string]bool)

	root, err := r.fetchHistoryDocument(ctx, entry, requested, final)
	if err != nil {
		return nil, err
	}

	documents := []*historyDocument{root}
	for current := root; current.prevURL != ""; {
		next := current.prevURL

		if document, ok := cached[next]; ok && document.archive && allowed[document.finalURL] {
			// The cached archive was part of the previous result
			// and is still on the chain, so it is immutable and
			// can be reused. Detecting its address again means the
			// chain repeats or loops.
			if requested[next] {
				return nil, fmt.Errorf("rss: duplicate history request address %q", next)
			}
			if final[document.finalURL] {
				return nil, fmt.Errorf("rss: history archive cycle at %q", document.finalURL)
			}
			requested[next] = true
			final[document.finalURL] = true
			documents = append(documents, document)
			current = document
			continue
		}

		document, err := r.fetchHistoryDocument(ctx, next, requested, final)
		if err != nil {
			return nil, err
		}
		if !document.archive {
			return nil, fmt.Errorf("rss: history archive %q does not declare fh:archive", next)
		}
		documents = append(documents, document)
		current = document
	}

	history := r.mergeHistory(documents, previous)

	// Only now that every document has been read, closed, and parsed
	// successfully is the cache replaced. Documents are indexed both
	// by the address that was requested and, when a redirect changed
	// it, by the final response address, as later chains may link
	// either.
	cache := make(map[string]*historyDocument, len(documents)*2)
	for _, document := range documents[1:] {
		if !document.archive {
			continue
		}
		cache[document.requestURL] = document
		cache[document.finalURL] = document
	}
	r.historyMu.Lock()
	if r.history == nil {
		r.history = make(map[string]map[string]*historyDocument)
	}
	r.history[entry] = cache
	r.historyMu.Unlock()

	return history, nil
}

// fetchHistoryDocument requests and parses one document. The
// requested and final maps are used to reject duplicate request
// addresses and archive cycles.
func (r *Reader) fetchHistoryDocument(ctx context.Context, requestURL string, requested, final map[string]bool) (*historyDocument, error) {
	if requested[requestURL] {
		return nil, fmt.Errorf("rss: duplicate history request address %q", requestURL)
	}
	requested[requestURL] = true

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, requestURL, nil)
	if err != nil {
		return nil, fmt.Errorf("rss: failed to construct history GET request: %v", err)
	}

	res, err := r.http.Do(req)
	if err != nil {
		return nil, fmt.Errorf("rss: failed to request history document %q: %v", requestURL, err)
	}

	body, readErr := io.ReadAll(res.Body)
	closeErr := res.Body.Close()
	if res.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("rss: history document %q returned HTTP status %d", requestURL, res.StatusCode)
	}
	if readErr != nil {
		return nil, fmt.Errorf("rss: failed to read history document %q: %v", requestURL, readErr)
	}
	if closeErr != nil {
		return nil, fmt.Errorf("rss: failed to close history document %q: %v", requestURL, closeErr)
	}

	finalURL := requestURL
	if res.Request != nil && res.Request.URL != nil {
		finalURL = historyStripFragment(res.Request.URL.String())
	}
	if final[finalURL] {
		return nil, fmt.Errorf("rss: history archive cycle at %q", finalURL)
	}
	final[finalURL] = true

	return r.parseHistoryDocument(requestURL, finalURL, body)
}

// parseHistoryDocument parses an Atom document and extracts the
// archive metadata needed by history capture.
func (r *Reader) parseHistoryDocument(requestURL, finalURL string, body []byte) (*historyDocument, error) {
	feed, err := atom1.Parse(bytes.NewReader(body))
	if err != nil {
		return nil, fmt.Errorf("rss: failed to parse history document %q: %v", requestURL, err)
	}

	updated, err := historyUpdated(feed.Updated)
	if err != nil {
		return nil, fmt.Errorf("rss: history document %q: %v", requestURL, err)
	}

	document := &historyDocument{
		requestURL: requestURL,
		finalURL:   finalURL,
		feed:       feed,
		updated:    updated,
		archive:    feed.Archive != nil,
	}

	var prev *atom1.Link
	for i := range feed.Link {
		link := &feed.Link[i]
		// Only direct children of the feed, in the Atom namespace,
		// are archive links. Entry links and links in other
		// namespaces are ignored, and only the first prev-archive
		// link of a document is used.
		if link.XMLName.Space != atom1.AtomNamespace || link.Rel != "prev-archive" || prev != nil {
			continue
		}
		prev = link
	}

	if prev != nil {
		prevURL, err := historyResolveLink(finalURL, feed.XMLBase, *prev)
		if err != nil {
			return nil, fmt.Errorf("rss: history document %q has an invalid prev-archive link: %v", requestURL, err)
		}
		document.prevURL = prevURL
	}

	return document, nil
}

// mergeHistory combines the documents of one capture into a new
// feed, without sharing mutable values with previous or the cache.
func (r *Reader) mergeHistory(documents []*historyDocument, previous *History) *History {
	items := make([]*Item, 0)
	positions := make(map[string]int)
	updated := make(map[string]time.Time)

	var root *Feed
	for _, document := range documents {
		converted, err := r.ConvertAtom1(document.finalURL, document.feed)
		if err != nil {
			// ConvertAtom1 does not currently fail, but keep the
			// result usable if that ever changes.
			if root == nil {
				root = &Feed{
					UpdateURL:  document.finalURL,
					NextUpdate: r.now().Add(r.defaultUpdateInterval),
				}
			}
			continue
		}
		if root == nil {
			// Channel metadata comes from the entry document.
			root = converted
		}

		for _, item := range converted.Items {
			if item == nil || item.ID == "" {
				continue
			}

			if position, ok := positions[item.ID]; ok {
				// The document with the newer feed-level updated
				// time wins. Ties keep the document encountered
				// first, and the item keeps its original position.
				if document.updated.After(updated[item.ID]) {
					items[position] = item
					updated[item.ID] = document.updated
				}
				continue
			}

			positions[item.ID] = len(items)
			updated[item.ID] = document.updated
			items = append(items, item)
		}
	}

	read := make(map[string]bool)
	if previous != nil && previous.Feed != nil {
		for _, item := range previous.Feed.Items {
			if item != nil {
				read[item.ID] = item.Read
			}
		}
	}

	var unread uint32
	for _, item := range items {
		item.Read = read[item.ID]
		if !item.Read {
			unread++
		}
	}

	root.Items = items
	root.Unread = unread

	urls := make([]string, len(documents))
	for i, document := range documents {
		urls[i] = document.finalURL
	}

	return &History{
		Feed:      root,
		Documents: urls,
		entry:     documents[0].requestURL,
	}
}

// historyEntryURL validates and normalizes a subscription entry
// address.
func historyEntryURL(start string) (string, error) {
	parsed, err := url.Parse(start)
	if err != nil {
		return "", fmt.Errorf("rss: invalid history entry URL %q: %v", start, err)
	}
	if parsed.Host == "" || !historyHTTPScheme(parsed.Scheme) {
		return "", fmt.Errorf("rss: invalid history entry URL %q", start)
	}
	parsed.Fragment = ""
	return parsed.String(), nil
}

// historyResolveLink resolves a prev-archive href starting from the
// final response address, applying the feed's and then the link's
// xml:base.
func historyResolveLink(documentURL, feedBase string, link atom1.Link) (string, error) {
	base, err := url.Parse(documentURL)
	if err != nil {
		return "", err
	}

	for _, raw := range []string{feedBase, link.XMLBase} {
		if raw == "" {
			continue
		}
		reference, err := url.Parse(raw)
		if err != nil {
			return "", err
		}
		base = base.ResolveReference(reference)
	}

	if link.Href == "" {
		return "", errors.New("missing href")
	}
	reference, err := url.Parse(link.Href)
	if err != nil {
		return "", err
	}

	target := base.ResolveReference(reference)
	target.Fragment = ""
	if target.Host == "" || !historyHTTPScheme(target.Scheme) {
		return "", fmt.Errorf("unsupported address %q", target.String())
	}

	return target.String(), nil
}

// historyStripFragment removes a URL fragment, which is not part of
// the document address.
func historyStripFragment(address string) string {
	parsed, err := url.Parse(address)
	if err != nil {
		return address
	}
	parsed.Fragment = ""
	return parsed.String()
}

func historyHTTPScheme(scheme string) bool {
	return strings.EqualFold(scheme, "http") || strings.EqualFold(scheme, "https")
}

// historyUpdated validates the feed-level updated timestamp. Archive
// ordering requires an RFC 3339 time with a time zone.
func historyUpdated(value string) (time.Time, error) {
	value = strings.TrimSpace(value)
	if value == "" {
		return time.Time{}, errors.New("missing feed updated time")
	}

	updated, err := time.Parse(time.RFC3339Nano, value)
	if err != nil {
		return time.Time{}, fmt.Errorf("invalid feed updated time %q: %v", value, err)
	}

	return updated, nil
}
