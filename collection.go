package gofeed

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
)

// collectionFile is the name of the snapshot stored inside an OpenCollection
// directory.
const collectionFile = "collection.json"

// Identity kinds used inside a single source. GUID identities and fallback
// link identities live in separate namespaces: an item whose GUID string
// equals one of its links must not collapse into the link-identified item.
const (
	identityKindGUID = "guid"
	identityKindLink = "link"
)

// ErrInvalidCollectionSource is returned when Collect or Remove is handed a
// sourceURL that is not an absolute http or https URL.
var ErrInvalidCollectionSource = errors.New("gofeed: collection source must be an absolute http or https URL")

// ItemOrigin points at one of the folded universal items inside a
// CollectionSnapshot source.
type ItemOrigin struct {
	SourceURL string `json:"sourceURL"`
	ItemIndex int    `json:"itemIndex"`
}

// CollectedRecord is one deduplicated article. Item is the representative
// observation (the first member when the group's members are ordered), and
// Origins lists every observation that currently belongs to the article.
type CollectedRecord struct {
	Item    *Item        `json:"item"`
	Origins []ItemOrigin `json:"origins"`
}

// SourceSnapshot keeps the full, translated feed observation of one source:
// the key the caller used, the final address the body came from, and the
// folded universal items (one per in-source identity, in first-seen order).
type SourceSnapshot struct {
	URL      string  `json:"url"`
	FinalURL string  `json:"finalURL"`
	Items    []*Item `json:"items"`
}

// CollectionSnapshot is the complete, recomputed state of a Collection.
type CollectionSnapshot struct {
	Sources []SourceSnapshot  `json:"sources"`
	Records []CollectedRecord `json:"records"`
}

// sourceEntry is the parsed identity of one folded item inside a source. The
// entry's slice position matches its Item's position in SourceSnapshot.Items.
type sourceEntry struct {
	kind  string // identityKindGUID or identityKindLink
	id    string // raw GUID string, or the first valid canonical link
	links []string
	// firstIndex is the physical feed position at which this identity first
	// appeared. Duplicate identities keep this position even though their Item
	// payload is overwritten by the last occurrence.
	firstIndex int
}

// sourceObservation is everything the collection remembers about one source.
type sourceObservation struct {
	snapshot SourceSnapshot
	entries  []sourceEntry
}

// Collection is a local, on-disk set of feed observations. Each source keeps
// its raw observations; article grouping is recomputed from scratch on every
// change, so a link that used to bridge two articles but has disappeared no
// longer leaves a historical merge behind.
//
// A Collection is safe for serial use: every operation holds the same mutex,
// so Collect, Remove and Snapshot see a consistent sequence of states.
type Collection struct {
	dir    string
	parser *Parser
	mu     sync.Mutex
	// sources is keyed by the caller-provided source key.
	sources map[string]*sourceObservation
}

// OpenCollection opens the collection persisted in directory, creating the
// directory if needed. The parser is used for every subsequent Collect; its
// Client, context handling, authentication, byte limit and custom
// translators all remain in effect. An existing collection.json is reloaded
// without issuing any requests; a corrupt or self-contradictory file is
// rejected rather than trusted.
func OpenCollection(directory string, parser *Parser) (*Collection, error) {
	if parser == nil {
		return nil, errors.New("gofeed: OpenCollection requires a non-nil Parser")
	}
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return nil, err
	}
	c := &Collection{
		dir:     directory,
		parser:  parser,
		sources: map[string]*sourceObservation{},
	}
	if err := c.load(); err != nil {
		return nil, err
	}
	return c, nil
}

