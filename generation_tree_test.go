/*
Copyright 2026 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

package fuzz

import (
	"bytes"
	"errors"
	"fmt"
	"math/rand"
	"reflect"
	"regexp"
	"strings"
	"testing"
	"time"
	"unsafe"

	"github.com/google/gofuzz/task_support/taskcheck"
)

type gtInner struct {
	Name  string
	Flag  bool
	Score int16
	Data  []uint8
}

type gtOuter struct {
	ID      uint64
	Inner   *gtInner
	Tags    []string
	Matrix  [2]int32
	Skipped string
	hidden  int
}

type gtReduceItem struct {
	A uint64
	B string
}

type gtReduce struct {
	Items  []gtReduceItem
	Label  string
	UCount uint32
	Flag   bool
	Ptr    *gtReduceItem
}

type gtList struct {
	V    int
	Next *gtList
}

type gtSelfFuzz struct{ N int }

func (*gtSelfFuzz) Fuzz(c Continue) {}

type gtCustom struct{ X int }

type gtNamedInt int32

type gtNamedString string

type gtHand struct{ A int8 }

type gtChain struct{ Next *gtChain }

func gtOuterFuzzer(seed int64) *Fuzzer {
	return NewWithSeed(seed).NilChance(0).NumElements(1, 4).
		SkipFieldsWithPattern(regexp.MustCompile("Skipped"))
}

func TestTreeGeneration(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := gtOuterFuzzer(42)
		var out gtOuter
		tree, err := f.RecordTree(&out)
		c.Check(err == nil, "RecordTree failed: %v", err)
		if err != nil {
			return
		}

		// Skipped and unexported fields stay zero.
		c.Check(out.Skipped == "", "skipped field not zero: %q", out.Skipped)
		c.Check(out.hidden == 0, "unexported field not zero: %d", out.hidden)
		c.Check(out.Inner != nil, "Inner should be non-nil with NilChance(0)")
		c.Check(len(out.Tags) >= 1 && len(out.Tags) <= 4, "Tags length %d out of [1,4]", len(out.Tags))

		// Read-only tree shape: root is a struct over the 4 generated fields.
		root := tree.Root()
		c.Check(root.Kind() == NodeStruct, "root kind %v, want struct", root.Kind())
		c.Check(root.NumChildren() == 4, "root children %d, want 4", root.NumChildren())
		if root.NumChildren() == 4 {
			c.Check(root.Child(0).Kind() == NodeUint, "child 0 kind %v, want uint", root.Child(0).Kind())
			c.Check(root.Child(1).Kind() == NodePtr, "child 1 kind %v, want ptr", root.Child(1).Kind())
			c.Check(root.Child(2).Kind() == NodeSlice, "child 2 kind %v, want slice", root.Child(2).Kind())
			c.Check(root.Child(3).Kind() == NodeArray, "child 3 kind %v, want array", root.Child(3).Kind())
			idVal, ok := root.Child(0).LeafValue()
			c.Check(ok, "child 0 should be a leaf")
			c.Check(idVal == out.ID, "leaf value %v != generated ID %v", idVal, out.ID)
			c.Check(root.Child(3).NumChildren() == 2, "array node children %d, want 2", root.Child(3).NumChildren())
		}

		// Replay reproduces the recorded object exactly.
		var back gtOuter
		err = tree.Replay(&back)
		c.Check(err == nil, "Replay failed: %v", err)
		c.Check(reflect.DeepEqual(out, back), "replay mismatch:\n%+v\n%+v", out, back)

		// Same seed, config and type produce byte-identical records.
		f2 := gtOuterFuzzer(42)
		var out2 gtOuter
		tree2, err2 := f2.RecordTree(&out2)
		c.Check(err2 == nil, "second RecordTree failed: %v", err2)
		b1, mErr1 := tree.MarshalBinary()
		b2, mErr2 := tree2.MarshalBinary()
		c.Check(mErr1 == nil && mErr2 == nil, "MarshalBinary failed: %v %v", mErr1, mErr2)
		c.Check(bytes.Equal(b1, b2), "same seed/config/type must give identical records")
		c.Check(reflect.DeepEqual(out, out2), "same seed/config/type must give identical objects")

		// RecordTree draws exactly one Int63 per call from the Fuzzer's source.
		r := rand.New(rand.NewSource(42))
		s1 := r.Int63()
		s2 := r.Int63()
		c.Check(root.Seed() == s1, "root seed %d, want first Int63 %d", root.Seed(), s1)
		var out3 gtOuter
		tree3, err3 := f.RecordTree(&out3)
		c.Check(err3 == nil, "third RecordTree failed: %v", err3)
		if err3 == nil {
			c.Check(tree3.Root().Seed() == s2, "second root seed %d, want second Int63 %d", tree3.Root().Seed(), s2)
			b3, _ := tree3.MarshalBinary()
			c.Check(!bytes.Equal(b1, b3), "different draws must give different records")
		}

		// A failed precheck consumes no randomness.
		fb := gtOuterFuzzer(7)
		var badMap struct{ M map[string]int }
		_, errB := fb.RecordTree(&badMap)
		c.Check(errB != nil, "map field should be rejected")
		c.Check(strings.Contains(fmt.Sprint(errB), ".M"), "error should carry the field path: %v", errB)
		var goodB gtOuter
		treeB, errB2 := fb.RecordTree(&goodB)
		c.Check(errB2 == nil, "RecordTree after failed precheck failed: %v", errB2)
		fc := gtOuterFuzzer(7)
		var goodC gtOuter
		treeC, errC := fc.RecordTree(&goodC)
		c.Check(errC == nil, "RecordTree on fresh fuzzer failed: %v", errC)
		if errB2 == nil && errC == nil {
			bB, _ := treeB.MarshalBinary()
			bC, _ := treeC.MarshalBinary()
			c.Check(bytes.Equal(bB, bC), "failed precheck must consume zero randomness")
		}

		// Unsupported kinds are rejected before generation.
		var uMap struct{ M map[string]int }
		var uIface struct{ I interface{} }
		var uFloat struct{ F float64 }
		var uCplx struct{ C complex128 }
		var uChan struct{ Ch chan int }
		var uFunc struct{ Fn func() }
		var uUnsafe struct{ P unsafe.Pointer }
		c.Check(func() bool { _, e := gtOuterFuzzer(1).RecordTree(&uMap); return e != nil }(), "map should be rejected")
		c.Check(func() bool { _, e := gtOuterFuzzer(1).RecordTree(&uIface); return e != nil }(), "interface should be rejected")
		c.Check(func() bool { _, e := gtOuterFuzzer(1).RecordTree(&uFloat); return e != nil }(), "float should be rejected")
		c.Check(func() bool { _, e := gtOuterFuzzer(1).RecordTree(&uCplx); return e != nil }(), "complex should be rejected")
		c.Check(func() bool { _, e := gtOuterFuzzer(1).RecordTree(&uChan); return e != nil }(), "chan should be rejected")
		c.Check(func() bool { _, e := gtOuterFuzzer(1).RecordTree(&uFunc); return e != nil }(), "func should be rejected")
		c.Check(func() bool { _, e := gtOuterFuzzer(1).RecordTree(&uUnsafe); return e != nil }(), "unsafe.Pointer should be rejected")

		// AllowUnexportedFields is rejected.
		_, eUnexp := NewWithSeed(1).AllowUnexportedFields(true).RecordTree(&gtOuter{})
		c.Check(eUnexp != nil, "AllowUnexportedFields should be rejected")

		// Custom Funcs, fuzz.Interface and default handlers are rejected.
		fCust := NewWithSeed(1).Funcs(func(x *gtCustom, cf Continue) { x.X = 1 })
		var hCust struct{ C gtCustom }
		_, eCust := fCust.RecordTree(&hCust)
		c.Check(eCust != nil && strings.Contains(fmt.Sprint(eCust), ".C"),
			"custom func should be rejected with field path: %v", eCust)
		var hSelf struct{ S gtSelfFuzz }
		_, eSelf := gtOuterFuzzer(1).RecordTree(&hSelf)
		c.Check(eSelf != nil, "fuzz.Interface implementation should be rejected")
		var hTime struct{ T time.Time }
		_, eTime := gtOuterFuzzer(1).RecordTree(&hTime)
		c.Check(eTime != nil, "default handler (time.Time) should be rejected")

		// Skipped fields are exempt from the precheck and stay zero.
		fSkip := NewWithSeed(1).SkipFieldsWithPattern(regexp.MustCompile("^Bad$"))
		var hSkip struct {
			Bad  map[string]int
			Keep int
		}
		treeSkip, eSkip := fSkip.RecordTree(&hSkip)
		c.Check(eSkip == nil, "skipped unsupported field should be allowed: %v", eSkip)
		c.Check(hSkip.Bad == nil, "skipped field must stay zero")
		if eSkip == nil {
			c.Check(treeSkip.Root().NumChildren() == 1, "only the non-skipped field gets a node")
		}

		// Recursive types terminate via MaxDepth; truncation is a zero node.
		fRec := NewWithSeed(3).NilChance(0).MaxDepth(5)
		var l gtList
		treeL, eL := fRec.RecordTree(&l)
		c.Check(eL == nil, "recursive record failed: %v", eL)
		if eL == nil {
			var l2 gtList
			c.Check(treeL.Replay(&l2) == nil, "recursive replay failed")
			c.Check(reflect.DeepEqual(l, l2), "recursive replay mismatch")
			c.Check(l2.Next != nil && l2.Next.Next != nil, "expected a chain of 3 structs")
			c.Check(l2.Next.Next.Next == nil, "recursion should be truncated by MaxDepth")
			c.Check(l2.Next.Next.V == 0, "depth-truncated leaf must be zero")
			foundZero := false
			var walk func(n *TreeNode)
			walk = func(n *TreeNode) {
				if n.Kind() == NodeZero {
					foundZero = true
				}
				for i := 0; i < n.NumChildren(); i++ {
					walk(n.Child(i))
				}
			}
			walk(treeL.Root())
			c.Check(foundZero, "expected a zero node from depth truncation")
		}

		// Named scalar types work.
		var hn struct {
			A gtNamedInt
			B gtNamedString
		}
		treeN, eN := gtOuterFuzzer(11).RecordTree(&hn)
		c.Check(eN == nil, "named scalar record failed: %v", eN)
		if eN == nil {
			var hn2 struct {
				A gtNamedInt
				B gtNamedString
			}
			c.Check(treeN.Replay(&hn2) == nil, "named scalar replay failed")
			c.Check(hn == hn2, "named scalar replay mismatch: %v vs %v", hn, hn2)
		}

		// Configuration prechecks.
		_, eD0 := NewWithSeed(1).MaxDepth(0).RecordTree(&gtOuter{})
		c.Check(eD0 != nil, "MaxDepth 0 should be rejected")
		_, eD1 := NewWithSeed(1).MaxDepth(maxRecordDepth + 1).RecordTree(&gtOuter{})
		c.Check(eD1 != nil, "excessive MaxDepth should be rejected")

		// Bad destinations.
		_, eNil := gtOuterFuzzer(1).RecordTree(nil)
		c.Check(eNil != nil, "nil destination should be rejected")
		_, eVal := gtOuterFuzzer(1).RecordTree(gtOuter{})
		c.Check(eVal != nil, "non-pointer destination should be rejected")
		var nilPtr *gtOuter
		_, eNilPtr := gtOuterFuzzer(1).RecordTree(nilPtr)
		c.Check(eNilPtr != nil, "nil pointer destination should be rejected")

		// Replay errors leave the destination untouched.
		c.Check(tree.Replay(nil) != nil, "nil replay target should be rejected")
		c.Check(tree.Replay(out) != nil, "non-pointer replay target should be rejected")
		var wrongType struct{ X int }
		wrongType.X = 777
		c.Check(tree.Replay(&wrongType) != nil, "type mismatch should be rejected")
		c.Check(wrongType.X == 777, "sentinel changed after failed replay: %d", wrongType.X)
		var nilTarget *gtOuter
		c.Check(tree.Replay(nilTarget) != nil, "nil pointer replay target should be rejected")
	})
}

func TestTreeReduction(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := NewWithSeed(2024).NilChance(0).NumElements(3, 5)
		var orig gtReduce
		tree, err := f.RecordTree(&orig)
		c.Check(err == nil, "RecordTree failed: %v", err)
		if err != nil {
			return
		}
		n := len(orig.Items)
		c.Check(n >= 3, "want at least 3 items, got %d", n)
		c.Check(orig.Label != "", "want a non-empty label for string shrinking")
		c.Check(orig.UCount != 0, "want a non-zero UCount for uint shrinking")
		c.Check(orig.Ptr != nil, "want a non-nil Ptr with NilChance(0)")
		if n == 0 || orig.Label == "" || orig.UCount == 0 || orig.Ptr == nil {
			return
		}

		// Run A: deleting slice elements preserves the surviving subtrees and
		// all sibling fields.
		idx := n / 2
		target := orig.Items[idx]
		sliceNode := tree.Root().Child(0)
		targetNode := sliceNode.Child(idx)
		siblingIDs := make([]uint64, tree.Root().NumChildren())
		for i := range siblingIDs {
			siblingIDs[i] = tree.Root().Child(i).ID()
		}
		keepA := func(obj interface{}) (bool, error) {
			rs := obj.(*gtReduce)
			if rs.Label != orig.Label || rs.UCount != orig.UCount || rs.Flag != orig.Flag {
				return false, nil
			}
			if rs.Ptr == nil || rs.Ptr.A != orig.Ptr.A || rs.Ptr.B != orig.Ptr.B {
				return false, nil
			}
			for _, it := range rs.Items {
				if it.A == target.A && it.B == target.B {
					return true, nil
				}
			}
			return false, nil
		}
		finalA, callsA, errA := tree.Minimize(keepA, 100000)
		c.Check(errA == nil, "Minimize A failed: %v", errA)
		c.Check(callsA > 1, "Minimize A should have made several calls, got %d", callsA)
		if errA == nil {
			var minA gtReduce
			c.Check(finalA.Replay(&minA) == nil, "replay of minimized tree A failed")
			c.Check(len(minA.Items) == 1, "want exactly 1 surviving item, got %d", len(minA.Items))
			if len(minA.Items) == 1 {
				c.Check(minA.Items[0] == target, "surviving item changed: %+v vs %+v", minA.Items[0], target)
			}
			c.Check(minA.Label == orig.Label, "sibling Label changed: %q", minA.Label)
			c.Check(minA.UCount == orig.UCount, "sibling UCount changed: %d", minA.UCount)
			c.Check(minA.Flag == orig.Flag, "sibling Flag changed: %v", minA.Flag)
			c.Check(minA.Ptr != nil && *minA.Ptr == *orig.Ptr, "sibling Ptr changed")
			// Node identity: the slice node and the survivor keep their IDs
			// and seeds; sibling subtrees are untouched.
			newSlice := finalA.Root().Child(0)
			c.Check(newSlice.ID() == sliceNode.ID(), "slice node ID changed")
			c.Check(newSlice.Seed() == sliceNode.Seed(), "slice node seed changed")
			if newSlice.NumChildren() == 1 {
				surv := newSlice.Child(0)
				c.Check(surv.ID() == targetNode.ID(), "survivor ID %d, want %d", surv.ID(), targetNode.ID())
				c.Check(surv.Seed() == targetNode.Seed(), "survivor seed changed")
				c.Check(surv.NumChildren() == targetNode.NumChildren(), "survivor children changed")
			} else {
				c.Check(false, "minimized slice should have 1 child, got %d", newSlice.NumChildren())
			}
			for i := 1; i < len(siblingIDs); i++ {
				c.Check(finalA.Root().Child(i).ID() == siblingIDs[i],
					"sibling node %d ID changed", i)
			}
		}

		// Run B: keep always true shrinks everything; callback mutations do
		// not leak into the tree, other candidates, or the result.
		before, _ := tree.MarshalBinary()
		poisoned := false
		keepB := func(obj interface{}) (bool, error) {
			rs := obj.(*gtReduce)
			if rs.Label == "POISON" || (len(rs.Items) > 0 && rs.Items[0].A == ^uint64(0)) {
				poisoned = true
			}
			rs.Label = "POISON"
			rs.Items = []gtReduceItem{{A: ^uint64(0)}}
			rs.Ptr = nil
			return true, nil
		}
		finalB, callsB, errB := tree.Minimize(keepB, 1000)
		c.Check(errB == nil, "Minimize B failed: %v", errB)
		c.Check(!poisoned, "callback mutations leaked into a later replay")
		if errB == nil {
			var minB gtReduce
			c.Check(finalB.Replay(&minB) == nil, "replay of minimized tree B failed")
			c.Check(minB.Items == nil, "Items should shrink to nil, got %v", minB.Items)
			c.Check(minB.Label == "", "Label should shrink to empty, got %q", minB.Label)
			c.Check(minB.UCount == 0, "UCount should shrink to 0, got %d", minB.UCount)
			c.Check(!minB.Flag, "Flag should shrink to false")
			c.Check(minB.Ptr == nil, "Ptr should shrink to nil")
		}
		after, _ := tree.MarshalBinary()
		c.Check(bytes.Equal(before, after), "Minimize must not mutate its receiver")
		_ = callsB

		// Run C: strings shrink by rune, longest interval first, so the last
		// rune survives.
		keepC := func(obj interface{}) (bool, error) {
			return len(obj.(*gtReduce).Label) >= 1, nil
		}
		finalC, _, errC := tree.Minimize(keepC, 100000)
		c.Check(errC == nil, "Minimize C failed: %v", errC)
		if errC == nil {
			var minC gtReduce
			c.Check(finalC.Replay(&minC) == nil, "replay of minimized tree C failed")
			runes := []rune(orig.Label)
			want := string(runes[len(runes)-1:])
			c.Check(minC.Label == want, "Label %q, want single last rune %q", minC.Label, want)
		}

		// Run D: unsigned integers shrink toward zero by halving.
		keepD := func(obj interface{}) (bool, error) {
			return obj.(*gtReduce).UCount > 0, nil
		}
		finalD, _, errD := tree.Minimize(keepD, 100000)
		c.Check(errD == nil, "Minimize D failed: %v", errD)
		if errD == nil {
			var minD gtReduce
			c.Check(finalD.Replay(&minD) == nil, "replay of minimized tree D failed")
			c.Check(minD.UCount == 1, "UCount %d, want 1", minD.UCount)
		}

		// Run E: signed integers shrink toward zero, keeping the sign.
		type signed struct {
			N int32
			M int64
		}
		var sOrig signed
		sTree, sErr := NewWithSeed(77).NilChance(0).RecordTree(&sOrig)
		c.Check(sErr == nil, "signed record failed: %v", sErr)
		if sErr == nil {
			c.Check(sOrig.N != 0, "want non-zero N for signed shrinking")
			keepE := func(obj interface{}) (bool, error) {
				return obj.(*signed).N != 0, nil
			}
			finalE, _, errE := sTree.Minimize(keepE, 100000)
			c.Check(errE == nil, "Minimize E failed: %v", errE)
			if errE == nil {
				var minE signed
				c.Check(finalE.Replay(&minE) == nil, "replay of minimized tree E failed")
				wantN := int32(1)
				if sOrig.N < 0 {
					wantN = -1
				}
				c.Check(minE.N == wantN, "N %d, want %d", minE.N, wantN)
				c.Check(minE.M == 0, "unpinned M should shrink to 0, got %d", minE.M)
			}
		}

		// Run F: slice deletion candidates come in order: longest interval
		// first, left to right among equal lengths.
		var lengths []int
		keepF := func(obj interface{}) (bool, error) {
			l := len(obj.(*gtReduce).Items)
			lengths = append(lengths, l)
			return l == n, nil // only the original passes
		}
		budget := 1 + n*(n+1)/2
		finalF, callsF, errF := tree.Minimize(keepF, budget)
		c.Check(errF == nil, "Minimize F failed: %v", errF)
		c.Check(callsF == budget, "calls %d, want budget %d", callsF, budget)
		c.Check(finalF == tree, "no candidate should have been accepted")
		wantLengths := []int{n}
		for l := n; l >= 1; l-- {
			for start := 0; start+l <= n; start++ {
				wantLengths = append(wantLengths, n-l)
			}
		}
		c.Check(reflect.DeepEqual(lengths, wantLengths),
			"deletion order %v, want %v", lengths, wantLengths)

		// Run G: error and budget semantics.
		_, calls0, err0 := tree.Minimize(func(interface{}) (bool, error) { return true, nil }, 0)
		c.Check(err0 != nil && calls0 == 0, "maxCalls 0 should fail with 0 calls, got %v / %d", err0, calls0)
		noTree, calls1, err1 := tree.Minimize(func(interface{}) (bool, error) { return false, nil }, 10)
		c.Check(err1 != nil, "unsatisfiable keep should fail")
		c.Check(calls1 == 1, "unsatisfiable keep should stop after the initial check, got %d", calls1)
		c.Check(noTree == nil, "unsatisfiable keep should return no tree")

		boom := errors.New("boom")
		count := 0
		keepErr := func(interface{}) (bool, error) {
			count++
			if count == 3 {
				return false, boom
			}
			return true, nil
		}
		treeG, callsG, errG := tree.Minimize(keepErr, 100)
		c.Check(errG == boom, "callback error should propagate, got %v", errG)
		c.Check(callsG == 3, "calls %d, want 3", callsG)
		c.Check(treeG != nil, "last accepted tree should be returned on callback error")
		if treeG != nil {
			var minG gtReduce
			c.Check(treeG.Replay(&minG) == nil, "replay of tree G failed")
			c.Check(minG.Items == nil, "the single accepted candidate should have emptied Items")
			c.Check(minG.Label == orig.Label, "Label should be untouched in tree G")
		}

		treeH, callsH, errH := tree.Minimize(func(interface{}) (bool, error) { return true, nil }, 3)
		c.Check(errH == nil, "budget exhaustion is not an error: %v", errH)
		c.Check(callsH == 3, "calls %d, want 3", callsH)
		if treeH != nil {
			var minH gtReduce
			c.Check(treeH.Replay(&minH) == nil, "replay of tree H failed")
			c.Check(minH.Items == nil && minH.Label == "",
				"budget of 3 should accept exactly 2 candidates: %+v", minH)
		}
	})
}

func TestTreePersistence(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := NewWithSeed(556).NilChance(0.3).NumElements(1, 3).
			SkipFieldsWithPattern(regexp.MustCompile("Skipped"))
		var orig gtOuter
		tree, err := f.RecordTree(&orig)
		c.Check(err == nil, "RecordTree failed: %v", err)
		if err != nil {
			return
		}
		data, err := tree.MarshalBinary()
		c.Check(err == nil, "MarshalBinary failed: %v", err)
		if err != nil {
			return
		}

		// Round trip: decode, replay, re-encode.
		dec, err := DecodeGenerationTree(data, &gtOuter{})
		c.Check(err == nil, "DecodeGenerationTree failed: %v", err)
		if err != nil {
			return
		}
		var back gtOuter
		c.Check(dec.Replay(&back) == nil, "replay of decoded tree failed")
		c.Check(reflect.DeepEqual(orig, back), "decoded replay mismatch:\n%+v\n%+v", orig, back)
		data2, err := dec.MarshalBinary()
		c.Check(err == nil, "re-encode failed: %v", err)
		c.Check(bytes.Equal(data, data2), "decode-then-encode must reproduce the record")

		// Decoding does not touch the prototype.
		proto := &gtOuter{ID: 12345}
		_, err = DecodeGenerationTree(data, proto)
		c.Check(err == nil, "decode with valued prototype failed: %v", err)
		c.Check(proto.ID == 12345, "prototype was modified by decoding")

		// Wrong prototypes are rejected.
		_, err = DecodeGenerationTree(data, &gtReduce{})
		c.Check(err != nil, "wrong prototype type should be rejected")
		_, err = DecodeGenerationTree(data, gtOuter{})
		c.Check(err != nil, "non-pointer prototype should be rejected")
		_, err = DecodeGenerationTree(data, nil)
		c.Check(err != nil, "nil prototype should be rejected")

		// Truncation, trailing data and version skew are rejected.
		for _, cut := range []int{0, 5, len(data) / 2, len(data) - 1} {
			_, err := DecodeGenerationTree(data[:cut], &gtOuter{})
			c.Check(err != nil, "record truncated to %d bytes should be rejected", cut)
		}
		trailing := append(append([]byte{}, data...), 0)
		_, err = DecodeGenerationTree(trailing, &gtOuter{})
		c.Check(err != nil, "trailing data should be rejected")
		badVersion := append([]byte{}, data...)
		badVersion[len(treeMagic)-1]++
		_, err = DecodeGenerationTree(badVersion, &gtOuter{})
		c.Check(err != nil, "unsupported version should be rejected")

		// A failed replay keeps the destination untouched.
		sentinel := gtReduce{Label: "sentinel", UCount: 999}
		err = dec.Replay(&sentinel)
		c.Check(err != nil, "replaying into the wrong type should fail")
		c.Check(sentinel.Label == "sentinel" && sentinel.UCount == 999,
			"sentinel changed after failed replay: %+v", sentinel)

		// Hand-built invalid records are rejected at decode time.
		handType := reflect.TypeOf(gtHand{})
		handBind := treeTypeFingerprint(handType)
		mkHand := func(root *TreeNode) *GenerationTree {
			return &GenerationTree{
				root: root, rootType: handType, typeBind: handBind,
				nilChance: 0.2, minElements: 1, maxElements: 1, maxDepth: 5,
			}
		}
		// Duplicate node IDs.
		dup := mkHand(&TreeNode{id: 7, seed: 1, kind: NodeStruct, children: []*TreeNode{
			{id: 7, seed: 2, kind: NodeInt, bits: 8},
		}})
		dupData, err := dup.MarshalBinary()
		c.Check(err == nil, "marshal of duplicate-ID record failed: %v", err)
		_, err = DecodeGenerationTree(dupData, &gtHand{})
		c.Check(err != nil && strings.Contains(fmt.Sprint(err), "duplicate"),
			"duplicate IDs should be rejected: %v", err)
		// Node kind not matching the type.
		badKind := mkHand(&TreeNode{id: 1, seed: 1, kind: NodeStruct, children: []*TreeNode{
			{id: 2, seed: 2, kind: NodeString},
		}})
		badKindData, err := badKind.MarshalBinary()
		c.Check(err == nil, "marshal of bad-kind record failed: %v", err)
		_, err = DecodeGenerationTree(badKindData, &gtHand{})
		c.Check(err != nil, "kind mismatch should be rejected")
		// Leaf override out of range for the field's bit width.
		outOfRange := mkHand(&TreeNode{id: 1, seed: 1, kind: NodeStruct, children: []*TreeNode{
			{id: 2, seed: 2, kind: NodeInt, bits: 8, hasOverride: true, intVal: 200},
		}})
		outOfRangeData, err := outOfRange.MarshalBinary()
		c.Check(err == nil, "marshal of out-of-range record failed: %v", err)
		_, err = DecodeGenerationTree(outOfRangeData, &gtHand{})
		c.Check(err != nil && strings.Contains(fmt.Sprint(err), "range"),
			"out-of-range leaf override should be rejected: %v", err)
		// Invalid frozen policy.
		badPolicy := mkHand(&TreeNode{id: 1, seed: 1, kind: NodeStruct, children: []*TreeNode{
			{id: 2, seed: 2, kind: NodeInt, bits: 8},
		}})
		badPolicy.nilChance = 1.5
		badPolicyData, err := badPolicy.MarshalBinary()
		c.Check(err == nil, "marshal of bad-policy record failed: %v", err)
		_, err = DecodeGenerationTree(badPolicyData, &gtHand{})
		c.Check(err != nil, "invalid NilChance should be rejected")
		// Excessive depth.
		chainType := reflect.TypeOf(gtChain{})
		var idCounter uint64
		node := &TreeNode{id: idCounter, seed: 1, kind: NodePtr, nilFlag: true}
		idCounter++
		for i := 0; i < maxRecordDepth+2; i++ {
			strct := &TreeNode{id: idCounter, seed: 1, kind: NodeStruct}
			idCounter++
			ptr := &TreeNode{id: idCounter, seed: 1, kind: NodePtr}
			idCounter++
			strct.children = []*TreeNode{ptr}
			ptr.children = []*TreeNode{node}
			node = strct
		}
		deep := &GenerationTree{
			root: node, rootType: chainType, typeBind: treeTypeFingerprint(chainType),
			nilChance: 0.2, minElements: 1, maxElements: 1, maxDepth: 5,
		}
		deepData, err := deep.MarshalBinary()
		c.Check(err == nil, "marshal of deep record failed: %v", err)
		_, err = DecodeGenerationTree(deepData, &gtChain{})
		c.Check(err != nil && strings.Contains(fmt.Sprint(err), "depth"),
			"excessive depth should be rejected: %v", err)

		// A minimized tree can be saved, decoded and replayed on its own.
		small, _, err := tree.Minimize(func(obj interface{}) (bool, error) {
			return len(obj.(*gtOuter).Tags) >= 1, nil
		}, 100000)
		c.Check(err == nil, "Minimize failed: %v", err)
		if err == nil {
			smallData, err := small.MarshalBinary()
			c.Check(err == nil, "marshal of minimized tree failed: %v", err)
			smallDec, err := DecodeGenerationTree(smallData, &gtOuter{})
			c.Check(err == nil, "decode of minimized tree failed: %v", err)
			if err == nil {
				var s1, s2 gtOuter
				c.Check(small.Replay(&s1) == nil, "replay of minimized tree failed")
				c.Check(smallDec.Replay(&s2) == nil, "replay of decoded minimized tree failed")
				c.Check(reflect.DeepEqual(s1, s2), "minimized round trip mismatch")
				c.Check(len(s1.Tags) == 1, "minimized Tags length %d, want 1", len(s1.Tags))
				smallData2, _ := smallDec.MarshalBinary()
				c.Check(bytes.Equal(smallData, smallData2),
					"minimized record must re-encode identically")
			}
		}
	})
}

func Example_generationTree() {
	type Packet struct {
		Header string
		Sizes  []uint16
		Retry  int
	}

	f := NewWithSeed(100).NilChance(0).NumElements(2, 6)
	var p Packet
	tree, err := f.RecordTree(&p)
	if err != nil {
		panic(err)
	}

	// Shrink the input while the interesting condition (at least two sizes)
	// still holds.
	small, calls, err := tree.Minimize(func(obj interface{}) (bool, error) {
		return len(obj.(*Packet).Sizes) >= 2, nil
	}, 1000)
	if err != nil {
		panic(err)
	}

	var out Packet
	if err := small.Replay(&out); err != nil {
		panic(err)
	}

	// The minimized record can be persisted and replayed later.
	data, err := small.MarshalBinary()
	if err != nil {
		panic(err)
	}
	decoded, err := DecodeGenerationTree(data, &Packet{})
	if err != nil {
		panic(err)
	}
	var roundTrip Packet
	if err := decoded.Replay(&roundTrip); err != nil {
		panic(err)
	}

	fmt.Printf("recorded: header %q, %d sizes, retry %d\n", p.Header, len(p.Sizes), p.Retry)
	fmt.Printf("minimized after %d checks: header %q, %d sizes, retry %d\n", calls, out.Header, len(out.Sizes), out.Retry)
	fmt.Printf("round trip equal: %v\n", reflect.DeepEqual(out, roundTrip))

	// Output:
	// recorded: header "査¿Ï", 4 sizes, retry 5866900430724761491
	// minimized after 21 checks: header "", 2 sizes, retry 0
	// round trip equal: true
}
