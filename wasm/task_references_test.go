// Copyright 2026 The go-interpreter Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package wasm_test

import (
	"bytes"
	"testing"

	"github.com/go-interpreter/wagon/disasm"
	"github.com/go-interpreter/wagon/task_support/taskcheck"
	"github.com/go-interpreter/wagon/validate"
	"github.com/go-interpreter/wagon/wasm"
	"github.com/go-interpreter/wagon/wasm/leb128"
	ops "github.com/go-interpreter/wagon/wasm/operators"
)

// --- small binary builders -------------------------------------------------

func u32(v uint32) []byte {
	var b bytes.Buffer
	_, _ = leb128.WriteVarUint32(&b, v)
	return b.Bytes()
}

func vec(items ...[]byte) []byte {
	var b bytes.Buffer
	b.Write(u32(uint32(len(items))))
	for _, it := range items {
		b.Write(it)
	}
	return b.Bytes()
}

func section(id byte, payload []byte) []byte {
	var b bytes.Buffer
	b.WriteByte(id)
	b.Write(u32(uint32(len(payload))))
	b.Write(payload)
	return b.Bytes()
}

func typeSection(sigs ...wasm.FunctionSig) []byte {
	var items [][]byte
	for _, sig := range sigs {
		var b bytes.Buffer
		_ = sig.MarshalWASM(&b)
		items = append(items, b.Bytes())
	}
	return section(byte(wasm.SectionIDType), vec(items...))
}

func functionSection(types ...uint32) []byte {
	var items [][]byte
	for _, t := range types {
		items = append(items, u32(t))
	}
	return section(byte(wasm.SectionIDFunction), vec(items...))
}

func tableSection(tables ...wasm.Table) []byte {
	var items [][]byte
	for _, t := range tables {
		var b bytes.Buffer
		_ = t.MarshalWASM(&b)
		items = append(items, b.Bytes())
	}
	return section(byte(wasm.SectionIDTable), vec(items...))
}

func elementSection(segs ...*wasm.ElementSegment) []byte {
	var items [][]byte
	for _, seg := range segs {
		var b bytes.Buffer
		_ = seg.MarshalWASM(&b)
		items = append(items, b.Bytes())
	}
	return section(byte(wasm.SectionIDElement), vec(items...))
}

func codeSection(bodies ...wasm.FunctionBody) []byte {
	var items [][]byte
	for _, body := range bodies {
		var b bytes.Buffer
		_ = body.MarshalWASM(&b)
		items = append(items, b.Bytes())
	}
	return section(byte(wasm.SectionIDCode), vec(items...))
}

func importSection(entries ...wasm.ImportEntry) []byte {
	var items [][]byte
	for _, e := range entries {
		var b bytes.Buffer
		if err := writeImportEntry(&b, e); err != nil {
			panic(err)
		}
		items = append(items, b.Bytes())
	}
	return section(byte(wasm.SectionIDImport), vec(items...))
}

// writeImportEntry mirrors the unexported encoder used by SectionImports.
func writeImportEntry(w *bytes.Buffer, e wasm.ImportEntry) error {
	if _, err := leb128.WriteVarUint32(w, uint32(len(e.ModuleName))); err != nil {
		return err
	}
	w.WriteString(e.ModuleName)
	if _, err := leb128.WriteVarUint32(w, uint32(len(e.FieldName))); err != nil {
		return err
	}
	w.WriteString(e.FieldName)
	if err := e.Type.Kind().MarshalWASM(w); err != nil {
		return err
	}
	return e.Type.MarshalWASM(w)
}

func module(parts ...[]byte) []byte {
	raw := []byte{0, 97, 115, 109, 1, 0, 0, 0}
	for _, p := range parts {
		raw = append(raw, p...)
	}
	return raw
}

func offsetExpr(v int32) []byte {
	var b bytes.Buffer
	b.WriteByte(0x41) // i32.const
	_, _ = leb128.WriteVarint64(&b, int64(v))
	b.WriteByte(0x0b)
	return b.Bytes()
}

func refFunc(index uint32) []byte {
	return append([]byte{0xd2}, append(u32(index), 0x0b)...)
}

func refNull(t wasm.ElemType) []byte {
	return []byte{0xd0, byte(t), 0x0b}
}

