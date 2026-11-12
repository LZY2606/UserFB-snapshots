package mail

import (
	"errors"
	"fmt"
	"io"
	"sort"
	"strings"

	"github.com/emersion/go-message"
)

// ErrNoPresentation is returned by ReadPresentation when the message doesn't
// contain any body part that can be presented. Use errors.Is to check for it.
var ErrNoPresentation = errors.New("mail: message contains no presentable body")

// A Presentation is a single presentable view of a mail message: one selected
// body plus the inline resources that belong to it.
//
// Unlike Reader, which flattens the whole MIME tree, a Presentation keeps the
// relationship between the selected body and its inline resources, so callers
// can tell which images belong to the selected HTML and which ones were part
// of another candidate or an attachment.
type Presentation struct {
	Header    Header            // The message header.
	MediaType string            // The media type of the selected body, e.g. "text/html".
	Body      []byte            // The decoded body, as a standalone byte slice.
	Path      []int             // The path of the body part, using Entity.Walk indices (root is nil).
	Resources []RelatedResource // Inline resources contributed by multipart/related layers on the body's branch, sorted by Path.
}

// A RelatedResource is an inline resource (typically an image) attached to the
// selected body by a multipart/related layer.
type RelatedResource struct {
	ContentID string // The normalized Content-ID, without angle brackets or surrounding whitespace.
	MediaType string // The media type of the resource, e.g. "image/png".
	Body      []byte // The decoded resource body, as a standalone byte slice.
	Path      []int  // The path of the resource part, using Entity.Walk indices.
}

// ReadPresentation reads a mail message from r and extracts a single
// presentable body together with its inline resources.
//
// The preferred list orders the acceptable body media types by decreasing
// preference. Only "text/html" and "text/plain" are allowed, each at most
// once. If preferred is empty, it defaults to "text/html" then "text/plain".
// Any other value results in an error.
//
// The body is selected as follows:
//
//   - A part with an "attachment" Content-Disposition is excluded, along with
//     its whole subtree. message/rfc822 parts are never descended into.
//   - Inside multipart/alternative, a candidate is selected in each subtree
//     and the one with the most preferred media type wins. If several
//     candidates share the same media type, the last one in wire order wins.
//   - Inside multipart/related, the body can only come from the direct child
//     designated by the "start" parameter (compared against Content-ID), or
//     from the first direct child if there is no "start" parameter. The
//     designated root may itself be multipart/alternative or
//     multipart/related. If "start" matches zero or several parts, an error
//     is returned. Other children are never considered as bodies, even if
//     the designated root yields none.
//   - Inside multipart/mixed or any other multipart, the first subtree that
//     yields a body wins.
//
// Resources are only contributed by the multipart/related layers on the
// branch of the selected body. In each such layer, the direct children that
// are not the layer root, not attachments, not multipart and carry a
// Content-ID are collected. Resources from unselected alternative branches
// or from attachments are never included. Content-IDs are compared after
// trimming surrounding whitespace and one pair of angle brackets; the
// comparison is case-sensitive. Duplicate Content-IDs within a single layer
// result in an error; when an inner and an outer layer share a Content-ID,
// the inner resource shadows the outer one. Resources are sorted by Path in
// lexicographical order. Resources don't need to be referenced by the body
// and "cid:" links are left untouched.
//
// ReadPresentation is stricter than CreateReader: malformed structure, read
// failures, unknown transfer encodings and unknown charsets all result in an
// error and a nil Presentation. If no supported body can be found, the
// returned error verifies errors.Is(err, ErrNoPresentation).
func ReadPresentation(r io.Reader, preferred []string) (*Presentation, error) {
	rank, err := preferenceRank(preferred)
	if err != nil {
		return nil, err
	}

	e, err := message.Read(r)
	if err != nil {
		return nil, err
	}

	root, err := readPresentationTree(e)
	if err != nil {
		return nil, err
	}

	s := &presentationSelector{rank: rank}
	leaf, path, err := s.selectBody(root, nil)
	if err != nil {
		return nil, err
	}
	if leaf == nil {
		return nil, ErrNoPresentation
	}

	resources, err := collectRelatedResources(root, path)
	if err != nil {
		return nil, err
	}

	return &Presentation{
		Header:    Header{e.Header},
		MediaType: leaf.mediaType,
		Body:      leaf.body,
		Path:      path,
		Resources: resources,
	}, nil
}

