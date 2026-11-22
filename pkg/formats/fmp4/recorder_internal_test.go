package fmp4

import (
	"errors"
	"testing"
)

// TestRecorderSequenceExhaustion verifies that a non-empty commit is rejected
// once all 32-bit sequence numbers have been consumed.
func TestRecorderSequenceExhaustion(t *testing.T) {
	initData := &Init{Tracks: []*InitTrack{
		{
			ID:        7,
			TimeScale: 44100,
			Codec:     audioOnlyCodec(),
		},
	}}

	var f recordingMemFile
	rec, err := OpenRecorder(&f, initData)
	if err != nil {
		t.Fatal(err)
	}

	// simulate a history that already consumed every sequence number
	rec.nextSeq = 0

	// an empty commit is still free and performs no sequence allocation
	err = rec.Commit()
	if err != nil {
		t.Fatalf("empty commit with exhausted numbers must succeed, got %v", err)
	}

	err = rec.WriteSample(7, 0, &Sample{Duration: 1024, Payload: []byte{1, 2, 3}})
	if err != nil {
		t.Fatal(err)
	}

	err = rec.Commit()
	if !errors.Is(err, errSequenceExhausted) {
		t.Fatalf("expected sequence exhaustion, got %v", err)
	}

	// the sample stays queued, so every further non-empty commit is rejected the
	// same way; the recorder is not left in a terminal state
	err = rec.Commit()
	if !errors.Is(err, errSequenceExhausted) {
		t.Fatalf("second commit must keep returning exhaustion, got %v", err)
	}

	if rec.terminal {
		t.Fatal("exhaustion must not terminate the session")
	}
	if rec.nextSeq != 0 {
		t.Fatalf("nextSeq changed after rejected commit: %d", rec.nextSeq)
	}
	if len(rec.queues[7]) != 1 {
		t.Fatal("rejected commit must keep the queued sample")
	}
}
