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
	"encoding/binary"
	"errors"
	"fmt"
	"math"
	"math/rand"
	"reflect"
	"regexp"
	"strings"
	"unicode/utf8"
)

// This file implements generation trees: a record/replay facility that
// captures random object generation as an explicit, editable tree.
//
// Seed derivation (tree derivation version 1, recorded as the "GFTREE01"
// format version):
//
//   - RecordTree draws exactly one Int63 from the Fuzzer's random source and
//     uses it as the seed of the root node.
//   - Every node owns an independent random stream:
//     rand.New(rand.NewSource(nodeSeed)).
//   - The seed of a child node is derived from the parent seed and a stable
//     slot (struct field index, slice/array element index, or 0 for a pointer
//     element) via deriveChildSeed, a splitmix64-style mixer:
//
//     x  = uint64(parentSeed) ^ (slot * 0x9E3779B97F4A7C15) ^ 0xD1B54A32D192ED03
//     x += 0x9E3779B97F4A7C15
//     x  = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9
//     x  = (x ^ (x >> 27)) * 0x94D049BB133111EB
//     x  = x ^ (x >> 31)
//     childSeed = int64(x)
//
// Because each child stream depends only on the parent seed and the slot,
// pruning a subtree never re-seeds its surviving siblings.
//
// The binary record layout (all integers little-endian or uvarint/varint as
// noted) is:
//
//	magic        8 bytes, "GFTREE01"
//	typeBind     uvarint-length-prefixed type fingerprint string
//	nilChance    8 bytes little-endian float64
//	minElements  uvarint
//	maxElements  uvarint
//	maxDepth     uvarint
//	skipPatterns uvarint count, then that many uvarint-length-prefixed strings
//	nodeCount    uvarint, total number of nodes
//	nodes        nodeCount nodes in preorder, each:
//	  id         uvarint, unique within the record
//	  seed       8 bytes little-endian
//	  kind       1 byte (see NodeKind)
//	  payload    kind dependent:
//	    zero:    nothing
//	    bool:    flags byte (bit0: has override, bit1: override value)
//	    int:     bits byte, override flag byte, [varint override]
//	    uint:    bits byte, override flag byte, [uvarint override]
//	    string:  override flag byte, [uvarint-length-prefixed override]
//	    struct, slice, array, ptr:
//	             nilFlag byte, childCount uvarint, then childCount nodes
//
// Records contain no machine addresses and no Go object snapshots; pruning
// and leaf shrinking are represented by the explicit tree shape and by leaf
// overrides.

// Limits enforced when a record is published (RecordTree or
// DecodeGenerationTree). They bound memory use and recursion depth.
const (
	maxRecordNodes = 1 << 20
	maxRecordDepth = 1024
	maxRecordBytes = 64 << 20
)

// treeMagic identifies the record format and its version.
const treeMagic = "GFTREE01"

// treeSeedDerivationVersion identifies the child-seed derivation scheme
// documented at the top of this file. It is frozen by treeMagic.
const treeSeedDerivationVersion = 1

// NodeKind identifies what a TreeNode generates.
type NodeKind int

const (
	// NodeZero is a depth-truncated position; it replays as the zero value.
	NodeZero NodeKind = iota
	NodeBool
	NodeInt
	NodeUint
	NodeString
	NodeStruct
	NodeSlice
	NodeArray
	NodePtr
)

// String returns a short human-readable name for the kind.
func (k NodeKind) String() string {
	switch k {
	case NodeZero:
		return "zero"
	case NodeBool:
		return "bool"
	case NodeInt:
		return "int"
	case NodeUint:
		return "uint"
	case NodeString:
		return "string"
	case NodeStruct:
		return "struct"
	case NodeSlice:
		return "slice"
	case NodeArray:
		return "array"
	case NodePtr:
		return "ptr"
	}
	return "unknown"
}

// TreeNode is one node of a GenerationTree. Nodes are immutable snapshots:
// once a tree is published by RecordTree, DecodeGenerationTree or Minimize,
// its nodes never change. All state is private; the accessor methods are the
// read-only interface. Nodes returned by accessors are shared with the
// owning tree and must be treated as read-only; they remain valid for as
// long as any tree referencing them is reachable. Trees produced by Minimize
// may share substructure with their input, which is safe because both are
// immutable.
type TreeNode struct {
	id   uint64
	seed int64
	kind NodeKind

	bits    uint8 // bit width for NodeInt/NodeUint leaves
	nilFlag bool  // NodeSlice/NodePtr: nil; NodeArray: left as zero value

	children []*TreeNode

	hasOverride bool
	boolVal     bool
	intVal      int64
	uintVal     uint64
	strVal      string
}

// ID returns the stable identity of this node within its tree. Surviving
// nodes keep their IDs across minimization.
func (n *TreeNode) ID() uint64 { return n.id }

// Kind returns what this node generates.
func (n *TreeNode) Kind() NodeKind { return n.kind }

// Seed returns the node's local generation seed, derived from its parent
// seed and slot at record time and preserved by minimization.
func (n *TreeNode) Seed() int64 { return n.seed }

// NumChildren returns the number of child nodes, in generation order.
func (n *TreeNode) NumChildren() int { return len(n.children) }

// Child returns the i-th child node, in generation order. For structs the
// children correspond to the generated (exported, non-skipped) fields in
// declaration order; for slices and arrays to element indices; for pointers
// there is at most one child (the pointee).
func (n *TreeNode) Child(i int) *TreeNode { return n.children[i] }

// IsNil reports whether a NodeSlice or NodePtr node produces nil, or whether
// a NodeArray node is left as the zero value.
func (n *TreeNode) IsNil() bool { return n.nilFlag }

