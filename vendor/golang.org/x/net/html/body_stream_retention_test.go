package html

import (
	"fmt"
	"strings"
	"testing"

	"github.com/PuerkitoBio/goquery/task_support/taskcheck"
)

// nodeSet collects every node reachable from a tree so pointer identity can be
// compared without relying on garbage collection timing.
func nodeSet(root *Node) map[*Node]bool {
	seen := make(map[*Node]bool)
	var walk func(*Node)
	walk = func(n *Node) {
		seen[n] = true
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			walk(c)
		}
	}
	walk(root)
	return seen
}

func activeBody(p *parser) *Node {
	for h := p.doc.FirstChild; h != nil; h = h.NextSibling {
		if h.Type != ElementNode || h.DataAtom.String() != "html" {
			continue
		}
		for c := h.FirstChild; c != nil; c = c.NextSibling {
			if c.Type == ElementNode && c.DataAtom.String() == "body" {
				return c
			}
		}
	}
	return nil
}

func countNodes(n *Node) int {
	count := 0
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		count += 1 + countNodes(c)
	}
	return count
}

// TestBodyStreamRetention checks, using the real parser captured through a
// ParseOption, that once a batch has been handed off none of its nodes are
// still referenced by the active parse tree, the deferred text buffers or the
// parser's saved node state (open elements, active formatting elements, form
// pointer and template insertion modes).
func TestBodyStreamRetention(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var captured *parser
		capture := func(p *parser) { captured = p }

		const records = 1500
		var src strings.Builder
		src.WriteString("<body>")
		for i := 0; i < records; i++ {
			if i%37 == 0 {
				// Entities force several text tokens to be coalesced into one
				// node through the parser's deferred text map.
				fmt.Fprintf(&src, "<p id=\"r%d\">v%d&amp;x<!--c%d--></p>", i, i, i)
			} else {
				fmt.Fprintf(&src, "<p id=\"r%d\">v%d</p>", i, i)
			}
		}

		var maxPending int
		var maxTokenizerCap int
		batches := 0
		stats, err := ParseBodyStream(strings.NewReader(src.String()), func(root *Node) error {
			c.Check(captured != nil, "parser was not captured through the ParseOption")

			// The parser is at the documented safe frontier.
			c.Check(len(captured.oe) == 2, "batch %d: open element stack has %d entries, want 2", batches, len(captured.oe))
			if len(captured.oe) == 2 {
				c.Check(captured.oe[0].Namespace == "" && captured.oe[0].DataAtom.String() == "html", "batch %d: stack bottom is not an HTML html element", batches)
				c.Check(captured.oe[1].Namespace == "" && captured.oe[1].DataAtom.String() == "body", "batch %d: stack top is not an HTML body element", batches)
			}
			c.Check(len(captured.afe) == 0, "batch %d: active formatting elements list is not empty", batches)
			c.Check(captured.form == nil, "batch %d: form element pointer is set", batches)
			c.Check(len(captured.templateStack) == 0, "batch %d: template insertion mode stack is not empty", batches)
			c.Check(!captured.framesetOK, "batch %d: frameset-ok is still true", batches)
			c.Check(!captured.fosterParenting, "batch %d: foster parenting is still active", batches)
			c.Check(!captured.hasSelfClosingToken, "batch %d: a self-closing token is still pending", batches)

			// No node of the delivered batch is reachable from the active
			// document tree (which includes the active body).
			delivered := nodeSet(root)
			active := nodeSet(captured.doc)
			leaked := 0
			for n := range delivered {
				if n.Type == DocumentNode || (n.Type == ElementNode && (n.DataAtom.String() == "html" || n.DataAtom.String() == "head" || n.DataAtom.String() == "body") && n.Parent == root) {
					// The batch skeleton is new and cannot be confused with the
					// active tree; skip it.
					continue
				}
				if active[n] {
					leaked++
				}
			}
			c.Check(leaked == 0, "batch %d: %d delivered nodes are still reachable from the active parse tree", batches, leaked)

			// The deferred text buffers no longer name any delivered node.
			deferredLeaked := 0
			for n := range captured.textNodes {
				if delivered[n] {
					deferredLeaked++
				}
			}
			c.Check(deferredLeaked == 0, "batch %d: %d delivered nodes are still held by the deferred text map", batches, deferredLeaked)

			// The parser's saved node state cannot reference delivered nodes.
			stateLeaked := 0
			for _, n := range captured.oe {
				if delivered[n] {
					stateLeaked++
				}
			}
			for _, n := range captured.afe {
				if n.Type == ElementNode && delivered[n] {
					stateLeaked++
				}
			}
			if captured.form != nil && delivered[captured.form] {
				stateLeaked++
			}
			if captured.head != nil && delivered[captured.head] {
				stateLeaked++
			}
			c.Check(stateLeaked == 0, "batch %d: %d delivered nodes are still held by saved parser state", batches, stateLeaked)

			// Text was fully materialized before handoff, including text that
			// went through the deferred text map.
			textOK := true
			var walk func(*Node)
			walk = func(n *Node) {
				if n.Type == TextNode {
					if n.Data == "" || captured.textNodes[n] != nil {
						textOK = false
					}
				}
				for ch := n.FirstChild; ch != nil; ch = ch.NextSibling {
					walk(ch)
				}
			}
			walk(root)
			c.Check(textOK, "batch %d: delivered text was not fully materialized", batches)

			// The active body only holds the not-yet-delivered tail and does
			// not grow with the number of already delivered records.
			if body := activeBody(captured); body != nil {
				if n := countNodes(body); n > maxPending {
					maxPending = n
				}
			}
			if cap(captured.tokenizer.buf) > maxTokenizerCap {
				maxTokenizerCap = cap(captured.tokenizer.buf)
			}

			batches++
			return nil
		}, capture)

		c.Check(err == nil, "unexpected error: %v", err)
		c.Check(batches == records, "committed %d batches, want %d", batches, records)
		c.Check(stats.Batches == records, "stats.Batches = %d, want %d", stats.Batches, records)
		c.Check(stats.PeakPendingNodes == 3, "stats.PeakPendingNodes = %d, want a small constant 3", stats.PeakPendingNodes)
		// Of p + text + comment only the paragraph may be pending.
		c.Check(maxPending <= 3, "active body held up to %d nodes at batch time", maxPending)
		// The tokenizer's rolling buffer stays bounded: the whole source
		// (which is far larger) is never retained.
		sourceLen := src.Len()
		c.Check(sourceLen > 30000, "test source is not large enough: %d", sourceLen)
		c.Check(maxTokenizerCap <= 8192, "tokenizer buffered %d bytes, whole source is %d", maxTokenizerCap, sourceLen)

		// After EOF the parser holds no committed node either. The active tree
		// is down to the document element, an empty head and an empty body; no
		// delivered paragraph can be reached from it.
		total := countNodes(captured.doc)
		bodyCount := 0
		if body := activeBody(captured); body != nil {
			bodyCount = countNodes(body)
		}
		// document + html + head are the only non-body nodes.
		c.Check(total-bodyCount == 3, "parser tree after EOF has %d nodes outside the body, want document/html/head", total-bodyCount)
		c.Check(bodyCount == 0, "active body after EOF still holds %d nodes", bodyCount)
		c.Check(len(captured.oe) == 2, "open element stack after EOF has %d entries, want 2", len(captured.oe))
		c.Check(captured.textNodes == nil || len(captured.textNodes) == 0, "deferred text remains after EOF: %d entries", len(captured.textNodes))
	})
}
