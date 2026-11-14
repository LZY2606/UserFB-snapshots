package jsonapi

import (
	"fmt"
	"io"
	"log"
	"net/http"
	"strings"
)

// ExampleGraphDecoder shows the graph decoder being handed an HTTP response
// body directly. The document references one author from two articles and has
// that author point back at the articles; every reference resolves to the same
// Go pointer instead of producing separate allocations.
func ExampleGraphDecoder() {
	// The registry is built once and is safe to reuse across requests,
	// including concurrent ones.
	decoder, err := NewGraphDecoder(
		(*GraphArticle)(nil),
		(*GraphPerson)(nil),
		(*GraphTag)(nil),
		(*GraphMagazine)(nil),
	)
	if err != nil {
		log.Fatal(err)
	}

	document := `{
		"data": [
			{"type": "articles", "id": "a1", "attributes": {"title": "Sharing pointers in Go"},
				"relationships": {"author": {"data": {"type": "people", "id": "u1"}}}},
			{"type": "articles", "id": "a2", "attributes": {"title": "Second article"},
				"relationships": {"author": {"data": {"type": "people", "id": "u1"}}}}
		],
		"included": [
			{"type": "people", "id": "u1", "attributes": {"name": "Ada"},
				"relationships": {"articles": {"data": [
					{"type": "articles", "id": "a1"},
					{"type": "articles", "id": "a2"}
				]}}}
		]
	}`

	// In a real client this is the *http.Response returned by http.Client.Do.
	// The decoder only reads the body; closing it stays the caller's job.
	resp := &http.Response{
		StatusCode: http.StatusOK,
		Header:     http.Header{"Content-Type": []string{MediaType}},
		Body:       io.NopCloser(strings.NewReader(document)),
	}
	defer func() {
		if err := resp.Body.Close(); err != nil {
			log.Fatal(err)
		}
	}()

	var articles []*GraphArticle
	if err := decoder.Decode(resp.Body, &articles); err != nil {
		log.Fatal(err)
	}

	first, second := articles[0], articles[1]
	fmt.Println(first.ID, first.Title)
	fmt.Println(second.ID, second.Title)
	fmt.Println(first.Author == second.Author)
	fmt.Println(first.Author.ID, first.Author.Name)
	fmt.Println(first.Author.Articles[0] == first && first.Author.Articles[1] == second)

	// Output:
	// a1 Sharing pointers in Go
	// a2 Second article
	// true
	// u1 Ada
	// true
}
