package graphql

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"

	"github.com/machinebox/graphql/task_support/taskcheck"
)

// scriptedTransport is a controllable http.RoundTripper. Every request is
// recorded, its body is closed by the transport, and timing is driven by
// channels instead of timers.
type scriptedTransport struct {
	mu       sync.Mutex
	requests []recordedRequest
	closed   atomic.Int64

	arrived chan recordedRequest
	respond chan *scriptedResponse

	// When gate is non-nil every round trip blocks until it is closed; route
	// then picks the response from the outgoing query, so concurrent requests
	// still get deterministic bodies.
	gate  chan struct{}
	route func(query string) *scriptedResponse
}

type recordedRequest struct {
	method string
	query  string
	vars   map[string]interface{}
	header http.Header
}

type scriptedResponse struct {
	status int
	body   string
	err    error
}

type scriptedBody struct {
	io.Reader
	closed *atomic.Int64
}

func (b *scriptedBody) Close() error {
	b.closed.Add(1)
	return nil
}

func newScriptedTransport() *scriptedTransport {
	return &scriptedTransport{
		respond: make(chan *scriptedResponse, 8),
		arrived: make(chan recordedRequest, 8),
	}
}

func (t *scriptedTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	defer r.Body.Close()
	raw, err := io.ReadAll(r.Body)
	if err != nil {
		return nil, err
	}
	var body struct {
		Query     string                 `json:"query"`
		Variables map[string]interface{} `json:"variables"`
	}
	if err := json.Unmarshal(raw, &body); err != nil {
		return nil, err
	}
	rec := recordedRequest{
		method: r.Method,
		query:  body.Query,
		vars:   body.Variables,
		header: r.Header.Clone(),
	}
	t.mu.Lock()
	t.requests = append(t.requests, rec)
	t.mu.Unlock()
	select {
	case t.arrived <- rec:
	default:
	}
	if t.gate != nil {
		<-t.gate
	}
	var resp *scriptedResponse
	if t.route != nil {
		resp = t.route(body.Query)
	} else {
		resp = <-t.respond
	}
	if resp.err != nil {
		return nil, resp.err
	}
	return &http.Response{
		StatusCode: resp.status,
		Header:     make(http.Header),
		Body:       &scriptedBody{Reader: strings.NewReader(resp.body), closed: &t.closed},
		Request:    r,
	}, nil
}

func (t *scriptedTransport) snapshot() ([]recordedRequest, int) {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]recordedRequest, len(t.requests))
	copy(out, t.requests)
	return out, int(t.closed.Load())
}

func (t *scriptedTransport) callCount() int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return len(t.requests)
}

func collectClient(tr http.RoundTripper) *Client {
	return NewClient("https://collector.invalid/graphql", WithHTTPClient(&http.Client{Transport: tr}))
}

func mustCollect(c *taskcheck.C, client *Client, req *Request, col *Collection) {
	if err := client.Collect(context.Background(), req, col); err != nil {
		c.Check(false, "Collect failed: %v", err)
	}
}

