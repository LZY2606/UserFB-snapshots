// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf

import (
	"encoding/binary"
	"testing"

	"example.org/offline-dwarf/task_support/taskcheck"
)

// This file builds tiny, lawful DWARF byte streams from scratch and drives
// the production entry points (New, Reader, Type, AddTypes) only.

const (
	sigS0 = uint64(0x0101010101010101)
	sigS1 = uint64(0x0202020202020202)
	sigA  = uint64(0xaaaaaaaa11111111)
	sigB  = uint64(0xbbbbbbbb22222222)
	sigC  = uint64(0xcccccccc33333333)
	sigX1 = uint64(0x0a01)
	sigX2 = uint64(0x0a02)
	sigY1 = uint64(0x0b01)
	sigY2 = uint64(0x0b02)
)

func ap8(b []byte, x byte) []byte { return append(b, x) }
func ap16(b []byte, o binary.ByteOrder, x uint16) []byte {
	var t [2]byte
	o.PutUint16(t[:], x)
	return append(b, t[:]...)
}
func ap32(b []byte, o binary.ByteOrder, x uint32) []byte {
	var t [4]byte
	o.PutUint32(t[:], x)
	return append(b, t[:]...)
}
func ap64(b []byte, o binary.ByteOrder, x uint64) []byte {
	var t [8]byte
	o.PutUint64(t[:], x)
	return append(b, t[:]...)
}
func apStr(b []byte, s string) []byte { return append(b, []byte(s+"\x00")...) }

func is64Int(is64 bool) int {
	if is64 {
		return 8
	}
	return 4
}

// lengthPrefix is the size of the unit_length field itself.
func lengthPrefix(is64 bool) int {
	if is64 {
		return 12
	}
	return 4
}

// cuHeaderLen is the full DWARF 5 compile unit header length, including
// the unit_length field.
func cuHeaderLen(is64 bool) int {
	return lengthPrefix(is64) + 2 + 1 + 1 + is64Int(is64)
}

// tuHeaderLen is the full DWARF 5 type unit header length, including the
// unit_length field, the 8-byte signature and the type offset field.
func tuHeaderLen(is64 bool) int {
	return cuHeaderLen(is64) + 8 + is64Int(is64)
}

// wrapUnit prepends the DWARF 32/64-bit unit length field.
func wrapUnit(o binary.ByteOrder, is64 bool, body []byte) []byte {
	if is64 {
		b := []byte{0xff, 0xff, 0xff, 0xff}
		b = ap64(b, o, uint64(len(body)))
		return append(b, body...)
	}
	return append(ap32(nil, o, uint32(len(body))), body...)
}

// v5Header builds a DWARF 5 unit header body (everything after the length
// field). When isTU is set, signature and unit-relative type offset are
// appended.
func v5Header(o binary.ByteOrder, is64 bool, utype, asize uint8, sig, typeRel uint64, isTU bool) []byte {
	b := ap16(nil, o, 5)
	b = ap8(b, utype)
	b = ap8(b, asize)
	if is64 {
		b = ap64(b, o, 0) // debug_abbrev_offset
	} else {
		b = ap32(b, o, 0)
	}
	if isTU {
		b = ap64(b, o, sig)
		if is64 {
			b = ap64(b, o, typeRel)
		} else {
			b = ap32(b, o, uint32(typeRel))
		}
	}
	return b
}

func baseDIE(abbrevID byte, name string, enc byte, size byte) []byte {
	b := []byte{abbrevID}
	b = apStr(b, name)
	b = ap8(b, enc)
	b = ap8(b, size)
	return b
}

func sigTypedefDIE(abbrevID byte, name string, o binary.ByteOrder, sig uint64) []byte {
	b := []byte{abbrevID}
	b = apStr(b, name)
	b = ap64(b, o, sig)
	return b
}

// coordAbbrev is the shared abbreviation table for the coordinate tests.
var coordAbbrev = []byte{
	1, 0x11, 1, 0, 0, // compile unit, children
	2, 0x16, 0, 3, 0x08, 0x49, 0x20, 0, 0, // typedef: name string, type ref_sig8
	3, 0x24, 0, 3, 0x08, 0x3e, 0x0b, 0x0b, 0x0b, 0, 0, // base type
	4, 0x41, 1, 0, 0, // type unit root, children
	5, 0x16, 0, 3, 0x08, 0x49, 0x13, 0, 0, // typedef: type ref4
	6, 0x16, 0, 3, 0x08, 0x49, 0x10, 0, 0, // typedef: type ref_addr
	7, 0x16, 0, 3, 0x08, 0x49, 0x14, 0, 0, // typedef: type ref8
	0,
}

