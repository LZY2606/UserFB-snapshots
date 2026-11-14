package goquery

import (
	"errors"
	"fmt"
	"io"
	"strings"
	"testing"

	"github.com/PuerkitoBio/goquery/task_support/taskcheck"
	"golang.org/x/net/html"
)

// scriptedReader serves pre-split chunks, one chunk per Read call. It can also
// model an input gate: while the chunk at gateAt has not been released, any
// attempt to read it returns errGateClosed instead of bytes.
type scriptedReader struct {
	chunks   [][]byte
	idx      int
	off      int
	gateAt   int
	gateOpen bool
	blocked  bool
	reads    int
	closes   int
}

var errGateClosed = errors.New("source read past the delivery frontier")

func newScriptedReader(parts ...string) *scriptedReader {
	r := &scriptedReader{gateAt: -1}
	for _, p := range parts {
		r.chunks = append(r.chunks, []byte(p))
	}
	return r
}

func newByteChunkedReader(s string) *scriptedReader {
	r := &scriptedReader{gateAt: -1}
	for i := 0; i < len(s); i++ {
		r.chunks = append(r.chunks, []byte{s[i]})
	}
	return r
}

func (r *scriptedReader) Read(p []byte) (int, error) {
	r.reads++
	if r.idx == r.gateAt && !r.gateOpen {
		r.blocked = true
		return 0, errGateClosed
	}
	if r.idx >= len(r.chunks) {
		return 0, io.EOF
	}
	chunk := r.chunks[r.idx]
	n := copy(p, chunk[r.off:])
	r.off += n
	if r.off == len(chunk) {
		r.idx++
		r.off = 0
	}
	return n, nil
}

func (r *scriptedReader) Close() error {
	r.closes++
	return nil
}

// bulkErrReader returns all its data together with err in a single Read, as a
// regular io.Reader is allowed to do.
type bulkErrReader struct {
	data []byte
	err  error
	done bool
	read int
}

func (r *bulkErrReader) Read(p []byte) (int, error) {
	r.read++
	if r.done {
		return 0, r.err
	}
	r.done = true
	return copy(p, r.data), r.err
}

func streamDocBody(d *Document) *html.Node {
	return d.Find("body").Get(0)
}

func attrValue(n *html.Node, key string) string {
	for _, a := range n.Attr {
		if a.Key == key {
			return a.Val
		}
	}
	return ""
}

func renderChildren(n *html.Node) string {
	var b strings.Builder
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		if err := html.Render(&b, c); err != nil {
			b.WriteString("render-error")
		}
	}
	return b.String()
}

func countNodes(n *html.Node) int {
	count := 0
	for c := n.FirstChild; c != nil; c = c.NextSibling {
		count += 1 + countNodes(c)
	}
	return count
}

func fullParseBody(src string, opts ...html.ParseOption) *html.Node {
	root, err := html.ParseWithOptions(strings.NewReader(src), opts...)
	if err != nil {
		return nil
	}
	var find func(*html.Node) *html.Node
	find = func(n *html.Node) *html.Node {
		if n.Type == html.ElementNode && n.DataAtom.String() == "body" && n.Namespace == "" {
			return n
		}
		for c := n.FirstChild; c != nil; c = c.NextSibling {
			if got := find(c); got != nil {
				return got
			}
		}
		return nil
	}
	return find(root)
}

func checkBatchSkeleton(c *taskcheck.C, d *Document) {
	root := d.Get(0)
	c.Check(root != nil && root.Type == html.DocumentNode, "batch root is not a document node")
	htmlNode := d.Find("html").Get(0)
	c.Check(htmlNode != nil, "batch has no html element")
	if htmlNode != nil {
		c.Check(len(htmlNode.Attr) == 0, "batch html element copied attributes from the source page")
	}
	head := d.Find("head").Get(0)
	c.Check(head != nil, "batch has no head element")
	if head != nil {
		c.Check(head.FirstChild == nil, "batch head is not empty")
	}
	body := streamDocBody(d)
	c.Check(body != nil, "batch has no body element")
	if body != nil {
		c.Check(len(body.Attr) == 0, "batch body element copied attributes from the source page")
	}
}