// LeafValue returns the effective value of a leaf node (bool, int64, uint64
// or string) after applying any minimization override. The second return
// value is false for non-leaf nodes. Integer values are truncated to the
// recorded bit width of the field they were generated for.
func (n *TreeNode) LeafValue() (interface{}, bool) {
	switch n.kind {
	case NodeBool:
		return n.effectiveBool(), true
	case NodeInt:
		return n.effectiveInt(), true
	case NodeUint:
		return n.effectiveUint(), true
	case NodeString:
		return n.effectiveString(), true
	}
	return nil, false
}

func (n *TreeNode) effectiveBool() bool {
	if n.hasOverride {
		return n.boolVal
	}
	return randBool(rand.New(rand.NewSource(n.seed)))
}

func (n *TreeNode) effectiveInt() int64 {
	if n.hasOverride {
		return n.intVal
	}
	v := int64(randUint64(rand.New(rand.NewSource(n.seed))))
	return truncateIntBits(v, n.bits)
}

func (n *TreeNode) effectiveUint() uint64 {
	if n.hasOverride {
		return n.uintVal
	}
	v := randUint64(rand.New(rand.NewSource(n.seed)))
	return truncateUintBits(v, n.bits)
}

func (n *TreeNode) effectiveString() string {
	if n.hasOverride {
		return n.strVal
	}
	return randString(rand.New(rand.NewSource(n.seed)))
}

func truncateIntBits(v int64, bits uint8) int64 {
	if bits == 0 || bits >= 64 {
		return v
	}
	s := 64 - bits
	return v << s >> s
}

func truncateUintBits(v uint64, bits uint8) uint64 {
	if bits == 0 || bits >= 64 {
		return v
	}
	return v & (uint64(1)<<bits - 1)
}

// shallowCopy copies n without copying the child slice header's backing
// array; callers replace children explicitly.
func (n *TreeNode) shallowCopy() *TreeNode {
	cp := *n
	return &cp
}

// GenerationTree is a recorded object generation. It is immutable once
// published; Replay, MarshalBinary and the read-only node accessors may be
// used concurrently. Minimize returns new trees instead of modifying the
// receiver.
type GenerationTree struct {
	root         *TreeNode
	rootType     reflect.Type
	typeBind     string
	nilChance    float64
	minElements  int
	maxElements  int
	maxDepth     int
	skipPatterns []string
	skipRes      []*regexp.Regexp
}

// Root returns the root node of the tree. The returned node is shared with
// the tree and is read-only.
func (t *GenerationTree) Root() *TreeNode { return t.root }

// Type returns the type of the generated object (the element type of the
// pointer passed to RecordTree or DecodeGenerationTree).
func (t *GenerationTree) Type() reflect.Type { return t.rootType }

// deriveChildSeed implements tree derivation version 1; see the file-level
// comment. It must never change without bumping the record format version.
func deriveChildSeed(parentSeed int64, slot uint64) int64 {
	x := uint64(parentSeed) ^ (slot * 0x9E3779B97F4A7C15) ^ 0xD1B54A32D192ED03
	x += 0x9E3779B97F4A7C15
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9
	x = (x ^ (x >> 27)) * 0x94D049BB133111EB
	x = x ^ (x >> 31)
	return int64(x)
}

var treeInterfaceType = reflect.TypeOf((*Interface)(nil)).Elem()

// RecordTree generates random content into dst (which must be a non-nil
// pointer) and returns a tree recording exactly how the object was
// generated, so it can be replayed, minimized and persisted.
//
// Unlike Fuzz, this mode supports only bools, signed and unsigned integers
// (including named types), strings, and structs, arrays, slices and pointers
// built from them. Recursive types are supported and are expanded up to
// MaxDepth. Maps, interfaces, floats, complexes, chans, funcs and
// unsafe.Pointer are rejected, as are types reachable through custom Funcs,
// fuzz.Interface implementations or default handlers, and the
// AllowUnexportedFields option. The type and configuration precheck runs
// before any randomness is consumed; on precheck failure the Fuzzer's random
// stream is untouched. On success exactly one Int63 is drawn from the
// Fuzzer's random source as the root seed. NilChance, NumElements, MaxDepth
// and the field skip patterns are frozen into the record. Unexported and
// skipped fields are left at their zero values.
func (f *Fuzzer) RecordTree(dst interface{}) (*GenerationTree, error) {
	f.fuzzLock.Lock()
	defer f.fuzzLock.Unlock()

	v := reflect.ValueOf(dst)
	if v.Kind() != reflect.Ptr || v.IsNil() {
		return nil, errors.New("gofuzz: RecordTree needs a non-nil pointer")
	}
	t := v.Type().Elem()
	if err := f.precheckTreeConfig(); err != nil {
		return nil, err
	}
	if err := f.precheckTreeType(t, t.String(), map[reflect.Type]bool{}); err != nil {
		return nil, err
	}

	// Exactly one draw from the Fuzzer's random source.
	rootSeed := f.r.Int63()

	g := &treeGen{f: f}
	fresh := reflect.New(t).Elem()
	root, err := g.genNode(fresh, rootSeed, 0)
	if err != nil {
		return nil, err
	}
	tree := &GenerationTree{
		root:        root,
		rootType:    t,
		typeBind:    treeTypeFingerprint(t),
		nilChance:   f.nilChance,
		minElements: f.minElements,
		maxElements: f.maxElements,
		maxDepth:    f.maxDepth,
		skipRes:     append([]*regexp.Regexp(nil), f.skipFieldPatterns...),
	}
	for _, p := range f.skipFieldPatterns {
		tree.skipPatterns = append(tree.skipPatterns, p.String())
	}
	v.Elem().Set(fresh)
	return tree, nil
}

func (f *Fuzzer) precheckTreeConfig() error {
	if f.allowUnexportedFields {
		return errors.New("gofuzz: RecordTree: AllowUnexportedFields is not supported")
	}
	if f.maxDepth < 1 || f.maxDepth > maxRecordDepth {
		return fmt.Errorf("gofuzz: RecordTree: MaxDepth %d is outside the supported range [1, %d]", f.maxDepth, maxRecordDepth)
	}
	return nil
}