type coordSections struct {
	abbrev, info      []byte
	byteOff, aliasOff Offset
	intOff, longOff   Offset
	ref4Off, ref8Off  Offset
	refAddrOff        Offset
}

// buildCoord lays out TU(sigS0), a compile unit, then TU(sigS1) as the
// non-first type unit. Type units therefore appear both before and after a
// regular compile unit. All reported offsets are global info coordinates;
// header-relative values are derived where the format requires them.
func buildCoord(o binary.ByteOrder, is64 bool, asize uint8) coordSections {
	tuHdr := tuHeaderLen(is64)
	cuHdr := cuHeaderLen(is64)

	// Unit 0: type unit S0, primary base type "long".
	tu0Dies := []byte{4} // TagTypeUnit root
	tu0Dies = append(tu0Dies, baseDIE(3, "long", encSigned, 8)...)
	tu0Dies = append(tu0Dies, 0)
	tu0Head := v5Header(o, is64, utType, asize, sigS0, uint64(tuHdr+1), true)
	tu0 := wrapUnit(o, is64, append(tu0Head, tu0Dies...))
	longOff := Offset(tuHdr + 1)

	// Unit 1: compile unit.
	cuBase := Offset(len(tu0))
	cuDies := []byte{1} // compile unit root
	bytePos := len(cuDies)
	cuDies = append(cuDies, baseDIE(3, "byte", encSigned, 1)...)
	byteOff := cuBase + Offset(cuHdr+bytePos)

	// ref4 is unit-relative (counting the length prefix) and always
	// 4 bytes wide, even in DWARF64.
	ref4 := []byte{5}
	ref4 = apStr(ref4, "RefLocal")
	ref4 = ap32(ref4, o, uint32(byteOff-cuBase))
	ref4Off := cuBase + Offset(cuHdr+len(cuDies))
	cuDies = append(cuDies, ref4...)

	// ref8 uses the same unit-relative base, but is 8 bytes wide.
	ref8 := []byte{7}
	ref8 = apStr(ref8, "RefLocal8")
	ref8 = ap64(ref8, o, uint64(byteOff-cuBase))
	ref8Off := cuBase + Offset(cuHdr+len(cuDies))
	cuDies = append(cuDies, ref8...)

	// ref_addr is a coordinate over the whole info section.
	refAddr := []byte{6}
	refAddr = apStr(refAddr, "RefGlobal")
	if is64 {
		refAddr = ap64(refAddr, o, uint64(byteOff))
	} else {
		refAddr = ap32(refAddr, o, uint32(byteOff))
	}
	refAddrOff := cuBase + Offset(cuHdr+len(cuDies))
	cuDies = append(cuDies, refAddr...)

	// Signature reference into the following type unit.
	aliasOff := cuBase + Offset(cuHdr+len(cuDies))
	cuDies = append(cuDies, sigTypedefDIE(2, "Alias", o, sigS1)...)
	cuDies = append(cuDies, 0)
	cuHead := v5Header(o, is64, utCompile, asize, 0, 0, false)
	cu := wrapUnit(o, is64, append(cuHead, cuDies...))

	// Unit 2: type unit S1, primary base type "int" (non-first unit).
	tu1Dies := []byte{4}
	tu1Dies = append(tu1Dies, baseDIE(3, "int", encSigned, 4)...)
	tu1Dies = append(tu1Dies, 0)
	tu1Head := v5Header(o, is64, utType, asize, sigS1, uint64(tuHdr+1), true)
	tu1 := wrapUnit(o, is64, append(tu1Head, tu1Dies...))
	intOff := Offset(len(tu0) + len(cu) + tuHdr + 1)

	info := append(append(tu0, cu...), tu1...)
	return coordSections{
		abbrev: coordAbbrev, info: info,
		byteOff: byteOff, aliasOff: aliasOff, intOff: intOff, longOff: longOff,
		ref4Off: ref4Off, ref8Off: ref8Off, refAddrOff: refAddrOff,
	}
}

