package core

import (
	"context"
	"fmt"
	"sync"
	"sync/atomic"
	"time"

	"github.com/go-rod/rod"
	"github.com/go-rod/rod/lib/proto"
)

// cookieKey identifies a cookie inside one lane. Chromium scopes cookies by
// name, domain and path, and partitioned cookies additionally carry their
// partition key, so two cookies that differ only there are distinct entries.
type cookieKey struct {
	Name                  string
	Domain                string
	Path                  string
	Partitioned           bool
	PartitionTopLevelSite string
	PartitionCrossSite    bool
	PartitionOpaque       bool
}

func cookieIdentity(cookie *proto.NetworkCookie) cookieKey {
	key := cookieKey{
		Name:            cookie.Name,
		Domain:          cookie.Domain,
		Path:            cookie.Path,
		PartitionOpaque: cookie.PartitionKeyOpaque,
	}
	if cookie.PartitionKey != nil {
		key.Partitioned = true
		key.PartitionTopLevelSite = cookie.PartitionKey.TopLevelSite
		key.PartitionCrossSite = cookie.PartitionKey.HasCrossSiteAncestor
	}
	return key
}

// cookieRestorableEqual reports whether the attributes that can be restored
// via Network.setCookie match. CDP-only statistics such as Size, and derived
// flags such as Session, are not page edits and must not count as changes.
func cookieRestorableEqual(a, b *proto.NetworkCookie) bool {
	return a.Value == b.Value &&
		a.Secure == b.Secure &&
		a.HTTPOnly == b.HTTPOnly &&
		a.SameSite == b.SameSite &&
		a.Expires == b.Expires &&
		a.Priority == b.Priority &&
		a.SameParty == b.SameParty &&
		a.SourceScheme == b.SourceScheme &&
		a.SourcePort == b.SourcePort
}

func cloneCookie(cookie *proto.NetworkCookie) *proto.NetworkCookie {
	if cookie == nil {
		return nil
	}
	cloned := *cookie
	if cookie.PartitionKey != nil {
		partition := *cookie.PartitionKey
		cloned.PartitionKey = &partition
	}
	return &cloned
}

func cookiesFromMap(cookies map[cookieKey]*proto.NetworkCookie) []*proto.NetworkCookie {
	if len(cookies) == 0 {
		return nil
	}
	out := make([]*proto.NetworkCookie, 0, len(cookies))
	for _, cookie := range cookies {
		out = append(out, cloneCookie(cookie))
	}
	return out
}

// lanePageSession is one page's private snapshot of a lane, taken when the
// page starts. Commits are accepted only while the lane generation still
// matches the one captured here: a DropCookies, a full SaveCookies replace,
// or an LRU eviction plus recreation fences off every older session.
type lanePageSession struct {
	store      *LaneStore
	key        ProxyLaneKey
	generation uint64
	snapshot   map[cookieKey]*proto.NetworkCookie
}

// lanePageRegistration ties a live rod target to its lane session. The
// package-level registry is keyed by target id, so Browser value copies and
// page clones (page.Context/Timeout produce new *Page values) all resolve the
// same registration, and a page can be settled exactly once.
type lanePageRegistration struct {
	ownerID    uint64
	key        ProxyLaneKey
	session    *lanePageSession
	requestCtx context.Context
	done       atomic.Bool
}

type lanePageRegistry struct {
	mu    sync.Mutex
	pages map[proto.TargetTargetID]*lanePageRegistration
}

func newLanePageRegistry() *lanePageRegistry {
	return &lanePageRegistry{pages: map[proto.TargetTargetID]*lanePageRegistration{}}
}

var globalLanePageRegistry = newLanePageRegistry()

var laneOwnerCounter atomic.Uint64

func nextLaneOwnerID() uint64 {
	return laneOwnerCounter.Add(1)
}

// beginLanePage snapshots the lane for this page and registers the target so
// that the later close entry point can settle it. Pages without a lane store
// or lane key keep their previous fully isolated behavior.
func (b *Browser) beginLanePage(ctx context.Context, page *rod.Page) {
	if b == nil || b.ProxyLaneStore == nil || page == nil {
		return
	}
	laneKey := proxyLaneKeyFromContext(ctx)
	if laneKey.Empty() {
		return
	}
	session := b.ProxyLaneStore.beginPage(laneKey)
	if session == nil {
		return
	}
	registration := &lanePageRegistration{
		ownerID:    b.laneOwner,
		key:        laneKey,
		session:    session,
		requestCtx: ctx,
	}
	globalLanePageRegistry.mu.Lock()
	globalLanePageRegistry.pages[page.TargetID] = registration
	globalLanePageRegistry.mu.Unlock()
}

