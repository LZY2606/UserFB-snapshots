package beep_test

import (
	"errors"
	"reflect"
	"testing"

	"github.com/faiface/beep"
)

// dupBranchSentinel is the exact error value a failing scriptSource exposes after exhaustion.
// Tests assert both errors.Is and pointer identity, so it must survive Dup unchanged.
type dupBranchSentinel struct{ msg string }

func (e *dupBranchSentinel) Error() string { return e.msg }

var errDupBranch = &dupBranchSentinel{"dup branch test source failed"}

// dupBranchFrames is the deterministic payload used by most Dup branch tests.
var dupBranchFrames = [][2]float64{{1, -1}, {2, -2}, {3, -3}, {4, -4}}

// scriptSource is a deterministic Streamer. It hands out its frames (Err stays nil while the
// last frame is delivered) and, once exhausted, the next non-empty Stream returns 0, false and
// makes Err expose ferr (nil for a clean EOF). calls counts non-empty Stream invocations so
// tests can prove the source is never pulled again after it reported termination.
type scriptSource struct {
	frames [][2]float64
	ferr   error

	pos   int
	err   error
	calls int
}

func (s *scriptSource) Stream(samples [][2]float64) (n int, ok bool) {
	if len(samples) == 0 {
		return 0, true
	}
	s.calls++
	n = copy(samples, s.frames[s.pos:])
	s.pos += n
	if n == 0 {
		s.err = s.ferr
		return 0, false
	}
	return n, true
}

func (s *scriptSource) Err() error { return s.err }

// streamOne calls s.Stream with a fresh buffer of size req and returns the streamed frames.
func streamOne(s beep.Streamer, req int) (frames [][2]float64, ok bool) {
	buf := make([][2]float64, req)
	n, ok := s.Stream(buf)
	return buf[:n], ok
}

// requireStream asserts a single Stream call produces exactly want and ok.
func requireStream(t *testing.T, name string, s beep.Streamer, req int, want [][2]float64, ok bool) {
	t.Helper()
	got, gotOk := streamOne(s, req)
	if gotOk != ok {
		t.Fatalf("%s: Stream(%d) ok = %v, want %v", name, req, gotOk, ok)
	}
	if len(got) != 0 || len(want) != 0 {
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("%s: Stream(%d) frames = %v, want %v", name, req, got, want)
		}
	}
}

// requireDrained asserts s is drained: Stream returns 0, false and Err exposes wantErr with
// both errors.Is and identity preserved.
func requireDrained(t *testing.T, name string, s beep.Streamer, wantErr error) {
	t.Helper()
	if frames, ok := streamOne(s, 1); ok || len(frames) != 0 {
		t.Fatalf("%s: drained Streamer returned %v, %v, want 0, false", name, frames, ok)
	}
	err := s.Err()
	if !errors.Is(err, wantErr) {
		t.Fatalf("%s: errors.Is(Err(), wantErr) = false, Err() = %v", name, err)
	}
	if err != wantErr {
		t.Fatalf("%s: Err() = %v, not identical to the source error %v", name, err, wantErr)
	}
}

// TestDupBranchErrorDeferredUntilDrained reproduces the original bug: the fast branch observes
// the source failure while the slow branch has not read anything yet. The slow branch must keep
// Err nil and stream all of its buffered frames before it reports termination and exposes the
// source error.
func TestDupBranchErrorDeferredUntilDrained(t *testing.T) {
	src := &scriptSource{frames: dupBranchFrames, ferr: errDupBranch}
	fast, slow := beep.Dup(src)

	requireStream(t, "fast", fast, 4, dupBranchFrames, true)
	if src.calls != 1 {
		t.Fatalf("source called %d times, want 1", src.calls)
	}

	// The fast branch hits the failure.
	requireDrained(t, "fast", fast, errDupBranch)
	callsAfterTerm := src.calls

	// The slow branch has read nothing; the shared failure must not leak into it.
	if err := slow.Err(); err != nil {
		t.Fatalf("slow.Err() = %v before slow drained, want nil", err)
	}

	// The slow branch streams all four buffered frames, Err nil throughout.
	requireStream(t, "slow", slow, 2, dupBranchFrames[:2], true)
	if err := slow.Err(); err != nil {
		t.Fatalf("slow.Err() = %v with buffer non-empty, want nil", err)
	}
	requireStream(t, "slow", slow, 2, dupBranchFrames[2:], true)
	if err := slow.Err(); err != nil {
		t.Fatalf("slow.Err() = %v before slow returned 0, false, want nil", err)
	}

	// Only now the slow branch reports termination and exposes the original error.
	requireDrained(t, "slow", slow, errDupBranch)

	// The terminated source is never pulled again, by either branch.
	requireDrained(t, "fast again", fast, errDupBranch)
	requireDrained(t, "slow again", slow, errDupBranch)
	if src.calls != callsAfterTerm {
		t.Fatalf("source called %d times after termination, want %d", src.calls, callsAfterTerm)
	}
}