func TestTypeUnit5Coordinates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		variants := []struct {
			name  string
			order binary.ByteOrder
			is64  bool
			asize uint8
		}{
			{"dwarf32-le-a4", binary.LittleEndian, false, 4},
			{"dwarf32-be-a2", binary.BigEndian, false, 2},
			{"dwarf64-le-a8", binary.LittleEndian, true, 8},
			{"dwarf64-be-a4", binary.BigEndian, true, 4},
		}
		for _, v := range variants {
			s := buildCoord(v.order, v.is64, v.asize)

			d, err := New(s.abbrev, nil, nil, s.info, nil, nil, nil, nil)
			c.Check(err == nil, "%s: New failed: %v", v.name, err)
			if err != nil {
				continue
			}

			// Reader traversal order and untouched Field.Class /
			// ref_sig8 uint64 value.
			r := d.Reader()
			var tags []Tag
			var sawSigClass, sawSigVal bool
			for {
				e, err := r.Next()
				c.Check(err == nil, "%s: Reader.Next error: %v", v.name, err)
				if e == nil {
					break
				}
				tags = append(tags, e.Tag)
				if f := e.AttrField(AttrType); f != nil && f.Class == ClassReferenceSig {
					sawSigClass = true
					sig, ok := f.Val.(uint64)
					sawSigVal = ok && sig == sigS1
				}
			}
			wantTags := []Tag{
				TagTypeUnit, TagBaseType, 0, // TU0
				TagCompileUnit, TagBaseType, TagTypedef, TagTypedef, TagTypedef, TagTypedef, 0, // CU
				TagTypeUnit, TagBaseType, 0, // TU1 (non-first)
			}
			matched := len(tags) == len(wantTags)
			if matched {
				for i := range tags {
					if tags[i] != wantTags[i] {
						matched = false
					}
				}
			}
			c.Check(matched, "%s: traversal order %v want %v", v.name, tags, wantTags)
			c.Check(sawSigClass, "%s: ref_sig8 Field.Class was not ClassReferenceSig", v.name)
			c.Check(sawSigVal, "%s: ref_sig8 uint64 value was altered or lost", v.name)

			// Direct read at the global DIE offset, then signature
			// read: same Go type object.
			direct, err := d.Type(s.intOff)
			c.Check(err == nil, "%s: Type(intOff) error: %v", v.name, err)
			c.Check(direct != nil && direct.Common().Name == "int" && direct.Size() == 4,
				"%s: direct TU type = %v size %d", v.name, nameOf(direct), sizeOf(direct))
			viaSig, err := d.Type(s.aliasOff)
			c.Check(err == nil, "%s: Type(alias) error: %v", v.name, err)
			td, ok := viaSig.(*TypedefType)
			c.Check(ok, "%s: alias is %T, want *TypedefType", v.name, viaSig)
			if ok {
				c.Check(td.Type == direct, "%s: signature type and global-offset type are different objects", v.name)
			}

			// Reverse entry order on a second, identical Data.
			d2, err := New(s.abbrev, nil, nil, s.info, nil, nil, nil, nil)
			c.Check(err == nil, "%s: second New failed: %v", v.name, err)
			sigFirst, err := d2.Type(s.aliasOff)
			c.Check(err == nil, "%s: sig-first Type error: %v", v.name, err)
			direct2, err := d2.Type(s.intOff)
			c.Check(err == nil, "%s: direct-second Type error: %v", v.name, err)
			if td2, ok := sigFirst.(*TypedefType); ok {
				c.Check(td2.Type == direct2, "%s: sig-first identity broken: %p vs %p", v.name, td2.Type, direct2)
			}

			// Unit-local references follow their own unit;
			// ref_addr is an info-wide coordinate.
			tLocal, err := d.Type(s.ref4Off)
			c.Check(err == nil, "%s: ref4 typedef error: %v", v.name, err)
			if ltd, ok := tLocal.(*TypedefType); ok {
				c.Check(ltd.Type.Common().Name == "byte", "%s: ref4 resolved to %q", v.name, ltd.Type.Common().Name)
			}
			tLocal8, err := d.Type(s.ref8Off)
			c.Check(err == nil, "%s: ref8 typedef error: %v", v.name, err)
			if l8, ok := tLocal8.(*TypedefType); ok {
				c.Check(l8.Type.Common().Name == "byte", "%s: ref8 resolved to %q", v.name, l8.Type.Common().Name)
			}
			tGlobal, err := d.Type(s.refAddrOff)
			c.Check(err == nil, "%s: ref_addr typedef error: %v", v.name, err)
			if gtd, ok := tGlobal.(*TypedefType); ok {
				want, _ := d.Type(s.byteOff)
				c.Check(gtd.Type == want, "%s: ref_addr did not resolve to the global DIE object", v.name)
			}

			// The preceding type unit is reachable both ways too.
			longDirect, err := d.Type(s.longOff)
			c.Check(err == nil, "%s: preceding TU direct read error: %v", v.name, err)
			c.Check(nameOf(longDirect) == "long" && sizeOf(longDirect) == 8,
				"%s: preceding TU type = %q size %d", v.name, nameOf(longDirect), sizeOf(longDirect))

			// An unknown signature must report an error.
			bogus := buildUnknownSig(v.order, v.is64, v.asize)
			db, err := New(s.abbrev, nil, nil, bogus, nil, nil, nil, nil)
			c.Check(err == nil, "%s: New(unknown sig) error: %v", v.name, err)
			if err == nil {
				_, err = db.Type(Offset(cuHeaderLen(v.is64) + 1))
				c.Check(err != nil, "%s: unknown signature was accepted: %v", v.name, err)
			}
		}

		// The first unit's type offset lands past its own end, on the
		// neighboring unit: it must be rejected rather than treating the
		// neighbor's DIE as the target.
		o := binary.LittleEndian
		goodDies := []byte{4}
		goodDies = append(goodDies, baseDIE(3, "long", encSigned, 8)...)
		goodDies = append(goodDies, 0)
		goodHead := v5Header(o, false, utType, 8, sigS0, uint64(tuHeaderLen(false)+1), true)
		neighbor := wrapUnit(o, false, append(goodHead, goodDies...))

		badDies := []byte{4, 0}
		badHead := v5Header(o, false, utType, 8, sigS1, uint64(len(neighbor)+1), true)
		bad := wrapUnit(o, false, append(badHead, badDies...))
		info := append(bad, neighbor...)
		_, err := New(coordAbbrev, nil, nil, info, nil, nil, nil, nil)
		c.Check(err != nil, "out-of-unit type_offset was accepted: %v", err)
	})
}

