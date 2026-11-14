// Copyright 2026 The go-interpreter Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package wasm

import (
	"bytes"
	"fmt"
	"io"

	"github.com/go-interpreter/wagon/wasm/leb128"
)

const (
	refNullOpcode byte = 0xd0
	refFuncOpcode byte = 0xd2
)

// InvalidElementFlagsError reports a flags byte that is not supported by the
// reference-types proposal (the valid range is 0 through 7).
type InvalidElementFlagsError uint8

func (e InvalidElementFlagsError) Error() string {
	return fmt.Sprintf("wasm: invalid element segment flags: %d", uint8(e))
}

// InvalidElementKindError reports an elemkind byte other than 0x00.
type InvalidElementKindError uint8

func (e InvalidElementKindError) Error() string {
	return fmt.Sprintf("wasm: invalid element kind: %#x", uint8(e))
}

// InvalidElementExprError reports a malformed element initializer expression.
type InvalidElementExprError struct {
	Reason string
}

func (e InvalidElementExprError) Error() string {
	return "wasm: invalid element expression: " + e.Reason
}

// ElementMode describes how the elements of a segment become visible.
type ElementMode uint8

const (
	// ElementModeActive segments are copied into a table at instantiation time.
	ElementModeActive ElementMode = iota
	// ElementModePassive segments are only available to table.init/table.copy.
	ElementModePassive
	// ElementModeDeclarative segments exist only for validation purposes.
	ElementModeDeclarative
)

func (m ElementMode) String() string {
	switch m {
	case ElementModeActive:
		return "active"
	case ElementModePassive:
		return "passive"
	case ElementModeDeclarative:
		return "declarative"
	default:
		return fmt.Sprintf("<unknown element mode %d>", uint8(m))
	}
}

// ElementExprKind distinguishes the two initializer expressions accepted in
// element segments.
type ElementExprKind uint8

const (
	// ElementExprNull is a ref.null expression.
	ElementExprNull ElementExprKind = iota
	// ElementExprFunc is a ref.func expression.
	ElementExprFunc
)

// ElementExpr is a decoded element initializer expression. Only a single
// ref.null or ref.func instruction followed by end is allowed.
type ElementExpr struct {
	Kind      ElementExprKind
	RefType   ElemType // funcref for ref.func, the immediate type for ref.null
	FuncIndex uint32   // only meaningful when Kind is ElementExprFunc
}

// ParseElementExpr decodes a raw element initializer expression. The raw bytes
// must contain exactly one ref.func or ref.null instruction followed by end.
func ParseElementExpr(raw []byte) (ElementExpr, error) {
	var out ElementExpr
	r := bytes.NewReader(raw)
	op, err := ReadByte(r)
	if err != nil {
		return out, InvalidElementExprError{Reason: "truncated expression"}
	}

	switch op {
	case refFuncOpcode:
		index, err := leb128.ReadVarUint32(r)
		if err != nil {
			return out, InvalidElementExprError{Reason: "truncated ref.func index"}
		}
		out.Kind = ElementExprFunc
		out.RefType = ElemTypeFuncref
		out.FuncIndex = index
	case refNullOpcode:
		b, err := ReadByte(r)
		if err != nil {
			return out, InvalidElementExprError{Reason: "truncated ref.null type"}
		}
		t := ElemType(b)
		if !t.IsValid() {
			return out, InvalidRefTypeError(b)
		}
		out.Kind = ElementExprNull
		out.RefType = t
	default:
		return out, InvalidElementExprError{Reason: fmt.Sprintf("unexpected opcode %#x", op)}
	}

	if b, err := ReadByte(r); err != nil {
		return out, InvalidElementExprError{Reason: "missing end"}
	} else if b != end {
		return out, InvalidElementExprError{Reason: fmt.Sprintf("unexpected opcode %#x after initializer", b)}
	}
	if r.Len() != 0 {
		return out, InvalidElementExprError{Reason: "trailing bytes after end"}
	}
	return out, nil
}

// readElementExpr consumes one ref.func/ref.null expression from r and returns
// its exact bytes, including the terminating end opcode.
func readElementExpr(r io.Reader) ([]byte, error) {
	buf := new(bytes.Buffer)
	tee := io.TeeReader(r, buf)

	op, err := ReadByte(tee)
	if err != nil {
		return nil, InvalidElementExprError{Reason: "truncated expression"}
	}
	switch op {
	case refFuncOpcode:
		if _, err := leb128.ReadVarUint32(tee); err != nil {
			return nil, InvalidElementExprError{Reason: "truncated ref.func index"}
		}
	case refNullOpcode:
		b, err := ReadByte(tee)
		if err != nil {
			return nil, InvalidElementExprError{Reason: "truncated ref.null type"}
		}
		t := ElemType(b)
		if !t.IsValid() {
			return nil, InvalidRefTypeError(b)
		}
	default:
		return nil, InvalidElementExprError{Reason: fmt.Sprintf("unexpected opcode %#x", op)}
	}

	b, err := ReadByte(tee)
	if err != nil {
		return nil, InvalidElementExprError{Reason: "missing end"}
	}
	if b != end {
		return nil, InvalidElementExprError{Reason: fmt.Sprintf("unexpected opcode %#x after initializer", b)}
	}
	return buf.Bytes(), nil
}

