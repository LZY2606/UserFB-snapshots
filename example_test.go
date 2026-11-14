package rss_test

import (
	"context"
	"fmt"
	"log"

	"github.com/SlyMarbo/rss/v2"
)

// ExampleReader_CaptureHistory shows how to capture an Atom
// subscription entry together with its RFC 5005 archive chain. The
// example is compiled as part of the test suite but not executed, as
// it needs a live feed.
func ExampleReader_CaptureHistory() {
	reader, err := rss.NewReader()
	if err != nil {
		log.Fatal(err)
	}

	ctx := context.Background()
	entry := "https://example.com/atom"

	// Capture the entry and the whole archive chain.
	history, err := reader.CaptureHistory(ctx, entry, nil, "full")
	if err != nil {
		log.Fatal(err)
	}

	fmt.Println(history.Feed.Title)
	for _, document := range history.Documents {
		// Final response addresses, from the entry to the oldest
		// archive.
		fmt.Println(document)
	}

	// Some time later, reuse successfully captured archives and
	// continue along the chain from where it ended.
	next, err := reader.CaptureHistory(ctx, entry, history, "incremental")
	if err != nil {
		log.Fatal(err)
	}

	fmt.Println(next.Feed.Unread)
}