func (f *Fuzzer) skipFieldByName(name string) bool {
	for _, p := range f.skipFieldPatterns {
		if p.MatchString(name) {
			return true
		}
	}
	return false
}

// precheckTreeType verifies that the type at the given field path can be
// handled by tree generation. Skipped and unexported fields are not checked.
func (f *Fuzzer) precheckTreeType(t reflect.Type, path string, seen map[reflect.Type]bool) error {
	if seen[t] {
		return nil
	}
	seen[t] = true

	if _, ok := f.fuzzFuncs[t]; ok {
		return fmt.Errorf("gofuzz: RecordTree: custom fuzz func for %s at %s is not supported", t, path)
	}
	if _, ok := f.fuzzFuncs[reflect.PtrTo(t)]; ok {
		return fmt.Errorf("gofuzz: RecordTree: custom fuzz func for *%s at %s is not supported", t, path)
	}
	if _, ok := f.defaultFuzzFuncs[t]; ok {
		return fmt.Errorf("gofuzz: RecordTree: default handler for %s at %s is not supported", t, path)
	}
	if _, ok := f.defaultFuzzFuncs[reflect.PtrTo(t)]; ok {
		return fmt.Errorf("gofuzz: RecordTree: default handler for *%s at %s is not supported", t, path)
	}
	if t.Implements(treeInterfaceType) || reflect.PtrTo(t).Implements(treeInterfaceType) {
		return fmt.Errorf("gofuzz: RecordTree: type implementing fuzz.Interface at %s is not supported", path)
	}

	switch t.Kind() {
	case reflect.Bool,
		reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
		reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr,
		reflect.String:
		return nil
	case reflect.Struct:
		for i := 0; i < t.NumField(); i++ {
			field := t.Field(i)
			if field.PkgPath != "" { // unexported
				continue
			}
			if f.skipFieldByName(field.Name) {
				continue
			}
			if err := f.precheckTreeType(field.Type, path+"."+field.Name, seen); err != nil {
				return err
			}
		}
		return nil
	case reflect.Slice, reflect.Array:
		return f.precheckTreeType(t.Elem(), path+"[]", seen)
	case reflect.Ptr:
		return f.precheckTreeType(t.Elem(), path+"*", seen)
	default:
		return fmt.Errorf("gofuzz: RecordTree: unsupported type %s at %s", t, path)
	}
}

// treeGen holds state for a single RecordTree run.
type treeGen struct {
	f      *Fuzzer
	nodes  uint64
	nextID uint64
}

func treeGenCount(r *rand.Rand, min, max int) int {
	if min == max {
		return min
	}
	return min + r.Intn(max-min+1)
}

// genNode mirrors doFuzz's dispatch, but every node draws from its own
// stream derived from its seed.
func (g *treeGen) genNode(v reflect.Value, seed int64, depth int) (*TreeNode, error) {
	g.nodes++
	if g.nodes > maxRecordNodes {
		return nil, fmt.Errorf("gofuzz: RecordTree: record exceeds the limit of %d nodes", maxRecordNodes)
	}
	id := g.nextID
	g.nextID++
	if depth >= g.f.maxDepth {
		// Depth truncation: the value stays at its zero value.
		return &TreeNode{id: id, seed: seed, kind: NodeZero}, nil
	}
	r := rand.New(rand.NewSource(seed))
	t := v.Type()
	switch v.Kind() {
	case reflect.Bool:
		v.SetBool(randBool(r))
		return &TreeNode{id: id, seed: seed, kind: NodeBool}, nil
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		fuzzInt(v, r)
		return &TreeNode{id: id, seed: seed, kind: NodeInt, bits: uint8(t.Bits())}, nil
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		fuzzUint(v, r)
		return &TreeNode{id: id, seed: seed, kind: NodeUint, bits: uint8(t.Bits())}, nil
	case reflect.String:
		v.SetString(randString(r))
		return &TreeNode{id: id, seed: seed, kind: NodeString}, nil
	case reflect.Ptr:
		n := &TreeNode{id: id, seed: seed, kind: NodePtr}
		if r.Float64() >= g.f.nilChance {
			v.Set(reflect.New(t.Elem()))
			child, err := g.genNode(v.Elem(), deriveChildSeed(seed, 0), depth+1)
			if err != nil {
				return nil, err
			}
			n.children = []*TreeNode{child}
		} else {
			v.Set(reflect.Zero(t))
			n.nilFlag = true
		}
		return n, nil
	case reflect.Slice:
		n := &TreeNode{id: id, seed: seed, kind: NodeSlice}
		if r.Float64() >= g.f.nilChance {
			count := treeGenCount(r, g.f.minElements, g.f.maxElements)
			v.Set(reflect.MakeSlice(t, count, count))
			for i := 0; i < count; i++ {
				child, err := g.genNode(v.Index(i), deriveChildSeed(seed, uint64(i)), depth+1)
				if err != nil {
					return nil, err
				}
				n.children = append(n.children, child)
			}
		} else {
			v.Set(reflect.Zero(t))
			n.nilFlag = true
		}
		return n, nil
	case reflect.Array:
		n := &TreeNode{id: id, seed: seed, kind: NodeArray}
		if r.Float64() >= g.f.nilChance {
			for i := 0; i < v.Len(); i++ {
				child, err := g.genNode(v.Index(i), deriveChildSeed(seed, uint64(i)), depth+1)
				if err != nil {
					return nil, err
				}
				n.children = append(n.children, child)
			}
		} else {
			v.Set(reflect.Zero(t))
			n.nilFlag = true
		}
		return n, nil
	case reflect.Struct:
		n := &TreeNode{id: id, seed: seed, kind: NodeStruct}
		for i := 0; i < t.NumField(); i++ {
			field := t.Field(i)
			if field.PkgPath != "" { // unexported
				continue
			}
			if g.f.skipFieldByName(field.Name) {
				continue
			}
			child, err := g.genNode(v.Field(i), deriveChildSeed(seed, uint64(i)), depth+1)
			if err != nil {
				return nil, err
			}
			n.children = append(n.children, child)
		}
		return n, nil
	}
	// Unreachable: precheckTreeType rejected every other kind.
	return nil, fmt.Errorf("gofuzz: RecordTree: unsupported type %s", t)
}

