// Copyright 2010 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package html

import (
	"errors"
	"fmt"
	"io"

	a "golang.org/x/net/html/atom"
)

// BodyStreamStats describes the work performed by ParseBodyStream.
type BodyStreamStats struct {
	// Batches is the number of times the yield callback was invoked,
	// including a batch whose callback returned an error.
	Batches int
	// Nodes is the total number of body nodes handed to the callback, counted
	// recursively and including text and comment nodes. The per-batch
	// document/html/head/body skeleton is not counted.
	Nodes int
	// PeakPendingNodes is the maximum number of descendants of the active
	// body element observed immediately after a real token was fully
	// processed, before that token's safe prefix was released. The final EOF
	// processing is observed as well.
	PeakPendingNodes int
}

var (
	errNilBodyStreamReader = errors.New("html: ParseBodyStream called with a nil Reader")
	errNilBodyStreamYield  = errors.New("html: ParseBodyStream called with a nil yield function")
)

// ParseBodyStream parses the HTML from r like Parse, but instead of returning
// the whole document it repeatedly hands off the prefix of the body element's
// children that the HTML5 tree construction algorithm has finished with.
//
// Each call to yield receives a fresh document root with a minimal
// document/html/head/body skeleton (an empty head and no attributes copied
// from the source page). The body of that skeleton owns the nodes that were
// already committed. Nodes are moved, not cloned, so they keep their identity
// and their exact HTML5 tree shape. A batch is only committed when the open
// element stack is back to exactly html and body, the list of active
// formatting elements and the template insertion mode stack are empty, the
// form element pointer is nil, frameset-ok is false, and no foster parenting
// or token reprocessing is in progress. The last direct text node of the body
// is retained between batches because more text may still be coalesced into
// it.
//
// At EOF the regular HTML5 EOF error handling is performed first and then all
// remaining body children, including unclosed elements and trailing text, are
// delivered as a final batch. A frameset document without a body produces no
// batches.
//
// yield is called synchronously and the input is not read again until the
// callback returns. If yield returns an error, ParseBodyStream stops
// immediately, does not read any more input and returns that error unchanged.
// A non-EOF error from r is returned unchanged as well; it does not cause the
// remaining, not-yet-safe nodes to be delivered. ParseBodyStream never closes
// r.
func ParseBodyStream(r io.Reader, yield func(*Node) error, opts ...ParseOption) (BodyStreamStats, error) {
	if r == nil {
		return BodyStreamStats{}, errNilBodyStreamReader
	}
	if yield == nil {
		return BodyStreamStats{}, errNilBodyStreamYield
	}
	p := &parser{
		tokenizer:  NewTokenizer(r),
		doc:        &Node{Type: DocumentNode},
		scripting:  true,
		framesetOK: true,
		im:         initialIM,
	}
	for _, f := range opts {
		f(p)
	}
	return p.parseBodyStream(yield)
}

// parseBodyStream is the streaming counterpart of parse. It mirrors the
// tokenizer loop of parse so that the tokenizer's (n > 0, err) contract is
// preserved, but observes the parser state after each real token and commits
// safe prefixes instead of keeping the whole document.
func (p *parser) parseBodyStream(yield func(*Node) error) (stats BodyStreamStats, err error) {
	defer func() {
		if panicErr := recover(); panicErr != nil {
			err = fmt.Errorf("%s", panicErr)
		}
	}()
	for {
		// CDATA sections are allowed only in foreign content.
		n := p.oe.top()
		p.tokenizer.AllowCDATA(n != nil && n.Namespace != "")
		// Read and parse the next token. The tokenizer first consumes any
		// complete tokens from bytes returned together with a reader error;
		// the error is only observed once those bytes are exhausted.
		p.tokenizer.Next()
		p.tok = p.tokenizer.Token()
		if p.tok.Type == ErrorToken {
			readErr := p.tokenizer.Err()
			if readErr != nil && readErr != io.EOF {
				// A real reader failure: keep what was already committed, but
				// do not force out the unstable tail.
				return stats, readErr
			}
			// EOF: run the regular HTML5 EOF error handling, then deliver
			// everything that is left without requiring the safe frontier.
			p.parseCurrentToken()
			p.observePending(&stats)
			p.flushTextNodes()
			if err := p.emitBodyBatch(&stats, yield, true); err != nil {
				return stats, err
			}
			return stats, nil
		}
		p.parseCurrentToken()
		p.observePending(&stats)
		if p.bodyFrontierReady() {
			if err := p.emitBodyBatch(&stats, yield, false); err != nil {
				return stats, err
			}
		}
	}
}

