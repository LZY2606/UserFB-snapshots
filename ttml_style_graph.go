package astisub

import (
	"crypto/sha1"
	"encoding/hex"
	"fmt"
	"strings"
)

// TTMLStyleReferences holds the ordered, space-separated style references of a
// TTML style, region, body, div, p or span "style" attribute.
//
// A single-reference attribute keeps being represented through the existing
// Style pointer on Style, Region, Item and LineItem, exactly as before. The
// extension is only populated when the attribute contains several references
// (e.g. style="bold blue"): references are tokenized on XML whitespace, kept
// in their document order and resolved with later references overriding the
// same-named attributes of earlier ones. See README.md for the full
// representation rules.
type TTMLStyleReferences struct {
	ids  []string
	refs []*Style
}

// IDs returns a copy of the referenced style IDs in reference order.
func (r *TTMLStyleReferences) IDs() []string {
	if r == nil {
		return nil
	}
	out := make([]string, len(r.ids))
	copy(out, r.ids)
	return out
}

// Styles returns a copy of the referenced styles in reference order.
func (r *TTMLStyleReferences) Styles() []*Style {
	if r == nil {
		return nil
	}
	out := make([]*Style, len(r.refs))
	copy(out, r.refs)
	return out
}

// newTTMLStyleReferences builds a reference list from raw style IDs.
func newTTMLStyleReferences(ids []string) *TTMLStyleReferences {
	cp := make([]string, len(ids))
	copy(cp, ids)
	return &TTMLStyleReferences{ids: cp}
}

// ttmlStyleRefIDs splits a TTML "style" attribute on XML whitespace
// (#x20, #x9, #xD, #xA), ignoring empty tokens. Duplicate IDs are kept since
// later occurrences legitimately override earlier ones.
func ttmlStyleRefIDs(raw string) []string {
	var ids []string
	start := -1
	for i, r := range raw {
		switch r {
		case ' ', '\t', '\r', '\n':
			if start >= 0 {
				ids = append(ids, raw[start:i])
				start = -1
			}
		default:
			if start < 0 {
				start = i
			}
		}
	}
	if start >= 0 {
		ids = append(ids, raw[start:])
	}
	return ids
}

// link resolves the stored IDs against the provided style map. Every ID is
// guaranteed to exist; callers validate references beforehand.
func (r *TTMLStyleReferences) link(styles map[string]*Style) {
	r.refs = make([]*Style, len(r.ids))
	for i, id := range r.ids {
		r.refs[i] = styles[id]
	}
}

// ttmlStyleResolver computes the effective StyleAttributes of every head
// style. Referenced styles are merged in reference order and a style's own
// attributes override everything inherited from its references. Diamond
// sharing is supported: a shared ancestor is resolved once and reused.
type ttmlStyleResolver struct {
	styles    map[string]*Style
	rawRefs   map[string][]string
	effective map[string]*StyleAttributes
}

// resolve returns the effective attributes of style id, detecting missing
// references and cycles. chain holds the IDs of the styles currently being
// resolved from the graph entry point down to id.
func (r *ttmlStyleResolver) resolve(id string, chain []string) (*StyleAttributes, error) {
	if cached, ok := r.effective[id]; ok {
		return cached, nil
	}
	for _, ancestor := range chain {
		if ancestor == id {
			return nil, fmt.Errorf("astisub: ttml style cycle detected in style chain %s -> %s", strings.Join(chain, " -> "), id)
		}
	}
	s, ok := r.styles[id]
	if !ok {
		if len(chain) == 0 {
			return nil, fmt.Errorf("astisub: ttml style %q doesn't exist", id)
		}
		return nil, fmt.Errorf("astisub: ttml style %q referenced by style chain %s doesn't exist", id, strings.Join(chain, " -> "))
	}

	chain = append(chain, id)
	out := &StyleAttributes{}
	for _, refID := range r.rawRefs[id] {
		parent, err := r.resolve(refID, chain)
		if err != nil {
			return nil, err
		}
		out.mergeTTMLOverrides(parent)
	}
	// The style's own attributes win over every inherited one.
	out.mergeTTMLOverrides(s.InlineStyle)
	// Re-derive WebVTT/STL layout fields from the composed TTML attributes so
	// an overridden attribute cannot leave a stale derived value behind.
	out.propagateTTMLAttributes()
	r.effective[id] = out
	return out, nil
}

// compose builds the effective attributes carried by a node referencing
// refIDs in order: the effective attributes of the references are merged
// first, the node's own inline attributes are applied last.
func (r *ttmlStyleResolver) compose(refIDs []string, own *StyleAttributes) *StyleAttributes {
	out := &StyleAttributes{}
	for _, id := range refIDs {
		out.mergeTTMLOverrides(r.effective[id])
	}
	out.mergeTTMLOverrides(own)
	out.propagateTTMLAttributes()
	return out
}

// ttmlMergedStyle returns the shared synthetic style materializing the
// effective attributes of a reference list. The same ordered reference list
// always resolves to the same synthetic style: synthetic styles are never
// mutated per node and are not emitted back to TTML, so repeated write/read
// cycles cannot create new ones. refAttribute is the exact value the node's
// "style" attribute must carry when written back.
func ttmlMergedStyle(styles map[string]*Style, cache map[string]*Style, cacheKey string, refIDs []string, refAttribute string, effective *StyleAttributes) *Style {
	if s, ok := cache[cacheKey]; ok {
		return s
	}

	// Deterministic ID derived from the normalized reference list, suffixed
	// only if it collides with an ID already present in the document.
	sum := sha1.Sum([]byte(cacheKey))
	base := "astisub_merged_" + hex.EncodeToString(sum[:8])
	id := base
	for i := 1; ; i++ {
		if _, exists := styles[id]; !exists {
			break
		}
		id = fmt.Sprintf("%s_%d", base, i)
	}

	refs := newTTMLStyleReferences(refIDs)
	refs.link(styles)
	s := &Style{
		ID:               id,
		InlineStyle:      effective,
		StyleReferences:  refs,
		ttmlSynthetic:    true,
		ttmlRefAttribute: refAttribute,
	}
	styles[id] = s
	cache[cacheKey] = s
	return s
}

// ttmlStyleRefAttribute renders the value of an output TTML "style"
// attribute: the verbatim reference attribute when set on a synthetic style,
// the full ordered reference list for a declared multi-reference style, and
// the single referenced style's ID otherwise.
func ttmlStyleRefAttribute(primary *Style, refs *TTMLStyleReferences) string {
	if primary != nil && primary.ttmlRefAttribute != "" {
		return primary.ttmlRefAttribute
	}
	if refs != nil && len(refs.ids) > 0 {
		return strings.Join(refs.ids, " ")
	}
	if primary != nil {
		return primary.ID
	}
	return ""
}