// TestCollectionProjection covers two-way projection between response shapes
// and stored entities: aliases, fragments, directives, arguments, shared
// entities in lists and isolated embedded objects.
func TestCollectionProjection(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		tr := newScriptedTransport()
		client := collectClient(tr)
		col := NewCollection()

		first := NewRequest(`
			# first catalogue fetch
			query ProductV1($id: ID!) {
				left: product(id: $id) {
					t: __typename
					k: id
					name
					seller { __typename id name }
				}
			}
		`)
		first.Var("id", "7")
		first.Header.Set("X-Collection", "projection")
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"left":{"t":"Product","k":"7","name":"Paper","seller":{"__typename":"Seller","id":"s","name":"Old"}}}}`}
		mustCollect(c, client, first, col)

		// The request really went over the wire as POST JSON.
		reqs, closed := tr.snapshot()
		c.Check(len(reqs) == 1, "expected one POST, got %d", len(reqs))
		c.Check(reqs[0].method == http.MethodPost, "method was %q", reqs[0].method)
		c.Check(strings.Contains(reqs[0].query, "left: product(id: $id)"), "query not forwarded: %q", reqs[0].query)
		c.Check(reqs[0].vars["id"] == "7", "variable id not forwarded: %v", reqs[0].vars)
		c.Check(reqs[0].header.Get("X-Collection") == "projection", "custom header lost")
		c.Check(closed == 1, "body closed count = %d", closed)

		second := NewRequest(`query { right: product(id: "7") { __typename id price, seller { __typename id name } } }`)
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"right":{"__typename":"Product","id":"7","price":19,"seller":{"__typename":"Seller","id":"s","name":"New"}}}}`}
		mustCollect(c, client, second, col)

		// Read back through a totally different shape: renamed alias, variable
		// argument, named fragment and an inline fragment, plus directives.
		readReq := NewRequest(`
			query ReadBack($pid: ID!, $withSeller: Boolean!) {
				item: product(id: $pid) {
					__typename
					...ProductFields
					... on Product { price }
					seller @include(if: $withSeller) { __typename ... SellerFields }
					ignored: name @skip(if: true)
				}
			}
			fragment ProductFields on Product { t: __typename k: id name }
			fragment SellerFields on Seller { id name }
		`)
		readReq.Var("pid", "7")
		readReq.Var("withSeller", true)
		out, err := col.Read(readReq)
		c.Check(err == nil, "Read failed: %v", err)
		item, ok := out["item"].(map[string]interface{})
		c.Check(ok, "projected item missing or wrong type: %#v", out)
		c.Check(item["t"] == "Product", "typename alias projection: %v", item["t"])
		c.Check(item["k"] == "7", "id alias projection: %v", item["k"])
		c.Check(item["name"] == "Paper", "merged name missing: %v", item["name"])
		c.Check(item["price"] == float64(19), "merged price missing: %v", item["price"])
		_, ignoredPresent := item["ignored"]
		c.Check(!ignoredPresent, "skipped field key must be absent")
		seller, ok := item["seller"].(map[string]interface{})
		c.Check(ok, "seller projection missing")
		c.Check(seller["name"] == "New", "seller name should be updated, got %v", seller["name"])
		c.Check(seller["id"] == "s", "seller id missing")

		// Read never touches the network.
		before := tr.callCount()
		_, err = col.Read(readReq)
		c.Check(err == nil, "second Read failed: %v", err)
		c.Check(tr.callCount() == before, "Read issued a network request")

		// Uncollected fields are ErrIncomplete with a nil result, never null.
		missing := NewRequest(`{ product(id: "7") { __typename id weight } }`)
		res, err := col.Read(missing)
		c.Check(err != nil && errors.Is(err, ErrIncomplete), "expected ErrIncomplete, got %v", err)
		c.Check(res == nil, "incomplete result must be nil")

		// Directive flips the seller selection off; the rest still projects.
		off := NewRequest(`query($show:Boolean!){ p: product(id:"7"){ __typename id name seller @include(if:$show){__typename id name} } }`)
		off.Var("show", false)
		out2, err := col.Read(off)
		c.Check(err == nil, "Read with include=false failed: %v", err)
		p := out2["p"].(map[string]interface{})
		_, sellerPresent := p["seller"]
		c.Check(!sellerPresent, "included=false field must be absent")

		// Lists keep order, duplicates and null slots; two elements can share
		// one entity while projecting to independent maps.
		listReq := NewRequest(`{ shelf: products(ids: ["7", "7", "9", "9"]) { __typename id name price } }`)
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"shelf":[
			{"__typename":"Product","id":"7","name":"Paper","price":19},
			{"__typename":"Product","id":"7","name":"Paper","price":19},
			null,
			{"__typename":"Product","id":"9","name":"Tape","price":3}
		]}}`}
		mustCollect(c, client, listReq, col)
		listRead := NewRequest(`{ shelf: products(ids: ["7", "7", "9", "9"]) { __typename id name } }`)
		listOut, err := col.Read(listRead)
		c.Check(err == nil, "list Read failed: %v", err)
		shelf, ok := listOut["shelf"].([]interface{})
		c.Check(ok && len(shelf) == 4, "shelf shape wrong: %#v", listOut)
		s0, _ := shelf[0].(map[string]interface{})
		s1, _ := shelf[1].(map[string]interface{})
		c.Check(s0 != nil && s1 != nil && s0["name"] == "Paper" && s1["name"] == "Paper", "shared entity projected at both slots")
		c.Check(shelf[2] == nil, "null slot must be preserved")
		c.Check(shelf[3].(map[string]interface{})["name"] == "Tape", "distinct entity projected")

		// Projection outputs are independent of the store and of each other.
		s0["name"] = "mutated"
		again, err := col.Read(listRead)
		c.Check(err == nil, "re-Read failed: %v", err)
		shelf2 := again["shelf"].([]interface{})
		c.Check(shelf2[0].(map[string]interface{})["name"] == "Paper", "store leaked a projection mutation")
		c.Check(shelf2[1].(map[string]interface{})["name"] == "Paper", "sibling projection was mutated")

		// Embedded objects stay isolated even when their contents are equal,
		// and replacing one owning field does not touch the other. Different
		// original field names give them different field identities.
		embedReq := NewRequest(`{ a: noteA { text } b: noteB { text } }`)
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"a":{"text":"same"},"b":{"text":"same"}}}`}
		mustCollect(c, client, embedReq, col)
		embedOut, err := col.Read(embedReq)
		c.Check(err == nil, "embedded Read failed: %v", err)
		c.Check(embedOut["a"].(map[string]interface{})["text"] == "same", "embedded a")
		c.Check(embedOut["b"].(map[string]interface{})["text"] == "same", "embedded b")
		replace := NewRequest(`{ a: noteA { text } }`)
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"a":{"text":"changed"}}}`}
		mustCollect(c, client, replace, col)
		after, err := col.Read(embedReq)
		c.Check(err == nil, "embedded re-Read failed: %v", err)
		c.Check(after["a"].(map[string]interface{})["text"] == "changed", "embedded a should be replaced")
		c.Check(after["b"].(map[string]interface{})["text"] == "same", "embedded b must be independent")

		// Variables on the request are never rewritten by Read.
		varsBefore := fmt.Sprintf("%v", readReq.Vars())
		_, _ = col.Read(readReq)
		c.Check(fmt.Sprintf("%v", readReq.Vars()) == varsBefore, "req.Vars was modified")

		// A non-matching type condition excludes those fields; judging a
		// condition without an unconditioned __typename must fail.
		mismatch := NewRequest(`{ product(id:"7"){ __typename id ... on Other { name } } }`)
		mm, err := col.Read(mismatch)
		c.Check(err == nil, "mismatching type condition should simply exclude: %v", err)
		mmItem := mm["product"].(map[string]interface{})
		_, hasName := mmItem["name"]
		c.Check(!hasName, "mismatched fragment field leaked")
		noTn := NewRequest(`{ product(id:"7"){ id ... on Product { name } } }`)
		_, err = col.Read(noTn)
		c.Check(err != nil, "type condition without unconditioned __typename must fail")

		// A shared fragment reached from two positions must contribute its
		// fields at both; the second walk cannot be skipped as "visited".
		shared := NewRequest(`{ p: product(id:"7"){ __typename id ...Core ... on Product { ...Core } } } fragment Core on Product { name price }`)
		sharedOut, err := col.Read(shared)
		c.Check(err == nil, "shared fragment Read failed: %v", err)
		sp := sharedOut["p"].(map[string]interface{})
		c.Check(sp["name"] == "Paper" && sp["price"] == float64(19), "overlapping fragment selections must merge: %#v", sp)

		// A finite query may follow an entity ring; both occurrences of the
		// same entity project fully, with no global visited truncation.
		ringReq := NewRequest(`{ p: product(id:"7"){ __typename id name seller { __typename id name best { __typename id name } } } }`)
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"p":{"__typename":"Product","id":"7","name":"Paper","seller":{"__typename":"Seller","id":"s","name":"New","best":{"__typename":"Product","id":"7","name":"Paper"}}}}}`}
		mustCollect(c, client, ringReq, col)
		ringOut, err := col.Read(ringReq)
		c.Check(err == nil, "entity ring Read failed: %v", err)
		rp := ringOut["p"].(map[string]interface{})
		ringSeller := rp["seller"].(map[string]interface{})
		ringBest := ringSeller["best"].(map[string]interface{})
		c.Check(ringBest["name"] == "Paper", "second occurrence of the ring entity was truncated")
		c.Check(ringSeller["name"] == "New", "ring seller projection")

		// Document default values apply only when req.Var does not bind the
		// variable; an explicit binding always wins.
		defReq := NewRequest(`query($label: String = "fallback"){ d: flag(v: $label) }`)
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"d":"fallback"}}`}
		mustCollect(c, client, defReq, col)
		defOut, err := col.Read(defReq)
		c.Check(err == nil, "default-variable Read failed: %v", err)
		c.Check(defOut["d"] == "fallback", "document default value not used")
		over := NewRequest(`query($label: String = "fallback"){ d: flag(v: $label) }`)
		over.Var("label", "explicit")
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"d":"explicit"}}`}
		mustCollect(c, client, over, col)
		overOut, err := col.Read(over)
		c.Check(err == nil, "bound-variable Read failed: %v", err)
		c.Check(overOut["d"] == "explicit", "req.Var must override the document default")
	})
}