// releaseLanePage drops an unsettled registration without writing anything.
// It is used when navigation, cookie reads, or the originating request fail.
func releaseLanePage(page *rod.Page) {
	if page == nil {
		return
	}
	globalLanePageRegistry.release(page.TargetID)
}

// laneRegistrationFor returns the live registration for a page, if any. It
// does not consume it, so Navigate can inspect the snapshot before close.
func laneRegistrationFor(page *rod.Page) *lanePageRegistration {
	if page == nil {
		return nil
	}
	globalLanePageRegistry.mu.Lock()
	defer globalLanePageRegistry.mu.Unlock()
	return globalLanePageRegistry.pages[page.TargetID]
}

// settleLanePage reads the isolated browser context's complete cookie set and
// merges the page's own delta into the lane. It is a no-op for pages that
// have no lane, and safe to invoke from every close path because each
// registration is consumed at most once.
func settleLanePage(ctx context.Context, page *rod.Page) {
	if page == nil {
		return
	}
	registration, ok := globalLanePageRegistry.take(page.TargetID)
	if !ok {
		return
	}
	// A canceled originating request never gets to publish its edits.
	if registration.requestCtx != nil && registration.requestCtx.Err() != nil {
		return
	}
	// No URL filter: reading only the initial URL would silently delete
	// cookies created by redirects, other paths, or other domains.
	res, err := proto.NetworkGetCookies{}.Call(page.Context(EnsureContext(ctx)))
	if err != nil {
		WithRequest(EnsureContext(ctx)).WithError(err).Debug("Read lane cookies failed, discarding page edits")
		return
	}
	registration.session.store.commitPage(registration.session, res.Cookies)
}

// settleLanePageBounded gives the final cookie read its own deadline so a
// wedged Chrome target cannot stall a close path, while never inheriting the
// originating request's cancellation (a request that finished normally may
// still be using an already-expired context by the time defer runs).
func settleLanePageBounded(ctx context.Context, page *rod.Page) {
	if page == nil {
		return
	}
	if laneRegistrationFor(page) == nil {
		return
	}
	base := EnsureContext(ctx)
	if base.Err() != nil {
		base = context.Background()
	}
	settleCtx, cancel := context.WithTimeout(base, laneSettleTimeout)
	defer cancel()
	settleLanePage(settleCtx, page)
}

func (r *lanePageRegistry) take(targetID proto.TargetTargetID) (*lanePageRegistration, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	registration, ok := r.pages[targetID]
	if !ok || !registration.done.CompareAndSwap(false, true) {
		return nil, false
	}
	delete(r.pages, targetID)
	return registration, true
}

func (r *lanePageRegistry) release(targetID proto.TargetTargetID) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if registration, ok := r.pages[targetID]; ok && registration.done.CompareAndSwap(false, true) {
		delete(r.pages, targetID)
	}
}

// releaseOwner drops every unsettled page owned by one Browser instance,
// including all of its value copies. Used when the browser shuts down.
func (r *lanePageRegistry) releaseOwner(ownerID uint64) {
	r.mu.Lock()
	defer r.mu.Unlock()
	for targetID, registration := range r.pages {
		if registration.ownerID != ownerID {
			continue
		}
		if registration.done.CompareAndSwap(false, true) {
			delete(r.pages, targetID)
		}
	}
}

// restoreLaneSessionCookies replays the page's starting snapshot into the new
// isolated browser context.
func restoreLaneSessionCookies(page *rod.Page, session *lanePageSession) error {
	if page == nil || session == nil || len(session.snapshot) == 0 {
		return nil
	}
	cookies := make([]*proto.NetworkCookie, 0, len(session.snapshot))
	for _, cookie := range session.snapshot {
		cookies = append(cookies, cloneCookie(cookie))
	}
	if err := (proto.NetworkSetCookies{Cookies: cookieParams(cookies)}).Call(page); err != nil {
		return fmt.Errorf("restore lane cookies: %w", err)
	}
	return nil
}

// laneSettleTimeout bounds the final cookie read so shutdown paths cannot hang
// waiting on an unresponsive Chrome target.
const laneSettleTimeout = 5 * time.Second
