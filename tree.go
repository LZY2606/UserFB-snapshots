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
	"errors"
	"fmt"
	"math"
	"math/rand"
	"reflect"
	"regexp"
	"unicode/utf8"
)

// This file implements the editable "generation tree" mode.
//
// A generation tree is a recorded, replayable description of one random
// generation run. Instead of producing only the finished object, RecordTree
// records every structural decision (slice length, nil-ness, depth cut-off)
// together with a per-node random seed; Replay drives the same reflection
// dispatch and the same scalar generators used by the ordinary Fuzz path, so
// no object is ever serialized into the tree and no external randomness is
// needed to replay it.
//
// Seed derivation (stable across processes and library versions that keep
// treeFormatVersion unchanged):
//
//   - RecordTree draws exactly one value, Int63(), from the Fuzzer's random
//     source; that non-negative value is the root node's seed.
//   - Every non-root node gets its own independent rand.New(rand.NewSource(s))
//     stream, where the seed is a 63-bit value derived deterministically from
//     the parent node's seed and the node's stable slot:
//
//	s = ((parentSeed*1099511628211) XOR mix(slot)) XOR 1469598103934665603
//
//	with mix(slot) = (slot*11400714819323198485) XOR (slot<<7 | slot>>57),
//	all arithmetic modulo 2^64, and the high bit of s cleared afterwards.
//
// Slots are stable positions, never element indices: a struct field uses its
// field index, an array/slice element its element index, a pointed-to value
// uses slot 0, and a string's rune leaves use their rune index. Deleting
// slice or string elements therefore never renumbers or re-seeds the
// surviving nodes.

// treeFormatVersion is the version of both the seed derivation and the
// on-disk binary record layout (see tree_persist.go). Records carrying any
// other version are rejected.
const treeFormatVersion uint16 = 1

// Limits applied both when generating and when decoding records. They bound
// the memory a record can pin and the depth Replay has to walk.
const (
	maxTreeNodes = 200000
	maxTreeDepth = 500
	maxTreeBytes = 64 << 20 // 64 MiB encoded record
)

// Sentinel errors returned by the generation-tree API. Callers can compare
// against these with errors.Is; additional wrapping may add context.
var (
	// ErrTreeUnsupportedType is returned when a reachable, non-skipped field
	// uses a kind the tree mode does not support, or would be handled by a
	// custom fuzz function, a fuzz.Interface implementation or a default
	// fuzz function, or when AllowUnexportedFields is enabled.
	ErrTreeUnsupportedType = errors.New("gofuzz: generation tree does not support this type or configuration")

	// ErrTreeInvalidArgument is returned when an API argument is not a
	// non-nil pointer (for dst) or a non-nil value (for prototype).
	ErrTreeInvalidArgument = errors.New("gofuzz: invalid argument")

	// ErrTreeBudget is returned by Minimize when the call budget is less
	// than one, so the initial check cannot even be performed.
	ErrTreeBudget = errors.New("gofuzz: minimize budget must be at least 1")

	// ErrTreeConditionNotMet is returned by Minimize when the original
	// record itself does not satisfy the keep predicate.
	ErrTreeConditionNotMet = errors.New("gofuzz: original record does not satisfy keep condition")

	// ErrTreeCorrupt is returned by DecodeGenerationTree or Replay when a
	// record is malformed, truncated, inconsistent with its bound type or
	// outside the supported limits.
	ErrTreeCorrupt = errors.New("gofuzz: invalid generation tree record")
)

// NodeKind identifies the role of a GenerationNode.
type NodeKind uint8

const (
	KindRoot NodeKind = iota
	KindStruct
	KindArray
	KindSlice
	KindPointer
	KindString
	KindBool
	KindInt
	KindUint
)

func (k NodeKind) String() string {
	switch k {
	case KindRoot:
		return "root"
	case KindStruct:
		return "struct"
	case KindArray:
		return "array"
	case KindSlice:
		return "slice"
	case KindPointer:
		return "pointer"
	case KindString:
		return "string"
	case KindBool:
		return "bool"
	case KindInt:
		return "int"
	case KindUint:
		return "uint"
	default:
		return fmt.Sprintf("unknown(%d)", uint8(k))
	}
}

