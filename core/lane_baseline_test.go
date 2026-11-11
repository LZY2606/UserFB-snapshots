package core

import (
	"context"
	"encoding/json"
	"fmt"
	"sync"
	"testing"

	"github.com/go-rod/rod"
	"github.com/go-rod/rod/lib/cdp"
	"github.com/go-rod/rod/lib/proto"
)

// This transport supplies CDP responses only; persistence is the real Browser/LaneStore code.
type testCookieCDP struct {
	mu       sync.Mutex
	cookies  []*proto.NetworkCookie
	restored []*proto.NetworkCookieParam
	entered  chan struct{}
	release  chan struct{}
}

func (c *testCookieCDP) Event() <-chan *cdp.Event { return nil }
func (c *testCookieCDP) Call(ctx context.Context, sessionID, method string, params interface{}) ([]byte, error) {
	switch method {
	case "Network.getCookies":
		c.mu.Lock()
		raw, err := json.Marshal(struct {
			Cookies []*proto.NetworkCookie `json:"cookies"`
		}{c.cookies})
		entered, release := c.entered, c.release
		c.mu.Unlock()
		if entered != nil {
			close(entered)
			select {
			case <-release:
			case <-ctx.Done():
				return nil, ctx.Err()
			}
		}
		return raw, err
	case "Network.setCookies":
		raw, err := json.Marshal(params)
		if err != nil {
			return nil, err
		}
		var req proto.NetworkSetCookies
		if err := json.Unmarshal(raw, &req); err != nil {
			return nil, err
		}
		c.mu.Lock()
		c.restored = req.Cookies
		c.mu.Unlock()
		return []byte(`{}`), nil
	default:
		return nil, fmt.Errorf("unexpected CDP command %s", method)
	}
}
func testCookie(name, value string) *proto.NetworkCookie {
	return &proto.NetworkCookie{Name: name, Value: value, Domain: "source.example.invalid", Path: "/", Session: true}
}
func testPage(c *testCookieCDP) *rod.Page {
	return rod.New().Client(c).PageFromSession("fixture-session")
}
func TestLaneBaseline(t *testing.T) {
	key := ProxyLaneKey{Tenant: "fixture", Engine: "bing", SessionID: "local"}
	ctx := WithProxyLaneKey(context.Background(), key)
	t.Run("restore_and_independent_updates", func(t *testing.T) {
		s := NewLaneStore(3)
		s.SaveCookies(key, []*proto.NetworkCookie{testCookie("a", "0"), testCookie("b", "0")})
		b := &Browser{BrowserOpts: BrowserOpts{ProxyLaneStore: s}}
		c := &testCookieCDP{}
		if err := b.restoreLaneCookies(ctx, testPage(c)); err != nil {
			t.Fatal(err)
		}
		if len(c.restored) != 2 || c.restored[0].Name != "a" || c.restored[1].Name != "b" {
			t.Fatal("normal restore lost cookies")
		}
		fmt.Println("BASE_LANE_RESTORE_TWO_OK")
		// Both isolated pages inherited a=0,b=0. A changes a; B changes only b.
		c.cookies = []*proto.NetworkCookie{testCookie("a", "1"), testCookie("b", "0")}
		b.saveLaneCookies(ctx, testPage(c), "https://source.example.invalid/")
		c.cookies = []*proto.NetworkCookie{testCookie("a", "0"), testCookie("b", "1")}
		b.saveLaneCookies(ctx, testPage(c), "https://source.example.invalid/")
		got := s.Cookies(key)
		if len(got) != 2 || got[0].Value != "0" || got[1].Value != "1" {
			t.Fatalf("starting lost-update witness changed: %#v", got)
		}
		fmt.Println("BASE_LANE_LATE_SNAPSHOT_LOSES_A_UPDATE")
	})
	t.Run("drop_during_cookie_read", func(t *testing.T) {
		s := NewLaneStore(3)
		s.SaveCookies(key, []*proto.NetworkCookie{testCookie("session", "old")})
		b := &Browser{BrowserOpts: BrowserOpts{ProxyLaneStore: s}}
		c := &testCookieCDP{cookies: s.Cookies(key), entered: make(chan struct{}), release: make(chan struct{})}
		done := make(chan struct{})
		go func() { defer close(done); b.saveLaneCookies(ctx, testPage(c), "https://source.example.invalid/") }()
		<-c.entered
		s.DropCookies(key)
		if len(s.Cookies(key)) != 0 {
			t.Fatal("drop did not clear")
		}
		close(c.release)
		<-done
		got := s.Cookies(key)
		if len(got) != 1 || got[0].Value != "old" {
			t.Fatal("starting stale-read witness changed")
		}
		fmt.Println("BASE_LANE_OLD_CDP_REPLY_RESURRECTS_DROPPED_COOKIE")
	})
	t.Run("evict_during_cookie_read", func(t *testing.T) {
		s := NewLaneStore(1)
		s.SaveCookies(key, []*proto.NetworkCookie{testCookie("session", "evicted")})
		b := &Browser{BrowserOpts: BrowserOpts{ProxyLaneStore: s}}
		c := &testCookieCDP{cookies: s.Cookies(key), entered: make(chan struct{}), release: make(chan struct{})}
		done := make(chan struct{})
		go func() { defer close(done); b.saveLaneCookies(ctx, testPage(c), "https://source.example.invalid/") }()
		<-c.entered
		other := ProxyLaneKey{Tenant: "fixture", Engine: "bing", SessionID: "other"}
		s.SaveCookies(other, []*proto.NetworkCookie{testCookie("session", "new")})
		if len(s.Cookies(key)) != 0 {
			t.Fatal("capacity eviction did not occur")
		}
		close(c.release)
		<-done
		if len(s.Cookies(key)) != 1 || len(s.Cookies(other)) != 0 {
			t.Fatal("starting eviction witness changed")
		}
		fmt.Println("BASE_LANE_STALE_REPLY_RECREATES_EVICTED_LANE")
	})
}
