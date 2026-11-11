package rtproto

import "testing"

func TestSeqDeltaModular(t *testing.T) {
	cases := []struct {
		name string
		a, b uint16
		want int
	}{
		{"plain forward", 100, 110, 10},
		{"wrap forward", 65534, 1, 3},
		{"wrap at boundary", 65535, 0, 1},
		{"backward reorder", 100, 97, -3},
		{"backward across zero", 0, 65535, -1},
		{"same", 1234, 1234, 0},
		{"large jump stays negative", 0, 0x8000, -32768},
		{"just inside half window", 0, 0x7fff, 32767},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := SeqDelta(tc.a, tc.b); got != tc.want {
				t.Fatalf("SeqDelta(%d,%d) = %d, want %d", tc.a, tc.b, got, tc.want)
			}
		})
	}
}

func TestSeqUnwrapperWraparound(t *testing.T) {
	u := NewSeqUnwrapper()
	// Start near the top of the space, wrap once, then feed a reordered packet.
	seqs := []uint16{65533, 65534, 65535, 0, 1, 2}
	want := []int64{65533, 65534, 65535, 65536, 65537, 65538}
	for i, s := range seqs {
		if got := u.Extend(s); got != want[i] {
			t.Fatalf("Extend(%d) step %d = %d, want %d", s, i, got, want[i])
		}
	}
	// A late packet belonging to the pre-wrap region keeps its old extended
	// value and does not move the high-water mark.
	late := u.Extend(65534)
	if late != 65534 {
		t.Fatalf("reordered Extend(65534) = %d, want 65534", late)
	}
	if got := u.Extend(3); got != 65539 {
		t.Fatalf("Extend(3) after reorder = %d, want 65539", got)
	}
}

func TestExpandNACKWordAndSet(t *testing.T) {
	// PID=10, BLP bits 0 and 2 set -> exact losses 10, 11, 13.
	r := ExpandNACKWord(10, 0b101)
	if r.Start != 10 || r.End != 13 {
		t.Fatalf("ExpandNACKWord span = %+v, want 10..13", r)
	}
	seqs := ExpandNACKWordSeqs(10, 0b101)
	wantSeqs := []uint16{10, 11, 13}
	if len(seqs) != len(wantSeqs) {
		t.Fatalf("exact NACK seqs = %v, want %v", seqs, wantSeqs)
	}
	for i, w := range wantSeqs {
		if seqs[i] != w {
			t.Fatalf("exact NACK seqs = %v, want %v", seqs, wantSeqs)
		}
	}
	// The dense range form includes the hole (12) inside the span.
	set := NACKSet([]NACKRange{r, {Start: 13, End: 14}})
	for _, s := range []uint16{10, 11, 12, 13, 14} {
		if !set[s] {
			t.Fatalf("dense set missing seq %d", s)
		}
	}
	if len(set) != 5 {
		t.Fatalf("dense set size = %d, want 5", len(set))
	}
}

func TestNACKSetWraparound(t *testing.T) {
	// Range crossing the 16-bit boundary terminates at the modular end.
	set := NACKSet([]NACKRange{{Start: 65534, End: 1}})
	want := []uint16{65534, 65535, 0, 1}
	if len(set) != len(want) {
		t.Fatalf("size = %d, want %d: %v", len(set), len(want), set)
	}
	for _, s := range want {
		if !set[s] {
			t.Fatalf("wrapped set missing %d: %v", s, set)
		}
	}
}

func TestMergeNACKSetsOverlap(t *testing.T) {
	// Two overlapping feedback intervals collapse without double counting.
	a := NACKSet([]NACKRange{{Start: 100, End: 105}})
	b := NACKSet([]NACKRange{{Start: 103, End: 108}})
	merged := MergeNACKSets(a, b)
	if len(merged) != 9 { // 100..108
		t.Fatalf("merged size = %d, want 9", len(merged))
	}
	// Inputs must not be mutated.
	if len(a) != 6 || len(b) != 6 {
		t.Fatalf("merge mutated inputs: %d %d", len(a), len(b))
	}
}

func TestTWCCTransportSeqs(t *testing.T) {
	f := &TWCCFeedback{
		BaseSequence: 65534,
		Runs:         []TWCCRun{{FirstTransportSeq: 65534, Count: 4}},
	}
	got := TWCCTransportSeqs(f)
	want := []uint16{65534, 65535, 0, 1}
	if len(got) != len(want) {
		t.Fatalf("got %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("index %d: got %d want %d", i, got[i], want[i])
		}
	}
}