// indirectBody builds a function body containing a call_indirect. The
// trailing end opcode is added by FunctionBody.MarshalWASM, so it is not
// included here. A selector is pushed for validation-oriented bodies.
func indirectBody(typeIdx, tableIdx uint32) []byte {
	body := []byte{ops.I32Const, 0, ops.CallIndirect}
	body = append(body, u32(typeIdx)...)
	body = append(body, u32(tableIdx)...)
	return body
}

// indirectCode returns the raw instruction bytes for call_indirect without a
// preceding selector, for direct Disassemble/Assemble roundtrip checks.
func indirectCode(typeIdx, tableIdx uint32) []byte {
	body := []byte{ops.CallIndirect}
	body = append(body, u32(typeIdx)...)
	body = append(body, u32(tableIdx)...)
	return body
}

func bodyWith(code []byte) wasm.FunctionBody {
	return wasm.FunctionBody{Code: code}
}

func sig(params ...wasm.ValueType) wasm.FunctionSig {
	return wasm.FunctionSig{Form: 0x60, ParamTypes: params}
}

func decode(t *testing.T, raw []byte) (*wasm.Module, error) {
	t.Helper()
	return wasm.DecodeModule(bytes.NewReader(raw))
}

func encodeModule(t *testing.T, m *wasm.Module) []byte {
	t.Helper()
	var b bytes.Buffer
	if err := wasm.EncodeModule(&b, m); err != nil {
		t.Fatalf("EncodeModule: %v", err)
	}
	return b.Bytes()
}

// --- TestElementForms ------------------------------------------------------

