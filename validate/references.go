// Copyright 2026 The go-interpreter Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package validate

import (
	"fmt"

	"github.com/go-interpreter/wagon/wasm"
)

// ElementValidationError describes a statically invalid element segment.
type ElementValidationError struct {
	Segment int    // Index of the offending segment in the element section.
	Index   uint32 // Function index referenced by the segment, when relevant.
	Reason  string
}

func (e ElementValidationError) Error() string {
	if e.Reason == "" {
		return fmt.Sprintf("validate: invalid element segment %d", e.Segment)
	}
	return fmt.Sprintf("validate: invalid element segment %d: %s", e.Segment, e.Reason)
}

// InvalidTableTypeError is returned when a table does not have the reference
// type required by an element segment or call_indirect instruction.
type InvalidTableTypeError struct {
	Table  uint32
	Got    wasm.ElemType
	Wanted wasm.ElemType
}

func (e InvalidTableTypeError) Error() string {
	return fmt.Sprintf("validate: table %d has element type %s, wanted %s", e.Table, e.Got, e.Wanted)
}

// refType normalizes the zero ElemType used by legacy tables and segments to
// funcref.
func refType(t wasm.ElemType) wasm.ElemType {
	if t == 0 {
		return wasm.ElemTypeFuncref
	}
	return t
}

// importedFuncCount returns the number of function imports, which occupy the
// prefix of the function index space.
func importedFuncCount(m *wasm.Module) int {
	if m.Import == nil {
		return 0
	}
	n := 0
	for _, entry := range m.Import.Entries {
		if entry.Type.Kind() == wasm.ExternalFunction {
			n++
		}
	}
	return n
}

// functionCount returns the size of the function index space. When ReadModule
// has already populated the runtime index space (which includes imported
// functions and imported bodies), that is authoritative. Otherwise it is
// derived from the declared sections, with imports occupying the prefix.
func functionCount(m *wasm.Module) int {
	if len(m.FunctionIndexSpace) > 0 {
		return len(m.FunctionIndexSpace)
	}
	imported := importedFuncCount(m)
	if m.Code != nil {
		return imported + len(m.Code.Bodies)
	}
	if m.Function != nil {
		return imported + len(m.Function.Types)
	}
	return imported
}

// tableTypes returns the element type of every table in index order: imported
// tables first, then locally defined tables.
func tableTypes(m *wasm.Module) []wasm.ElemType {
	var tables []wasm.ElemType
	if m.Import != nil {
		for _, entry := range m.Import.Entries {
			if entry.Type.Kind() == wasm.ExternalTable {
				tables = append(tables, refType(entry.Type.(wasm.TableImport).Type.ElementType))
			}
		}
	}
	if m.Table != nil {
		for _, table := range m.Table.Entries {
			tables = append(tables, refType(table.ElementType))
		}
	}
	return tables
}

// verifyElements statically checks the element section. It intentionally does
// not consult table slots: passive and declarative segments need no table at
// all, and active segments are checked against the declared table type only.
func verifyElements(m *wasm.Module) error {
	if m.Elements == nil {
		return nil
	}
	tables := tableTypes(m)
	funcs := uint32(functionCount(m))

	for i, seg := range m.Elements.Entries {
		segType := seg.ReferenceType()
		if !segType.IsValid() {
			return ElementValidationError{Segment: i, Reason: fmt.Sprintf("unsupported reference type %#x", uint8(segType))}
		}

		if seg.IsActive() {
			if int(seg.Index) >= len(tables) {
				return ElementValidationError{Segment: i, Reason: fmt.Sprintf("table index %d out of range", seg.Index)}
			}
			if tables[seg.Index] != segType {
				return ElementValidationError{
					Segment: i,
					Reason:  fmt.Sprintf("segment type %s does not match table %d type %s", segType, seg.Index, tables[seg.Index]),
				}
			}
		}

		if !seg.UsesExpressions() {
			// A function index vector always denotes funcref references.
			if segType != wasm.ElemTypeFuncref {
				return ElementValidationError{Segment: i, Reason: fmt.Sprintf("function index segment cannot have type %s", segType)}
			}
			for _, index := range seg.Elems {
				if index >= funcs {
					return ElementValidationError{Segment: i, Index: index, Reason: fmt.Sprintf("function index %d out of range", index)}
				}
			}
			continue
		}

		for _, raw := range seg.Exprs {
			expr, err := wasm.ParseElementExpr(raw)
			if err != nil {
				return ElementValidationError{Segment: i, Reason: err.Error()}
			}
			switch expr.Kind {
			case wasm.ElementExprFunc:
				if segType != wasm.ElemTypeFuncref {
					return ElementValidationError{Segment: i, Index: expr.FuncIndex, Reason: fmt.Sprintf("ref.func in %s segment", segType)}
				}
				if expr.FuncIndex >= funcs {
					return ElementValidationError{Segment: i, Index: expr.FuncIndex, Reason: fmt.Sprintf("function index %d out of range", expr.FuncIndex)}
				}
			case wasm.ElementExprNull:
				if expr.RefType != segType {
					return ElementValidationError{
						Segment: i,
						Reason:  fmt.Sprintf("ref.null %s does not match segment type %s", expr.RefType, segType),
					}
				}
			default:
				return ElementValidationError{Segment: i, Reason: "unsupported element expression"}
			}
		}
	}
	return nil
}