// Collect fetches and parses sourceURL with the collection's Parser, then
// replaces the whole observation for that source: items missing from the
// feed are withdrawn. The source key is sourceURL with its fragment removed
// and is never renamed to the final response address. On any failure the
// previous on-disk file and in-memory snapshot are left untouched.
func (c *Collection) Collect(ctx context.Context, sourceURL string) (*CollectionSnapshot, error) {
	key, err := collectionSourceKey(sourceURL)
	if err != nil {
		return nil, err
	}

	// Fetch and fully build the new observation before touching state, so a
	// network or classification failure leaves the existing snapshot
	// byte-for-byte intact.
	obs, err := c.fetch(ctx, key)
	if err != nil {
		return nil, err
	}

	c.mu.Lock()
	defer c.mu.Unlock()

	previous, hadPrevious := c.sources[key]
	c.sources[key] = obs
	snapshot := c.buildLocked()
	if err := c.persist(snapshot); err != nil {
		// Roll the in-memory state back so a failed write never diverges from
		// the surviving on-disk file.
		if hadPrevious {
			c.sources[key] = previous
		} else {
			delete(c.sources, key)
		}
		return nil, err
	}
	return snapshot, nil
}

// Remove withdraws every observation recorded under sourceURL and rebuilds
// the article groups. Removing an unknown source is a successful no-op.
func (c *Collection) Remove(sourceURL string) (*CollectionSnapshot, error) {
	key, err := collectionSourceKey(sourceURL)
	if err != nil {
		return nil, err
	}
	c.mu.Lock()
	defer c.mu.Unlock()

	previous, ok := c.sources[key]
	if !ok {
		// Unknown source: nothing to withdraw, nothing to persist.
		return c.buildLocked(), nil
	}
	delete(c.sources, key)

	snapshot := c.buildLocked()
	if err := c.persist(snapshot); err != nil {
		c.sources[key] = previous
		return nil, err
	}
	return snapshot, nil
}

// Snapshot returns a deep copy of the current collection state. Callers may
// mutate the returned snapshot, including its Items, without affecting the
// collection or any later result.
func (c *Collection) Snapshot() *CollectionSnapshot {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.buildLocked()
}

// fetch performs the network round-trip and turns the response into a new
// source observation. It does not mutate collection state.
func (c *Collection) fetch(ctx context.Context, key string) (*sourceObservation, error) {
	feed, finalURL, err := c.parser.parseURLFinal(key, ctx)
	if err != nil {
		return nil, err
	}
	// Fragments are never transmitted to a server, so they are not part of
	// the address the body came from; drop one the client kept around.
	if u, perr := url.Parse(finalURL); perr == nil {
		u.Fragment = ""
		u.RawFragment = ""
		finalURL = u.String()
	}
	if err := validateHTTPURL(finalURL); err != nil {
		return nil, fmt.Errorf("gofeed: collection final URL %q is unusable: %w", finalURL, err)
	}
	if feed == nil {
		return nil, errors.New("gofeed: parser returned no feed")
	}

	// Folded items and entries grow in lockstep: one slot per in-source
	// identity, ordered by first appearance.
	items := make([]*Item, 0, len(feed.Items))
	entries := make([]sourceEntry, 0, len(feed.Items))
	seen := map[string]int{} // identity "kind\x00id" -> folded slot

	for i, item := range feed.Items {
		entry, err := classifyEntry(item, finalURL)
		if err != nil {
			return nil, fmt.Errorf("gofeed: collection item %d of %s: %w", i, key, err)
		}
		identityKey := entry.kind + "\x00" + entry.id
		if pos, ok := seen[identityKey]; ok {
			// Repeated identity: keep the first position, overwrite the Item
			// payload and link set with the last occurrence.
			entries[pos].links = entry.links
			items[pos] = cloneItem(item)
			continue
		}
		seen[identityKey] = len(entries)
		entry.firstIndex = i
		entries = append(entries, entry)
		items = append(items, cloneItem(item))
	}

	return &sourceObservation{
		snapshot: SourceSnapshot{
			URL:      key,
			FinalURL: finalURL,
			Items:    items,
		},
		entries: entries,
	}, nil
}

