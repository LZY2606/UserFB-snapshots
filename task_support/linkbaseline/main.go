package main

import (
	"fmt"
	"os"
	"strings"

	rss "github.com/SlyMarbo/rss/v2"
)

func main() {
	r, err := rss.NewReader()
	if err != nil {
		panic(err)
	}
	sources := []string{
		`<rss version="2.0" xmlns:atom="http://www.w3.org/2005/Atom"><channel><title>channel</title><atom:link rel="hub" href="/hub"/><atom:link rel="self" href="/topic"/><item><guid>a</guid><title>old</title></item><item><guid>a</guid><title>ignored</title></item></channel></rss>`,
		`<feed xmlns="http://www.w3.org/2005/Atom"><title>channel</title><link rel="hub" href="/hub"/><link rel="self" href="/topic"/><entry><id>a</id><title>old</title></entry><entry><id>a</id><title>ignored</title></entry></feed>`,
	}
	for i, source := range sources {
		feed, err := r.Parse("https://source.invalid/feed", strings.NewReader(source))
		if err != nil || len(feed.Items) != 1 || feed.Items[0].ID != "a" || feed.Items[0].Title != "old" || feed.Items[0].Read || feed.Unread != 1 {
			fmt.Fprintln(os.Stderr, "BASELINE parse/dedup failed", i, err)
			os.Exit(1)
		}
		links := make(map[string]string)
		for _, link := range feed.Links {
			links[link.Rel] = link.Href
		}
		if links["hub"] != "/hub" || links["self"] != "/topic" {
			fmt.Fprintln(os.Stderr, "BASELINE relation preservation failed", i)
			os.Exit(1)
		}
	}
	fmt.Println("BASELINE websub-discovery-links=ok formats=RSS2,Atom")
}
