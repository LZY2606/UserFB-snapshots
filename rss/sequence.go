package rss

import (
	"errors"
	"fmt"
	"strings"

	"github.com/mmcdole/gofeed/internal/shared"
	xpp "github.com/mmcdole/goxpp/v2"
)

// ErrInvalidSequence is returned by Parser.Parse when UseRSS1Sequence is
// enabled and an RSS 1.0 channel declares an item sequence that cannot be
// resolved: a Seq is declared more than once, an li is missing a non-empty
// RDF resource, a resource does not identify a root-level item, or one
// resource value matches more than one item's RDF about. Errors wrapping it
// carry the offending resource value or describe the declaration conflict.
var ErrInvalidSequence = errors.New("rss: invalid RSS 1.0 item sequence")

// rss1Sequence is the parsed view of an RSS 1.0 channel's
// <items><rdf:Seq><rdf:li rdf:resource="..."/></rdf:Seq></items> declaration.
// resources holds the raw rdf:resource values in declaration order, including
// repeated references; an empty slice is a valid, empty sequence.
type rss1Sequence struct {
	resources []string
}

// parseRSS1Sequence parses an RSS 1.0 <items> element, starting with the
// parser on its opening tag and leaving it on the closing tag. Only an
// RDF-namespace Seq counts as a declaration: elements in any other namespace,
// including ones that reuse the local name "Seq", are foreign content and are
// skipped. It returns nil when the wrapper contains no RDF Seq.
func parseRSS1Sequence(p *xpp.Parser) (*rss1Sequence, error) {
	if err := p.Expect(xpp.StartTag, "items"); err != nil {
		return nil, err
	}

	seq := &rss1Sequence{resources: []string{}}
	foundSeq := false

	err := shared.ForEachChild(p, func(name string) error {
		if name != "seq" || strings.TrimSpace(p.Space()) != shared.RDFNamespace {
			return p.Skip()
		}
		if foundSeq {
			return fmt.Errorf("%w: channel declares multiple item sequences", ErrInvalidSequence)
		}
		foundSeq = true

		resources, err := parseRSS1Seq(p)
		if err != nil {
			return err
		}
		seq.resources = resources
		return nil
	})
	if err != nil {
		return nil, err
	}

	if err := p.Expect(xpp.EndTag, "items"); err != nil {
		return nil, err
	}

	if !foundSeq {
		return nil, nil
	}
	return seq, nil
}

// parseRSS1Seq parses the direct li children of an RDF Seq, starting on the
// Seq opening tag and ending on its closing tag. Only RDF-namespace li
// elements are members; every member must carry a non-empty RDF resource.
func parseRSS1Seq(p *xpp.Parser) ([]string, error) {
	if err := p.Expect(xpp.StartTag, "seq"); err != nil {
		return nil, err
	}

	resources := []string{}
	err := shared.ForEachChild(p, func(name string) error {
		if name != "li" || strings.TrimSpace(p.Space()) != shared.RDFNamespace {
			// Foreign-namespace children are not sequence members.
			return p.Skip()
		}
		resource := rdfAttribute(p, "resource")
		if resource == "" {
			return fmt.Errorf("%w: sequence li is missing a non-empty rdf:resource attribute", ErrInvalidSequence)
		}
		resources = append(resources, resource)
		return p.Skip()
	})
	if err != nil {
		return nil, err
	}

	if err := p.Expect(xpp.EndTag, "seq"); err != nil {
		return nil, err
	}
	return resources, nil
}

// rdfAttribute returns the value of the RDF-namespace attribute named local.
// It compares against the full RDF namespace URI, so the result does not
// depend on the prefix the document happens to bind; an attribute in any
// other namespace is ignored.
func rdfAttribute(p *xpp.Parser, local string) string {
	for _, attr := range p.Attrs() {
		if attr.Name.Space == shared.RDFNamespace && attr.Name.Local == local {
			return attr.Value
		}
	}
	return ""
}

// applyRSS1Sequence replaces the channel's items with the root-level items
// named by the sequence, in sequence order. aboutIndex maps each raw
// rdf:about value to the root-level items carrying it. Resources match the
// raw values exactly: titles, links and GUIDs are never alternative
// identities and no normalization is applied, and repeated references keep
// their repeated positions. It returns ErrInvalidSequence when a resource
// names no item or names several items. Unreferenced items, including ones
// without an identity or with a duplicated about, never block a sequence
// that does not name them.
func applyRSS1Sequence(channel *Feed, aboutIndex map[string][]*Item, seq *rss1Sequence) error {
	selected := make([]*Item, 0, len(seq.resources))
	for _, resource := range seq.resources {
		matches := aboutIndex[resource]
		switch len(matches) {
		case 0:
			return fmt.Errorf("%w: sequence references unknown item resource %q", ErrInvalidSequence, resource)
		case 1:
			selected = append(selected, matches[0])
		default:
			return fmt.Errorf("%w: sequence resource %q matches %d items with the same rdf:about", ErrInvalidSequence, resource, len(matches))
		}
	}

	channel.Items = selected
	return nil
}
