package rss

import (
	"bytes"
	"context"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"time"
)

// ErrSubscriptionBusy indicates that a [Subscription.Subscribe] or
// [Subscription.Resync] call could not start because another call of
// the same method is still in progress on the same subscription.
var ErrSubscriptionBusy = errors.New("rss: subscription operation already in progress")

// maxNotificationBody is the largest WebSub notification body a
// [Subscription] will accept.
const maxNotificationBody = 1 << 20 // 1 MiB

// subscriptionLease records a single generation of a WebSub
// subscription: a unique callback path, its secret, and the state of
// the subscribe handshake.
type subscriptionLease struct {
	id        uint64
	path      string
	secret    []byte
	accepted  bool // The hub returned 202 to the subscribe request.
	confirmed bool // The hub verified the subscription.
	expires   time.Time
}

// notification records a successfully published WebSub notification
// so that a later [Subscription.Resync] can replay it on top of a
// full fetch that may predate it.
type notification struct {
	seq   uint64
	items []*Item
}

// Subscription is a WebSub subscriber for a single feed. It combines
// push notifications received over HTTP with full fetches of the
// feed's topic, so that deletions are eventually noticed without
// letting stale full fetches overwrite newer notifications.
//
// A Subscription is constructed with [Reader.NewSubscription] and
// implements [http.Handler]; route the hub's verification requests
// and notifications to it. The subscription's hub and topic are
// fixed at construction time and never change.
type Subscription struct {
	reader *Reader
	hub    string
	topic  string
	base   *url.URL

	mu            sync.Mutex
	feed          *Feed
	active        *subscriptionLease
	pending       *subscriptionLease
	retired       map[string]struct{}
	nextLease     uint64
	notifySeq     uint64
	notifications []notification
	subscribing   bool
	resyncing     bool
}

// NewSubscription creates a [Subscription] for a feed that advertises
// WebSub. The initial feed must contain exactly one link with
// rel="hub" and exactly one with rel="self" in its Links, as produced
// by [Reader.Fetch] or [Reader.Parse] for feeds that declare them.
// Both links are resolved against the feed's UpdateURL; the hub, the
// resolved self link, and callbackBase must all be absolute HTTP(S)
// URLs without userinfo or fragments, and callbackBase must not carry
// a query string.
//
// NewSubscription performs no network activity. It clones initial, so
// later changes to initial do not affect the subscription, and binds
// the subscription's topic to the resolved self link.
func (r *Reader) NewSubscription(initial *Feed, callbackBase string) (*Subscription, error) {
	if initial == nil {
		return nil, errors.New("rss: cannot subscribe to a nil feed")
	}

	var hubHref, selfHref string
	for _, link := range initial.Links {
		if link == nil {
			continue
		}
		switch link.Rel {
		case "hub":
			if hubHref != "" {
				return nil, errors.New("rss: feed has multiple hub links")
			}
			hubHref = link.Href
		case "self":
			if selfHref != "" {
				return nil, errors.New("rss: feed has multiple self links")
			}
			selfHref = link.Href
		}
	}
	if hubHref == "" || selfHref == "" {
		return nil, errors.New("rss: feed must have exactly one hub link and one self link")
	}

	base, err := url.Parse(initial.UpdateURL)
	if err != nil {
		return nil, fmt.Errorf("rss: invalid feed update URL: %v", err)
	}

	hub, err := resolveSubscriptionURL(base, hubHref)
	if err != nil {
		return nil, fmt.Errorf("rss: invalid hub link: %v", err)
	}
	self, err := resolveSubscriptionURL(base, selfHref)
	if err != nil {
		return nil, fmt.Errorf("rss: invalid self link: %v", err)
	}

	callback, err := url.Parse(callbackBase)
	if err != nil {
		return nil, fmt.Errorf("rss: invalid callback base: %v", err)
	}
	if !validSubscriptionURL(callback) || callback.RawQuery != "" {
		return nil, fmt.Errorf("rss: invalid callback base %q: must be an absolute HTTP(S) URL without userinfo, query, or fragment", callbackBase)
	}

	feed := cloneFeed(initial)
	feed.UpdateURL = self

	return &Subscription{
		reader:  r,
		hub:     hub,
		topic:   self,
		base:    callback,
		feed:    feed,
		retired: make(map[string]struct{}),
	}, nil
}