// buildUnknownSig emits one compile unit whose typedef names a signature
// that no type unit defines.
func buildUnknownSig(o binary.ByteOrder, is64 bool, asize uint8) []byte {
	head := v5Header(o, is64, utCompile, asize, 0, 0, false)
	dies := []byte{1}
	dies = append(dies, sigTypedefDIE(2, "Bogus", o, 0xdeadbeef)...)
	dies = append(dies, 0)
	return wrapUnit(o, is64, append(head, dies...))
}

// --- recursive, shared-identity type units --------------------------------

// Abbreviation table:
//
//	1 compile unit (children), 2 sig typedef, 3 base type, 4 type unit root,
//	8 structure (name, byte size, children), 9 member (name, ref4 type, loc),
//	10 pointer (ref_sig8 type), 11 member (name, ref_sig8 type, loc).
var recurAbbrev = []byte{
	1, 0x11, 1, 0, 0,
	2, 0x16, 0, 3, 0x08, 0x49, 0x20, 0, 0,
	3, 0x24, 0, 3, 0x08, 0x3e, 0x0b, 0x0b, 0x0b, 0, 0,
	4, 0x41, 1, 0, 0,
	8, 0x13, 1, 3, 0x08, 0x0b, 0x0b, 0, 0,
	9, 0x0d, 0, 3, 0x08, 0x49, 0x13, 0x38, 0x0b, 0, 0,
	10, 0x0f, 0, 0x49, 0x20, 0, 0,
	11, 0x0d, 0, 3, 0x08, 0x49, 0x20, 0x38, 0x0b, 0, 0,
	0,
}

type recurSections struct {
	abbrev, info             []byte
	aTypedefOff, bTypedefOff Offset
	aStructOff, bStructOff   Offset
	cBase                    Offset
}