// persist atomically writes snapshot to directory/collection.json via a
// temporary file in the same directory, then renames it into place. The
// trailing newline is part of the written payload.
func (c *Collection) persist(snapshot *CollectionSnapshot) error {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	enc.SetIndent("", "  ")
	if err := enc.Encode(snapshot); err != nil {
		return err
	}
	data := buf.Bytes() // Encoder.Encode already appends exactly one newline.

	tmp, err := os.CreateTemp(c.dir, ".collection-*.tmp")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	committed := false
	defer func() {
		if !committed {
			tmp.Close()
			os.Remove(tmpName)
		}
	}()
	if _, err := tmp.Write(data); err != nil {
		return err
	}
	if err := tmp.Sync(); err != nil {
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmpName, filepath.Join(c.dir, collectionFile)); err != nil {
		return err
	}
	committed = true
	return nil
}

// load reads and verifies an existing collection.json without any network
// access. A missing file simply means an empty collection.
func (c *Collection) load() error {
	path := filepath.Join(c.dir, collectionFile)
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}

	var snap CollectionSnapshot
	dec := json.NewDecoder(bytes.NewReader(data))
	if err := dec.Decode(&snap); err != nil {
		return fmt.Errorf("gofeed: collection file %s is corrupt: %w", path, err)
	}
	// A second top-level value means trailing data; only io.EOF is expected.
	var extra json.RawMessage
	if err := dec.Decode(&extra); !errors.Is(err, io.EOF) {
		if err == nil {
			err = errors.New("unexpected trailing JSON value")
		}
		return fmt.Errorf("gofeed: collection file %s is corrupt: %w", path, err)
	}
	if err := c.ingestSnapshot(&snap); err != nil {
		return fmt.Errorf("gofeed: collection file %s is not trustworthy: %w", path, err)
	}
	return nil
}

// ingestSnapshot rebuilds internal observations from a decoded snapshot and
// cross-checks the stored Records against a fresh rebuild. Nothing in here
// issues requests.
func (c *Collection) ingestSnapshot(snap *CollectionSnapshot) error {
	observed := make(map[string]*sourceObservation, len(snap.Sources))
	prevKey := ""
	for si := range snap.Sources {
		src := &snap.Sources[si]
		if si > 0 && src.URL <= prevKey {
			return errors.New("sources are not sorted by source key or contain duplicates")
		}
		prevKey = src.URL
		key, err := collectionSourceKey(src.URL)
		if err != nil {
			return err
		}
		if key != src.URL {
			return fmt.Errorf("source %q is not a fragment-free source key", src.URL)
		}
		if err := validateHTTPURL(src.FinalURL); err != nil {
			return fmt.Errorf("source %q: %w", src.URL, err)
		}
		if u, perr := url.Parse(src.FinalURL); perr != nil || u.Fragment != "" || u.RawFragment != "" {
			return fmt.Errorf("source %q final URL %q carries a fragment", src.URL, src.FinalURL)
		}

		entries := make([]sourceEntry, 0, len(src.Items))
		items := make([]*Item, 0, len(src.Items))
		seen := map[string]int{}
		for i, item := range src.Items {
			entry, err := classifyEntry(item, src.FinalURL)
			if err != nil {
				return fmt.Errorf("source %q item %d: %w", src.URL, i, err)
			}
			identityKey := entry.kind + "\x00" + entry.id
			if pos, ok := seen[identityKey]; ok {
				entries[pos].links = entry.links
				items[pos] = cloneItem(item)
				continue
			}
			seen[identityKey] = len(entries)
			entry.firstIndex = i
			entries = append(entries, entry)
			items = append(items, cloneItem(item))
		}
		observed[src.URL] = &sourceObservation{
			snapshot: SourceSnapshot{URL: src.URL, FinalURL: src.FinalURL, Items: items},
			entries:  entries,
		}
	}

	c.sources = observed
	rebuilt := c.buildLocked()
	if !recordsEqual(snap.Records, rebuilt.Records) {
		return errors.New("stored records do not match the sources they were computed from")
	}
	return nil
}

