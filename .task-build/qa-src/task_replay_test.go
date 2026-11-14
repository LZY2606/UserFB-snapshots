// Copyright 2026 Gregory Petrosyan <gregory.petrosyan@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

package rapid

import (
	"context"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"pgregory.net/rapid/task_support/taskcheck"
)

// This file is a self-contained, benign test fixture. It exercises only
// rapid's own property machinery (shrinking, fail-file persistence and the
// fuzz byte bridge); it does not modify production code or probe any
// third-party program.

const (
	taskSeed      = uint64(1815)
	taskChecks    = 30
	taskFailMsg   = "task replay invariant"
	taskLevelMax  = 15
	taskLevelA    = 2 // site A fires at level sum >= 2
	taskPairCount = 3
)

type taskGateMode int

const (
	gateFailing taskGateMode = iota // the property keeps its two real failure sites
	gateSkip                        // every replay is an invalid (skipped) test case
)

// taskPair is the nested value the fixture property works with. level is a
// plain 4-bit word so block minimization walks the values 0,1,2,... in
// order; marker carries the nested/discardable-attempt structure but does
// not influence which failure site fires.
type taskPair struct {
	level  uint8
	marker int
}

type taskRecording struct {
	pairs   []taskPair
	visited [2]int
}

var (
	taskMu        sync.Mutex
	taskGate      = gateFailing
	taskCalls     int
	taskOrigins   [2]int
	taskRec       *taskRecording
	taskTracePath string
)

func taskResetState() {
	taskMu.Lock()
	defer taskMu.Unlock()
	taskGate = gateFailing
	taskCalls = 0
	taskOrigins = [2]int{}
	taskRec = nil
	taskTracePath = ""
}
func taskRecordCall() {
	taskMu.Lock()
	taskCalls++
	taskMu.Unlock()
}

func taskRecordOrigin(ix int) {
	taskMu.Lock()
	taskOrigins[ix]++
	if taskRec != nil {
		taskRec.visited[ix]++
	}
	path := taskTracePath
	taskMu.Unlock()
	// In bridge-helper ("main") runs the property itself is responsible for
	// leaving an observable origin trace.
	if path != "" {
		f, err := os.OpenFile(path, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0600)
		if err == nil {
			fmt.Fprintf(f, "origin=%d\n", ix)
			_ = f.Close()
		}
	}
}

// taskLevelGen draws one level as a plain 4-bit word: exactly one bitstream
// word per value, so the shrinker's block minimizer walks candidate values
// 0, 1, 2, ... in order against this word.
var taskLevelGen = Custom(func(t *T) uint8 {
	return uint8(t.s.drawBits(4))
})

// taskMarkerGen carries the nested/discardable-attempt structure of the
// fixture without influencing the failure sites: drawing index 2 rejects
// the value and discards the surrounding "try" group.
var taskMarkerGen = SampledFrom([]int{0, 1, 2}).Filter(func(v int) bool {
	return v != 2
})

// taskPairGen nests a one-word level inside a Custom group which also
// contains a filtered, retry-capable marker.
var taskPairGen = Custom(func(t *T) taskPair {
	level := taskLevelGen.Draw(t, "level")
	marker := taskMarkerGen.Draw(t, "marker")
	return taskPair{level: level, marker: marker}
})

var taskPairsGen = SliceOfN(taskPairGen, taskPairCount, taskPairCount)

// taskFailSiteA and taskFailSiteB fail with the identical message but live
// on different source lines, so their tracebacks differ while errorString
// stays the same.
func taskFailSiteA(t *T) {
	t.Fatalf(taskFailMsg)
}

func taskFailSiteB(t *T) {
	t.Fatalf(taskFailMsg)
}

// taskReplayProp is the property under test. With levels L0..L2 in 0..15
// (see taskModelClassify):
//
//   - site A fires once S = L0+L1+L2 >= 2;
//   - site B fires when S == 1;
//   - S == 0 passes.
//
// Because every level is its own bitstream word, minimizing a level walks
// through S=2 (site A), S=1 (site B) and S=0 (passing) in order. The B
// witness is strictly smaller than every A witness while carrying the same
// message, which is exactly what the shrinker traceback check has to guard
// against.
func taskReplayProp(t *T) {
	taskRecordCall()

	pairs := taskPairsGen.Draw(t, "pairs")

	if taskRec != nil {
		taskRec.pairs = append(taskRec.pairs, pairs...)
	}

	if taskGate == gateSkip {
		t.Skip("task property switched to skip mode")
	}

	sum := uint8(0)
	for _, p := range pairs {
		sum += p.level
	}
	if sum >= taskLevelA {
		taskRecordOrigin(0)
		taskFailSiteA(t)
	}
	if sum == 1 {
		taskRecordOrigin(1)
		taskFailSiteB(t)
	}
}

// -----------------------------------------------------------------------
// Independent finite semantic model. It knows nothing about rapid's search
// order: it enumerates the whole finite value domain of the property.
// -----------------------------------------------------------------------

type taskModelPair struct {
	level uint8
}

