package jsonapi

import (
	"errors"
	"io"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/google/jsonapi/task_support/taskcheck"
)

// --- Graph test models -----------------------------------------------------

type GraphArticle struct {
	ID          string         `jsonapi:"primary,articles"`
	Title       string         `jsonapi:"attr,title"`
	Notes       []string       `jsonapi:"attr,notes"`
	PublishedAt time.Time      `jsonapi:"attr,published_at,rfc3339"`
	Author      *GraphPerson   `jsonapi:"relation,author"`
	Reviewers   []*GraphPerson `jsonapi:"relation,reviewers"`
	Category    *GraphTag      `jsonapi:"relation,category"`
	Related     []interface{}  `jsonapi:"relation,related"`
}

type GraphPerson struct {
	ID         string          `jsonapi:"primary,people"`
	Name       string          `jsonapi:"attr,name"`
	Alias      *string         `jsonapi:"attr,alias"`
	Articles   []*GraphArticle `jsonapi:"relation,articles"`
	BestFriend *GraphPerson    `jsonapi:"relation,best_friend"`
	Favorite   interface{}     `jsonapi:"relation,favorite"`
}

type GraphTag struct {
	ID     string `jsonapi:"primary,tags"`
	Label  string `jsonapi:"attr,label"`
	Weight int    `jsonapi:"attr,weight"`
}

type GraphMagazine struct {
	ID       string      `jsonapi:"primary,magazines"`
	Name     string      `jsonapi:"attr,name"`
	Featured interface{} `jsonapi:"relation,featured"`
}

// --- Models used only to exercise registration rejection -------------------

type graphBadPrimary struct {
	ID int `jsonapi:"primary,bad-primary"`
}

type graphNoPrimary struct {
	Name string `jsonapi:"attr,name"`
}

type graphTwoPrimaries struct {
	ID  string `jsonapi:"primary,two-a"`
	ID2 string `jsonapi:"primary,two-b"`
}

type graphEmptyType struct {
	ID string `jsonapi:"primary,"`
}

type graphUnexported struct {
	ID     string `jsonapi:"primary,bad-unexported"`
	secret string `jsonapi:"attr,secret"`
}

type graphIntRelation struct {
	ID   string `jsonapi:"primary,bad-int-rel"`
	Nope int    `jsonapi:"relation,nope"`
}

type graphSliceOfValues struct {
	ID    string     `jsonapi:"primary,bad-slice-values"`
	Items []GraphTag `jsonapi:"relation,items"`
}

type graphUnknownTarget struct {
	ID     string         `jsonapi:"primary,bad-unknown-target"`
	Friend *graphIsolated `jsonapi:"relation,friend"`
}

type graphIsolated struct {
	ID string `jsonapi:"primary,isolated-ones"`
}

type graphDuplicateNameA struct {
	ID string `jsonapi:"primary,same-type-name"`
}

type graphDuplicateNameB struct {
	ID string `jsonapi:"primary,same-type-name"`
}

// --- Controllable readers --------------------------------------------------

type countingReader struct {
	r     io.Reader
	reads int
}

func (r *countingReader) Read(p []byte) (int, error) {
	r.reads++
	return r.r.Read(p)
}

var errGraphTailRead = errors.New("graph test: synthetic failure when reading for EOF")

type tailFailReader struct {
	data []byte
	pos  int
}

func (r *tailFailReader) Read(p []byte) (int, error) {
	if r.pos >= len(r.data) {
		return 0, errGraphTailRead
	}
	n := copy(p, r.data[r.pos:])
	r.pos += n
	return n, nil
}

type closeTrackingReader struct {
	io.Reader
	closed bool
}

func (r *closeTrackingReader) Close() error {
	r.closed = true
	return nil
}

// --- Fixtures --------------------------------------------------------------

