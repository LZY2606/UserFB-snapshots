package jar

import (
	"container/list"
	"net/http"

	"github.com/PuerkitoBio/goquery"
)

// ForkableHistory is an optional interface a History implementation may
// satisfy to support forking, eg branching a browser tab into two
// independent tabs. The History interface itself does not require it, and
// browsers using a History implementation that does not satisfy
// ForkableHistory cannot be forked.
type ForkableHistory interface {
	History

	// Fork returns an independent copy of the history. The returned history
	// holds deep copies of the recorded states, in the same order and with
	// the same maximum length as the original, so that Push, Pop, Clear and
	// SetMax called on either history do not affect the other. The original
	// history must not be modified by the call.
	Fork() (History, error)
}

// Fork creates and returns an independent copy of the history. Every
// recorded state is deep copied, the order of the states and the maximum
// history length are preserved, and the original history is left untouched.
func (his *MemoryHistory) Fork() (History, error) {
	fork := &MemoryHistory{
		list:    list.New(),
		maxHist: his.maxHist,
	}
	for e := his.list.Back(); e != nil; e = e.Prev() {
		fork.list.PushFront(e.Value.(*State).Fork())
	}
	return fork, nil
}

// Fork returns a deep copy of the state. The copy has its own DOM nodes,
// request and response headers, URLs and other mutable metadata, so it may
// be modified without affecting the original state. The request and
// response bodies are neither read, closed nor rewound; the copy references
// the same body readers as the original.
func (s *State) Fork() *State {
	if s == nil {
		return nil
	}
	return &State{
		Request:  forkRequest(s.Request),
		Response: forkResponse(s.Response),
		Dom:      forkDocument(s.Dom),
	}
}

// forkRequest returns a deep copy of the given request. The body is not
// read, closed or rewound; the copy references the same body reader.
func forkRequest(req *http.Request) *http.Request {
	if req == nil {
		return nil
	}
	return req.Clone(req.Context())
}

// forkResponse returns a deep copy of the given response, including its
// headers and the request that elicited it. The body is not read, closed or
// rewound; the copy references the same body reader.
func forkResponse(resp *http.Response) *http.Response {
	if resp == nil {
		return nil
	}
	fork := new(http.Response)
	*fork = *resp
	fork.Header = resp.Header.Clone()
	fork.Request = forkRequest(resp.Request)
	return fork
}

// forkDocument returns a deep copy of the given document. The copy holds
// its own node tree, with the parent-child relationships between the nodes
// preserved, so DOM modifications made through one document never affect
// the other.
func forkDocument(doc *goquery.Document) *goquery.Document {
	if doc == nil {
		return nil
	}
	fork := goquery.CloneDocument(doc)
	if doc.Url != nil {
		u := *doc.Url
		fork.Url = &u
	}
	return fork
}