// taskModelClassify returns (origin, fails): origin 0 is site A, origin 1
// is site B.
func taskModelClassify(pairs [taskPairCount]taskModelPair) (int, bool) {
	sum := uint8(0)
	for _, p := range pairs {
		sum += p.level
	}
	if sum >= taskLevelA {
		return 0, true
	}
	if sum == 1 {
		return 1, true
	}
	return -1, false
}

// taskModelMinimumWitness is derived by hand from the semantics, not from a
// rapid run: the cheapest site-A witness has a total level sum of exactly
// 2; the profile (1, 1, 0) is one such minimum, unique up to pair ordering.
func taskModelMinimumWitness() [taskPairCount]taskModelPair {
	return [taskPairCount]taskModelPair{{1}, {1}, {0}}
}

// taskModelVerify exhaustively enumerates the finite domain (levels 0..15,
// three pairs, 4096 inputs) and checks that:
//   - the hand-derived minimum is an A-witness;
//   - no A-witness is cheaper (smaller level sum) than the hand minimum;
//   - B-witnesses exist and every one of them is strictly cheaper than any
//     A-witness (so a traceback-blind shrinker could be lured onto the
//     wrong failure origin);
//   - passing inputs exist as well.
func taskModelVerify() string {
	want := taskModelMinimumWitness()
	if origin, fails := taskModelClassify(want); !fails || origin != 0 {
		return "hand-derived minimum is not a site-A witness"
	}

	wantSum := uint8(0)
	for _, p := range want {
		wantSum += p.level
	}

	var (
		aCount, bCount, passCount int
		minASum                   = uint8(255)
		maxBSum                   = uint8(0)
	)
	for l0 := 0; l0 <= taskLevelMax; l0++ {
		for l1 := 0; l1 <= taskLevelMax; l1++ {
			for l2 := 0; l2 <= taskLevelMax; l2++ {
				ps := [taskPairCount]taskModelPair{{uint8(l0)}, {uint8(l1)}, {uint8(l2)}}
				origin, fails := taskModelClassify(ps)
				if !fails {
					passCount++
					continue
				}
				sum := uint8(l0 + l1 + l2)
				switch origin {
				case 0:
					aCount++
					if sum < minASum {
						minASum = sum
					}
					if sum < wantSum {
						return fmt.Sprintf("found A-witness cheaper than the hand-derived minimum: %+v", ps)
					}
				case 1:
					bCount++
					if sum > maxBSum {
						maxBSum = sum
					}
				}
			}
		}
	}

	if aCount == 0 || bCount == 0 || passCount == 0 {
		return fmt.Sprintf("degenerate domain: A=%d B=%d pass=%d", aCount, bCount, passCount)
	}
	if minASum != wantSum {
		return fmt.Sprintf("enumerated minimum A sum %d differs from derived %d", minASum, wantSum)
	}
	if maxBSum >= minASum {
		return fmt.Sprintf("B-witnesses are not strictly cheaper: max B sum %d vs min A sum %d", maxBSum, minASum)
	}
	return ""
}

// taskRefTraceback builds the traceback of one failure site in isolation.
func taskRefTraceback(t *testing.T, fail func(*T)) string {
	t.Helper()
	err := checkOnce(newT(taskShieldTB{TB: t}, newBufBitStream(nil, false), false, nil), func(t *T) {
		fail(t)
	})
	if err == nil {
		t.Fatalf("reference failure site did not fail")
	}
	return traceback(err)
}

// taskOriginOf identifies the failure source from the traceback's leaf
// frame names rather than from a full string match: real replay tracebacks
// carry extra property frames, but the site function name is invariant.
func taskOriginOf(err *testError) int {
	if err == nil {
		return -1
	}
	tb := traceback(err)
	switch {
	case strings.Contains(tb, "taskFailSiteA"):
		return 0
	case strings.Contains(tb, "taskFailSiteB"):
		return 1
	default:
		return -1
	}
}

// taskShieldTB lets rapid internals run against a TB that never reports
// itself as failed: property failures are returned as *testError values and
// judged through c.Check, instead of tripping doCheck's !tb.Failed()
// assertion and turning a content failure into a panic. Logs still flow to
// the real test output.
type taskShieldTB struct {
	TB
}

func (taskShieldTB) Helper()                           {}
func (taskShieldTB) Failed() bool                      { return false }
func (taskShieldTB) Fail()                             {}
func (taskShieldTB) FailNow()                          {}
func (taskShieldTB) Error(...any)                      {}
func (taskShieldTB) Fatal(...any)                      {}
func (taskShieldTB) Errorf(string, ...any)             {}
func (taskShieldTB) Fatalf(string, ...any)             {}
func (taskShieldTB) Skip(...any)                       {}
func (taskShieldTB) Skipf(string, ...any)              {}
func (taskShieldTB) SkipNow()                          {}
func (s taskShieldTB) Logf(format string, args ...any) { s.TB.Logf(format, args...) }
func (s taskShieldTB) Log(args ...any)                 { s.TB.Log(args...) }