// preferenceRank validates the preferred list and returns a map from media
// type to preference rank (lower is more preferred).
func preferenceRank(preferred []string) (map[string]int, error) {
	if len(preferred) == 0 {
		preferred = []string{"text/html", "text/plain"}
	}
	rank := make(map[string]int, len(preferred))
	for _, t := range preferred {
		if t != "text/html" && t != "text/plain" {
			return nil, fmt.Errorf("mail: unsupported preferred media type %q", t)
		}
		if _, dup := rank[t]; dup {
			return nil, fmt.Errorf("mail: duplicate preferred media type %q", t)
		}
		rank[t] = len(rank)
	}
	return rank, nil
}

// presentationPart is a fully-read node of a message's MIME tree.
type presentationPart struct {
	header      message.Header
	mediaType   string
	mediaParams map[string]string
	disp        string
	multipart   bool
	children    []*presentationPart // multipart only
	body        []byte              // non-multipart only
}

// contentID returns the part's normalized Content-ID, and whether the part
// carries a usable one.
func (p *presentationPart) contentID() (string, bool) {
	v := p.header.Get("Content-ID")
	if v == "" {
		return "", false
	}
	id := normalizeContentID(v)
	return id, id != ""
}

// normalizeContentID trims surrounding whitespace and one pair of angle
// brackets. The result is compared case-sensitively.
func normalizeContentID(s string) string {
	s = strings.TrimSpace(s)
	if len(s) >= 2 && s[0] == '<' && s[len(s)-1] == '>' {
		s = s[1 : len(s)-1]
	}
	return strings.TrimSpace(s)
}

// readPresentationTree reads a whole entity subtree into memory. Any
// structural, read, encoding or charset error is fatal.
func readPresentationTree(e *message.Entity) (*presentationPart, error) {
	mediaType, mediaParams, err := e.Header.ContentType()
	if err != nil {
		return nil, err
	}
	// A missing or malformed Content-Disposition isn't fatal: the part is
	// just not considered an attachment.
	disp, _, _ := e.Header.ContentDisposition()

	part := &presentationPart{
		header:      e.Header,
		mediaType:   mediaType,
		mediaParams: mediaParams,
		disp:        disp,
	}

	mr := e.MultipartReader()
	if mr == nil {
		// A leaf part. Note that message/rfc822 is a leaf too: nested
		// messages are never descended into.
		body, err := io.ReadAll(e.Body)
		if err != nil {
			return nil, err
		}
		part.body = body
		return part, nil
	}

	part.multipart = true
	for {
		p, err := mr.NextPart()
		if err == io.EOF {
			break
		} else if err != nil {
			return nil, err
		}
		child, err := readPresentationTree(p)
		if err != nil {
			return nil, err
		}
		part.children = append(part.children, child)
	}
	return part, nil
}

// presentationSelector selects a body part according to a preference rank.
type presentationSelector struct {
	rank map[string]int
}