// resolveSubscriptionURL resolves href against base and requires the
// result to be an absolute HTTP(S) URL without userinfo or fragment.
func resolveSubscriptionURL(base *url.URL, href string) (string, error) {
	ref, err := url.Parse(href)
	if err != nil {
		return "", err
	}
	u := base.ResolveReference(ref)
	if !validSubscriptionURL(u) {
		return "", fmt.Errorf("%q is not an absolute HTTP(S) URL without userinfo or fragment", u)
	}
	return u.String(), nil
}

// validSubscriptionURL reports whether u is an absolute HTTP(S) URL
// without userinfo or fragment.
func validSubscriptionURL(u *url.URL) bool {
	return u != nil && u.IsAbs() && u.Opaque == "" &&
		(u.Scheme == "http" || u.Scheme == "https") &&
		u.Host != "" && u.User == nil && u.Fragment == ""
}

// Subscribe asks the hub to deliver notifications for the
// subscription's topic. It is used both for the initial subscription
// and to renew an existing lease; every call generates a fresh random
// callback path and secret, appended to the callback base URL given
// to [Reader.NewSubscription].
//
// Subscribe returns once the hub has accepted the request with a 202
// response; it does not wait for the hub's asynchronous verification.
// The new generation only becomes active once both the acceptance and
// the verification have completed; until then any previous lease
// remains in effect. A failed renewal never revokes the previous
// lease, and a new call discards any candidate that has not completed
// the handshake.
//
// If another Subscribe call is already in progress on this
// subscription, Subscribe returns [ErrSubscriptionBusy].
func (s *Subscription) Subscribe(ctx context.Context) error {
	if ctx == nil {
		return errors.New("rss: nil context")
	}
	if err := ctx.Err(); err != nil {
		return err
	}

	s.mu.Lock()
	if s.subscribing {
		s.mu.Unlock()
		return ErrSubscriptionBusy
	}
	s.subscribing = true
	if s.pending != nil {
		s.retired[s.pending.path] = struct{}{}
		s.pending = nil
	}
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		s.subscribing = false
		s.mu.Unlock()
	}()

	var token [16]byte
	if _, err := rand.Read(token[:]); err != nil {
		return fmt.Errorf("rss: failed to generate callback path: %v", err)
	}
	callback := *s.base
	callback.Path = strings.TrimSuffix(callback.Path, "/") + "/" + hex.EncodeToString(token[:])
	callback.RawPath = ""

	var secret [32]byte
	if _, err := rand.Read(secret[:]); err != nil {
		return fmt.Errorf("rss: failed to generate secret: %v", err)
	}

	lease := &subscriptionLease{
		path:   callback.Path,
		secret: secret[:],
	}
	s.mu.Lock()
	s.nextLease++
	lease.id = s.nextLease
	s.pending = lease
	s.mu.Unlock()

	form := url.Values{}
	form.Set("hub.mode", "subscribe")
	form.Set("hub.topic", s.topic)
	form.Set("hub.callback", callback.String())
	form.Set("hub.secret", string(secret[:]))
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, s.hub, strings.NewReader(form.Encode()))
	if err != nil {
		return fmt.Errorf("rss: failed to construct subscribe request: %v", err)
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")

	res, err := s.reader.http.Do(req)
	if err != nil {
		return fmt.Errorf("rss: failed to request subscription: %v", err)
	}
	if res.StatusCode != http.StatusAccepted {
		io.Copy(io.Discard, res.Body)
		res.Body.Close()
		return fmt.Errorf("rss: hub refused subscription: %s", res.Status)
	}
	if _, err := io.Copy(io.Discard, res.Body); err != nil {
		res.Body.Close()
		return fmt.Errorf("rss: failed to read hub response: %v", err)
	}
	if err := res.Body.Close(); err != nil {
		return fmt.Errorf("rss: failed to close hub response: %v", err)
	}

	s.mu.Lock()
	lease.accepted = true
	s.activateLocked(lease)
	s.mu.Unlock()

	return nil
}