// Replay regenerates the recorded object into dst, which must be a non-nil
// pointer to the type the tree was recorded from. The object is fully
// generated in a fresh zero value first; only on success is dst overwritten.
// Replay uses no Fuzzer and consumes no external randomness: structural
// choices come from the tree and leaf values from the recorded seeds or
// overrides. On any type mismatch or invalid record, dst is left untouched.
func (t *GenerationTree) Replay(dst interface{}) error {
	if t.rootType == nil || t.root == nil {
		return errors.New("gofuzz: Replay: empty generation tree")
	}
	v := reflect.ValueOf(dst)
	if v.Kind() != reflect.Ptr || v.IsNil() {
		return errors.New("gofuzz: Replay needs a non-nil pointer")
	}
	if v.Type().Elem() != t.rootType {
		return fmt.Errorf("gofuzz: Replay: tree holds %s, cannot replay into %s", t.rootType, v.Type().Elem())
	}
	fresh := reflect.New(t.rootType).Elem()
	rc := &replayCtx{skipRes: t.skipRes}
	if err := rc.replayNode(fresh, t.root); err != nil {
		return err
	}
	v.Elem().Set(fresh)
	return nil
}

type replayCtx struct {
	skipRes []*regexp.Regexp
}

func skipNameByRes(skipRes []*regexp.Regexp, name string) bool {
	for _, re := range skipRes {
		if re.MatchString(name) {
			return true
		}
	}
	return false
}

func (rc *replayCtx) replayNode(v reflect.Value, n *TreeNode) error {
	t := v.Type()
	switch n.kind {
	case NodeZero:
		return nil
	case NodeBool:
		if t.Kind() != reflect.Bool {
			return rc.mismatch(v, n)
		}
		v.SetBool(n.effectiveBool())
		return nil
	case NodeInt:
		switch t.Kind() {
		case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		default:
			return rc.mismatch(v, n)
		}
		if n.hasOverride {
			v.SetInt(n.intVal)
		} else {
			fuzzInt(v, rand.New(rand.NewSource(n.seed)))
		}
		return nil
	case NodeUint:
		switch t.Kind() {
		case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		default:
			return rc.mismatch(v, n)
		}
		if n.hasOverride {
			v.SetUint(n.uintVal)
		} else {
			fuzzUint(v, rand.New(rand.NewSource(n.seed)))
		}
		return nil
	case NodeString:
		if t.Kind() != reflect.String {
			return rc.mismatch(v, n)
		}
		if n.hasOverride {
			v.SetString(n.strVal)
		} else {
			v.SetString(randString(rand.New(rand.NewSource(n.seed))))
		}
		return nil
	case NodePtr:
		if t.Kind() != reflect.Ptr {
			return rc.mismatch(v, n)
		}
		if n.nilFlag {
			if len(n.children) != 0 {
				return fmt.Errorf("gofuzz: Replay: nil ptr node %d has children", n.id)
			}
			return nil
		}
		if len(n.children) != 1 {
			return fmt.Errorf("gofuzz: Replay: ptr node %d has %d children", n.id, len(n.children))
		}
		v.Set(reflect.New(t.Elem()))
		return rc.replayNode(v.Elem(), n.children[0])
	case NodeSlice:
		if t.Kind() != reflect.Slice {
			return rc.mismatch(v, n)
		}
		if n.nilFlag {
			if len(n.children) != 0 {
				return fmt.Errorf("gofuzz: Replay: nil slice node %d has children", n.id)
			}
			return nil
		}
		v.Set(reflect.MakeSlice(t, len(n.children), len(n.children)))
		for i, c := range n.children {
			if err := rc.replayNode(v.Index(i), c); err != nil {
				return err
			}
		}
		return nil
	case NodeArray:
		if t.Kind() != reflect.Array {
			return rc.mismatch(v, n)
		}
		if n.nilFlag {
			if len(n.children) != 0 {
				return fmt.Errorf("gofuzz: Replay: zeroed array node %d has children", n.id)
			}
			return nil
		}
		if len(n.children) != t.Len() {
			return fmt.Errorf("gofuzz: Replay: array node %d has %d children, want %d", n.id, len(n.children), t.Len())
		}
		for i, c := range n.children {
			if err := rc.replayNode(v.Index(i), c); err != nil {
				return err
			}
		}
		return nil
	case NodeStruct:
		if t.Kind() != reflect.Struct {
			return rc.mismatch(v, n)
		}
		ci := 0
		for i := 0; i < t.NumField(); i++ {
			field := t.Field(i)
			if field.PkgPath != "" { // unexported
				continue
			}
			if skipNameByRes(rc.skipRes, field.Name) {
				continue
			}
			if ci >= len(n.children) {
				return fmt.Errorf("gofuzz: Replay: struct node %d is missing child for field %s", n.id, field.Name)
			}
			if err := rc.replayNode(v.Field(i), n.children[ci]); err != nil {
				return err
			}
			ci++
		}
		if ci != len(n.children) {
			return fmt.Errorf("gofuzz: Replay: struct node %d has %d children, want %d", n.id, len(n.children), ci)
		}
		return nil
	}
	return fmt.Errorf("gofuzz: Replay: unknown node kind %d", n.kind)
}

func (rc *replayCtx) mismatch(v reflect.Value, n *TreeNode) error {
	return fmt.Errorf("gofuzz: Replay: node %d kind %s does not match type %s", n.id, n.kind, v.Type())
}

