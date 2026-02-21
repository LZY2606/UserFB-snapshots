package frontier

import (
	"net/url"
	"testing"
)

func mustURL(raw string) *url.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u
}

func TestDeduplication(t *testing.T) {
	f := New()
	u := mustURL("https://example.com/page")

	if !f.Push(u, 0) {
		t.Fatal("expected first push to return true")
	}
	if f.Push(u, 0) {
		t.Fatal("expected duplicate push to return false")
	}
	// Bloom filter provides approximate count, should be close to 1
	count := f.VisitedCount()
	if count == 0 {
		t.Fatalf("expected visited count > 0, got %d", count)
	}
}

func TestPushPop(t *testing.T) {
	f := New()
	u1 := mustURL("https://example.com/a")
	u2 := mustURL("https://example.com/b")
	f.Push(u1, 0)
	f.Push(u2, 1)
	f.Close()

	item1, ok := f.Pop()
	if !ok || item1.URL.String() != u1.String() {
		t.Fatalf("unexpected item: %+v ok=%v", item1, ok)
	}
	item2, ok := f.Pop()
	if !ok || item2.Depth != 1 {
		t.Fatalf("unexpected item: %+v ok=%v", item2, ok)
	}
	_, ok = f.Pop()
	if ok {
		t.Fatal("expected Pop to return false on closed empty frontier")
	}
}

func TestFragmentNormalization(t *testing.T) {
	f := New()
	u1 := mustURL("https://example.com/page#section1")
	u2 := mustURL("https://example.com/page#section2")

	f.Push(u1, 0)
	if f.Push(u2, 0) {
		t.Fatal("URLs differing only by fragment should be deduplicated")
	}
}
