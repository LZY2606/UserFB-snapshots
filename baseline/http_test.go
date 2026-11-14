package baseline

import (
	"fmt"
	"io"
	"net/http"
	"reflect"
	"strings"
	"testing"

	"github.com/google/jsonapi"
)

type person struct {
	ID   string `jsonapi:"primary,people"`
	Name string `jsonapi:"attr,name"`
}
type article struct {
	ID     string  `jsonapi:"primary,articles"`
	Title  string  `jsonapi:"attr,title"`
	Author *person `jsonapi:"relation,author"`
}
type roundTrip func(*http.Request) (*http.Response, error)

func (f roundTrip) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

type trackedBody struct {
	io.Reader
	closed bool
}

func (b *trackedBody) Close() error { b.closed = true; return nil }

// This only exercises the published decoder. It is not a graph decoder.
func TestJSONAPIHTTPBaseline(t *testing.T) {
	const document = `{"data":[{"type":"articles","id":"p1","attributes":{"title":"First"},"relationships":{"author":{"data":{"type":"people","id":"u1"}}}},{"type":"articles","id":"p2","attributes":{"title":"Second"},"relationships":{"author":{"data":{"type":"people","id":"u1"}}}}],"included":[{"type":"people","id":"u1","attributes":{"name":"Ada"}}]}`
	b := &trackedBody{Reader: strings.NewReader(document)}
	calls := 0
	client := &http.Client{Transport: roundTrip(func(req *http.Request) (*http.Response, error) {
		calls++
		if req.Method != "GET" || req.URL.String() != "https://example.invalid/articles" {
			t.Fatalf("unexpected request: %s %s", req.Method, req.URL)
		}
		return &http.Response{StatusCode: 200, Header: http.Header{"Content-Type": []string{jsonapi.MediaType}}, Body: b, Request: req}, nil
	})}
	resp, err := client.Get("https://example.invalid/articles")
	if err != nil {
		t.Fatal(err)
	}
	models, err := jsonapi.UnmarshalManyPayload(resp.Body, reflect.TypeOf((*article)(nil)))
	if err := resp.Body.Close(); err != nil {
		t.Fatal(err)
	}
	if err != nil {
		t.Fatal(err)
	}
	if len(models) != 2 {
		t.Fatalf("articles=%d", len(models))
	}
	first, second := models[0].(*article), models[1].(*article)
	if first.ID != "p1" || second.ID != "p2" || first.Title != "First" || second.Title != "Second" || first.Author == nil || second.Author == nil {
		t.Fatal("wrong decoded fields")
	}
	if first.Author.ID != "u1" || second.Author.ID != "u1" || first.Author.Name != "Ada" || second.Author.Name != "Ada" {
		t.Fatal("sideloaded attributes lost")
	}
	if first.Author == second.Author {
		t.Fatal("fixed legacy behavior changed: expected separate allocations")
	}
	if calls != 1 || !b.closed {
		t.Fatal("HTTP lifecycle mismatch")
	}
	fmt.Println("JSONAPI_HTTP_BASELINE_OK requests=1 roots=2 author_id=u1 author_name=Ada")
	fmt.Println("LEGACY_RELATION_IDENTITY_WITNESS same_resource=true same_pointer=false")
}