// GenerationNode is the read-only view of one node of a GenerationTree.
//
// Instances are snapshots: the methods return values (or copies of slices),
// never internal storage, and Minimize/Replay never mutate a node that has
// already been handed out. Reduced trees are built from fresh, detached node
// copies. The caller owns nothing through these views and cannot modify the
// tree through them.
type GenerationNode struct {
	id       uint64
	kind     NodeKind
	typeName string
	slot     uint32
	seed     int64
	children []*GenerationNode

	// Kind-specific state.
	nilNode    bool   // KindPointer/KindSlice: the value replays as nil.
	depthCut   bool   // nilness was forced by the MaxDepth cut-off.
	leaf       bool   // KindBool/KindInt/KindUint: value below is in effect.
	isRune     bool   // KindInt leaf that is a rune child of a KindString node.
	boolVal    bool   // KindBool (generated value; false when leaf is false).
	intVal     int64  // KindInt, or a rune leaf's rune.
	uintVal    uint64 // KindUint.
	runeVal    rune   // KindString's children: the generated rune.
	overridden bool   // leaf: value below replaced the generated one.
	overBool   bool
	overInt    int64
	overUint   uint64
}

// ID returns the node's stable identifier. IDs are unique within one tree;
// they survive structural reduction: keeping a node keeps its ID.
func (n *GenerationNode) ID() uint64 { return n.id }

// Kind returns the node kind.
func (n *GenerationNode) Kind() NodeKind { return n.kind }

// TypeName returns the fully qualified Go type name this node is bound to,
// e.g. "time.Duration" or "pkg.MyStruct".
func (n *GenerationNode) TypeName() string { return n.typeName }

// Slot returns the stable slot of this node within its parent (field index,
// element index, rune index, or 0 for a pointed-to value).
func (n *GenerationNode) Slot() uint32 { return n.slot }

// Seed returns the node's derived 63-bit generation seed.
func (n *GenerationNode) Seed() int64 { return n.seed }

// Children returns a fresh slice of the node's children in their stable
// order (struct field order, element order, or rune order).
func (n *GenerationNode) Children() []*GenerationNode {
	out := make([]*GenerationNode, len(n.children))
	copy(out, n.children)
	return out
}

// IsNil reports whether a KindPointer node currently replays as nil.
func (n *GenerationNode) IsNil() bool { return n.kind == KindPointer && n.nilNode }

// IsLeaf reports whether a KindBool, KindInt or KindUint node carries an
// explicit (generated or minimized) scalar value rather than children.
func (n *GenerationNode) IsLeaf() bool { return n.leaf }

// BoolValue returns the value that a KindBool leaf replays as.
func (n *GenerationNode) BoolValue() bool {
	if n.kind != KindBool {
		return false
	}
	if n.overridden {
		return n.overBool
	}
	return n.boolVal
}

// IntValue returns the value that a KindInt leaf replays as.
func (n *GenerationNode) IntValue() int64 {
	if n.kind != KindInt {
		return 0
	}
	if n.overridden {
		return n.overInt
	}
	return n.intVal
}

// UintValue returns the value that a KindUint leaf replays as.
func (n *GenerationNode) UintValue() uint64 {
	if n.kind != KindUint {
		return 0
	}
	if n.overridden {
		return n.overUint
	}
	return n.uintVal
}

// RuneValue returns the rune that a rune leaf (child of KindString) replays.
func (n *GenerationNode) RuneValue() rune { return n.runeVal }

// IsOverridden reports whether the leaf value was changed by Minimize rather
// than produced by the original generation.
func (n *GenerationNode) IsOverridden() bool { return n.overridden }

// GenerationTree is an immutable, replayable record of one generation run.
//
// Trees are snapshots. Replay never mutates the tree; Minimize returns a new
// tree and leaves the receiver untouched. The root is accessible via Root.
type GenerationTree struct {
	version   uint16
	root      *GenerationNode
	config    frozenConfig
	boundType reflect.Type // static type the root is bound to.
	nextID    uint64       // only used while building; copied trees freeze this.
}

// frozenConfig is the policy snapshot taken inside RecordTree. It is part of
// the persistent record.
type frozenConfig struct {
	nilChance             float64
	minElements           int
	maxElements           int
	maxDepth              int
	allowUnexportedFields bool
	skipFieldPatterns     []string
}