// taskRunShrink drives the real doCheck pipeline with a fixed seed and a
// bounded number of checks, returning the shrunk word stream together with
// the two reproduced errors.
func taskRunShrink(t *testing.T) ([]uint64, uint64, *testError, *testError) {
	t.Helper()
	tb := taskShieldTB{TB: t}
	valid, invalid, _, seed, failfile, buf, err1, err2 := doCheck(
		tb, checkDeadline(t), taskChecks, taskSeed, "", false, taskReplayProp)
	if err1 == nil || err2 == nil {
		t.Fatalf("doCheck did not find a failure: valid=%d invalid=%d", valid, invalid)
	}
	if failfile != "" {
		t.Fatalf("unexpected fail file %q", failfile)
	}
	if seed == 0 {
		t.Fatalf("doCheck returned a zero failure seed")
	}
	return buf, seed, err1, err2
}

// taskReplay runs a word stream through the property and collects the
// concrete generated values and the failure origin.
func taskReplay(t *testing.T, buf []uint64) (*taskRecording, *testError) {
	t.Helper()
	rec := &taskRecording{}
	taskMu.Lock()
	taskRec = rec
	taskMu.Unlock()
	defer func() {
		taskMu.Lock()
		taskRec = nil
		taskMu.Unlock()
	}()

	err := checkOnce(newT(taskShieldTB{TB: t}, newBufBitStream(buf, false), false, nil), taskReplayProp)
	return rec, err
}

// -----------------------------------------------------------------------
// TestTaskShrinkIdentity: shrinker must preserve the failure origin.
// -----------------------------------------------------------------------

func TestTaskShrinkIdentity(t *testing.T) {
	taskResetState()

	taskcheck.Run(t, func(c *taskcheck.C) {
		// The stream layer really does support nested groups with discarded
		// attempts: begin/endGroup(..., discard=true) marks a "try" group
		// whose words prune() later drops, exactly as Filter retries do.
		s := newBufBitStream([]uint64{7, 0}, true)
		i := s.beginGroup(tryLabel, false)
		u1 := s.drawBits(3)
		s.endGroup(i, true) // discarded attempt
		j := s.beginGroup(tryLabel, false)
		u2 := s.drawBits(3)
		s.endGroup(j, false) // accepted attempt
		c.Check(u1 == 7 && u2 == 0, "both attempts draw their words in order")
		sawDiscard := false
		for _, g := range s.recordedBits.groups {
			if g.discard {
				sawDiscard = true
			}
		}
		c.Check(sawDiscard, "rejected attempt is recorded as a discarded group")
		c.Check(len(s.recordedBits.data) == 2, "both words present before pruning")
		s.recordedBits.prune()
		c.Check(len(s.recordedBits.data) == 1 && s.recordedBits.data[0] == 0,
			"pruning drops the discarded group and keeps the accepted word")
		c.Check(len(s.recordedBits.groups) == 1 && !s.recordedBits.groups[0].discard,
			"only the accepted nested group survives pruning")

		// Sanity-check the independent finite model before relying on it.
		c.Check(taskModelVerify() == "", "finite semantic model is self-consistent: %s", taskModelVerify())

		valid, invalid, _, seed, failfile, buf, err1, err2 := doCheck(
			taskShieldTB{TB: t}, checkDeadline(t), taskChecks, taskSeed, "", false, taskReplayProp)

		c.Check(seed != 0 && err1 != nil && err2 != nil, "real doCheck finds and reproduces a failure")
		c.Check(failfile == "", "no fail file is involved at this stage, got %q", failfile)
		c.Check(valid >= 0 && invalid >= 0 && valid+invalid < taskChecks,
			"search stayed within the bounded check budget: valid=%d invalid=%d budget=%d", valid, invalid, taskChecks)
		c.Check(traceback(err1) == traceback(err2), "reproduced failure has a stable traceback")
		c.Check(errorString(err1) == errorString(err2) && err1.Error() == taskFailMsg,
			"reproduced failure keeps the same message %q", err1.Error())
		c.Check(len(buf) > 0 && len(buf) <= 40, "shrunk word stream is non-empty and compact: %d words", len(buf))

		tbA := taskRefTraceback(t, taskFailSiteA)
		tbB := taskRefTraceback(t, taskFailSiteB)
		c.Check(strings.Contains(tbA, "taskFailSiteA") && strings.Contains(tbB, "taskFailSiteB"),
			"reference tracebacks point at their own failure site")
		c.Check(tbA != tbB, "the two same-message failure sites have distinct tracebacks")
		c.Check(errorString(err1) == taskFailMsg && errorString(err2) == taskFailMsg,
			"both sites share the identical message")
		c.Check(taskOriginOf(err2) == 0, "shrinker preserved the original site-A origin")
		c.Check(taskOriginOf(err2) != 1, "shrunk witness did not migrate to the site-B origin")

		// Independently replay the final word stream and inspect what it
		// actually generates, rather than trusting the shrinker's own bookkeeping.
		rec, err := taskReplay(t, buf)
		c.Check(err != nil && !err.isInvalidData(), "final stream replays as a real failure, not invalid data")
		c.Check(err != nil && err.Error() == taskFailMsg, "replayed failure carries the same message")
		c.Check(taskOriginOf(err) == 0, "replayed failure originates at site A")
		c.Check(rec.visited[0] == 1 && rec.visited[1] == 0,
			"exactly one site-A hit and no site-B hit on replay: %+v", rec.visited)
		c.Check(len(rec.pairs) == taskPairCount, "replay generates the nested three-pair value")

		// Compare the replayed values against the independently derived
		// minimum witness (order-independent level profile).
		want := taskModelMinimumWitness()
		c.Check(len(rec.pairs) == len(want), "witness pair count matches model")
		gotLevelSum := uint8(0)
		for _, p := range rec.pairs {
			gotLevelSum += p.level
			c.Check(p.level <= taskLevelMax, "replayed level stays in the model domain")
		}
		wantLevelSum := uint8(0)
		for _, p := range want {
			wantLevelSum += p.level
		}
		c.Check(gotLevelSum == wantLevelSum,
			"shrunk witness has the minimum level sum for site A: got %d want %d", gotLevelSum, wantLevelSum)
		nonZero := 0
		for _, p := range rec.pairs {
			if p.level != 0 {
				nonZero++
			}
		}
		c.Check(nonZero >= 1 && nonZero <= 2, "level sum 2 is carried by one or two pairs: %d non-zero", nonZero)
		c.Check(gotLevelSum == taskLevelA, "the site-A boundary sum 2 is reproduced")

		// The same stream must fail identically a second time (word-stream
		// replay stability, independent of the shrinker internals).
		rec2, err2b := taskReplay(t, buf)
		c.Check(err2b != nil && traceback(err2b) == traceback(err), "second replay gives the same traceback")
		c.Check(reflect.DeepEqual(rec2.pairs, rec.pairs), "second replay generates identical values")
	})
}