// Minimize shrinks the recorded object while keep reports that the failure
// condition of interest still holds. keep receives a fresh pointer of the
// same type as the original RecordTree destination, replayed independently
// from each candidate tree; keep may mutate or destroy that object without
// affecting the tree, other candidates, or the final result. Only a true
// return value accepts a candidate.
//
// The original record is checked first and that check counts against
// maxCalls. If the original does not satisfy keep, or maxCalls is less than
// one, Minimize returns an error. Candidates are enumerated in node
// preorder: slices and strings delete contiguous intervals (longest first,
// left to right among equal lengths; strings delete by rune), non-nil
// pointers become nil, true becomes false, and non-zero integers try zero
// and then successive halvings toward zero. Structural changes may go below
// the recorded NumElements minimum and may produce nil; the frozen policy
// only constrains the initial generation. After each accepted candidate,
// enumeration restarts from the new tree. Minimize stops when no candidate
// is accepted or the call budget is spent; it does not guarantee a global
// minimum. It returns the last accepted tree and the number of keep calls
// made. If keep returns an error, Minimize stops and returns the last
// accepted tree, the call count, and that error.
func (t *GenerationTree) Minimize(keep func(interface{}) (bool, error), maxCalls int) (*GenerationTree, int, error) {
	if t.rootType == nil || t.root == nil {
		return nil, 0, errors.New("gofuzz: Minimize: empty generation tree")
	}
	if keep == nil {
		return nil, 0, errors.New("gofuzz: Minimize needs a keep callback")
	}
	if maxCalls < 1 {
		return nil, 0, fmt.Errorf("gofuzz: Minimize: maxCalls %d is less than 1", maxCalls)
	}
	calls := 0
	check := func(tree *GenerationTree) (bool, error) {
		calls++
		obj := reflect.New(tree.rootType)
		if err := tree.Replay(obj.Interface()); err != nil {
			return false, err
		}
		return keep(obj.Interface())
	}
	ok, err := check(t)
	if err != nil {
		return nil, calls, err
	}
	if !ok {
		return nil, calls, errors.New("gofuzz: Minimize: the original record does not satisfy keep")
	}
	current := t
	for {
		accepted := false
		var cbErr error
		enumCandidates(current.root, func(repl *TreeNode) bool {
			if calls >= maxCalls {
				return true
			}
			cand := current.withRoot(repl)
			ok, err := check(cand)
			if err != nil {
				cbErr = err
				return true
			}
			if ok {
				current = cand
				accepted = true
				return true
			}
			return false
		})
		if cbErr != nil {
			return current, calls, cbErr
		}
		if !accepted {
			// No acceptable candidate, or the budget ran out.
			return current, calls, nil
		}
	}
}

func (t *GenerationTree) withRoot(root *TreeNode) *GenerationTree {
	cp := *t
	cp.root = root
	return &cp
}

// enumCandidates emits candidate replacements for the subtree rooted at n in
// node preorder: first the candidates that modify n itself, then the
// candidates of its children in order. emit returning true stops the
// enumeration.
func enumCandidates(n *TreeNode, emit func(*TreeNode) bool) bool {
	for _, repl := range selfCandidates(n) {
		if emit(repl) {
			return true
		}
	}
	for i, c := range n.children {
		if enumCandidates(c, func(childRepl *TreeNode) bool {
			cp := n.shallowCopy()
			cp.children = make([]*TreeNode, len(n.children))
			copy(cp.children, n.children)
			cp.children[i] = childRepl
			return emit(cp)
		}) {
			return true
		}
	}
	return false
}

// selfCandidates returns the candidate replacements that modify only n.
// Surviving child subtrees are reused as-is, keeping their identities,
// seeds, order and values.
func selfCandidates(n *TreeNode) []*TreeNode {
	switch n.kind {
	case NodeSlice:
		if n.nilFlag || len(n.children) == 0 {
			return nil
		}
		cnt := len(n.children)
		var out []*TreeNode
		for l := cnt; l >= 1; l-- {
			for start := 0; start+l <= cnt; start++ {
				repl := n.shallowCopy()
				repl.children = make([]*TreeNode, 0, cnt-l)
				repl.children = append(repl.children, n.children[:start]...)
				repl.children = append(repl.children, n.children[start+l:]...)
				repl.nilFlag = len(repl.children) == 0
				out = append(out, repl)
			}
		}
		return out
	case NodeString:
		runes := []rune(n.effectiveString())
		if len(runes) == 0 {
			return nil
		}
		var out []*TreeNode
		for l := len(runes); l >= 1; l-- {
			for start := 0; start+l <= len(runes); start++ {
				shrunk := make([]rune, 0, len(runes)-l)
				shrunk = append(shrunk, runes[:start]...)
				shrunk = append(shrunk, runes[start+l:]...)
				repl := n.shallowCopy()
				repl.hasOverride = true
				repl.strVal = string(shrunk)
				out = append(out, repl)
			}
		}
		return out
	case NodePtr:
		if n.nilFlag || len(n.children) == 0 {
			return nil
		}
		repl := n.shallowCopy()
		repl.children = nil
		repl.nilFlag = true
		return []*TreeNode{repl}
	case NodeBool:
		if !n.effectiveBool() {
			return nil
		}
		repl := n.shallowCopy()
		repl.hasOverride = true
		repl.boolVal = false
		return []*TreeNode{repl}
	case NodeInt:
		v := n.effectiveInt()
		if v == 0 {
			return nil
		}
		out := []*TreeNode{intLeafOverride(n, 0)}
		for v != 0 {
			v /= 2 // truncates toward zero, cannot overflow
			if v == 0 {
				break
			}
			out = append(out, intLeafOverride(n, v))
		}
		return out
	case NodeUint:
		v := n.effectiveUint()
		if v == 0 {
			return nil
		}
		out := []*TreeNode{uintLeafOverride(n, 0)}
		for v != 0 {
			v >>= 1
			if v == 0 {
				break
			}
			out = append(out, uintLeafOverride(n, v))
		}
		return out
	}
	return nil
}

func intLeafOverride(n *TreeNode, v int64) *TreeNode {
	repl := n.shallowCopy()
	repl.hasOverride = true
	repl.intVal = v
	return repl
}