// Root returns the read-only root node.
func (t *GenerationTree) Root() *GenerationNode { return t.root }

// Version returns the record format version.
func (t *GenerationTree) Version() uint16 { return t.version }

// RecordTree records the generation of a fresh value of the same type as
// *dst without writing to dst. dst must be a non-nil pointer.
//
// The reachable, non-skipped fields must only use bool, signed/unsigned
// integer types (including named types), string, and structs, arrays,
// slices and pointers built from them. Unsupported kinds (map, interface,
// float, complex, chan, func, unsafe.Pointer), custom/default fuzz
// functions, fuzz.Interface implementations and AllowUnexportedFields cause
// an ErrTreeUnsupportedType error carrying the offending field path; in
// that case no randomness is consumed from the Fuzzer.
func (f *Fuzzer) RecordTree(dst interface{}) (*GenerationTree, error) {
	f.fuzzLock.Lock()
	defer f.fuzzLock.Unlock()

	rv := reflect.ValueOf(dst)
	if !rv.IsValid() || rv.Kind() != reflect.Ptr || rv.IsNil() {
		return nil, fmt.Errorf("%w: dst must be a non-nil pointer", ErrTreeInvalidArgument)
	}
	targetType := rv.Type().Elem()

	cfg := frozenConfig{
		nilChance:             f.nilChance,
		minElements:           f.minElements,
		maxElements:           f.maxElements,
		maxDepth:              f.maxDepth,
		allowUnexportedFields: f.allowUnexportedFields,
		skipFieldPatterns:     make([]string, len(f.skipFieldPatterns)),
	}
	for i, p := range f.skipFieldPatterns {
		cfg.skipFieldPatterns[i] = p.String()
	}

	// Pre-flight: validate the whole reachable type and configuration before
	// the single random draw, so a rejected record consumes nothing.
	if err := validateTreeType(targetType, f); err != nil {
		return nil, err
	}

	rootSeed := f.r.Int63() // the one and only random draw for this record.
	if rootSeed < 0 {
		rootSeed &= 1<<63 - 1 // Int63 is already non-negative; keep the invariant explicit.
	}

	rec := &treeRecorder{
		cfg:    cfg,
		nextID: 1,
	}
	// Composite roots keep the dedicated KindRoot; scalar and string roots
	// carry their real kind so leaf payloads and reduction edits are
	// self-describing in the record.
	rootKind := KindRoot
	if k := kindFor(targetType); k == KindBool || k == KindInt || k == KindUint || k == KindString {
		rootKind = k
	}
	root := rec.newNode(rootKind, targetType, 0, rootSeed)
	rec.gen(targetType, root, 1) // depth 1 mirrors doFuzz's curDepth accounting.
	if rec.err != nil {
		return nil, rec.err
	}
	return &GenerationTree{
		version:   treeFormatVersion,
		root:      root,
		config:    cfg,
		boundType: targetType,
		nextID:    rec.nextID,
	}, nil
}

// treeRecorder carries the mutable state of one RecordTree generation.
type treeRecorder struct {
	cfg    frozenConfig
	nextID uint64
	nodes  int
	err    error
}

func (rec *treeRecorder) newNode(kind NodeKind, t reflect.Type, slot uint32, seed int64) *GenerationNode {
	rec.nodes++
	if rec.nodes > maxTreeNodes {
		rec.err = fmt.Errorf("%w: node limit %d exceeded", ErrTreeCorrupt, maxTreeNodes)
	}
	n := &GenerationNode{
		id:       rec.nextID,
		kind:     kind,
		typeName: t.String(),
		slot:     slot,
		seed:     seed,
	}
	rec.nextID++
	return n
}

// childSeed derives the seed of a child node from a parent seed and a stable
// slot. See the file-level comment for the versioned derivation.
func childSeed(parentSeed int64, slot uint32) int64 {
	const (
		prime1 = 1099511628211
		prime2 = 11400714819323198485
		offset = 1469598103934665603
	)
	s := uint64(slot)
	mix := s*prime2 ^ (s<<7 | s>>57)
	derived := (uint64(parentSeed)*prime1 ^ mix) ^ offset
	return int64(derived &^ (uint64(1) << 63))
}

func (rec *treeRecorder) child(parent *GenerationNode, kind NodeKind, t reflect.Type, slot uint32) *GenerationNode {
	return rec.newNode(kind, t, slot, childSeed(parent.seed, slot))
}