// selectBody returns the leaf part chosen as the body of the subtree rooted
// at part, or nil if the subtree yields no presentable body. path is the
// path of part itself.
func (s *presentationSelector) selectBody(part *presentationPart, path []int) (*presentationPart, []int, error) {
	if part.disp == "attachment" {
		return nil, nil, nil
	}

	if !part.multipart {
		if _, ok := s.rank[part.mediaType]; ok {
			return part, path, nil
		}
		return nil, nil, nil
	}

	switch part.mediaType {
	case "multipart/alternative":
		var best *presentationPart
		var bestPath []int
		bestRank := len(s.rank)
		for i, child := range part.children {
			leaf, leafPath, err := s.selectBody(child, appendPath(path, i))
			if err != nil {
				return nil, nil, err
			}
			if leaf == nil {
				continue
			}
			// On a tie, the last candidate in wire order wins.
			if rank := s.rank[leaf.mediaType]; rank <= bestRank {
				best, bestPath, bestRank = leaf, leafPath, rank
			}
		}
		return best, bestPath, nil

	case "multipart/related":
		rootIdx, err := relatedRootIndex(part)
		if err != nil {
			return nil, nil, err
		}
		if rootIdx < 0 {
			return nil, nil, nil
		}
		// Only the designated root may provide the body. If it yields
		// none, don't fall back to another displayable leaf.
		return s.selectBody(part.children[rootIdx], appendPath(path, rootIdx))

	default:
		// multipart/mixed and any other multipart: the first subtree
		// yielding a body wins.
		for i, child := range part.children {
			leaf, leafPath, err := s.selectBody(child, appendPath(path, i))
			if err != nil {
				return nil, nil, err
			}
			if leaf != nil {
				return leaf, leafPath, nil
			}
		}
		return nil, nil, nil
	}
}

// relatedRootIndex returns the index of the direct child of a
// multipart/related part designated as its root, or -1 if the part has no
// children. A "start" parameter matching zero or several children is an
// error.
func relatedRootIndex(part *presentationPart) (int, error) {
	start := normalizeContentID(part.mediaParams["start"])
	if start == "" {
		if len(part.children) == 0 {
			return -1, nil
		}
		return 0, nil
	}
	idx := -1
	for i, child := range part.children {
		id, ok := child.contentID()
		if !ok || id != start {
			continue
		}
		if idx >= 0 {
			return -1, fmt.Errorf("mail: multipart/related start %q matches multiple parts", start)
		}
		idx = i
	}
	if idx < 0 {
		return -1, fmt.Errorf("mail: multipart/related start %q matches no part", start)
	}
	return idx, nil
}

// collectRelatedResources gathers the inline resources contributed by the
// multipart/related layers on the path of the selected body.
func collectRelatedResources(root *presentationPart, bodyPath []int) ([]RelatedResource, error) {
	byID := make(map[string]RelatedResource)

	node := root
	var path []int
	for {
		if node.multipart && node.mediaType == "multipart/related" {
			rootIdx, err := relatedRootIndex(node)
			if err != nil {
				return nil, err
			}
			layerSeen := make(map[string]bool)
			for i, child := range node.children {
				if i == rootIdx || child.disp == "attachment" || child.multipart {
					continue
				}
				id, ok := child.contentID()
				if !ok {
					continue
				}
				if layerSeen[id] {
					return nil, fmt.Errorf("mail: duplicate Content-ID %q in multipart/related layer", id)
				}
				layerSeen[id] = true
				// Layers are visited from the outside in, so an inner
				// layer shadows an outer layer with the same ID.
				byID[id] = RelatedResource{
					ContentID: id,
					MediaType: child.mediaType,
					Body:      child.body,
					Path:      appendPath(path, i),
				}
			}
		}

		if len(bodyPath) == 0 {
			break
		}
		node = node.children[bodyPath[0]]
		path = appendPath(path, bodyPath[0])
		bodyPath = bodyPath[1:]
	}

	resources := make([]RelatedResource, 0, len(byID))
	for _, r := range byID {
		resources = append(resources, r)
	}
	sort.Slice(resources, func(i, j int) bool {
		return comparePath(resources[i].Path, resources[j].Path) < 0
	})
	return resources, nil
}

// appendPath returns a fresh slice with i appended to path.
func appendPath(path []int, i int) []int {
	p := make([]int, len(path)+1)
	copy(p, path)
	p[len(path)] = i
	return p
}

// comparePath compares two paths lexicographically.
func comparePath(a, b []int) int {
	for i := 0; i < len(a) && i < len(b); i++ {
		if a[i] != b[i] {
			if a[i] < b[i] {
				return -1
			}
			return 1
		}
	}
	switch {
	case len(a) < len(b):
		return -1
	case len(a) > len(b):
		return 1
	}
	return 0
}