// buildRecur creates TU_A (address size 8) and TU_B (address size 4) that
// point at each other through pointer members and signatures, plus TU_C
// holding the shared "int" base type.
func buildRecur(o binary.ByteOrder, is64 bool) recurSections {
	tuHdr := tuHeaderLen(is64)
	cuHdr := cuHeaderLen(is64)

	makeTU := func(asize uint8, sig uint64, dies []byte, rootPos int) []byte {
		head := v5Header(o, is64, utType, asize, sig, uint64(tuHdr+rootPos), true)
		return wrapUnit(o, is64, append(head, dies...))
	}

	// TU_A: struct NodeA { int payload; NodeB *next; }
	tuADies := []byte{4} // type unit root
	structPos := 1
	tuADies = append(tuADies, 8) // structure
	tuADies = apStr(tuADies, "NodeA")
	tuADies = ap8(tuADies, 16) // byte size
	ptrPos := len(tuADies)
	tuADies = append(tuADies, 10) // pointer -> sig B
	tuADies = ap64(tuADies, o, sigB)
	tuADies = append(tuADies, 11) // member payload -> sig C
	tuADies = apStr(tuADies, "payload")
	tuADies = ap64(tuADies, o, sigC)
	tuADies = ap8(tuADies, 0)    // offset 0
	tuADies = append(tuADies, 9) // member next -> local pointer DIE
	tuADies = apStr(tuADies, "next")
	tuADies = ap32(tuADies, o, uint32(tuHdr+ptrPos)) // ref4: unit-relative
	tuADies = ap8(tuADies, 8)
	tuADies = append(tuADies, 0, 0) // end struct, end type unit
	tuA := makeTU(8, sigA, tuADies, structPos)

	// TU_B: struct NodeB { NodeA *back; int marker; }
	tuBDies := []byte{4}
	structBPos := 1
	tuBDies = append(tuBDies, 8)
	tuBDies = apStr(tuBDies, "NodeB")
	tuBDies = ap8(tuBDies, 8)
	ptrBPos := len(tuBDies)
	tuBDies = append(tuBDies, 10) // pointer -> sig A
	tuBDies = ap64(tuBDies, o, sigA)
	tuBDies = append(tuBDies, 9) // member back -> local pointer DIE
	tuBDies = apStr(tuBDies, "back")
	tuBDies = ap32(tuBDies, o, uint32(tuHdr+ptrBPos))
	tuBDies = ap8(tuBDies, 0)
	tuBDies = append(tuBDies, 11) // member marker -> sig C
	tuBDies = apStr(tuBDies, "marker")
	tuBDies = ap64(tuBDies, o, sigC)
	tuBDies = ap8(tuBDies, 4)
	tuBDies = append(tuBDies, 0, 0)
	tuB := makeTU(4, sigB, tuBDies, structBPos)

	// TU_C: shared base type "int".
	tuCDies := []byte{4}
	cPos := 1
	tuCDies = append(tuCDies, baseDIE(3, "int", encSigned, 4)...)
	tuCDies = append(tuCDies, 0)
	tuC := makeTU(8, sigC, tuCDies, cPos)

	// Compile unit with the two named entry points.
	cuHead := v5Header(o, is64, utCompile, 8, 0, 0, false)
	cuDies := []byte{1}
	aPos := len(cuDies)
	cuDies = append(cuDies, sigTypedefDIE(2, "TA", o, sigA)...)
	bPos := len(cuDies)
	cuDies = append(cuDies, sigTypedefDIE(2, "TB", o, sigB)...)
	cuDies = append(cuDies, 0)
	cu := wrapUnit(o, is64, append(cuHead, cuDies...))

	info := append(append(tuA, tuB...), tuC...)
	cuBase := Offset(len(info))
	info = append(info, cu...)

	return recurSections{
		abbrev: recurAbbrev, info: info,
		aTypedefOff: cuBase + Offset(cuHdr+aPos),
		bTypedefOff: cuBase + Offset(cuHdr+bPos),
		aStructOff:  Offset(tuHdr + structPos),
		bStructOff:  Offset(len(tuA) + tuHdr + structBPos),
		cBase:       Offset(len(tuA) + len(tuB) + tuHdr + cPos),
	}
}