func TestElementForms(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Legacy MVP segment: flags 0, implicit table zero, function indices.
		legacy := &wasm.ElementSegment{Offset: offsetExpr(0), Elems: []uint32{0}}
		legacyRaw := module(
			typeSection(sig()),
			functionSection(0, 0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 2}}),
			elementSection(legacy),
			codeSection(bodyWith(nil), bodyWith(nil)),
		)

		m, err := decode(t, legacyRaw)
		c.Check(err == nil, "legacy decode: %v", err)
		if err == nil {
			seg := m.Elements.Entries[0]
			c.Check(seg.Mode == wasm.ElementModeActive, "legacy mode = %v", seg.Mode)
			c.Check(seg.UsesExpressions() == false, "legacy stored expressions")
			c.Check(len(seg.Elems) == 1 && seg.Elems[0] == 0, "legacy elems = %v", seg.Elems)
			c.Check(bytes.Equal(seg.Offset, offsetExpr(0)), "legacy offset bytes = %x", seg.Offset)
			c.Check(seg.ExplicitTable == false, "legacy table should be implicit")
			c.Check(seg.Flags == 0, "legacy flags = %d", seg.Flags)
			c.Check(encodeModule(t, m) != nil, "legacy re-encode")
			c.Check(bytes.Equal(encodeModule(t, m), legacyRaw), "legacy roundtrip bytes differ")
		}

		// Explicit table zero (flags 2) must be distinguishable from implicit
		// table zero (flags 0), and both must roundtrip.
		explicit0 := &wasm.ElementSegment{Index: 0, ExplicitTable: true, Offset: offsetExpr(0), Elems: []uint32{0}}
		rawExplicit := module(
			typeSection(sig()),
			functionSection(0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}),
			elementSection(explicit0),
			codeSection(bodyWith(nil)),
		)
		em, err := decode(t, rawExplicit)
		c.Check(err == nil, "explicit table decode: %v", err)
		if err == nil {
			eseg := em.Elements.Entries[0]
			c.Check(eseg.ExplicitTable, "explicit table zero not recorded")
			c.Check(eseg.Flags == 2, "explicit table flags = %d", eseg.Flags)
			c.Check(bytes.Equal(encodeModule(t, em), rawExplicit), "explicit table zero roundtrip")
		}

		// Flags 1: passive segment with elemkind and function indices.
		passive := &wasm.ElementSegment{Mode: wasm.ElementModePassive, Elems: []uint32{1}}
		rawPassive := module(typeSection(sig()), functionSection(0), elementSection(passive))
		pm, err := decode(t, rawPassive)
		c.Check(err == nil, "passive decode: %v", err)
		if err == nil {
			pseg := pm.Elements.Entries[0]
			c.Check(pseg.Mode == wasm.ElementModePassive, "passive mode = %v", pseg.Mode)
			c.Check(pseg.Index == 0, "passive table index = %d", pseg.Index)
			c.Check(pseg.Flags == 1, "passive flags = %d", pseg.Flags)
			c.Check(bytes.Equal(encodeModule(t, pm), rawPassive), "passive roundtrip")
		}

		// Flags 3: declarative segment.
		decl := &wasm.ElementSegment{Mode: wasm.ElementModeDeclarative, Elems: []uint32{0}}
		rawDecl := module(typeSection(sig()), functionSection(0), elementSection(decl))
		dm, err := decode(t, rawDecl)
		c.Check(err == nil, "declarative decode: %v", err)
		if err == nil {
			dseg := dm.Elements.Entries[0]
			c.Check(dseg.Mode == wasm.ElementModeDeclarative, "declarative mode = %v", dseg.Mode)
			c.Check(dseg.IsActive() == false, "declarative segment marked active")
			c.Check(bytes.Equal(encodeModule(t, dm), rawDecl), "declarative roundtrip")
		}

		// Flags 4: active, implicit table zero, expression vector. A null
		// reference must stay null (not become function zero), and the bytes
		// must roundtrip exactly.
		actExpr := &wasm.ElementSegment{Offset: offsetExpr(0), Exprs: [][]byte{refNull(wasm.ElemTypeFuncref), refFunc(0)}}
		rawActExpr := module(
			typeSection(sig()),
			functionSection(0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 2}}),
			elementSection(actExpr),
			codeSection(bodyWith(nil)),
		)
		am, err := decode(t, rawActExpr)
		c.Check(err == nil, "expression segment decode: %v", err)
		if err == nil {
			aseg := am.Elements.Entries[0]
			c.Check(aseg.UsesExpressions(), "expression segment not recorded as expressions")
			c.Check(len(aseg.Exprs) == 2, "expression count = %d", len(aseg.Exprs))
			c.Check(aseg.Flags == 4, "expression segment flags = %d", aseg.Flags)
			c.Check(bytes.Equal(aseg.Exprs[0], refNull(wasm.ElemTypeFuncref)), "null expr changed: %x", aseg.Exprs[0])
			c.Check(bytes.Equal(aseg.Exprs[1], refFunc(0)), "ref.func expr changed: %x", aseg.Exprs[1])
			c.Check(bytes.Equal(encodeModule(t, am), rawActExpr), "expression segment roundtrip")

			elems, terr := aseg.TableElems()
			c.Check(terr == nil, "TableElems: %v", terr)
			if terr == nil {
				c.Check(len(elems) == 2, "table elems count = %d", len(elems))
				c.Check(elems[0] == nil, "null became a function index")
				c.Check(elems[1] != nil && *elems[1] == 0, "ref.func zero lost")
			}
		}

		// Flags 5: passive externref expression segment; may hold null only.
		externPassive := &wasm.ElementSegment{Mode: wasm.ElementModePassive, ElemType: wasm.ElemTypeExternRef, Exprs: [][]byte{refNull(wasm.ElemTypeExternRef)}}
		rawExtern := module(elementSection(externPassive))
		xe, err := decode(t, rawExtern)
		c.Check(err == nil, "externref passive decode: %v", err)
		if err == nil {
			xseg := xe.Elements.Entries[0]
			c.Check(xseg.Mode == wasm.ElementModePassive, "externref mode = %v", xseg.Mode)
			c.Check(xseg.ReferenceType() == wasm.ElemTypeExternRef, "externref type = %v", xseg.ReferenceType())
			c.Check(xseg.Flags == 5, "externref flags = %d", xseg.Flags)
			c.Check(xseg.Index == 0, "externref passive index = %d", xseg.Index)
			c.Check(bytes.Equal(encodeModule(t, xe), rawExtern), "externref roundtrip")
		}

		// Flags 6: active with explicit table index and expression vector.
		tableExpr := &wasm.ElementSegment{Index: 1, ExplicitTable: true, Offset: offsetExpr(0), ElemType: wasm.ElemTypeFuncref, Exprs: [][]byte{refFunc(0)}}
		rawTableExpr := module(
			typeSection(sig()),
			functionSection(0),
			tableSection(
				wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}},
				wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}},
			),
			elementSection(tableExpr),
			codeSection(bodyWith(nil)),
		)
		tm, err := decode(t, rawTableExpr)
		c.Check(err == nil, "explicit expression table decode: %v", err)
		if err == nil {
			tseg := tm.Elements.Entries[0]
			c.Check(tseg.Index == 1 && tseg.ExplicitTable, "explicit table index = %d", tseg.Index)
			c.Check(tseg.Flags == 6, "explicit expression flags = %d", tseg.Flags)
			c.Check(bytes.Equal(encodeModule(t, tm), rawTableExpr), "explicit expression roundtrip")
		}

		// Flags 7: declarative expression segment.
		declExpr := &wasm.ElementSegment{Mode: wasm.ElementModeDeclarative, ElemType: wasm.ElemTypeFuncref, Exprs: [][]byte{refFunc(0)}}
		rawDeclExpr := module(typeSection(sig()), functionSection(0), elementSection(declExpr))
		dy, err := decode(t, rawDeclExpr)
		c.Check(err == nil, "declarative expression decode: %v", err)
		if err == nil {
			dseg := dy.Elements.Entries[0]
			c.Check(dseg.Mode == wasm.ElementModeDeclarative, "declarative expr mode = %v", dseg.Mode)
			c.Check(dseg.UsesExpressions(), "declarative expr not expressions")
			c.Check(bytes.Equal(encodeModule(t, dy), rawDeclExpr), "declarative expression roundtrip")
		}

		// A passive segment appended after an active one: the reader must not
		// misparse the following bytes, and a later segment must survive.
		mixed := module(
			typeSection(sig()),
			functionSection(0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}),
			elementSection(
				&wasm.ElementSegment{Offset: offsetExpr(0), Elems: []uint32{0}},
				&wasm.ElementSegment{Mode: wasm.ElementModePassive, Elems: []uint32{0}},
				&wasm.ElementSegment{Mode: wasm.ElementModeDeclarative, Exprs: [][]byte{refNull(wasm.ElemTypeFuncref)}},
			),
			codeSection(bodyWith(nil)),
		)
		mx, err := decode(t, mixed)
		c.Check(err == nil, "mixed decode: %v", err)
		if err == nil {
			c.Check(len(mx.Elements.Entries) == 3, "mixed segment count = %d", len(mx.Elements.Entries))
			c.Check(mx.Elements.Entries[0].Mode == wasm.ElementModeActive, "mixed[0] mode = %v", mx.Elements.Entries[0].Mode)
			c.Check(mx.Elements.Entries[1].Mode == wasm.ElementModePassive, "mixed[1] mode = %v", mx.Elements.Entries[1].Mode)
			c.Check(mx.Elements.Entries[2].Mode == wasm.ElementModeDeclarative, "mixed[2] mode = %v", mx.Elements.Entries[2].Mode)
			c.Check(bytes.Equal(encodeModule(t, mx), mixed), "mixed roundtrip")
		}
	})
}