func TestBodyStreamFrontier(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const prefix = `<body><p id="p1">A</p>`
		const suffix = `tail<table id="t">F<tr><td>cell</td></tr></table>` +
			`<p><b>1<i>2</b>3</i></p>` +
			`<template id="tpl"><p>tp</p></template>` +
			`<form id="f"><p>fx</p></form>` +
			`<p id="p2">Z</p>end</body>t2`

		reader := newScriptedReader(prefix)
		reader.gateAt = 1
		for i := 0; i < len(suffix); i++ {
			reader.chunks = append(reader.chunks, []byte{suffix[i]})
		}

		var batches []string
		var docs []*Document
		stats, err := StreamBody(reader, func(d *Document) error {
			checkBatchSkeleton(c, d)
			if len(batches) == 0 {
				// The only accepted way to get past the gate is this callback.
				c.Check(!reader.blocked, "parser tried to read the suffix before the first batch was delivered")
				c.Check(reader.idx == 1 && reader.off == 0, "parser read past the exact prefix before delivering the first batch")
				reader.gateOpen = true
			}
			body := streamDocBody(d)
			batches = append(batches, renderChildren(body))
			docs = append(docs, d)
			return nil
		})

		c.Check(err == nil, "unexpected error: %v", err)
		c.Check(reader.blocked == false, "gate reader was hit before delivery")
		c.Check(reader.gateOpen, "suffix was never released by a batch")
		c.Check(len(batches) == 7, "got %d batches, want 7: %q", len(batches), batches)
		want := []string{
			`<p id="p1">A</p>`,
			`tailF<table id="t"><tbody><tr><td>cell</td></tr></tbody></table>`,
			`<p><b>1<i>2</i></b><i>3</i></p>`,
			`<template id="tpl"><p>tp</p></template>`,
			`<form id="f"><p>fx</p></form>`,
			`<p id="p2">Z</p>`,
			`endt2`,
		}
		for i := range want {
			if i < len(batches) {
				c.Check(batches[i] == want[i], "batch %d: got %q, want %q", i, batches[i], want[i])
			}
		}
		c.Check(stats.Batches == 7, "stats.Batches = %d, want 7", stats.Batches)
		c.Check(stats.Nodes == 24, "stats.Nodes = %d, want 24", stats.Nodes)

		if len(docs) >= 4 {
			// Table foster parenting keeps the moved-out text complete and
			// before the table in the same batch.
			body := streamDocBody(docs[1])
			first := body.FirstChild
			c.Check(first != nil && first.Type == html.TextNode && first.Data == "tailF",
				"foster text was not delivered as one complete text node: %#v", first)
			if first != nil && first.NextSibling != nil {
				tbl := first.NextSibling
				c.Check(tbl.Type == html.ElementNode && tbl.DataAtom.String() == "table" && attrValue(tbl, "id") == "t",
					"table did not follow the foster text")
				c.Check(tbl.FirstChild != nil && tbl.FirstChild.DataAtom.String() == "tbody",
					"table did not get an implicit tbody")
			}

			// Formatting element reconstruction: the adopted b/i tree is
			// exactly what a full parse produces.
			c.Check(docs[2].Find("p > b > i").Text() == "2", "misnested formatting did not reconstruct b > i")
			c.Check(docs[2].Find("p > i").Length() == 1 && docs[2].Find("p > i").Text() == "3",
				"misnested formatting did not produce the adopted sibling i")

			// A template is entirely in one batch; it is never split.
			tplBatches := 0
			for _, d := range docs {
				if d.Find("template").Length() > 0 {
					tplBatches++
					c.Check(d.Find("template p").Text() == "tp", "template content was split across batches")
				}
			}
			c.Check(tplBatches == 1, "template appeared in %d batches, want 1", tplBatches)

			// The form pointer was cleared before the following paragraph was
			// committed, so p2 is never in the same batch as the form.
			formIdx, p2Idx := -1, -1
			for i, d := range docs {
				if d.Find("form").Length() > 0 {
					formIdx = i
				}
				if d.Find("#p2").Length() > 0 {
					p2Idx = i
				}
			}
			c.Check(formIdx >= 0 && p2Idx > formIdx, "form and following paragraph were committed together (form=%d p2=%d)", formIdx, p2Idx)

			// The trailing text node was held back and later input was merged
			// into the very same node instead of creating a second one.
			lastBody := streamDocBody(docs[len(docs)-1])
			c.Check(lastBody.FirstChild != nil && lastBody.FirstChild == lastBody.LastChild,
				"trailing text was split into multiple nodes")
			if lastBody.FirstChild != nil {
				c.Check(lastBody.FirstChild.Type == html.TextNode && lastBody.FirstChild.Data == "endt2",
					"trailing text was not merged completely: %q", lastBody.FirstChild.Data)
			}
		}

		// Later input must not mutate an earlier batch.
		if len(docs) > 0 {
			body := streamDocBody(docs[0])
			c.Check(renderChildren(body) == `<p id="p1">A</p>`, "an earlier batch changed while the stream continued")
			p1 := docs[0].Find("#p1").Get(0)
			c.Check(p1 != nil && p1.Parent == body, "delivered node is not a direct child of the batch body")
		}
	})
}

