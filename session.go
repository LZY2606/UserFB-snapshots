package sitemap

import (
	"fmt"
	"net/url"
	"sort"
	"strings"
	"time"
)

// Snapshot is the result of one Session.Sync round. All slices are sorted and
// independent of the session's internal state.
type Snapshot struct {
	// URLs is the set of page locs reachable from the leaf sitemap documents.
	URLs []string
	// Added are pages not present in the previous successful snapshot.
	Added []string
	// Removed are pages present in the previous successful snapshot but gone now.
	Removed []string
}

// sessionDoc is the state kept for one reachable document between rounds.
type sessionDoc struct {
	// body is the last successfully downloaded document body.
	body []byte
	// commonTime is the single lastmod instant shared by every declaration of
	// this document in the round that produced body.
	commonTime time.Time
	// hasTime reports whether commonTime is meaningful.
	hasTime bool
}

// rawDoc is a document body obtained while walking one round, either freshly
// downloaded or tentatively carried over from the previous round.
type rawDoc struct {
	body    []byte
	isIndex bool
	index   Index
	sitemap Sitemap
	// reused reports that body came from the previous round instead of a
	// download this round.
	reused bool
}

// indexDeclaration is one <sitemap> entry after loc resolution.
type indexDeclaration struct {
	target  string
	lastmod string
}

// Session keeps cross-round state while repeatedly crawling one sitemap tree.
// A Session is intended to be used sequentially, not concurrently.
type Session struct {
	root string
	// docs is the state saved by the last successful Sync; nil before the
	// first successful round.
	docs map[string]*sessionDoc
	// pages is the page set saved by the last successful Sync.
	pages map[string]bool
}

// NewSession creates a Session for root, which must be an absolute HTTP(S)
// sitemap or sitemapindex URL.
func NewSession(root string) (*Session, error) {
	identity, err := docIdentity(root)
	if err != nil {
		return nil, fmt.Errorf("invalid root sitemap URL %q: %v", root, err)
	}

	u, err := url.Parse(identity)
	if err != nil {
		return nil, fmt.Errorf("invalid root sitemap URL %q: %v", root, err)
	}
	if !u.IsAbs() || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
		return nil, fmt.Errorf("invalid root sitemap URL %q: must be an absolute http or https URL", root)
	}

	return &Session{root: u.String(), docs: nil, pages: nil}, nil
}

// Sync performs one crawl round. mode is "full" (re-download every reachable
// document) or "incremental" (reuse documents from the previous successful
// round when all current parent declarations vouch for them with the same
// lastmod). options is passed through to the fetch closure configured with
// SetFetch.
//
// The root document is fetched every round. On any download or parse failure
// Sync returns an empty Snapshot and an error, and the state saved by the
// previous successful round is untouched.
func (s *Session) Sync(mode string, options interface{}) (Snapshot, error) {
	if mode != "full" && mode != "incremental" {
		return Snapshot{}, fmt.Errorf("unknown sync mode %q: must be %q or %q", mode, "full", "incremental")
	}

	fetcher := newDocFetcher(options)

	// The root is read every round, in every mode.
	rootBody, err := fetcher.fetchDoc(s.root, true)
	if err != nil {
		return Snapshot{}, fmt.Errorf("failed to retrieve %s: %v", s.root, err)
	}

	rootIsIndex, rootIdx, rootSmap, err := classifyRoot(s.root, rootBody)
	if err != nil {
		return Snapshot{}, err
	}

	raw := map[string]*rawDoc{}
	if rootIsIndex {
		// Phase 1: discover the reachable graph and obtain every body.
		// A document can only be reused once ALL declarations of the round
		// vouch for it, so walks may be repeated; the fetcher guarantees a
		// document is downloaded at most once even across repetitions.
		forcedFresh := map[string]bool{}
		for {
			raw = map[string]*rawDoc{
				s.root: {body: rootBody, isIndex: true, index: rootIdx},
			}
			decls := map[string][]indexDeclaration{}

			if err := s.discover(s.root, rootIdx, map[string]bool{}, mode, fetcher, forcedFresh, raw, decls); err != nil {
				return Snapshot{}, err
			}

			invalid := map[string]bool{}
			for target, rd := range raw {
				if !rd.reused {
					continue
				}
				commonTime, hasTime := groupEvidence(decls[target])
				old := s.docs[target]
				if !hasTime || old == nil || !old.hasTime || !old.commonTime.Equal(commonTime) {
					invalid[target] = true
				}
			}
			if len(invalid) == 0 {
				break
			}
			for target := range invalid {
				forcedFresh[target] = true
			}
		}
	} else {
		raw[s.root] = &rawDoc{body: rootBody, isIndex: false, sitemap: rootSmap}
	}

	// Phase 2: rebuild the final reachable graph in memory. Documents and
	// out-edges dropped by a type change or a rewritten index disappear here,
	// even when an earlier speculative walk had visited them.
	reachable := map[string]bool{}
	finalDecls := map[string][]indexDeclaration{}
	if rootIsIndex {
		if err := memWalk(s.root, rootIdx, map[string]bool{}, raw, reachable, finalDecls); err != nil {
			return Snapshot{}, err
		}
	} else {
		reachable[s.root] = true
	}

	next := map[string]*sessionDoc{}
	pages := map[string]bool{}

	for docURL := range reachable {
		rd := raw[docURL]
		doc := &sessionDoc{body: rd.body}
		if docURL != s.root {
			if commonTime, hasTime := groupEvidence(finalDecls[docURL]); hasTime {
				doc.commonTime = commonTime
				doc.hasTime = true
			}
		}
		next[docURL] = doc

		if !rd.isIndex {
			for _, u := range rd.sitemap.URL {
				// Page locs are only trimmed; they are never reinterpreted
				// against the document address.
				pages[strings.TrimSpace(u.Loc)] = true
			}
		}
	}

	snapshot := buildSnapshot(pages, s.pages)

	// The round succeeded: commit the new state atomically.
	s.docs = next
	s.pages = pages
	return snapshot, nil
}