// TestDupBranchCleanEOF checks that a normal EOF seals the shared source the same way, except
// Err stays nil on both branches forever.
func TestDupBranchCleanEOF(t *testing.T) {
	src := &scriptSource{frames: dupBranchFrames} // ferr nil: clean EOF
	fast, slow := beep.Dup(src)

	requireStream(t, "fast", fast, 4, dupBranchFrames, true)
	requireStream(t, "fast", fast, 1, nil, false)
	if err := fast.Err(); err != nil {
		t.Fatalf("fast.Err() = %v after clean EOF, want nil", err)
	}
	callsAfterEOF := src.calls

	requireStream(t, "slow", slow, 3, dupBranchFrames[:3], true)
	requireStream(t, "slow", slow, 3, dupBranchFrames[3:], true)
	requireStream(t, "slow", slow, 1, nil, false)
	if err := slow.Err(); err != nil {
		t.Fatalf("slow.Err() = %v after clean EOF, want nil", err)
	}

	// Repeated EOF reads and the sibling's catch-up must not re-pull the source.
	requireStream(t, "fast", fast, 5, nil, false)
	requireStream(t, "slow", slow, 5, nil, false)
	if src.calls != callsAfterEOF {
		t.Fatalf("source called %d times after EOF, want %d", src.calls, callsAfterEOF)
	}
}

// TestDupBranchBufferExactFill checks that when the buffered frames exactly fill the request,
// the branch does not pull the source at all.
func TestDupBranchBufferExactFill(t *testing.T) {
	src := &scriptSource{frames: dupBranchFrames, ferr: errDupBranch}
	fast, slow := beep.Dup(src)

	requireStream(t, "fast", fast, 4, dupBranchFrames, true)
	calls := src.calls

	// The buffer holds exactly 4 frames; asking for 4 must not touch the source.
	requireStream(t, "slow", slow, 4, dupBranchFrames, true)
	if src.calls != calls {
		t.Fatalf("exact fill pulled the source: calls = %d, want %d", src.calls, calls)
	}
	if err := slow.Err(); err != nil {
		t.Fatalf("slow.Err() = %v, want nil", err)
	}

	// The next request finds the buffer empty and only then observes the termination.
	requireDrained(t, "slow", slow, errDupBranch)
}

// TestDupBranchBufferPartialFill checks the deferred-error path: a request larger than the
// buffer pulls the source for the remainder, the source fails, yet the already delivered PCM
// is returned as n > 0, true and the error surfaces only on the next non-empty request.
func TestDupBranchBufferPartialFill(t *testing.T) {
	src := &scriptSource{frames: dupBranchFrames, ferr: errDupBranch}
	fast, slow := beep.Dup(src)

	requireStream(t, "fast", fast, 4, dupBranchFrames, true)

	// Buffer holds 4 frames, request asks for 6: the source is pulled for the remaining 2,
	// fails, but the 4 buffered frames are still delivered with ok.
	requireStream(t, "slow", slow, 6, dupBranchFrames, true)
	if err := slow.Err(); err != nil {
		t.Fatalf("slow.Err() = %v after partial fill, want nil (error must be deferred)", err)
	}

	// The deferred termination is reported on the next non-empty request.
	requireDrained(t, "slow", slow, errDupBranch)
}

