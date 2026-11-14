package rss_test

import (
	"context"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	rss "github.com/SlyMarbo/rss/v2"
	"github.com/SlyMarbo/rss/v2/task_support/taskcheck"
)

const (
	testHubURL    = "https://hub.example/endpoint"
	testTopicURL  = "https://source.example/feed"
	testHub2URL   = "https://hub2.example/endpoint"
	testTopic2URL = "https://source.example/atom"
	testCallback  = "https://subscriber.example/callbacks"
)

// roundTripFunc adapts a function to http.RoundTripper.
type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

// testClock is a manually advanced clock for the reader.
type testClock struct {
	mu  sync.Mutex
	now time.Time
}

func newTestClock(now time.Time) *testClock { return &testClock{now: now} }

func (c *testClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *testClock) Advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.now = c.now.Add(d)
}

// switchboard is a RoundTripper whose GET and POST handlers can be
// swapped between phases of a test. It counts GET requests.
type switchboard struct {
	mu   sync.Mutex
	get  func(*http.Request) (*http.Response, error)
	post func(*http.Request) (*http.Response, error)
	gets atomic.Int32
}

func (s *switchboard) set(get, post func(*http.Request) (*http.Response, error)) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.get, s.post = get, post
}

func (s *switchboard) RoundTrip(req *http.Request) (*http.Response, error) {
	s.mu.Lock()
	get, post := s.get, s.post
	s.mu.Unlock()
	if req.Method == http.MethodPost {
		if post != nil {
			return post(req)
		}
		return httpResponse(req, http.StatusInternalServerError, "unexpected POST"), nil
	}
	s.gets.Add(1)
	if get != nil {
		return get(req)
	}
	return httpResponse(req, http.StatusInternalServerError, "unexpected GET"), nil
}

// httpResponse builds an in-memory response.
func httpResponse(req *http.Request, status int, body string) *http.Response {
	return &http.Response{
		StatusCode: status,
		Status:     fmt.Sprintf("%d %s", status, http.StatusText(status)),
		Header:     make(http.Header),
		Request:    req,
		Body:       io.NopCloser(strings.NewReader(body)),
	}
}

// closeCountingBody records Close calls.
type closeCountingBody struct {
	io.Reader
	closes *atomic.Int32
}

func (b *closeCountingBody) Close() error {
	b.closes.Add(1)
	return nil
}

// gatedReadCloser blocks its first read until gate is closed, and
// signals started once reading has begun.
type gatedReadCloser struct {
	started chan struct{}
	gate    <-chan struct{}
	body    *strings.Reader
	once    sync.Once
}

func (g *gatedReadCloser) Read(p []byte) (int, error) {
	g.once.Do(func() { close(g.started) })
	<-g.gate
	return g.body.Read(p)
}

func (g *gatedReadCloser) Close() error { return nil }

// Feed documents.

func rss2Item(id, title string) string {
	return `<item><guid>` + id + `</guid><title>` + title + `</title></item>`
}

func rss2FeedDoc(hub, self, title string, items ...string) string {
	return `<rss version="2.0" xmlns:atom="http://www.w3.org/2005/Atom"><channel>` +
		`<title>` + title + `</title>` +
		`<atom:link rel="hub" href="` + hub + `"/>` +
		`<atom:link rel="self" href="` + self + `"/>` +
		strings.Join(items, "") + `</channel></rss>`
}

func rss2ItemsDoc(items ...string) string {
	return `<rss version="2.0"><channel><title>notification</title>` +
		strings.Join(items, "") + `</channel></rss>`
}

func atomEntry(id, title string) string {
	return `<entry><id>` + id + `</id><title>` + title + `</title></entry>`
}

func atomFeedDoc(hub, self, title string, entries ...string) string {
	return `<feed xmlns="http://www.w3.org/2005/Atom"><title>` + title + `</title>` +
		`<link rel="hub" href="` + hub + `"/><link rel="self" href="` + self + `"/>` +
		strings.Join(entries, "") + `</feed>`
}

func atomItemsDoc(entries ...string) string {
	return `<feed xmlns="http://www.w3.org/2005/Atom"><title>notification</title>` +
		strings.Join(entries, "") + `</feed>`
}

// readForm consumes a subscribe request body.
func readForm(c *taskcheck.C, req *http.Request) url.Values {
	body, err := io.ReadAll(req.Body)
	c.Check(err == nil, "read subscribe body: %v", err)
	req.Body.Close()
	form, err := url.ParseQuery(string(body))
	c.Check(err == nil, "parse subscribe form: %v", err)
	return form
}

// callbackPath extracts the path of the subscribed callback URL.
func callbackPath(form url.Values) string {
	u, err := url.Parse(form.Get("hub.callback"))
	if err != nil {
		return ""
	}
	return u.Path
}

// callbackSecret extracts the subscribed secret.
func callbackSecret(form url.Values) []byte {
	return []byte(form.Get("hub.secret"))
}

