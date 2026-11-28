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

package fuzz

import (
	"fmt"
	"reflect"
)

// This file implements GenerationTree.Minimize: structural and leaf-level
// reduction driven by an external keep predicate. Every candidate is an
// independent tree; the receiver and every previously accepted tree stay
// untouched, and each keep invocation replays into a brand-new zero value so
// callback mutations cannot leak across calls.

// Minimize repeatedly shrinks the tree while keep accepts the replayed value.
//
// keep receives a fresh pointer of the same type the tree was recorded for
// and reports whether the failure condition of interest still holds. It is
// always invoked on an independently replayed object, so keep may mutate the
// object freely; such mutations never affect the tree or later calls.
//
// maxCalls bounds the number of keep invocations, including the mandatory
// initial check of the unmodified record. Candidates are enumerated in node
// preorder: slice/string interval deletions (longest first, then
// left-to-right), slice nil-ing, pointer nil-ing, bool true->false and
// non-zero integer zero/halving. Each accepted candidate restarts the
// enumeration from the root. The result is a reduced (not necessarily
// globally minimal) tree.
//
// It returns the last accepted tree, the number of keep calls actually made
// and an error: ErrTreeBudget if maxCalls < 1, ErrTreeConditionNotMet if the
// original record fails keep, or the error keep itself returned.
func (t *GenerationTree) Minimize(keep func(interface{}) (bool, error), maxCalls int) (*GenerationTree, int, error) {
	if maxCalls < 1 {
		return t, 0, ErrTreeBudget
	}
	if keep == nil {
		return t, 0, fmt.Errorf("%w: keep must not be nil", ErrTreeInvalidArgument)
	}

	calls := 0
	// Initial check on the unmodified record; this is call one.
	ok, err := t.invokeKeep(keep)
	calls++
	if err != nil {
		return t, calls, err
	}
	if !ok {
		return t, calls, ErrTreeConditionNotMet
	}

	current := t
	for {
		edits := enumerateEdits(current.root)
		progressed := false
		for _, ed := range edits {
			if calls >= maxCalls {
				return current, calls, nil
			}
			candidate, err := applyEdit(current, ed)
			if err != nil {
				return current, calls, err
			}
			ok, err := candidate.invokeKeep(keep)
			calls++
			if err != nil {
				return current, calls, err
			}
			if ok {
				current = candidate
				progressed = true
				break // re-enumerate from the new tree's root.
			}
		}
		if !progressed {
			return current, calls, nil
		}
	}
}

// invokeKeep replays the tree into a fresh value and calls keep on a pointer
// to it. The allocated object is owned solely by this call.
func (t *GenerationTree) invokeKeep(keep func(interface{}) (bool, error)) (bool, error) {
	ptr := reflect.New(t.boundType)
	if err := t.Replay(ptr.Interface()); err != nil {
		return false, err
	}
	return keep(ptr.Interface())
}

// editKind distinguishes the single-node transformations Minimize tries.
type editKind uint8

const (
	editSliceDelete editKind = iota + 1
	editSliceNil
	editStringDelete
	editPointerNil
	editLeafBool
	editLeafInt
	editLeafUint
)

// treeEdit is one candidate transformation, located by stable node ID.
type treeEdit struct {
	kind   editKind
	nodeID uint64
	start  int // interval edits: first child slot position (positional).
	length int // interval edits: number of children to remove.
	bval   bool
	ival   int64
	uval   uint64
}

