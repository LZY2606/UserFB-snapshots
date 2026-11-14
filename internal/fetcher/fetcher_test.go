package fetcher

import (
	"context"
	"errors"
	"testing"
	"time"
)

func TestWaitReturnsAfterDelayWhenContextAlive(t *testing.T) {
	f := New(Config{MinDelay: 20 * time.Millisecond})

	if err := f.Wait(context.Background(), "example.com", 0); err != nil {
		t.Fatalf("first Wait should not block: %v", err)
	}
	start := time.Now()
	if err := f.Wait(context.Background(), "example.com", 0); err != nil {
		t.Fatalf("second Wait should sleep then succeed: %v", err)
	}
	if elapsed := time.Since(start); elapsed < 15*time.Millisecond {
		t.Fatalf("Wait returned before the politeness delay elapsed: %s", elapsed)
	}
}

func TestWaitInterruptibleByContext(t *testing.T) {
	f := New(Config{MinDelay: 10 * time.Second})

	// Prime the host so the next call actually parks in the delay.
	if err := f.Wait(context.Background(), "slow.example.com", 0); err != nil {
		t.Fatal(err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	go func() {
		time.Sleep(30 * time.Millisecond)
		cancel()
	}()

	start := time.Now()
	err := f.Wait(ctx, "slow.example.com", 0)
	elapsed := time.Since(start)

	if !errors.Is(err, context.Canceled) {
		t.Fatalf("want context.Canceled, got %v", err)
	}
	if elapsed > time.Second {
		t.Fatalf("Wait was not interrupted; slept %s for a 10s delay", elapsed)
	}
}

func TestWaitRejectsAlreadyCanceledContext(t *testing.T) {
	f := New(Config{MinDelay: time.Hour})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	start := time.Now()
	if err := f.Wait(ctx, "blocked.example.com", 0); !errors.Is(err, context.Canceled) {
		t.Fatalf("want context.Canceled, got %v", err)
	}
	if time.Since(start) > 100*time.Millisecond {
		t.Fatal("already-canceled context must return immediately")
	}
}
