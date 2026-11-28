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
	"encoding/binary"
	"fmt"
	"math"
	"reflect"
)

// This file defines the versioned binary record of a GenerationTree. The
// layout is part of the treeFormatVersion contract: it stores only static
// type bindings, the frozen policy and tree nodes (IDs, seeds, slots and
// explicit scalar overrides). It never contains machine addresses or live Go
// objects. All multi-byte integers are big-endian; the encoding is canonical
// in that one valid tree always serializes to the same bytes.
//
// Layout:
//
//	magic        4 bytes  "GFZT"
//	version      u16      must equal treeFormatVersion
//	boundType    sstring  fully qualified bound type name
//	config:
//	  nilChance          f64
//	  minElements        i32
//	  maxElements        i32
//	  maxDepth           i32
//	  allowUnexported    u8   (always 0 in produced records)
//	  skipPatternCount   u32, then that many sstrings
//	nodeCount    u32
//	rootID       u64
//	nodes        nodeCount node records, in preorder
//
// An sstring is a u32 byte length followed by UTF-8 bytes. A node record is:
//
//	id u64; kind u8; slot u32; seed u64(bits); typeName sstring; flags u8;
//	kind-specific scalar payload (leaves only); childCount u32; then that
//	many child node records, recursively.
//
// Flag bits: 1 nil, 2 leaf, 4 rune, 8 overridden, 16 depthCut.
var treeMagic = [4]byte{'G', 'F', 'Z', 'T'}

const (
	flagNil      = 1 << 0
	flagLeaf     = 1 << 1
	flagRune     = 1 << 2
	flagOverride = 1 << 3
	flagDepthCut = 1 << 4
)

// MarshalBinary encodes the tree into the versioned, self-describing binary
// record. The encoding is deterministic: a valid tree, including one decoded
// from a record, always produces identical bytes.
func (t *GenerationTree) MarshalBinary() ([]byte, error) {
	if t == nil || t.root == nil || t.boundType == nil {
		return nil, fmt.Errorf("%w: nothing to encode", ErrTreeCorrupt)
	}
	if err := t.validate(t.boundType); err != nil {
		return nil, err
	}
	enc := &treeEncoder{buf: make([]byte, 0, 256)}
	enc.bytes(treeMagic[:])
	enc.u16(t.version)
	enc.sstring(t.boundType.String())
	enc.f64(t.config.nilChance)
	enc.i32(int32(t.config.minElements))
	enc.i32(int32(t.config.maxElements))
	enc.i32(int32(t.config.maxDepth))
	if t.config.allowUnexportedFields {
		return nil, fmt.Errorf("%w: records cannot carry AllowUnexportedFields", ErrTreeCorrupt)
	}
	enc.u8(0)
	enc.u32(uint32(len(t.config.skipFieldPatterns)))
	for _, p := range t.config.skipFieldPatterns {
		enc.sstring(p)
	}
	count := countNodes(t.root)
	if count > maxTreeNodes {
		return nil, fmt.Errorf("%w: node limit %d exceeded", ErrTreeCorrupt, maxTreeNodes)
	}
	enc.u32(uint32(count))
	enc.u64(uint64(t.root.id))
	enc.writeNode(t.root)
	return enc.buf, nil
}

type treeEncoder struct {
	buf []byte
}

func (e *treeEncoder) bytes(b []byte) { e.buf = append(e.buf, b...) }
func (e *treeEncoder) u8(v uint8)     { e.buf = append(e.buf, v) }
func (e *treeEncoder) u16(v uint16) {
	var b [2]byte
	binary.BigEndian.PutUint16(b[:], v)
	e.buf = append(e.buf, b[:]...)
}
func (e *treeEncoder) u32(v uint32) {
	var b [4]byte
	binary.BigEndian.PutUint32(b[:], v)
	e.buf = append(e.buf, b[:]...)
}
func (e *treeEncoder) i32(v int32) { e.u32(uint32(v)) }
func (e *treeEncoder) f64(v float64) {
	var b [8]byte
	binary.BigEndian.PutUint64(b[:], math.Float64bits(v))
	e.buf = append(e.buf, b[:]...)
}
func (e *treeEncoder) u64(v uint64) {
	var b [8]byte
	binary.BigEndian.PutUint64(b[:], v)
	e.buf = append(e.buf, b[:]...)
}
func (e *treeEncoder) i64(v int64) { e.u64(uint64(v)) }
func (e *treeEncoder) sstring(s string) {
	e.u32(uint32(len(s)))
	e.buf = append(e.buf, s...)
}