// TestCollectionIdentity covers field identity rules, entity keying, argument
// normalization and every parse/binding rejection that must happen before any
// request is sent.
func TestCollectionIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		tr := newScriptedTransport()
		client := collectClient(tr)
		col := NewCollection()

		// Same original field, same args in different key order: one identity.
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"p":{"__typename":"Product","id":"1","color":"red"}}}`}
		mustCollect(c, client, NewRequest(`{ p: product(filter: {a: 1, b: "x"}) { __typename id color } }`), col)
		read1, err := col.Read(NewRequest(`{ p: product(filter: {b: "x", a: 1}) { __typename id color } }`))
		c.Check(err == nil, "reordered input-object keys broke identity: %v", err)
		c.Check(read1["p"].(map[string]interface{})["color"] == "red", "identity merge failed")

		// Different argument values stay separate, regardless of response alias.
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"q":{"__typename":"Product","id":"2","color":"blue"}}}`}
		mustCollect(c, client, NewRequest(`{ q: product(filter: {a: 2, b: "x"}) { __typename id color } }`), col)
		readBoth, err := col.Read(NewRequest(`{ one: product(filter:{a:1,b:"x"}){__typename id color} two: product(filter:{a:2,b:"x"}){__typename id color} }`))
		c.Check(err == nil, "reading both identities failed: %v", err)
		c.Check(readBoth["one"].(map[string]interface{})["color"] == "red", "identity one")
		c.Check(readBoth["two"].(map[string]interface{})["color"] == "blue", "identity two")

		// Integer arguments normalize numerically; list order matters;
		// strings, booleans and null never share an identity.
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"n1":1,"n2":2,"s":"str","b":true,"z":null,"l":[1]}}`}
		mustCollect(c, client, NewRequest(`{ n1: flag(v: 1) n2: flag(v: 2) s: flag(v: "true") b: flag(v: true) z: flag(v: null) l: flag(v: [1, 2]) }`), col)
		norm, err := col.Read(NewRequest(`{ n1: flag(v: 1) s: flag(v: "true") b: flag(v: true) z: flag(v: null) l: flag(v: [1, 2]) }`))
		c.Check(err == nil, "normalized Read failed: %v", err)
		c.Check(norm["n1"] == float64(1) && norm["s"] == "str" && norm["b"] == true, "string/bool/number confused: %#v", norm)
		zVal, zThere := norm["z"]
		c.Check(zThere && zVal == nil, "explicit null must project as a present nil")
		swapped, err := col.Read(NewRequest(`{ l: flag(v: [2, 1]) }`))
		c.Check(err != nil && errors.Is(err, ErrIncomplete), "reordered list args must be a different identity: %v", err)
		c.Check(swapped == nil, "incomplete swapped read must be nil")

		// The entity key is an ordered pair, not a collision-prone string:
		// ("AB","C") and ("A","BC") must stay distinct.
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"one":{"__typename":"AB","id":"C","v":1},"two":{"__typename":"A","id":"BC","v":2}}}`}
		mustCollect(c, client, NewRequest(`{ one: thing(k: 1) { __typename id v } two: thing(k: 2) { __typename id v } }`), col)
		pair, err := col.Read(NewRequest(`{ one: thing(k: 1) { __typename id v } two: thing(k: 2) { __typename id v } }`))
		c.Check(err == nil, "ordered-pair Read failed: %v", err)
		c.Check(pair["one"].(map[string]interface{})["v"] == float64(1), "entity one of pair")
		c.Check(pair["two"].(map[string]interface{})["v"] == float64(2), "entity two of pair would have collided under string concatenation")

		// Objects without a non-empty id never merge, even with equal content.
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"x":{"__typename":"Tag","label":"a"},"y":{"__typename":"Tag","label":"a"}}}`}
		mustCollect(c, client, NewRequest(`{ x: tag(k: 1) { __typename label } y: tag(k: 2) { __typename label } }`), col)
		noID, err := col.Read(NewRequest(`{ x: tag(k: 1) { __typename label } y: tag(k: 2) { __typename label } }`))
		c.Check(err == nil, "embedded tag Read failed: %v", err)
		c.Check(noID["x"].(map[string]interface{})["label"] == "a", "tag x stored")
		c.Check(noID["y"].(map[string]interface{})["label"] == "a", "tag y stored independently")

		// Late conflict inside one response: same entity field, two values.
		// The whole response must be rejected and the old collection retained.
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"first":{"__typename":"Product","id":"1","color":"red"},"second":{"__typename":"Product","id":"1","color":"green"}}}`}
		err = client.Collect(context.Background(), NewRequest(`{ first: product(id:"1"){__typename id color} second: product(id:"1"){__typename id color} }`), col)
		c.Check(err != nil, "conflicting entity values must fail the collection, got nil")
		stable, err := col.Read(NewRequest(`{ p: product(filter:{a:1,b:"x"}){__typename id color} }`))
		c.Check(err == nil, "rollback Read failed: %v", err)
		c.Check(stable["p"].(map[string]interface{})["color"] == "red", "conflicting response must not publish partial data")

		// Rejections happen before any network activity.
		badQueries := []string{
			`mutation { doThing { id } }`,
			`query A { a { id } } query B { b { id } }`,
			`{ p { ...Missing } }`,
			`{ ...A } fragment A on X { ...B } fragment B on X { ...A }`,
			`query($v: String!) { p(id: $v) { id } }`,
			`{ p @skip(if: 1) { id } }`,
			`{ p( { id } }`,
			`{ p(v: 1.5) { id } }`,
			`{ p(v: """block""") { id } }`,
		}
		sentBefore := tr.callCount()
		for _, q := range badQueries {
			req := NewRequest(q)
			err := client.Collect(context.Background(), req, NewCollection())
			c.Check(err != nil, "bad query was accepted: %q", q)
			_, rerr := NewCollection().Read(req)
			c.Check(rerr != nil, "bad query passed Read: %q", q)
		}
		c.Check(tr.callCount() == sentBefore, "rejected query triggered a request: %d vs %d", tr.callCount(), sentBefore)

		// File uploads are refused without touching the network.
		fileReq := NewRequest(`{ p { id } }`)
		fileReq.File("upload", "f.txt", strings.NewReader("data"))
		err = client.Collect(context.Background(), fileReq, NewCollection())
		c.Check(err != nil, "file upload must be rejected by Collect")
		c.Check(tr.callCount() == sentBefore, "rejected file upload triggered a request")
	})
}

