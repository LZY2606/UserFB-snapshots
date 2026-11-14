// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Regression tests for DWARF 5 type units (DW_UT_type) embedded in
// .debug_info: signature lookup, offset coordinates, type identity
// across queries, mutually recursive type units, and isolation from
// DWARF 4 .debug_types sections.

package dwarf

import (
	"encoding/binary"
	"reflect"
	"strings"
	"testing"

	"example.org/offline-dwarf/task_support/fixture"
	"example.org/offline-dwarf/task_support/taskcheck"
)

// Abbreviation codes shared by every fixture in this file. The table
// below lives at offset 0 of the synthetic .debug_abbrev section.
const (
	tu5AbCompile = iota + 1
	tu5AbBase
	tu5AbTypedefSig
	tu5AbTypeUnit
	tu5AbStruct
	tu5AbMemberRef
	tu5AbPtrSig
	tu5AbPtrRef
	tu5AbTypedefRef
	tu5AbTypedefAddr
)

// tu5Abbrev is a .debug_abbrev section. Tags, attributes and forms are
// written as raw ULEB128 (all values fit in one byte).
var tu5Abbrev = []byte{
	// 1: DW_TAG_compile_unit, children, no attributes
	tu5AbCompile, 0x11, 1, 0, 0,
	// 2: DW_TAG_base_type: name(string), encoding(data1), byte_size(data1)
	tu5AbBase, 0x24, 0, 0x03, 0x08, 0x3e, 0x0b, 0x0b, 0x0b, 0, 0,
	// 3: DW_TAG_typedef: name(string), type(ref_sig8)
	tu5AbTypedefSig, 0x16, 0, 0x03, 0x08, 0x49, 0x20, 0, 0,
	// 4: DW_TAG_type_unit, children, no attributes
	tu5AbTypeUnit, 0x41, 1, 0, 0,
	// 5: DW_TAG_structure_type: name(string), byte_size(data2)
	tu5AbStruct, 0x13, 1, 0x03, 0x08, 0x0b, 0x05, 0, 0,
	// 6: DW_TAG_member: name(string), type(ref4), data_member_location(data1)
	tu5AbMemberRef, 0x0d, 0, 0x03, 0x08, 0x49, 0x13, 0x38, 0x0b, 0, 0,
	// 7: DW_TAG_pointer_type: type(ref_sig8)
	tu5AbPtrSig, 0x0f, 0, 0x49, 0x20, 0, 0,
	// 8: DW_TAG_pointer_type: type(ref4)
	tu5AbPtrRef, 0x0f, 0, 0x49, 0x13, 0, 0,
	// 9: DW_TAG_typedef: name(string), type(ref4)
	tu5AbTypedefRef, 0x16, 0, 0x03, 0x08, 0x49, 0x13, 0, 0,
	// 10: DW_TAG_typedef: name(string), type(ref_addr)
	tu5AbTypedefAddr, 0x16, 0, 0x03, 0x08, 0x49, 0x10, 0, 0,
	0,
}

// tu5Unit describes one unit of a synthetic .debug_info section.
type tu5Unit struct {
	vers  int    // DWARF version: 4 or 5
	utype byte   // DWARF 5 unit type; unused for version 4
	asize byte   // address size
	sig   uint64 // type signature (type units only)
	toff  int    // offset of primary type DIE within dies (type units only)
	dies  []byte // encoded DIE stream

	start Offset // offset of dies within the assembled info
}

// tu5LenField is the size of the unit_length field.
func tu5LenField(is64 bool) int {
	if is64 {
		return 12 // 0xffffffff escape + 8-byte length
	}
	return 4
}

// tu5HdrLen is the size of a unit header after the unit_length field.
func tu5HdrLen(vers int, utype byte, is64 bool) int {
	n := 2 // version
	if vers >= 5 {
		n += 2 // unit type + address size
	}
	if is64 {
		n += 8 // abbrev offset
	} else {
		n += 4
	}
	if vers < 5 {
		n++ // address size
	}
	if vers >= 5 && utype == utType {
		n += 8 // type signature
		if is64 {
			n += 8 // type offset
		} else {
			n += 4
		}
	}
	return n
}