// -----------------------------------------------------------------------
// Log-recording TB wrapper, used to distinguish "not executed", "ignored
// after invalidation" and "failure reproduced" through logs and return values.
// -----------------------------------------------------------------------

type recordingTB struct {
	taskShieldTB
	mu   sync.Mutex
	logs []string
}

func newRecordingTB(t TB) *recordingTB {
	return &recordingTB{taskShieldTB: taskShieldTB{TB: t}}
}

func (r *recordingTB) Helper() {}

func (r *recordingTB) Logf(format string, args ...any) {
	r.mu.Lock()
	r.logs = append(r.logs, fmt.Sprintf(format, args...))
	r.mu.Unlock()
}

func (r *recordingTB) Log(args ...any) {
	r.mu.Lock()
	r.logs = append(r.logs, fmt.Sprint(args...))
	r.mu.Unlock()
}

func (r *recordingTB) logText() string {
	r.mu.Lock()
	defer r.mu.Unlock()
	return strings.Join(r.logs, "\n")
}

// -----------------------------------------------------------------------
// TestTaskReplayFiles: fail-file save/load and replay evidence chain.
// -----------------------------------------------------------------------

func TestTaskReplayFiles(t *testing.T) {
	taskResetState()

	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()

		// Phase 1: take the shrinker's evidence and persist it for real.
		buf, seed, err1, err2 := taskRunShrink(t)
		c.Check(err1.Error() == taskFailMsg, "inherited shrink evidence keeps the shared message")
		c.Check(taskOriginOf(err2) == 0, "inherited shrink evidence fails at site A")

		failPath := filepath.Join(dir, "evidence.fail")
		out := captureTestOutput(t, taskReplayProp, buf)
		c.Check(len(out) > 0, "captureTestOutput produced a replay log")
		c.Check(saveFailFile(failPath, rapidVersion, out, seed, buf) == nil,
			"fail file saved under the temporary directory")

		version2, seed2, buf2, loadErr := loadFailFile(failPath)
		c.Check(loadErr == nil, "saved fail file loads back")
		c.Check(version2 == rapidVersion && seed2 == seed, "version and seed round-trip")
		c.Check(reflect.DeepEqual(buf2, buf), "word stream round-trips byte for word")

		// Phase 2: checkFailFile replays the file twice and confirms the
		// same failure both times.
		rtb := newRecordingTB(t)
		taskMu.Lock()
		taskCalls = 0
		taskOrigins = [2]int{}
		taskMu.Unlock()
		gotBuf, fErr1, fErr2 := checkFailFile(rtb, failPath, taskReplayProp)
		c.Check(fErr1 != nil && fErr2 != nil, "checkFailFile reproduces the failure twice")
		c.Check(reflect.DeepEqual(gotBuf, buf), "checkFailFile hands back the same stream")
		c.Check(taskOriginOf(fErr1) == 0 && taskOriginOf(fErr2) == 0,
			"both fail-file replays keep the site-A origin")
		c.Check(fErr1.Error() == taskFailMsg, "replayed failure message is unchanged")
		taskMu.Lock()
		callsAfterCheck := taskCalls
		originsAfterCheck := taskOrigins
		taskMu.Unlock()
		c.Check(callsAfterCheck == 2, "property ran exactly twice in checkFailFile, got %d", callsAfterCheck)
		c.Check(originsAfterCheck == [2]int{2, 0}, "both runs hit site A: %+v", originsAfterCheck)
		c.Check(rtb.logText() == "", "no warnings logged for a valid reproducing fail file")

		// Phase 3: doCheck with an explicit fail file stops at replay and
		// never starts a fresh random search.
		taskMu.Lock()
		taskCalls = 0
		taskMu.Unlock()
		valid, invalid, early, retSeed, retFile, retBuf, dErr1, dErr2 := doCheck(
			rtb, checkDeadline(t), taskChecks, seed, failPath, false, taskReplayProp)
		c.Check(dErr1 != nil && dErr2 != nil, "doCheck confirms the failure from the fail file")
		c.Check(retFile == failPath, "doCheck reports the fail file as the failure source, got %q", retFile)
		c.Check(valid == 0 && invalid == 0 && !early && retSeed == 0,
			"random search never ran: valid=%d invalid=%d", valid, invalid)
		c.Check(reflect.DeepEqual(retBuf, buf), "doCheck returns the fail-file stream")
		c.Check(taskOriginOf(dErr1) == 0 && taskOriginOf(dErr2) == 0, "doCheck replay stays at site A")
		taskMu.Lock()
		callsAfterDo := taskCalls
		taskMu.Unlock()
		c.Check(callsAfterDo == 2, "doCheck executed exactly the two fail-file replays, got %d", callsAfterDo)

		// Phase 4: property switched to skipping -> old file is invalid and
		// the normal bounded check continues executing.
		taskMu.Lock()
		taskGate = gateSkip
		taskCalls = 0
		taskMu.Unlock()
		sBuf, sErr1, sErr2 := checkFailFile(rtb, failPath, taskReplayProp)
		c.Check(sBuf == nil && sErr1 == nil && sErr2 == nil, "skipped replay invalidates the fail file")
		c.Check(strings.Contains(rtb.logText(), "no longer valid"),
			"invalidation is explained in the logs: %q", rtb.logText())
		taskMu.Lock()
		skipCalls := taskCalls
		taskMu.Unlock()
		c.Check(skipCalls == 1, "only the first validation replay ran, got %d", skipCalls)

		taskMu.Lock()
		taskCalls = 0
		taskMu.Unlock()
		sValid, sInvalid, _, sSeed, sFile, _, sDoErr1, sDoErr2 := doCheck(
			rtb, checkDeadline(t), taskChecks, seed, failPath, false, taskReplayProp)
		c.Check(sDoErr1 == nil && sDoErr2 == nil, "after ignoring the stale file no failure is reported")
		c.Check(sFile == "" && sSeed == 0, "no failure source is returned")
		c.Check(sValid == 0 && sInvalid == taskChecks*invalidChecksMult,
			"normal check kept running until the invalid-case budget: valid=%d invalid=%d", sValid, sInvalid)
		taskMu.Lock()
		skipSearchCalls := taskCalls
		taskMu.Unlock()
		c.Check(skipSearchCalls == 1+taskChecks*invalidChecksMult,
			"one validation replay plus a full bounded skipped search: %d", skipSearchCalls)
		taskMu.Lock()
		taskGate = gateFailing
		taskMu.Unlock()

		// Phase 5: hand-written text fixtures (not produced by saveFailFile).
		hexWords := make([]string, len(buf))
		for i, u := range buf {
			hexWords[i] = fmt.Sprintf("0x%x", u)
		}
		commentFixture := filepath.Join(dir, "comments.fail")
		content := "# hand written fixture\n# second comment line\n\n" +
			rapidVersion + "#0\n" + strings.Join(hexWords, "\n") + "\n"
		c.Check(os.WriteFile(commentFixture, []byte(content), 0600) == nil,
			"comment-prefixed fixture written")
		cBuf, cErr1, cErr2 := checkFailFile(newRecordingTB(t), commentFixture, taskReplayProp)
		c.Check(cErr1 != nil && cErr2 != nil && reflect.DeepEqual(cBuf, buf),
			"comments and blank lines are ignored, failure still reproduced")

		badVersion := filepath.Join(dir, "version.fail")
		c.Check(os.WriteFile(badVersion, []byte("v0.0.1#0\n"+strings.Join(hexWords, "\n")+"\n"), 0600) == nil,
			"version-mismatch fixture written")
		vtb := newRecordingTB(t)
		vBuf, vErr1, vErr2 := checkFailFile(vtb, badVersion, taskReplayProp)
		c.Check(vBuf == nil && vErr1 == nil && vErr2 == nil, "version mismatch is not replayed")
		c.Check(strings.Contains(vtb.logText(), "version"), "version mismatch is logged: %q", vtb.logText())

		badWord := filepath.Join(dir, "badword.fail")
		c.Check(os.WriteFile(badWord, []byte(rapidVersion+"#0\n0xnotanumber\n"), 0600) == nil,
			"invalid-word fixture written")
		wtb := newRecordingTB(t)
		wBuf, wErr1, wErr2 := checkFailFile(wtb, badWord, taskReplayProp)
		c.Check(wBuf == nil && wErr1 == nil && wErr2 == nil, "invalid word is not replayed")
		c.Check(strings.Contains(wtb.logText(), "ignoring fail file"), "invalid word is logged: %q", wtb.logText())

		etb := newRecordingTB(t)
		eBuf, eErr1, eErr2 := checkFailFile(etb, filepath.Join(dir, "does-not-exist.fail"), taskReplayProp)
		c.Check(eBuf == nil && eErr1 == nil && eErr2 == nil, "unreadable file is not replayed")
		c.Check(strings.Contains(etb.logText(), "ignoring fail file"), "read failure is logged: %q", etb.logText())

		emptyFile := filepath.Join(dir, "empty.fail")
		c.Check(os.WriteFile(emptyFile, []byte("# only comments\n"), 0600) == nil, "empty fixture written")
		ntb := newRecordingTB(t)
		nBuf, nErr1, nErr2 := checkFailFile(ntb, emptyFile, taskReplayProp)
		c.Check(nBuf == nil && nErr1 == nil && nErr2 == nil, "comment-only file is not replayed")
		c.Check(strings.Contains(ntb.logText(), "ignoring fail file"), "empty file is logged: %q", ntb.logText())

		// Phase 6: glob-based discovery from a temporary working directory.
		wd, wdErr := os.Getwd()
		c.Check(wdErr == nil, "working directory recorded")
		c.Check(os.Chdir(dir) == nil, "chdir into temporary directory")
		t.Cleanup(func() { _ = os.Chdir(wd) })
		globDir, globPath := failFileName(t.Name())
		c.Check(saveFailFile(globPath, rapidVersion, nil, seed, buf) == nil,
			"fail file laid down at the glob location")
		gValid, gInvalid, _, gSeed, gFile, gBuf, gErr1, gErr2 := doCheck(
			taskShieldTB{TB: t}, checkDeadline(t), taskChecks, seed, "", true, taskReplayProp)
		c.Check(gErr1 != nil && gErr2 != nil, "glob discovery finds and replays the failure")
		c.Check(gFile != "" && strings.HasPrefix(gFile, globDir), "discovered file path reported: %q", gFile)
		c.Check(reflect.DeepEqual(gBuf, buf), "glob replay uses the saved stream")
		c.Check(taskOriginOf(gErr1) == 0 && taskOriginOf(gErr2) == 0, "glob replay keeps site A")
		c.Check(gValid == 0 && gInvalid == 0 && gSeed == 0, "glob replay short-circuits the random search")
	})
}