func TestBodyStreamReclaim(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		run := func(t *taskcheck.C, records int, keepFirst bool) (BodyStreamStats, []string, *Document, *html.Node) {
			var src strings.Builder
			src.WriteString("<body>")
			for i := 0; i < records; i++ {
				fmt.Fprintf(&src, "<p>%d</p>", i)
			}
			reader := strings.NewReader(src.String())
			var stats BodyStreamStats
			var rendered []string
			var first *Document
			var firstP *html.Node
			var err error
			stats, err = StreamBody(reader, func(d *Document) error {
				body := streamDocBody(d)
				rendered = append(rendered, renderChildren(body))
				if first == nil {
					first = d
					firstP = d.Find("p").Get(0)
					if keepFirst {
						// Edit the retained batch from inside the callback.
						d.Find("body").AppendHtml(`<span id="marker">M</span>`)
						if firstP != nil && firstP.FirstChild != nil {
							firstP.FirstChild.Data += "!"
						}
					}
				} else if keepFirst {
					if d.Find("#marker").Length() != 0 {
						t.Check(false, "a later batch contains an edit made to an earlier batch")
					}
					if first.Find("#marker").Length() != 1 {
						t.Check(false, "the retained batch lost its own edit")
					}
				}
				return nil
			})
			t.Check(err == nil, "unexpected error: %v", err)
			return stats, rendered, first, firstP
		}

		const records = 300
		stats, rendered, first, firstP := run(c, records, true)
		c.Check(stats.Batches == records, "stats.Batches = %d, want %d", stats.Batches, records)
		c.Check(stats.Nodes == 2*records, "stats.Nodes = %d, want %d", stats.Nodes, 2*records)
		c.Check(stats.PeakPendingNodes == 2, "stats.PeakPendingNodes = %d, want a small constant 2", stats.PeakPendingNodes)
		c.Check(len(rendered) == records, "got %d batches, want %d", len(rendered), records)
		for _, i := range []int{0, 1, records / 2, records - 1} {
			if i < len(rendered) {
				c.Check(rendered[i] == fmt.Sprintf("<p>%d</p>", i), "batch %d = %q", i, rendered[i])
			}
		}
		// The first batch was retained and rewritten; the stream itself must
		// have stayed isolated from those edits.
		c.Check(first != nil && first.Find("p").Length() == 1, "retained batch lost its paragraph")
		c.Check(first != nil && first.Find("#marker").Length() == 1, "retained batch lost the appended marker")
		c.Check(firstP != nil && firstP.FirstChild != nil && firstP.FirstChild.Data == "0!",
			"retained paragraph edit was lost: %#v", firstP)
		if first != nil {
			body := streamDocBody(first)
			c.Check(renderChildren(body) == `<p>0!</p><span id="marker">M</span>`,
				"retained batch render = %q", renderChildren(body))
		}

		// Peak pending nodes must not grow with the number of independent
		// records: a much longer stream has exactly the same peak.
		longStats, longRendered, _, _ := run(c, 4000, false)
		c.Check(longStats.PeakPendingNodes == stats.PeakPendingNodes,
			"peak pending nodes grew with stream length: %d vs %d", longStats.PeakPendingNodes, stats.PeakPendingNodes)
		c.Check(longStats.Nodes == 2*4000, "long stream stats.Nodes = %d, want %d", longStats.Nodes, 2*4000)
		c.Check(len(longRendered) == 4000, "long stream produced %d batches", len(longRendered))
		c.Check(longRendered[len(longRendered)-1] == "<p>3999</p>", "long stream last batch = %q", longRendered[len(longRendered)-1])
	})
}