// functionSigAt resolves a function index to its signature using only the
// module's declared sections, so it works before the runtime index space is
// populated. Function imports occupy the prefix of the index space, while the
// function section stores the type index of each locally defined function.
func functionSigAt(m *wasm.Module, index uint32) (*wasm.FunctionSig, error) {
	if m.Types == nil {
		return nil, fmt.Errorf("validate: missing types section")
	}

	imported := uint32(importedFuncCount(m))
	if index < imported {
		var i uint32
		for _, entry := range m.Import.Entries {
			if entry.Type.Kind() != wasm.ExternalFunction {
				continue
			}
			if i == index {
				typeIndex := entry.Type.(wasm.FuncImport).Type
				if int(typeIndex) >= len(m.Types.Entries) {
					return nil, wasm.InvalidFunctionIndexError(index)
				}
				return &m.Types.Entries[typeIndex], nil
			}
			i++
		}
	}

	if m.Function == nil {
		return nil, wasm.InvalidFunctionIndexError(index)
	}
	defined := index - imported
	// After ReadModule the function section has already had the imported type
	// indices prepended; after DecodeModule it only lists local functions.
	if len(m.FunctionIndexSpace) > 0 {
		defined = index
	}
	if int(defined) >= len(m.Function.Types) {
		return nil, wasm.InvalidFunctionIndexError(index)
	}
	typeIndex := m.Function.Types[defined]
	if int(typeIndex) >= len(m.Types.Entries) {
		return nil, wasm.InvalidFunctionIndexError(index)
	}
	return &m.Types.Entries[typeIndex], nil
}

// resolveFunction returns the signature-bearing function for index, preferring
// the populated runtime index space when it is available.
func resolveFunction(m *wasm.Module, index uint32) (*wasm.Function, error) {
	if len(m.FunctionIndexSpace) > 0 {
		fn := m.GetFunction(int(index))
		if fn == nil {
			return nil, wasm.InvalidFunctionIndexError(index)
		}
		return fn, nil
	}
	sig, err := functionSigAt(m, index)
	if err != nil {
		return nil, err
	}
	return &wasm.Function{Sig: sig}, nil
}

// functionBodyAt returns the locally defined function at the given index in
// the function index space, decoding it from the function and code sections
// when the runtime index space has not been populated.
func functionBodyAt(m *wasm.Module, index uint32) (*wasm.FunctionSig, *wasm.FunctionBody, error) {
	if len(m.FunctionIndexSpace) > 0 {
		fn := m.GetFunction(int(index))
		if fn == nil || fn.Sig == nil || fn.Body == nil {
			return nil, nil, wasm.InvalidFunctionIndexError(index)
		}
		return fn.Sig, fn.Body, nil
	}

	if m.Function == nil || m.Code == nil {
		return nil, nil, wasm.InvalidFunctionIndexError(index)
	}
	imported := uint32(importedFuncCount(m))
	if index < imported {
		// Imported functions have no body to check.
		return nil, nil, nil
	}
	defined := index - imported
	if int(defined) >= len(m.Function.Types) || int(defined) >= len(m.Code.Bodies) {
		return nil, nil, wasm.InvalidFunctionIndexError(index)
	}
	sig, err := functionSigAt(m, index)
	if err != nil {
		return nil, nil, err
	}
	return sig, &m.Code.Bodies[defined], nil
}

// globalAt resolves a global index without requiring the global index space to
// have been populated.
func globalAt(m *wasm.Module, index uint32) *wasm.GlobalEntry {
	if len(m.GlobalIndexSpace) > 0 {
		return m.GetGlobal(int(index))
	}
	if m.Import != nil {
		var i uint32
		for _, entry := range m.Import.Entries {
			if entry.Type.Kind() != wasm.ExternalGlobal {
				continue
			}
			if i == index {
				g := entry.Type.(wasm.GlobalVarImport).Type
				return &wasm.GlobalEntry{Type: g}
			}
			i++
		}
		if index < i {
			return nil
		}
		index -= i
	}
	if m.Global == nil || int(index) >= len(m.Global.Globals) {
		return nil
	}
	return &m.Global.Globals[index]
}