// activateLocked promotes lease to the active generation once its
// subscribe request has been both accepted and verified. The caller
// must hold s.mu.
func (s *Subscription) activateLocked(lease *subscriptionLease) {
	if !lease.accepted || !lease.confirmed || s.active == lease {
		return
	}
	if s.active != nil {
		s.retired[s.active.path] = struct{}{}
	}
	s.active = lease
	if s.pending == lease {
		s.pending = nil
	}
}

// Resync replaces the subscription's state with a full fetch of the
// topic, then replays on top of it every notification that was
// successfully received since the fetch began. This is how deleted
// items are noticed: the full fetch replaces the item membership and
// order, while notifications received in the meantime are not lost.
// Items keep their local read state, including SetRead calls made
// while the fetch was in flight.
//
// Resync ignores the feed's NextUpdate time. If the fetch fails or
// the context is cancelled, the previous state is kept untouched and
// a later Resync can try again. If another Resync call is already in
// progress on this subscription, Resync returns [ErrSubscriptionBusy]
// without making any request.
func (s *Subscription) Resync(ctx context.Context) error {
	if ctx == nil {
		return errors.New("rss: nil context")
	}
	if err := ctx.Err(); err != nil {
		return err
	}

	s.mu.Lock()
	if s.resyncing {
		s.mu.Unlock()
		return ErrSubscriptionBusy
	}
	s.resyncing = true
	boundary := s.notifySeq
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		s.resyncing = false
		s.mu.Unlock()
	}()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, s.topic, nil)
	if err != nil {
		return fmt.Errorf("rss: failed to construct topic request: %v", err)
	}
	res, err := s.reader.http.Do(req)
	if err != nil {
		return fmt.Errorf("rss: failed to fetch topic: %v", err)
	}
	if res.StatusCode != http.StatusOK {
		io.Copy(io.Discard, res.Body)
		res.Body.Close()
		return fmt.Errorf("rss: failed to fetch topic: %s", res.Status)
	}
	feed, err := s.reader.Parse(s.topic, res.Body)
	if err != nil {
		io.Copy(io.Discard, res.Body)
		res.Body.Close()
		return fmt.Errorf("rss: failed to parse topic: %v", err)
	}
	if _, err := io.Copy(io.Discard, res.Body); err != nil {
		res.Body.Close()
		return fmt.Errorf("rss: failed to read topic body: %v", err)
	}
	if err := res.Body.Close(); err != nil {
		return fmt.Errorf("rss: failed to close topic body: %v", err)
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	read := s.applyFeedLocked(feed)
	for _, n := range s.notifications {
		if n.seq > boundary {
			s.mergeItemsLocked(n.items, read)
		}
	}
	s.notifications = nil
	s.recomputeUnreadLocked()
	return nil
}

// applyFeedLocked replaces the subscription's state with a full
// fetch, keeping local read state and the fixed topic URL. It returns
// the previous read state by item ID. The caller must hold s.mu.
func (s *Subscription) applyFeedLocked(feed *Feed) map[string]bool {
	read := make(map[string]bool, len(s.feed.Items))
	for _, item := range s.feed.Items {
		read[item.ID] = item.Read
	}
	for _, item := range feed.Items {
		item.Read = read[item.ID]
	}
	feed.UpdateURL = s.topic
	s.feed = feed
	return read
}

