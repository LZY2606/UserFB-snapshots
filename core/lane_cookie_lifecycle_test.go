package core

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sync"
	"testing"
	"time"

	"github.com/go-rod/rod/lib/proto"
	browserprofile "github.com/karust/openserp/core/browser"
	"github.com/karust/openserp/testutil/cdptest"
)

func lifecycleCookie(name, value string) *proto.NetworkCookie {
	return &proto.NetworkCookie{
		Name:     name,
		Value:    value,
		Domain:   "source.example.invalid",
		Path:     "/",
		Size:     len(value),
		Session:  true,
		SameSite: proto.NetworkCookieSameSiteLax,
	}
}

func lifecycleCookieWith(name, value, domain, path string) *proto.NetworkCookie {
	cookie := lifecycleCookie(name, value)
	cookie.Domain = domain
	cookie.Path = path
	return cookie
}

func findLifecycleCookie(t *testing.T, cookies []*proto.NetworkCookie, name string) *proto.NetworkCookie {
	t.Helper()
	for _, cookie := range cookies {
		if cookie.Name == name {
			return cookie
		}
	}
	t.Fatalf("cookie %q not found in %d cookies", name, len(cookies))
	return nil
}

func lifecycleKey() ProxyLaneKey {
	return ProxyLaneKey{Tenant: "tenant-a", Engine: "bing", SessionID: "sid-a"}
}

