package store

import (
	"bytes"
	"context"
	"path/filepath"
	"testing"

	"rtpfreeze/internal/capture"
)

func openTestStore(t *testing.T) *Store {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.db")
	st, err := Open(path)
	if err != nil {
		t.Fatalf("open store: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

func importSample(t *testing.T, st *Store) *ImportResult {
	t.Helper()
	raw, err := capture.JSONL()
	if err != nil {
		t.Fatal(err)
	}
	res, err := st.Import(context.Background(), "sample.jsonl", bytes.NewReader(raw))
	if err != nil {
		t.Fatalf("import: %v", err)
	}
	return res
}

func TestImportSampleAndQueryRoundTrip(t *testing.T) {
	st := openTestStore(t)
	res := importSample(t, st)
	ctx := context.Background()

	if res.PacketCount == 0 || res.Generations < 4 {
		t.Fatalf("import summary too small: %+v", res)
	}
	if res.Freezes == 0 {
		t.Fatal("sample should produce at least one freeze interval")
	}

	caps, err := st.ListCaptures(ctx)
	if err != nil || len(caps) != 1 {
		t.Fatalf("ListCaptures = %d, err %v", len(caps), err)
	}

	o, err := st.Overview(ctx, res.CaptureID)
	if err != nil {
		t.Fatal(err)
	}
	// The sample spans an ICE restart, so at least two transport epochs.
	if len(o.Epochs) < 2 {
		t.Fatalf("want >=2 epochs, got %d", len(o.Epochs))
	}
	// The same SSRC value must appear in multiple generations.
	reused := map[uint32]int{}
	for _, g := range o.Generations {
		reused[g.SSRC]++
	}
	anyReused := false
	for _, n := range reused {
		if n > 1 {
			anyReused = true
		}
	}
	if !anyReused {
		t.Fatal("no SSRC reuse recorded; boundaries not partitioned")
	}

	rtpRows, err := st.RTPPackets(ctx, res.CaptureID, RTPFilter{GenID: -1, TrackID: -1, Limit: 10000})
	if err != nil {
		t.Fatal(err)
	}
	if len(rtpRows) == 0 || rtpRows[0].PacketIndex != rtpRows[0].PacketIndex {
		t.Fatal("rtp rows missing")
	}
	// Original indices come back in ascending order.
	for i := 1; i < len(rtpRows); i++ {
		if rtpRows[i].PacketIndex <= rtpRows[i-1].PacketIndex {
			t.Fatalf("rtp rows not ordered by original index at %d", i)
		}
	}

	fb, err := st.Feedback(ctx, res.CaptureID, FeedbackFilter{GenID: -1, Limit: 5000})
	if err != nil {
		t.Fatal(err)
	}
	if len(fb) == 0 {
		t.Fatal("no feedback rows persisted")
	}
	ambiguous := 0
	for _, ev := range fb {
		if len(ev.Candidates) == 0 {
			t.Fatalf("feedback %d has no candidates row", ev.Position)
		}
		if ev.Status == "ambiguous" || ev.Status == "cross_epoch" {
			ambiguous++
		}
	}
	if ambiguous == 0 {
		t.Fatal("sample should contain ambiguous/cross-epoch feedback")
	}

	freezes, err := st.Freezes(ctx, res.CaptureID)
	if err != nil {
		t.Fatal(err)
	}
	if len(freezes) != res.Freezes {
		t.Fatalf("freeze count mismatch: %d vs %d", len(freezes), res.Freezes)
	}

	srs, err := st.SenderReports(ctx, res.CaptureID)
	if err != nil {
		t.Fatal(err)
	}
	if len(srs) == 0 {
		t.Fatal("sender report mapping not persisted")
	}
}

func TestRawPacketPreservesOriginalRecord(t *testing.T) {
	st := openTestStore(t)
	res := importSample(t, st)
	raw, err := st.RawPacket(context.Background(), res.CaptureID, 0)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Contains([]byte(raw), []byte(`"index":0`)) {
		t.Fatalf("raw packet lost its original index: %s", raw)
	}
}

func TestReImportIsIndependent(t *testing.T) {
	st := openTestStore(t)
	first := importSample(t, st)
	second := importSample(t, st)
	if first.CaptureID == second.CaptureID {
		t.Fatal("re-import must create a new capture id")
	}
	o1, err := st.Overview(context.Background(), first.CaptureID)
	if err != nil {
		t.Fatal(err)
	}
	o2, err := st.Overview(context.Background(), second.CaptureID)
	if err != nil {
		t.Fatal(err)
	}
	if len(o1.Generations) != len(o2.Generations) {
		t.Fatal("deterministic import produced different partitions")
	}
}