// gen populates node n so that it describes how to generate a value of type
// t. depth mirrors fuzzerContext.doFuzz's curDepth (1-based).
func (rec *treeRecorder) gen(t reflect.Type, n *GenerationNode, depth int) {
	if rec.err != nil {
		return
	}
	if depth > maxTreeDepth {
		rec.err = fmt.Errorf("%w: depth limit %d exceeded", ErrTreeCorrupt, maxTreeDepth)
		return
	}
	switch t.Kind() {
	case reflect.Bool:
		n.leaf = true
		n.boolVal = randBool(rand.New(rand.NewSource(n.seed)))
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		n.leaf = true
		// Generate with the shared scalar routine, then narrow to the
		// concrete (possibly named) type exactly like SetInt.
		n.intVal = narrowInt(t, int64(randUint64(rand.New(rand.NewSource(n.seed)))))
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		n.leaf = true
		n.uintVal = narrowUint(t, randUint64(rand.New(rand.NewSource(n.seed))))
	case reflect.String:
		rec.genString(t, n)
	case reflect.Struct:
		for i := 0; i < t.NumField(); i++ {
			sf := t.Field(i)
			if rec.isSkipped(sf.Name) || !sf.IsExported() {
				// Skipped and unexported fields are left at zero, exactly as
				// the ordinary Fuzz path does with AllowUnexportedFields off.
				continue
			}
			child := rec.child(n, kindFor(sf.Type), sf.Type, uint32(i))
			if rec.err != nil {
				return
			}
			n.children = append(n.children, child)
			rec.gen(sf.Type, child, depth+1)
		}
	case reflect.Array:
		for i := 0; i < t.Len(); i++ {
			child := rec.child(n, kindFor(t.Elem()), t.Elem(), uint32(i))
			if rec.err != nil {
				return
			}
			n.children = append(n.children, child)
			rec.gen(t.Elem(), child, depth+1)
		}
	case reflect.Slice:
		rec.genSlice(t, n, depth)
	case reflect.Ptr:
		r := rand.New(rand.NewSource(n.seed))
		if r.Float64() < rec.cfg.nilChance || depth >= rec.cfg.maxDepth {
			n.nilNode = true
			n.depthCut = depth >= rec.cfg.maxDepth
			return
		}
		child := rec.child(n, kindFor(t.Elem()), t.Elem(), 0)
		if rec.err != nil {
			return
		}
		n.children = append(n.children, child)
		rec.gen(t.Elem(), child, depth+1)
	default:
		rec.err = fmt.Errorf("%w: kind %s at %s", ErrTreeUnsupportedType, t.Kind(), t.String())
	}
}

// narrowInt truncates a generated int64 to the representable range of t,
// matching reflect.Value.SetInt's two's-complement narrowing.
func narrowInt(t reflect.Type, x int64) int64 {
	if t.Bits() == 64 {
		return x
	}
	shift := uint(64 - t.Bits())
	return (x << shift) >> shift
}

// narrowUint truncates a generated uint64 to the width of t.
func narrowUint(t reflect.Type, x uint64) uint64 {
	if t.Bits() == 64 {
		return x
	}
	return x & (uint64(1)<<uint(t.Bits()) - 1)
}

func (rec *treeRecorder) genString(t reflect.Type, n *GenerationNode) {
	r := rand.New(rand.NewSource(n.seed))
	s := randString(r)
	runes := []rune(s)
	for i, ru := range runes {
		child := rec.newNode(KindInt, reflect.TypeOf(rune(0)), uint32(i), childSeed(n.seed, uint32(i)))
		if rec.err != nil {
			return
		}
		child.leaf = true
		child.isRune = true
		child.runeVal = ru
		child.intVal = int64(ru)
		n.children = append(n.children, child)
	}
}

