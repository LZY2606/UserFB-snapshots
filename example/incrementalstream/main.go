//go:build ignore

// incrementalstream is a small, self-contained example of QueryIncremental.
// It starts an httptest server that emits a multipart/mixed response piece by
// piece and prints the query value observed in every callback.
package main

import (
	"context"
	"fmt"
	"log"
	"net/http"
	"net/http/httptest"

	"github.com/shurcooL/graphql"
)

func main() {
	// A stand-in for the collection service: the first part carries the
	// catalog card and the first entry; a later part appends another entry.
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "multipart/mixed; boundary=parts")
		flusher := w.(http.Flusher)

		write := func(s string) {
			fmt.Fprint(w, s)
			flusher.Flush()
		}

		write("--parts\r\nContent-Type: application/json\r\n\r\n" +
			`{"data":{"catalog":{"card":{"name":"Ada"},"entries":[{"id":"e0"}]}},"hasNext":true}` +
			"\r\n")
		write("--parts\r\nContent-Type: application/json\r\n\r\n" +
			`{"incremental":[{"items":[{"id":"e1"}],"path":["catalog","entries",1]}],"hasNext":false}` +
			"\r\n")
		write("--parts--\r\n")
	}))
	defer server.Close()

	client := graphql.NewClient(server.URL, server.Client())

	var q struct {
		Catalog struct {
			Card struct {
				Name graphql.String
			}
			Entries []struct {
				ID graphql.ID
			}
		}
	}

	err := client.QueryIncremental(context.Background(), &q, nil, func(part graphql.IncrementalResult) error {
		fmt.Printf("part: hasNext=%v entries=%d card=%q\n",
			part.HasNext, len(q.Catalog.Entries), q.Catalog.Card.Name)
		return nil
	})
	if err != nil {
		log.Fatal(err)
	}

	// Output:
	// part: hasNext=true entries=1 card="Ada"
	// part: hasNext=false entries=2 card="Ada"
}