// --- TestReferenceTyping ---------------------------------------------------

func TestReferenceTyping(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		baseTypes := typeSection(sig())

		verify := func(raw []byte) error {
			m, err := decode(t, raw)
			if err != nil {
				return err
			}
			return validate.VerifyModule(m)
		}

		// Function imports occupy their own index prefix. An element segment
		// referencing the imported function (index 0) and the defined function
		// (index 1) must validate; skipping the reset of the prefix would
		// wrongly reject index 1 as out of range.
		rawInterleaved := module(
			baseTypes,
			importSection(
				wasm.ImportEntry{ModuleName: "m", FieldName: "f", Type: wasm.FuncImport{Type: 0}},
				wasm.ImportEntry{ModuleName: "m", FieldName: "t", Type: wasm.TableImport{Type: wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}}},
			),
			functionSection(0),
			elementSection(&wasm.ElementSegment{ExplicitTable: true, Offset: offsetExpr(0), Elems: []uint32{0, 1}}),
			codeSection(bodyWith(nil)),
		)
		c.Check(verify(rawInterleaved) == nil, "import prefix validation: %v", verify(rawInterleaved))

		// Imported table counts as table zero; a local table is table one.
		rawImportedTable := module(
			baseTypes,
			importSection(
				wasm.ImportEntry{ModuleName: "m", FieldName: "t", Type: wasm.TableImport{Type: wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}}},
			),
			functionSection(0),
			elementSection(&wasm.ElementSegment{Index: 0, ExplicitTable: true, Offset: offsetExpr(0), Elems: []uint32{0}}),
			codeSection(bodyWith(nil)),
		)
		c.Check(verify(rawImportedTable) == nil, "imported table validation: %v", verify(rawImportedTable))

		// Function index beyond the real function range must be rejected.
		rawFuncOutOfRange := module(
			baseTypes,
			functionSection(0),
			elementSection(&wasm.ElementSegment{Offset: offsetExpr(0), Elems: []uint32{1}}),
			codeSection(bodyWith(nil)),
		)
		c.Check(verify(rawFuncOutOfRange) != nil, "out-of-range function accepted")

		// An active funcref segment targeting an externref table is invalid.
		rawTypeMismatch := module(
			baseTypes,
			functionSection(0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeExternRef, Limits: wasm.ResizableLimits{Initial: 1}}),
			elementSection(&wasm.ElementSegment{Offset: offsetExpr(0), Elems: []uint32{0}}),
			codeSection(bodyWith(nil)),
		)
		c.Check(verify(rawTypeMismatch) != nil, "funcref segment accepted on externref table")

		// A passive externref segment needs no table and accepts null.
		rawExternPassive := module(elementSection(&wasm.ElementSegment{
			Mode:     wasm.ElementModePassive,
			ElemType: wasm.ElemTypeExternRef,
			Exprs:    [][]byte{refNull(wasm.ElemTypeExternRef)},
		}))
		c.Check(verify(rawExternPassive) == nil, "externref passive rejected: %v", verify(rawExternPassive))

		// A passive funcref segment also needs no table.
		rawPassiveNoTable := module(elementSection(&wasm.ElementSegment{
			Mode:  wasm.ElementModePassive,
			Elems: []uint32{0},
		}))
		c.Check(verify(rawPassiveNoTable) != nil, "passive segment with no functions accepted")

		// externref segments must not contain ref.func.
		rawExternFunc := module(typeSection(sig()), functionSection(0), elementSection(&wasm.ElementSegment{
			Mode:     wasm.ElementModePassive,
			ElemType: wasm.ElemTypeExternRef,
			Exprs:    [][]byte{refFunc(0)},
		}))
		c.Check(verify(rawExternFunc) != nil, "ref.func accepted in externref segment")

		// A null reference of the wrong type inside an externref segment is
		// invalid.
		rawNullMismatch := module(elementSection(&wasm.ElementSegment{
			Mode:     wasm.ElementModePassive,
			ElemType: wasm.ElemTypeExternRef,
			Exprs:    [][]byte{refNull(wasm.ElemTypeFuncref)},
		}))
		c.Check(verify(rawNullMismatch) != nil, "mismatched ref.null accepted")

		// call_indirect may only target an existing funcref table; table one
		// must exist here, and an externref table must be rejected.
		rawIndirectOK := module(
			baseTypes,
			functionSection(0, 0),
			tableSection(
				wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}},
				wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}},
			),
			codeSection(
				bodyWith(indirectBody(0, 0)),
				bodyWith(indirectBody(0, 1)),
			),
		)
		c.Check(verify(rawIndirectOK) == nil, "call_indirect to table one rejected: %v", verify(rawIndirectOK))

		rawIndirectMissing := module(
			baseTypes,
			functionSection(0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}),
			codeSection(bodyWith(indirectBody(0, 5))),
		)
		c.Check(verify(rawIndirectMissing) != nil, "call_indirect to missing table accepted")

		rawIndirectExtern := module(
			baseTypes,
			functionSection(0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeExternRef, Limits: wasm.ResizableLimits{Initial: 1}}),
			codeSection(bodyWith(indirectBody(0, 0))),
		)
		c.Check(verify(rawIndirectExtern) != nil, "call_indirect to externref table accepted")

		// A malformed element segment fails to decode rather than panic. The
		// payload below is flags 8 followed by plausible segment bytes.
		badFlags := module(typeSection(sig()), section(byte(wasm.SectionIDElement), []byte{1, 8, 0x41, 0x00, 0x0b, 1, 0}))
		recovered := func() (panicked bool) {
			defer func() { panicked = recover() != nil }()
			if _, derr := decode(t, badFlags); derr == nil {
				t.Error("invalid element flags accepted")
			}
			return false
		}()
		c.Check(!recovered, "invalid flags panicked instead of erroring")

		// Truncated initializer expressions, unsupported reference types, and
		// an invalid elemkind all return errors.
		malformed := map[string][]byte{
			"truncated ref.func index": section(byte(wasm.SectionIDElement), []byte{1, 5, 0x70, 1, 0xd2, 0x80}),
			"truncated expression":     section(byte(wasm.SectionIDElement), []byte{1, 5, 0x70, 1, 0xd2}),
			"missing end":              section(byte(wasm.SectionIDElement), []byte{1, 5, 0x70, 1, 0xd0, 0x70}),
			"bad reference type":       section(byte(wasm.SectionIDElement), []byte{1, 5, 0x6e, 0}),
			"bad ref.null type":        section(byte(wasm.SectionIDElement), []byte{1, 5, 0x70, 1, 0xd0, 0x6e, 0x0b}),
			"bad elemkind":             section(byte(wasm.SectionIDElement), []byte{1, 1, 0x01, 0}),
			"truncated offset":         section(byte(wasm.SectionIDElement), []byte{1, 0, 0x41}),
			"expression with extra op": section(byte(wasm.SectionIDElement), []byte{1, 5, 0x70, 1, 0xd0, 0x70, 0x41, 0x00, 0x0b}),
		}
		for name, sec := range malformed {
			raw := module(typeSection(sig()), functionSection(0), sec)
			panicked := func() (panicked bool) {
				defer func() { panicked = recover() != nil }()
				if _, derr := decode(t, raw); derr == nil {
					t.Errorf("%s: malformed element section accepted", name)
				}
				return false
			}()
			c.Check(!panicked, "%s: panicked instead of erroring", name)
		}

		// ParseElementExpr rejects non-single-expression byte strings.
		for _, expr := range [][]byte{
			{0xd0, 0x70},             // no end
			{0xd2, 0x00, 0x0b, 0x0b}, // trailing end
			{0x41, 0x00, 0x0b},       // not a reference expression
			{},
		} {
			if _, perr := wasm.ParseElementExpr(expr); perr == nil {
				t.Errorf("ParseElementExpr accepted %x", expr)
			}
		}
		// A well-formed ref.null is parsed without turning into function zero.
		ne, perr := wasm.ParseElementExpr(refNull(wasm.ElemTypeFuncref))
		c.Check(perr == nil, "ref.null parse: %v", perr)
		c.Check(ne.Kind == wasm.ElementExprNull, "ref.null parsed as kind %d", ne.Kind)
		c.Check(ne.RefType == wasm.ElemTypeFuncref, "ref.null type = %v", ne.RefType)
		nf, perr := wasm.ParseElementExpr(refFunc(7))
		c.Check(perr == nil, "ref.func parse: %v", perr)
		c.Check(nf.Kind == wasm.ElementExprFunc && nf.FuncIndex == 7, "ref.func parsed = %+v", nf)
	})
}