// TestLaneCookieLifecycleStore covers the LaneStore merge contract with
// channel-barriered concurrent page sessions.
func TestLaneCookieLifecycleStore(t *testing.T) {
	t.Run("independent_modifications_both_survive", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{
			lifecycleCookie("a", "0"),
			lifecycleCookie("b", "0"),
		})

		sessionA := store.beginPage(key)
		sessionB := store.beginPage(key)
		if sessionA == nil || sessionB == nil {
			t.Fatal("expected both page sessions")
		}

		releaseA := make(chan struct{})
		releaseB := make(chan struct{})
		committed := make(chan struct{}, 2)
		go func() {
			<-releaseA
			store.commitPage(sessionA, []*proto.NetworkCookie{
				lifecycleCookie("a", "1"),
				lifecycleCookie("b", "0"),
			})
			committed <- struct{}{}
		}()
		go func() {
			<-releaseB
			store.commitPage(sessionB, []*proto.NetworkCookie{
				lifecycleCookie("a", "0"),
				lifecycleCookie("b", "1"),
			})
			committed <- struct{}{}
		}()

		close(releaseA)
		<-committed
		close(releaseB)
		<-committed

		got := store.Cookies(key)
		if len(got) != 2 {
			t.Fatalf("expected 2 cookies, got %d", len(got))
		}
		if findLifecycleCookie(t, got, "a").Value != "1" {
			t.Fatal("page A's update to a was lost")
		}
		if findLifecycleCookie(t, got, "b").Value != "1" {
			t.Fatal("page B's update to b was lost")
		}
	})

	t.Run("real_delete_and_no_resurrection", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{
			lifecycleCookie("a", "0"),
			lifecycleCookie("b", "0"),
		})

		deleter := store.beginPage(key)
		quiet := store.beginPage(key)

		// The deleting page removes b from its isolated context.
		if ok := store.commitPage(deleter, []*proto.NetworkCookie{lifecycleCookie("a", "0")}); !ok {
			t.Fatal("delete commit rejected")
		}
		// The other page never touched anything: its inherited b must not come back.
		if ok := store.commitPage(quiet, []*proto.NetworkCookie{
			lifecycleCookie("a", "0"),
			lifecycleCookie("b", "0"),
		}); !ok {
			t.Fatal("quiet commit rejected")
		}

		got := store.Cookies(key)
		if len(got) != 1 || got[0].Name != "a" {
			t.Fatalf("deleted cookie b was resurrected: %#v", got)
		}
	})

	t.Run("same_key_conflict_last_commit_wins", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{
			lifecycleCookie("a", "0"),
			lifecycleCookie("b", "0"),
		})

		sessionA := store.beginPage(key)
		sessionB := store.beginPage(key)

		firstDone := make(chan struct{})
		go func() {
			store.commitPage(sessionA, []*proto.NetworkCookie{
				lifecycleCookie("a", "1"),
				lifecycleCookie("b", "0"),
			})
			close(firstDone)
		}()
		<-firstDone

		release := make(chan struct{})
		secondDone := make(chan struct{})
		go func() {
			<-release
			store.commitPage(sessionB, []*proto.NetworkCookie{
				lifecycleCookie("a", "2"),
				lifecycleCookie("b", "0"),
			})
			close(secondDone)
		}()
		close(release)
		<-secondDone

		got := store.Cookies(key)
		if findLifecycleCookie(t, got, "a").Value != "2" {
			t.Fatal("later commit to the same cookie must win")
		}
		if findLifecycleCookie(t, got, "b").Value != "0" {
			t.Fatal("unrelated cookie changed")
		}
	})

	t.Run("empty_lane_drop_fences_old_pages_but_keeps_profile", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		// A lane with no cookies at all.
		oldSession := store.beginPage(key)

		store.DropCookies(key)
		if stats := store.Stats(); stats.CookiesDropped != 0 {
			t.Fatalf("dropping an empty lane must not count: %#v", stats)
		}

		if ok := store.commitPage(oldSession, []*proto.NetworkCookie{lifecycleCookie("a", "late")}); ok {
			t.Fatal("page started before an empty-lane drop must lose write-back")
		}
		if got := store.Cookies(key); len(got) != 0 {
			t.Fatalf("stale commit polluted dropped lane: %#v", got)
		}

		// A page started after the drop works normally.
		newSession := store.beginPage(key)
		if ok := store.commitPage(newSession, []*proto.NetworkCookie{lifecycleCookie("a", "fresh")}); !ok {
			t.Fatal("post-drop page could not save its session")
		}
		if findLifecycleCookie(t, store.Cookies(key), "a").Value != "fresh" {
			t.Fatal("post-drop session was not persisted")
		}

		// Repeated drops of an always-empty lane never inflate the counter.
		emptyKey := ProxyLaneKey{Tenant: "tenant-a", Engine: "bing", SessionID: "sid-empty"}
		store.beginPage(emptyKey) // materialize the lane without any cookies
		store.DropCookies(emptyKey)
		store.DropCookies(emptyKey)
		if stats := store.Stats(); stats.CookiesDropped != 0 {
			t.Fatalf("repeated empty drops changed counter: %#v", stats)
		}
	})

	t.Run("drop_with_cookies_counts_once_and_preserves_profile", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		profile := store.Profile(key, func() browserprofile.Profile {
			return browserprofile.Profile{ID: "profile-a"}
		})
		if profile.ID != "profile-a" {
			t.Fatalf("unexpected profile: %#v", profile)
		}
		store.SaveCookies(key, []*proto.NetworkCookie{lifecycleCookie("a", "0")})
		session := store.beginPage(key)

		store.DropCookies(key)
		store.DropCookies(key) // already empty: no extra count
		if stats := store.Stats(); stats.CookiesDropped != 1 {
			t.Fatalf("expected cookies_dropped=1, got %#v", stats)
		}
		if ok := store.commitPage(session, []*proto.NetworkCookie{lifecycleCookie("a", "late")}); ok {
			t.Fatal("stale session wrote after drop")
		}
		// The profile survives a cookie drop.
		if got := store.Profile(key, func() browserprofile.Profile {
			return browserprofile.Profile{ID: "should-not-be-used"}
		}); got.ID != "profile-a" {
			t.Fatalf("drop did not preserve the profile: %#v", got)
		}
	})

	t.Run("eviction_recreation_rejects_late_commits", func(t *testing.T) {
		store := NewLaneStore(1)
		key := lifecycleKey()
		other := ProxyLaneKey{Tenant: "tenant-a", Engine: "bing", SessionID: "sid-b"}
		store.SaveCookies(key, []*proto.NetworkCookie{lifecycleCookie("a", "0")})
		oldSession := store.beginPage(key)

		// Filling the single capacity evicts key.
		store.SaveCookies(other, []*proto.NetworkCookie{lifecycleCookie("a", "other")})
		if got := store.Cookies(key); len(got) != 0 {
			t.Fatal("eviction did not remove the lane")
		}

		barrier := make(chan struct{})
		lateDone := make(chan struct{})
		go func() {
			<-barrier
			ok := store.commitPage(oldSession, []*proto.NetworkCookie{lifecycleCookie("a", "late")})
			if ok {
				panic("late commit should have been rejected")
			}
			close(lateDone)
		}()
		close(barrier)
		<-lateDone

		// The late reply neither recreated the lane nor evicted the survivor.
		if got := store.Cookies(key); len(got) != 0 {
			t.Fatal("stale commit recreated an evicted lane")
		}
		if findLifecycleCookie(t, store.Cookies(other), "a").Value != "other" {
			t.Fatal("stale commit displaced the surviving lane")
		}
		if stats := store.Stats(); stats.Active != 1 {
			t.Fatalf("lane capacity grew beyond bound: %#v", stats)
		}

		// Same key recreated: the new instance accepts its own writes, the old
		// instance remains fenced forever (no permanent tombstones).
		newSession := store.beginPage(key)
		if newSession.generation == oldSession.generation {
			t.Fatal("recreated lane reused a stale generation")
		}
		if ok := store.commitPage(newSession, []*proto.NetworkCookie{lifecycleCookie("a", "new")}); !ok {
			t.Fatal("recreated lane rejected its current page")
		}
		if findLifecycleCookie(t, store.Cookies(key), "a").Value != "new" {
			t.Fatal("recreated lane did not persist its session")
		}
		if ok := store.commitPage(oldSession, []*proto.NetworkCookie{lifecycleCookie("a", "zombie")}); ok {
			t.Fatal("evicted session wrote into the recreated lane")
		}
		if findLifecycleCookie(t, store.Cookies(key), "a").Value != "new" {
			t.Fatal("zombie commit overwrote the new instance")
		}
		if stats := store.Stats(); stats.Active != 1 {
			t.Fatalf("tombstones leaked into lane capacity: %#v", stats)
		}
	})

	t.Run("full_save_replaces_and_invalidates_snapshots", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{lifecycleCookie("a", "0")})
		session := store.beginPage(key)

		store.SaveCookies(key, []*proto.NetworkCookie{lifecycleCookie("a", "managed")})
		if ok := store.commitPage(session, []*proto.NetworkCookie{lifecycleCookie("a", "page")}); ok {
			t.Fatal("manager replace must invalidate earlier page snapshots")
		}
		if findLifecycleCookie(t, store.Cookies(key), "a").Value != "managed" {
			t.Fatal("managed state was overwritten by a stale page")
		}
	})

	t.Run("partition_key_and_attributes_are_part_of_identity_and_diff", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		base := lifecycleCookie("a", "0")
		partitioned := lifecycleCookie("a", "0")
		partitioned.PartitionKey = &proto.NetworkCookiePartitionKey{
			TopLevelSite:         "https://partitioned.example.invalid",
			HasCrossSiteAncestor: true,
		}
		store.SaveCookies(key, []*proto.NetworkCookie{base, partitioned})
		session := store.beginPage(key)

		// Same name/domain/path, different partition key: two distinct entries.
		finalBase := lifecycleCookie("a", "1")
		finalPartitioned := lifecycleCookie("a", "0")
		finalPartitioned.PartitionKey = &proto.NetworkCookiePartitionKey{
			TopLevelSite:         "https://partitioned.example.invalid",
			HasCrossSiteAncestor: true,
		}
		finalPartitioned.Secure = true // restorable attribute change
		if ok := store.commitPage(session, []*proto.NetworkCookie{finalBase, finalPartitioned}); !ok {
			t.Fatal("partitioned commit rejected")
		}

		got := store.Cookies(key)
		if len(got) != 2 {
			t.Fatalf("partition key must distinguish cookies, got %d", len(got))
		}
		var sawSecure bool
		for _, cookie := range got {
			if cookie.PartitionKey != nil && cookie.Secure {
				sawSecure = true
			}
		}
		if !sawSecure {
			t.Fatal("restorable attribute change was not persisted")
		}

		// A read-only CDP statistic (Size) changing alone is not a page edit.
		quiet := store.beginPage(key)
		sizeOnly := lifecycleCookie("a", "1")
		sizeOnly.Size = 9999
		plain := lifecycleCookie("a", "0")
		plain.PartitionKey = partitioned.PartitionKey
		plain.Secure = true
		plain.Size = 4242
		if ok := store.commitPage(quiet, []*proto.NetworkCookie{sizeOnly, plain}); !ok {
			t.Fatal("size-only commit rejected")
		}
		for _, cookie := range store.Cookies(key) {
			if cookie.PartitionKey == nil && cookie.Size == 9999 {
				t.Fatal("CDP-only size statistic was treated as a modification")
			}
		}
	})

	t.Run("cross_domain_and_path_cookies_survive_merge", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		session := store.beginPage(key)
		final := []*proto.NetworkCookie{
			lifecycleCookieWith("root", "1", "example.invalid", "/"),
			lifecycleCookieWith("nested", "2", "shop.example.invalid", "/cart"),
			lifecycleCookieWith("redirect", "3", "login.elsewhere.example", "/"),
		}
		if ok := store.commitPage(session, final); !ok {
			t.Fatal("commit rejected")
		}
		if len(store.Cookies(key)) != 3 {
			t.Fatal("redirect/other-path cookies must be retained, not URL-filtered away")
		}
	})

	t.Run("concurrent_pages_never_share_mutable_objects", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		seed := lifecycleCookie("a", "0")
		store.SaveCookies(key, []*proto.NetworkCookie{seed})

		session := store.beginPage(key)
		// Mutating the caller's object after beginPage must not change the snapshot.
		seed.Value = "mutated"
		snap, ok := session.snapshot[cookieIdentity(lifecycleCookie("a", "0"))]
		if !ok || snap.Value != "0" {
			t.Fatal("beginPage did not take an independent snapshot")
		}

		returned := store.Cookies(key)
		returned[0].Value = "mutated-too"
		if findLifecycleCookie(t, store.Cookies(key), "a").Value != "0" {
			t.Fatal("Cookies returned a shared mutable object")
		}
	})
}

