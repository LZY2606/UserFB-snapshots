package readability

import (
	"bytes"
	"encoding/json"
	"net/url"
	"reflect"
	"strings"
	"testing"

	"github.com/go-shiori/go-readability/task_support/taskcheck"
	"golang.org/x/net/html"
)

// traceTestURL is the base URL used by the selection trace tests.
func traceTestURL() *url.URL {
	u, _ := url.Parse("https://example.invalid/article")
	return u
}

// findSelectionNode returns the SelectionNode with the given ID, or nil.
func findSelectionNode(trace *SelectionTrace, id int) *SelectionNode {
	for i := range trace.Nodes {
		if trace.Nodes[i].ID == id {
			return &trace.Nodes[i]
		}
	}
	return nil
}

// checkTraceConsistency verifies the invariants that must hold for every
// trace: each ID is defined exactly once, and every ID referenced by
// events, rankings and winners is defined in Nodes.
func checkTraceConsistency(c *taskcheck.C, trace *SelectionTrace) {
	defined := make(map[int]bool)
	for _, n := range trace.Nodes {
		c.Check(!defined[n.ID], "node id %d defined more than once", n.ID)
		defined[n.ID] = true
		c.Check(n.ID > 0, "node id %d is not positive", n.ID)
		if n.Synthetic {
			c.Check(n.Path == nil, "synthetic node %d has a path", n.ID)
		}
	}

	checkRef := func(id int, what string) {
		if id == 0 {
			c.Check(false, "%s references id 0", what)
			return
		}
		c.Check(defined[id], "%s references undefined id %d", what, id)
	}

	for _, at := range trace.Attempts {
		for _, ev := range at.Events {
			checkRef(ev.NodeID, "event node")
			if ev.Kind == SelectionEventContribution {
				checkRef(ev.SourceID, "contribution source")
			} else {
				c.Check(ev.SourceID == 0, "%s event has SourceID %d", ev.Kind, ev.SourceID)
				c.Check(ev.Level == -1, "%s event has Level %d", ev.Kind, ev.Level)
			}
		}
		for _, id := range at.RankedIDs {
			checkRef(id, "ranked candidate")
		}
		if at.WinnerNodeID != 0 {
			checkRef(at.WinnerNodeID, "winner")
		}
	}
}

// resolvePath walks the original tree following a SelectionNode path.
func resolvePath(root *html.Node, path []int) *html.Node {
	cur := root
	for _, idx := range path {
		var next *html.Node
		i := 0
		for child := cur.FirstChild; child != nil; child = child.NextSibling {
			if i == idx {
				next = child
				break
			}
			i++
		}
		if next == nil {
			return nil
		}
		cur = next
	}
	return cur
}

// hasEvent reports whether events contains an event exactly equal to want.
func hasEvent(events []SelectionEvent, want SelectionEvent) bool {
	for _, ev := range events {
		if ev == want {
			return true
		}
	}
	return false
}

// contributionSources collects the SourceID of every contribution event.
func contributionSources(at SelectionAttempt) map[int]bool {
	out := make(map[int]bool)
	for _, ev := range at.Events {
		if ev.Kind == SelectionEventContribution {
			out[ev.SourceID] = true
		}
	}
	return out
}

// initialNodes collects the NodeID of every initial event.
func initialNodes(at SelectionAttempt) map[int]bool {
	out := make(map[int]bool)
	for _, ev := range at.Events {
		if ev.Kind == SelectionEventInitial {
			out[ev.NodeID] = true
		}
	}
	return out
}