// mergeItemsLocked merges notification items into the current state:
// existing items are revised in place by ID, new items are appended,
// and absent items are kept. Local read state wins: items already
// present keep their read state, and new items fall back to read
// (a nil map means unread). The caller must hold s.mu.
func (s *Subscription) mergeItemsLocked(items []*Item, read map[string]bool) {
	index := make(map[string]int, len(s.feed.Items))
	for i, item := range s.feed.Items {
		index[item.ID] = i
	}
	for _, item := range items {
		if item == nil {
			continue
		}
		if i, ok := index[item.ID]; ok {
			item.Read = s.feed.Items[i].Read
			s.feed.Items[i] = item
		} else {
			index[item.ID] = len(s.feed.Items)
			item.Read = read[item.ID]
			s.feed.Items = append(s.feed.Items, item)
		}
	}
}

// recomputeUnreadLocked recalculates the feed's unread count. The
// caller must hold s.mu.
func (s *Subscription) recomputeUnreadLocked() {
	var unread uint32
	for _, item := range s.feed.Items {
		if !item.Read {
			unread++
		}
	}
	s.feed.Unread = unread
}

// Snapshot returns a copy of the subscription's current state. The
// result is fully detached from the subscription: modifying it has
// no effect on later snapshots.
func (s *Subscription) Snapshot() *Feed {
	s.mu.Lock()
	defer s.mu.Unlock()
	return cloneFeed(s.feed)
}

// SetRead marks the item with the given ID as read or unread and
// recomputes the feed's unread count. It reports whether an item
// with that ID exists.
func (s *Subscription) SetRead(id string, read bool) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, item := range s.feed.Items {
		if item.ID == id {
			item.Read = read
			s.recomputeUnreadLocked()
			return true
		}
	}
	return false
}

// ServeHTTP receives the hub's verification requests (GET) and
// notifications (POST). Route every request under the callback base
// URL given to [Reader.NewSubscription] to this handler.
func (s *Subscription) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		s.serveVerify(w, r)
	case http.MethodPost:
		s.serveNotification(w, r)
	default:
		w.Header().Set("Allow", "GET, POST")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

// serveVerify handles the hub's subscription verification. The
// challenge is echoed only if the path belongs to the pending or
// active generation and every hub parameter is valid. The lease
// starts at the first valid verification; later verifications only
// echo the challenge again.
func (s *Subscription) serveVerify(w http.ResponseWriter, r *http.Request) {
	query := r.URL.Query()
	challenge := query.Get("hub.challenge")
	leaseSeconds, leaseErr := strconv.Atoi(query.Get("hub.lease_seconds"))

	s.mu.Lock()
	var lease *subscriptionLease
	if s.pending != nil && r.URL.Path == s.pending.path {
		lease = s.pending
	} else if s.active != nil && r.URL.Path == s.active.path {
		lease = s.active
	} else if _, ok := s.retired[r.URL.Path]; ok {
		s.mu.Unlock()
		http.Error(w, "subscription generation retired", http.StatusGone)
		return
	} else {
		s.mu.Unlock()
		http.NotFound(w, r)
		return
	}

	valid := query.Get("hub.mode") == "subscribe" &&
		query.Get("hub.topic") == s.topic &&
		challenge != "" &&
		leaseErr == nil && leaseSeconds > 0
	if !valid {
		s.mu.Unlock()
		http.Error(w, "invalid verification request", http.StatusBadRequest)
		return
	}

	if !lease.confirmed {
		lease.confirmed = true
		lease.expires = s.reader.now().Add(time.Duration(leaseSeconds) * time.Second)
	}
	s.activateLocked(lease)
	s.mu.Unlock()

	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.WriteHeader(http.StatusOK)
	io.WriteString(w, challenge)
}