const heteroDocument = `{
  "data": {
    "type": "articles", "id": "a1",
    "attributes": {
      "title": "Hetero",
      "notes": ["x", "y"],
      "published_at": "2026-11-14T10:00:00Z"
    },
    "relationships": {
      "author": {"data": {"type": "people", "id": "p1"}},
      "reviewers": {"data": [
        {"type": "people", "id": "p1"},
        {"type": "people", "id": "p1"}
      ]},
      "related": {"data": [
        {"type": "tags", "id": "t1"},
        {"type": "people", "id": "p2"}
      ]},
      "category": {"data": null},
      "web": {"links": {"related": "https://example.invalid/web"}}
    }
  },
  "included": [
    {"type": "tags", "id": "t1", "attributes": {"label": "cool", "weight": 7}},
    {"type": "people", "id": "p2", "attributes": {"name": "Grace"}, "relationships": {
      "favorite": {"data": {"type": "articles", "id": "a1"}},
      "best_friend": {"data": {"type": "people", "id": "p1"}}
    }},
    {"type": "people", "id": "p1", "attributes": {"name": "Ada", "alias": null}, "relationships": {
      "articles": {"data": [{"type": "articles", "id": "a1"}]},
      "best_friend": {"data": {"type": "people", "id": "p2"}}
    }}
  ]
}`

func mustGraphDecoder(t *testing.T, models ...interface{}) *GraphDecoder {
	t.Helper()
	d, err := NewGraphDecoder(models...)
	if err != nil {
		t.Fatalf("NewGraphDecoder returned unexpected error: %v", err)
	}
	return d
}

func newStandardGraphDecoder(t *testing.T) *GraphDecoder {
	t.Helper()
	return mustGraphDecoder(t,
		(*GraphArticle)(nil),
		(*GraphPerson)(nil),
		(*GraphTag)(nil),
		(*GraphMagazine)(nil),
	)
}

// --- TestGraphIdentity -----------------------------------------------------

func TestGraphIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dec := newStandardGraphDecoder(t)

		// Heterogeneous chain: article -> people/tag, through a shared person
		// node, and back to the primary record via a typed slice and an
		// interface{} edge. included is deliberately shuffled relative to the
		// order in which relationships mention the resources.
		root := &GraphArticle{}
		before := root
		err := dec.Decode(strings.NewReader(heteroDocument), root)
		c.Check(err == nil, "hetero document decode: %v", err)

		if err == nil {
			c.Check(root == before, "single-object decode must keep the caller root pointer, root=%p before=%p", root, before)
			c.Check(root.ID == "a1", "primary id copied to caller root, got %q", root.ID)
			c.Check(root.Title == "Hetero", "string attribute decoded, got %q", root.Title)
			c.Check(len(root.Notes) == 2 && root.Notes[0] == "x" && root.Notes[1] == "y",
				"string slice attribute decoded, got %#v", root.Notes)
			wantTime, perr := time.Parse(time.RFC3339, "2026-11-14T10:00:00Z")
			c.Check(perr == nil, "fixture time parses: %v", perr)
			c.Check(root.PublishedAt.Equal(wantTime), "rfc3339 time attribute decoded, got %v", root.PublishedAt)

			c.Check(root.Author != nil, "author relation materialized")
			c.Check(root.Author.ID == "p1" && root.Author.Name == "Ada", "author fields filled from included, got %#v", root.Author)
			c.Check(root.Author.Alias == nil, "explicit null pointer attribute stays nil")

			// Back-reference through a typed slice lands on the caller root.
			c.Check(len(root.Author.Articles) == 1 && root.Author.Articles[0] == root,
				"person.articles must point back at the caller-owned root")

			// Repeated identifiers keep their positions and share one pointer.
			c.Check(len(root.Reviewers) == 2, "duplicate linkage identifiers keep positions, got len=%d", len(root.Reviewers))
			c.Check(root.Reviewers[0] == root.Author && root.Reviewers[1] == root.Author,
				"duplicate reviewers resolve to the shared author pointer")

			// Heterogeneous slice routes by the response "type".
			c.Check(len(root.Related) == 2, "heterogeneous related slice length, got %d", len(root.Related))
			tag, tagOK := root.Related[0].(*GraphTag)
			c.Check(tagOK && tag.ID == "t1" && tag.Label == "cool" && tag.Weight == 7,
				"related[0] is a concrete *GraphTag, got %#v", root.Related[0])
			person2, personOK := root.Related[1].(*GraphPerson)
			c.Check(personOK && person2.ID == "p2" && person2.Name == "Grace",
				"related[1] is a concrete *GraphPerson, got %#v", root.Related[1])

			// Shared node reached through two paths.
			c.Check(root.Author.BestFriend != nil && root.Author.BestFriend.ID == "p2", "p1.best_friend is p2")
			c.Check(person2 == root.Author.BestFriend, "p2 reached from related and best_friend is the same pointer")
			c.Check(person2.BestFriend == root.Author, "p2.best_friend cycles back to the shared p1")

			// interface{} back-reference to the primary also lands on the root.
			favArticle, favOK := person2.Favorite.(*GraphArticle)
			c.Check(favOK && favArticle == root, "interface favorite must hold concrete *GraphArticle equal to root")

			// Null to-one stays nil; unknown document members are ignored.
			c.Check(root.Category == nil, "null to-one relationship stays nil")
		}

		// Self loop.
		selfDoc := `{"data":{"type":"people","id":"p3","attributes":{"name":"Solo"},
			"relationships":{"best_friend":{"data":{"type":"people","id":"p3"}}}}}`
		solo := &GraphPerson{}
		err = dec.Decode(strings.NewReader(selfDoc), solo)
		c.Check(err == nil, "self-loop decode: %v", err)
		c.Check(solo.ID == "p3" && solo.Name == "Solo", "self-loop resource fields")
		c.Check(solo.BestFriend == solo, "self-loop must reference the same Go object")

		// Missing definition: a shared placeholder carrying only the primary key.
		ghostDoc := `{"data":{"type":"articles","id":"a2","attributes":{"title":"Ghost"},
			"relationships":{
				"author":{"data":{"type":"people","id":"p9"}},
				"reviewers":{"data":[{"type":"people","id":"p9"}]}
			}}}`
		ghost := &GraphArticle{}
		err = dec.Decode(strings.NewReader(ghostDoc), ghost)
		c.Check(err == nil, "placeholder decode: %v", err)
		c.Check(ghost.Author != nil && ghost.Author.ID == "p9" && ghost.Author.Name == "",
			"missing definition becomes primary-key-only placeholder, got %#v", ghost.Author)
		c.Check(len(ghost.Reviewers) == 1 && ghost.Reviewers[0] == ghost.Author,
			"placeholder identity is shared across relations")

		// A mapped relationship with links but no data is treated as absent:
		// the published field keeps its zero value and drops stale state.
		linksOnly := &GraphArticle{ID: "a3", Title: "Links", Category: &GraphTag{ID: "stale", Label: "Stale"}}
		linksDoc := `{"data":{"type":"articles","id":"a3","attributes":{"title":"Links"},
			"relationships":{"category":{"links":{"self":"https://example.invalid/cat"}}}}}`
		err = dec.Decode(strings.NewReader(linksDoc), linksOnly)
		c.Check(err == nil, "links-only relation decode: %v", err)
		c.Check(linksOnly.ID == "a3" && linksOnly.Title == "Links", "links-only resource still decoded")
		c.Check(linksOnly.Category == nil, "links-only mapped relation is treated as not provided")

		// Two primary records referencing each other, plus an unreferenced
		// included resource and empty/null linkage shapes.
		mutualDoc := `{
			"data": [
				{"type":"articles","id":"m1","relationships":{
					"related":{"data":[{"type":"articles","id":"m2"}]},
					"reviewers":{"data":[]},
					"category":{"data":null}}},
				{"type":"articles","id":"m2","relationships":{
					"related":{"data":[{"type":"articles","id":"m1"}]}}}
			],
			"included": [
				{"type":"tags","id":"unreachable","attributes":{"label":"never visited","weight":1}}
			]
		}`
		var roots []*GraphArticle
		err = dec.Decode(strings.NewReader(mutualDoc), &roots)
		c.Check(err == nil, "mutual primary decode: %v", err)
		c.Check(len(roots) == 2 && roots[0].ID == "m1" && roots[1].ID == "m2",
			"primary order preserved, got %#v", roots)
		c.Check(len(roots[0].Related) == 1, "cross-primary linkage present")
		c.Check(roots[0].Related[0].(*GraphArticle) == roots[1], "m1.related points at the m2 primary object")
		c.Check(roots[1].Related[0].(*GraphArticle) == roots[0], "m2.related points at the m1 primary object")
		c.Check(roots[0].Reviewers != nil && len(roots[0].Reviewers) == 0,
			"empty to-many linkage is a non-nil empty slice")
		c.Check(roots[0].Category == nil, "null to-one in array document stays nil")

		// Empty data array yields a non-nil empty slice.
		empty := []*GraphArticle{{ID: "stale"}}
		err = dec.Decode(strings.NewReader(`{"data":[]}`), &empty)
		c.Check(err == nil, "empty data decode: %v", err)
		c.Check(empty != nil && len(empty) == 0, "empty data publishes a non-nil empty slice, got %#v", empty)
	})
}