func uintLeafOverride(n *TreeNode, v uint64) *TreeNode {
	repl := n.shallowCopy()
	repl.hasOverride = true
	repl.uintVal = v
	return repl
}

// treeTypeFingerprint builds a static type binding. Named types are pinned
// by package path and name; unnamed types are described structurally.
// Unnamed types cannot be recursive in Go, so this always terminates.
func treeTypeFingerprint(t reflect.Type) string {
	var sb strings.Builder
	writeTypeFingerprint(&sb, t)
	return sb.String()
}

func writeTypeFingerprint(sb *strings.Builder, t reflect.Type) {
	if t.Name() != "" {
		fmt.Fprintf(sb, "n(%s.%s)", t.PkgPath(), t.Name())
		return
	}
	switch t.Kind() {
	case reflect.Ptr:
		sb.WriteString("p(")
		writeTypeFingerprint(sb, t.Elem())
		sb.WriteString(")")
	case reflect.Slice:
		sb.WriteString("s(")
		writeTypeFingerprint(sb, t.Elem())
		sb.WriteString(")")
	case reflect.Array:
		fmt.Fprintf(sb, "a%d(", t.Len())
		writeTypeFingerprint(sb, t.Elem())
		sb.WriteString(")")
	case reflect.Struct:
		sb.WriteString("t{")
		for i := 0; i < t.NumField(); i++ {
			field := t.Field(i)
			sb.WriteString(field.Name)
			sb.WriteString(":")
			writeTypeFingerprint(sb, field.Type)
			sb.WriteString(";")
		}
		sb.WriteString("}")
	default:
		sb.WriteString(t.Kind().String())
	}
}

// MarshalBinary encodes the tree as a self-contained record holding the
// format version, the static type binding, the frozen generation policy and
// the generation tree itself. The encoding is canonical: the same tree
// always encodes to the same bytes, and decoding and re-encoding a valid
// record reproduces the input bytes.
func (t *GenerationTree) MarshalBinary() ([]byte, error) {
	if t.rootType == nil || t.root == nil {
		return nil, errors.New("gofuzz: MarshalBinary: empty generation tree")
	}
	buf := &bytes.Buffer{}
	buf.WriteString(treeMagic)
	writeTreeString(buf, t.typeBind)
	var fixed [8]byte
	binary.LittleEndian.PutUint64(fixed[:], math.Float64bits(t.nilChance))
	buf.Write(fixed[:])
	writeTreeUvarint(buf, uint64(t.minElements))
	writeTreeUvarint(buf, uint64(t.maxElements))
	writeTreeUvarint(buf, uint64(t.maxDepth))
	writeTreeUvarint(buf, uint64(len(t.skipPatterns)))
	for _, p := range t.skipPatterns {
		writeTreeString(buf, p)
	}
	writeTreeUvarint(buf, countTreeNodes(t.root))
	writeTreeNode(buf, t.root)
	return buf.Bytes(), nil
}

func countTreeNodes(n *TreeNode) uint64 {
	c := uint64(1)
	for _, child := range n.children {
		c += countTreeNodes(child)
	}
	return c
}

func writeTreeUvarint(buf *bytes.Buffer, v uint64) {
	var tmp [binary.MaxVarintLen64]byte
	n := binary.PutUvarint(tmp[:], v)
	buf.Write(tmp[:n])
}

func writeTreeVarint(buf *bytes.Buffer, v int64) {
	var tmp [binary.MaxVarintLen64]byte
	n := binary.PutVarint(tmp[:], v)
	buf.Write(tmp[:n])
}

func writeTreeString(buf *bytes.Buffer, s string) {
	writeTreeUvarint(buf, uint64(len(s)))
	buf.WriteString(s)
}

func writeTreeBoolByte(buf *bytes.Buffer, b bool) {
	if b {
		buf.WriteByte(1)
	} else {
		buf.WriteByte(0)
	}
}

func writeTreeNode(buf *bytes.Buffer, n *TreeNode) {
	writeTreeUvarint(buf, n.id)
	var seed [8]byte
	binary.LittleEndian.PutUint64(seed[:], uint64(n.seed))
	buf.Write(seed[:])
	buf.WriteByte(byte(n.kind))
	switch n.kind {
	case NodeZero:
		// no payload
	case NodeBool:
		var flags byte
		if n.hasOverride {
			flags |= 1
			if n.boolVal {
				flags |= 2
			}
		}
		buf.WriteByte(flags)
	case NodeInt:
		buf.WriteByte(n.bits)
		writeTreeBoolByte(buf, n.hasOverride)
		if n.hasOverride {
			writeTreeVarint(buf, n.intVal)
		}
	case NodeUint:
		buf.WriteByte(n.bits)
		writeTreeBoolByte(buf, n.hasOverride)
		if n.hasOverride {
			writeTreeUvarint(buf, n.uintVal)
		}
	case NodeString:
		writeTreeBoolByte(buf, n.hasOverride)
		if n.hasOverride {
			writeTreeString(buf, n.strVal)
		}
	case NodeStruct, NodeSlice, NodeArray, NodePtr:
		writeTreeBoolByte(buf, n.nilFlag)
		writeTreeUvarint(buf, uint64(len(n.children)))
	default:
		// Cannot happen for trees built by this package.
		panic(fmt.Sprintf("gofuzz: MarshalBinary: unknown node kind %d", n.kind))
	}
	for _, child := range n.children {
		writeTreeNode(buf, child)
	}
}