// tu5Build assembles units into a .debug_info section and records each
// unit's DIE stream offset in u.start.
func tu5Build(order binary.ByteOrder, is64 bool, units ...*tu5Unit) []byte {
	var info []byte
	off := 0
	for _, u := range units {
		hdrLen := tu5HdrLen(u.vers, u.utype, is64)
		u.start = Offset(off + tu5LenField(is64) + hdrLen)
		var hdr []byte
		if is64 {
			hdr = append(hdr, 0xff, 0xff, 0xff, 0xff)
			hdr = tu5Append64(hdr, order, uint64(hdrLen+len(u.dies)))
		} else {
			hdr = tu5Append32(hdr, order, uint32(hdrLen+len(u.dies)))
		}
		hdr = tu5Append16(hdr, order, uint16(u.vers))
		if u.vers >= 5 {
			hdr = append(hdr, u.utype, u.asize)
		}
		if is64 {
			hdr = tu5Append64(hdr, order, 0) // abbrev offset
		} else {
			hdr = tu5Append32(hdr, order, 0)
		}
		if u.vers < 5 {
			hdr = append(hdr, u.asize)
		}
		if u.vers >= 5 && u.utype == utType {
			hdr = tu5Append64(hdr, order, u.sig)
			// The type offset is relative to the start of the unit header.
			toff := uint64(tu5LenField(is64) + hdrLen + u.toff)
			if is64 {
				hdr = tu5Append64(hdr, order, toff)
			} else {
				hdr = tu5Append32(hdr, order, uint32(toff))
			}
		}
		info = append(info, hdr...)
		info = append(info, u.dies...)
		off += len(hdr) + len(u.dies)
	}
	return info
}

// tu5TypesSection assembles one DWARF 4 .debug_types type unit.
func tu5TypesSection(order binary.ByteOrder, asize byte, sig uint64, toff uint32, dies []byte) []byte {
	var sec []byte
	sec = tu5Append32(sec, order, uint32(2+4+1+8+4+len(dies)))
	sec = tu5Append16(sec, order, 4)
	sec = tu5Append32(sec, order, 0) // abbrev offset
	sec = append(sec, asize)
	sec = tu5Append64(sec, order, sig)
	sec = tu5Append32(sec, order, toff)
	return append(sec, dies...)
}

func tu5Append16(b []byte, o binary.ByteOrder, v uint16) []byte {
	var t [2]byte
	o.PutUint16(t[:], v)
	return append(b, t[:]...)
}

func tu5Append32(b []byte, o binary.ByteOrder, v uint32) []byte {
	var t [4]byte
	o.PutUint32(t[:], v)
	return append(b, t[:]...)
}

func tu5Append64(b []byte, o binary.ByteOrder, v uint64) []byte {
	var t [8]byte
	o.PutUint64(t[:], v)
	return append(b, t[:]...)
}