// groupMember is one observation taking part in article grouping.
type groupMember struct {
	sourceKey  string
	itemIndex  int // folded position inside the source, used by ItemOrigin
	firstIndex int // physical first-seen position, used for member ordering
	item       *Item
}

// buildLocked recomputes the whole snapshot, including the article groups,
// from the observations currently in c.sources. The caller holds c.mu.
func (c *Collection) buildLocked() *CollectionSnapshot {
	keys := make([]string, 0, len(c.sources))
	for key := range c.sources {
		keys = append(keys, key)
	}
	sort.Strings(keys)

	sources := make([]SourceSnapshot, 0, len(keys))
	var members []groupMember
	// linkIndex maps a canonical link to the first member carrying it; later
	// members sharing it are unioned into the same group.
	linkIndex := map[string]int{}
	uf := newUnionFind(0)

	for _, key := range keys {
		obs := c.sources[key]
		sources = append(sources, cloneSourceSnapshot(obs.snapshot))
		for p, entry := range obs.entries {
			idx := len(members)
			members = append(members, groupMember{
				sourceKey:  key,
				itemIndex:  p,
				firstIndex: entry.firstIndex,
				item:       obs.snapshot.Items[p],
			})
			uf.Grow(1)
			for _, link := range entry.links {
				if other, ok := linkIndex[link]; ok {
					uf.Union(idx, other)
				} else {
					linkIndex[link] = idx
				}
			}
		}
	}

	// Collect members per union-find root, in first-encountered order.
	groups := map[int][]int{}
	var roots []int
	for i := range members {
		root := uf.Find(i)
		if _, ok := groups[root]; !ok {
			roots = append(roots, root)
		}
		groups[root] = append(groups[root], i)
	}

	lessMember := func(a, b int) bool {
		ma, mb := members[a], members[b]
		if ma.sourceKey != mb.sourceKey {
			return ma.sourceKey < mb.sourceKey
		}
		return ma.firstIndex < mb.firstIndex
	}

	ordered := make([][]int, 0, len(roots))
	for _, root := range roots {
		ids := groups[root]
		sort.Slice(ids, func(i, j int) bool { return lessMember(ids[i], ids[j]) })
		ordered = append(ordered, ids)
	}
	// Groups themselves order by their own first member.
	sort.Slice(ordered, func(i, j int) bool {
		return lessMember(ordered[i][0], ordered[j][0])
	})

	records := make([]CollectedRecord, 0, len(ordered))
	for _, ids := range ordered {
		origins := make([]ItemOrigin, 0, len(ids))
		for _, mid := range ids {
			m := members[mid]
			origins = append(origins, ItemOrigin{
				SourceURL: m.sourceKey,
				ItemIndex: m.itemIndex,
			})
		}
		first := members[ids[0]]
		records = append(records, CollectedRecord{
			// Deep-copy the representative item so callers cannot mutate state.
			Item:    cloneItem(first.item),
			Origins: origins,
		})
	}

	return &CollectionSnapshot{Sources: sources, Records: records}
}

// classifyEntry turns one translated item into its in-source identity. A nil
// item, or an item with neither a non-empty GUID nor a usable link, makes the
// whole Collect fail: the caller asked for a complete replacement, not a
// silently partial one.
func classifyEntry(item *Item, baseURL string) (sourceEntry, error) {
	if item == nil {
		return sourceEntry{}, errors.New("nil item")
	}
	links := collectCanonicalLinks(item, baseURL)
	if item.GUID != "" {
		// The GUID is kept as its raw string and is never an identity beyond
		// this source.
		return sourceEntry{kind: identityKindGUID, id: item.GUID, links: links}, nil
	}
	if len(links) == 0 {
		return sourceEntry{}, errors.New("item has no GUID and no usable http/https link")
	}
	return sourceEntry{kind: identityKindLink, id: links[0], links: links}, nil
}