// observePending records the number of active body descendants before any
// release happens for the token that was just processed.
func (p *parser) observePending(stats *BodyStreamStats) {
	body := p.streamBody()
	if body == nil {
		return
	}
	if n := countDescendantNodes(body); n > stats.PeakPendingNodes {
		stats.PeakPendingNodes = n
	}
}

// streamBody returns the active body element, if the document has one. The
// body may have been removed by a frameset document.
func (p *parser) streamBody() *Node {
	if len(p.oe) >= 2 {
		if n := p.oe[1]; n.Type == ElementNode && n.DataAtom == a.Body && n.Namespace == "" {
			return n
		}
	}
	for h := p.doc.FirstChild; h != nil; h = h.NextSibling {
		if h.Type != ElementNode || h.DataAtom != a.Html {
			continue
		}
		for c := h.FirstChild; c != nil; c = c.NextSibling {
			if c.Type == ElementNode && c.DataAtom == a.Body {
				return c
			}
		}
	}
	return nil
}

func countDescendantNodes(n *Node) int {
	count := 0
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		count += 1 + countDescendantNodes(c)
	}
	return count
}

// flushTextNodes materializes text that the parser coalesced into
// p.textNodes, so a committed subtree never depends on deferred text state.
// Subsequent text tokens continue from the materialized Data.
func (p *parser) flushTextNodes() {
	for n, b := range p.textNodes {
		n.Data = string(b)
	}
	clear(p.textNodes)
}

// bodyFrontierReady reports whether the parser is at the conservative safe
// boundary described by the streaming contract: the open element stack is
// exactly the HTML html and body elements, no formatting elements, form
// pointer or template insertion mode are pending, frameset-ok is false and no
// foster insertion or token reprocessing is in flight.
func (p *parser) bodyFrontierReady() bool {
	if len(p.oe) != 2 {
		return false
	}
	if p.oe[0].Namespace != "" || p.oe[0].DataAtom != a.Html {
		return false
	}
	if p.oe[1].Namespace != "" || p.oe[1].DataAtom != a.Body {
		return false
	}
	if len(p.afe) != 0 || p.form != nil || len(p.templateStack) != 0 {
		return false
	}
	if p.framesetOK || p.fosterParenting || p.hasSelfClosingToken {
		return false
	}
	return true
}

// emitBodyBatch moves the committable prefix of the body's direct children
// into a fresh batch document and calls yield. When force is true (EOF) every
// remaining direct child is committed, including the trailing text node.
func (p *parser) emitBodyBatch(stats *BodyStreamStats, yield func(*Node) error, force bool) error {
	body := p.streamBody()
	if body == nil {
		return nil
	}
	var move []*Node
	for c := body.FirstChild; c != nil; c = c.NextSibling {
		if !force && c == body.LastChild && c.Type == TextNode {
			// More text may still be coalesced into this node.
			break
		}
		move = append(move, c)
	}
	if len(move) == 0 {
		return nil
	}
	// Text is complete before handoff, and no deferred text state keeps the
	// moved nodes alive afterwards.
	p.flushTextNodes()
	root, batchBody := newBodyStreamDocument()
	for _, c := range move {
		body.RemoveChild(c)
		batchBody.AppendChild(c)
	}
	stats.Batches++
	stats.Nodes += countDescendantNodes(batchBody)
	return yield(root)
}

// newBodyStreamDocument builds the minimal per-batch skeleton: a document
// node and html, empty head and body elements, all in the HTML namespace and
// without attributes copied from the source page.
func newBodyStreamDocument() (doc, body *Node) {
	doc = &Node{Type: DocumentNode}
	htmlNode := &Node{Type: ElementNode, DataAtom: a.Html, Data: a.Html.String()}
	head := &Node{Type: ElementNode, DataAtom: a.Head, Data: a.Head.String()}
	body = &Node{Type: ElementNode, DataAtom: a.Body, Data: a.Body.String()}
	doc.AppendChild(htmlNode)
	htmlNode.AppendChild(head)
	htmlNode.AppendChild(body)
	return doc, body
}
