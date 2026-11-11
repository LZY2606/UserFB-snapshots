package regress_test

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"sync"
	"testing"

	g "github.com/shurcooL/graphql"
)

// boundary is the MIME boundary used by every scripted server below.
const incBoundary = "parts"

// boundaryLine opens the next part (and closes the previous one).
func boundaryLine() string { return "--" + incBoundary + "\r\n" }

// partHead is the header block that starts each part's content.
func partHead() string { return "Content-Type: application/json\r\n\r\n" }

// partDoc wraps a JSON document as one part's content, without the leading
// boundary line (scripts write that line separately so it can be flushed
// before blocking on a callback gate).
func partDoc(doc string) string { return partHead() + doc + "\r\n" }

// mimePart is one complete part: its opening boundary line plus its content.
func mimePart(doc string) string { return boundaryLine() + partDoc(doc) }

// mimeEnd is the closing MIME boundary.
func mimeEnd() string { return "--" + incBoundary + "--\r\n" }

// splitAt cuts s at the given byte offsets, so callers can force writes to
// land in the middle of JSON tokens or MIME boundary lines.
func splitAt(s string, cuts ...int) []string {
	var out []string
	start := 0
	for _, c := range cuts {
		out = append(out, s[start:c])
		start = c
	}
	return append(out, s[start:])
}

// writeChunks writes and flushes every chunk separately.
func writeChunks(t *testing.T, w http.ResponseWriter, chunks ...string) {
	t.Helper()
	fl, _ := w.(http.Flusher)
	for _, ch := range chunks {
		if _, err := io.WriteString(w, ch); err != nil {
			t.Logf("server write: %v", err)
			return
		}
		if fl != nil {
			fl.Flush()
		}
	}
}

// waitGate blocks until the client callback signals, the request dies,
// or the test ends.
func waitGate(r *http.Request, gate <-chan struct{}) bool {
	select {
	case <-gate:
		return true
	case <-r.Context().Done():
		return false
	}
}

// closeTracker marks the moment the client closes a response body.
type closeTracker struct {
	io.ReadCloser
	t      *trackingTransport
	closed bool
}

func (c *closeTracker) Close() error {
	if !c.closed {
		c.closed = true
		c.t.mu.Lock()
		c.t.closes++
		c.t.mu.Unlock()
	}
	return c.ReadCloser.Close()
}

// trackingTransport counts requests and response-body closes.
type trackingTransport struct {
	base   http.RoundTripper
	mu     sync.Mutex
	hits   int
	bodies int
	closes int
}

func (t *trackingTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	t.mu.Lock()
	t.hits++
	t.mu.Unlock()
	resp, err := t.base.RoundTrip(req)
	if err != nil {
		return resp, err
	}
	t.mu.Lock()
	t.bodies++
	t.mu.Unlock()
	resp.Body = &closeTracker{ReadCloser: resp.Body, t: t}
	return resp, nil
}

func (t *trackingTransport) check(t2 *testing.T, wantRequests int) {
	t2.Helper()
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.hits != wantRequests {
		t2.Errorf("HTTP requests = %d, want %d", t.hits, wantRequests)
	}
	if t.bodies != wantRequests {
		t2.Errorf("response bodies = %d, want %d", t.bodies, wantRequests)
	}
	if t.closes != wantRequests {
		t2.Errorf("closed response bodies = %d, want %d", t.closes, wantRequests)
	}
}

func trackingClient(srv *httptest.Server) (*http.Client, *trackingTransport) {
	tr := &trackingTransport{base: srv.Client().Transport}
	return &http.Client{Transport: tr}, tr
}

// deliveryQuery is the query shape used by TestIncrementalDelivery.
type deliveryQuery struct {
	Catalog struct {
		Card struct {
			DisplayName g.String `graphql:"displayName"`
			Title       g.String `graphql:"title"`
		} `graphql:"card"`
		Entries []struct {
			ID   g.ID     `graphql:"id"`
			Name g.String `graphql:"name"`
		} `graphql:"entries"`
	} `graphql:"catalog"`
}