// serveNotification handles a notification from the hub. The body is
// read in full and its X-Hub-Signature checked against the
// generation's secret before it is parsed and merged into the
// subscription's state.
func (s *Subscription) serveNotification(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	var lease *subscriptionLease
	status := http.StatusNotFound
	switch {
	case s.active != nil && r.URL.Path == s.active.path:
		if s.reader.now().Before(s.active.expires) {
			lease = s.active
		} else {
			status = http.StatusGone
		}
	case s.pending != nil && r.URL.Path == s.pending.path:
		status = http.StatusConflict
	default:
		if _, ok := s.retired[r.URL.Path]; ok {
			status = http.StatusGone
		}
	}
	s.mu.Unlock()
	if lease == nil {
		http.Error(w, http.StatusText(status), status)
		return
	}

	body, err := io.ReadAll(io.LimitReader(r.Body, maxNotificationBody+1))
	if err != nil {
		r.Body.Close()
		http.Error(w, "failed to read notification", http.StatusBadRequest)
		return
	}
	if len(body) > maxNotificationBody {
		r.Body.Close()
		http.Error(w, "notification too large", http.StatusRequestEntityTooLarge)
		return
	}
	if err := r.Body.Close(); err != nil {
		http.Error(w, "failed to close notification", http.StatusBadRequest)
		return
	}

	if !validSignature(r.Header.Get("X-Hub-Signature"), lease.secret, body) {
		http.Error(w, "invalid notification signature", http.StatusForbidden)
		return
	}

	feed, err := s.reader.Parse(s.topic, bytes.NewReader(body))
	if err != nil {
		http.Error(w, "failed to parse notification", http.StatusBadRequest)
		return
	}

	s.mu.Lock()
	defer s.mu.Unlock()
	if s.active != lease || !s.reader.now().Before(lease.expires) {
		http.Error(w, "subscription generation retired", http.StatusGone)
		return
	}
	s.notifySeq++
	s.notifications = append(s.notifications, notification{seq: s.notifySeq, items: feed.Items})
	s.mergeItemsLocked(feed.Items, nil)
	s.recomputeUnreadLocked()
	w.WriteHeader(http.StatusNoContent)
}

// validSignature checks a WebSub X-Hub-Signature header of the form
// "sha256=<hex>" against the HMAC-SHA256 of body keyed with secret.
func validSignature(header string, secret, body []byte) bool {
	hexMAC, ok := strings.CutPrefix(header, "sha256=")
	if !ok {
		return false
	}
	mac, err := hex.DecodeString(hexMAC)
	if err != nil {
		return false
	}
	expected := hmac.New(sha256.New, secret)
	expected.Write(body)
	return hmac.Equal(mac, expected.Sum(nil))
}

// cloneFeed returns a deep copy of f.
func cloneFeed(f *Feed) *Feed {
	if f == nil {
		return nil
	}
	out := *f
	out.Author = cloneAuthor(f.Author)
	out.Links = cloneLinks(f.Links)
	out.Image = cloneImage(f.Image)
	out.Categories = append([]string(nil), f.Categories...)
	out.Items = make([]*Item, len(f.Items))
	for i, item := range f.Items {
		out.Items[i] = cloneItem(item)
	}
	return &out
}

// cloneAuthor returns a deep copy of a.
func cloneAuthor(a *Author) *Author {
	if a == nil {
		return nil
	}
	out := *a
	out.Links = cloneLinks(a.Links)
	return &out
}

// cloneLinks returns a deep copy of links.
func cloneLinks(links []*Link) []*Link {
	if links == nil {
		return nil
	}
	out := make([]*Link, len(links))
	for i, link := range links {
		if link != nil {
			copy := *link
			out[i] = &copy
		}
	}
	return out
}

// cloneImage returns a deep copy of i.
func cloneImage(i *Image) *Image {
	if i == nil {
		return nil
	}
	out := *i
	return &out
}

// cloneItem returns a deep copy of item.
func cloneItem(item *Item) *Item {
	if item == nil {
		return nil
	}
	out := *item
	out.Categories = append([]string(nil), item.Categories...)
	out.Links = cloneLinks(item.Links)
	out.Image = cloneImage(item.Image)
	if item.Enclosures != nil {
		out.Enclosures = make([]*Enclosure, len(item.Enclosures))
		for i, enclosure := range item.Enclosures {
			if enclosure != nil {
				copy := *enclosure
				out.Enclosures[i] = &copy
			}
		}
	}
	return &out
}