// --- TestGraphRouting ------------------------------------------------------

func TestGraphRouting(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dec := newStandardGraphDecoder(t)

		// Registration rejections.
		_, err := NewGraphDecoder(nil)
		c.Check(err != nil, "nil model argument rejected")
		_, err = NewGraphDecoder(GraphArticle{})
		c.Check(err != nil, "non-pointer model rejected")
		_, err = NewGraphDecoder((*GraphArticle)(nil), (*GraphArticle)(nil))
		c.Check(err != nil, "duplicate registration rejected")
		_, err = NewGraphDecoder((*graphDuplicateNameA)(nil), (*graphDuplicateNameB)(nil))
		c.Check(err != nil, "duplicate resource type name rejected")
		_, err = NewGraphDecoder((*graphBadPrimary)(nil))
		c.Check(err != nil, "non-string primary field rejected")
		_, err = NewGraphDecoder((*graphNoPrimary)(nil))
		c.Check(err != nil, "model without primary field rejected")
		_, err = NewGraphDecoder((*graphTwoPrimaries)(nil))
		c.Check(err != nil, "model with two primary fields rejected")
		_, err = NewGraphDecoder((*graphEmptyType)(nil))
		c.Check(err != nil, "empty resource type name rejected")
		_, err = NewGraphDecoder((*graphUnexported)(nil))
		c.Check(err != nil, "unexported tagged field rejected: %v", err)
		_, err = NewGraphDecoder((*graphIntRelation)(nil))
		c.Check(err != nil, "scalar relation shape rejected")
		_, err = NewGraphDecoder((*graphSliceOfValues)(nil), (*GraphTag)(nil))
		c.Check(err != nil, "slice of non-pointer structs relation rejected")
		_, err = NewGraphDecoder((*graphUnknownTarget)(nil))
		c.Check(err != nil, "relation to unregistered model rejected")

		// Destination shape validation happens before the reader is touched.
		cr := &countingReader{r: strings.NewReader(heteroDocument)}
		err = dec.Decode(cr, nil)
		c.Check(err != nil && cr.reads == 0, "nil destination rejected before reading, reads=%d", cr.reads)
		cr = &countingReader{r: strings.NewReader(heteroDocument)}
		err = dec.Decode(cr, GraphArticle{})
		c.Check(err != nil && cr.reads == 0, "non-pointer destination rejected before reading, reads=%d", cr.reads)
		cr = &countingReader{r: strings.NewReader(heteroDocument)}
		err = dec.Decode(cr, &graphIsolated{})
		c.Check(err != nil && cr.reads == 0, "unregistered destination type rejected before reading, reads=%d", cr.reads)
		cr = &countingReader{r: strings.NewReader(heteroDocument)}
		var badSlice []GraphArticle
		err = dec.Decode(cr, &badSlice)
		c.Check(err != nil && cr.reads == 0, "*[]T destination rejected before reading, reads=%d", cr.reads)

		// Document-level routing and structural validation.
		routingErrorCases := map[string]string{
			"null primary data":         `{"data":null}`,
			"missing data":              `{"meta":{}}`,
			"object into slice dst":     `{"data":{"type":"articles","id":"z"}}`,
			"array into object dst":     `{"data":[]}`,
			"missing type":              `{"data":{"id":"a1"}}`,
			"empty type":                `{"data":{"type":"","id":"a1"}}`,
			"missing id":                `{"data":{"type":"articles"}}`,
			"empty id":                  `{"data":{"type":"articles","id":""}}`,
			"duplicate data included":   `{"data":{"type":"articles","id":"d1"},"included":[{"type":"articles","id":"d1"}]}`,
			"duplicate included":        `{"data":{"type":"articles","id":"d1"},"included":[{"type":"tags","id":"t1"},{"type":"tags","id":"t1"}]}`,
			"identifier attributes":     `{"data":{"type":"articles","id":"a1","relationships":{"reviewers":{"data":[{"type":"people","id":"p1","attributes":{}}]}}}}`,
			"identifier relationships":  `{"data":{"type":"articles","id":"a1","relationships":{"author":{"data":{"type":"people","id":"p1","relationships":{}}}}}}`,
			"attributes not object":     `{"data":{"type":"articles","id":"a1","attributes":[]}}`,
			"relationships not object":  `{"data":{"type":"articles","id":"a1","relationships":[]}}`,
			"included not array":        `{"data":{"type":"articles","id":"a1"},"included":{}}`,
			"lid rejected":              `{"data":{"type":"articles","id":"a1","lid":"local-1"}}`,
			"errors document":           `{"errors":[{"title":"nope"}]}`,
			"to-one got array":          `{"data":{"type":"articles","id":"a1","relationships":{"author":{"data":[{"type":"people","id":"p1"}]}}}}`,
			"to-many got object":        `{"data":{"type":"articles","id":"a1","relationships":{"reviewers":{"data":{"type":"people","id":"p1"}}}}}`,
			"to-many got null":          `{"data":{"type":"articles","id":"a1","relationships":{"reviewers":{"data":null}}}}`,
			"typed relation wrong type": `{"data":{"type":"articles","id":"a1","relationships":{"author":{"data":{"type":"tags","id":"t1"}}}}}`,
			"unknown interface type":    `{"data":{"type":"magazines","id":"mg1","relationships":{"featured":{"data":{"type":"widgets","id":"w1"}}}}}`,
			"unknown relation type":     `{"data":{"type":"articles","id":"a1","relationships":{"reviewers":{"data":[{"type":"widgets","id":"w1"}]}}}}`,
			"primary type mismatch":     `{"data":{"type":"people","id":"p1"}}`,
			"trailing second document":  heteroDocument + `{"data":null}`,
		}
		for name, document := range routingErrorCases {
			var dst interface{}
			if strings.Contains(name, "slice dst") {
				dst = &[]*GraphArticle{}
			} else if strings.Contains(name, "object dst") {
				dst = &GraphArticle{}
			} else if strings.Contains(name, "magazine") || strings.Contains(name, "interface") {
				dst = &GraphMagazine{}
			} else if strings.Contains(name, "primary type mismatch") {
				dst = &GraphArticle{}
			} else {
				dst = &GraphArticle{}
			}
			err := dec.Decode(strings.NewReader(document), dst)
			c.Check(err != nil, "case %q must fail", name)
		}

		// interface{} routing selects the concrete pointer from the response type.
		magTagDoc := `{"data":{"type":"magazines","id":"mg1","attributes":{"name":"Monthly"},
			"relationships":{"featured":{"data":{"type":"tags","id":"t9"}}}},
			"included":[{"type":"tags","id":"t9","attributes":{"label":"hot","weight":1}}]}`
		mag := &GraphMagazine{}
		err = dec.Decode(strings.NewReader(magTagDoc), mag)
		c.Check(err == nil, "magazine/tag routing decode: %v", err)
		featuredTag, ok := mag.Featured.(*GraphTag)
		c.Check(ok && featuredTag.ID == "t9" && featuredTag.Label == "hot",
			"interface field holds concrete *GraphTag, got %#v", mag.Featured)

		magPersonDoc := `{"data":{"type":"magazines","id":"mg2","relationships":{"featured":{"data":{"type":"people","id":"p8"}}}},
			"included":[{"type":"people","id":"p8","attributes":{"name":"Hero"}}]}`
		mag2 := &GraphMagazine{}
		err = dec.Decode(strings.NewReader(magPersonDoc), mag2)
		c.Check(err == nil, "magazine/person routing decode: %v", err)
		featuredPerson, ok := mag2.Featured.(*GraphPerson)
		c.Check(ok && featuredPerson.ID == "p8" && featuredPerson.Name == "Hero",
			"interface field holds concrete *GraphPerson, got %#v", mag2.Featured)

		// Truncated JSON keeps the standard sentinel discoverable via errors.Is.
		truncated := heteroDocument[:len(heteroDocument)/2]
		err = dec.Decode(strings.NewReader(truncated), &GraphArticle{})
		c.Check(err != nil, "truncated document fails")
		c.Check(errors.Is(err, io.ErrUnexpectedEOF), "truncated JSON preserves io.ErrUnexpectedEOF, got %v", err)

		// A late attribute conversion failure is reported with the resource.
		badTimeDoc := `{"data":{"type":"articles","id":"a1","attributes":{"published_at":"not-a-timestamp"}}}`
		err = dec.Decode(strings.NewReader(badTimeDoc), &GraphArticle{})
		c.Check(err != nil && strings.Contains(err.Error(), "a1"),
			"attribute error names the resource, got %v", err)
	})
}