// collectCanonicalLinks gathers Item.Link first and Item.Links after,
// resolving each candidate against the final response URL, dropping
// unparseable or non-HTTP(S) candidates and removing duplicates in
// encounter order.
func collectCanonicalLinks(item *Item, baseURL string) []string {
	candidates := make([]string, 0, 1+len(item.Links))
	if item.Link != "" {
		candidates = append(candidates, item.Link)
	}
	candidates = append(candidates, item.Links...)

	seen := map[string]bool{}
	var out []string
	for _, candidate := range candidates {
		normalized, ok := resolveCanonicalLink(candidate, baseURL)
		if !ok || seen[normalized] {
			continue
		}
		seen[normalized] = true
		out = append(out, normalized)
	}
	return out
}

// resolveCanonicalLink resolves raw against the final response URL and
// canonicalizes it. It returns ok=false for anything that is not an absolute
// http/https URL after resolution.
func resolveCanonicalLink(raw, baseURL string) (string, bool) {
	if strings.TrimSpace(raw) == "" {
		return "", false
	}
	base, err := url.Parse(baseURL)
	if err != nil {
		return "", false
	}
	ref, err := url.Parse(raw)
	if err != nil {
		return "", false
	}
	resolved := base.ResolveReference(ref)
	normalized, err := normalizeHTTPURL(resolved.String())
	if err != nil {
		return "", false
	}
	return normalized, true
}

// validateHTTPURL checks that raw is an absolute http/https URL without
// changing its spelling. It is used where the URL must be stored as observed
// (source keys, final response URLs); link candidates go through
// normalizeHTTPURL instead.
func validateHTTPURL(raw string) error {
	u, err := url.Parse(raw)
	if err != nil {
		return err
	}
	if u.Opaque != "" || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
		return fmt.Errorf("url %q is not an absolute http or https URL", raw)
	}
	return nil
}

// normalizeHTTPURL validates and canonicalizes an absolute http/https URL:
// fragment removed, scheme and host lowercased, default port stripped, empty
// path turned into "/". Escaped path segments and the query string keep their
// original order and content; nothing is guessed or reordered.
func normalizeHTTPURL(raw string) (string, error) {
	u, err := url.Parse(raw)
	if err != nil {
		return "", err
	}
	if u.Opaque != "" || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
		return "", fmt.Errorf("url %q is not an absolute http or https URL", raw)
	}

	scheme := strings.ToLower(u.Scheme)
	host := strings.ToLower(u.Hostname())
	port := u.Port()
	if (scheme == "http" && port == "80") || (scheme == "https" && port == "443") {
		port = ""
	}
	u.Scheme = scheme
	if port != "" {
		u.Host = host + ":" + port
	} else {
		u.Host = host
	}
	u.Fragment = ""
	u.RawFragment = ""
	if u.Path == "" {
		u.Path = "/"
	}
	return u.String(), nil
}

// collectionSourceKey validates sourceURL and returns it with only the
// fragment removed. Case, port and path spelling are preserved, and the key
// never follows redirects.
func collectionSourceKey(sourceURL string) (string, error) {
	u, err := url.Parse(sourceURL)
	if err != nil {
		return "", ErrInvalidCollectionSource
	}
	if u.Opaque != "" || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
		return "", ErrInvalidCollectionSource
	}
	u.Fragment = ""
	u.RawFragment = ""
	return u.String(), nil
}