func TestTypeUnit5RecursiveIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		o := binary.LittleEndian
		assertGraph := func(label string, s recurSections, firstB bool) {
			d, err := New(s.abbrev, nil, nil, s.info, nil, nil, nil, nil)
			c.Check(err == nil, "%s: New error: %v", label, err)
			if err != nil {
				return
			}
			// Choose which signature is resolved first.
			if firstB {
				_, _ = d.Type(s.bTypedefOff)
			} else {
				_, _ = d.Type(s.aTypedefOff)
			}
			ta, err := d.Type(s.aTypedefOff)
			c.Check(err == nil, "%s: Type(A) error: %v", label, err)
			tb, err := d.Type(s.bTypedefOff)
			c.Check(err == nil, "%s: Type(B) error: %v", label, err)
			sa, ok1 := ta.(*TypedefType)
			sb, ok2 := tb.(*TypedefType)
			c.Check(ok1 && ok2, "%s: entry types %T %T", label, ta, tb)
			if !ok1 || !ok2 {
				return
			}
			nodeA, ok1 := sa.Type.(*StructType)
			nodeB, ok2 := sb.Type.(*StructType)
			c.Check(ok1 && ok2, "%s: struct types %T %T", label, sa.Type, sb.Type)
			if !ok1 || !ok2 {
				return
			}

			// Member order is preserved.
			c.Check(len(nodeA.Field) == 2 && nodeA.Field[0].Name == "payload" && nodeA.Field[1].Name == "next",
				"%s: NodeA fields = %v", label, fieldNames(nodeA))
			c.Check(len(nodeB.Field) == 2 && nodeB.Field[0].Name == "back" && nodeB.Field[1].Name == "marker",
				"%s: NodeB fields = %v", label, fieldNames(nodeB))

			// Pointer widths come from the unit that owns the
			// pointer DIE: 8 in TU_A, 4 in TU_B.
			nextPtr, ok := nodeA.Field[1].Type.(*PtrType)
			c.Check(ok, "%s: next is %T", label, nodeA.Field[1].Type)
			backPtr, ok := nodeB.Field[0].Type.(*PtrType)
			c.Check(ok, "%s: back is %T", label, nodeB.Field[0].Type)
			if ok {
				c.Check(nextPtr.Size() == 8, "%s: next pointer size = %d, want 8", label, nextPtr.Size())
				c.Check(backPtr.Size() == 4, "%s: back pointer size = %d, want 4", label, backPtr.Size())
				c.Check(nextPtr.Type == nodeB, "%s: next does not identity-match NodeB", label)
				c.Check(backPtr.Type == nodeA, "%s: back does not identity-match NodeA", label)
			}

			// The shared third type keeps one identity across
			// both signature references and a global read.
			shared, err := d.Type(s.cBase)
			c.Check(err == nil, "%s: Type(C) error: %v", label, err)
			c.Check(nodeA.Field[0].Type == shared, "%s: payload is not the shared int object", label)
			c.Check(nodeB.Field[1].Type == shared, "%s: marker is not the shared int object", label)
			c.Check(shared.Common().Name == "int" && shared.Size() == 4,
				"%s: shared type = %q size %d", label, nameOf(shared), sizeOf(shared))

			// Direct global-offset reads return the same objects
			// as signature reads, and sizes are all finalized.
			directA, err := d.Type(s.aStructOff)
			c.Check(err == nil, "%s: direct NodeA error: %v", label, err)
			directB, err := d.Type(s.bStructOff)
			c.Check(err == nil, "%s: direct NodeB error: %v", label, err)
			c.Check(directA == nodeA, "%s: NodeA identity split", label)
			c.Check(directB == nodeB, "%s: NodeB identity split", label)
			c.Check(nodeA.Size() == 16 && nodeB.Size() == 8,
				"%s: struct sizes %d %d", label, nodeA.Size(), nodeB.Size())
		}
		for _, is64 := range []bool{false, true} {
			label := "dwarf32"
			if is64 {
				label = "dwarf64"
			}
			s := buildRecur(o, is64)
			assertGraph(label+"/a-first", s, false)
			assertGraph(label+"/b-first", s, true)
		}
	})
}

// --- DWARF 4 .debug_types isolation ---------------------------------------

var legacyAbbrev = []byte{
	1, 0x11, 0, 0, 0, // compile unit without children
	2, 0x16, 0, 3, 0x08, 0x49, 0x20, 0, 0, // sig typedef
	3, 0x24, 0, 3, 0x08, 0x3e, 0x0b, 0x0b, 0x0b, 0, 0, // base type
	0,
}

// v4CUHeaderLen is the full DWARF 4 compile unit header length, including
// the 4-byte unit length (ver 2 + abbrev off 4 + address size 1 = 7).
const v4CUHeaderLen = 4 + 2 + 4 + 1