// ElementSegment describes a group of repeated elements that begin at a
// specified offset (for active segments) or that are merely declared.
//
// The reference-types proposal defines eight encodings, selected by a flags
// byte in the range 0-7:
//
//	0: active, implicit table 0, vec(funcidx)
//	1: passive, elemkind, vec(funcidx)
//	2: active, explicit table index, offset, elemkind, vec(funcidx)
//	3: declarative, elemkind, vec(funcidx)
//	4: active, implicit table 0, offset, vec(expr)
//	5: passive, reftype, vec(expr)
//	6: active, explicit table index, offset, reftype, vec(expr)
//	7: declarative, reftype, vec(expr)
//
// The legacy MVP encoding corresponds to flags 0. Constructing an
// ElementSegment with only Index, Offset, and Elems (that is, Mode left at its
// zero value, ExplicitTable false, and Exprs nil) therefore encodes flags 0.
//
// Index is the target table index for active segments and is encoded when
// ExplicitTable is true (flags 2 and 6). Offset holds the raw bytes of the
// numeric initializer expression used by active segments, including its end
// opcode. Exactly one of Elems (function index vector) and Exprs (initializer
// expression vector) describes the segment contents; Exprs is non-nil when the
// expression encoding is used, even if the vector is empty. Expression bytes
// are preserved verbatim so that a null reference is never confused with
// function zero.
type ElementSegment struct {
	Index  uint32 // The index into the global table space for active segments.
	Offset []byte // Initializer expression for the table offset; active segments only.
	Elems  []uint32

	// Flags is the raw flags byte observed while decoding. It is informational;
	// MarshalWASM derives the encoding from the fields below so that the exact
	// segment form is preserved.
	Flags uint8
	// Mode selects active, passive, or declarative semantics.
	Mode ElementMode
	// ExplicitTable records that the encoding carried an explicit table index.
	// This distinguishes explicit table zero (flags 2/6) from the implicit
	// table zero used by flags 0/4.
	ExplicitTable bool
	// ElemType is the reference type of the segment contents.
	ElemType ElemType
	// Exprs holds raw ref.func/ref.null expressions when the expression
	// encoding is used. It is nil for function-index segments.
	Exprs [][]byte
}

// InvalidRefTypeError reports an unsupported reference type byte.
type InvalidRefTypeError uint8

func (e InvalidRefTypeError) Error() string {
	return fmt.Sprintf("wasm: unsupported reference type: %#x", uint8(e))
}

// IsActive reports whether the segment is copied into a table at instantiation.
func (s *ElementSegment) IsActive() bool { return s.Mode == ElementModeActive }

// UsesExpressions reports whether the segment stores initializer expressions
// rather than plain function indices.
func (s *ElementSegment) UsesExpressions() bool { return s.Exprs != nil }

// ReferenceType returns the reference type of the segment contents. The zero
// value is interpreted as funcref for the benefit of legacy element segments
// that predate the explicit type field.
func (s *ElementSegment) ReferenceType() ElemType {
	if s.ElemType == 0 {
		return ElemTypeFuncref
	}
	return s.ElemType
}

// encodingFlags computes the reference-types flags byte for s.
func (s *ElementSegment) encodingFlags() (uint8, error) {
	exprs := s.UsesExpressions()
	explicitTable := s.ExplicitTable || (s.Mode == ElementModeActive && s.Index != 0)
	switch s.Mode {
	case ElementModeActive:
		if explicitTable {
			if exprs {
				return 6, nil
			}
			return 2, nil
		}
		if exprs {
			return 4, nil
		}
		return 0, nil
	case ElementModePassive:
		if exprs {
			return 5, nil
		}
		return 1, nil
	case ElementModeDeclarative:
		if exprs {
			return 7, nil
		}
		return 3, nil
	default:
		return 0, fmt.Errorf("wasm: invalid element mode %d", uint8(s.Mode))
	}
}