// -----------------------------------------------------------------------
// TestTaskByteBridge: independent little-endian encoding through MakeFuzz
// in an isolated test subprocess.
// -----------------------------------------------------------------------

const (
	bridgeHelperEnv  = "TASK_BRIDGE_HELPER"
	bridgeCaseEnv    = "TASK_BRIDGE_CASE"
	bridgeGateEnv    = "TASK_BRIDGE_GATE"
	bridgeDrawsEnv   = "TASK_BRIDGE_DRAWS"
	bridgeInputEnv   = "TASK_BRIDGE_INPUT"
	bridgeTraceEnv   = "TASK_BRIDGE_TRACE"
	bridgePassMarker = "BRIDGE_CHILD_DONE"
	bridgeSkipMarker = "BRIDGE_CHILD_SKIP"
)

// taskEncodeLE is the test's own little-endian encoder, defined
// independently of engine.checkFuzz.
func taskEncodeLE(words []uint64) []byte {
	b := make([]byte, 8*len(words))
	for i, w := range words {
		binary.LittleEndian.PutUint64(b[i*8:], w)
	}
	return b
}

func taskWriteTrace(path string, words []uint64, marker string) {
	if path == "" {
		return
	}
	lines := make([]string, 0, len(words)+1)
	for _, w := range words {
		lines = append(lines, fmt.Sprintf("0x%x", w))
	}
	if marker != "" {
		lines = append(lines, marker)
	}
	_ = os.WriteFile(path, []byte(strings.Join(lines, "\n")+"\n"), 0600)
}