func tu5Join(parts ...[]byte) []byte {
	var out []byte
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

// DIE encoders. Reference fields that need layout-dependent values are
// patched by the caller.

func tu5Base(name string, enc, size byte) []byte {
	d := []byte{tu5AbBase}
	d = append(d, name...)
	return append(d, 0, enc, size)
}

func tu5TypedefSig(o binary.ByteOrder, name string, sig uint64) []byte {
	d := []byte{tu5AbTypedefSig}
	d = append(d, name...)
	d = append(d, 0)
	return tu5Append64(d, o, sig)
}

func tu5TypedefRef(o binary.ByteOrder, name string, ref4 uint32) []byte {
	d := []byte{tu5AbTypedefRef}
	d = append(d, name...)
	d = append(d, 0)
	return tu5Append32(d, o, ref4)
}

func tu5TypedefAddr(o binary.ByteOrder, is64 bool, name string, addr uint64) []byte {
	d := []byte{tu5AbTypedefAddr}
	d = append(d, name...)
	d = append(d, 0)
	if is64 {
		return tu5Append64(d, o, addr)
	}
	return tu5Append32(d, o, uint32(addr))
}

func tu5Struct(o binary.ByteOrder, name string, size uint16, kids ...[]byte) []byte {
	d := []byte{tu5AbStruct}
	d = append(d, name...)
	d = append(d, 0)
	d = tu5Append16(d, o, size)
	for _, k := range kids {
		d = append(d, k...)
	}
	return append(d, 0)
}

func tu5Member(o binary.ByteOrder, name string, ref4 uint32, loc byte) []byte {
	d := []byte{tu5AbMemberRef}
	d = append(d, name...)
	d = append(d, 0)
	d = tu5Append32(d, o, ref4)
	return append(d, loc)
}

func tu5PtrSig(o binary.ByteOrder, sig uint64) []byte {
	return tu5Append64([]byte{tu5AbPtrSig}, o, sig)
}

func tu5PtrRef(o binary.ByteOrder, ref4 uint32) []byte {
	return tu5Append32([]byte{tu5AbPtrRef}, o, ref4)
}

// tu5MustType reads a type or records a failed check.
func tu5MustType(c *taskcheck.C, d *Data, off Offset) Type {
	typ, err := d.Type(off)
	if err != nil {
		c.Check(false, "Type(%d): %v", off, err)
		return nil
	}
	return typ
}

// TestTypeUnit5Coordinates checks that DWARF 5 type units in
// .debug_info keep the section's coordinate rules: type units may sit
// before or after compile units, type_offset is relative to its own
// unit header, ref4 stays unit-relative, ref_addr stays info-wide, and
// the Reader's traversal order and field classes are unchanged.
func TestTypeUnit5Coordinates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			sigEarly   = 0x1111111111111111
			sigLate    = 0x2222222222222222
			sigMissing = 0x3333333333333333
			sigBad     = 0x4444444444444444
			sigWord    = 0x5555555555555555
		)
		le := binary.LittleEndian

		// Type unit before the compile unit.
		tuPre := &tu5Unit{vers: 5, utype: utType, asize: 8, sig: sigEarly, toff: 1}
		tuPre.dies = tu5Join(
			[]byte{tu5AbTypeUnit},
			tu5Base("early", encUnsigned, 2),
			[]byte{0},
		)

		// Compile unit in the middle.
		dCU := []byte{tu5AbCompile}
		dTdEarly := tu5TypedefSig(le, "Early", sigEarly)
		dTdLate := tu5TypedefSig(le, "Late", sigLate)
		dTdAddr := tu5TypedefAddr(le, false, "ViaAddr", 0) // patched after layout
		dTdLocal := tu5TypedefRef(le, "Local", 0)          // patched below
		dTdMissing := tu5TypedefSig(le, "Missing", sigMissing)
		dInt := tu5Base("int", encSigned, 4)
		tdEarlyRel := len(dCU)
		tdLateRel := tdEarlyRel + len(dTdEarly)
		tdAddrRel := tdLateRel + len(dTdLate)
		tdLocalRel := tdAddrRel + len(dTdAddr)
		tdMissingRel := tdLocalRel + len(dTdLocal)
		cuIntRel := tdMissingRel + len(dTdMissing)
		cuHdr := uint32(tu5LenField(false) + tu5HdrLen(5, utCompile, false))
		le.PutUint32(dTdLocal[len(dTdLocal)-4:], cuHdr+uint32(cuIntRel))
		cu := &tu5Unit{vers: 5, utype: utCompile, asize: 8}
		cu.dies = tu5Join(dCU, dTdEarly, dTdLate, dTdAddr, dTdLocal, dTdMissing, dInt, []byte{0})

		// Type unit after the compile unit.
		tuPost := &tu5Unit{vers: 5, utype: utType, asize: 8, sig: sigLate, toff: 1}
		tuPost.dies = tu5Join(
			[]byte{tu5AbTypeUnit},
			tu5Base("late", encSigned, 8),
			[]byte{0},
		)

		info := tu5Build(le, false, tuPre, cu, tuPost)

		// ref_addr is an info-wide coordinate, only known after layout.
		preTypeOff := tuPre.start + 1
		postTypeOff := tuPost.start + 1
		tdEarlyOff := cu.start + Offset(tdEarlyRel)
		tdLateOff := cu.start + Offset(tdLateRel)
		tdAddrOff := cu.start + Offset(tdAddrRel)
		tdLocalOff := cu.start + Offset(tdLocalRel)
		tdMissingOff := cu.start + Offset(tdMissingRel)
		cuIntOff := cu.start + Offset(cuIntRel)
		le.PutUint32(info[int(cu.start)+tdAddrRel+len(dTdAddr)-4:], uint32(postTypeOff))

		d, err := New(tu5Abbrev, nil, nil, info, nil, nil, nil, nil)
		if err != nil {
			c.Check(false, "New: %v", err)
			return
		}

		// Traversal order is unchanged and covers every unit.
		var tags []Tag
		r := d.Reader()
		for {
			e, err := r.Next()
			if err != nil {
				c.Check(false, "Reader.Next: %v", err)
				return
			}
			if e == nil {
				break
			}
			tags = append(tags, e.Tag)
		}
		wantTags := []Tag{
			TagTypeUnit, TagBaseType, 0,
			TagCompileUnit, TagTypedef, TagTypedef, TagTypedef, TagTypedef, TagTypedef, TagBaseType, 0,
			TagTypeUnit, TagBaseType, 0,
		}
		c.Check(reflect.DeepEqual(tags, wantTags), "traversal tags = %v, want %v", tags, wantTags)

		// ref_sig8 keeps its class and raw uint64 value.
		r.Seek(tdEarlyOff)
		e, err := r.Next()
		if err != nil || e == nil {
			c.Check(false, "seek typedef Early: %v", err)
			return
		}
		f := e.AttrField(AttrType)
		c.Check(f != nil && f.Class == ClassReferenceSig, "typedef Early AttrType class = %v", f)
		c.Check(f != nil && f.Val == uint64(sigEarly), "typedef Early AttrType val = %#v", f)

		// ref_addr keeps its class and info-wide value.
		r.Seek(tdAddrOff)
		e, err = r.Next()
		if err != nil || e == nil {
			c.Check(false, "seek typedef ViaAddr: %v", err)
			return
		}
		f = e.AttrField(AttrType)
		c.Check(f != nil && f.Class == ClassReference && f.Val == postTypeOff,
			"typedef ViaAddr AttrType = %v %#v, want ClassReference %#x", f, f.Val, postTypeOff)

		// ref4 follows its own unit.
		r.Seek(tdLocalOff)
		e, err = r.Next()
		if err != nil || e == nil {
			c.Check(false, "seek typedef Local: %v", err)
			return
		}
		f = e.AttrField(AttrType)
		c.Check(f != nil && f.Class == ClassReference && f.Val == cuIntOff,
			"typedef Local AttrType = %v %#v, want ClassReference %#x", f, f.Val, cuIntOff)

		// The type before the CU: same object by offset and by
		// signature, in either query order.
		tEarly := tu5MustType(c, d, preTypeOff)
		viaEarly := tu5MustType(c, d, tdEarlyOff)
		if tEarly == nil || viaEarly == nil {
			return
		}
		c.Check(tEarly.Common().Name == "early" && tEarly.Size() == 2,
			"early = %v size %d, want early/2", tEarly, tEarly.Size())
		if td, ok := viaEarly.(*TypedefType); ok {
			c.Check(td.Type == tEarly, "type unit before CU: signature and offset types differ")
		} else {
			c.Check(false, "typedef Early is %T", viaEarly)
		}
		d2, err := New(tu5Abbrev, nil, nil, info, nil, nil, nil, nil)
		if err != nil {
			c.Check(false, "New (reverse): %v", err)
			return
		}
		viaEarly2 := tu5MustType(c, d2, tdEarlyOff)
		tEarly2 := tu5MustType(c, d2, preTypeOff)
		if td, ok := viaEarly2.(*TypedefType); ok && tEarly2 != nil {
			c.Check(td.Type == tEarly2, "reversed query order: signature and offset types differ")
		} else if !ok {
			c.Check(false, "typedef Early is %T", viaEarly2)
		}

		// The type after the CU resolves the same way.
		tLate := tu5MustType(c, d, postTypeOff)
		viaLate := tu5MustType(c, d, tdLateOff)
		if tLate == nil || viaLate == nil {
			return
		}
		c.Check(tLate.Common().Name == "late" && tLate.Size() == 8,
			"late = %v size %d, want late/8", tLate, tLate.Size())
		if td, ok := viaLate.(*TypedefType); ok {
			c.Check(td.Type == tLate, "type unit after CU: signature and offset types differ")
		} else {
			c.Check(false, "typedef Late is %T", viaLate)
		}

		// ref_addr reaches the type unit's DIE by info-wide offset.
		viaAddr := tu5MustType(c, d, tdAddrOff)
		if td, ok := viaAddr.(*TypedefType); ok {
			c.Check(td.Type == tLate, "ref_addr typedef did not reach the type unit type")
		} else {
			c.Check(false, "typedef ViaAddr is %T", viaAddr)
		}

		// ref4 stays inside its own unit.
		viaLocal := tu5MustType(c, d, tdLocalOff)
		tInt := tu5MustType(c, d, cuIntOff)
		if td, ok := viaLocal.(*TypedefType); ok && tInt != nil {
			c.Check(td.Type == tInt, "ref4 typedef left its own unit")
			c.Check(tInt.Common().Name == "int" && tInt.Size() == 4,
				"int = %v size %d", tInt, tInt.Size())
		} else if !ok {
			c.Check(false, "typedef Local is %T", viaLocal)
		}

		// An unknown signature is an error.
		_, err = d.Type(tdMissingOff)
		c.Check(err != nil && strings.Contains(err.Error(), "no type unit with signature"),
			"missing signature error = %v", err)

		// The provided fixture agrees: signature and offset give one type.
		s := fixture.V5()
		df, err := New(s.Abbrev, nil, nil, s.Info, nil, nil, nil, nil)
		if err != nil {
			c.Check(false, "New fixture.V5: %v", err)
		} else {
			direct := tu5MustType(c, df, Offset(s.TypeOffset))
			alias := tu5MustType(c, df, Offset(s.AliasOffset))
			if direct != nil && alias != nil {
				c.Check(direct.Common().Name == "int" && direct.Size() == 4,
					"fixture.V5 type = %v size %d", direct, direct.Size())
				if td, ok := alias.(*TypedefType); ok {
					c.Check(td.Type == direct, "fixture.V5: signature and offset types differ")
				} else {
					c.Check(false, "fixture.V5 alias is %T", alias)
				}
			}
		}

		// A type offset escaping its unit must not name a neighbor's DIE.
		tuBad := &tu5Unit{vers: 5, utype: utType, asize: 8, sig: sigBad}
		tuBad.dies = tu5Join(
			[]byte{tu5AbTypeUnit},
			tu5Base("rogue", encSigned, 2),
			[]byte{0},
		)
		tuBad.toff = len(tuBad.dies) + 13 // lands inside the next unit's DIEs
		dCU2 := []byte{tu5AbCompile}
		dTdBad := tu5TypedefSig(le, "Bad", sigBad)
		dMine := tu5Base("mine", encSigned, 4)
		tdBadRel := len(dCU2)
		mineRel := tdBadRel + len(dTdBad)
		cu2 := &tu5Unit{vers: 5, utype: utCompile, asize: 8}
		cu2.dies = tu5Join(dCU2, dTdBad, dMine, []byte{0})
		info2 := tu5Build(le, false, tuBad, cu2)
		db, err := New(tu5Abbrev, nil, nil, info2, nil, nil, nil, nil)
		if err != nil {
			c.Check(false, "New (bad type offset): %v", err)
		} else {
			_, err = db.Type(cu2.start + Offset(tdBadRel))
			c.Check(err != nil && strings.Contains(err.Error(), "outside its unit"),
				"escaping type offset error = %v", err)
			mine := tu5MustType(c, db, cu2.start+Offset(mineRel))
			if mine != nil {
				c.Check(mine.Common().Name == "mine" && mine.Size() == 4,
					"neighbor type = %v size %d", mine, mine.Size())
			}
		}

		// DWARF64, big endian, 4-byte addresses.
		be := binary.BigEndian
		dCU3 := []byte{tu5AbCompile}
		dTdW := tu5TypedefSig(be, "W", sigWord)
		tdWRel := len(dCU3)
		cu3 := &tu5Unit{vers: 5, utype: utCompile, asize: 4}
		cu3.dies = tu5Join(dCU3, dTdW, []byte{0})
		dPtr := tu5PtrRef(be, 0) // patched below
		dWord := tu5Base("word", encUnsigned, 4)
		ptrRel := 1
		wordRel := ptrRel + len(dPtr)
		tuHdr64 := uint32(tu5LenField(true) + tu5HdrLen(5, utType, true))
		be.PutUint32(dPtr[len(dPtr)-4:], tuHdr64+uint32(wordRel))
		tuW := &tu5Unit{vers: 5, utype: utType, asize: 4, sig: sigWord, toff: ptrRel}
		tuW.dies = tu5Join([]byte{tu5AbTypeUnit}, dPtr, dWord, []byte{0})
		info3 := tu5Build(be, true, cu3, tuW)
		d3, err := New(tu5Abbrev, nil, nil, info3, nil, nil, nil, nil)
		if err != nil {
			c.Check(false, "New (DWARF64 BE): %v", err)
			return
		}
		viaW := tu5MustType(c, d3, cu3.start+Offset(tdWRel))
		tPtr := tu5MustType(c, d3, tuW.start+Offset(ptrRel))
		if viaW == nil || tPtr == nil {
			return
		}
		if td, ok := viaW.(*TypedefType); ok {
			c.Check(td.Type == tPtr, "DWARF64 BE: signature and offset types differ")
		} else {
			c.Check(false, "typedef W is %T", viaW)
		}
		pt, ok := tPtr.(*PtrType)
		c.Check(ok, "DWARF64 BE primary type is %T", tPtr)
		if ok {
			c.Check(pt.Size() == 4, "DWARF64 BE pointer size = %d, want 4", pt.Size())
			c.Check(pt.Type != nil && pt.Type.Common().Name == "word" && pt.Type.Size() == 4,
				"DWARF64 BE pointee = %v", pt.Type)
		}
	})
}