// --- TestGraphPublication --------------------------------------------------

func TestGraphPublication(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		dec := newStandardGraphDecoder(t)

		// Pre-built old graph; the relation node is also held by an external
		// business variable.
		oldAuthor := &GraphPerson{ID: "old-p", Name: "Old Author"}
		oldRoot := &GraphArticle{
			ID:        "old-a",
			Title:     "Old Title",
			Author:    oldAuthor,
			Reviewers: []*GraphPerson{oldAuthor},
		}
		failureDocuments := map[string]string{
			"duplicate definition":   `{"data":{"type":"articles","id":"a1"},"included":[{"type":"articles","id":"a1"}]}`,
			"bad attribute":          `{"data":{"type":"articles","id":"a1","attributes":{"published_at":"nope"}}}`,
			"relation type mismatch": `{"data":{"type":"articles","id":"a1","relationships":{"author":{"data":{"type":"tags","id":"t1"}}}}}`,
			"trailing json":          heteroDocument + ` ` + `{"meta":{}}`,
		}
		for name, document := range failureDocuments {
			root := &GraphArticle{
				ID:        oldRoot.ID,
				Title:     oldRoot.Title,
				Author:    oldAuthor,
				Reviewers: []*GraphPerson{oldAuthor},
			}
			err := dec.Decode(strings.NewReader(document), root)
			c.Check(err != nil, "failure case %q returns an error", name)
			c.Check(root.ID == "old-a" && root.Title == "Old Title",
				"case %q leaves old root scalars untouched", name)
			c.Check(root.Author == oldAuthor && root.Author.Name == "Old Author",
				"case %q leaves old root relation and node untouched", name)
			c.Check(len(root.Reviewers) == 1 && root.Reviewers[0] == oldAuthor,
				"case %q leaves old slice untouched", name)

			slice := []*GraphArticle{{ID: "old-s1"}, {ID: "old-s2"}}
			err = dec.Decode(strings.NewReader(document), &slice)
			c.Check(err != nil, "slice failure case %q returns an error", name)
			c.Check(len(slice) == 2 && slice[0].ID == "old-s1" && slice[1].ID == "old-s2",
				"case %q leaves the old destination slice untouched", name)
		}
		c.Check(oldAuthor.ID == "old-p" && oldAuthor.Name == "Old Author",
			"externally held old node is never filled in place")

		// Reader fails when the decoder probes for EOF: parse succeeded but
		// publication must not happen, and the cause must survive errors.Is.
		tail := &tailFailReader{data: []byte(heteroDocument)}
		tailRoot := &GraphArticle{ID: "old-a", Title: "Old Title", Author: oldAuthor}
		err := dec.Decode(tail, tailRoot)
		c.Check(errors.Is(err, errGraphTailRead), "tail read failure preserved via errors.Is, got %v", err)
		c.Check(tailRoot.ID == "old-a" && tailRoot.Title == "Old Title" && tailRoot.Author == oldAuthor,
			"tail read failure does not publish anything")

		// A successful decode fully replaces stale state and never mutates the
		// previously attached node.
		replaceRoot := &GraphArticle{ID: "stale", Title: "Stale", Author: oldAuthor}
		minimalDoc := `{"data":{"type":"articles","id":"new-a","attributes":{"title":"New"}}}`
		err = dec.Decode(strings.NewReader(minimalDoc), replaceRoot)
		c.Check(err == nil, "replacement decode: %v", err)
		c.Check(replaceRoot.ID == "new-a" && replaceRoot.Title == "New", "new scalars published")
		c.Check(replaceRoot.Author == nil, "stale relation absent from the new document is cleared")
		c.Check(oldAuthor.ID == "old-p" && oldAuthor.Name == "Old Author",
			"old relation node kept by another variable is not mutated")

		// The decoder does not close the caller's reader.
		body := &closeTrackingReader{Reader: strings.NewReader(heteroDocument)}
		err = dec.Decode(body, &GraphArticle{})
		c.Check(err == nil, "decode through close-tracked reader: %v", err)
		c.Check(!body.closed, "Decode must not close the caller's reader")

		// Sequential calls keep their object sets isolated.
		first := &GraphArticle{}
		err = dec.Decode(strings.NewReader(heteroDocument), first)
		c.Check(err == nil, "first sequential decode: %v", err)
		second := &GraphArticle{}
		err = dec.Decode(strings.NewReader(heteroDocument), second)
		c.Check(err == nil, "second sequential decode: %v", err)
		c.Check(first != second, "primary objects differ across calls")
		c.Check(first.Author != second.Author, "same identity in two calls yields distinct objects")
		c.Check(first.Author.Articles[0] == first && second.Author.Articles[0] == second,
			"each call keeps its own internally shared graph")

		// Concurrent calls on one read-only decoder: race detector guards the
		// implementation; identities must never leak across requests.
		const goroutines = 8
		const iterations = 12
		var wg sync.WaitGroup
		var mu sync.Mutex
		roots := make([]*GraphArticle, 0, goroutines*iterations)
		for g := 0; g < goroutines; g++ {
			wg.Add(1)
			go func() {
				defer wg.Done()
				for i := 0; i < iterations; i++ {
					root := &GraphArticle{}
					if derr := dec.Decode(strings.NewReader(heteroDocument), root); derr != nil {
						t.Errorf("concurrent decode failed: %v", derr)
						return
					}
					if root.Author == nil || root.Author.Articles[0] != root ||
						root.Reviewers[0] != root.Author || root.Reviewers[1] != root.Author {
						t.Errorf("concurrent decode produced broken topology")
						return
					}
					mu.Lock()
					roots = append(roots, root)
					mu.Unlock()
				}
			}()
		}
		wg.Wait()
		c.Check(len(roots) == goroutines*iterations, "all concurrent decodes completed, got %d", len(roots))
		seen := map[*GraphArticle]bool{}
		duplicates := 0
		for _, r := range roots {
			if seen[r] {
				duplicates++
			}
			seen[r] = true
		}
		c.Check(duplicates == 0, "no primary object is shared across concurrent calls, duplicates=%d", duplicates)
	})
}
