/*
Copyright 2024 Google Inc. All rights reserved.

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

package fuzz_test

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"reflect"
	"regexp"
	"testing"

	fuzz "github.com/google/gofuzz"
	"github.com/google/gofuzz/task_support/taskcheck"
)

// Fixed-seed fixture types. They only use the tree-mode-supported subset:
// bool, integers (including named types), string, struct, array, slice,
// pointer and recursion.

type treeMyInt int32
type treeMyString string

type treeInner struct {
	Label string
	N     int32
}

type treeScalars struct {
	B   bool
	I8  int8
	I16 int16
	I32 int32
	I64 int64
	U8  uint8
	U16 uint16
	U32 uint32
	U64 uint64
	Up  uintptr
	NI  treeMyInt
	S   treeMyString
	A   [3]int16
}

type treeNode struct {
	Value int64
	Flag  bool
	Name  string
	Inner *treeInner
	Items []int32
	Child *treeNode
}

type treeRecChain struct {
	Next *treeRecChain
	V    int
}

type treeBasket struct {
	Items []int32
	Name  string
	Tail  int64
	Mark  bool
}

// treeSelfFuzzer implements fuzz.Interface and must therefore be rejected by
// the tree pre-flight rather than handled.
type treeSelfFuzzer int32

func (s *treeSelfFuzzer) Fuzz(cc fuzz.Continue) { *s = 1 }

// treeMixed mixes exported and unexported fields. The unexported field stays
// zero in tree mode.
type treeMixed struct {
	Public int64
	secret int64
}

// Secret exposes the unexported field for assertions from the external test
// package.
func (m treeMixed) Secret() int64 { return m.secret }

// flatten returns the nodes in preorder and asserts every ID is unique.
func flatten(c *taskcheck.C, t *fuzz.GenerationTree) []*fuzz.GenerationNode {
	seen := map[uint64]bool{}
	var out []*fuzz.GenerationNode
	var walk func(n *fuzz.GenerationNode)
	walk = func(n *fuzz.GenerationNode) {
		c.Check(n != nil, "preorder walk hit a nil node")
		c.Check(!seen[n.ID()], "duplicate node id %d", n.ID())
		seen[n.ID()] = true
		out = append(out, n)
		for _, ch := range n.Children() {
			walk(ch)
		}
	}
	walk(t.Root())
	return out
}

func findByID(n *fuzz.GenerationNode, id uint64) *fuzz.GenerationNode {
	if n.ID() == id {
		return n
	}
	for _, ch := range n.Children() {
		if got := findByID(ch, id); got != nil {
			return got
		}
	}
	return nil
}

func firstKind(n *fuzz.GenerationNode, kind fuzz.NodeKind) *fuzz.GenerationNode {
	if n.Kind() == kind {
		return n
	}
	for _, ch := range n.Children() {
		if got := firstKind(ch, kind); got != nil {
			return got
		}
	}
	return nil
}

func TestTreeGeneration(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// RecordTree must not touch dst.
		original := treeNode{Value: 1, Name: "sentinel"}
		dst := original
		f := fuzz.NewWithSeed(42).NilChance(0).NumElements(2, 2).MaxDepth(6)
		tree, err := f.RecordTree(&dst)
		c.Check(err == nil, "RecordTree returned error: %v", err)
		c.Check(reflect.DeepEqual(dst, original), "RecordTree modified dst: %#v", dst)
		c.Check(tree.Version() == 1, "unexpected format version %d", tree.Version())

		// Root identity and a fully populated ID space.
		nodes := flatten(c, tree)
		c.Check(tree.Root().ID() == 1, "root id is %d, want 1", tree.Root().ID())
		c.Check(len(nodes) > 5, "expected a rich tree, got %d nodes", len(nodes))
		c.Check(tree.Root().Seed() >= 0, "root seed must be a 63-bit value")

		// Replay builds a complete value on a fresh object, twice identically.
		var a, b treeNode
		c.Check(tree.Replay(&a) == nil, "first Replay error: %v", err)
		c.Check(tree.Replay(&b) == nil, "second Replay error: %v", err)
		c.Check(reflect.DeepEqual(a, b), "replays differ:\n%#v\n%#v", a, b)
		c.Check(len(a.Items) == 2, "expected 2 items, got %d", len(a.Items))
		c.Check(a.Inner != nil && a.Child != nil, "expected non-nil pointers with NilChance 0")

		// Mutating a replayed object and replaying again must overwrite it.
		a.Value = -999
		a.Items = nil
		c.Check(tree.Replay(&a) == nil, "re-Replay error")
		c.Check(reflect.DeepEqual(a, b), "re-Replay did not rebuild the whole object")

		// Same seed, same config, same type => byte-identical records and
		// identical values, including node ids/seeds.
		f2 := fuzz.NewWithSeed(42).NilChance(0).NumElements(2, 2).MaxDepth(6)
		tree2, err := f2.RecordTree(&treeNode{})
		c.Check(err == nil, "second RecordTree error: %v", err)
		b1, _ := tree.MarshalBinary()
		b2, _ := tree2.MarshalBinary()
		c.Check(bytes.Equal(b1, b2), "same seed/config produced different records")
		var a2 treeNode
		c.Check(tree2.Replay(&a2) == nil, "replay of second tree error")
		c.Check(reflect.DeepEqual(a2, a), "identical records replayed differently")

		// Every scalar kind plus named types round-trips, and the leaf values
		// visible on the tree are exactly those replayed into the object.
		sf := fuzz.NewWithSeed(7).NilChance(0)
		st, err := sf.RecordTree(&treeScalars{})
		c.Check(err == nil, "scalars RecordTree error: %v", err)
		var sv treeScalars
		c.Check(st.Replay(&sv) == nil, "scalars Replay error")
		fields := st.Root().Children() // one child per struct field, in order
		c.Check(len(fields) == 13, "scalar fixture field count = %d", len(fields))
		c.Check(fields[0].BoolValue() == sv.B, "bool leaf/object mismatch")
		c.Check(fields[1].IntValue() == int64(sv.I8), "int8 leaf/object mismatch")
		c.Check(fields[5].UintValue() == uint64(sv.U8), "uint8 leaf/object mismatch")
		c.Check(fields[10].IntValue() == int64(sv.NI), "named int leaf/object mismatch")
		c.Check(len(fields[12].Children()) == 3, "array node should hold 3 elements")
		c.Check(int16(fields[12].Children()[0].IntValue()) == sv.A[0], "array element leaf/object mismatch")

		// Scalar roots (int and string) record, shrink, persist and replay.
		intTree, err := fuzz.NewWithSeed(909).RecordTree(new(int64))
		c.Check(err == nil, "int-root RecordTree error: %v", err)
		var iv0 int64
		c.Check(intTree.Replay(&iv0) == nil, "int-root Replay error")
		intReduced, _, err := intTree.Minimize(func(x interface{}) (bool, error) {
			return true, nil // always accept; drives the value to zero
		}, 100)
		c.Check(err == nil, "int-root Minimize error: %v", err)
		var iv1 int64
		c.Check(intReduced.Replay(&iv1) == nil, "int-root reduced Replay error")
		c.Check(iv1 == 0, "int root should shrink to zero, got %d", iv1)
		idata, err := intReduced.MarshalBinary()
		c.Check(err == nil, "int-root MarshalBinary error: %v", err)
		idec, err := fuzz.DecodeGenerationTree(idata, new(int64))
		c.Check(err == nil, "int-root DecodeGenerationTree error: %v", err)
		var iv2 int64
		c.Check(idec.Replay(&iv2) == nil && iv2 == 0, "int-root round-trip mismatch")

		strTree, err := fuzz.NewWithSeed(910).RecordTree(new(string))
		c.Check(err == nil, "string-root RecordTree error: %v", err)
		var sv0 string
		c.Check(strTree.Replay(&sv0) == nil && len([]rune(sv0)) > 0, "string-root Replay error")
		strReduced, _, err := strTree.Minimize(func(x interface{}) (bool, error) {
			return len([]rune(*x.(*string))) >= 1, nil
		}, 100)
		c.Check(err == nil, "string-root Minimize error: %v", err)
		var sv1 string
		c.Check(strReduced.Replay(&sv1) == nil, "string-root reduced Replay error")
		c.Check(len([]rune(sv1)) == 1, "string root should shrink to one rune, got %q", sv1)
	})

	taskcheck.Run(t, func(c *taskcheck.C) {
		// Recursive types truncate at MaxDepth.
		f := fuzz.NewWithSeed(11).NilChance(0).MaxDepth(3)
		tree, err := f.RecordTree(&treeRecChain{})
		c.Check(err == nil, "recursive RecordTree error: %v", err)
		var v treeRecChain
		c.Check(tree.Replay(&v) == nil, "recursive Replay error")
		c.Check(v.Next != nil, "depth-3 chain should keep the first pointer")
		c.Check(v.Next.Next == nil, "depth cut-off not honoured on replay")

		deepestNil := false
		var walk func(n *fuzz.GenerationNode)
		walk = func(n *fuzz.GenerationNode) {
			if n.Kind() == fuzz.KindPointer && n.IsNil() {
				deepestNil = true
			}
			for _, ch := range n.Children() {
				walk(ch)
			}
		}
		walk(tree.Root())
		c.Check(deepestNil, "no depth-cut nil pointer recorded")

		// NilChance(1) records nil pointers without children.
		allNil, err := fuzz.NewWithSeed(3).NilChance(1).RecordTree(&treeNode{})
		c.Check(err == nil, "nil-chance RecordTree error: %v", err)
		var nv treeNode
		c.Check(allNil.Replay(&nv) == nil, "nil-chance Replay error")
		c.Check(nv.Inner == nil && nv.Child == nil, "NilChance(1) produced non-nil pointers")

		// Pre-flight rejection consumes no randomness: a failed RecordTree
		// followed by a successful one matches a pristine fuzzer exactly.
		bad := struct {
			OK int
			M  map[string]int
		}{}
		failed := fuzz.NewWithSeed(99)
		_, err = failed.RecordTree(&bad)
		c.Check(errors.Is(err, fuzz.ErrTreeUnsupportedType), "want unsupported error, got %v", err)
		c.Check(err.Error() != "", "error carries no message/path")

		_, err = fuzz.NewWithSeed(5).AllowUnexportedFields(true).RecordTree(&treeNode{})
		c.Check(errors.Is(err, fuzz.ErrTreeUnsupportedType), "AllowUnexportedFields should be rejected")

		good1, err := failed.RecordTree(&treeNode{})
		c.Check(err == nil, "RecordTree after pre-flight failure error: %v", err)
		good2, err := fuzz.NewWithSeed(99).RecordTree(&treeNode{})
		c.Check(err == nil, "pristine RecordTree error: %v", err)
		g1, _ := good1.MarshalBinary()
		g2, _ := good2.MarshalBinary()
		c.Check(bytes.Equal(g1, g2), "failed pre-flight consumed randomness")

		// Skipped fields do not participate in the unsupported-type check and
		// stay zero on replay; an unskipped float is rejected with a path.
		skipped := struct {
			Keep    int
			XXX_Map map[string]int
		}{}
		st, err := fuzz.NewWithSeed(1).SkipFieldsWithPattern(regexp.MustCompile(`^XXX_`)).RecordTree(&skipped)
		c.Check(err == nil, "skipped unsupported field should be allowed, got %v", err)
		var sv struct {
			Keep    int
			XXX_Map map[string]int
		}
		c.Check(st.Replay(&sv) == nil, "skipped-field Replay error")
		c.Check(sv.XXX_Map == nil, "skipped field must remain zero")

		withFloat := struct {
			F float64
		}{}
		_, err = fuzz.NewWithSeed(1).RecordTree(&withFloat)
		c.Check(errors.Is(err, fuzz.ErrTreeUnsupportedType), "float should be rejected, got %v", err)

		// Custom fuzz functions and fuzz.Interface implementations are refused.
		cf := fuzz.NewWithSeed(1).Funcs(func(i *int32, cc fuzz.Continue) { *i = 7 })
		_, err = cf.RecordTree(&struct{ V int32 }{})
		c.Check(errors.Is(err, fuzz.ErrTreeUnsupportedType), "custom fuzz func should be rejected, got %v", err)
		selfFuzz := treeSelfFuzzer(0)
		_, err = fuzz.NewWithSeed(1).RecordTree(&selfFuzz)
		c.Check(errors.Is(err, fuzz.ErrTreeUnsupportedType), "fuzz.Interface should be rejected, got %v", err)

		// Unexported, non-skipped fields stay zero and do not break replay.
		exportedOnly, err := fuzz.NewWithSeed(1).RecordTree(&treeMixed{})
		c.Check(err == nil, "mixed-visibility RecordTree error: %v", err)
		var mv treeMixed
		c.Check(exportedOnly.Replay(&mv) == nil, "mixed-visibility Replay error")
		c.Check(mv.Secret() == 0, "unexported field must remain zero, got %d", mv.Secret())
		c.Check(mv.Public != 0, "exported field should be generated")

		// Bad arguments.
		_, err = fuzz.NewWithSeed(1).RecordTree(nil)
		c.Check(errors.Is(err, fuzz.ErrTreeInvalidArgument), "nil dst should fail argument check")
		var notPtr int
		_, err = fuzz.NewWithSeed(1).RecordTree(notPtr)
		c.Check(errors.Is(err, fuzz.ErrTreeInvalidArgument), "non-pointer dst should fail argument check")
		c.Check(tree.Replay(nil) != nil, "nil Replay dst should error")
	})
}

func TestTreeReduction(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Structural slice reduction: keep at least one item. The longest-first
		// order keeps the original last element, with its id, seed and value.
		f := fuzz.NewWithSeed(101).NilChance(0).NumElements(4, 4)
		tree, err := f.RecordTree(&treeBasket{})
		c.Check(err == nil, "RecordTree error: %v", err)
		var original treeBasket
		c.Check(tree.Replay(&original) == nil, "baseline Replay error")
		c.Check(len(original.Items) == 4, "fixture should start with 4 items")

		sliceNode := firstKind(tree.Root(), fuzz.KindSlice)
		c.Check(sliceNode != nil, "no slice node found")
		before := sliceNode.Children()
		lastSurvivor := before[len(before)-1]

		keepCalls := 0
		reduced, calls, err := tree.Minimize(func(x interface{}) (bool, error) {
			keepCalls++
			b := x.(*treeBasket)
			// Pin the surviving element's value as well so that leaf
			// shrinking cannot mask the structural identity checks below.
			valueOK := len(b.Items) == 0 || b.Items[len(b.Items)-1] == original.Items[3]
			return len(b.Items) >= 1 && valueOK && b.Tail == original.Tail && b.Mark == original.Mark, nil
		}, 1000)
		c.Check(err == nil, "Minimize error: %v", err)
		c.Check(calls == keepCalls, "call accounting mismatch: %d vs %d", calls, keepCalls)
		c.Check(calls >= 2, "expected several keep calls, got %d", calls)

		var got treeBasket
		c.Check(reduced.Replay(&got) == nil, "reduced Replay error")
		c.Check(len(got.Items) == 1, "want exactly one surviving item, got %d", len(got.Items))
		c.Check(got.Items[0] == original.Items[3], "survivor value changed: %d vs %d", got.Items[0], original.Items[3])
		c.Check(got.Tail == original.Tail && got.Mark == original.Mark, "sibling fields changed")

		newSlice := firstKind(reduced.Root(), fuzz.KindSlice)
		c.Check(len(newSlice.Children()) == 1, "reduced slice node should have one child")
		survivor := newSlice.Children()[0]
		c.Check(survivor.ID() == lastSurvivor.ID(), "survivor id %d != original last id %d", survivor.ID(), lastSurvivor.ID())
		c.Check(survivor.Seed() == lastSurvivor.Seed(), "survivor seed was re-drawn")
		c.Check(survivor.Slot() == lastSurvivor.Slot(), "survivor was renumbered: slot %d", survivor.Slot())

		// The receiver tree stays untouched (immutable snapshot semantics).
		var untouched treeBasket
		c.Check(tree.Replay(&untouched) == nil, "original Replay error")
		c.Check(reflect.DeepEqual(untouched, original), "Minimize mutated the original tree")

		// String reduction works on runes; shortening below the kept length is
		// rejected, surviving runes keep identity and order.
		st, err := fuzz.NewWithSeed(202).NilChance(0).RecordTree(&treeBasket{})
		c.Check(err == nil, "string RecordTree error: %v", err)
		var sb treeBasket
		c.Check(st.Replay(&sb) == nil, "string baseline Replay error")
		wantRunes := []rune(sb.Name)
		c.Check(len(wantRunes) >= 2, "fixture string too short: %q", sb.Name)
		strNode := firstKind(st.Root(), fuzz.KindString)
		origRuneIDs := make([]uint64, len(strNode.Children()))
		for i, rn := range strNode.Children() {
			origRuneIDs[i] = rn.ID()
		}
		redStr, _, err := st.Minimize(func(x interface{}) (bool, error) {
			b := x.(*treeBasket)
			return len([]rune(b.Name)) >= 1, nil
		}, 1000)
		c.Check(err == nil, "string Minimize error: %v", err)
		var sgot treeBasket
		c.Check(redStr.Replay(&sgot) == nil, "string reduced Replay error")
		c.Check(len([]rune(sgot.Name)) == 1, "want one rune, got %q", sgot.Name)
		c.Check([]rune(sgot.Name)[0] == wantRunes[len(wantRunes)-1], "wrong rune survived")
		c.Check(firstKind(redStr.Root(), fuzz.KindString).Children()[0].ID() == origRuneIDs[len(origRuneIDs)-1],
			"surviving rune id changed")

		// Pointer nil-ing and leaf shrinking on an always-accepting keep.
		pt, err := fuzz.NewWithSeed(303).NilChance(0).RecordTree(&treeNode{})
		c.Check(err == nil, "pointer RecordTree error: %v", err)
		redPtr, pcalls, err := pt.Minimize(func(x interface{}) (bool, error) { return true, nil }, 5000)
		c.Check(err == nil, "pointer Minimize error: %v", err)
		c.Check(pcalls <= 5000, "budget overrun: %d", pcalls)
		var pv treeNode
		c.Check(redPtr.Replay(&pv) == nil, "pointer reduced Replay error")
		c.Check(pv.Inner == nil && pv.Child == nil, "pointers should shrink to nil")
		var flagOverride bool
		var walk func(n *fuzz.GenerationNode)
		walk = func(n *fuzz.GenerationNode) {
			if n.IsLeaf() && n.IsOverridden() {
				flagOverride = true
			}
			for _, ch := range n.Children() {
				walk(ch)
			}
		}
		walk(redPtr.Root())
		c.Check(flagOverride, "no leaf override recorded during minimization")
	})

	taskcheck.Run(t, func(c *taskcheck.C) {
		tree, err := fuzz.NewWithSeed(404).NilChance(0).NumElements(3, 3).RecordTree(&treeBasket{})
		c.Check(err == nil, "RecordTree error: %v", err)
		var original treeBasket
		c.Check(tree.Replay(&original) == nil, "baseline Replay error")

		// Budget below one is an error before any keep call.
		_, zeroCalls, err := tree.Minimize(func(interface{}) (bool, error) { return true, nil }, 0)
		c.Check(errors.Is(err, fuzz.ErrTreeBudget), "want ErrTreeBudget, got %v", err)
		c.Check(zeroCalls == 0, "budget error made keep calls: %d", zeroCalls)

		// A failing original condition is itself the first (and only) call.
		_, calls, err := tree.Minimize(func(interface{}) (bool, error) { return false, nil }, 100)
		c.Check(errors.Is(err, fuzz.ErrTreeConditionNotMet), "want condition error, got %v", err)
		c.Check(calls == 1, "initial check should count once, got %d", calls)
		before, _ := tree.MarshalBinary()
		after, _ := func() ([]byte, error) {
			t2, _, _ := tree.Minimize(func(interface{}) (bool, error) { return false, nil }, 100)
			return t2.MarshalBinary()
		}()
		c.Check(bytes.Equal(before, after), "rejected minimization changed the tree")

		// Budget exhaustion returns the last accepted tree and exact count.
		exhausted, ecalls, err := tree.Minimize(func(x interface{}) (bool, error) {
			return true, nil
		}, 3)
		c.Check(err == nil, "exhaustion should not be an error: %v", err)
		c.Check(ecalls == 3, "want 3 calls, got %d", ecalls)
		exBytes, _ := exhausted.MarshalBinary()
		c.Check(!bytes.Equal(exBytes, before), "no candidate accepted within budget")

		// A keep error stops the run and returns the last accepted tree.
		errBoom := errors.New("boom")
		stage := 0
		errTree, ecalls2, err := tree.Minimize(func(x interface{}) (bool, error) {
			stage++
			if stage == 3 {
				return false, errBoom
			}
			return true, nil
		}, 100)
		c.Check(errors.Is(err, errBoom), "want propagated error, got %v", err)
		c.Check(ecalls2 == 3, "want 3 calls before error, got %d", ecalls2)
		var ev treeBasket
		c.Check(errTree.Replay(&ev) == nil, "error-path tree must still replay")
		errBytes, _ := errTree.MarshalBinary()
		c.Check(bytes.Equal(errBytes, exBytes) || !bytes.Equal(errBytes, before),
			"error-path tree is neither the prior accepted tree nor a new one")

		// Callback mutations are isolated: every invocation gets a fresh
		// object, and the final tree is not contaminated.
		mutCalls := 0
		isoTree, _, err := tree.Minimize(func(x interface{}) (bool, error) {
			b := x.(*treeBasket)
			c.Check(b.Tail != 1234567, "callback mutation leaked into a later invocation")
			b.Tail = 1234567
			b.Items = append(b.Items, 999)
			mutCalls++
			return len(b.Items) >= 1, nil
		}, 50)
		c.Check(err == nil, "isolation Minimize error: %v", err)
		c.Check(mutCalls >= 2, "isolation fixture made %d calls", mutCalls)
		var iv treeBasket
		c.Check(isoTree.Replay(&iv) == nil, "isolation tree Replay error")
		c.Check(iv.Tail != 1234567, "callback mutation contaminated the result tree")
		for _, v := range iv.Items {
			c.Check(v != 999, "callback-appended item contaminated the result tree")
		}
	})
}

func TestTreePersistence(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		tree, err := fuzz.NewWithSeed(505).NilChance(0).NumElements(2, 2).MaxDepth(5).
			RecordTree(&treeNode{})
		c.Check(err == nil, "RecordTree error: %v", err)
		var live treeNode
		c.Check(tree.Replay(&live) == nil, "live Replay error")

		data, err := tree.MarshalBinary()
		c.Check(err == nil, "MarshalBinary error: %v", err)
		c.Check(len(data) > 0 && len(data) < 64<<20, "record size out of sensible bounds")

		// Decode binds the static type from the prototype without writing to it.
		proto := treeNode{Value: 777, Name: "proto-sentinel"}
		decoded, err := fuzz.DecodeGenerationTree(data, &proto)
		c.Check(err == nil, "DecodeGenerationTree error: %v", err)
		c.Check(proto.Value == 777 && proto.Name == "proto-sentinel", "prototype was modified")

		var dv treeNode
		c.Check(decoded.Replay(&dv) == nil, "decoded Replay error")
		c.Check(reflect.DeepEqual(dv, live), "decoded tree replays a different value:\n%#v\n%#v", dv, live)

		// Canonical encoding: decode -> encode is byte-stable, ids survive.
		data2, err := decoded.MarshalBinary()
		c.Check(err == nil, "re-MarshalBinary error: %v", err)
		c.Check(bytes.Equal(data, data2), "encoding is not canonical")
		c.Check(decoded.Root().ID() == tree.Root().ID(), "root id changed across persistence")
		flat1 := flatten(c, tree)
		flat2 := flatten(c, decoded)
		c.Check(len(flat1) == len(flat2), "node count changed across persistence: %d vs %d", len(flat1), len(flat2))
		for i := range flat1 {
			c.Check(flat1[i].ID() == flat2[i].ID() && flat1[i].Seed() == flat2[i].Seed(),
				"node %d identity changed across persistence", i)
		}

		// A minimized tree can be saved on its own and replays identically.
		reduced, _, err := tree.Minimize(func(x interface{}) (bool, error) {
			n := x.(*treeNode)
			return len(n.Items) >= 1, nil
		}, 500)
		c.Check(err == nil, "reduction error: %v", err)
		rdata, err := reduced.MarshalBinary()
		c.Check(err == nil, "reduced MarshalBinary error: %v", err)
		rdec, err := fuzz.DecodeGenerationTree(rdata, &treeNode{})
		c.Check(err == nil, "reduced decode error: %v", err)
		var rv1, rv2 treeNode
		c.Check(reduced.Replay(&rv1) == nil, "reduced replay error")
		c.Check(rdec.Replay(&rv2) == nil, "reduced-decoded replay error")
		c.Check(reflect.DeepEqual(rv1, rv2), "persisted reduced tree diverged")
	})

	taskcheck.Run(t, func(c *taskcheck.C) {
		tree, err := fuzz.NewWithSeed(606).NilChance(0).RecordTree(&treeScalars{})
		c.Check(err == nil, "RecordTree error: %v", err)
		data, err := tree.MarshalBinary()
		c.Check(err == nil, "MarshalBinary error: %v", err)

		mustReject := func(name string, mutate func([]byte) []byte, proto interface{}) {
			raw := mutate(append([]byte(nil), data...))
			dec, derr := fuzz.DecodeGenerationTree(raw, proto)
			c.Check(derr != nil && errors.Is(derr, fuzz.ErrTreeCorrupt), "%s: want ErrTreeCorrupt, got %v", name, derr)
			c.Check(dec == nil, "%s: returned a tree despite invalid record", name)
		}

		mustReject("truncated", func(b []byte) []byte { return b[:len(b)-5] }, &treeScalars{})
		mustReject("trailing", func(b []byte) []byte { return append(b, 0xEE) }, &treeScalars{})
		mustReject("bad magic", func(b []byte) []byte { b[0] = 'X'; return b }, &treeScalars{})
		mustReject("bad version", func(b []byte) []byte {
			binary.BigEndian.PutUint16(b[4:6], 999)
			return b
		}, &treeScalars{})

		// Header layout up to the first (root) node: magic(4) version(2)
		// boundType sstring(4+nameLen) config(8+4+4+4+1+4) nodeCount(4)
		// rootID(8). The root node's own id field is the next 8 bytes.
		const nameLen = len("fuzz_test.treeScalars")
		rootNodeOff := 4 + 2 + 4 + nameLen + 8 + 4 + 4 + 4 + 1 + 4 + 4 + 8
		c.Check(binary.BigEndian.Uint64(data[rootNodeOff:rootNodeOff+8]) == 1, "root offset self-check failed")
		mustReject("zero node id", func(b []byte) []byte {
			for i := 0; i < 8; i++ {
				b[rootNodeOff+i] = 0
			}
			return b
		}, &treeScalars{})
		mustReject("root id mismatch", func(b []byte) []byte {
			// rootID lives 8 bytes before the root node record.
			binary.BigEndian.PutUint64(b[rootNodeOff-8:rootNodeOff], 999999)
			return b
		}, &treeScalars{})
		mustReject("bad kind", func(b []byte) []byte {
			b[rootNodeOff+8] = 99 // kind byte follows the 8-byte id
			return b
		}, &treeScalars{})

		// Out-of-range leaf value: locate the int8 node through its type name
		// and overwrite the int64 payload (after flags) with an unrepresentable
		// value. "int8" appears before "uint8" in field order.
		raw := append([]byte(nil), data...)
		at := bytes.Index(raw, []byte("int8"))
		c.Check(at > 0, "int8 node not located in record")
		payload := at + len("int8") + 1 // skip the name bytes and the flags byte
		binary.BigEndian.PutUint64(raw[payload:payload+8], 1<<40)
		dec, err := fuzz.DecodeGenerationTree(raw, &treeScalars{})
		c.Check(err != nil && errors.Is(err, fuzz.ErrTreeCorrupt), "out-of-range leaf accepted: %v", err)
		c.Check(dec == nil, "out-of-range leaf yielded a tree")

		// Type mismatch against the prototype leaves the prototype untouched.
		guard := treeNode{Value: 31337}
		dec, err = fuzz.DecodeGenerationTree(data, &guard)
		c.Check(err != nil && errors.Is(err, fuzz.ErrTreeCorrupt), "prototype mismatch accepted: %v", err)
		c.Check(dec == nil && guard.Value == 31337, "mismatched prototype mutated")

		// Decoding needs a non-nil pointer prototype and consumes no record.
		dec, err = fuzz.DecodeGenerationTree(data, nil)
		c.Check(errors.Is(err, fuzz.ErrTreeInvalidArgument), "nil prototype: %v", err)
		c.Check(dec == nil, "nil prototype returned a tree")
		dec, err = fuzz.DecodeGenerationTree(nil, &treeScalars{})
		c.Check(errors.Is(err, fuzz.ErrTreeCorrupt), "empty record: %v", err)

		// Replay type mismatch preserves dst.
		sentinel := treeNode{Value: 4242}
		err = tree.Replay(&sentinel) // tree is bound to treeScalars
		c.Check(err != nil && errors.Is(err, fuzz.ErrTreeCorrupt), "Replay mismatch: %v", err)
		c.Check(sentinel.Value == 4242, "failed Replay modified dst")
	})
}

// Example_generationTree shows recording, shrinking, persisting and replaying
// a generation tree.
func Example_generationTree() {
	type packet struct {
		Items []int32
		Tail  int64
	}

	f := fuzz.NewWithSeed(1234).NilChance(0).NumElements(4, 4)
	tree, err := f.RecordTree(&packet{})
	if err != nil {
		fmt.Println("record error:", err)
		return
	}

	var first packet
	if err := tree.Replay(&first); err != nil {
		fmt.Println("replay error:", err)
		return
	}
	fmt.Println("initial items:", len(first.Items))

	// Shrink to the smallest slice that still keeps the failure condition.
	reduced, calls, err := tree.Minimize(func(x interface{}) (bool, error) {
		return len(x.(*packet).Items) >= 1, nil
	}, 1000)
	if err != nil {
		fmt.Println("minimize error:", err)
		return
	}

	var small packet
	if err := reduced.Replay(&small); err != nil {
		fmt.Println("reduced replay error:", err)
		return
	}
	fmt.Println("reduced items:", len(small.Items))
	fmt.Println("keep calls:", calls)

	// Persist the reduced tree and replay it again from the binary record.
	data, err := reduced.MarshalBinary()
	if err != nil {
		fmt.Println("marshal error:", err)
		return
	}
	reloaded, err := fuzz.DecodeGenerationTree(data, &packet{})
	if err != nil {
		fmt.Println("decode error:", err)
		return
	}
	var again packet
	if err := reloaded.Replay(&again); err != nil {
		fmt.Println("reloaded replay error:", err)
		return
	}
	fmt.Println("persisted items:", len(again.Items))
	fmt.Println("stable replay:", reflect.DeepEqual(again, small))
	// Output:
	// initial items: 4
	// reduced items: 1
	// keep calls: 11
	// persisted items: 1
	// stable replay: true
}