// enumerateEdits walks the tree in node preorder and, for each node, appends
// every transformation applicable to it in the order the specification
// mandates.
func enumerateEdits(root *GenerationNode) []treeEdit {
	var edits []treeEdit
	var walk func(n *GenerationNode)
	walk = func(n *GenerationNode) {
		switch n.kind {
		case KindSlice:
			if !n.nilNode {
				l := len(n.children)
				for d := l; d >= 1; d-- { // longest interval first.
					for start := 0; start+d <= l; start++ { // left to right.
						edits = append(edits, treeEdit{kind: editSliceDelete, nodeID: n.id, start: start, length: d})
					}
				}
				edits = append(edits, treeEdit{kind: editSliceNil, nodeID: n.id})
			}
		case KindString:
			l := len(n.children)
			for d := l; d >= 1; d-- {
				for start := 0; start+d <= l; start++ {
					edits = append(edits, treeEdit{kind: editStringDelete, nodeID: n.id, start: start, length: d})
				}
			}
		case KindPointer:
			if !n.nilNode && len(n.children) > 0 {
				edits = append(edits, treeEdit{kind: editPointerNil, nodeID: n.id})
			}
		case KindBool:
			if n.leaf && n.BoolValue() {
				edits = append(edits, treeEdit{kind: editLeafBool, nodeID: n.id, bval: false})
			}
		case KindInt:
			if n.leaf && !n.isRune {
				v := n.IntValue()
				if v != 0 {
					edits = append(edits, treeEdit{kind: editLeafInt, nodeID: n.id, ival: 0})
					for half := halfTowardZero(v); half != 0; half = halfTowardZero(half) {
						edits = append(edits, treeEdit{kind: editLeafInt, nodeID: n.id, ival: half})
					}
				}
			}
		case KindUint:
			if n.leaf {
				v := n.UintValue()
				if v != 0 {
					edits = append(edits, treeEdit{kind: editLeafUint, nodeID: n.id, uval: 0})
					for half := v / 2; half != 0; half /= 2 {
						edits = append(edits, treeEdit{kind: editLeafUint, nodeID: n.id, uval: half})
					}
				}
			}
		}
		for _, ch := range n.children {
			walk(ch)
		}
	}
	walk(root)
	return edits
}

// halfTowardZero returns v/2 rounded toward zero. Go integer division
// already truncates toward zero and never overflows, including for the
// extreme value math.MinInt64.
func halfTowardZero(v int64) int64 {
	return v / 2
}

// applyEdit builds a detached deep copy of t with one edit applied. Node IDs,
// seeds, slots and relative ordering are preserved; only the targeted node
// changes.
func applyEdit(t *GenerationTree, ed treeEdit) (*GenerationTree, error) {
	copyRoot := deepCopyNode(t.root)
	copyTree := &GenerationTree{
		version:   t.version,
		root:      copyRoot,
		config:    t.config,
		boundType: t.boundType,
		nextID:    t.nextID,
	}
	target := findNode(copyRoot, ed.nodeID)
	if target == nil {
		return nil, fmt.Errorf("%w: internal error: edit target %d missing", ErrTreeCorrupt, ed.nodeID)
	}
	switch ed.kind {
	case editSliceDelete, editStringDelete:
		if ed.start < 0 || ed.length <= 0 || ed.start+ed.length > len(target.children) {
			return nil, fmt.Errorf("%w: internal error: bad deletion interval", ErrTreeCorrupt)
		}
		target.children = append(target.children[:ed.start], target.children[ed.start+ed.length:]...)
	case editSliceNil:
		target.nilNode = true
		target.children = nil
	case editPointerNil:
		target.nilNode = true
		target.children = nil
	case editLeafBool:
		target.overridden = true
		target.overBool = ed.bval
	case editLeafInt:
		target.overridden = true
		target.overInt = ed.ival
	case editLeafUint:
		target.overridden = true
		target.overUint = ed.uval
	default:
		return nil, fmt.Errorf("%w: internal error: unknown edit kind", ErrTreeCorrupt)
	}
	if err := copyTree.validate(copyTree.boundType); err != nil {
		return nil, err
	}
	return copyTree, nil
}

func findNode(n *GenerationNode, id uint64) *GenerationNode {
	if n.id == id {
		return n
	}
	for _, ch := range n.children {
		if found := findNode(ch, id); found != nil {
			return found
		}
	}
	return nil
}

// deepCopyNode returns a detached copy of the node subtree. IDs, seeds,
// slots and ordering are retained verbatim; nothing is shared with the
// source, so later edits cannot reach the original tree.
func deepCopyNode(n *GenerationNode) *GenerationNode {
	if n == nil {
		return nil
	}
	cp := *n
	if n.children != nil {
		cp.children = make([]*GenerationNode, len(n.children))
		for i, ch := range n.children {
			cp.children[i] = deepCopyNode(ch)
		}
	}
	return &cp
}
