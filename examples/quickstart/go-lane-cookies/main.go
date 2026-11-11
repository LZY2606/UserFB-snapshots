// Lane cookie persistence: when each collection finishes using a page, its
// isolated browser context is read in full and only that page's own cookie
// delta is merged back into the proxy lane. Run this where Chromium is
// available:
//
//	go run ./examples/quickstart/go-lane-cookies
package main

import (
	"context"
	"fmt"
	"log"
	"time"

	"github.com/karust/openserp/core"
)

func main() {
	// One bounded store keeps sticky sessions per engine + proxy session id.
	laneStore := core.NewLaneStore(core.DefaultProxyLaneMaxLanes)

	browser, err := core.NewBrowser(core.BrowserOpts{
		IsHeadless:     true,
		Timeout:        30 * time.Second,
		ProxyLaneStore: laneStore,
	})
	if err != nil {
		log.Fatalf("launch browser: %v", err)
	}
	defer func() { _ = browser.Close() }()

	// Both requests belong to the same tenant/engine/session lane.
	laneKey := core.ProxyLaneKey{Tenant: "tenant-a", Engine: "bing", SessionID: "sticky-session"}
	ctx := core.WithProxyLaneKey(context.Background(), laneKey)

	// First collection: Navigate restores the lane snapshot into a fresh
	// isolated browser context. Nothing is saved during navigation; in-tab
	// work (form submission, pagination, redirects) keeps mutating cookies.
	firstPage, err := browser.Navigate(ctx, "https://www.bing.com/")
	if err != nil {
		log.Fatalf("first navigate: %v", err)
	}
	// DeferClosePage is the save point: it reads the complete context cookie
	// set and merges this page's additions, changes and deletions into the
	// lane before the context is destroyed.
	core.DeferClosePage(ctx, firstPage, browser)()

	// Second collection on the same lane restores everything the first page
	// finished with, including cookies created after the initial navigation.
	secondPage, err := browser.Navigate(ctx, "https://www.bing.com/")
	if err != nil {
		log.Fatalf("second navigate: %v", err)
	}
	core.DeferClosePage(ctx, secondPage, browser)()

	// Manager-side access stays a full replace and invalidates in-flight page
	// snapshots. A captcha challenge instead calls DropCookies, which fences
	// off pages that already started while preserving the browser profile.
	laneStore.DropCookies(laneKey)
	fmt.Printf("lane stats: %+v\n", laneStore.Stats())
}
