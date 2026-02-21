package frontier

import (
	"net/url"
	"testing"
)

func TestFrontierSaveRestore(t *testing.T) {
	// Create frontier and add some URLs
	f1 := New()

	urls := []string{
		"https://example.com/page1",
		"https://example.com/page2",
		"https://example.com/page3",
	}

	for i, rawURL := range urls {
		u, _ := url.Parse(rawURL)
		if !f1.Push(u, i) {
			t.Fatalf("Failed to push URL: %s", rawURL)
		}
	}

	// Try to push duplicate - should fail
	u, _ := url.Parse(urls[0])
	if f1.Push(u, 0) {
		t.Error("Expected duplicate push to fail")
	}

	// Save state
	snap, err := f1.Save()
	if err != nil {
		t.Fatalf("Save failed: %v", err)
	}

	if len(snap.Queue) != 3 {
		t.Errorf("Expected 3 items in queue, got %d", len(snap.Queue))
	}

	// Create new frontier and restore
	f2 := New()
	if err := f2.Restore(snap); err != nil {
		t.Fatalf("Restore failed: %v", err)
	}

	// Verify queue length
	if f2.Len() != 3 {
		t.Errorf("Expected queue length 3, got %d", f2.Len())
	}

	// Verify visited set works (duplicates should be rejected)
	u, _ = url.Parse(urls[0])
	if f2.Push(u, 0) {
		t.Error("Expected duplicate URL to be rejected after restore")
	}

	// Pop items and verify order
	for i := 0; i < 3; i++ {
		item, ok := f2.Pop()
		if !ok {
			t.Fatalf("Failed to pop item %d", i)
		}
		if item.Depth != i {
			t.Errorf("Expected depth %d, got %d", i, item.Depth)
		}
		if item.URL.String() != urls[i] {
			t.Errorf("Expected URL %s, got %s", urls[i], item.URL.String())
		}
	}
}

func TestFrontierRestoreEmpty(t *testing.T) {
	f1 := New()

	// Save empty frontier
	snap, err := f1.Save()
	if err != nil {
		t.Fatalf("Save failed: %v", err)
	}

	if len(snap.Queue) != 0 {
		t.Errorf("Expected empty queue, got %d items", len(snap.Queue))
	}

	// Restore to new frontier
	f2 := New()
	if err := f2.Restore(snap); err != nil {
		t.Fatalf("Restore failed: %v", err)
	}

	if f2.Len() != 0 {
		t.Errorf("Expected empty queue after restore, got %d", f2.Len())
	}
}

func TestFrontierRestoreWithFragments(t *testing.T) {
	f1 := New()

	// Add URLs with different fragments (should be deduplicated)
	urls := []string{
		"https://example.com/page#section1",
		"https://example.com/page#section2",
	}

	u1, _ := url.Parse(urls[0])
	if !f1.Push(u1, 0) {
		t.Fatal("Failed to push first URL")
	}

	u2, _ := url.Parse(urls[1])
	if f1.Push(u2, 0) {
		t.Error("Expected URL with different fragment to be deduplicated")
	}

	// Save and restore
	snap, err := f1.Save()
	if err != nil {
		t.Fatalf("Save failed: %v", err)
	}

	f2 := New()
	if err := f2.Restore(snap); err != nil {
		t.Fatalf("Restore failed: %v", err)
	}

	// Try to push the second URL again - should still be rejected
	if f2.Push(u2, 0) {
		t.Error("Expected duplicate URL (different fragment) to be rejected after restore")
	}
}