// TestTypeUnit5RecursiveIdentity checks that two type units referring
// to each other by signature terminate, preserve node identity and
// member order, and take pointer sizes from their own unit's address
// size, while a shared third type stays a single object.
func TestTypeUnit5RecursiveIdentity(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			sigA = 0xAAAAAAAAAAAAAAAA
			sigB = 0xBBBBBBBBBBBBBBBB
			sigC = 0xCCCCCCCCCCCCCCCC
		)
		le := binary.LittleEndian
		tuHdr := uint32(tu5LenField(false) + tu5HdrLen(5, utType, false))

		// Compile unit with a typedef entering the cycle by signature.
		dCU := []byte{tu5AbCompile}
		dAlias := tu5TypedefSig(le, "AliasA", sigA)
		aliasRel := len(dCU)
		cu := &tu5Unit{vers: 5, utype: utCompile, asize: 8}
		cu.dies = tu5Join(dCU, dAlias, []byte{0})

		// Type unit A (8-byte addresses):
		//	struct A { to_b *B; shared *Shared }
		// Member ref4 values are patched before the struct copies them.
		mA0 := tu5Member(le, "to_b", 0, 0)
		mA1 := tu5Member(le, "shared", 0, 8)
		ptrB := tu5PtrSig(le, sigB)
		ptrCA := tu5PtrSig(le, sigC)
		structARel := 1
		structALen := 1 + 2 + 2 + len(mA0) + len(mA1) + 1
		ptrBRel := structARel + structALen
		ptrCARel := ptrBRel + len(ptrB)
		le.PutUint32(mA0[len(mA0)-5:], tuHdr+uint32(ptrBRel))
		le.PutUint32(mA1[len(mA1)-5:], tuHdr+uint32(ptrCARel))
		structA := tu5Struct(le, "A", 16, mA0, mA1)
		tuA := &tu5Unit{vers: 5, utype: utType, asize: 8, sig: sigA, toff: structARel}
		tuA.dies = tu5Join([]byte{tu5AbTypeUnit}, structA, ptrB, ptrCA, []byte{0})

		// Type unit B (4-byte addresses):
		//	struct B { to_a *A; shared *Shared }
		mB0 := tu5Member(le, "to_a", 0, 0)
		mB1 := tu5Member(le, "shared", 0, 4)
		ptrA := tu5PtrSig(le, sigA)
		ptrCB := tu5PtrSig(le, sigC)
		structBRel := 1
		structBLen := 1 + 2 + 2 + len(mB0) + len(mB1) + 1
		ptrARel := structBRel + structBLen
		ptrCBRel := ptrARel + len(ptrA)
		le.PutUint32(mB0[len(mB0)-5:], tuHdr+uint32(ptrARel))
		le.PutUint32(mB1[len(mB1)-5:], tuHdr+uint32(ptrCBRel))
		structB := tu5Struct(le, "B", 8, mB0, mB1)
		tuB := &tu5Unit{vers: 5, utype: utType, asize: 4, sig: sigB, toff: structBRel}
		tuB.dies = tu5Join([]byte{tu5AbTypeUnit}, structB, ptrA, ptrCB, []byte{0})

		// Type unit C shared by both: struct Shared { v int }
		mC0 := tu5Member(le, "v", 0, 0)
		intC := tu5Base("int", encSigned, 4)
		structCRel := 1
		structCLen := 1 + 7 + 2 + len(mC0) + 1
		intCRel := structCRel + structCLen
		le.PutUint32(mC0[len(mC0)-5:], tuHdr+uint32(intCRel))
		structC := tu5Struct(le, "Shared", 4, mC0)
		tuC := &tu5Unit{vers: 5, utype: utType, asize: 8, sig: sigC, toff: structCRel}
		tuC.dies = tu5Join([]byte{tu5AbTypeUnit}, structC, intC, []byte{0})

		info := tu5Build(le, false, cu, tuA, tuB, tuC)
		offAlias := cu.start + Offset(aliasRel)
		offA := tuA.start + Offset(structARel)
		offB := tuB.start + Offset(structBRel)
		offC := tuC.start + Offset(structCRel)

		newData := func() *Data {
			d, err := New(tu5Abbrev, nil, nil, info, nil, nil, nil, nil)
			if err != nil {
				c.Check(false, "New: %v", err)
				return nil
			}
			return d
		}

		// Direct offset first, signature second: one object.
		d1 := newData()
		if d1 == nil {
			return
		}
		tA := tu5MustType(c, d1, offA)
		via := tu5MustType(c, d1, offAlias)
		if tA == nil || via == nil {
			return
		}
		if td, ok := via.(*TypedefType); ok {
			c.Check(td.Type == tA, "direct-first: signature and offset types differ")
		} else {
			c.Check(false, "AliasA is %T", via)
		}

		// Signature first, direct offset second: still one object.
		d2 := newData()
		if d2 == nil {
			return
		}
		via2 := tu5MustType(c, d2, offAlias)
		tA2 := tu5MustType(c, d2, offA)
		if td, ok := via2.(*TypedefType); ok && tA2 != nil {
			c.Check(td.Type == tA2, "signature-first: signature and offset types differ")
		} else if !ok {
			c.Check(false, "AliasA is %T", via2)
		}

		// Entering through the shared type first keeps it identical.
		d3 := newData()
		if d3 == nil {
			return
		}
		tC3 := tu5MustType(c, d3, offC)
		via3 := tu5MustType(c, d3, offAlias)
		if td, ok := via3.(*TypedefType); ok && tC3 != nil {
			if st3, ok := td.Type.(*StructType); ok && len(st3.Field) == 2 {
				if p, ok := st3.Field[1].Type.(*PtrType); ok {
					c.Check(p.Type == tC3, "shared-first: shared type identity lost")
				}
			}
		} else if !ok {
			c.Check(false, "AliasA is %T", via3)
		}

		// Structure of A: name, kind, member order, offsets, size.
		st, ok := tA.(*StructType)
		c.Check(ok, "A is %T", tA)
		if !ok {
			return
		}
		c.Check(st.Kind == "struct" && st.StructName == "A",
			"A header = %s %s", st.Kind, st.StructName)
		c.Check(st.Size() == 16, "A size = %d, want 16", st.Size())
		c.Check(len(st.Field) == 2, "A has %d fields, want 2", len(st.Field))
		if len(st.Field) != 2 {
			return
		}
		c.Check(st.Field[0].Name == "to_b" && st.Field[1].Name == "shared",
			"A field order = %s, %s", st.Field[0].Name, st.Field[1].Name)
		c.Check(st.Field[0].ByteOffset == 0 && st.Field[1].ByteOffset == 8,
			"A field offsets = %d, %d", st.Field[0].ByteOffset, st.Field[1].ByteOffset)

		// A.to_b is an 8-byte pointer (A's address size) to B.
		pB, ok := st.Field[0].Type.(*PtrType)
		c.Check(ok, "A.to_b is %T", st.Field[0].Type)
		if !ok {
			return
		}
		c.Check(pB.Size() == 8, "A.to_b pointer size = %d, want 8", pB.Size())
		tB := tu5MustType(c, d1, offB)
		if tB == nil {
			return
		}
		c.Check(pB.Type == tB, "A.to_b does not reach B by signature")

		// Structure of B, with a 4-byte pointer back to the same A.
		sb, ok := tB.(*StructType)
		c.Check(ok, "B is %T", tB)
		if !ok {
			return
		}
		c.Check(sb.StructName == "B" && sb.Size() == 8,
			"B = %s size %d, want B/8", sb.StructName, sb.Size())
		c.Check(len(sb.Field) == 2, "B has %d fields, want 2", len(sb.Field))
		if len(sb.Field) != 2 {
			return
		}
		c.Check(sb.Field[0].Name == "to_a" && sb.Field[1].Name == "shared",
			"B field order = %s, %s", sb.Field[0].Name, sb.Field[1].Name)
		pA, ok := sb.Field[0].Type.(*PtrType)
		c.Check(ok, "B.to_a is %T", sb.Field[0].Type)
		if !ok {
			return
		}
		c.Check(pA.Size() == 4, "B.to_a pointer size = %d, want 4", pA.Size())
		c.Check(pA.Type == tA, "recursive B.to_a is not the same A object")

		// The shared third type is one object through both units.
		pCA, ok1 := st.Field[1].Type.(*PtrType)
		pCB, ok2 := sb.Field[1].Type.(*PtrType)
		c.Check(ok1 && ok2, "shared members are %T and %T", st.Field[1].Type, sb.Field[1].Type)
		if !ok1 || !ok2 {
			return
		}
		tC := tu5MustType(c, d1, offC)
		if tC == nil {
			return
		}
		c.Check(pCA.Type == tC && pCB.Type == tC, "shared type C has multiple objects")
		c.Check(pCA.Size() == 8 && pCB.Size() == 4,
			"shared pointer sizes = %d, %d, want 8, 4", pCA.Size(), pCB.Size())
		sc, ok := tC.(*StructType)
		c.Check(ok && sc.StructName == "Shared" && sc.Size() == 4,
			"C = %v size %d", tC, tC.Size())
		if ok && len(sc.Field) == 1 {
			c.Check(sc.Field[0].Name == "v" && sc.Field[0].Type.Common().Name == "int" &&
				sc.Field[0].Type.Size() == 4, "C.v = %v", sc.Field[0].Type)
		} else if ok {
			c.Check(false, "C has %d fields, want 1", len(sc.Field))
		}

		// The recursion left no size unresolved.
		c.Check(tA.Size() == 16 && tB.Size() == 8 && tC.Size() == 4,
			"sizes = %d, %d, %d, want 16, 8, 4", tA.Size(), tB.Size(), tC.Size())
	})
}