// DecodeGenerationTree decodes a record produced by MarshalBinary.
// prototype must be a non-nil pointer; only its type is used and it is never
// written to. Decoding consumes no randomness. The record is fully validated
// before the tree is published: unsupported versions, type mismatches,
// duplicate node IDs, inconsistent parent/child structure or node kinds,
// out-of-range leaf values, truncation and trailing data are all rejected.
func DecodeGenerationTree(data []byte, prototype interface{}) (*GenerationTree, error) {
	v := reflect.ValueOf(prototype)
	if v.Kind() != reflect.Ptr || v.IsNil() {
		return nil, errors.New("gofuzz: DecodeGenerationTree needs a non-nil pointer prototype")
	}
	t := v.Type().Elem()
	if len(data) > maxRecordBytes {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: record exceeds the limit of %d bytes", maxRecordBytes)
	}
	d := &treeDecoder{data: data}
	magic, err := d.readBytes(len(treeMagic))
	if err != nil {
		return nil, err
	}
	if string(magic) != treeMagic {
		return nil, errors.New("gofuzz: DecodeGenerationTree: unsupported record version")
	}
	bind, err := d.readString()
	if err != nil {
		return nil, err
	}
	if bind != treeTypeFingerprint(t) {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: record type %q does not match prototype type %s", bind, t)
	}
	nilChanceBits, err := d.readUint64()
	if err != nil {
		return nil, err
	}
	nilChance := math.Float64frombits(nilChanceBits)
	if nilChance < 0 || nilChance > 1 || math.IsNaN(nilChance) {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: invalid NilChance %v", nilChance)
	}
	minElements, err := d.readUvarint()
	if err != nil {
		return nil, err
	}
	maxElements, err := d.readUvarint()
	if err != nil {
		return nil, err
	}
	if minElements > maxElements || maxElements > maxRecordNodes {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: invalid NumElements [%d, %d]", minElements, maxElements)
	}
	maxDepth, err := d.readUvarint()
	if err != nil {
		return nil, err
	}
	if maxDepth < 1 || maxDepth > maxRecordDepth {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: invalid MaxDepth %d", maxDepth)
	}
	numPatterns, err := d.readUvarint()
	if err != nil {
		return nil, err
	}
	if numPatterns > 1024 {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: too many skip patterns: %d", numPatterns)
	}
	tree := &GenerationTree{
		rootType:    t,
		typeBind:    bind,
		nilChance:   nilChance,
		minElements: int(minElements),
		maxElements: int(maxElements),
		maxDepth:    int(maxDepth),
	}
	for i := uint64(0); i < numPatterns; i++ {
		p, err := d.readString()
		if err != nil {
			return nil, err
		}
		re, err := regexp.Compile(p)
		if err != nil {
			return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: invalid skip pattern %q: %v", p, err)
		}
		tree.skipPatterns = append(tree.skipPatterns, p)
		tree.skipRes = append(tree.skipRes, re)
	}
	nodeCount, err := d.readUvarint()
	if err != nil {
		return nil, err
	}
	if nodeCount < 1 || nodeCount > maxRecordNodes {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: invalid node count %d", nodeCount)
	}
	dc := &decodeCtx{
		d:       d,
		skipRes: tree.skipRes,
		ids:     map[uint64]struct{}{},
	}
	root, err := dc.readNode(t, 0)
	if err != nil {
		return nil, err
	}
	if dc.nodes != nodeCount {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: record declares %d nodes but contains %d", nodeCount, dc.nodes)
	}
	if d.off != len(d.data) {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: %d trailing bytes", len(d.data)-d.off)
	}
	tree.root = root
	return tree, nil
}

var errTreeTruncated = errors.New("gofuzz: DecodeGenerationTree: record is truncated")

type treeDecoder struct {
	data []byte
	off  int
}

func (d *treeDecoder) readBytes(n int) ([]byte, error) {
	if len(d.data)-d.off < n {
		return nil, errTreeTruncated
	}
	b := d.data[d.off : d.off+n]
	d.off += n
	return b, nil
}

func (d *treeDecoder) readByte() (byte, error) {
	if d.off >= len(d.data) {
		return 0, errTreeTruncated
	}
	b := d.data[d.off]
	d.off++
	return b, nil
}

func (d *treeDecoder) readUint64() (uint64, error) {
	b, err := d.readBytes(8)
	if err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint64(b), nil
}

func (d *treeDecoder) readUvarint() (uint64, error) {
	if d.off >= len(d.data) {
		return 0, errTreeTruncated
	}
	v, n := binary.Uvarint(d.data[d.off:])
	if n <= 0 {
		return 0, errTreeTruncated
	}
	d.off += n
	return v, nil
}

func (d *treeDecoder) readVarint() (int64, error) {
	if d.off >= len(d.data) {
		return 0, errTreeTruncated
	}
	v, n := binary.Varint(d.data[d.off:])
	if n <= 0 {
		return 0, errTreeTruncated
	}
	d.off += n
	return v, nil
}

func (d *treeDecoder) readString() (string, error) {
	n, err := d.readUvarint()
	if err != nil {
		return "", err
	}
	if n > uint64(len(d.data)-d.off) {
		return "", errTreeTruncated
	}
	s := string(d.data[d.off : d.off+int(n)])
	d.off += int(n)
	return s, nil
}

type decodeCtx struct {
	d       *treeDecoder
	skipRes []*regexp.Regexp
	ids     map[uint64]struct{}
	nodes   uint64
}