// TableElems returns the table entries contributed by the segment, in order.
// A nil entry represents a null reference, which leaves the corresponding
// table slot uninitialized rather than pointing at function zero.
func (s *ElementSegment) TableElems() ([]*uint32, error) {
	elems := make([]*uint32, 0, len(s.Elems)+len(s.Exprs))
	if !s.UsesExpressions() {
		for _, index := range s.Elems {
			i := index
			elems = append(elems, &i)
		}
		return elems, nil
	}
	for _, raw := range s.Exprs {
		expr, err := ParseElementExpr(raw)
		if err != nil {
			return nil, err
		}
		if expr.Kind == ElementExprFunc {
			i := expr.FuncIndex
			elems = append(elems, &i)
		} else {
			elems = append(elems, nil)
		}
	}
	return elems, nil
}

func (s *ElementSegment) UnmarshalWASM(r io.Reader) error {
	*s = ElementSegment{}

	flag, err := ReadByte(r)
	if err != nil {
		return err
	}
	if flag > 7 {
		return InvalidElementFlagsError(flag)
	}
	s.Flags = flag

	// Decode the mode and the table index.
	switch flag {
	case 0, 4:
		s.Mode = ElementModeActive
		s.Index = 0
	case 1, 5:
		s.Mode = ElementModePassive
	case 2, 6:
		s.Mode = ElementModeActive
		s.ExplicitTable = true
		s.Index, err = leb128.ReadVarUint32(r)
		if err != nil {
			return err
		}
	case 3, 7:
		s.Mode = ElementModeDeclarative
	}

	// Active segments carry an offset expression before the type field.
	if s.Mode == ElementModeActive {
		s.Offset, err = readInitExpr(r)
		if err != nil {
			return err
		}
	}

	// Flags 1-3 carry an elemkind byte, which must be 0x00 (funcref).
	switch flag {
	case 1, 2, 3:
		kind, err := ReadByte(r)
		if err != nil {
			return err
		}
		if kind != 0x00 {
			return InvalidElementKindError(kind)
		}
	}

	// Flags 5-7 carry an explicit reference type.
	switch flag {
	case 5, 6, 7:
		var refType ElemType
		if err := refType.decode(r); err != nil {
			return err
		}
		s.ElemType = refType
	default:
		s.ElemType = ElemTypeFuncref
	}

	if flag <= 3 {
		numElems, err := leb128.ReadVarUint32(r)
		if err != nil {
			return err
		}
		s.Elems = make([]uint32, 0, getInitialCap(numElems))
		for i := uint32(0); i < numElems; i++ {
			e, err := leb128.ReadVarUint32(r)
			if err != nil {
				return err
			}
			s.Elems = append(s.Elems, e)
		}
		return nil
	}

	numExprs, err := leb128.ReadVarUint32(r)
	if err != nil {
		return err
	}
	s.Exprs = make([][]byte, 0, getInitialCap(numExprs))
	for i := uint32(0); i < numExprs; i++ {
		expr, err := readElementExpr(r)
		if err != nil {
			return err
		}
		s.Exprs = append(s.Exprs, expr)
	}
	return nil
}

// decode reads a reference type byte into t.
func (t *ElemType) decode(r io.Reader) error {
	b, err := ReadByte(r)
	if err != nil {
		return err
	}
	*t = ElemType(b)
	if !t.IsValid() {
		return InvalidRefTypeError(b)
	}
	return nil
}

// explicitTableFlag reports whether a flags value encodes an explicit table
// index (flags 2 and 6).
func explicitTableFlag(flag uint8) bool {
	return flag == 2 || flag == 6
}

func (s *ElementSegment) MarshalWASM(w io.Writer) error {
	flag, err := s.encodingFlags()
	if err != nil {
		return err
	}
	if err := writeByte(w, flag); err != nil {
		return err
	}

	if s.Mode == ElementModeActive && explicitTableFlag(flag) {
		if _, err := leb128.WriteVarUint32(w, s.Index); err != nil {
			return err
		}
	}
	if s.Mode == ElementModeActive {
		if _, err := w.Write(s.Offset); err != nil {
			return err
		}
	}

	if flag >= 1 && flag <= 3 {
		if err := writeByte(w, 0x00); err != nil {
			return err
		}
	}
	if flag >= 5 && flag <= 7 {
		if err := s.ReferenceType().MarshalWASM(w); err != nil {
			return err
		}
	}

	if s.UsesExpressions() {
		if _, err := leb128.WriteVarUint32(w, uint32(len(s.Exprs))); err != nil {
			return err
		}
		for _, expr := range s.Exprs {
			if _, err := w.Write(expr); err != nil {
				return err
			}
		}
		return nil
	}

	if _, err := leb128.WriteVarUint32(w, uint32(len(s.Elems))); err != nil {
		return err
	}
	for _, e := range s.Elems {
		if _, err := leb128.WriteVarUint32(w, e); err != nil {
			return err
		}
	}
	return nil
}