func (rec *treeRecorder) genSlice(t reflect.Type, n *GenerationNode, depth int) {
	r := rand.New(rand.NewSource(n.seed))
	if r.Float64() < rec.cfg.nilChance || depth >= rec.cfg.maxDepth {
		n.nilNode = true
		n.depthCut = depth >= rec.cfg.maxDepth
		return
	}
	count := rec.cfg.minElements
	if rec.cfg.maxElements > rec.cfg.minElements {
		count += r.Intn(rec.cfg.maxElements - rec.cfg.minElements + 1)
	}
	for i := 0; i < count; i++ {
		child := rec.child(n, kindFor(t.Elem()), t.Elem(), uint32(i))
		if rec.err != nil {
			return
		}
		n.children = append(n.children, child)
		rec.gen(t.Elem(), child, depth+1)
	}
}

func (rec *treeRecorder) isSkipped(fieldName string) bool {
	for _, p := range rec.cfg.skipFieldPatterns {
		// Patterns are pre-validated regular expressions from the Fuzzer.
		re, err := regexp.Compile(p)
		if err != nil {
			continue
		}
		if re.MatchString(fieldName) {
			return true
		}
	}
	return false
}

// kindFor maps a reflect.Kind to the node kind used by the tree mode.
func kindFor(t reflect.Type) NodeKind {
	switch t.Kind() {
	case reflect.Struct:
		return KindStruct
	case reflect.Array:
		return KindArray
	case reflect.Slice:
		return KindSlice
	case reflect.Ptr:
		return KindPointer
	case reflect.String:
		return KindString
	case reflect.Bool:
		return KindBool
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		return KindInt
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		return KindUint
	default:
		return KindRoot // unreachable for validated types
	}
}

// validateTreeType walks every reachable, non-skipped field and returns an
// ErrTreeUnsupportedType with the field path if anything is outside the
// supported subset or any handler would be invoked.
func validateTreeType(t reflect.Type, f *Fuzzer) error {
	if f.allowUnexportedFields {
		return fmt.Errorf("%w: AllowUnexportedFields is enabled", ErrTreeUnsupportedType)
	}
	seen := map[reflect.Type]bool{}
	return validateType(t, f, seen, "")
}

func validateType(t reflect.Type, f *Fuzzer, seen map[reflect.Type]bool, path string) error {
	if err := checkNoHandlers(t, f, path); err != nil {
		return err
	}
	switch t.Kind() {
	case reflect.Bool,
		reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64,
		reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr,
		reflect.String:
		return nil
	case reflect.Struct:
		if seen[t] {
			return nil
		}
		seen[t] = true
		for i := 0; i < t.NumField(); i++ {
			sf := t.Field(i)
			childPath := path + "." + sf.Name
			if fieldSkipped(f, sf.Name) {
				continue
			}
			if !sf.IsExported() {
				// Unexported fields are left zero, exactly as in the
				// ordinary Fuzz path when AllowUnexportedFields is off.
				continue
			}
			if err := validateType(sf.Type, f, seen, childPath); err != nil {
				return err
			}
		}
		return nil
	case reflect.Array, reflect.Slice:
		return validateType(t.Elem(), f, seen, path+"[]")
	case reflect.Ptr:
		return validateType(t.Elem(), f, seen, path+"*")
	default:
		// map, interface, float, complex, chan, func, unsafe.Pointer.
		return fmt.Errorf("%w: unsupported kind %s at %s", ErrTreeUnsupportedType, t.Kind(), path)
	}
}

func fieldSkipped(f *Fuzzer, name string) bool {
	for _, p := range f.skipFieldPatterns {
		if p.MatchString(name) {
			return true
		}
	}
	return false
}

// checkNoHandlers rejects any type that the ordinary dispatch would route to
// a custom fuzz function, a default fuzz function or a fuzz.Interface
// implementation. Both value and pointer forms are considered, mirroring
// fuzzerContext.tryCustom.
func checkNoHandlers(t reflect.Type, f *Fuzzer, path string) error {
	interfaceType := reflect.TypeOf((*Interface)(nil)).Elem()
	candidates := []reflect.Type{t}
	if t.Kind() != reflect.Ptr {
		candidates = append(candidates, reflect.PtrTo(t))
	}
	for _, ct := range candidates {
		if _, ok := f.fuzzFuncs[ct]; ok {
			return fmt.Errorf("%w: custom fuzz function registered for %s at %s", ErrTreeUnsupportedType, ct.String(), path)
		}
		if _, ok := f.defaultFuzzFuncs[ct]; ok {
			return fmt.Errorf("%w: default fuzz function registered for %s at %s", ErrTreeUnsupportedType, ct.String(), path)
		}
		if ct.Implements(interfaceType) {
			return fmt.Errorf("%w: %s implements fuzz.Interface at %s", ErrTreeUnsupportedType, ct.String(), path)
		}
	}
	return nil
}

