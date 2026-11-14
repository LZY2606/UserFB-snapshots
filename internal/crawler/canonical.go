package crawler

import (
	"net/url"
	"slices"
	"strings"

	"golang.org/x/net/html"
	"golang.org/x/net/html/atom"

	"github.com/s0rg/crawley/internal/links"
)

// urlOrigin is a normalized scheme/hostname/effective-port triple.
type urlOrigin struct {
	Scheme string
	Host   string
	Port   string
}

func defaultPort(scheme string) (rv string) {
	switch scheme {
	case "http":
		rv = "80"
	case "https":
		rv = "443"
	}

	return
}

func isHTTP(u *url.URL) (yes bool) {
	return u.Scheme == "http" || u.Scheme == "https"
}

func originOf(u *url.URL) (rv urlOrigin) {
	rv.Scheme = strings.ToLower(u.Scheme)
	rv.Host = strings.ToLower(u.Hostname())
	rv.Port = u.Port()

	if rv.Port == "" {
		rv.Port = defaultPort(rv.Scheme)
	}

	return
}

func sameOrigin(a, b *url.URL) (yes bool) {
	return originOf(a) == originOf(b)
}

// normalizeURL builds URL identity: lower-cased scheme and hostname, no default
// port, no fragment, empty path becomes "/". Query order, path case, escapes
// and trailing slashes are preserved. Only http(s) URLs have identity.
func normalizeURL(s string) (rv string, ok bool) {
	u, err := url.Parse(s)
	if err != nil {
		return "", false
	}

	u.Scheme = strings.ToLower(u.Scheme)

	if !isHTTP(u) {
		return "", false
	}

	host := strings.ToLower(u.Hostname())
	if host == "" {
		return "", false
	}

	if port := u.Port(); port != "" && port != defaultPort(u.Scheme) {
		host += ":" + port
	}

	u.Host = host
	u.Fragment = ""
	u.RawFragment = ""

	if u.Path == "" {
		u.Path = "/"
	}

	if u.RawPath != "" {
		if p, err := url.PathUnescape(u.RawPath); err != nil || p != u.Path {
			u.RawPath = ""
		}
	}

	return u.String(), true
}

// urlIdentity returns normalized identity for given URL,
// falling back to the original string for non-http(s) ones.
func urlIdentity(s string) (rv string) {
	if n, ok := normalizeURL(s); ok {
		return n
	}

	return s
}

// resolveHeadBase computes effective base URL for a document: first head <base>
// href resolved against the final document URL. Empty, invalid or non-HTTP
// values fall back to the final document URL itself.
func resolveHeadBase(doc *url.URL, head links.HeadInfo) (rv *url.URL) {
	if head.HasBase && head.BaseHref != "" {
		if ref, err := url.Parse(head.BaseHref); err == nil {
			if u := doc.ResolveReference(ref); u != nil && isHTTP(u) {
				return u
			}
		}
	}

	return doc
}

// pickCanonical selects the first head canonical href that is non-empty,
// resolves to an HTTP(S) URL without userinfo and shares origin with the
// final document URL. Other candidates are skipped.
func pickCanonical(doc, base *url.URL, hrefs []string) (rv string, ok bool) {
	for _, h := range hrefs {
		if h == "" {
			continue
		}

		ref, err := url.Parse(h)
		if err != nil {
			continue
		}

		u := base.ResolveReference(ref)
		if u == nil || !isHTTP(u) || u.User != nil || !sameOrigin(doc, u) {
			continue
		}

		return u.String(), true
	}

	return "", false
}

// withoutCanonicalLinks wraps given token filter, excluding canonical link
// tags from ordinary link extraction - they are control info, not links.
func withoutCanonicalLinks(f links.TokenFilter) links.TokenFilter {
	return func(t html.Token) (ok bool) {
		if t.DataAtom == atom.Link {
			for i := 0; i < len(t.Attr); i++ {
				if a := &t.Attr[i]; a.Key == "rel" && links.HasRelToken(a.Val, "canonical") {
					return false
				}
			}
		}

		return f(t)
	}
}

// canonicalState holds per-Run identity graph, dispatch marks and output
// candidates. It is only accessed from the scheduler goroutine.
type canonicalState struct {
	parent     map[string]string // undirected equivalence (union-find)
	edges      map[string]string // directed identity edges
	dispatched map[string]bool   // per group root
	candidates []string
}

func newCanonicalState() (rv *canonicalState) {
	return &canonicalState{
		parent:     make(map[string]string),
		edges:      make(map[string]string),
		dispatched: make(map[string]bool),
	}
}

func (cs *canonicalState) find(x string) (root string) {
	p, ok := cs.parent[x]
	if !ok {
		cs.parent[x] = x

		return x
	}

	if p == x {
		return x
	}

	root = cs.find(p)
	cs.parent[x] = root

	return
}

// addEdge records directed identity edge from -> to and merges their
// undirected equivalence groups. Endpoints must be normalized identities.
func (cs *canonicalState) addEdge(from, to string) {
	if _, ok := cs.edges[from]; !ok {
		cs.edges[from] = to
	}

	rf, rt := cs.find(from), cs.find(to)
	if rf == rt {
		return
	}

	cs.parent[rf] = rt
	cs.dispatched[rt] = cs.dispatched[rt] || cs.dispatched[rf]
}

// dispatchedTo reports if given identity belongs to a group
// with at least one already dispatched URL.
func (cs *canonicalState) dispatchedTo(id string) (yes bool) {
	return cs.dispatched[cs.find(id)]
}

// dispatch marks the whole group of given identity as dispatched.
func (cs *canonicalState) dispatch(id string) {
	cs.dispatched[cs.find(id)] = true
}

func (cs *canonicalState) addCandidate(u string) {
	cs.candidates = append(cs.candidates, u)
}

// representative follows directed edges to a sink. On cycles the
// lexicographically smallest identity inside the cycle is selected;
// any predecessor flowing into a cycle resolves to the same value.
func (cs *canonicalState) representative(id string) (rv string) {
	var (
		seen = make(map[string]int)
		path []string
	)

	cur := id

	for {
		if idx, ok := seen[cur]; ok {
			rv = cur

			for _, n := range path[idx:] {
				if n < rv {
					rv = n
				}
			}

			return
		}

		seen[cur] = len(path)
		path = append(path, cur)

		next, ok := cs.edges[cur]
		if !ok {
			return cur
		}

		cur = next
	}
}

// results maps every candidate to its final representative,
// dedups and sorts them by absolute URL byte order.
func (cs *canonicalState) results() (rv []string) {
	uniq := make(map[string]struct{})

	for _, cand := range cs.candidates {
		uniq[cs.representative(urlIdentity(cand))] = struct{}{}
	}

	rv = make([]string, 0, len(uniq))

	for u := range uniq {
		rv = append(rv, u)
	}

	slices.Sort(rv)

	return
}