// TestTypeUnit5LegacyIsolation checks that DWARF 4 .debug_types
// sections added with AddTypes keep working next to .debug_info, and
// that numerically equal offsets in different sections never resolve
// to each other's entries.
func TestTypeUnit5LegacyIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			sigLeg1 = 0x0123456789ABCDEF
			sigLeg2 = 0xFEDCBA9876543210
		)
		le := binary.LittleEndian

		// DWARF 4 compile unit in .debug_info. Padding places the
		// "int" DIE at info offset 25, the same numeric offset as the
		// type DIEs in the .debug_types sections below.
		dCU := []byte{tu5AbCompile}
		pad1 := tu5Base("pad", encSigned, 8)
		pad2 := tu5Base("pd", encSigned, 8)
		dInt := tu5Base("int", encSigned, 4)
		dLeg1 := tu5TypedefSig(le, "Leg", sigLeg1)
		dLeg2 := tu5TypedefSig(le, "LegTwo", sigLeg2)
		intRel := len(dCU) + len(pad1) + len(pad2)
		leg1Rel := intRel + len(dInt)
		leg2Rel := leg1Rel + len(dLeg1)
		cu := &tu5Unit{vers: 4, asize: 8}
		cu.dies = tu5Join(dCU, pad1, pad2, dInt, dLeg1, dLeg2, []byte{0})
		info := tu5Build(le, false, cu)
		intOff := cu.start + Offset(intRel)
		leg1Off := cu.start + Offset(leg1Rel)
		leg2Off := cu.start + Offset(leg2Rel)
		c.Check(intOff == 25, "int DIE at info offset %d, want 25", intOff)

		// Two DWARF 4 .debug_types sections; each type DIE sits at
		// section offset 25, colliding numerically with the info DIE
		// and with each other.
		types1 := tu5TypesSection(le, 8, sigLeg1, 25,
			tu5Join([]byte{0, 0}, tu5Base("legone", encUnsigned, 1)))
		types2 := tu5TypesSection(le, 8, sigLeg2, 25,
			tu5Join([]byte{0, 0}, tu5Base("legtwo", encUnsigned, 2)))

		newData := func() *Data {
			d, err := New(tu5Abbrev, nil, nil, info, nil, nil, nil, nil)
			if err != nil {
				c.Check(false, "New: %v", err)
				return nil
			}
			if err := d.AddTypes("types1", types1); err != nil {
				c.Check(false, "AddTypes types1: %v", err)
				return nil
			}
			if err := d.AddTypes("types2", types2); err != nil {
				c.Check(false, "AddTypes types2: %v", err)
				return nil
			}
			return d
		}

		d := newData()
		if d == nil {
			return
		}

		// Info offset 25 is the info base type.
		tInt := tu5MustType(c, d, intOff)
		if tInt == nil {
			return
		}
		c.Check(tInt.Common().Name == "int" && tInt.Size() == 4,
			"info type = %v size %d, want int/4", tInt, tInt.Size())

		// The same numeric offset in each types section is its own type.
		via1 := tu5MustType(c, d, leg1Off)
		td1, ok := via1.(*TypedefType)
		c.Check(ok, "Leg is %T", via1)
		if !ok {
			return
		}
		tLeg1 := td1.Type
		c.Check(tLeg1.Common().Name == "legone" && tLeg1.Size() == 1,
			"types1 type = %v size %d, want legone/1", tLeg1, tLeg1.Size())
		via2 := tu5MustType(c, d, leg2Off)
		td2, ok := via2.(*TypedefType)
		c.Check(ok, "LegTwo is %T", via2)
		if !ok {
			return
		}
		tLeg2 := td2.Type
		c.Check(tLeg2.Common().Name == "legtwo" && tLeg2.Size() == 2,
			"types2 type = %v size %d, want legtwo/2", tLeg2, tLeg2.Size())

		// Equal numeric offsets in different sections stay distinct.
		c.Check(tLeg1 != tInt, "types1 reused the info type object")
		c.Check(tLeg2 != tInt && tLeg2 != tLeg1, "types sections share type objects")

		// Signature reads do not pollute the info type cache, and
		// each type unit keeps its own cached type.
		c.Check(tu5MustType(c, d, intOff) == tInt, "info type cache polluted by types sections")
		via1b := tu5MustType(c, d, leg1Off)
		if td, ok := via1b.(*TypedefType); ok {
			c.Check(td.Type == tLeg1, "types1 type not cached per unit")
		} else {
			c.Check(false, "Leg is %T", via1b)
		}

		// Reverse order: signatures first, info offset second.
		dr := newData()
		if dr == nil {
			return
		}
		r1 := tu5MustType(c, dr, leg1Off)
		r2 := tu5MustType(c, dr, leg2Off)
		rInt := tu5MustType(c, dr, intOff)
		if r1 == nil || r2 == nil || rInt == nil {
			return
		}
		c.Check(rInt.Common().Name == "int" && rInt.Size() == 4,
			"reversed: info type = %v size %d", rInt, rInt.Size())
		if td, ok := r1.(*TypedefType); ok {
			c.Check(td.Type.Common().Name == "legone" && td.Type.Size() == 1,
				"reversed: types1 type = %v size %d", td.Type, td.Type.Size())
			c.Check(td.Type != rInt, "reversed: types1 reused the info type object")
		} else {
			c.Check(false, "Leg is %T", r1)
		}
		if td, ok := r2.(*TypedefType); ok {
			c.Check(td.Type.Common().Name == "legtwo" && td.Type.Size() == 2,
				"reversed: types2 type = %v size %d", td.Type, td.Type.Size())
		} else {
			c.Check(false, "LegTwo is %T", r2)
		}
	})
}
