// Copyright 2017 The go-interpreter Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Package wasm provides functions for reading and parsing WebAssembly modules.
//
// # Reference types
//
// Element segments follow the encodings defined by the reference-types
// proposal. The eight flags values (0-7) select one of the active, passive, or
// declarative modes, an optional explicit table index, and either a vector of
// function indices or a vector of initializer expressions. The legacy MVP
// encoding is flags 0:
//
//	ElementSegment{Index: 0, Offset: offsetExpr, Elems: []uint32{...}}
//
// Constructing an ElementSegment this way keeps producing the legacy active
// segment. Decoding a segment also fills the reference-types metadata:
//
//   - Mode reports whether the segment is active, passive, or declarative
//     (ElementModeActive, ElementModePassive, ElementModeDeclarative).
//   - ExplicitTable distinguishes an explicit table zero (flags 2 and 6) from
//     the implicit table zero used by flags 0 and 4.
//   - ElemType is the reference type of the contents (ElemTypeFuncref or
//     ElemTypeExternRef); use ElementSegment.ReferenceType to treat the zero
//     value as funcref for legacy segments.
//   - Exprs holds the raw bytes of the initializer expressions when the segment
//     uses the expression encoding. It is nil for function-index segments, so
//     UsesExpressions can be used to tell the two apart. Expression bytes are
//     preserved verbatim, so a null reference is never confused with function
//     index zero, and passive or declarative segments never become table-zero
//     initialization.
//
// Element initializer expressions are limited to a single ref.null or ref.func
// instruction followed by end. ElementExpr, ElementExprKind, and
// ParseElementExpr decode such an expression; TableElems exposes the entries a
// segment contributes to a table, with nil standing for a null reference.
//
// Tables and element types use ElemType: ElemTypeFuncref (0x70, also known as
// ElemTypeAnyFunc) and ElemTypeExternRef (0x6f). Active segments must match
// the declared type of their target table; passive and declarative segments
// require no table at all. An externref segment may contain null references
// but cannot contain ref.func.
package wasm