// discover walks index documents, records declarations and makes sure every
// reachable document has a body in raw. Reuse decisions are tentative because
// later parents may declare the same document with conflicting evidence; the
// caller validates every reused body once the whole graph is known.
func (s *Session) discover(docURL string, idx Index, ancestors map[string]bool, mode string, fetcher *docFetcher, forcedFresh map[string]bool, raw map[string]*rawDoc, decls map[string][]indexDeclaration) error {
	if ancestors[docURL] {
		return fmt.Errorf("sitemapindex cycle detected at %s", docURL)
	}

	local, err := collectDeclarations(docURL, idx)
	if err != nil {
		return err
	}

	order := make([]string, 0, len(local))
	seen := map[string]bool{}
	for _, decl := range local {
		decls[decl.target] = append(decls[decl.target], decl)
		if !seen[decl.target] {
			seen[decl.target] = true
			order = append(order, decl.target)
		}
	}

	nextAncestors := make(map[string]bool, len(ancestors)+1)
	for k := range ancestors {
		nextAncestors[k] = true
	}
	nextAncestors[docURL] = true

	for _, childURL := range order {
		if _, done := raw[childURL]; done {
			continue
		}

		var rd *rawDoc
		if mode == "incremental" && !forcedFresh[childURL] && s.reuseSoFar(childURL, decls[childURL]) {
			old := s.docs[childURL]
			isIndex, childIdx, childSmap, err := classifyChild(old.body)
			if err != nil {
				return fmt.Errorf("failed to parse %s in sitemapindex.xml: %v", childURL, err)
			}
			rd = &rawDoc{body: old.body, isIndex: isIndex, index: childIdx, sitemap: childSmap, reused: true}
			// Reused bodies occupy the fetcher cache without a download so
			// that a later forced refresh can still replace them.
			fetcher.cacheReused(childURL, old.body)
		} else {
			body, err := fetcher.fetchFresh(childURL)
			if err != nil {
				return fmt.Errorf("failed to retrieve %s in sitemapindex.xml: %v", childURL, err)
			}
			isIndex, childIdx, childSmap, err := classifyChild(body)
			if err != nil {
				return fmt.Errorf("failed to parse %s in sitemapindex.xml: %v", childURL, err)
			}
			rd = &rawDoc{body: body, isIndex: isIndex, index: childIdx, sitemap: childSmap}
		}
		raw[childURL] = rd

		if rd.isIndex {
			if err := s.discover(childURL, rd.index, nextAncestors, mode, fetcher, forcedFresh, raw, decls); err != nil {
				return err
			}
		}
	}

	return nil
}

