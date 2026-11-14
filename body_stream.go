package goquery

import (
	"errors"
	"io"

	"golang.org/x/net/html"
)

// BodyStreamStats reports the work done by StreamBody. It is an alias for the
// statistics type of the underlying streaming parser.
type BodyStreamStats = html.BodyStreamStats

var (
	errNilStreamBodyReader = errors.New("goquery: StreamBody called with a nil Reader")
	errNilStreamBodyYield  = errors.New("goquery: StreamBody called with a nil yield function")
)

// StreamBody parses the HTML from r and, instead of building the whole
// document first, hands off batches of body content as soon as the HTML5 tree
// construction algorithm has reached a safe boundary. It is intended for very
// long listing pages where keeping every already-closed record alive until EOF
// wastes memory.
//
// Each non-empty batch is a new Document with a minimal document/html/head/body
// skeleton: the head is empty and no attributes of the source html or body
// element are copied. The body owns the nodes that were moved out of the
// active parse tree, in document order. Nodes are moved, not cloned, so they
// keep their identity and the exact tree shape produced by HTML5 parsing
// (implicit paragraph closes, table foster parenting, formatting element
// reconstruction, template and form rules are all preserved). A batch is
// committed only when the open element stack is back to exactly a HTML
// html/body pair, the active formatting elements list, form pointer and
// template insertion mode stack are empty, frameset-ok is false and no foster
// insertion or token reprocessing is pending. The last direct text child of the
// body is kept back between batches because later text may still be merged
// into it.
//
// yield is called synchronously; no more input is consumed until it returns.
// The Document passed to yield can be queried and modified as usual (Find,
// Text, Contents, AppendHtml and so on). Later input cannot change a document
// that was already delivered, and modifications made by the callback cannot
// affect the remaining parse. The parse options given to StreamBody are stored
// on every batch Document, so HTML added later with AppendHtml or SetHtml is
// parsed with the same options.
//
// At true EOF the regular HTML5 end-of-file error handling is performed and
// all remaining body children, including unclosed elements and trailing text,
// are delivered as a final batch. A frameset document with no body produces no
// batches. Concatenating the bodies of the batches reproduces the body children
// of a regular full parse, assuming the caller does not modify them.
//
// If yield returns an error, parsing stops immediately, no further input is
// read and that error is returned unchanged. A non-EOF error from r is
// returned unchanged as well and does not cause the not-yet-safe tail to be
// delivered. A normal EOF returns a nil error. StreamBody never closes r; the
// caller remains responsible for that.
//
// The returned BodyStreamStats reports the number of committed batches, the
// total number of body nodes they contain (recursively, excluding the
// per-batch skeleton) and the peak number of nodes pending in the active body.
func StreamBody(r io.Reader, yield func(*Document) error, opts ...html.ParseOption) (BodyStreamStats, error) {
	if r == nil {
		return BodyStreamStats{}, errNilStreamBodyReader
	}
	if yield == nil {
		return BodyStreamStats{}, errNilStreamBodyYield
	}
	return html.ParseBodyStream(r, func(root *html.Node) error {
		return yield(newDocument(root, nil, opts...))
	}, opts...)
}