// verify drives a hub verification GET against sub.
func verify(sub *rss.Subscription, path string, q url.Values) *httptest.ResponseRecorder {
	req := httptest.NewRequest(http.MethodGet, path+"?"+q.Encode(), nil)
	rec := httptest.NewRecorder()
	sub.ServeHTTP(rec, req)
	return rec
}

// confirm drives a well-formed hub verification for the given
// subscribe form.
func confirm(sub *rss.Subscription, form url.Values, challenge, leaseSeconds string) *httptest.ResponseRecorder {
	return verify(sub, callbackPath(form), url.Values{
		"hub.mode":          {"subscribe"},
		"hub.topic":         {form.Get("hub.topic")},
		"hub.challenge":     {challenge},
		"hub.lease_seconds": {leaseSeconds},
	})
}

// checkVerify asserts a successful verification response.
func checkVerify(c *taskcheck.C, rec *httptest.ResponseRecorder, challenge string) {
	res := rec.Result()
	body, err := io.ReadAll(res.Body)
	c.Check(err == nil, "read verification body: %v", err)
	c.Check(rec.Code == http.StatusOK, "verification status = %d, want 200", rec.Code)
	c.Check(string(body) == challenge, "verification echoed %q, want %q", body, challenge)
	c.Check(res.Header.Get("Content-Type") == "application/octet-stream",
		"verification Content-Type = %q", res.Header.Get("Content-Type"))
	c.Check(res.Header.Get("X-Content-Type-Options") == "nosniff",
		"verification X-Content-Type-Options = %q", res.Header.Get("X-Content-Type-Options"))
}

// notify posts a signed notification to sub.
func notify(sub *rss.Subscription, path string, secret []byte, body string) *httptest.ResponseRecorder {
	mac := hmac.New(sha256.New, secret)
	mac.Write([]byte(body))
	req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
	req.Header.Set("X-Hub-Signature", "sha256="+hex.EncodeToString(mac.Sum(nil)))
	rec := httptest.NewRecorder()
	sub.ServeHTTP(rec, req)
	return rec
}

// itemIDs lists the IDs of a feed's items in order.
func itemIDs(feed *rss.Feed) []string {
	ids := make([]string, len(feed.Items))
	for i, item := range feed.Items {
		ids[i] = item.ID
	}
	return ids
}