func (dc *decodeCtx) readNode(t reflect.Type, depth int) (*TreeNode, error) {
	if depth > maxRecordDepth {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: record exceeds the depth limit of %d", maxRecordDepth)
	}
	dc.nodes++
	if dc.nodes > maxRecordNodes {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: record exceeds the limit of %d nodes", maxRecordNodes)
	}
	id, err := dc.d.readUvarint()
	if err != nil {
		return nil, err
	}
	if _, dup := dc.ids[id]; dup {
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: duplicate node id %d", id)
	}
	dc.ids[id] = struct{}{}
	seed, err := dc.d.readUint64()
	if err != nil {
		return nil, err
	}
	kindByte, err := dc.d.readByte()
	if err != nil {
		return nil, err
	}
	kind := NodeKind(kindByte)
	n := &TreeNode{id: id, seed: int64(seed), kind: kind}
	if err := dc.checkKind(kind, t); err != nil {
		return nil, err
	}
	switch kind {
	case NodeZero:
		// no payload
	case NodeBool:
		flags, err := dc.d.readByte()
		if err != nil {
			return nil, err
		}
		if flags&^byte(3) != 0 {
			return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: node %d has invalid bool flags %d", id, flags)
		}
		n.hasOverride = flags&1 != 0
		n.boolVal = flags&2 != 0
	case NodeInt:
		bits, err := dc.d.readByte()
		if err != nil {
			return nil, err
		}
		if int(bits) != t.Bits() {
			return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: node %d has %d bits, want %d", id, bits, t.Bits())
		}
		n.bits = bits
		if err := dc.readOverrideFlag(n); err != nil {
			return nil, err
		}
		if n.hasOverride {
			val, err := dc.d.readVarint()
			if err != nil {
				return nil, err
			}
			if truncateIntBits(val, bits) != val {
				return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: node %d int override %d out of %d-bit range", id, val, bits)
			}
			n.intVal = val
		}
	case NodeUint:
		bits, err := dc.d.readByte()
		if err != nil {
			return nil, err
		}
		if int(bits) != t.Bits() {
			return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: node %d has %d bits, want %d", id, bits, t.Bits())
		}
		n.bits = bits
		if err := dc.readOverrideFlag(n); err != nil {
			return nil, err
		}
		if n.hasOverride {
			val, err := dc.d.readUvarint()
			if err != nil {
				return nil, err
			}
			if truncateUintBits(val, bits) != val {
				return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: node %d uint override %d out of %d-bit range", id, val, bits)
			}
			n.uintVal = val
		}
	case NodeString:
		if err := dc.readOverrideFlag(n); err != nil {
			return nil, err
		}
		if n.hasOverride {
			s, err := dc.d.readString()
			if err != nil {
				return nil, err
			}
			if !utf8.ValidString(s) {
				return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: node %d string override is not valid UTF-8", id)
			}
			n.strVal = s
		}
	case NodeStruct, NodeSlice, NodeArray, NodePtr:
		nilFlag, err := dc.readBoolByte()
		if err != nil {
			return nil, err
		}
		n.nilFlag = nilFlag
		childCount, err := dc.d.readUvarint()
		if err != nil {
			return nil, err
		}
		if childCount > maxRecordNodes {
			return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: node %d declares %d children", id, childCount)
		}
		var childTypes []reflect.Type
		switch kind {
		case NodeStruct:
			for _, i := range generatedFieldIndexes(t, dc.skipRes) {
				childTypes = append(childTypes, t.Field(i).Type)
			}
			if childCount != uint64(len(childTypes)) {
				return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: struct node %d has %d children, want %d", id, childCount, len(childTypes))
			}
		case NodeArray:
			if nilFlag {
				if childCount != 0 {
					return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: zeroed array node %d has %d children", id, childCount)
				}
			} else {
				if childCount != uint64(t.Len()) {
					return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: array node %d has %d children, want %d", id, childCount, t.Len())
				}
				for i := 0; i < t.Len(); i++ {
					childTypes = append(childTypes, t.Elem())
				}
			}
		case NodeSlice:
			if nilFlag && childCount != 0 {
				return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: nil slice node %d has %d children", id, childCount)
			}
			for i := uint64(0); i < childCount; i++ {
				childTypes = append(childTypes, t.Elem())
			}
		case NodePtr:
			if childCount > 1 {
				return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: ptr node %d has %d children", id, childCount)
			}
			if nilFlag != (childCount == 0) {
				return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: ptr node %d has inconsistent nil flag", id)
			}
			if childCount == 1 {
				childTypes = append(childTypes, t.Elem())
			}
		}
		for _, ct := range childTypes {
			child, err := dc.readNode(ct, depth+1)
			if err != nil {
				return nil, err
			}
			n.children = append(n.children, child)
		}
	default:
		return nil, fmt.Errorf("gofuzz: DecodeGenerationTree: node %d has unknown kind %d", id, kindByte)
	}
	return n, nil
}

func (dc *decodeCtx) checkKind(kind NodeKind, t reflect.Type) error {
	ok := false
	switch kind {
	case NodeZero:
		ok = true
	case NodeBool:
		ok = t.Kind() == reflect.Bool
	case NodeInt:
		switch t.Kind() {
		case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
			ok = true
		}
	case NodeUint:
		switch t.Kind() {
		case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
			ok = true
		}
	case NodeString:
		ok = t.Kind() == reflect.String
	case NodeStruct:
		ok = t.Kind() == reflect.Struct
	case NodeSlice:
		ok = t.Kind() == reflect.Slice
	case NodeArray:
		ok = t.Kind() == reflect.Array
	case NodePtr:
		ok = t.Kind() == reflect.Ptr
	}
	if !ok {
		return fmt.Errorf("gofuzz: DecodeGenerationTree: node kind %s does not match type %s", kind, t)
	}
	return nil
}

func (dc *decodeCtx) readBoolByte() (bool, error) {
	b, err := dc.d.readByte()
	if err != nil {
		return false, err
	}
	if b > 1 {
		return false, fmt.Errorf("gofuzz: DecodeGenerationTree: invalid flag byte %d", b)
	}
	return b == 1, nil
}

func (dc *decodeCtx) readOverrideFlag(n *TreeNode) error {
	has, err := dc.readBoolByte()
	if err != nil {
		return err
	}
	n.hasOverride = has
	return nil
}

// generatedFieldIndexes returns the indexes of the struct fields that
// participate in generation: exported and not skipped.
func generatedFieldIndexes(t reflect.Type, skipRes []*regexp.Regexp) []int {
	var idx []int
	for i := 0; i < t.NumField(); i++ {
		field := t.Field(i)
		if field.PkgPath != "" { // unexported
			continue
		}
		if skipNameByRes(skipRes, field.Name) {
			continue
		}
		idx = append(idx, i)
	}
	return idx
}