// taskBridgeRawProp draws whole 64-bit words straight from the stream, so
// the recorded trace is exactly the decoded word stream: it makes byte-order
// and tail-padding mistakes directly observable.
func taskBridgeRawProp(draws int, gate string) func(*T) {
	return func(t *T) {
		var got []uint64
		t.Cleanup(func() {
			marker := ""
			if gate == "skip" {
				marker = bridgeSkipMarker
			}
			taskWriteTrace(taskTracePath, got, marker)
		})
		for i := 0; i < draws; i++ {
			got = append(got, t.s.drawBits(64))
		}
		switch gate {
		case "skip":
			t.Log(bridgeSkipMarker)
			t.Skip("bridge property skips on purpose")
		case "crash":
			panic("bridge induced crash")
		case "hang":
			time.Sleep(15 * time.Second)
		}
		t.Log(bridgePassMarker)
	}
}

// TestTaskByteBridgeHelper is the subprocess entry point; it skips unless
// explicitly invoked by TestTaskByteBridge.
func TestTaskByteBridgeHelper(t *testing.T) {
	if os.Getenv(bridgeHelperEnv) != "1" {
		t.Skip("subprocess helper")
	}

	caseName := os.Getenv(bridgeCaseEnv)
	input, err := hex.DecodeString(os.Getenv(bridgeInputEnv))
	if err != nil {
		t.Fatalf("bad helper input: %v", err)
	}
	taskMu.Lock()
	taskTracePath = os.Getenv(bridgeTraceEnv)
	switch os.Getenv(bridgeGateEnv) {
	case "skip":
		taskGate = gateSkip
	default:
		taskGate = gateFailing
	}
	taskMu.Unlock()

	switch caseName {
	case "raw":
		draws := 0
		for _, ch := range os.Getenv(bridgeDrawsEnv) {
			draws = draws*10 + int(ch-'0')
		}
		MakeFuzz(taskBridgeRawProp(draws, os.Getenv(bridgeGateEnv)))(t, input)
	case "main":
		MakeFuzz(taskReplayProp)(t, input)
	default:
		t.Fatalf("unknown bridge case %q", caseName)
	}
}