// Replay regenerates the recorded value into a fresh zero value of the bound
// type and, on success, stores it into *dst. dst must be a non-nil pointer of
// the exact type used when recording (or when decoding with a prototype). No
// external random source is used: each node generates from its own seed.
//
// If the record is invalid or dst's element type does not match the bound
// type, dst is left untouched and an error is returned.
func (t *GenerationTree) Replay(dst interface{}) error {
	rv := reflect.ValueOf(dst)
	if !rv.IsValid() || rv.Kind() != reflect.Ptr || rv.IsNil() {
		return fmt.Errorf("%w: dst must be a non-nil pointer", ErrTreeInvalidArgument)
	}
	elemType := rv.Type().Elem()
	if t.boundType == nil || elemType != t.boundType {
		return fmt.Errorf("%w: type mismatch: dst %s, record %s", ErrTreeCorrupt, elemType.String(), t.root.typeName)
	}
	if err := t.validate(t.boundType); err != nil {
		return err
	}

	fresh := reflect.New(elemType).Elem()
	rep := &treeReplayer{}
	if err := rep.replay(elemType, t.root, fresh, 1); err != nil {
		return err
	}
	rv.Elem().Set(fresh)
	return nil
}

type treeReplayer struct{}

func (rep *treeReplayer) replay(t reflect.Type, n *GenerationNode, v reflect.Value, depth int) error {
	if n.typeName != t.String() {
		return fmt.Errorf("%w: node %d type %s does not match %s", ErrTreeCorrupt, n.id, n.typeName, t.String())
	}
	switch t.Kind() {
	case reflect.Bool:
		v.SetBool(n.BoolValue())
	case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
		v.SetInt(n.IntValue())
	case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
		v.SetUint(n.UintValue())
	case reflect.String:
		var b []byte
		for _, ch := range n.children {
			b = append(b, []byte(string(ch.runeVal))...)
		}
		v.SetString(string(b))
	case reflect.Struct:
		for _, ch := range n.children {
			sf := t.Field(int(ch.slot))
			if err := rep.replay(sf.Type, ch, v.Field(int(ch.slot)), depth+1); err != nil {
				return err
			}
		}
	case reflect.Array:
		for _, ch := range n.children {
			if err := rep.replay(t.Elem(), ch, v.Index(int(ch.slot)), depth+1); err != nil {
				return err
			}
		}
	case reflect.Slice:
		if n.nilNode {
			v.Set(reflect.Zero(t))
			return nil
		}
		// Children keep their original slots and order; after reduction
		// their slots may be sparse, so positions here are positional.
		s := reflect.MakeSlice(t, len(n.children), len(n.children))
		for i, ch := range n.children {
			if err := rep.replay(t.Elem(), ch, s.Index(i), depth+1); err != nil {
				return err
			}
		}
		v.Set(s)
	case reflect.Ptr:
		if n.nilNode || len(n.children) == 0 {
			v.Set(reflect.Zero(t))
			return nil
		}
		p := reflect.New(t.Elem())
		if err := rep.replay(t.Elem(), n.children[0], p.Elem(), depth+1); err != nil {
			return err
		}
		v.Set(p)
	default:
		return fmt.Errorf("%w: unsupported kind %s at %s", ErrTreeCorrupt, t.Kind(), t.String())
	}
	return nil
}