func TestWebSubLease(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := newTestClock(time.Date(2026, 6, 1, 12, 0, 0, 0, time.UTC))

		// Phase 1: the hub verifies the subscription before it
		// returns 202; the lease still starts at the verification.
		var sub *rss.Subscription
		var subscribeForm url.Values
		var confirmRec *httptest.ResponseRecorder
		var hubBodies atomic.Int32
		transport := roundTripFunc(func(req *http.Request) (*http.Response, error) {
			c.Check(req.Method == http.MethodPost, "hub request method = %s, want POST", req.Method)
			c.Check(req.URL.String() == testHubURL, "hub request URL = %q, want %q", req.URL, testHubURL)
			c.Check(req.Header.Get("Content-Type") == "application/x-www-form-urlencoded",
				"hub request Content-Type = %q", req.Header.Get("Content-Type"))
			subscribeForm = readForm(c, req)
			// Verify before answering 202.
			confirmRec = confirm(sub, subscribeForm, "challenge-1", "600")
			return &http.Response{
				StatusCode: http.StatusAccepted,
				Status:     "202 Accepted",
				Header:     make(http.Header),
				Request:    req,
				Body:       &closeCountingBody{Reader: strings.NewReader("accepted"), closes: &hubBodies},
			}, nil
		})
		reader, err := rss.NewReader(rss.WithNow(clock.Now), rss.WithHTTPClient(&http.Client{Transport: transport}))
		c.Check(err == nil, "NewReader: %v", err)
		feed, err := reader.Parse(testTopicURL, strings.NewReader(
			rss2FeedDoc(testHubURL, testTopicURL, "channel", rss2Item("a", "a-old"))))
		c.Check(err == nil, "Parse: %v", err)
		sub, err = reader.NewSubscription(feed, testCallback)
		c.Check(err == nil, "NewSubscription: %v", err)

		err = sub.Subscribe(context.Background())
		c.Check(err == nil, "Subscribe: %v", err)
		c.Check(subscribeForm.Get("hub.mode") == "subscribe", "hub.mode = %q", subscribeForm.Get("hub.mode"))
		c.Check(subscribeForm.Get("hub.topic") == testTopicURL, "hub.topic = %q", subscribeForm.Get("hub.topic"))
		c.Check(strings.HasPrefix(subscribeForm.Get("hub.callback"), testCallback+"/"),
			"hub.callback = %q", subscribeForm.Get("hub.callback"))
		secret := subscribeForm.Get("hub.secret")
		c.Check(len(secret) > 0 && len(secret) < 200, "hub.secret length = %d", len(secret))
		checkVerify(c, confirmRec, "challenge-1")
		c.Check(hubBodies.Load() == 1, "hub response bodies closed = %d, want 1", hubBodies.Load())

		// The generation activated once acceptance and verification
		// had both completed.
		path := callbackPath(subscribeForm)
		rec := notify(sub, path, callbackSecret(subscribeForm), rss2ItemsDoc(rss2Item("b", "b-new")))
		c.Check(rec.Code == http.StatusNoContent, "notification status = %d, want 204", rec.Code)
		snap := sub.Snapshot()
		c.Check(fmt.Sprint(itemIDs(snap)) == "[a b]", "items = %v, want [a b]", itemIDs(snap))
		c.Check(snap.Unread == 2, "Unread = %d, want 2", snap.Unread)
		c.Check(snap.UpdateURL == testTopicURL, "UpdateURL = %q, want topic", snap.UpdateURL)

		// A repeated verification echoes the challenge but does not
		// extend the lease.
		clock.Advance(550 * time.Second)
		rec = notify(sub, path, callbackSecret(subscribeForm), rss2ItemsDoc(rss2Item("c", "c-new")))
		c.Check(rec.Code == http.StatusNoContent, "notification before expiry = %d, want 204", rec.Code)
		checkVerify(c, confirm(sub, subscribeForm, "challenge-2", "600"), "challenge-2")
		clock.Advance(100 * time.Second) // 650s since the first verification.
		rec = notify(sub, path, callbackSecret(subscribeForm), rss2ItemsDoc(rss2Item("d", "d-new")))
		c.Check(rec.Code == http.StatusGone, "notification after lease expiry = %d, want 410", rec.Code)
		snap = sub.Snapshot()
		c.Check(fmt.Sprint(itemIDs(snap)) == "[a b c]", "expired notification published: %v", itemIDs(snap))

		// Phase 2: delayed confirmation, failed renewal, and busy
		// detection on a fresh subscription.
		var mu sync.Mutex
		var forms []url.Values
		behavior := 0 // 0: accept, 1: refuse, 2: block.
		entered := make(chan struct{})
		release := make(chan struct{})
		transport2 := roundTripFunc(func(req *http.Request) (*http.Response, error) {
			form := readForm(c, req)
			mu.Lock()
			forms = append(forms, form)
			mode := behavior
			mu.Unlock()
			switch mode {
			case 1:
				return httpResponse(req, http.StatusInternalServerError, "no"), nil
			case 2:
				close(entered)
				<-release
			}
			return httpResponse(req, http.StatusAccepted, ""), nil
		})
		reader2, err := rss.NewReader(rss.WithNow(clock.Now), rss.WithHTTPClient(&http.Client{Transport: transport2}))
		c.Check(err == nil, "NewReader: %v", err)
		feed2, err := reader2.Parse(testTopicURL, strings.NewReader(
			rss2FeedDoc(testHubURL, testTopicURL, "channel2", rss2Item("x", "x-old"))))
		c.Check(err == nil, "Parse: %v", err)
		sub2, err := reader2.NewSubscription(feed2, testCallback)
		c.Check(err == nil, "NewSubscription: %v", err)

		c.Check(sub2.Subscribe(context.Background()) == nil, "first Subscribe failed")
		c.Check(len(forms) == 1, "subscribe requests = %d, want 1", len(forms))

		// The candidate has not been confirmed yet: notifications to
		// its path are refused with 409.
		rec = notify(sub2, callbackPath(forms[0]), callbackSecret(forms[0]), rss2ItemsDoc(rss2Item("y", "y-new")))
		c.Check(rec.Code == http.StatusConflict, "unconfirmed candidate notification = %d, want 409", rec.Code)

		// The delayed confirmation activates the generation.
		checkVerify(c, confirm(sub2, forms[0], "late", "3600"), "late")
		rec = notify(sub2, callbackPath(forms[0]), callbackSecret(forms[0]), rss2ItemsDoc(rss2Item("y", "y-new")))
		c.Check(rec.Code == http.StatusNoContent, "confirmed notification = %d, want 204", rec.Code)

		// A failed renewal must not revoke the active lease.
		mu.Lock()
		behavior = 1
		mu.Unlock()
		err = sub2.Subscribe(context.Background())
		c.Check(err != nil, "renewal refused by hub must fail")
		rec = notify(sub2, callbackPath(forms[0]), callbackSecret(forms[0]), rss2ItemsDoc(rss2Item("z", "z-new")))
		c.Check(rec.Code == http.StatusNoContent, "old lease after failed renewal = %d, want 204", rec.Code)
		c.Check(len(forms) == 2, "subscribe requests = %d, want 2", len(forms))
		rec = notify(sub2, callbackPath(forms[1]), callbackSecret(forms[1]), rss2ItemsDoc(rss2Item("w", "w-new")))
		c.Check(rec.Code == http.StatusConflict, "incomplete candidate notification = %d, want 409", rec.Code)

		// A concurrent Subscribe is reported busy; the blocked one
		// discards the incomplete candidate from the failed renewal.
		mu.Lock()
		behavior = 2
		mu.Unlock()
		blocked := make(chan error, 1)
		go func() { blocked <- sub2.Subscribe(context.Background()) }()
		<-entered
		err = sub2.Subscribe(context.Background())
		c.Check(errors.Is(err, rss.ErrSubscriptionBusy), "concurrent Subscribe = %v, want ErrSubscriptionBusy", err)
		close(release)
		c.Check(<-blocked == nil, "blocked Subscribe failed")

		// The discarded candidate can no longer be activated by a
		// late confirmation.
		rec = confirm(sub2, forms[1], "too-late", "3600")
		c.Check(rec.Code == http.StatusGone, "discarded candidate confirmation = %d, want 410", rec.Code)
		rec = notify(sub2, callbackPath(forms[1]), callbackSecret(forms[1]), rss2ItemsDoc(rss2Item("w", "w-new")))
		c.Check(rec.Code == http.StatusGone, "discarded candidate notification = %d, want 410", rec.Code)

		// Confirming the newest candidate retires the old lease.
		c.Check(len(forms) == 3, "subscribe requests = %d, want 3", len(forms))
		checkVerify(c, confirm(sub2, forms[2], "gen3", "3600"), "gen3")
		rec = notify(sub2, callbackPath(forms[2]), callbackSecret(forms[2]), rss2ItemsDoc(rss2Item("v", "v-new")))
		c.Check(rec.Code == http.StatusNoContent, "new generation notification = %d, want 204", rec.Code)
		rec = notify(sub2, callbackPath(forms[0]), callbackSecret(forms[0]), rss2ItemsDoc(rss2Item("u", "u-new")))
		c.Check(rec.Code == http.StatusGone, "retired generation notification = %d, want 410", rec.Code)

		snap = sub2.Snapshot()
		c.Check(fmt.Sprint(itemIDs(snap)) == "[x y z v]", "items = %v, want [x y z v]", itemIDs(snap))
	})
}