func (e *treeEncoder) writeNode(n *GenerationNode) {
	e.u64(uint64(n.id))
	e.u8(uint8(n.kind))
	e.u32(n.slot)
	e.i64(n.seed)
	e.sstring(n.typeName)

	var flags uint8
	if n.nilNode {
		flags |= flagNil
	}
	if n.leaf {
		flags |= flagLeaf
	}
	if n.isRune {
		flags |= flagRune
	}
	if n.overridden {
		flags |= flagOverride
	}
	if n.depthCut {
		flags |= flagDepthCut
	}
	e.u8(flags)

	switch n.kind {
	case KindBool:
		e.u8(boolByte(n.boolVal))
		if n.overridden {
			e.u8(boolByte(n.overBool))
		}
	case KindInt:
		e.i64(n.intVal)
		if n.overridden {
			e.i64(n.overInt)
		}
		if n.isRune {
			e.i64(int64(n.runeVal))
		}
	case KindUint:
		e.u64(n.uintVal)
		if n.overridden {
			e.u64(n.overUint)
		}
	}

	e.u32(uint32(len(n.children)))
	for _, ch := range n.children {
		e.writeNode(ch)
	}
}

func boolByte(b bool) uint8 {
	if b {
		return 1
	}
	return 0
}

func countNodes(n *GenerationNode) int {
	c := 1
	for _, ch := range n.children {
		c += countNodes(ch)
	}
	return c
}

// DecodeGenerationTree parses a record and validates it against the type
// supplied by prototype before returning a usable tree. prototype must be a
// non-nil pointer; only its element type is used, and prototype itself is
// never modified. Decoding consumes no randomness.
//
// Truncated input, trailing bytes, unsupported versions, duplicate IDs,
// broken parent/child or kind relationships, out-of-range leaf values and
// policy violations are all reported as ErrTreeCorrupt and no tree is
// returned.
func DecodeGenerationTree(data []byte, prototype interface{}) (*GenerationTree, error) {
	if len(data) == 0 {
		return nil, fmt.Errorf("%w: empty record", ErrTreeCorrupt)
	}
	if len(data) > maxTreeBytes {
		return nil, fmt.Errorf("%w: record exceeds %d bytes", ErrTreeCorrupt, maxTreeBytes)
	}
	rv := reflect.ValueOf(prototype)
	if !rv.IsValid() || rv.Kind() != reflect.Ptr || rv.IsNil() {
		return nil, fmt.Errorf("%w: prototype must be a non-nil pointer", ErrTreeInvalidArgument)
	}
	boundType := rv.Type().Elem()

	dec := &treeDecoder{data: data}
	magic := dec.next(4)
	if dec.err != nil || string(magic) != string(treeMagic[:]) {
		return nil, fmt.Errorf("%w: bad magic", ErrTreeCorrupt)
	}
	version := dec.u16()
	if dec.err != nil {
		return nil, dec.err
	}
	if version != treeFormatVersion {
		return nil, fmt.Errorf("%w: unsupported version %d", ErrTreeCorrupt, version)
	}
	boundName := dec.sstring()
	if dec.err != nil {
		return nil, dec.err
	}
	if boundName != boundType.String() {
		return nil, fmt.Errorf("%w: record type %s does not match prototype %s", ErrTreeCorrupt, boundName, boundType.String())
	}

	cfg := frozenConfig{}
	cfg.nilChance = dec.f64()
	cfg.minElements = int(dec.i32())
	cfg.maxElements = int(dec.i32())
	cfg.maxDepth = int(dec.i32())
	allowUnexported := dec.u8()
	patternCount := dec.u32()
	if dec.err != nil {
		return nil, dec.err
	}
	if allowUnexported != 0 {
		return nil, fmt.Errorf("%w: AllowUnexportedFields records are unsupported", ErrTreeCorrupt)
	}
	if patternCount > maxTreeNodes {
		return nil, fmt.Errorf("%w: implausible skip pattern count", ErrTreeCorrupt)
	}
	cfg.skipFieldPatterns = make([]string, 0, patternCount)
	for i := uint32(0); i < patternCount; i++ {
		cfg.skipFieldPatterns = append(cfg.skipFieldPatterns, dec.sstring())
		if dec.err != nil {
			return nil, dec.err
		}
	}

	declaredCount := dec.u32()
	rootID := dec.u64()
	if dec.err != nil {
		return nil, dec.err
	}
	if declaredCount == 0 || declaredCount > maxTreeNodes {
		return nil, fmt.Errorf("%w: node count %d out of range", ErrTreeCorrupt, declaredCount)
	}

	ids := map[uint64]bool{}
	root, gotCount, err := dec.readNode(ids, 1)
	if err != nil {
		return nil, err
	}
	if gotCount != int(declaredCount) {
		return nil, fmt.Errorf("%w: declared %d nodes but found %d", ErrTreeCorrupt, declaredCount, gotCount)
	}
	if root.id != rootID {
		return nil, fmt.Errorf("%w: root id mismatch", ErrTreeCorrupt)
	}
	if root.kind != KindRoot && root.kind != kindFor(boundType) {
		return nil, fmt.Errorf("%w: root node has kind %s", ErrTreeCorrupt, root.kind)
	}
	if dec.pos != len(data) {
		return nil, fmt.Errorf("%w: %d trailing bytes", ErrTreeCorrupt, len(data)-dec.pos)
	}

	tree := &GenerationTree{
		version:   version,
		root:      root,
		config:    cfg,
		boundType: boundType,
		nextID:    uint64(gotCount) + 1,
	}
	if err := tree.validate(boundType); err != nil {
		return nil, err
	}
	return tree, nil
}