func TestBodyStreamErrors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// nil Reader / nil callback fail before reading anything.
		called := false
		stats, err := StreamBody(nil, func(*Document) error { called = true; return nil })
		c.Check(err != nil, "nil Reader did not produce an error")
		c.Check(!called, "nil Reader still invoked the callback")
		c.Check(stats == (BodyStreamStats{}), "nil Reader produced non-zero stats: %+v", stats)
		_, htmlErr := html.ParseBodyStream(nil, func(*html.Node) error { return nil })
		c.Check(htmlErr != nil, "html.ParseBodyStream accepted a nil Reader")

		reader := strings.NewReader("<body><p>A</p>")
		stats, err = StreamBody(reader, nil)
		c.Check(err != nil, "nil yield did not produce an error")
		c.Check(stats == (BodyStreamStats{}), "nil yield produced non-zero stats: %+v", stats)
		_, htmlErr = html.ParseBodyStream(strings.NewReader("x"), nil)
		c.Check(htmlErr != nil, "html.ParseBodyStream accepted a nil yield")

		// A reader returning (n > 0, err): the complete tokens in those bytes
		// are parsed first, then the error is observed unchanged.
		fault := errors.New("source interrupted")
		bulk := &bulkErrReader{data: []byte(`<body><p>A</p><p>B</p>`), err: fault}
		var got []string
		stats, err = StreamBody(bulk, func(d *Document) error {
			got = append(got, renderChildren(streamDocBody(d)))
			return nil
		})
		c.Check(err == fault, "reader error identity lost: %v", err)
		c.Check(len(got) == 2 && got[0] == "<p>A</p>" && got[1] == "<p>B</p>",
			"complete tokens returned with the error were lost: %q", got)
		c.Check(stats.Batches == 2, "stats.Batches = %d, want 2", stats.Batches)

		// An incomplete trailing token must not be repaired as if it were EOF.
		bulk = &bulkErrReader{data: []byte(`<body><p>A</p><p`), err: fault}
		got = nil
		stats, err = StreamBody(bulk, func(d *Document) error {
			got = append(got, renderChildren(streamDocBody(d)))
			return nil
		})
		c.Check(err == fault, "partial-token reader error identity lost: %v", err)
		c.Check(len(got) == 1 && got[0] == "<p>A</p>", "a not-yet-stable token was published on reader failure: %q", got)
		c.Check(stats.Batches == 1, "stats.Batches = %d, want 1", stats.Batches)

		// A stable node is not enough: the trailing text is still unstable, so
		// a reader failure must not force it out as an EOF flush.
		bulk = &bulkErrReader{data: []byte(`<body><p>A</p>tail`), err: fault}
		got = nil
		stats, err = StreamBody(bulk, func(d *Document) error {
			got = append(got, renderChildren(streamDocBody(d)))
			return nil
		})
		c.Check(err == fault, "tail reader error identity lost: %v", err)
		c.Check(len(got) == 1 && got[0] == "<p>A</p>", "reader failure forced out unstable tail text: %q", got)

		// A callback error is returned unchanged, the failing batch is counted
		// and no further Read happens.
		cbErr := errors.New("stop consuming")
		gate := newByteChunkedReader(`<body><p>A</p><p>B</p><p>C</p>`)
		var readsAtError int
		batches := 0
		stats, err = StreamBody(gate, func(d *Document) error {
			batches++
			if batches == 2 {
				readsAtError = gate.reads
				return cbErr
			}
			return nil
		})
		c.Check(err == cbErr, "callback error identity lost: %v", err)
		c.Check(batches == 2, "callback was invoked %d times, want 2", batches)
		c.Check(stats.Batches == 2, "stats.Batches = %d, want the failing batch included", stats.Batches)
		c.Check(stats.Nodes == 4, "stats.Nodes = %d, want 4", stats.Nodes)
		c.Check(gate.reads == readsAtError, "reader was read after the callback returned an error (%d -> %d)", readsAtError, gate.reads)

		// The reader is never closed, on success or on failure.
		closer := newScriptedReader(`<body><p>A</p>`)
		_, err = StreamBody(closer, func(*Document) error { return nil })
		c.Check(err == nil, "unexpected error: %v", err)
		c.Check(closer.closes == 0, "StreamBody closed the caller's Reader on success")
		closer = newScriptedReader(`<body><p>A</p><p>B</p>`)
		_, err = StreamBody(closer, func(*Document) error { return cbErr })
		c.Check(err == cbErr, "unexpected error: %v", err)
		c.Check(closer.closes == 0, "StreamBody closed the caller's Reader on failure")

		// True EOF delivers the remaining tolerant parse result.
		src := `<body><p>A</p><div><span>tail`
		var tail []string
		stats, err = StreamBody(strings.NewReader(src), func(d *Document) error {
			tail = append(tail, renderChildren(streamDocBody(d)))
			return nil
		})
		c.Check(err == nil, "EOF returned an error: %v", err)
		c.Check(len(tail) >= 2, "expected the closed prefix and an EOF tail batch, got %q", tail)
		if len(tail) >= 2 {
			c.Check(tail[0] == "<p>A</p>", "first batch = %q", tail[0])
			c.Check(tail[len(tail)-1] == "<div><span>tail</span></div>", "EOF tail batch = %q", tail[len(tail)-1])
		}
		c.Check(stats.Nodes == 5, "stats.Nodes = %d, want 5 (p, div, span, 2 texts)", stats.Nodes)

		// A frameset document has no body and therefore produces no batch.
		batches = 0
		stats, err = StreamBody(strings.NewReader(`<html><frameset><frame src="a"></frameset>`), func(*Document) error {
			batches++
			return nil
		})
		c.Check(err == nil, "frameset returned an error: %v", err)
		c.Check(batches == 0, "frameset produced %d batches", batches)
		c.Check(stats == (BodyStreamStats{}), "frameset produced stats %+v", stats)

		// A failed call does not leak state into the next call.
		var second []string
		stats, err = StreamBody(strings.NewReader(`<body><p>X</p>`), func(d *Document) error {
			second = append(second, renderChildren(streamDocBody(d)))
			return nil
		})
		c.Check(err == nil, "second call returned an error: %v", err)
		c.Check(len(second) == 1 && second[0] == "<p>X</p>", "second call batches = %q", second)
		c.Check(stats.Batches == 1 && stats.Nodes == 2, "second call stats = %+v", stats)
	})
}

