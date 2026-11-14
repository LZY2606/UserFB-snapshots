package frontier

import (
	"context"
	"testing"
	"time"
)

func TestPopContextCancelWakesBlockedPop(t *testing.T) {
	f := New()
	ctx, cancel := context.WithCancel(context.Background())

	done := make(chan bool, 1)
	go func() {
		_, ok := f.PopContext(ctx)
		done <- ok
	}()

	select {
	case <-done:
		t.Fatal("PopContext returned before cancellation or an item was available")
	case <-time.After(50 * time.Millisecond):
	}

	cancel()
	select {
	case ok := <-done:
		if ok {
			t.Fatal("canceled PopContext must return ok=false")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("PopContext did not wake on context cancellation")
	}
}

func TestPopContextCloseWakesBlockedPop(t *testing.T) {
	f := New()
	done := make(chan bool, 1)
	go func() {
		_, ok := f.PopContext(context.Background())
		done <- ok
	}()

	time.Sleep(50 * time.Millisecond)
	f.Close()

	select {
	case ok := <-done:
		if ok {
			t.Fatal("closed PopContext must return ok=false")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("PopContext did not wake when the frontier closed")
	}
}

func TestPopContextRejectsQueuedItemAfterCancel(t *testing.T) {
	f := New()
	f.Push(mustURL("https://example.com/queued"), 0)
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	if _, ok := f.PopContext(ctx); ok {
		t.Fatal("PopContext must not admit a queued page after cancellation")
	}
	// The queued item is still available to an uncanceled Pop, proving the
	// rejection was driven by the context rather than queue state.
	item, ok := f.Pop()
	if !ok || item.URL.String() != "https://example.com/queued" {
		t.Fatalf("queued item lost after canceled PopContext: item=%+v ok=%v", item, ok)
	}
}

func TestPopContextDeliversItemBeforeCancel(t *testing.T) {
	f := New()
	f.Push(mustURL("https://example.com/ready"), 2)
	item, ok := f.PopContext(context.Background())
	if !ok || item.Depth != 2 || item.URL.String() != "https://example.com/ready" {
		t.Fatalf("unexpected PopContext result: item=%+v ok=%v", item, ok)
	}
}

func TestPushRejectedAfterClose(t *testing.T) {
	f := New()
	f.Close()
	if f.Push(mustURL("https://example.com/late"), 0) {
		t.Fatal("Push after Close must return false")
	}
}