func v4TypesUnit(o binary.ByteOrder, asize uint8, sig uint64, name string, size byte) []byte {
	dies := baseDIE(3, name, encSigned, size)
	dies = append(dies, 0)
	// Post-length header: ver(2) + abbrev off(4) + asize(1) + sig(8) +
	// type offset(4) = 19 bytes.
	headLen := 2 + 4 + 1 + 8 + 4
	typeRel := uint32(4 + headLen) // unit-relative, includes length prefix
	head := ap16(nil, o, 4)
	head = ap32(head, o, 0)
	head = ap8(head, asize)
	head = ap64(head, o, sig)
	head = ap32(head, o, typeRel)
	return wrapUnit(o, false, append(head, dies...))
}

func TestTypeUnit5LegacyIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		o := binary.LittleEndian

		// Two standalone sections, each with two units sharing the same
		// local coordinates but carrying distinct types and signatures.
		secX := append(
			v4TypesUnit(o, 8, sigX1, "x1", 2),
			v4TypesUnit(o, 8, sigX2, "x2", 6)...)
		secY := append(
			v4TypesUnit(o, 4, sigY1, "y1", 1),
			v4TypesUnit(o, 4, sigY2, "y3", 3)...)

		// DWARF 4 compile unit plus four signature typedefs.
		cuHead := ap16(nil, o, 4)
		cuHead = ap32(cuHead, o, 0)
		cuHead = ap8(cuHead, 8)
		cuDies := []byte{1}
		entryOffsets := map[uint64]Offset{}
		for _, spec := range []struct {
			name string
			sig  uint64
		}{
			{"TX1", sigX1}, {"TX2", sigX2}, {"TY1", sigY1}, {"TY2", sigY2},
		} {
			entryOffsets[spec.sig] = Offset(v4CUHeaderLen + len(cuDies))
			cuDies = append(cuDies, sigTypedefDIE(2, spec.name, o, spec.sig)...)
		}
		info := wrapUnit(o, false, append(cuHead, cuDies...))

		d, err := New(legacyAbbrev, nil, nil, info, nil, nil, nil, nil)
		c.Check(err == nil, "New error: %v", err)
		if err != nil {
			return
		}
		c.Check(d.AddTypes(".debug_types/x", secX) == nil, "AddTypes x failed")
		c.Check(d.AddTypes(".debug_types/y", secY) == nil, "AddTypes y failed")

		want := map[uint64]struct {
			name string
			size int64
		}{
			sigX1: {"x1", 2}, sigX2: {"x2", 6},
			sigY1: {"y1", 1}, sigY2: {"y3", 3},
		}
		for sig, exp := range want {
			tp, err := d.Type(entryOffsets[sig])
			c.Check(err == nil, "sig %x Type error: %v", sig, err)
			td, ok := tp.(*TypedefType)
			c.Check(ok, "sig %x gave %T", sig, tp)
			if ok {
				c.Check(td.Type.Common().Name == exp.name && td.Type.Size() == exp.size,
					"sig %x = %q size %d, want %q size %d",
					sig, td.Type.Common().Name, td.Type.Size(), exp.name, exp.size)
			}
		}

		// An info DIE sitting at the same numeric offset as a types
		// section local offset must resolve through info, never through
		// the type unit cache.
		infoType, err := d.Type(entryOffsets[sigX1])
		c.Check(err == nil, "info typedef read error: %v", err)
		_, isTypedef := infoType.(*TypedefType)
		c.Check(isTypedef, "info offset resolved to a types-section object: %T", infoType)

		// Type offset outside the unit must be rejected instead of
		// landing on the neighboring unit's DIE.
		bad := v4TypesUnit(o, 8, sigX1, "x1", 2)
		// Corrupt the 4-byte type offset field (global bytes 19..22).
		for i := 19; i < 23; i++ {
			bad[i] = 0xff
		}
		bad = append(bad, v4TypesUnit(o, 8, sigX2, "x2", 6)...)
		d2, err := New(legacyAbbrev, nil, nil, info, nil, nil, nil, nil)
		c.Check(err == nil, "second New error: %v", err)
		if err == nil {
			err = d2.AddTypes(".debug_types/bad", bad)
			c.Check(err != nil, "out-of-range legacy type offset accepted: %v", err)
		}
	})
}

func nameOf(t Type) string {
	if t == nil {
		return "<nil>"
	}
	return t.Common().Name
}

func sizeOf(t Type) int64 {
	if t == nil {
		return -1
	}
	return t.Size()
}

func fieldNames(t *StructType) []string {
	names := make([]string, len(t.Field))
	for i, f := range t.Field {
		names[i] = f.Name
	}
	return names
}