// memWalk rebuilds the reachable graph from the bodies collected in raw.
func memWalk(docURL string, idx Index, ancestors map[string]bool, raw map[string]*rawDoc, reachable map[string]bool, decls map[string][]indexDeclaration) error {
	reachable[docURL] = true

	if ancestors[docURL] {
		return fmt.Errorf("sitemapindex cycle detected at %s", docURL)
	}

	local, err := collectDeclarations(docURL, idx)
	if err != nil {
		return err
	}

	nextAncestors := make(map[string]bool, len(ancestors)+1)
	for k := range ancestors {
		nextAncestors[k] = true
	}
	nextAncestors[docURL] = true

	seen := map[string]bool{}
	for _, decl := range local {
		decls[decl.target] = append(decls[decl.target], decl)
		if seen[decl.target] {
			continue
		}
		seen[decl.target] = true

		childURL := decl.target
		if nextAncestors[childURL] {
			return fmt.Errorf("sitemapindex cycle detected at %s", childURL)
		}
		if reachable[childURL] {
			// Shared, acyclic subgraph: walk it only once.
			continue
		}

		rd, ok := raw[childURL]
		if !ok {
			return fmt.Errorf("unresolved sitemap reference %s", childURL)
		}
		if rd.isIndex {
			if err := memWalk(childURL, rd.index, nextAncestors, raw, reachable, decls); err != nil {
				return err
			}
		} else {
			reachable[childURL] = true
		}
	}

	return nil
}

// reuseSoFar applies the reuse rule to the declarations of childURL seen up to
// this point: every declaration must carry a valid lastmod, they must all name
// the same instant, and that instant must equal the evidence saved with the
// document in the previous round.
func (s *Session) reuseSoFar(childURL string, group []indexDeclaration) bool {
	commonTime, hasTime := groupEvidence(group)
	if !hasTime {
		return false
	}
	old, ok := s.docs[childURL]
	if !ok || !old.hasTime || !old.commonTime.Equal(commonTime) {
		return false
	}
	return true
}

// groupEvidence reduces a document's declarations to a single instant. It
// succeeds only when every declaration has a parseable lastmod and all of them
// represent the same instant.
func groupEvidence(group []indexDeclaration) (time.Time, bool) {
	var common time.Time
	for i, decl := range group {
		t, ok := parseLastMod(decl.lastmod)
		if !ok {
			return time.Time{}, false
		}
		if i == 0 {
			common = t
		} else if !common.Equal(t) {
			return time.Time{}, false
		}
	}
	return common, len(group) > 0
}

// parseLastMod accepts YYYY-MM-DD (normalized to UTC midnight) and timezone
// aware RFC3339 timestamps.
func parseLastMod(value string) (time.Time, bool) {
	value = strings.TrimSpace(value)
	if value == "" {
		return time.Time{}, false
	}

	if len(value) == len("2006-01-02") {
		if t, err := time.Parse("2006-01-02", value); err == nil {
			return t.UTC(), true
		}
		return time.Time{}, false
	}

	if t, err := time.Parse(time.RFC3339, value); err == nil {
		return t, true
	}
	return time.Time{}, false
}

// collectDeclarations resolves every loc of an index against the declaring
// document and returns the declarations in their original order.
func collectDeclarations(docURL string, idx Index) ([]indexDeclaration, error) {
	decls := make([]indexDeclaration, 0, len(idx.Sitemap))
	for _, part := range idx.Sitemap {
		target, err := resolveDocURL(docURL, part.Loc)
		if err != nil {
			return nil, fmt.Errorf("failed to parse %s in sitemapindex.xml: %v", part.Loc, err)
		}
		decls = append(decls, indexDeclaration{target: target, lastmod: part.LastMod})
	}
	return decls, nil
}

// buildSnapshot turns the reachable page set into a sorted Snapshot and
// computes the diff against the previous successful round.
func buildSnapshot(pages map[string]bool, previous map[string]bool) Snapshot {
	urls := make([]string, 0, len(pages))
	for loc := range pages {
		urls = append(urls, loc)
	}
	sort.Strings(urls)

	added := make([]string, 0)
	removed := make([]string, 0)
	for loc := range pages {
		if previous == nil || !previous[loc] {
			added = append(added, loc)
		}
	}
	if previous != nil {
		for loc := range previous {
			if !pages[loc] {
				removed = append(removed, loc)
			}
		}
	}
	sort.Strings(added)
	sort.Strings(removed)

	return Snapshot{URLs: urls, Added: added, Removed: removed}
}
