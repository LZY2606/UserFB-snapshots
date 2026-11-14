package sitemap_test

import (
	"context"
	"fmt"
	"net/http"
	"time"

	"github.com/oxffaa/gopher-parse-sitemap"
)

// resumeConsumer is the downstream sink. It must be idempotent: ResumeFromSite
// delivers each entry at least once, and an entry may be delivered again when
// the process stops between a successful consumer call and the checkpoint
// write.
func resumeConsumer(entry sitemap.Entry) error {
	lastModified := entry.GetLastModified()
	if lastModified == nil {
		fmt.Printf("no date for %s\n", entry.GetLocation())
		return nil
	}
	fmt.Printf("%s last changed %s\n", entry.GetLocation(), lastModified.Format("2006-01-02"))
	return nil
}

// ExampleResumeFromSite shows the cross-process shape of a resumable sitemap
// download. Each call is what a single invocation of a program does: the
// checkpoint lives in a file, so a crash and a later restart simply run
// ResumeFromSite again with the same arguments. The first call below is stopped
// early by the consumer to model an interrupted process; the second call is a
// fresh process that continues from the persisted byte offset.
func ExampleResumeFromSite() {
	const checkpointPath = "sitemap-resume.json"
	const sitemapURL = "https://example.com/sitemap.xml"

	client := &http.Client{Timeout: 30 * time.Second}
	ctx := context.Background()

	delivered := 0
	interrupted := fmt.Errorf("interrupting this process on purpose")
	err := sitemap.ResumeFromSite(ctx, client, sitemapURL, checkpointPath, func(entry sitemap.Entry) error {
		if err := resumeConsumer(entry); err != nil {
			return err
		}
		delivered++
		if delivered == 2 {
			return interrupted
		}
		return nil
	})
	if err != nil && err != interrupted {
		panic(err)
	}

	// A later invocation — in reality a different process with the same
	// checkpoint path — picks up after the last acknowledged entry and,
	// because the consumer returns nil for everything below, marks the
	// download complete.
	err = sitemap.ResumeFromSite(ctx, client, sitemapURL, checkpointPath, resumeConsumer)
	if err != nil {
		panic(err)
	}
}