// validate re-checks a decoded or reduced tree against its bound type before
// it can produce a value.
func (t *GenerationTree) validate(boundType reflect.Type) error {
	if t.version != treeFormatVersion {
		return fmt.Errorf("%w: unsupported version %d", ErrTreeCorrupt, t.version)
	}
	if t.root == nil {
		return fmt.Errorf("%w: missing root", ErrTreeCorrupt)
	}
	ids := map[uint64]bool{}
	count := 0
	var walk func(n *GenerationNode, typ reflect.Type, parent *GenerationNode, parentType reflect.Type, depth int) error
	walk = func(n *GenerationNode, typ reflect.Type, parent *GenerationNode, parentType reflect.Type, depth int) error {
		if n == nil {
			return fmt.Errorf("%w: nil node", ErrTreeCorrupt)
		}
		if depth > maxTreeDepth {
			return fmt.Errorf("%w: depth limit %d exceeded", ErrTreeCorrupt, maxTreeDepth)
		}
		count++
		if count > maxTreeNodes {
			return fmt.Errorf("%w: node limit %d exceeded", ErrTreeCorrupt, maxTreeNodes)
		}
		if n.id == 0 || ids[n.id] {
			return fmt.Errorf("%w: duplicate or zero node id %d", ErrTreeCorrupt, n.id)
		}
		ids[n.id] = true
		if n.typeName != typ.String() {
			return fmt.Errorf("%w: node %d type %s does not match %s", ErrTreeCorrupt, n.id, n.typeName, typ.String())
		}
		if n.kind != kindFor(typ) && !(parent == nil && n.kind == KindRoot) {
			return fmt.Errorf("%w: node %d kind %s does not match %s", ErrTreeCorrupt, n.id, n.kind, typ.Kind())
		}
		switch typ.Kind() {
		case reflect.Bool:
			if !n.leaf || len(n.children) != 0 {
				return fmt.Errorf("%w: malformed bool node %d", ErrTreeCorrupt, n.id)
			}
		case reflect.Int, reflect.Int8, reflect.Int16, reflect.Int32, reflect.Int64:
			if !n.leaf || len(n.children) != 0 {
				return fmt.Errorf("%w: malformed int node %d", ErrTreeCorrupt, n.id)
			}
			// A rune marker is only legal directly beneath a string node
			// (whose element Go type is int32), including a string root.
			if n.isRune && (parentType == nil || parentType.Kind() != reflect.String) {
				return fmt.Errorf("%w: rune marker on non-string int node %d", ErrTreeCorrupt, n.id)
			}
			if err := checkIntRange(typ, n.IntValue()); err != nil {
				return err
			}
		case reflect.Uint, reflect.Uint8, reflect.Uint16, reflect.Uint32, reflect.Uint64, reflect.Uintptr:
			if !n.leaf || len(n.children) != 0 {
				return fmt.Errorf("%w: malformed uint node %d", ErrTreeCorrupt, n.id)
			}
			if err := checkUintRange(typ, n.UintValue()); err != nil {
				return err
			}
		case reflect.String:
			if n.leaf || n.nilNode {
				return fmt.Errorf("%w: malformed string node %d", ErrTreeCorrupt, n.id)
			}
			// Rune leaves keep stable, non-reordered slots; after reduction
			// slots may be sparse but must stay unique and increasing.
			if err := checkOrderedSlots(n); err != nil {
				return err
			}
			for _, ch := range n.children {
				if !ch.leaf || !ch.isRune || ch.runeVal != rune(ch.intVal) || !utf8.ValidRune(ch.runeVal) {
					return fmt.Errorf("%w: malformed rune node %d", ErrTreeCorrupt, n.id)
				}
				if err := walk(ch, reflect.TypeOf(rune(0)), n, typ, depth+1); err != nil {
					return err
				}
			}
		case reflect.Struct:
			if n.leaf || n.nilNode {
				return fmt.Errorf("%w: malformed struct node %d", ErrTreeCorrupt, n.id)
			}
			slots := map[uint32]bool{}
			for _, ch := range n.children {
				if int(ch.slot) >= typ.NumField() || slots[ch.slot] {
					return fmt.Errorf("%w: struct node %d bad field slot %d", ErrTreeCorrupt, n.id, ch.slot)
				}
				slots[ch.slot] = true
				sf := typ.Field(int(ch.slot))
				if !sf.IsExported() || t.fieldIsSkipped(sf.Name) {
					return fmt.Errorf("%w: struct node %d references unexported or skipped field %s", ErrTreeCorrupt, n.id, sf.Name)
				}
				if err := walk(ch, sf.Type, n, typ, depth+1); err != nil {
					return err
				}
			}
		case reflect.Array:
			if n.leaf || n.nilNode || len(n.children) != typ.Len() {
				return fmt.Errorf("%w: malformed array node %d", ErrTreeCorrupt, n.id)
			}
			if err := checkDenseSlots(n); err != nil {
				return err
			}
			for _, ch := range n.children {
				if err := walk(ch, typ.Elem(), n, typ, depth+1); err != nil {
					return err
				}
			}
		case reflect.Slice:
			if n.leaf {
				return fmt.Errorf("%w: malformed slice node %d", ErrTreeCorrupt, n.id)
			}
			if n.nilNode && len(n.children) != 0 {
				return fmt.Errorf("%w: nil slice node %d has children", ErrTreeCorrupt, n.id)
			}
			// Element slots are stable identities: surviving elements keep
			// their original slot, so sparse slots are legal after deletion.
			if err := checkOrderedSlots(n); err != nil {
				return err
			}
			for _, ch := range n.children {
				if err := walk(ch, typ.Elem(), n, typ, depth+1); err != nil {
					return err
				}
			}
		case reflect.Ptr:
			if n.leaf {
				return fmt.Errorf("%w: malformed pointer node %d", ErrTreeCorrupt, n.id)
			}
			if n.nilNode {
				if len(n.children) != 0 {
					return fmt.Errorf("%w: nil pointer node %d has children", ErrTreeCorrupt, n.id)
				}
			} else {
				if len(n.children) != 1 || n.children[0].slot != 0 {
					return fmt.Errorf("%w: pointer node %d must have one slot-0 child", ErrTreeCorrupt, n.id)
				}
				if err := walk(n.children[0], typ.Elem(), n, typ, depth+1); err != nil {
					return err
				}
			}
		default:
			return fmt.Errorf("%w: unsupported kind %s", ErrTreeCorrupt, typ.Kind())
		}
		return nil
	}
	if err := walk(t.root, boundType, nil, nil, 1); err != nil {
		return err
	}
	return t.validateConfig()
}