type bridgeChildResult struct {
	exitCode int
	timedOut bool
	output   string
}

func taskRunBridgeChild(t *testing.T, caseName, gate string, draws int, input []byte, timeout time.Duration) bridgeChildResult {
	t.Helper()
	exe, err := os.Executable()
	if err != nil {
		t.Fatalf("test executable: %v", err)
	}

	trace, err := os.CreateTemp(t.TempDir(), "bridge-trace-*.txt")
	if err != nil {
		t.Fatalf("trace file: %v", err)
	}
	tracePath := trace.Name()
	_ = trace.Close()

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	args := []string{
		"-test.run=^TestTaskByteBridgeHelper$",
		"-test.v",
		fmt.Sprintf("-test.timeout=%v", timeout-timeout/4),
	}
	cmd := exec.CommandContext(ctx, exe, args...)
	cmd.Env = append(os.Environ(),
		bridgeHelperEnv+"=1",
		bridgeCaseEnv+"="+caseName,
		bridgeGateEnv+"="+gate,
		fmt.Sprintf("%s=%d", bridgeDrawsEnv, draws),
		bridgeInputEnv+"="+hex.EncodeToString(input),
		bridgeTraceEnv+"="+tracePath,
	)
	out, runErr := cmd.CombinedOutput()

	res := bridgeChildResult{output: string(out)}
	if ctx.Err() == context.DeadlineExceeded {
		res.timedOut = true
	}
	if ee, ok := runErr.(*exec.ExitError); ok {
		res.exitCode = ee.ExitCode()
	} else if runErr != nil && !res.timedOut {
		t.Fatalf("bridge child failed to start: %v\n%s", runErr, out)
	}
	res.output = res.output + "\nTRACEFILE:" + tracePath
	return res
}

func taskReadTrace(t *testing.T, res bridgeChildResult) []string {
	t.Helper()
	idx := strings.Index(res.output, "TRACEFILE:")
	path := strings.TrimPrefix(res.output[idx:], "TRACEFILE:")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read trace %q: %v", path, err)
	}
	var lines []string
	for _, l := range strings.Split(strings.TrimSpace(string(data)), "\n") {
		if l != "" {
			lines = append(lines, l)
		}
	}
	return lines
}