func TestSelectionTraceScores(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A small page where every score can be computed by hand:
		// - div#x1.article gets +25 from its class and +5 from its tag.
		// - P1 has 31 chars and 2 commas, so its score is 1+2 = 3.
		// - P2 has 110 chars and no comma, so its score is 1+1 = 2.
		// - the link inside P1 has 13 chars of text.
		p2 := strings.Repeat("q", 110)
		raw := `<html><head><title>s</title></head><body>` +
			`<div id="x1" class="article"><p>alpha, beta, <a href="/more">linktext here</a> tail</p><p>` + p2 + `</p></div>` +
			`<p>short</p></body></html>`

		ps := NewParser()
		ps.ExplainSelection = true
		ps.CharThresholds = 100
		article, err := ps.Parse(strings.NewReader(raw), traceTestURL())
		c.Check(err == nil, "parse failed: %v", err)
		trace := article.SelectionTrace
		c.Check(trace != nil, "trace is nil")
		if trace == nil {
			return
		}
		checkTraceConsistency(c, trace)

		// The parse succeeds on the first attempt (141 >= 100 chars).
		c.Check(len(trace.Attempts) == 1, "attempts = %d", len(trace.Attempts))
		c.Check(trace.ChosenAttempt == 0, "chosen = %d", trace.ChosenAttempt)
		c.Check(article.Length == 141, "article length = %d", article.Length)

		// Node numbering is preorder from 1 over the original elements:
		// html, head, title, body, div, p, a, p, p.
		c.Check(len(trace.Nodes) == 9, "nodes = %d", len(trace.Nodes))
		for i, n := range trace.Nodes {
			c.Check(n.ID == i+1, "node %d has id %d", i, n.ID)
			c.Check(!n.Synthetic, "node %d is synthetic", n.ID)
			c.Check(n.Path != nil, "original node %d has nil path", n.ID)
		}
		wantPaths := map[int][]int{
			1: {0},             // html
			2: {0, 0},          // head
			3: {0, 0, 0},       // title
			4: {0, 1},          // body
			5: {0, 1, 0},       // div#x1
			6: {0, 1, 0, 0},    // first p
			7: {0, 1, 0, 0, 1}, // a (after the first text node)
			8: {0, 1, 0, 1},    // second p
			9: {0, 1, 1},       // short p
		}
		for id, want := range wantPaths {
			n := findSelectionNode(trace, id)
			c.Check(n != nil && reflect.DeepEqual(n.Path, want),
				"node %d path = %v, want %v", id, n.Path, want)
		}

		// The attempt records the flags as they were at its start.
		at := trace.Attempts[0]
		c.Check(at.Index == 0, "attempt index = %d", at.Index)
		c.Check(at.StripUnlikelys && at.UseWeightClasses && at.CleanConditionally,
			"attempt flags = %v/%v/%v", at.StripUnlikelys, at.UseWeightClasses, at.CleanConditionally)

		// Every score write, in execution order, with hand-computed values.
		// div starts at 25 (class) + 5 (tag) = 30, body at 0.
		// P1 (id 6) adds 3/1 to the div and 3/2 to the body.
		// P2 (id 8) adds 2/1 to the div and 2/2 to the body.
		// Link density is 13/141 for the div and 13/146 for the body,
		// and After is the value stored with four decimal places.
		wantEvents := []SelectionEvent{
			{Kind: SelectionEventInitial, NodeID: 5, SourceID: 0, Level: -1, Before: 0, Operand: 30, After: 30},
			{Kind: SelectionEventContribution, NodeID: 5, SourceID: 6, Level: 0, Before: 30, Operand: 3, After: 33},
			{Kind: SelectionEventInitial, NodeID: 4, SourceID: 0, Level: -1, Before: 0, Operand: 0, After: 0},
			{Kind: SelectionEventContribution, NodeID: 4, SourceID: 6, Level: 1, Before: 0, Operand: 1.5, After: 1.5},
			{Kind: SelectionEventContribution, NodeID: 5, SourceID: 8, Level: 0, Before: 33, Operand: 2, After: 35},
			{Kind: SelectionEventContribution, NodeID: 4, SourceID: 8, Level: 1, Before: 1.5, Operand: 1, After: 2.5},
			{Kind: SelectionEventDensity, NodeID: 5, SourceID: 0, Level: -1, Before: 35, Operand: 13.0 / 141.0, After: 31.773},
			{Kind: SelectionEventDensity, NodeID: 4, SourceID: 0, Level: -1, Before: 2.5, Operand: 13.0 / 146.0, After: 2.2774},
		}
		c.Check(reflect.DeepEqual(at.Events, wantEvents),
			"events differ:\n got: %+v\nwant: %+v", at.Events, wantEvents)

		// The short paragraph (id 9, five chars) must not produce any
		// contribution and must not be initialized.
		for _, ev := range at.Events {
			c.Check(ev.NodeID != 9, "short paragraph got event %+v", ev)
			c.Check(ev.SourceID != 9, "short paragraph produced contribution %+v", ev)
		}

		// The div wins after density scaling and is used as the content.
		c.Check(reflect.DeepEqual(at.RankedIDs, []int{5, 4}), "ranked = %v", at.RankedIDs)
		c.Check(at.WinnerNodeID == 5, "winner = %d", at.WinnerNodeID)
		c.Check(at.CleanLength == 141, "clean length = %d", at.CleanLength)

		// A paragraph whose only ancestors are the html and document
		// nodes has no qualified ancestor, so it must not fabricate
		// contributions. With an empty body every attempt also fails to
		// form a candidate, so the winner is 0 and the chosen attempt -1.
		doc := &html.Node{Type: html.DocumentNode}
		htmlEl := &html.Node{Type: html.ElementNode, Data: "html"}
		body := &html.Node{Type: html.ElementNode, Data: "body"}
		p := &html.Node{Type: html.ElementNode, Data: "p"}
		doc.AppendChild(htmlEl)
		htmlEl.AppendChild(body)
		htmlEl.AppendChild(p)
		p.AppendChild(&html.Node{Type: html.TextNode, Data: strings.Repeat("z", 30)})

		ps2 := NewParser()
		ps2.ExplainSelection = true
		article2, err := ps2.ParseDocument(doc, traceTestURL())
		c.Check(err == nil, "parse failed: %v", err)
		trace2 := article2.SelectionTrace
		c.Check(trace2 != nil, "trace2 is nil")
		if trace2 == nil {
			return
		}
		checkTraceConsistency(c, trace2)

		c.Check(article2.Content == "" && article2.Length == 0 && article2.Node == nil,
			"empty parse produced content")
		c.Check(trace2.ChosenAttempt == -1, "chosen2 = %d", trace2.ChosenAttempt)
		c.Check(len(trace2.Attempts) == 4, "attempts2 = %d", len(trace2.Attempts))

		// Original elements: html=1, body=2, p=3 (path [0,1]).
		pNode := findSelectionNode(trace2, 3)
		c.Check(pNode != nil && !pNode.Synthetic && reflect.DeepEqual(pNode.Path, []int{0, 1}),
			"paragraph node = %+v", pNode)

		for i, at2 := range trace2.Attempts {
			c.Check(at2.Index == i, "attempt2 index = %d", at2.Index)
			c.Check(at2.WinnerNodeID == 0, "attempt %d winner = %d", i, at2.WinnerNodeID)
			c.Check(len(at2.RankedIDs) == 0, "attempt %d ranked = %v", i, at2.RankedIDs)
			c.Check(at2.CleanLength == 0, "attempt %d clean = %d", i, at2.CleanLength)
			for _, ev := range at2.Events {
				c.Check(ev.Kind != SelectionEventContribution,
					"attempt %d has fabricated contribution %+v", i, ev)
				c.Check(ev.SourceID != 3, "attempt %d uses orphan paragraph as source %+v", i, ev)
			}
			// The only event is the initialization of the synthetic
			// fallback container, a fresh node per attempt that does
			// not impersonate any original element.
			c.Check(len(at2.Events) == 1, "attempt %d events = %d", i, len(at2.Events))
			if len(at2.Events) == 1 {
				ev := at2.Events[0]
				want := SelectionEvent{Kind: SelectionEventInitial, NodeID: 4 + i,
					SourceID: 0, Level: -1, Before: 0, Operand: 5, After: 5}
				c.Check(ev == want, "attempt %d event = %+v, want %+v", i, ev, want)
				synth := findSelectionNode(trace2, ev.NodeID)
				c.Check(synth != nil && synth.Synthetic && synth.Path == nil,
					"fallback node %d = %+v", ev.NodeID, synth)
			}
		}
	})
}

func TestSelectionTraceIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Two paragraphs with identical text in different containers,
		// a div that is retagged to p, and a div whose phrasing content
		// is wrapped into brand new paragraphs.
		same := strings.Repeat("s", 30)
		raw := `<html><head><title>i</title></head><body><div id="wrap"><div id="only">` +
			`<div class="dupa"><p>` + same + `</p><p>` + strings.Repeat("f", 30) + `</p></div>` +
			`<div class="dupb"><p>` + same + `</p><p>` + strings.Repeat("g", 30) + `</p></div>` +
			`<div id="retag"><section>` + strings.Repeat("r", 30) + `</section></div>` +
			`<div id="synth">` + strings.Repeat("t", 30) + `<ul><li>x</li></ul>` + strings.Repeat("u", 30) + `</div>` +
			`</div></div></body></html>`

		doc, err := html.Parse(strings.NewReader(raw))
		c.Check(err == nil, "html parse failed: %v", err)
		var before bytes.Buffer
		html.Render(&before, doc)

		ps := NewParser()
		ps.ExplainSelection = true
		ps.CharThresholds = 100
		article, err := ps.ParseDocument(doc, traceTestURL())
		c.Check(err == nil, "parse failed: %v", err)
		trace := article.SelectionTrace
		c.Check(trace != nil, "trace is nil")
		if trace == nil {
			return
		}
		checkTraceConsistency(c, trace)

		// The input tree must not be modified and no tracing attribute
		// may appear in it or in the output HTML.
		var after bytes.Buffer
		html.Render(&after, doc)
		c.Check(before.String() == after.String(), "input tree changed")
		c.Check(!strings.Contains(article.Content, "data-readability") &&
			!strings.Contains(article.Content, "data-selection") &&
			!strings.Contains(article.Content, "data-trace"),
			"output contains tracing attribute: %q", article.Content)

		c.Check(len(trace.Attempts) == 1, "attempts = %d", len(trace.Attempts))
		at := trace.Attempts[0]

		// Elements with identical text are still different nodes:
		// p#8 and p#11 hold the same 30 characters but keep their own
		// IDs and paths, and their contributions go to their own parents.
		nSame1 := findSelectionNode(trace, 8)
		nSame2 := findSelectionNode(trace, 11)
		c.Check(nSame1 != nil && nSame2 != nil, "same-text nodes missing")
		if nSame1 != nil && nSame2 != nil {
			c.Check(!nSame1.Synthetic && !nSame2.Synthetic, "same-text nodes synthetic")
			c.Check(!reflect.DeepEqual(nSame1.Path, nSame2.Path), "same-text nodes share path")
			el1 := resolvePath(doc, nSame1.Path)
			el2 := resolvePath(doc, nSame2.Path)
			c.Check(el1 != nil && el2 != nil && el1 != el2, "paths resolve to same element")
			if el1 != nil && el2 != nil {
				text1, text2 := domText(el1), domText(el2)
				c.Check(text1 == text2 && text1 == same,
					"resolved texts %q != %q", text1, text2)
			}
		}
		c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventContribution,
			NodeID: 7, SourceID: 8, Level: 0, Before: 5, Operand: 1, After: 6}),
			"contribution of first same-text paragraph missing")
		c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventContribution,
			NodeID: 10, SourceID: 11, Level: 0, Before: 5, Operand: 1, After: 6}),
			"contribution of second same-text paragraph missing")

		// The div that was retagged to p keeps its original ID (13); its
		// path still resolves to a <div> in the original tree, while the
		// initialization already saw the new tag (operand 0, no +5).
		nRetag := findSelectionNode(trace, 13)
		c.Check(nRetag != nil && !nRetag.Synthetic, "retag node missing or synthetic")
		if nRetag != nil {
			el := resolvePath(doc, nRetag.Path)
			c.Check(el != nil && el.Type == html.ElementNode && el.Data == "div",
				"retag path resolves to %v", el)
		}
		c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventInitial,
			NodeID: 13, SourceID: 0, Level: -1, Before: 0, Operand: 0, After: 0}),
			"initialization of retagged div missing")
		c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventContribution,
			NodeID: 6, SourceID: 13, Level: 0, Before: 7, Operand: 1, After: 8}),
			"retagged div did not contribute as scored element")
		c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventContribution,
			NodeID: 13, SourceID: 14, Level: 0, Before: 0, Operand: 1, After: 1}),
			"section did not contribute to retagged div")

		// The paragraphs created while wrapping phrasing content are
		// synthetic: fresh IDs, nil path, and they do not impersonate
		// their parent div (id 15).
		var synthetic []SelectionNode
		for _, n := range trace.Nodes {
			if n.Synthetic {
				synthetic = append(synthetic, n)
			}
		}
		c.Check(len(synthetic) == 2, "synthetic nodes = %d", len(synthetic))
		for _, n := range synthetic {
			c.Check(n.Path == nil, "synthetic node %d has path", n.ID)
			c.Check(n.ID != 15, "synthetic node impersonates its parent div")
		}
		if len(synthetic) == 2 {
			c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventContribution,
				NodeID: 15, SourceID: synthetic[0].ID, Level: 0, Before: 5, Operand: 1, After: 6}),
				"first synthetic paragraph did not contribute")
			c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventContribution,
				NodeID: 15, SourceID: synthetic[1].ID, Level: 0, Before: 6, Operand: 1, After: 7}),
				"second synthetic paragraph did not contribute")
		}
		c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventInitial,
			NodeID: 15, SourceID: 0, Level: -1, Before: 0, Operand: 5, After: 5}),
			"initialization of synthetic paragraphs' parent missing")

		// Level divisors and the four decimal write-back: the wrap div
		// sits at level 2 (divisor 6) and the body at level 3 (divisor 9).
		c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventContribution,
			NodeID: 5, SourceID: 8, Level: 2, Before: 5, Operand: 1.0 / 6.0, After: 5.1667}),
			"level 2 contribution with rounded write-back missing")
		c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventContribution,
			NodeID: 4, SourceID: 8, Level: 3, Before: 0, Operand: 1.0 / 9.0, After: 0.1111}),
			"level 3 contribution with rounded write-back missing")

		// Without links the density events scale by zero.
		c.Check(hasEvent(at.Events, SelectionEvent{Kind: SelectionEventDensity,
			NodeID: 6, SourceID: 0, Level: -1, Before: 9.5, Operand: 0, After: 9.5}),
			"density event of top candidate missing")

		// The top ranked candidate (only, id 6) is lifted to its parent
		// (wrap, id 5) before sibling merging.
		c.Check(len(at.RankedIDs) == 7 && at.RankedIDs[0] == 6, "ranked = %v", at.RankedIDs)
		c.Check(at.WinnerNodeID == 5, "winner = %d", at.WinnerNodeID)
		c.Check(at.WinnerNodeID != at.RankedIDs[0], "winner was not promoted")
		c.Check(at.CleanLength == 211, "clean length = %d", at.CleanLength)

		// A single-child chain: the top ranked candidate (w3) is lifted
		// two levels up to w1 before the content is used.
		pc := strings.Repeat("c", 100)
		promo := `<html><head><title>p</title></head><body>` +
			`<div id="w1"><div id="w2"><div id="w3"><p>` + pc + `</p><p>` + pc + `</p></div></div></div>` +
			`</body></html>`
		ps2 := NewParser()
		ps2.ExplainSelection = true
		ps2.CharThresholds = 100
		article2, err := ps2.Parse(strings.NewReader(promo), traceTestURL())
		c.Check(err == nil, "parse failed: %v", err)
		trace2 := article2.SelectionTrace
		c.Check(trace2 != nil && len(trace2.Attempts) == 1, "promo trace broken")
		if trace2 != nil && len(trace2.Attempts) == 1 {
			checkTraceConsistency(c, trace2)
			at2 := trace2.Attempts[0]
			// w3=7 scores highest, then w2=6, w1=5, body=4.
			c.Check(reflect.DeepEqual(at2.RankedIDs, []int{7, 6, 5, 4}),
				"promo ranked = %v", at2.RankedIDs)
			c.Check(at2.WinnerNodeID == 5, "promo winner = %d", at2.WinnerNodeID)
			c.Check(trace2.ChosenAttempt == 0, "promo chosen = %d", trace2.ChosenAttempt)
		}
	})
}

