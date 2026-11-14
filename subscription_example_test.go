package rss_test

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"

	rss "github.com/SlyMarbo/rss/v2"
)

// ExampleReader_NewSubscription subscribes to a feed that advertises
// a WebSub hub. The hub here is a stand-in that verifies the
// callback before accepting the subscribe request, which the
// subscription handles transparently.
func ExampleReader_NewSubscription() {
	var sub *rss.Subscription
	client := &http.Client{Transport: roundTripFunc(func(req *http.Request) (*http.Response, error) {
		if req.Method != http.MethodPost {
			return nil, fmt.Errorf("unexpected request: %s %s", req.Method, req.URL)
		}
		body, _ := io.ReadAll(req.Body)
		req.Body.Close()
		form, _ := url.ParseQuery(string(body))
		fmt.Println("hub received:", form.Get("hub.mode"), form.Get("hub.topic"))

		// The hub verifies the callback, then accepts the request with
		// a 202. The verification may even arrive first, as here.
		verify := httptest.NewRequest(http.MethodGet, form.Get("hub.callback"), nil)
		query := verify.URL.Query()
		query.Set("hub.mode", "subscribe")
		query.Set("hub.topic", form.Get("hub.topic"))
		query.Set("hub.challenge", "hub-challenge")
		query.Set("hub.lease_seconds", "3600")
		verify.URL.RawQuery = query.Encode()
		recorder := httptest.NewRecorder()
		sub.ServeHTTP(recorder, verify)
		if recorder.Code != http.StatusOK || recorder.Body.String() != "hub-challenge" {
			return nil, fmt.Errorf("hub: verification failed: %d", recorder.Code)
		}
		return &http.Response{
			StatusCode: http.StatusAccepted,
			Header:     make(http.Header),
			Request:    req,
			Body:       io.NopCloser(strings.NewReader("")),
		}, nil
	})}

	reader, err := rss.NewReader(rss.WithHTTPClient(client))
	if err != nil {
		panic(err)
	}
	feed, err := reader.Parse("https://example.com/feed.xml", strings.NewReader(`
		<rss version="2.0" xmlns:atom="http://www.w3.org/2005/Atom">
			<channel>
				<title>Example feed</title>
				<atom:link rel="hub" href="https://hub.example/endpoint"/>
				<atom:link rel="self" href="https://example.com/feed.xml"/>
				<item><guid>1</guid><title>First post</title></item>
			</channel>
		</rss>`))
	if err != nil {
		panic(err)
	}

	sub, err = reader.NewSubscription(feed, "https://subscriber.example/callbacks")
	if err != nil {
		panic(err)
	}
	if err := sub.Subscribe(context.Background()); err != nil {
		panic(err)
	}

	snapshot := sub.Snapshot()
	fmt.Println(snapshot.Title)
	for _, item := range snapshot.Items {
		fmt.Println(item.ID, item.Title)
	}
	fmt.Println("unread:", snapshot.Unread)

	// Output:
	// hub received: subscribe https://example.com/feed.xml
	// Example feed
	// 1 First post
	// unread: 1
}

// ExampleSubscription_Resync combines push notifications with a full
// fetch of the topic: the notification adds a post, and the resync
// notices that another post was deleted.
func ExampleSubscription_Resync() {
	var form url.Values
	client := &http.Client{Transport: roundTripFunc(func(req *http.Request) (*http.Response, error) {
		switch req.Method {
		case http.MethodPost:
			body, _ := io.ReadAll(req.Body)
			req.Body.Close()
			form, _ = url.ParseQuery(string(body))
			return &http.Response{
				StatusCode: http.StatusAccepted,
				Header:     make(http.Header),
				Request:    req,
				Body:       io.NopCloser(strings.NewReader("")),
			}, nil
		default:
			// The topic now carries the first and third posts; the
			// second was deleted at the source.
			return &http.Response{
				StatusCode: http.StatusOK,
				Header:     make(http.Header),
				Request:    req,
				Body: io.NopCloser(strings.NewReader(`
					<rss version="2.0"><channel><title>Example feed</title>
						<item><guid>1</guid><title>First post</title></item>
						<item><guid>3</guid><title>Third post</title></item>
					</channel></rss>`)),
			}, nil
		}
	})}

	reader, err := rss.NewReader(rss.WithHTTPClient(client))
	if err != nil {
		panic(err)
	}
	feed, err := reader.Parse("https://example.com/feed.xml", strings.NewReader(`
		<rss version="2.0" xmlns:atom="http://www.w3.org/2005/Atom">
			<channel>
				<title>Example feed</title>
				<atom:link rel="hub" href="https://hub.example/endpoint"/>
				<atom:link rel="self" href="https://example.com/feed.xml"/>
				<item><guid>1</guid><title>First post</title></item>
				<item><guid>2</guid><title>Second post</title></item>
			</channel>
		</rss>`))
	if err != nil {
		panic(err)
	}
	sub, err := reader.NewSubscription(feed, "https://subscriber.example/callbacks")
	if err != nil {
		panic(err)
	}
	if err := sub.Subscribe(context.Background()); err != nil {
		panic(err)
	}

	// Confirm the subscription, as the hub would.
	verify := httptest.NewRequest(http.MethodGet, form.Get("hub.callback"), nil)
	query := verify.URL.Query()
	query.Set("hub.mode", "subscribe")
	query.Set("hub.topic", form.Get("hub.topic"))
	query.Set("hub.challenge", "hub-challenge")
	query.Set("hub.lease_seconds", "3600")
	verify.URL.RawQuery = query.Encode()
	sub.ServeHTTP(httptest.NewRecorder(), verify)

	// A push notification adds a third post.
	callback, _ := url.Parse(form.Get("hub.callback"))
	notify(sub, callback.Path, []byte(form.Get("hub.secret")), `
		<rss version="2.0"><channel><title>Example feed</title>
			<item><guid>3</guid><title>Third post</title></item>
		</channel></rss>`)

	// A resync replaces the membership with the full fetch, which
	// drops the deleted second post and keeps the notified third.
	if err := sub.Resync(context.Background()); err != nil {
		panic(err)
	}
	for _, item := range sub.Snapshot().Items {
		fmt.Println(item.ID, item.Title)
	}

	// Output:
	// 1 First post
	// 3 Third post
}