// TestCollectionCommit covers transactional commits, field-level merging of
// concurrent in-flight collections, later-commit-wins overlap, concurrent
// reads always observing a complete commit, and failure rollback.
func TestCollectionCommit(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		tr := newScriptedTransport()
		client := collectClient(tr)
		col := NewCollection()

		// Seed an initial entity so rollback behaviour is observable.
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"p":{"__typename":"Product","id":"42","name":"Seed"}}}`}
		mustCollect(c, client, NewRequest(`{ p: product(id: "42") { __typename id name } }`), col)

		// Two collections in flight concurrently, gated inside the round
		// trips; the router hands each query its deterministic body.
		gate := make(chan struct{})
		gtr := &scriptedTransport{
			arrived: make(chan recordedRequest, 2),
			gate:    gate,
			route: func(q string) *scriptedResponse {
				if strings.Contains(q, "price") {
					return &scriptedResponse{status: 200, body: `{"data":{"p":{"__typename":"Product","id":"42","name":"Seed","price":19}}}`}
				}
				return &scriptedResponse{status: 200, body: `{"data":{"p":{"__typename":"Product","id":"42","color":"green","inStock":true}}}`}
			},
		}
		gclient := collectClient(gtr)
		reqA := NewRequest(`{ p: product(id: "42") { __typename id name price } }`)
		reqB := NewRequest(`{ p: product(id: "42") { __typename id color inStock } }`)
		errs := make(chan error, 2)
		start := make(chan struct{})
		go func() { <-start; errs <- gclient.Collect(context.Background(), reqA, col) }()
		go func() { <-start; errs <- gclient.Collect(context.Background(), reqB, col) }()
		close(start)
		<-gtr.arrived
		<-gtr.arrived
		c.Check(gtr.callCount() == 2, "expected both in-flight requests to arrive, got %d", gtr.callCount())

		// While both are pending, concurrent reads must keep seeing the
		// complete seed commit rather than a partially merged state.
		readDone := make(chan struct{})
		go func() {
			for i := 0; i < 20; i++ {
				out, err := col.Read(NewRequest(`{ p: product(id: "42") { __typename id name } }`))
				if err != nil || out["p"].(map[string]interface{})["name"] != "Seed" {
					c.Check(false, "concurrent read observed an incomplete commit: %v %#v", err, out)
					break
				}
			}
			close(readDone)
		}()

		close(gate)
		c.Check(<-errs == nil, "first concurrent Collect failed")
		c.Check(<-errs == nil, "second concurrent Collect failed")
		<-readDone

		// Field-level merge: the union of both snapshots plus the seed field.
		merged, err := col.Read(NewRequest(`{ p: product(id: "42") { __typename id name price color inStock } }`))
		c.Check(err == nil, "merged read failed: %v", err)
		item := merged["p"].(map[string]interface{})
		c.Check(item["name"] == "Seed", "seed/overlap field survived: %v", item["name"])
		c.Check(item["price"] == float64(19), "disjoint field price survived")
		c.Check(item["color"] == "green", "disjoint field color survived")
		c.Check(item["inStock"] == true, "disjoint field inStock survived")

		// Later committer wins on an overlapping field, either release order.
		overlap := func(firstBody, secondBody string) string {
			tr2 := newScriptedTransport()
			cl2 := collectClient(tr2)
			co2 := NewCollection()
			tr2.respond <- &scriptedResponse{status: 200, body: firstBody}
			mustCollect(c, cl2, NewRequest(`{ p: product(id:"9"){__typename id name} }`), co2)
			tr2.respond <- &scriptedResponse{status: 200, body: secondBody}
			mustCollect(c, cl2, NewRequest(`{ q: product(id:"9"){__typename id name} }`), co2)
			out, err := co2.Read(NewRequest(`{ p: product(id:"9"){__typename id name} }`))
			c.Check(err == nil, "overlap read failed: %v", err)
			return out["p"].(map[string]interface{})["name"].(string)
		}
		c.Check(overlap(
			`{"data":{"p":{"__typename":"Product","id":"9","name":"first"}}}`,
			`{"data":{"q":{"__typename":"Product","id":"9","name":"second"}}}`,
		) == "second", "later commit should overwrite overlap")
		c.Check(overlap(
			`{"data":{"p":{"__typename":"Product","id":"9","name":"second"}}}`,
			`{"data":{"q":{"__typename":"Product","id":"9","name":"first"}}}`,
		) == "first", "overlap winner must track commit order")

		// Explicit null overwrites an old scalar; list fields are replaced
		// wholesale including order and null slots.
		tr.respond <- &scriptedResponse{status: 200, body: `{"data":{"p":{"__typename":"Product","id":"42","name":null,"tags":[null,{"__typename":"Tag","label":"b"}]}}}`}
		mustCollect(c, client, NewRequest(`{ p: product(id: "42") { __typename id name tags { __typename label } } }`), col)
		nulled, err := col.Read(NewRequest(`{ p: product(id: "42") { __typename id name tags { __typename label } } }`))
		c.Check(err == nil, "null/list read failed: %v", err)
		pn := nulled["p"].(map[string]interface{})
		nameVal, nameThere := pn["name"]
		c.Check(nameThere && nameVal == nil, "explicit null must overwrite the old name")
		tags := pn["tags"].([]interface{})
		c.Check(len(tags) == 2 && tags[0] == nil && tags[1].(map[string]interface{})["label"] == "b", "list replaced wholesale: %#v", tags)

		// Failures never publish: GraphQL error, non-200 status, read error,
		// pre-cancelled context. Every response body still gets closed.
		_, baselineClosed := tr.snapshot()
		tr.respond <- &scriptedResponse{status: 200, body: `{"errors":[{"message":"denied"}]}`}
		err = client.Collect(context.Background(), NewRequest(`{ p: product(id:"42"){__typename id name} }`), col)
		c.Check(err != nil && err.Error() == "graphql: denied", "graphql error handling: %v", err)

		tr.respond <- &scriptedResponse{status: 500, body: `boom`}
		err = client.Collect(context.Background(), NewRequest(`{ p: product(id:"42"){__typename id name} }`), col)
		c.Check(err != nil && strings.Contains(err.Error(), "non-200"), "non-200 handling: %v", err)

		sentinel := errors.New("body read sentinel")
		failClient := collectClient(transport(func(r *http.Request) (*http.Response, error) {
			return &http.Response{StatusCode: 200, Body: &failingBody{err: sentinel}, Request: r}, nil
		}))
		err = failClient.Collect(context.Background(), NewRequest(`{ p: product(id:"42"){__typename id name} }`), col)
		c.Check(errors.Is(err, sentinel), "read cause lost: %v", err)

		canceled, cancel := context.WithCancel(context.Background())
		cancel()
		err = client.Collect(canceled, NewRequest(`{ p: product(id:"42"){__typename id name} }`), col)
		c.Check(errors.Is(err, context.Canceled), "cancel cause lost: %v", err)

		// The last good commit remains readable after every failure.
		survivor, err := col.Read(NewRequest(`{ p: product(id: "42") { __typename id tags { __typename label } } }`))
		c.Check(err == nil, "survivor read failed: %v", err)
		survTags := survivor["p"].(map[string]interface{})["tags"].([]interface{})
		c.Check(len(survTags) == 2 && survTags[0] == nil, "rollback lost the last good list")

		_, closedNow := tr.snapshot()
		c.Check(closedNow > baselineClosed, "response bodies were not closed: %d vs %d", closedNow, baselineClosed)
	})
}

type transport func(*http.Request) (*http.Response, error)

func (f transport) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

type failingBody struct{ err error }

func (b *failingBody) Read([]byte) (int, error) { return 0, b.err }
func (b *failingBody) Close() error             { return nil }