func TestWebSubReconcile(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := newTestClock(time.Date(2026, 6, 1, 12, 0, 0, 0, time.UTC))
		sw := &switchboard{}
		reader, err := rss.NewReader(rss.WithNow(clock.Now), rss.WithHTTPClient(&http.Client{Transport: sw}))
		c.Check(err == nil, "NewReader: %v", err)
		feed, err := reader.Parse(testTopicURL, strings.NewReader(
			rss2FeedDoc(testHubURL, testTopicURL, "channel", rss2Item("a", "a-old"), rss2Item("b", "b-old"))))
		c.Check(err == nil, "Parse: %v", err)
		sub, err := reader.NewSubscription(feed, testCallback)
		c.Check(err == nil, "NewSubscription: %v", err)

		// Activate the first generation.
		var form url.Values
		sw.set(nil, func(req *http.Request) (*http.Response, error) {
			form = readForm(c, req)
			return httpResponse(req, http.StatusAccepted, ""), nil
		})
		c.Check(sub.Subscribe(context.Background()) == nil, "Subscribe failed")
		checkVerify(c, confirm(sub, form, "lease", "3600"), "lease")
		path, secret := callbackPath(form), callbackSecret(form)

		c.Check(sub.SetRead("a", true), "SetRead(a) before resync")
		c.Check(sub.Snapshot().Unread == 1, "Unread = %d, want 1", sub.Snapshot().Unread)

		// A full fetch in flight is overtaken by a notification: the
		// notification revises a and adds c, while the stale full
		// body still carries the old a and no longer carries b.
		getEntered := make(chan struct{})
		releaseGet := make(chan struct{})
		var topicBodies atomic.Int32
		sw.set(func(req *http.Request) (*http.Response, error) {
			c.Check(req.URL.String() == testTopicURL, "resync URL = %q, want topic", req.URL)
			close(getEntered)
			<-releaseGet
			return &http.Response{
				StatusCode: http.StatusOK,
				Status:     "200 OK",
				Header:     make(http.Header),
				Request:    req,
				Body: &closeCountingBody{
					Reader: strings.NewReader(rss2FeedDoc(testHubURL, testTopicURL, "full-title", rss2Item("a", "a-stale"))),
					closes: &topicBodies,
				},
			}, nil
		}, nil)
		resyncDone := make(chan error, 1)
		go func() { resyncDone <- sub.Resync(context.Background()) }()
		<-getEntered
		rec := notify(sub, path, secret, rss2ItemsDoc(rss2Item("a", "a-new"), rss2Item("c", "c-new")))
		c.Check(rec.Code == http.StatusNoContent, "in-flight notification = %d, want 204", rec.Code)
		c.Check(sub.SetRead("c", true), "SetRead(c) during fetch")
		close(releaseGet)
		c.Check(<-resyncDone == nil, "Resync failed")
		c.Check(topicBodies.Load() == 1, "topic bodies closed = %d, want 1", topicBodies.Load())

		snap := sub.Snapshot()
		c.Check(snap.Title == "full-title", "metadata from full fetch = %q", snap.Title)
		c.Check(fmt.Sprint(itemIDs(snap)) == "[a c]", "items = %v, want [a c]", itemIDs(snap))
		if len(snap.Items) == 2 {
			c.Check(snap.Items[0].Title == "a-new", "notification revision beats stale full body: %q", snap.Items[0].Title)
			c.Check(snap.Items[0].Read, "a keeps its local read state")
			c.Check(snap.Items[1].Read, "c keeps the read state set during the fetch")
		}
		c.Check(snap.Unread == 0, "Unread = %d, want 0", snap.Unread)
		c.Check(snap.UpdateURL == testTopicURL, "UpdateURL = %q, want topic", snap.UpdateURL)

		// A cancelled resync keeps the published notifications and
		// read state, and a later resync still works.
		rec = notify(sub, path, secret, rss2ItemsDoc(rss2Item("d", "d-new")))
		c.Check(rec.Code == http.StatusNoContent, "notification = %d, want 204", rec.Code)

		cancelled, cancel := context.WithCancel(context.Background())
		cancel()
		err = sub.Resync(cancelled)
		c.Check(errors.Is(err, context.Canceled), "pre-cancelled Resync = %v, want context.Canceled", err)
		c.Check(sw.gets.Load() == 1, "pre-cancelled Resync made %d GET requests, want 1", sw.gets.Load())

		entered2 := make(chan struct{})
		sw.set(func(req *http.Request) (*http.Response, error) {
			close(entered2)
			<-req.Context().Done()
			return nil, req.Context().Err()
		}, nil)
		ctx, cancel2 := context.WithCancel(context.Background())
		resyncDone2 := make(chan error, 1)
		go func() { resyncDone2 <- sub.Resync(ctx) }()
		<-entered2
		cancel2()
		err = <-resyncDone2
		c.Check(err != nil, "cancelled in-flight Resync must fail")
		snap = sub.Snapshot()
		c.Check(fmt.Sprint(itemIDs(snap)) == "[a c d]", "cancelled resync lost state: %v", itemIDs(snap))
		c.Check(snap.Unread == 1, "Unread = %d, want 1", snap.Unread)

		// A concurrent Resync is busy and makes no request; the
		// blocked one overlays only notifications after its boundary.
		entered3 := make(chan struct{})
		release3 := make(chan struct{})
		sw.set(func(req *http.Request) (*http.Response, error) {
			close(entered3)
			<-release3
			return httpResponse(req, http.StatusOK,
				rss2FeedDoc(testHubURL, testTopicURL, "full-2", rss2Item("a", "a-final"), rss2Item("d", "d-full"))), nil
		}, nil)
		before := sw.gets.Load()
		go func() { resyncDone2 <- sub.Resync(context.Background()) }()
		<-entered3
		err = sub.Resync(context.Background())
		c.Check(errors.Is(err, rss.ErrSubscriptionBusy), "concurrent Resync = %v, want ErrSubscriptionBusy", err)
		c.Check(sw.gets.Load() == before+1, "busy Resync made a GET request")
		rec = notify(sub, path, secret, rss2ItemsDoc(rss2Item("e", "e-new")))
		c.Check(rec.Code == http.StatusNoContent, "notification during resync = %d, want 204", rec.Code)
		close(release3)
		c.Check(<-resyncDone2 == nil, "Resync after cancellation failed")
		snap = sub.Snapshot()
		c.Check(fmt.Sprint(itemIDs(snap)) == "[a d e]", "items = %v, want [a d e]", itemIDs(snap))
		if len(snap.Items) == 3 {
			c.Check(snap.Items[0].Title == "a-final" && snap.Items[0].Read, "a = %+v", snap.Items[0])
			c.Check(snap.Items[1].Title == "d-full" && !snap.Items[1].Read, "d = %+v", snap.Items[1])
			c.Check(!snap.Items[2].Read, "e is unread")
		}

		// A slow notification body that finishes reading only after
		// a lease switch is rejected.
		payload := rss2ItemsDoc(rss2Item("z", "z-slow"))
		bodyStarted := make(chan struct{})
		bodyGate := make(chan struct{})
		slowBody := &gatedReadCloser{started: bodyStarted, gate: bodyGate, body: strings.NewReader(payload)}
		mac := hmac.New(sha256.New, secret)
		mac.Write([]byte(payload))
		req := httptest.NewRequest(http.MethodPost, path, slowBody)
		req.Header.Set("X-Hub-Signature", "sha256="+hex.EncodeToString(mac.Sum(nil)))
		notifyDone := make(chan int, 1)
		go func() {
			rec := httptest.NewRecorder()
			sub.ServeHTTP(rec, req)
			notifyDone <- rec.Code
		}()
		<-bodyStarted

		var form2 url.Values
		sw.set(nil, func(req *http.Request) (*http.Response, error) {
			form2 = readForm(c, req)
			return httpResponse(req, http.StatusAccepted, ""), nil
		})
		c.Check(sub.Subscribe(context.Background()) == nil, "renewal failed")
		checkVerify(c, confirm(sub, form2, "gen2", "3600"), "gen2")

		close(bodyGate)
		slowStatus := <-notifyDone
		c.Check(slowStatus == http.StatusGone, "slow notification across lease switch = %d, want 410", slowStatus)
		snap = sub.Snapshot()
		for _, item := range snap.Items {
			c.Check(item.ID != "z", "slow notification published item z")
		}

		// The new generation accepts notifications.
		rec = notify(sub, callbackPath(form2), callbackSecret(form2), rss2ItemsDoc(rss2Item("f", "f-new")))
		c.Check(rec.Code == http.StatusNoContent, "new generation notification = %d, want 204", rec.Code)
		snap = sub.Snapshot()
		c.Check(fmt.Sprint(itemIDs(snap)) == "[a d e f]", "items = %v, want [a d e f]", itemIDs(snap))
	})
}

func TestWebSubIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := newTestClock(time.Date(2026, 6, 1, 12, 0, 0, 0, time.UTC))
		var formsMu sync.Mutex
		forms := make(map[string]url.Values)
		transport := roundTripFunc(func(req *http.Request) (*http.Response, error) {
			if req.Method != http.MethodPost {
				return httpResponse(req, http.StatusNotFound, ""), nil
			}
			form := readForm(c, req)
			formsMu.Lock()
			forms[req.URL.String()] = form
			formsMu.Unlock()
			return httpResponse(req, http.StatusAccepted, ""), nil
		})
		reader, err := rss.NewReader(rss.WithNow(clock.Now), rss.WithHTTPClient(&http.Client{Transport: transport}))
		c.Check(err == nil, "NewReader: %v", err)

		// Two subscriptions on one reader, one RSS 2.0 and one Atom,
		// both parsed for real.
		feed1, err := reader.Parse(testTopicURL, strings.NewReader(
			rss2FeedDoc(testHubURL, testTopicURL, "rss-channel", rss2Item("r1", "r1-old"))))
		c.Check(err == nil, "Parse RSS 2.0: %v", err)
		feed2, err := reader.Parse(testTopic2URL, strings.NewReader(
			atomFeedDoc(testHub2URL, testTopic2URL, "atom-channel", atomEntry("a1", "a1-old"))))
		c.Check(err == nil, "Parse Atom: %v", err)
		sub1, err := reader.NewSubscription(feed1, testCallback)
		c.Check(err == nil, "NewSubscription RSS 2.0: %v", err)
		sub2, err := reader.NewSubscription(feed2, testCallback)
		c.Check(err == nil, "NewSubscription Atom: %v", err)

		// Mutating the constructor inputs must not leak in.
		feed1.Title = "mutated"
		feed1.Items[0].Title = "mutated"
		feed1.Items[0].Read = true
		feed1.Links[0].Href = "https://evil.example/hub"
		feed1.Items = append(feed1.Items, &rss.Item{ID: "injected"})
		feed2.Title = "mutated"
		feed2.Items[0].Title = "mutated"

		c.Check(sub1.Subscribe(context.Background()) == nil, "sub1 Subscribe failed")
		c.Check(sub2.Subscribe(context.Background()) == nil, "sub2 Subscribe failed")
		checkVerify(c, confirm(sub1, forms[testHubURL], "v1", "3600"), "v1")
		checkVerify(c, confirm(sub2, forms[testHub2URL], "v2", "3600"), "v2")

		snap1 := sub1.Snapshot()
		c.Check(snap1.Title == "rss-channel", "constructor input leaked into title: %q", snap1.Title)
		c.Check(fmt.Sprint(itemIDs(snap1)) == "[r1]", "constructor input leaked into items: %v", itemIDs(snap1))
		if len(snap1.Items) == 1 {
			c.Check(snap1.Items[0].Title == "r1-old" && !snap1.Items[0].Read, "constructor input leaked into item: %+v", snap1.Items[0])
		}
		c.Check(snap1.UpdateURL == testTopicURL, "sub1 UpdateURL = %q, want topic", snap1.UpdateURL)
		c.Check(sub2.Snapshot().UpdateURL == testTopic2URL, "sub2 UpdateURL = %q, want topic", sub2.Snapshot().UpdateURL)

		// Notifications flow to the two subscriptions independently.
		rec := notify(sub1, callbackPath(forms[testHubURL]), callbackSecret(forms[testHubURL]),
			rss2ItemsDoc(rss2Item("r2", "r2-new")))
		c.Check(rec.Code == http.StatusNoContent, "sub1 notification = %d, want 204", rec.Code)
		rec = notify(sub2, callbackPath(forms[testHub2URL]), callbackSecret(forms[testHub2URL]),
			atomItemsDoc(atomEntry("a2", "a2-new")))
		c.Check(rec.Code == http.StatusNoContent, "sub2 notification = %d, want 204", rec.Code)
		snap1 = sub1.Snapshot()
		snap2 := sub2.Snapshot()
		c.Check(fmt.Sprint(itemIDs(snap1)) == "[r1 r2]", "sub1 items = %v, want [r1 r2]", itemIDs(snap1))
		c.Check(fmt.Sprint(itemIDs(snap2)) == "[a1 a2]", "sub2 items = %v, want [a1 a2]", itemIDs(snap2))
		c.Check(snap2.Items[0].Title == "a1-old", "atom item title = %q", snap2.Items[0].Title)

		// Mutating a snapshot must not leak back in.
		snap1.Title = "mutated"
		snap1.Items[0].Title = "mutated"
		snap1.Items[0].Read = true
		snap1.Items[0].Links = append(snap1.Items[0].Links, &rss.Link{Href: "https://evil.example/"})
		snap1.Items = append(snap1.Items, &rss.Item{ID: "injected"})
		snap1.Links[0].Href = "https://evil.example/hub"
		fresh := sub1.Snapshot()
		c.Check(fresh.Title == "rss-channel", "snapshot mutation leaked into title: %q", fresh.Title)
		c.Check(fmt.Sprint(itemIDs(fresh)) == "[r1 r2]", "snapshot mutation leaked into items: %v", itemIDs(fresh))
		if len(fresh.Items) == 2 {
			c.Check(fresh.Items[0].Title == "r1-old" && !fresh.Items[0].Read, "snapshot mutation leaked into item: %+v", fresh.Items[0])
			c.Check(len(fresh.Items[0].Links) == 0, "snapshot mutation leaked into item links")
		}
		c.Check(fresh.Unread == 2, "Unread = %d, want 2", fresh.Unread)

		// SetRead tracks existing IDs and recomputes Unread.
		c.Check(sub1.SetRead("r1", true), "SetRead(r1) = false, want true")
		c.Check(sub1.Snapshot().Unread == 1, "Unread = %d, want 1", sub1.Snapshot().Unread)
		c.Check(!sub1.SetRead("missing", true), "SetRead(missing) = true, want false")
		c.Check(sub1.Snapshot().Unread == 1, "Unread after missing SetRead = %d, want 1", sub1.Snapshot().Unread)
		c.Check(sub1.SetRead("r1", false), "SetRead(r1, false) = false, want true")
		c.Check(sub1.Snapshot().Unread == 2, "Unread = %d, want 2", sub1.Snapshot().Unread)

		// Constructor validation.
		_, err = reader.NewSubscription(nil, testCallback)
		c.Check(err != nil, "nil initial feed accepted")
		link := func(rel, href string) *rss.Link { return &rss.Link{Rel: rel, Href: href} }
		mkFeed := func(links ...*rss.Link) *rss.Feed {
			return &rss.Feed{UpdateURL: testTopicURL, Links: links}
		}
		_, err = reader.NewSubscription(mkFeed(link("self", testTopicURL)), testCallback)
		c.Check(err != nil, "missing hub link accepted")
		_, err = reader.NewSubscription(mkFeed(link("hub", testHubURL)), testCallback)
		c.Check(err != nil, "missing self link accepted")
		_, err = reader.NewSubscription(mkFeed(
			link("hub", testHubURL), link("hub", testHubURL), link("self", testTopicURL)), testCallback)
		c.Check(err != nil, "duplicate hub links accepted")
		_, err = reader.NewSubscription(mkFeed(
			link("hub", "https://hub.example/endpoint#frag"), link("self", testTopicURL)), testCallback)
		c.Check(err != nil, "hub fragment accepted")
		_, err = reader.NewSubscription(mkFeed(
			link("hub", "ftp://hub.example/endpoint"), link("self", testTopicURL)), testCallback)
		c.Check(err != nil, "hub non-HTTP scheme accepted")
		_, err = reader.NewSubscription(mkFeed(
			link("hub", "https://user@hub.example/endpoint"), link("self", testTopicURL)), testCallback)
		c.Check(err != nil, "hub userinfo accepted")
		_, err = reader.NewSubscription(
			&rss.Feed{Links: []*rss.Link{link("hub", "/endpoint"), link("self", "/feed")}}, testCallback)
		c.Check(err != nil, "unresolvable relative links accepted")
		good := mkFeed(link("hub", testHubURL), link("self", testTopicURL))
		for _, bad := range []string{
			"", "not-a-url", "https://cb.example/cb?x=1", "https://cb.example/cb#frag",
			"https://user@cb.example/cb", "ftp://cb.example/cb",
		} {
			_, err = reader.NewSubscription(good, bad)
			c.Check(err != nil, "callback base %q accepted", bad)
		}

		// HTTP edge cases on the active generation.
		path1 := callbackPath(forms[testHubURL])
		secret1 := callbackSecret(forms[testHubURL])

		rec = verify(sub1, "/callbacks/unknown", url.Values{
			"hub.mode": {"subscribe"}, "hub.topic": {testTopicURL},
			"hub.challenge": {"c"}, "hub.lease_seconds": {"60"},
		})
		c.Check(rec.Code == http.StatusNotFound, "unknown verification path = %d, want 404", rec.Code)
		rec = notify(sub1, "/callbacks/unknown", secret1, rss2ItemsDoc(rss2Item("q", "q")))
		c.Check(rec.Code == http.StatusNotFound, "unknown notification path = %d, want 404", rec.Code)

		rec = notify(sub1, path1, []byte("wrong-secret"), rss2ItemsDoc(rss2Item("q", "q")))
		c.Check(rec.Code == http.StatusForbidden, "bad signature = %d, want 403", rec.Code)
		rec = httptest.NewRecorder()
		sub1.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, path1, strings.NewReader(rss2ItemsDoc(rss2Item("q", "q")))))
		c.Check(rec.Code == http.StatusForbidden, "missing signature = %d, want 403", rec.Code)

		rec = notify(sub1, path1, secret1, strings.Repeat("x", 1<<20+1))
		c.Check(rec.Code == http.StatusRequestEntityTooLarge, "oversize notification = %d, want 413", rec.Code)

		rec = notify(sub1, path1, secret1, `<rss version="2.0"><channel><item>`)
		c.Check(rec.Code == http.StatusBadRequest, "malformed notification = %d, want 400", rec.Code)

		rec = verify(sub1, path1, url.Values{
			"hub.mode": {"subscribe"}, "hub.topic": {testTopicURL},
			"hub.challenge": {"c"}, "hub.lease_seconds": {"abc"},
		})
		c.Check(rec.Code == http.StatusBadRequest, "bad lease_seconds = %d, want 400", rec.Code)
		rec = verify(sub1, path1, url.Values{
			"hub.mode": {"subscribe"}, "hub.topic": {testTopicURL},
			"hub.challenge": {""}, "hub.lease_seconds": {"60"},
		})
		c.Check(rec.Code == http.StatusBadRequest, "empty challenge = %d, want 400", rec.Code)
		rec = verify(sub1, path1, url.Values{
			"hub.mode": {"unsubscribe"}, "hub.topic": {testTopicURL},
			"hub.challenge": {"c"}, "hub.lease_seconds": {"60"},
		})
		c.Check(rec.Code == http.StatusBadRequest, "wrong mode = %d, want 400", rec.Code)
		rec = verify(sub1, path1, url.Values{
			"hub.mode": {"subscribe"}, "hub.topic": {"https://other.example/topic"},
			"hub.challenge": {"c"}, "hub.lease_seconds": {"60"},
		})
		c.Check(rec.Code == http.StatusBadRequest, "wrong topic = %d, want 400", rec.Code)

		// None of the rejected requests were published.
		c.Check(fmt.Sprint(itemIDs(sub1.Snapshot())) == "[r1 r2]",
			"rejected requests published: %v", itemIDs(sub1.Snapshot()))

		// Renewal retires the old generation for both verbs.
		c.Check(sub1.Subscribe(context.Background()) == nil, "sub1 renewal failed")
		formsMu.Lock()
		form1 := forms[testHubURL]
		formsMu.Unlock()
		checkVerify(c, confirm(sub1, form1, "v3", "3600"), "v3")
		rec = verify(sub1, path1, url.Values{
			"hub.mode": {"subscribe"}, "hub.topic": {testTopicURL},
			"hub.challenge": {"c"}, "hub.lease_seconds": {"60"},
		})
		c.Check(rec.Code == http.StatusGone, "retired verification path = %d, want 410", rec.Code)
		rec = notify(sub1, path1, secret1, rss2ItemsDoc(rss2Item("q", "q")))
		c.Check(rec.Code == http.StatusGone, "retired notification path = %d, want 410", rec.Code)
		rec = notify(sub1, callbackPath(form1), callbackSecret(form1), rss2ItemsDoc(rss2Item("r3", "r3-new")))
		c.Check(rec.Code == http.StatusNoContent, "renewed notification = %d, want 204", rec.Code)
		c.Check(fmt.Sprint(itemIDs(sub1.Snapshot())) == "[r1 r2 r3]",
			"items after renewal = %v, want [r1 r2 r3]", itemIDs(sub1.Snapshot()))
	})
}