type treeDecoder struct {
	data []byte
	pos  int
	err  error
}

func (d *treeDecoder) next(n int) []byte {
	if d.err != nil {
		return nil
	}
	if n < 0 || d.pos+n > len(d.data) {
		d.err = fmt.Errorf("%w: truncated record", ErrTreeCorrupt)
		return nil
	}
	b := d.data[d.pos : d.pos+n]
	d.pos += n
	return b
}

func (d *treeDecoder) u8() uint8 {
	b := d.next(1)
	if d.err != nil {
		return 0
	}
	return b[0]
}
func (d *treeDecoder) u16() uint16 {
	b := d.next(2)
	if d.err != nil {
		return 0
	}
	return binary.BigEndian.Uint16(b)
}
func (d *treeDecoder) u32() uint32 {
	b := d.next(4)
	if d.err != nil {
		return 0
	}
	return binary.BigEndian.Uint32(b)
}
func (d *treeDecoder) i32() int32 { return int32(d.u32()) }
func (d *treeDecoder) u64() uint64 {
	b := d.next(8)
	if d.err != nil {
		return 0
	}
	return binary.BigEndian.Uint64(b)
}
func (d *treeDecoder) i64() int64   { return int64(d.u64()) }
func (d *treeDecoder) f64() float64 { return math.Float64frombits(d.u64()) }
func (d *treeDecoder) sstring() string {
	n := d.u32()
	if d.err != nil {
		return ""
	}
	if n > maxTreeBytes {
		d.err = fmt.Errorf("%w: string length out of range", ErrTreeCorrupt)
		return ""
	}
	b := d.next(int(n))
	if d.err != nil {
		return ""
	}
	return string(b)
}

func (d *treeDecoder) readNode(ids map[uint64]bool, depth int) (*GenerationNode, int, error) {
	if depth > maxTreeDepth {
		return nil, 0, fmt.Errorf("%w: depth limit %d exceeded", ErrTreeCorrupt, maxTreeDepth)
	}
	n := &GenerationNode{}
	n.id = d.u64()
	n.kind = NodeKind(d.u8())
	n.slot = d.u32()
	n.seed = d.i64()
	n.typeName = d.sstring()
	flags := d.u8()
	if d.err != nil {
		return nil, 0, d.err
	}
	if n.id == 0 || ids[n.id] {
		return nil, 0, fmt.Errorf("%w: duplicate or zero node id %d", ErrTreeCorrupt, n.id)
	}
	ids[n.id] = true
	if n.seed < 0 {
		return nil, 0, fmt.Errorf("%w: node %d seed out of range", ErrTreeCorrupt, n.id)
	}
	if n.kind > KindUint {
		return nil, 0, fmt.Errorf("%w: node %d unknown kind %d", ErrTreeCorrupt, n.id, n.kind)
	}
	n.nilNode = flags&flagNil != 0
	n.leaf = flags&flagLeaf != 0
	n.isRune = flags&flagRune != 0
	n.overridden = flags&flagOverride != 0
	n.depthCut = flags&flagDepthCut != 0

	switch n.kind {
	case KindBool:
		if !n.leaf {
			return nil, 0, fmt.Errorf("%w: bool node %d not marked leaf", ErrTreeCorrupt, n.id)
		}
		n.boolVal = d.u8() != 0
		if n.overridden {
			n.overBool = d.u8() != 0
		}
	case KindInt:
		if !n.leaf {
			return nil, 0, fmt.Errorf("%w: int node %d not marked leaf", ErrTreeCorrupt, n.id)
		}
		n.intVal = d.i64()
		if n.overridden {
			n.overInt = d.i64()
		} else {
			n.overInt = n.intVal
		}
		if n.isRune {
			n.runeVal = rune(d.i64())
		} else {
			n.runeVal = rune(n.intVal)
		}
	case KindUint:
		if !n.leaf {
			return nil, 0, fmt.Errorf("%w: uint node %d not marked leaf", ErrTreeCorrupt, n.id)
		}
		n.uintVal = d.u64()
		if n.overridden {
			n.overUint = d.u64()
		} else {
			n.overUint = n.uintVal
		}
	default:
		if n.leaf {
			return nil, 0, fmt.Errorf("%w: non-scalar node %d marked leaf", ErrTreeCorrupt, n.id)
		}
	}
	if d.err != nil {
		return nil, 0, d.err
	}

	childCount := d.u32()
	if d.err != nil {
		return nil, 0, d.err
	}
	if childCount > maxTreeNodes {
		return nil, 0, fmt.Errorf("%w: node %d child count out of range", ErrTreeCorrupt, n.id)
	}
	total := 1
	for i := uint32(0); i < childCount; i++ {
		child, used, err := d.readNode(ids, depth+1)
		if err != nil {
			return nil, 0, err
		}
		n.children = append(n.children, child)
		total += used
		if total > maxTreeNodes {
			return nil, 0, fmt.Errorf("%w: node limit %d exceeded", ErrTreeCorrupt, maxTreeNodes)
		}
	}
	return n, total, nil
}
