package gofeed_test

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"

	"github.com/mmcdole/gofeed"
)

// exampleTransport serves one in-memory RSS document so the example runs
// without any network access.
type exampleTransport struct{}

func (exampleTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	doc := `<rss version="2.0"><channel><title>Example Feed</title>` +
		`<item><guid isPermaLink="false">story-1</guid><title>Example Story</title>` +
		`<link>https://example.test/story</link></item>` +
		`</channel></rss>`
	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     make(http.Header),
		Request:    r,
		Body:       io.NopCloser(strings.NewReader(doc)),
	}, nil
}

// ExampleOpenCollection shows collecting a feed into a local on-disk
// collection and reading the recomputed article records back out.
func ExampleOpenCollection() {
	dir, err := os.MkdirTemp("", "gofeed-collection-example-")
	if err != nil {
		fmt.Println("temp dir:", err)
		return
	}
	defer os.RemoveAll(dir)

	parser := gofeed.NewParser()
	parser.Client = &http.Client{Transport: exampleTransport{}}

	collection, err := gofeed.OpenCollection(dir, parser)
	if err != nil {
		fmt.Println("open:", err)
		return
	}

	snapshot, err := collection.Collect(context.Background(), "https://feeds.example/feed.xml")
	if err != nil {
		fmt.Println("collect:", err)
		return
	}

	for _, record := range snapshot.Records {
		fmt.Printf("%s observed %d time(s)\n", record.Item.Title, len(record.Origins))
	}

	// Reopening later rebuilds every group from the stored sources, without
	// issuing any requests.
	reopened, err := gofeed.OpenCollection(dir, parser)
	if err != nil {
		fmt.Println("reopen:", err)
		return
	}
	fmt.Println("records after reopen:", len(reopened.Snapshot().Records))

	// Output:
	// Example Story observed 1 time(s)
	// records after reopen: 1
}