// --- TestIndirectTables ----------------------------------------------------

func TestIndirectTables(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Multi-byte table index (>127) must survive disassemble/assemble.
		code := indirectCode(0, 130)
		instrs, err := disasm.Disassemble(code)
		c.Check(err == nil, "disassemble multi-byte table: %v", err)
		if err == nil {
			ci := instrs[0]
			c.Check(ci.Op.Code == ops.CallIndirect, "first instruction is %s", ci.Op.Name)
			c.Check(len(ci.Immediates) == 2, "call_indirect immediate count = %d", len(ci.Immediates))
			if len(ci.Immediates) == 2 {
				c.Check(ci.Immediates[0].(uint32) == 0, "type index = %d", ci.Immediates[0])
				c.Check(ci.Immediates[1].(uint32) == 130, "table index = %d", ci.Immediates[1])
			}
			out, aerr := disasm.Assemble(instrs)
			c.Check(aerr == nil, "assemble: %v", aerr)
			c.Check(bytes.Equal(out, code), "multi-byte indirect roundtrip: got %x want %x", out, code)
		}

		// Header bytes: 130 encodes as 0x82 0x01.
		c.Check(bytes.Contains(code, []byte{0x82, 0x01}), "LEB128 encoding of 130 missing: %x", code)

		// Up to 4-byte (uint32) values also roundtrip.
		codeBig := indirectCode(0, 0xffffffff)
		bi, berr := disasm.Disassemble(codeBig)
		c.Check(berr == nil, "disassemble uint32 max table: %v", berr)
		if berr == nil {
			bout, baerr := disasm.Assemble(bi)
			c.Check(baerr == nil, "assemble uint32 max: %v", baerr)
			c.Check(bytes.Equal(bout, codeBig), "uint32 max indirect roundtrip: got %x want %x", bout, codeBig)
			c.Check(bi[0].Immediates[1].(uint32) == 0xffffffff, "large table index = %d", bi[0].Immediates[1])
		}

		// A truncated unsigned LEB128 must error, not panic.
		truncated := []byte{ops.CallIndirect, 0, 0x80}
		recovered := func() (panicked bool) {
			defer func() { panicked = recover() != nil }()
			if _, derr := disasm.Disassemble(truncated); derr == nil {
				t.Error("truncated call_indirect LEB accepted")
			}
			return false
		}()
		c.Check(!recovered, "truncated call_indirect LEB panicked")

		// The called type is still checked against the type section, and a
		// function body with an unpopulated runtime index space is really
		// validated.
		rawBadType := module(
			typeSection(sig()),
			functionSection(0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}),
			codeSection(bodyWith(indirectBody(5, 0))),
		)
		m, err := decode(t, rawBadType)
		c.Check(err == nil, "bad-type module decode: %v", err)
		if err == nil {
			c.Check(len(m.FunctionIndexSpace) == 0, "DecodeModule populated runtime index space")
			verr := validate.VerifyModule(m)
			c.Check(verr != nil, "out-of-range call_indirect type accepted")
			// Same module with a valid type index validates, proving the
			// function body was actually reached.
			rawGoodType := module(
				typeSection(sig()),
				functionSection(0),
				tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}),
				codeSection(bodyWith(indirectBody(0, 0))),
			)
			gm, gerr := decode(t, rawGoodType)
			c.Check(gerr == nil, "good-type module decode: %v", gerr)
			if gerr == nil {
				c.Check(len(gm.FunctionIndexSpace) == 0, "good module runtime index populated")
				c.Check(validate.VerifyModule(gm) == nil, "good indirect module rejected: %v", validate.VerifyModule(gm))
			}
		}

		// A type mismatch in the selector still fails, showing param/result
		// checks run against the type section.
		rawSelector := module(
			typeSection(sig(wasm.ValueTypeI32)),
			functionSection(0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}),
			codeSection(bodyWith([]byte{ops.F32Const, 0, 0, 0, 0, ops.CallIndirect, 0, 0, ops.Drop})),
		)
		sm, serr := decode(t, rawSelector)
		c.Check(serr == nil, "selector module decode: %v", serr)
		if serr == nil {
			c.Check(validate.VerifyModule(sm) != nil, "call_indirect selector type mismatch accepted")
		}

		// VerifyModule directly accepts DecodeModule output and checks every
		// code body, including the last one.
		rawTwoBodies := module(
			typeSection(sig()),
			functionSection(0, 0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}),
			codeSection(
				bodyWith(nil),
				bodyWith(indirectBody(9, 0)), // invalid, second body
			),
		)
		tm, terr := decode(t, rawTwoBodies)
		c.Check(terr == nil, "two body decode: %v", terr)
		if terr == nil {
			c.Check(len(tm.FunctionIndexSpace) == 0, "two body runtime index populated")
			c.Check(validate.VerifyModule(tm) != nil, "invalid later function body not checked")
		}

		// Error recovery: after a failing module, a good module decodes and
		// validates normally.
		after, aerr := decode(t, module(
			typeSection(sig()),
			functionSection(0),
			tableSection(wasm.Table{ElementType: wasm.ElemTypeFuncref, Limits: wasm.ResizableLimits{Initial: 1}}),
			elementSection(&wasm.ElementSegment{Offset: offsetExpr(0), Elems: []uint32{0}}),
			codeSection(bodyWith(indirectBody(0, 0))),
		))
		c.Check(aerr == nil, "post-error module decode: %v", aerr)
		if aerr == nil {
			c.Check(validate.VerifyModule(after) == nil, "post-error module rejected: %v", validate.VerifyModule(after))
		}
	})
}