// checkDenseSlots verifies children slots are exactly 0..len-1 in order.
func checkDenseSlots(n *GenerationNode) error {
	for i, ch := range n.children {
		if ch.slot != uint32(i) {
			return fmt.Errorf("%w: node %d expected dense slot %d, got %d", ErrTreeCorrupt, n.id, i, ch.slot)
		}
	}
	return nil
}

// checkOrderedSlots verifies child slots are unique and strictly increasing,
// which allows sparse (post-deletion) but never reordered slots.
func checkOrderedSlots(n *GenerationNode) error {
	var prev uint32
	for i, ch := range n.children {
		if i > 0 && ch.slot <= prev {
			return fmt.Errorf("%w: node %d child slots not strictly increasing", ErrTreeCorrupt, n.id)
		}
		prev = ch.slot
	}
	return nil
}

func (t *GenerationTree) validateConfig() error {
	c := t.config
	if math.IsNaN(c.nilChance) || math.IsInf(c.nilChance, 0) || c.nilChance < 0 || c.nilChance > 1 {
		return fmt.Errorf("%w: nilChance out of range", ErrTreeCorrupt)
	}
	if c.minElements < 0 || c.maxElements < c.minElements || c.maxDepth < 0 {
		return fmt.Errorf("%w: element/depth policy out of range", ErrTreeCorrupt)
	}
	for _, p := range c.skipFieldPatterns {
		if _, err := regexp.Compile(p); err != nil {
			return fmt.Errorf("%w: invalid skip pattern %q: %v", ErrTreeCorrupt, p, err)
		}
	}
	return nil
}

// fieldIsSkipped reports whether name matches one of the frozen skip
// patterns. Patterns are known to compile because validateConfig ran first.
func (t *GenerationTree) fieldIsSkipped(name string) bool {
	for _, p := range t.config.skipFieldPatterns {
		if re, err := regexp.Compile(p); err == nil && re.MatchString(name) {
			return true
		}
	}
	return false
}

func checkIntRange(t reflect.Type, v int64) error {
	if t.Bits() == 64 {
		return nil // every int64 is representable.
	}
	limit := int64(1) << uint(t.Bits()-1)
	if v < -limit || v >= limit {
		return fmt.Errorf("%w: int value %d out of range for %s", ErrTreeCorrupt, v, t.String())
	}
	return nil
}

func checkUintRange(t reflect.Type, v uint64) error {
	bits := t.Bits()
	if bits < 64 && v >= uint64(1)<<uint(bits) {
		return fmt.Errorf("%w: uint value %d out of range for %s", ErrTreeCorrupt, v, t.String())
	}
	return nil
}