// TestDupBranchInterleavedBothDirections checks that both branches receive identical PCM no
// matter which branch leads and how the request sizes are chosen.
func TestDupBranchInterleavedBothDirections(t *testing.T) {
	scenarios := []struct {
		name     string
		firstReq []int // request sizes for the leading branch
		secondReq []int // request sizes for the lagging branch
	}{
		{"small_leads", []int{1, 3}, []int{2, 2}},
		{"large_leads", []int{4, 1}, []int{1, 1, 2}},
		{"alternating", []int{2, 2}, []int{3, 1}},
	}
	for _, swap := range []bool{false, true} {
		for _, sc := range scenarios {
			name := sc.name
			if swap {
				name += "_swapped"
			}
			t.Run(name, func(t *testing.T) {
				src := &scriptSource{frames: dupBranchFrames, ferr: errDupBranch}
				x, y := beep.Dup(src)
				lead, lag := x, y
				if swap {
					lead, lag = y, x
				}

				var leadGot, lagGot [][2]float64
				// Interleave: each lead request is followed by one lag request.
				for i := 0; i < len(sc.firstReq) || i < len(sc.secondReq); i++ {
					if i < len(sc.firstReq) {
						frames, _ := streamOne(lead, sc.firstReq[i])
						leadGot = append(leadGot, frames...)
					}
					if i < len(sc.secondReq) {
						frames, _ := streamOne(lag, sc.secondReq[i])
						lagGot = append(lagGot, frames...)
					}
				}
				// Drain both.
				for {
					frames, ok := streamOne(lead, 2)
					leadGot = append(leadGot, frames...)
					if !ok {
						break
					}
				}
				for {
					frames, ok := streamOne(lag, 2)
					lagGot = append(lagGot, frames...)
					if !ok {
						break
					}
				}

				if !reflect.DeepEqual(leadGot, dupBranchFrames) {
					t.Fatalf("leading branch got %v, want %v", leadGot, dupBranchFrames)
				}
				if !reflect.DeepEqual(lagGot, dupBranchFrames) {
					t.Fatalf("lagging branch got %v, want %v", lagGot, dupBranchFrames)
				}
				if err := lead.Err(); !errors.Is(err, errDupBranch) {
					t.Fatalf("lead.Err() = %v, want %v", err, errDupBranch)
				}
				if err := lag.Err(); !errors.Is(err, errDupBranch) {
					t.Fatalf("lag.Err() = %v, want %v", err, errDupBranch)
				}
			})
		}
	}
}

// TestDupBranchZeroLengthRequest pins down Stream(nil): before termination is exposed to the
// branch it returns 0, true without pulling the source, consuming the buffer, or surfacing a
// pending error; after the branch reported termination it returns 0, false. Querying Err never
// advances anything.
func TestDupBranchZeroLengthRequest(t *testing.T) {
	src := &scriptSource{frames: dupBranchFrames, ferr: errDupBranch}
	fast, slow := beep.Dup(src)

	// Zero-length request before anything happened.
	if n, ok := slow.Stream(nil); n != 0 || !ok {
		t.Fatalf("Stream(nil) = %d, %v, want 0, true", n, ok)
	}
	if src.calls != 0 {
		t.Fatalf("Stream(nil) pulled the source: calls = %d, want 0", src.calls)
	}

	requireStream(t, "fast", fast, 4, dupBranchFrames, true)
	requireDrained(t, "fast", fast, errDupBranch)

	// The source is terminated, but the slow branch has not been told yet: Stream(nil) stays
	// 0, true, consumes nothing, and does not surface the pending error.
	if n, ok := slow.Stream(nil); n != 0 || !ok {
		t.Fatalf("Stream(nil) with pending termination = %d, %v, want 0, true", n, ok)
	}
	if err := slow.Err(); err != nil {
		t.Fatalf("slow.Err() = %v after Stream(nil), want nil", err)
	}

	// Err queries do not advance the branch.
	for i := 0; i < 3; i++ {
		_ = slow.Err()
	}
	requireStream(t, "slow", slow, 4, dupBranchFrames, true)
	if err := slow.Err(); err != nil {
		t.Fatalf("slow.Err() = %v, want nil", err)
	}

	// Once the branch reported termination, Stream(nil) is 0, false.
	requireDrained(t, "slow", slow, errDupBranch)
	if n, ok := slow.Stream(nil); n != 0 || ok {
		t.Fatalf("Stream(nil) after termination = %d, %v, want 0, false", n, ok)
	}
}