// parseURLFinal runs the same fetch path as ParseURLWithContext and
// additionally reports the final absolute URL the body came from. A response
// without Request (some custom transports) falls back to the requested URL.
// The Parser's Client, context, authentication, byte limit and translators
// are all honored because the public URL entry point delegates here.
func (f *Parser) parseURLFinal(feedURL string, ctx context.Context) (feed *Feed, finalURL string, err error) {
	client := f.httpClient()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, feedURL, nil)
	if err != nil {
		return nil, "", err
	}
	req.Header.Set("User-Agent", f.UserAgent)
	if f.AuthConfig != nil && f.AuthConfig.Username != "" && f.AuthConfig.Password != "" {
		req.SetBasicAuth(f.AuthConfig.Username, f.AuthConfig.Password)
	}

	resp, err := client.Do(req)
	if err != nil {
		return nil, "", err
	}
	if resp == nil {
		return nil, "", errors.New("gofeed: http client returned no response")
	}
	defer func() {
		if ce := resp.Body.Close(); ce != nil && err == nil {
			err = ce
		}
	}()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, "", HTTPError{StatusCode: resp.StatusCode, Status: resp.Status}
	}

	finalURL = feedURL
	if resp.Request != nil && resp.Request.URL != nil {
		finalURL = resp.Request.URL.String()
	}

	var body io.Reader = resp.Body
	var limited *limitedReader
	if f.MaxByteSize > 0 {
		limited = &limitedReader{r: resp.Body, left: f.MaxByteSize}
		body = limited
	}
	feed, err = f.Parse(body)
	if limited != nil && limited.err == ErrResponseTooLarge {
		return nil, "", ErrResponseTooLarge
	}
	if err != nil {
		return nil, "", err
	}
	// XML parsers stop at the root's closing tag; drain the remainder through
	// the same limit before reporting success, mirroring ParseURLWithContext.
	if limited != nil {
		if _, err = io.Copy(io.Discard, limited); err != nil {
			return nil, "", err
		}
	}
	return feed, finalURL, nil
}

// cloneSourceSnapshot returns an independent copy of src, including every Item.
func cloneSourceSnapshot(src SourceSnapshot) SourceSnapshot {
	items := make([]*Item, len(src.Items))
	for i, item := range src.Items {
		items[i] = cloneItem(item)
	}
	return SourceSnapshot{URL: src.URL, FinalURL: src.FinalURL, Items: items}
}

// cloneItem round-trips one Item through JSON so every slice, map and pointer
// field gets an independent copy. Item only carries JSON-friendly types.
func cloneItem(item *Item) *Item {
	if item == nil {
		return nil
	}
	data, err := json.Marshal(item)
	if err != nil {
		// All Item field types marshal cleanly; a failure here indicates a
		// broken invariant rather than caller input.
		panic(fmt.Sprintf("gofeed: cannot clone item: %v", err))
	}
	var out Item
	if err := json.Unmarshal(data, &out); err != nil {
		panic(fmt.Sprintf("gofeed: cannot clone item: %v", err))
	}
	return &out
}

// recordsEqual compares two record sets structurally, including origins and
// ordering, via their deterministic JSON encoding.
func recordsEqual(a, b []CollectedRecord) bool {
	da, _ := json.Marshal(a)
	db, _ := json.Marshal(b)
	return bytes.Equal(da, db)
}

// unionFind is a small disjoint-set structure used to merge observations that
// share any canonical link, including chains of indirect links.
type unionFind struct {
	parent []int
	rank   []uint8
}

func newUnionFind(n int) *unionFind {
	uf := &unionFind{parent: make([]int, n), rank: make([]uint8, n)}
	for i := range uf.parent {
		uf.parent[i] = i
	}
	return uf
}

func (uf *unionFind) Grow(n int) {
	for i := 0; i < n; i++ {
		uf.parent = append(uf.parent, len(uf.parent))
		uf.rank = append(uf.rank, 0)
	}
}

func (uf *unionFind) Find(x int) int {
	for uf.parent[x] != x {
		uf.parent[x] = uf.parent[uf.parent[x]]
		x = uf.parent[x]
	}
	return x
}

func (uf *unionFind) Union(a, b int) {
	ra, rb := uf.Find(a), uf.Find(b)
	if ra == rb {
		return
	}
	if uf.rank[ra] < uf.rank[rb] {
		ra, rb = rb, ra
	}
	uf.parent[rb] = ra
	if uf.rank[ra] == uf.rank[rb] {
		uf.rank[ra]++
	}
}