// TestIncrementalDelivery drives a controllable streaming server: chunks may
// split JSON tokens and MIME boundaries, the server blocks between parts until
// the callback signals, and every case checks request count, callback-visible
// values and response-body closing.
func TestIncrementalDelivery(t *testing.T) {
	t.Run("application/json single callback", func(t *testing.T) {
		var srvHits int
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			srvHits++
			if r.Method != http.MethodPost {
				t.Errorf("method = %q, want POST", r.Method)
			}
			body, _ := io.ReadAll(r.Body)
			if !strings.Contains(string(body), `"query"`) || !strings.Contains(string(body), "displayName") {
				t.Errorf("request body was not the query derived from q: %s", body)
			}
			w.Header().Set("Content-Type", "application/json")
			fmt.Fprint(w, `{"data":{"catalog":{"card":{"displayName":"Ada"},"entries":[]}},`+
				`"errors":[{"message":"initial-warn","path":["catalog"],"locations":[{"line":1,"column":2}]}]}`)
		}))
		defer srv.Close()
		client, tr := trackingClient(srv)

		var q deliveryQuery
		var calls int
		err := g.NewClient(srv.URL, client).QueryIncremental(context.Background(), &q, nil, func(pr g.IncrementalResult) error {
			calls++
			if pr.HasNext {
				t.Errorf("plain JSON callback HasNext = true, want false")
			}
			if q.Catalog.Card.DisplayName != "Ada" {
				t.Errorf("inside callback q.Catalog.Card.DisplayName = %q, want Ada", q.Catalog.Card.DisplayName)
			}
			if len(pr.Errors) != 1 || pr.Errors[0].Message != "initial-warn" {
				t.Errorf("callback errors = %+v", pr.Errors)
			}
			if loc := pr.Errors[0].Locations; len(loc) != 1 || loc[0].Line != 1 || loc[0].Column != 2 {
				t.Errorf("callback locations = %+v", loc)
			}
			return nil
		})
		if calls != 1 {
			t.Errorf("callbacks = %d, want 1", calls)
		}
		if err == nil || !strings.Contains(err.Error(), "initial-warn") {
			t.Errorf("business error = %v, want it to contain initial-warn", err)
		}
		if q.Catalog.Card.DisplayName != "Ada" {
			t.Errorf("q.Catalog.Card.DisplayName = %q, want Ada", q.Catalog.Card.DisplayName)
		}
		tr.check(t, 1)
		if srvHits != 1 {
			t.Errorf("server hits = %d, want 1", srvHits)
		}
	})

	t.Run("multipart streaming with gates, merges, appends and errors", func(t *testing.T) {
		gate1, gate2 := make(chan struct{}), make(chan struct{})
		var srvHits int
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			srvHits++
			w.Header().Set("Content-Type", `multipart/mixed; boundary="parts"; deferSpec=20220824`)

			// Part 1, cut in the middle of JSON tokens and of its boundary line.
			first := mimePart(`{"data":{"catalog":{"card":{"displayName":"Ada"},"entries":[{"id":"e0","name":"Zero"}]}},"errors":[{"message":"initial-warn","path":["catalog"]}],"hasNext":true}`)
			writeChunks(t, w, splitAt(first, 7, 40, 91)...)
			// Close part 1 before blocking: this boundary line is what lets the
			// client finish parsing and invoke its callback.
			writeChunks(t, w, boundaryLine())
			if !waitGate(r, gate1) {
				return
			}

			// Part 2: merge data, append one item, report an item error whose
			// path is relative to the item path.
			second := partDoc(`{"incremental":[` +
				`{"data":{"title":"T2","displayName":"Ada2"},"path":["catalog","card"]},` +
				`{"items":[{"id":"e1","name":"One"}],"path":["catalog","entries",1],` +
				`"errors":[{"message":"partial-fail","path":["name"],"locations":[{"line":3,"column":4}]}]}` +
				`],"hasNext":true}`)
			writeChunks(t, w, splitAt(second, 3, 55, 130)...)
			writeChunks(t, w, boundaryLine())
			if !waitGate(r, gate2) {
				return
			}

			writeChunks(t, w, partDoc(`{"hasNext":false}`))
			writeChunks(t, w, mimeEnd())
		}))
		defer srv.Close()
		client, tr := trackingClient(srv)

		var q deliveryQuery
		var calls int
		var once1, once2 sync.Once
		err := g.NewClient(srv.URL, client).QueryIncremental(context.Background(), &q, nil, func(pr g.IncrementalResult) error {
			calls++
			switch calls {
			case 1:
				if !pr.HasNext {
					t.Error("part 1 HasNext = false, want true")
				}
				if q.Catalog.Card.DisplayName != "Ada" || len(q.Catalog.Entries) != 1 || q.Catalog.Entries[0].ID != "e0" {
					t.Errorf("after part 1 q = %+v", q)
				}
				if len(pr.Errors) != 1 || pr.Errors[0].Message != "initial-warn" {
					t.Errorf("part 1 errors = %+v", pr.Errors)
				}
				once1.Do(func() { close(gate1) }) // Let the server continue only now.
			case 2:
				if !pr.HasNext {
					t.Error("part 2 HasNext = false, want true")
				}
				if q.Catalog.Card.DisplayName != "Ada2" || q.Catalog.Card.Title != "T2" {
					t.Errorf("merge not visible in callback: %+v", q.Catalog.Card)
				}
				if len(q.Catalog.Entries) != 2 || q.Catalog.Entries[1].ID != "e1" || q.Catalog.Entries[1].Name != "One" {
					t.Errorf("append not visible in callback: %+v", q.Catalog.Entries)
				}
				if len(pr.Errors) != 1 {
					t.Fatalf("part 2 errors = %+v", pr.Errors)
				}
				wantPath := []any{"catalog", "entries", 1, "name"}
				if !reflect.DeepEqual(pr.Errors[0].Path, wantPath) {
					t.Errorf("absolute error path = %v, want %v", pr.Errors[0].Path, wantPath)
				}
				if loc := pr.Errors[0].Locations; len(loc) != 1 || loc[0].Line != 3 || loc[0].Column != 4 {
					t.Errorf("item error locations = %+v", loc)
				}
				once2.Do(func() { close(gate2) })
			case 3:
				if pr.HasNext {
					t.Error("final part HasNext = true, want false")
				}
				if len(pr.Errors) != 0 {
					t.Errorf("final part errors = %+v", pr.Errors)
				}
			default:
				t.Errorf("unexpected callback %d", calls)
			}
			return nil
		})
		if calls != 3 {
			t.Errorf("callbacks = %d, want 3", calls)
		}
		if err == nil || !strings.Contains(err.Error(), "initial-warn") || !strings.Contains(err.Error(), "partial-fail") {
			t.Errorf("returned error = %v, want both business messages", err)
		}
		if q.Catalog.Card.Title != "T2" || len(q.Catalog.Entries) != 2 {
			t.Errorf("final q = %+v", q)
		}
		tr.check(t, 1)
		if srvHits != 1 {
			t.Errorf("server hits = %d, want 1", srvHits)
		}
	})

	t.Run("first part may already be final", func(t *testing.T) {
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Content-Type", "multipart/mixed; boundary=parts")
			writeChunks(t, w, splitAt(mimePart(`{"data":{"catalog":{"card":{"displayName":"Solo"}}},"hasNext":false}`), 4, 33)...)
			writeChunks(t, w, mimeEnd())
		}))
		defer srv.Close()
		client, tr := trackingClient(srv)

		var q deliveryQuery
		var calls int
		err := g.NewClient(srv.URL, client).QueryIncremental(context.Background(), &q, nil, func(pr g.IncrementalResult) error {
			calls++
			if pr.HasNext {
				t.Error("single-part stream reported HasNext=true")
			}
			return nil
		})
		if err != nil {
			t.Fatalf("QueryIncremental: %v", err)
		}
		if calls != 1 || q.Catalog.Card.DisplayName != "Solo" {
			t.Errorf("calls=%d q=%+v", calls, q)
		}
		tr.check(t, 1)
	})
}