// newLifecycleBrowser builds a transport-backed Browser wired to a real
// LaneStore, exercising the production Navigate/close code without Chrome.
func newLifecycleBrowser(t *testing.T, store *LaneStore, transport *cdptest.Transport, leaveOpen bool) *Browser {
	t.Helper()
	browser := NewBrowserWithCDPClient(BrowserOpts{
		IsHeadless:     true,
		Timeout:        5 * time.Second,
		WaitLoadTime:   time.Second,
		ProxyLaneStore: store,
		LeavePageOpen:  leaveOpen,
	}, transport)
	t.Cleanup(func() {
		globalLanePageRegistry.mu.Lock()
		leaked := len(globalLanePageRegistry.pages)
		globalLanePageRegistry.mu.Unlock()
		if leaked != 0 {
			t.Errorf("test ended with %d leaked lane page registrations", leaked)
		}
	})
	return browser
}

func lifecycleNavigateCtx(key ProxyLaneKey) context.Context {
	ctx := WithEngine(context.Background(), key.Engine)
	return WithProxyLaneKey(ctx, key)
}

// TestLaneCookieLifecycleBrowser drives the real Navigate and teardown entry
// points through a controlled CDP transport.
func TestLaneCookieLifecycleBrowser(t *testing.T) {
	const pageURL = "https://source.example.invalid/search"

	t.Run("restore_then_full_context_read_at_close", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{
			lifecycleCookieWith("session", "seed", "source.example.invalid", "/"),
		})
		transport := cdptest.NewTransport()
		browser := newLifecycleBrowser(t, store, transport, false)

		page, err := browser.Navigate(lifecycleNavigateCtx(key), pageURL)
		if err != nil {
			t.Fatalf("navigate: %v", err)
		}
		target := transport.LastTarget()
		restored := transport.RestoredCookies(target.SessionID)
		if len(restored) != 1 || restored[0].Name != "session" || restored[0].Value != "seed" {
			t.Fatalf("snapshot was not restored into the new context: %#v", restored)
		}

		// In-tab work (form submission, pagination) produces more cookies,
		// including other domains and paths, before the page finishes.
		transport.SetFinalCookies(target.SessionID, []*proto.NetworkCookie{
			lifecycleCookieWith("session", "updated", "source.example.invalid", "/"),
			lifecycleCookieWith("cart", "1", "shop.example.invalid", "/cart"),
		})
		if err := ClosePageWithTimeout(context.Background(), page, time.Second); err != nil {
			t.Fatalf("close: %v", err)
		}

		getCalls := transport.Calls("Network.getCookies")
		if len(getCalls) != 1 {
			t.Fatalf("expected one full-context cookie read, got %d", len(getCalls))
		}
		// The read must not filter by the initial URL, or cross-domain/path
		// cookies would be interpreted as deletions.
		var req proto.NetworkGetCookies
		if err := jsonUnmarshalCall(getCalls[0].Params, &req); err != nil {
			t.Fatal(err)
		}
		if len(req.Urls) != 0 {
			t.Fatalf("full context read must not carry URL filters: %#v", req.Urls)
		}

		got := store.Cookies(key)
		if findLifecycleCookie(t, got, "session").Value != "updated" {
			t.Fatal("in-tab cookie update did not reach the lane")
		}
		if findLifecycleCookie(t, got, "cart").Domain != "shop.example.invalid" {
			t.Fatal("cross-path cookie was dropped")
		}
		if transport.GetCookiesCalls() != 1 {
			t.Fatal("page settled more than once")
		}
		// Settlement happened before the context was disposed.
		assertCookieReadBeforeDispose(t, transport)
	})

	t.Run("duplicate_close_settles_once", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		transport := cdptest.NewTransport()
		browser := newLifecycleBrowser(t, store, transport, false)

		page, err := browser.Navigate(lifecycleNavigateCtx(key), pageURL)
		if err != nil {
			t.Fatalf("navigate: %v", err)
		}
		target := transport.LastTarget()
		transport.SetFinalCookies(target.SessionID, []*proto.NetworkCookie{lifecycleCookie("a", "1")})

		if err := browser.ClosePage(context.Background(), page, time.Second); err != nil {
			t.Fatalf("first close: %v", err)
		}
		// A timeout copy or another Browser value must not settle it again.
		if err := ClosePageWithTimeout(context.Background(), page, time.Second); err != nil {
			t.Fatalf("second close: %v", err)
		}
		if transport.GetCookiesCalls() != 1 {
			t.Fatalf("page was settled %d times", transport.GetCookiesCalls())
		}
		if len(store.Cookies(key)) != 1 {
			t.Fatal("merge did not happen exactly once")
		}
	})

	t.Run("cookie_read_failure_discards_edits_and_releases_registration", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{lifecycleCookie("a", "0")})
		transport := cdptest.NewTransport()
		browser := newLifecycleBrowser(t, store, transport, false)

		page, err := browser.Navigate(lifecycleNavigateCtx(key), pageURL)
		if err != nil {
			t.Fatalf("navigate: %v", err)
		}
		transport.FailMethod("Network.getCookies", errors.New("target closed"))

		if err := ClosePageWithTimeout(context.Background(), page, time.Second); err != nil {
			// Bounded cleanup still runs; close errors on a dead target are tolerated.
			t.Logf("close returned: %v", err)
		}
		got := store.Cookies(key)
		if len(got) != 1 || got[0].Value != "0" {
			t.Fatalf("failed cookie read must discard page edits: %#v", got)
		}
		// Registration was released: a later close does not attempt another read.
		before := transport.GetCookiesCalls()
		_ = ClosePageWithTimeout(context.Background(), page, time.Second)
		if transport.GetCookiesCalls() != before {
			t.Fatal("failed settlement was not released")
		}
	})

	t.Run("canceled_request_never_publishes", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{lifecycleCookie("a", "0")})
		transport := cdptest.NewTransport()
		browser := newLifecycleBrowser(t, store, transport, false)

		ctx, cancel := context.WithCancel(lifecycleNavigateCtx(key))
		page, err := browser.Navigate(ctx, pageURL)
		if err != nil {
			t.Fatalf("navigate: %v", err)
		}
		target := transport.LastTarget()
		transport.SetFinalCookies(target.SessionID, []*proto.NetworkCookie{lifecycleCookie("a", "canceled")})
		cancel()

		DeferClosePage(ctx, page, browser)()
		got := store.Cookies(key)
		if len(got) != 1 || got[0].Value != "0" {
			t.Fatalf("canceled request published its edits: %#v", got)
		}
	})

	t.Run("late_cookie_reply_after_barrier_is_discarded", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{lifecycleCookie("a", "0")})
		transport := cdptest.NewTransport()
		browser := newLifecycleBrowser(t, store, transport, false)

		page, err := browser.Navigate(lifecycleNavigateCtx(key), pageURL)
		if err != nil {
			t.Fatalf("navigate: %v", err)
		}
		target := transport.LastTarget()
		transport.SetFinalCookies(target.SessionID, []*proto.NetworkCookie{lifecycleCookie("a", "in-flight")})
		entered, release := transport.BlockGetCookies()

		settled := make(chan struct{})
		go func() {
			_ = ClosePageWithTimeout(context.Background(), page, time.Second*30)
			close(settled)
		}()
		<-entered

		// While the read is in flight, a challenge clears the lane.
		store.DropCookies(key)
		close(release)
		<-settled

		if got := store.Cookies(key); len(got) != 0 {
			t.Fatalf("late CDP reply resurrected a dropped lane: %#v", got)
		}
	})

	t.Run("leave_page_open_settles_but_keeps_tab", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		transport := cdptest.NewTransport()
		browser := newLifecycleBrowser(t, store, transport, true)

		page, err := browser.Navigate(lifecycleNavigateCtx(key), pageURL)
		if err != nil {
			t.Fatalf("navigate: %v", err)
		}
		target := transport.LastTarget()
		transport.SetFinalCookies(target.SessionID, []*proto.NetworkCookie{lifecycleCookie("a", "kept-tab")})

		DeferClosePage(context.Background(), page, browser)()

		if findLifecycleCookie(t, store.Cookies(key), "a").Value != "kept-tab" {
			t.Fatal("LeavePageOpen must still settle the session snapshot")
		}
		if len(transport.ClosedTargets()) != 0 {
			t.Fatal("LeavePageOpen must not close the tab")
		}
		if len(transport.DisposedContexts()) != 0 {
			t.Fatal("LeavePageOpen must not dispose the context")
		}
		// Explicit close afterwards performs cleanup without a second settlement.
		if err := ClosePageWithTimeout(context.Background(), page, time.Second); err != nil {
			t.Fatalf("explicit close: %v", err)
		}
		if transport.GetCookiesCalls() != 1 {
			t.Fatal("snapshot settled again after the tab was kept open")
		}
		if len(transport.ClosedTargets()) != 1 || len(transport.DisposedContexts()) != 1 {
			t.Fatal("explicit close did not tear down the kept tab")
		}
	})

	t.Run("navigation_init_failure_leaves_no_registration", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		transport := cdptest.NewTransport()
		transport.FailMethod("Target.createTarget", errors.New("browser crashed"))
		browser := newLifecycleBrowser(t, store, transport, false)

		page, err := browser.Navigate(lifecycleNavigateCtx(key), pageURL)
		if err == nil {
			t.Fatal("expected navigation failure")
			_ = page
		}
		globalLanePageRegistry.mu.Lock()
		leaked := len(globalLanePageRegistry.pages)
		globalLanePageRegistry.mu.Unlock()
		if leaked != 0 {
			t.Fatalf("failed init leaked %d lane registrations", leaked)
		}
	})

	t.Run("navigation_failure_with_leave_open_releases_snapshot", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{lifecycleCookie("a", "0")})
		transport := cdptest.NewTransport()
		transport.FailMethod("Page.navigate", errors.New("net::ERR_FAILED"))
		browser := newLifecycleBrowser(t, store, transport, true)

		page, err := browser.Navigate(lifecycleNavigateCtx(key), pageURL)
		if err == nil {
			t.Fatal("expected navigation failure")
			_ = page
		}
		globalLanePageRegistry.mu.Lock()
		leaked := len(globalLanePageRegistry.pages)
		globalLanePageRegistry.mu.Unlock()
		if leaked != 0 {
			t.Fatalf("failed navigation with LeavePageOpen leaked %d registrations", leaked)
		}
		// The pre-existing lane state is untouched by a failed navigation.
		if findLifecycleCookie(t, store.Cookies(key), "a").Value != "0" {
			t.Fatal("failed navigation mutated the lane")
		}
	})

	t.Run("page_without_lane_keeps_isolation", func(t *testing.T) {
		store := NewLaneStore(10)
		transport := cdptest.NewTransport()
		browser := newLifecycleBrowser(t, store, transport, false)

		page, err := browser.Navigate(context.Background(), pageURL)
		if err != nil {
			t.Fatalf("navigate: %v", err)
		}
		if err := ClosePageWithTimeout(context.Background(), page, time.Second); err != nil {
			t.Fatalf("close: %v", err)
		}
		if len(transport.Calls("Network.setCookies")) != 0 {
			t.Fatal("lane-less page must not restore cookies")
		}
		if transport.GetCookiesCalls() != 0 {
			t.Fatal("lane-less page must not settle cookies")
		}
		if len(transport.ClosedTargets()) != 1 || len(transport.DisposedContexts()) != 1 {
			t.Fatal("lane-less page lost its normal cleanup")
		}
	})

	t.Run("browser_close_releases_unsettled_pages", func(t *testing.T) {
		store := NewLaneStore(10)
		key := lifecycleKey()
		store.SaveCookies(key, []*proto.NetworkCookie{lifecycleCookie("a", "0")})
		transport := cdptest.NewTransport()
		browser := newLifecycleBrowser(t, store, transport, false)

		page, err := browser.Navigate(lifecycleNavigateCtx(key), pageURL)
		if err != nil {
			t.Fatalf("navigate: %v", err)
		}
		target := transport.LastTarget()
		transport.SetFinalCookies(target.SessionID, []*proto.NetworkCookie{lifecycleCookie("a", "orphan")})

		if err := browser.Close(); err != nil {
			t.Fatalf("browser close: %v", err)
		}
		// The orphaned page can no longer write back.
		_ = ClosePageWithTimeout(context.Background(), page, time.Second)
		got := store.Cookies(key)
		if len(got) != 1 || got[0].Value != "0" {
			t.Fatalf("browser shutdown did not release unsettled pages: %#v", got)
		}
	})
}