// TestDupBranchNested checks that the termination/error ordering survives nesting: a Dup of a
// Dup branch behaves like any other source, with no first-level special cases.
func TestDupBranchNested(t *testing.T) {
	src := &scriptSource{frames: dupBranchFrames, ferr: errDupBranch}
	a, b := beep.Dup(src)
	c, d := beep.Dup(a)

	// c leads and runs into the failure, propagated through both Dup levels.
	requireStream(t, "c", c, 4, dupBranchFrames, true)
	requireDrained(t, "c", c, errDupBranch)
	callsAfterTerm := src.calls

	// d (same inner pair as c) and b (outer sibling) still hold all frames, Err nil.
	if err := d.Err(); err != nil {
		t.Fatalf("d.Err() = %v, want nil", err)
	}
	if err := b.Err(); err != nil {
		t.Fatalf("b.Err() = %v, want nil", err)
	}
	requireStream(t, "d", d, 4, dupBranchFrames, true)
	requireStream(t, "b", b, 4, dupBranchFrames, true)
	if err := d.Err(); err != nil {
		t.Fatalf("d.Err() = %v before draining, want nil", err)
	}
	if err := b.Err(); err != nil {
		t.Fatalf("b.Err() = %v before draining, want nil", err)
	}

	requireDrained(t, "d", d, errDupBranch)
	requireDrained(t, "b", b, errDupBranch)

	// The inner Dup sealed the source after its first termination; the outer branches'
	// catch-up never re-pulls it.
	if src.calls != callsAfterTerm {
		t.Fatalf("source called %d times after termination, want %d", src.calls, callsAfterTerm)
	}
}

// TestDupBranchTailAndBufferAliasing checks two memory rules: Stream never touches samples[n:]
// (tail sentinel survives), and the sibling buffer never aliases the caller's memory (clobbering
// a previously filled slice must not corrupt the other branch's future output).
func TestDupBranchTailAndBufferAliasing(t *testing.T) {
	src := &scriptSource{frames: dupBranchFrames, ferr: errDupBranch}
	fast, slow := beep.Dup(src)

	// Fast reads into a caller-owned slice, then the caller clobbers it.
	fastBuf := make([][2]float64, 4)
	n, ok := fast.Stream(fastBuf)
	if n != 4 || !ok {
		t.Fatalf("fast.Stream = %d, %v, want 4, true", n, ok)
	}
	for i := range fastBuf {
		fastBuf[i] = [2]float64{-999, 999}
	}

	// The slow branch must still see the original frames: the buffer copied, not borrowed.
	sentinel := [2]float64{777, -777}
	slowBuf := make([][2]float64, 6)
	for i := range slowBuf {
		slowBuf[i] = sentinel
	}
	n, ok = slow.Stream(slowBuf)
	if n != 4 || !ok {
		t.Fatalf("slow.Stream = %d, %v, want 4, true", n, ok)
	}
	if !reflect.DeepEqual(slowBuf[:n], dupBranchFrames) {
		t.Fatalf("slow frames = %v, want %v (buffer aliased caller memory?)", slowBuf[:n], dupBranchFrames)
	}
	// samples[n:] is untouched.
	for i := n; i < len(slowBuf); i++ {
		if slowBuf[i] != sentinel {
			t.Fatalf("slowBuf[%d] = %v, want untouched sentinel %v", i, slowBuf[i], sentinel)
		}
	}

	// A drained branch writes nothing at all.
	for i := range slowBuf {
		slowBuf[i] = sentinel
	}
	n, ok = slow.Stream(slowBuf)
	if n != 0 || ok {
		t.Fatalf("drained slow.Stream = %d, %v, want 0, false", n, ok)
	}
	for i := range slowBuf {
		if slowBuf[i] != sentinel {
			t.Fatalf("drained Stream wrote slowBuf[%d] = %v", i, slowBuf[i])
		}
	}
}

// TestDupBranchTakeAndMix consumes a normal Dup through Take and Mix as a graph-composition
// regression: no samples may be lost or duplicated on either branch.
func TestDupBranchTakeAndMix(t *testing.T) {
	t.Run("Take", func(t *testing.T) {
		src := &scriptSource{frames: dupBranchFrames}
		x, y := beep.Dup(src)

		gotX := collect(beep.Take(len(dupBranchFrames), x))
		gotY := collect(beep.Take(len(dupBranchFrames), y))

		if !reflect.DeepEqual(gotX, dupBranchFrames) {
			t.Fatalf("Take(x) = %v, want %v", gotX, dupBranchFrames)
		}
		if !reflect.DeepEqual(gotY, dupBranchFrames) {
			t.Fatalf("Take(y) = %v, want %v", gotY, dupBranchFrames)
		}
	})

	t.Run("Mix", func(t *testing.T) {
		src := &scriptSource{frames: dupBranchFrames}
		x, y := beep.Dup(src)

		got := collect(beep.Mix(x, y))

		want := make([][2]float64, len(dupBranchFrames))
		for i, f := range dupBranchFrames {
			want[i] = [2]float64{2 * f[0], 2 * f[1]}
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("Mix(x, y) = %v, want %v", got, want)
		}
	})
}