// Types for TestIncrementalIdentity: aliases, twin fragments, pointer fields
// and pointer slice elements.
type incChild struct {
	Kind g.String `graphql:"kind"`
}

type incEntry struct {
	ID    g.ID      `graphql:"id"`
	Name  g.String  `graphql:"name"`
	Child *incChild `graphql:"child"`
}

type incFragment struct {
	Kind g.String `graphql:"kind"`
	Note g.String `graphql:"note"`
}

type identityQuery struct {
	Catalog struct {
		Card struct {
			DisplayName g.String `graphql:"displayName"`
			Details     *struct {
				Version g.Int `graphql:"version"`
			} `graphql:"details"`
			FragA incFragment `graphql:"... on Card"`
			FragB incFragment `graphql:"... on Card"`
		} `graphql:"itemCard: card"`
		Entries []*incEntry `graphql:"entryList: entries"`
	} `graphql:"catalog"`
}

// TestIncrementalIdentity verifies graphql-name (alias) path resolution,
// fan-out into every fragment mapping the same field, and the pointer
// stability guarantees for pointer fields and pointer slice elements.
func TestIncrementalIdentity(t *testing.T) {
	gate := make(chan struct{})
	var gateOnce sync.Once
	release := func() { gateOnce.Do(func() { close(gate) }) }

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "multipart/mixed; boundary=parts")
		// Part 1: the response keys are the aliases itemCard/entryList, plus
		// twin fragments, one pointer element, a nil child pointer and a
		// populated details pointer.
		writeChunks(t, w, mimePart(`{"data":{"catalog":{`+
			`"itemCard":{"displayName":"Ada","details":{"version":1},"kind":"base","note":"n0"},`+
			`"entryList":[{"id":"e0","name":"Zero","child":null}]`+
			`}},"hasNext":true}`))
		writeChunks(t, w, boundaryLine())
		if !waitGate(r, gate) {
			return
		}
		// Part 2, one segment with several items: append an entry, patch the
		// child of the just-appended entry, allocate the nil child pointer of
		// entry 0, and mutate entry 0 in place. Paths use alias names.
		writeChunks(t, w, partDoc(`{"incremental":[`+
			`{"items":[{"id":"e1","name":"One","child":{"kind":"c1"}}],"path":["catalog","entryList",1]},`+
			`{"data":{"kind":"c2"},"path":["catalog","entryList",1,"child"]},`+
			`{"data":{"child":{"kind":"k0"}},"path":["catalog","entryList",0]},`+
			`{"data":{"name":"ZeroUp"},"path":["catalog","entryList",0]}`+
			`],"hasNext":true}`))
		writeChunks(t, w, boundaryLine())
		if !waitGate(r, gate) {
			return
		}
		// Part 3: null clears the details pointer, kind fans out into both
		// fragments, unmentioned note/displayName stay, and the entry 0 child
		// pointer is merged in place rather than replaced.
		writeChunks(t, w, partDoc(`{"incremental":[`+
			`{"data":{"details":null,"kind":"base2"},"path":["catalog","itemCard"]},`+
			`{"data":{"kind":"k1"},"path":["catalog","entryList",0,"child"]}`+
			`],"hasNext":true}`))
		writeChunks(t, w, boundaryLine())
		if !waitGate(r, gate) {
			return
		}
		// Part 4: the pointer field is re-allocated with fresh data.
		writeChunks(t, w, partDoc(`{"incremental":[`+
			`{"data":{"details":{"version":2}},"path":["catalog","itemCard"]}`+
			`],"hasNext":false}`))
		writeChunks(t, w, mimeEnd())
	}))
	defer srv.Close()
	client, tr := trackingClient(srv)

	var q identityQuery
	var calls int
	var savedEntry0 *incEntry
	var childV2 *incChild
	var detailsV1 any
	err := g.NewClient(srv.URL, client).QueryIncremental(context.Background(), &q, nil, func(pr g.IncrementalResult) error {
		calls++
		if calls <= 3 && !pr.HasNext {
			t.Errorf("part %d HasNext=false, want true", calls)
		}
		switch calls {
		case 1:
			savedEntry0 = q.Catalog.Entries[0]
			detailsV1 = q.Catalog.Card.Details
			release()
		case 2:
			if q.Catalog.Entries[1].Child.Kind != "c2" {
				t.Errorf("same-segment parent patch missing: %+v", q.Catalog.Entries[1].Child)
			}
			if q.Catalog.Entries[0].Child == nil || q.Catalog.Entries[0].Child.Kind != "k0" {
				t.Errorf("entry 0 child not allocated in the same segment: %+v", q.Catalog.Entries[0].Child)
			}
			childV2 = q.Catalog.Entries[0].Child
		case 3:
			// The non-nil child pointer must be merged in place, not replaced.
			if q.Catalog.Entries[0].Child != childV2 || q.Catalog.Entries[0].Child.Kind != "k1" {
				t.Errorf("in-place pointer merge failed: %+v", q.Catalog.Entries[0].Child)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatalf("QueryIncremental: %v", err)
	}
	if calls != 4 {
		t.Fatalf("callbacks = %d, want 4", calls)
	}

	// The pointer element delivered first is never rebuilt, and it was
	// patched in place.
	if q.Catalog.Entries[0] != savedEntry0 {
		t.Error("pointer slice element was rebuilt instead of updated in place")
	}
	if q.Catalog.Entries[0].Child != childV2 || q.Catalog.Entries[0].Child.Kind != "k1" {
		t.Errorf("child pointer was replaced instead of merged: %+v", q.Catalog.Entries[0].Child)
	}
	e0 := q.Catalog.Entries[0]
	if e0.Name != "ZeroUp" || e0.ID != "e0" {
		t.Errorf("entry 0 = %+v, want in-place update", e0)
	}
	if len(q.Catalog.Entries) != 2 {
		t.Fatalf("entries len = %d, want 2", len(q.Catalog.Entries))
	}
	if q.Catalog.Entries[1].ID != "e1" || q.Catalog.Entries[1].Child.Kind != "c2" {
		t.Errorf("entry 1 = %+v", q.Catalog.Entries[1])
	}
	// The twin fragments both reflect the same response field, and the
	// unmentioned note was retained.
	if q.Catalog.Card.FragA.Kind != "base2" || q.Catalog.Card.FragB.Kind != "base2" {
		t.Errorf("fragment fan-out failed: %+v", q.Catalog.Card)
	}
	if q.Catalog.Card.FragA.Note != "n0" || q.Catalog.Card.FragB.Note != "n0" {
		t.Errorf("unmentioned fragment note was lost: %+v", q.Catalog.Card)
	}
	if q.Catalog.Card.DisplayName != "Ada" {
		t.Errorf("displayName lost: %q", q.Catalog.Card.DisplayName)
	}
	// The null-cleared pointer was replaced only by the later allocation.
	if q.Catalog.Card.Details == nil || q.Catalog.Card.Details.Version != 2 {
		t.Errorf("details = %+v, want re-allocated version 2", q.Catalog.Card.Details)
	}
	if q.Catalog.Card.Details == detailsV1 {
		t.Error("null-cleared pointer was reused instead of replaced")
	}
	tr.check(t, 1)
}

// failureQuery is the shape used by every TestIncrementalFailure case.
type failureQuery struct {
	Catalog struct {
		Card struct {
			Name  g.String `graphql:"name"`
			Extra *struct {
				X g.String `graphql:"x"`
			} `graphql:"extra"`
		} `graphql:"card"`
		Entries []struct {
			ID g.ID `graphql:"id"`
		} `graphql:"entries"`
	} `graphql:"catalog"`
}

const failureFirstPart = `{"data":{"catalog":{"card":{"name":"keep","extra":null},"entries":[{"id":"e0"}]}},"hasNext":true}`

// TestIncrementalFailure covers bad paths, framing and truncation errors,
// atomic segment rollback, callback/context errors and body closing.
func TestIncrementalFailure(t *testing.T) {
	type tc struct {
		name      string
		header    string
		script    func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{})
		callback  func(t *testing.T, gate chan struct{}) func(g.IncrementalResult) error
		wantCalls int
		wantErr   string
		noFirst   bool // No part is delivered before the error.
	}

	cases := []tc{
		{
			name:   "hasNext=true then early EOF",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				// Part 1 is fully framed and flushed, then the connection
				// drops with no further part and no closing boundary.
				writeChunks(t, w, splitAt(mimePart(failureFirstPart), 5, 20)...)
				writeChunks(t, w, boundaryLine())
				waitGate(r, gate)
			},
			wantCalls: 1,
			wantErr:   "truncated",
		},
		{
			name:   "hasNext=false part without closing boundary",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				// Final document present, but no closing boundary: truncated.
				writeChunks(t, w, splitAt(partDoc(`{"hasNext":false}`), 2, 10)...)
			},
			wantCalls: 1,
			wantErr:   "truncated",
		},
		{
			name:   "non-JSON part",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				writeChunks(t, w, partDoc(`{not-json`))
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 1,
			wantErr:   "not valid JSON",
		},
		{
			name:   "invalid integer index format",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				writeChunks(t, w, partDoc(`{"incremental":[{"items":[],"path":["catalog","entries",1.5]}],"hasNext":false}`))
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 1,
			wantErr:   "invalid array index",
		},
		{
			name:   "another part after completion",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				writeChunks(t, w, partDoc(`{"hasNext":false}`))
				writeChunks(t, w, boundaryLine())
				writeChunks(t, w, splitAt(partDoc(`{"hasNext":false}`), 4)...)
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 2,
			wantErr:   "after the final part",
		},
		{
			name:   "bad second item rolls back the whole segment",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				writeChunks(t, w, partDoc(`{"incremental":[`+
					`{"data":{"name":"changed"},"path":["catalog","card"]},`+
					`{"data":{"x":1},"path":["catalog","nope"]}`+
					`],"hasNext":false}`))
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 1,
			wantErr:   "does not exist",
		},
		{
			name:   "successful append inside a failed segment is rolled back",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				// The append succeeds; the following bad path must undo it,
				// leaving the list at its previous length.
				writeChunks(t, w, partDoc(`{"incremental":[`+
					`{"items":[{"id":"ghost"}],"path":["catalog","entries",1]},`+
					`{"data":{"x":1},"path":["catalog","nope"]}`+
					`],"hasNext":false}`))
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 1,
			wantErr:   "does not exist",
		},
		{
			name:   "items cannot fill a hole",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				writeChunks(t, w, partDoc(`{"incremental":[{"items":[{"id":"hole"}],"path":["catalog","entries",3]}],"hasNext":false}`))
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 1,
			wantErr:   "list length",
		},
		{
			name:   "items cannot overwrite an existing element",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				writeChunks(t, w, partDoc(`{"incremental":[{"items":[{"id":"over"}],"path":["catalog","entries",0]}],"hasNext":false}`))
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 1,
			wantErr:   "list length",
		},
		{
			name:   "path cannot cross a null pointer or invent parents",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				writeChunks(t, w, partDoc(`{"incremental":[{"data":{"x":"y"},"path":["catalog","card","extra","x"]}],"hasNext":false}`))
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 1,
			wantErr:   "null",
		},
		{
			name:   "paths resolve by graphql name, never by Go field name",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				// "Card"/"Entries" are Go field names; the response keys are
				// lowercase, so these paths must not resolve.
				writeChunks(t, w, partDoc(`{"incremental":[{"data":{"name":"x"},"path":["catalog","Card"]}],"hasNext":false}`))
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 1,
			wantErr:   "does not exist",
		},
		{
			name:   "negative index is rejected",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				<-gate
				writeChunks(t, w, partDoc(`{"incremental":[{"items":[],"path":["catalog","entries",-1]}],"hasNext":false}`))
				writeChunks(t, w, mimeEnd())
			},
			wantCalls: 1,
			wantErr:   "invalid array index",
		},
		{
			name:   "callback error keeps errors.Is identity",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				if !waitGate(r, gate) {
					return
				}
				writeChunks(t, w, partDoc(`{"incremental":[{"data":{"name":"x"},"path":["catalog","card"]}],"hasNext":false}`))
				writeChunks(t, w, mimeEnd())
			},
			callback: func(t *testing.T, gate chan struct{}) func(g.IncrementalResult) error {
				var once sync.Once
				return func(pr g.IncrementalResult) error {
					once.Do(func() { close(gate) })
					return errSentinel
				}
			},
			wantCalls: 1,
			wantErr:   "sentinel",
		},
		{
			name:   "context cancellation stops the stream",
			header: "multipart/mixed; boundary=parts",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				writeChunks(t, w, mimePart(failureFirstPart))
				writeChunks(t, w, boundaryLine())
				waitGate(r, gate)
			},
			callback: func(t *testing.T, gate chan struct{}) func(g.IncrementalResult) error {
				return func(pr g.IncrementalResult) error {
					close(gate)
					return nil
				}
			},
			wantCalls: 1,
			wantErr:   "context canceled",
		},
		{
			name:   "non-200 status",
			header: "application/json",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				w.WriteHeader(http.StatusInternalServerError)
				io.WriteString(w, `{"errors":[{"message":"boom"}]}`)
			},
			wantCalls: 0,
			wantErr:   "non-200",
			noFirst:   true,
		},
		{
			name:   "unsupported content type",
			header: "text/plain",
			script: func(t *testing.T, w http.ResponseWriter, r *http.Request, gate chan struct{}) {
				io.WriteString(w, "hello")
			},
			wantCalls: 0,
			wantErr:   "unsupported response Content-Type",
			noFirst:   true,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			gate := make(chan struct{})
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", tc.header)
				tc.script(t, w, r, gate)
			}))
			defer srv.Close()
			client, tr := trackingClient(srv)

			var q failureQuery
			var calls int
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			var custom func(g.IncrementalResult) error
			if tc.callback != nil {
				custom = tc.callback(t, gate)
			}
			onPart := func(pr g.IncrementalResult) error {
				calls++
				if tc.name == "context cancellation stops the stream" {
					cancel()
				}
				if custom != nil {
					return custom(pr)
				}
				// Default: unblock the server once the first part is visible.
				if calls == 1 {
					select {
					case <-gate:
					default:
						close(gate)
					}
				}
				return nil
			}
			err := g.NewClient(srv.URL, client).QueryIncremental(ctx, &q, nil, onPart)
			if err == nil || !strings.Contains(err.Error(), tc.wantErr) {
				t.Fatalf("error = %v, want substring %q", err, tc.wantErr)
			}
			if tc.name == "callback error keeps errors.Is identity" && !errors.Is(err, errSentinel) {
				t.Errorf("errors.Is(err, errSentinel) = false for %v", err)
			}
			if tc.name == "context cancellation stops the stream" && !errors.Is(err, context.Canceled) {
				t.Errorf("errors.Is(err, context.Canceled) = false for %v", err)
			}
			if calls != tc.wantCalls {
				t.Errorf("callbacks = %d, want %d", calls, tc.wantCalls)
			}
			// Data from completed parts is always retained, including after
			// protocol failures.
			if !tc.noFirst {
				if q.Catalog.Card.Name != "keep" {
					t.Errorf("previously delivered card name = %q, want keep", q.Catalog.Card.Name)
				}
				if len(q.Catalog.Entries) != 1 || q.Catalog.Entries[0].ID != "e0" {
					t.Errorf("previously delivered entries = %+v", q.Catalog.Entries)
				}
			}
			if tc.name == "bad second item rolls back the whole segment" && q.Catalog.Card.Name == "changed" {
				t.Error("half-segment data was left behind after rollback")
			}
			if tc.name == "successful append inside a failed segment is rolled back" && len(q.Catalog.Entries) != 1 {
				t.Errorf("rolled-back append left %d entries, want 1", len(q.Catalog.Entries))
			}
			if tc.name == "items cannot fill a hole" && len(q.Catalog.Entries) != 1 {
				t.Error("hole-filling append mutated the list")
			}
			tr.check(t, 1)
		})
	}
}

var errSentinel = errors.New("sentinel callback error")