func jsonUnmarshalCall(raw []byte, out interface{}) error {
	if err := json.Unmarshal(raw, out); err != nil {
		return fmt.Errorf("unmarshal CDP params: %w", err)
	}
	return nil
}

func assertCookieReadBeforeDispose(t *testing.T, transport *cdptest.Transport) {
	t.Helper()
	readIndex, disposeIndex := -1, -1
	for i, call := range transport.Calls() {
		switch call.Method {
		case "Network.getCookies":
			readIndex = i
		case "Target.disposeBrowserContext":
			disposeIndex = i
		}
	}
	if readIndex < 0 {
		t.Fatal("no cookie read observed")
	}
	if disposeIndex < 0 {
		t.Fatal("no context disposal observed")
	}
	if readIndex > disposeIndex {
		t.Fatal("cookies were read after the browser context was disposed")
	}
}

// Compile-time guard that concurrent sessions and the registry are safe under
// the race detector; correctness itself is asserted in the subtests above.
func TestLaneCookieLifecycleRegistryNoSharedState(t *testing.T) {
	store := NewLaneStore(10)
	key := lifecycleKey()
	var wg sync.WaitGroup
	for i := 0; i < 16; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			session := store.beginPage(key)
			value := fmt.Sprintf("v-%d", i)
			store.commitPage(session, []*proto.NetworkCookie{lifecycleCookie("a", value)})
		}(i)
	}
	wg.Wait()
}