func TestBodyStreamParity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		collect := func(src string, r io.Reader, opts ...html.ParseOption) (string, []string, BodyStreamStats, error) {
			var batches []string
			stats, err := StreamBody(r, func(d *Document) error {
				checkBatchSkeleton(c, d)
				batches = append(batches, renderChildren(streamDocBody(d)))
				return nil
			}, opts...)
			return strings.Join(batches, ""), batches, stats, err
		}

		// Independent, hand-written structural expectations. These are not
		// derived from a second parse call.
		cases := []struct {
			name string
			src  string
			want string
		}{
			{
				name: "table foster parenting",
				src:  `<body><p>A</p><table>x<tr><td>y</td></tr></table>`,
				want: `<p>A</p>x<table><tbody><tr><td>y</td></tr></tbody></table>`,
			},
			{
				name: "formatting reconstruction",
				src:  `<body><b>1<p>2</b>3</p>`,
				want: `<b>1</b><p><b>2</b>3</p>`,
			},
			{
				name: "unclosed template",
				src:  `<body><template><p>tp</p>`,
				want: `<template><p>tp</p></template>`,
			},
			{
				name: "form pointer in table",
				src:  `<body><table><form><p>a</p></form><tr><td>c</td></tr></table><p>z</p>`,
				want: `<p>a</p><table><form></form><tbody><tr><td>c</td></tr></tbody></table><p>z</p>`,
			},
			{
				name: "implicit paragraph closes",
				src:  `<body><p>a<div>b</div><p>c`,
				want: `<p>a</p><div>b</div><p>c</p>`,
			},
		}
		for _, tc := range cases {
			want := tc.want
			// One-shot input.
			concat, _, stats, err := collect(tc.src, strings.NewReader(tc.src))
			c.Check(err == nil, "%s: unexpected error: %v", tc.name, err)
			c.Check(concat == want, "%s: streamed body = %q, want %q", tc.name, concat, want)

			// The same source split into many small pieces must not change
			// the result.
			chunked, chunkBatches, chunkStats, err := collect(tc.src, newByteChunkedReader(tc.src))
			c.Check(err == nil, "%s: chunked error: %v", tc.name, err)
			c.Check(chunked == want, "%s: chunked body = %q, want %q", tc.name, chunked, want)
			c.Check(chunkStats.Batches == stats.Batches && chunkStats.Nodes == stats.Nodes,
				"%s: chunked stats %+v differ from one-shot %+v", tc.name, chunkStats, stats)
			c.Check(len(chunkBatches) == stats.Batches, "%s: chunk batch count mismatch", tc.name)

			// And the independent expectation must agree with a regular full
			// parse of the same input.
			fullBody := fullParseBody(tc.src)
			var full string
			if fullBody != nil {
				full = renderChildren(fullBody)
			}
			c.Check(full == want, "%s: ordinary Parse body = %q, want %q", tc.name, full, want)
			if fullBody != nil {
				c.Check(stats.Nodes == countNodes(fullBody), "%s: stats.Nodes = %d, full parse has %d", tc.name, stats.Nodes, countNodes(fullBody))
			}
		}

		// Page-level head/doctype/html-body attributes are out of scope; only
		// body content is carried over, including comments.
		src := `<html lang="en"><head><title>t</title></head><body class="page"><!--note--><p>x</p></body></html>`
		concat, _, stats, err := collect(src, strings.NewReader(src))
		c.Check(err == nil, "unexpected error: %v", err)
		c.Check(concat == `<!--note--><p>x</p>`, "body content = %q", concat)
		c.Check(stats.Nodes == 3, "stats.Nodes = %d, want comment + p + text", stats.Nodes)

		// Non-default scripting option is honored for the whole stream.
		const noscriptDoc = `<body><p>before</p><noscript><a href="http://example.org">click</a></noscript></body>`
		concat, _, _, err = collect(noscriptDoc, strings.NewReader(noscriptDoc))
		c.Check(err == nil, "unexpected error: %v", err)
		c.Check(strings.Contains(concat, `href="http://example.org"`), "scripting-enabled stream lost raw noscript markup: %q", concat)
		c.Check(strings.Contains(concat, `click`), "scripting-enabled stream lost noscript text")

		var disabledDocs []*Document
		var disabledBatches []string
		_, err = StreamBody(strings.NewReader(noscriptDoc), func(d *Document) error {
			disabledDocs = append(disabledDocs, d)
			disabledBatches = append(disabledBatches, renderChildren(streamDocBody(d)))
			return nil
		}, html.ParseOptionEnableScripting(false))
		c.Check(err == nil, "unexpected error: %v", err)
		foundLink := false
		for _, d := range disabledDocs {
			if href, ok := d.Find("noscript a").Attr("href"); ok && href == "http://example.org" {
				foundLink = true
			}
		}
		c.Check(foundLink, "scripting-disabled stream did not parse noscript content as markup: %q", disabledBatches)
		full := fullParseBody(noscriptDoc, html.ParseOptionEnableScripting(false))
		c.Check(strings.Join(disabledBatches, "") == renderChildren(full),
			"scripting-disabled stream disagrees with full parse: %q vs %q", strings.Join(disabledBatches, ""), renderChildren(full))

		// A batch document keeps the parse options for HTML added later.
		var batchDoc *Document
		_, err = StreamBody(strings.NewReader(noscriptDoc), func(d *Document) error {
			if batchDoc == nil {
				batchDoc = d
			}
			return nil
		}, html.ParseOptionEnableScripting(false))
		c.Check(err == nil, "unexpected error: %v", err)
		if batchDoc != nil {
			batchDoc.Find("body").AppendHtml(`<noscript><a id="added" href="u">z</a></noscript>`)
			c.Check(batchDoc.Find("#added").Length() == 1, "AppendHtml did not inherit the stream's scripting option")
		}

		var defaultDoc *Document
		_, err = StreamBody(strings.NewReader(noscriptDoc), func(d *Document) error {
			if defaultDoc == nil {
				defaultDoc = d
			}
			return nil
		})
		c.Check(err == nil, "unexpected error: %v", err)
		if defaultDoc != nil {
			defaultDoc.Find("body").AppendHtml(`<noscript><a id="added" href="u">z</a></noscript>`)
			c.Check(defaultDoc.Find("#added").Length() == 0, "default stream unexpectedly parsed noscript markup")
		}
	})
}