func TestSelectionTraceAttempts(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A page that fails the character threshold in all four
		// relaxation rounds. The unlikely "breadcrumbs" block is dropped
		// in round 0, included in round 1 (the longest result), and the
		// class weights decide the selection in rounds 2-3, so the
		// longest result is not in the last round.
		pa := strings.Repeat("a", 200)
		pb := strings.Repeat("b", 120)
		raw := `<html><head><title>four</title></head><body><div id="outer" class="content">` +
			`<div id="a"><p>` + pa + `</p><p>` + pa + `</p></div>` +
			`<div id="b" class="breadcrumbs"><p>` + pb + `</p><p>` + pb + `</p></div>` +
			`</div></body></html>`

		doc, err := html.Parse(strings.NewReader(raw))
		c.Check(err == nil, "html parse failed: %v", err)
		var before bytes.Buffer
		html.Render(&before, doc)

		ps := NewParser()
		ps.ExplainSelection = true
		ps.CharThresholds = 100000
		articleDoc, err := ps.ParseDocument(doc, traceTestURL())
		c.Check(err == nil, "parse failed: %v", err)
		trace := articleDoc.SelectionTrace
		c.Check(trace != nil, "trace is nil")
		if trace == nil {
			return
		}
		checkTraceConsistency(c, trace)

		// The input tree must not be modified.
		var after bytes.Buffer
		html.Render(&after, doc)
		c.Check(before.String() == after.String(), "input tree changed")

		// Four attempts in execution order, with the flags as they were
		// at the start of each round.
		c.Check(len(trace.Attempts) == 4, "attempts = %d", len(trace.Attempts))
		wantFlags := [][3]bool{{true, true, true}, {false, true, true}, {false, false, true}, {false, false, false}}
		wantClean := []int{400, 640, 400, 400}
		wantWinner := []int{5, 5, 6, 6}
		wantRanked := [][]int{{5, 6, 4}, {5, 6, 9, 4}, {6, 5, 9, 4}, {6, 5, 9, 4}}
		for i, at := range trace.Attempts {
			c.Check(at.Index == i, "attempt %d index = %d", i, at.Index)
			c.Check(at.StripUnlikelys == wantFlags[i][0] &&
				at.UseWeightClasses == wantFlags[i][1] &&
				at.CleanConditionally == wantFlags[i][2],
				"attempt %d flags = %v/%v/%v", i,
				at.StripUnlikelys, at.UseWeightClasses, at.CleanConditionally)
			c.Check(at.CleanLength == wantClean[i],
				"attempt %d clean = %d, want %d", i, at.CleanLength, wantClean[i])
			c.Check(at.WinnerNodeID == wantWinner[i],
				"attempt %d winner = %d, want %d", i, at.WinnerNodeID, wantWinner[i])
			c.Check(reflect.DeepEqual(at.RankedIDs, wantRanked[i]),
				"attempt %d ranked = %v, want %v", i, at.RankedIDs, wantRanked[i])
		}

		// The longest result came from round 1, not the last round, and
		// ChosenAttempt points at its original index even though the
		// attempts were sorted by length to pick it.
		maxClean, maxIndex := -1, -1
		for i, at := range trace.Attempts {
			if at.CleanLength > maxClean {
				maxClean, maxIndex = at.CleanLength, i
			}
		}
		c.Check(maxIndex == 1 && maxIndex != len(trace.Attempts)-1,
			"longest attempt index = %d", maxIndex)
		c.Check(trace.ChosenAttempt == 1, "chosen = %d", trace.ChosenAttempt)
		c.Check(articleDoc.Length == 640, "article length = %d", articleDoc.Length)
		c.Check(trace.Attempts[trace.ChosenAttempt].CleanLength == articleDoc.Length,
			"chosen clean length %d != article length %d",
			trace.Attempts[trace.ChosenAttempt].CleanLength, articleDoc.Length)

		// Cloned trees of different attempts keep the same IDs for the
		// same original elements: the scored paragraphs and the
		// initialized candidates are identical across rounds.
		wantSources := []map[int]bool{
			{7: true, 8: true},
			{7: true, 8: true, 10: true, 11: true},
			{7: true, 8: true, 10: true, 11: true},
			{7: true, 8: true, 10: true, 11: true},
		}
		wantInitials := []map[int]bool{
			{4: true, 5: true, 6: true},
			{4: true, 5: true, 6: true, 9: true},
			{4: true, 5: true, 6: true, 9: true},
			{4: true, 5: true, 6: true, 9: true},
		}
		for i, at := range trace.Attempts {
			c.Check(reflect.DeepEqual(contributionSources(at), wantSources[i]),
				"attempt %d sources = %v", i, contributionSources(at))
			c.Check(reflect.DeepEqual(initialNodes(at), wantInitials[i]),
				"attempt %d initials = %v", i, initialNodes(at))
		}

		// Both entry points number the same HTML identically.
		articleReader, err := ps.Parse(strings.NewReader(raw), traceTestURL())
		c.Check(err == nil, "reader parse failed: %v", err)
		c.Check(articleReader.SelectionTrace != nil, "reader trace is nil")
		if articleReader.SelectionTrace != nil {
			c.Check(reflect.DeepEqual(articleDoc.SelectionTrace, articleReader.SelectionTrace),
				"entry points produced different traces")
			c.Check(articleDoc.Content == articleReader.Content &&
				articleDoc.TextContent == articleReader.TextContent &&
				articleDoc.Length == articleReader.Length,
				"entry points produced different content")
		}

		// With the switch off the trace is nil and the extraction result
		// is unchanged; turning it back on reproduces the same trace.
		ps.ExplainSelection = false
		articleOff, err := ps.Parse(strings.NewReader(raw), traceTestURL())
		c.Check(err == nil, "parse failed: %v", err)
		c.Check(articleOff.SelectionTrace == nil, "trace present while disabled")
		c.Check(articleOff.Content == articleDoc.Content &&
			articleOff.TextContent == articleDoc.TextContent &&
			articleOff.Length == articleDoc.Length &&
			articleOff.Excerpt == articleDoc.Excerpt &&
			articleOff.Title == articleDoc.Title,
			"extraction differs when trace is disabled")

		snapshot, err := json.Marshal(articleDoc.SelectionTrace)
		c.Check(err == nil, "cannot snapshot trace: %v", err)

		ps.ExplainSelection = true
		articleOn, err := ps.Parse(strings.NewReader(raw), traceTestURL())
		c.Check(err == nil, "parse failed: %v", err)
		c.Check(reflect.DeepEqual(articleOn.SelectionTrace, articleDoc.SelectionTrace),
			"same page produced different trace after toggle")

		// A failing parse returns an error and no trace, and the parser
		// can still be reused afterwards.
		ps.MaxElemsToParse = 2
		articleFail, err := ps.Parse(strings.NewReader(raw), traceTestURL())
		c.Check(err != nil, "oversized document did not fail")
		c.Check(articleFail.SelectionTrace == nil, "failed parse returned a trace")
		ps.MaxElemsToParse = 0
		articleReuse, err := ps.Parse(strings.NewReader(raw), traceTestURL())
		c.Check(err == nil, "reuse after failure failed: %v", err)
		c.Check(articleReuse.SelectionTrace != nil &&
			len(articleReuse.SelectionTrace.Attempts) == 4,
			"reuse after failure produced broken trace")

		// None of the later parses, toggles or failures may rewrite the
		// diagnostics returned earlier.
		snapshotAfter, err := json.Marshal(articleDoc.SelectionTrace)
		c.Check(err == nil, "cannot re-snapshot trace: %v", err)
		c.Check(bytes.Equal(snapshot, snapshotAfter),
			"previously returned trace was modified")
	})
}

// domText returns the text content of a node, using only the standard
// library structure of the tree.
func domText(n *html.Node) string {
	var sb strings.Builder
	var walk func(*html.Node)
	walk = func(cur *html.Node) {
		if cur.Type == html.TextNode {
			sb.WriteString(cur.Data)
		}
		for child := cur.FirstChild; child != nil; child = child.NextSibling {
			walk(child)
		}
	}
	walk(n)
	return sb.String()
}