func TestTaskByteBridge(t *testing.T) {
	taskResetState()

	taskcheck.Run(t, func(c *taskcheck.C) {
		// Hand-computed asymmetric sample: words whose byte patterns break
		// symmetry, so a reversed byte order changes every decoded value.
		words := []uint64{0x0102030405060708, 0x00000000000000ff}
		fullInput := taskEncodeLE(words)
		c.Check(hex.EncodeToString(fullInput) == "0807060504030201ff00000000000000",
			"hand-computed little-endian encoding: %x", fullInput)

		// Case 1: normal property, two full words.
		ok := taskRunBridgeChild(t, "raw", "normal", 2, fullInput, 30*time.Second)
		c.Check(ok.exitCode == 0 && !ok.timedOut, "normal child passes (exit=%d)", ok.exitCode)
		c.Check(strings.Contains(ok.output, bridgePassMarker), "normal child reached completion")
		trace := taskReadTrace(t, ok)
		c.Check(len(trace) == 2, "exactly two words traced, got %v", trace)
		c.Check(trace[0] == "0x102030405060708" && trace[1] == "0xff",
			"trace decodes back to the original asymmetric words: %v", trace)

		// Case 2: nine-byte tail -> last word zero-padded, no extra word.
		tailInput := append(taskEncodeLE(words[:1]), 0xff)
		c.Check(len(tailInput) == 9, "tail input is one word plus a single byte")
		tail := taskRunBridgeChild(t, "raw", "normal", 2, tailInput, 30*time.Second)
		c.Check(tail.exitCode == 0 && strings.Contains(tail.output, bridgePassMarker),
			"tail-padded child passes")
		tailTrace := taskReadTrace(t, tail)
		c.Check(len(tailTrace) == 2, "tail padding does not create an extra word: %v", tailTrace)
		c.Check(tailTrace[0] == "0x102030405060708" && tailTrace[1] == "0xff",
			"partial last word is zero-padded: %v", tailTrace)

		// Case 3: active skip inside the property.
		skip := taskRunBridgeChild(t, "raw", "skip", 2, fullInput, 30*time.Second)
		c.Check(skip.exitCode == 0 && !skip.timedOut, "skip child exits cleanly (exit=%d)", skip.exitCode)
		c.Check(strings.Contains(skip.output, "SKIP") && strings.Contains(skip.output, bridgeSkipMarker),
			"skip child reports a property-level skip")
		skipTrace := taskReadTrace(t, skip)
		c.Check(len(skipTrace) == 3 && skipTrace[2] == bridgeSkipMarker,
			"skip marker recorded after the drawn words: %v", skipTrace)

		// Case 4: stream exhaustion -> rapid invalidates the case, and the
		// property itself never gets to skip.
		exhaust := taskRunBridgeChild(t, "raw", "normal", 3, taskEncodeLE(words[:1]), 30*time.Second)
		c.Check(exhaust.exitCode == 0, "exhausted child is skipped, not failed (exit=%d)", exhaust.exitCode)
		c.Check(strings.Contains(exhaust.output, "SKIP"), "exhaustion surfaces as a skip")
		c.Check(!strings.Contains(exhaust.output, bridgeSkipMarker),
			"exhaustion happens in the engine, not in the property skip branch")
		exTrace := taskReadTrace(t, exhaust)
		c.Check(len(exTrace) == 1 && exTrace[0] == "0x102030405060708",
			"only the available word was drawn: %v", exTrace)

		// Case 5: same-origin failure through the public MakeFuzz entry,
		// using the shrunk word stream from the shrink-identity evidence.
		buf, _, _, shrinkErr := taskRunShrink(t)
		c.Check(taskOriginOf(shrinkErr) == 0, "shrink evidence is site A")
		fail := taskRunBridgeChild(t, "main", "normal", 0, taskEncodeLE(buf), 30*time.Second)
		c.Check(fail.exitCode == 1, "failing child exits like a failed Go test, got %d", fail.exitCode)
		c.Check(strings.Contains(fail.output, taskFailMsg), "same failure message in the child")
		c.Check(strings.Contains(fail.output, "[rapid] failed"), "failure is a stop-test, not a panic")
		failTrace := taskReadTrace(t, fail)
		c.Check(len(failTrace) >= 1, "child recorded a trace")
		c.Check(strings.Contains(strings.Join(failTrace, "\n"), "origin=0"),
			"child failure originates at site A: %v", failTrace)

		// Negative controls: an abnormal child outcome must never look like
		// a successful detection.
		crash := taskRunBridgeChild(t, "raw", "crash", 1, taskEncodeLE(words[:1]), 30*time.Second)
		c.Check(crash.exitCode != 0, "crashing child fails")
		c.Check(!strings.Contains(crash.output, bridgePassMarker) &&
			strings.Contains(crash.output, "panic"), "crash is distinguishable from a detected failure")

		hang := taskRunBridgeChild(t, "raw", "hang", 1, taskEncodeLE(words[:1]), 6*time.Second)
		c.Check(hang.timedOut || hang.exitCode != 0, "hanging child is killed, not accepted")
		c.Check(!strings.Contains(hang.output, bridgePassMarker), "timeout carries no success marker")

		buildOK, buildOut := taskBrokenModuleBuild(t)
		c.Check(!buildOK, "broken fixture module does not compile")
		c.Check(strings.Contains(buildOut, "fail") || strings.Contains(buildOut, "error") ||
			strings.Contains(buildOut, "syntax"), "compile failure is visible in the output: %q", buildOut)
	})
}

// taskBrokenModuleBuild creates a tiny benign module with a syntax error in
// a temporary directory and tries to build it, proving the harness treats
// compile failures as failures rather than detections.
func taskBrokenModuleBuild(t *testing.T) (bool, string) {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "go.mod"), []byte("module bridgebroken\n\ngo 1.23\n"), 0600); err != nil {
		t.Fatalf("go.mod: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, "broken_test.go"),
		[]byte("package bridgebroken\n\nfunc TestBroken(t *testing.T) {\n"), 0600); err != nil {
		t.Fatalf("broken source: %v", err)
	}
	goBin, err := exec.LookPath("go")
	if err != nil {
		t.Fatalf("go toolchain unavailable: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	cmd := exec.CommandContext(ctx, goBin, "test", "./...")
	cmd.Dir = dir
	out, runErr := cmd.CombinedOutput()
	return runErr == nil, string(out)
}
