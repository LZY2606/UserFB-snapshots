// Copyright 2012 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf

import (
	"fmt"
	"strconv"
)

// Parse the type units stored in a DWARF4 .debug_types section, or the
// DWARF 5 DW_UT_type units living in .debug_info. Each type unit defines
// a single primary type and an 8-byte signature. Other units may then use
// formRefSig8 to refer to the type.

// The typeUnit format is a single type with a signature. It holds
// the same data as a compilation unit.
type typeUnit struct {
	unit
	toff Offset // Section-global offset of the signature type DIE.
	name string // Name of the section holding the unit ("info" or a .debug_types name).

	// typeCache holds every type read from this unit's section. Type
	// units in .debug_info share Data.typeCache, so a type reached by
	// signature is the same object as one reached through its global
	// DIE offset. Each standalone .debug_types section gets its own
	// cache, keeping equal local offsets in different sections apart.
	typeCache map[Offset]Type

	// cache holds the primary type once it has been fully built. It is
	// nil while the type is still being constructed, which is how
	// signature cycles terminate without infinite recursion.
	cache Type
}

// Parse a .debug_types section.
func (d *Data) parseTypes(name string, types []byte) error {
	b := makeBuf(d, unknownFormat{}, name, 0, types)
	// All units of one section share one coordinate space and therefore
	// one type cache; a different AddTypes call gets a different one.
	sectionCache := make(map[Offset]Type)
	for len(b.data) > 0 {
		base := b.off
		n, dwarf64 := b.unitLength()
		if n != Offset(uint32(n)) {
			b.error("type unit length overflow")
			return b.err
		}
		hdroff := b.off
		vers := int(b.uint16())
		if vers != 4 {
			b.error("unsupported DWARF version " + strconv.Itoa(vers))
			return b.err
		}
		var ao uint64
		if !dwarf64 {
			ao = uint64(b.uint32())
		} else {
			ao = b.uint64()
		}
		atable, err := d.parseAbbrev(ao, vers)
		if err != nil {
			return err
		}
		asize := b.uint8()
		sig := b.uint64()

		var toff uint32
		if !dwarf64 {
			toff = b.uint32()
		} else {
			to64 := b.uint64()
			if to64 != uint64(uint32(to64)) {
				b.error("type unit type offset overflow")
				return b.err
			}
			toff = uint32(to64)
		}

		boff := b.off
		data := b.bytes(int(n - (b.off - hdroff)))
		// The type offset in the header is relative to the unit header
		// (which includes the unit length); resolve it to a
		// section-global DIE offset and keep it inside this unit.
		root := base + Offset(toff)
		if root < boff || root >= boff+Offset(len(data)) {
			return DecodeError{name, base, "type unit type offset out of range"}
		}
		d.typeSigs[sig] = &typeUnit{
			unit: unit{
				base:   base,
				off:    boff,
				data:   data,
				atable: atable,
				asize:  int(asize),
				vers:   vers,
				is64:   dwarf64,
			},
			toff:      root,
			name:      name,
			typeCache: sectionCache,
		}
		if b.err != nil {
			return b.err
		}
	}
	return nil
}

// Return the type for a type signature.
func (d *Data) sigToType(sig uint64) (Type, error) {
	tu := d.typeSigs[sig]
	if tu == nil {
		return nil, fmt.Errorf("no type unit with signature %v", sig)
	}
	if tu.cache != nil {
		return tu.cache, nil
	}

	// Read through the same offset-keyed cache as the surrounding
	// section. A type already under construction is returned directly,
	// which breaks mutually recursive type units instead of looping.
	b := makeBuf(d, tu, tu.name, tu.off, tu.data)
	r := &typeUnitReader{d: d, tu: tu, b: b}
	t, err := d.readType(tu.name, r, tu.toff, tu.typeCache, nil)
	if err != nil {
		return nil, err
	}

	tu.cache = t
	return t, nil
}

// typeUnitReader is a typeReader for a tagTypeUnit.
type typeUnitReader struct {
	d   *Data
	tu  *typeUnit
	b   buf
	err error
}

// Seek to a new position in the type unit. off is a section-global
// offset; references must stay within the unit they belong to.
func (tur *typeUnitReader) Seek(off Offset) {
	tur.err = nil
	doff := off - tur.tu.off
	if doff < 0 || doff >= Offset(len(tur.tu.data)) {
		tur.err = fmt.Errorf("%s: offset %d out of range; max %d", tur.tu.name, doff, len(tur.tu.data))
		return
	}
	tur.b = makeBuf(tur.d, tur.tu, tur.tu.name, off, tur.tu.data[doff:])
}

// AddressSize returns the size in bytes of addresses in the current type unit.
func (tur *typeUnitReader) AddressSize() int {
	return tur.tu.unit.asize
}

// Next reads the next [Entry] from the type unit.
func (tur *typeUnitReader) Next() (*Entry, error) {
	if tur.err != nil {
		return nil, tur.err
	}
	if len(tur.tu.data) == 0 {
		return nil, nil
	}
	e := tur.b.entry(nil, tur.tu.atable, tur.tu.base, tur.tu.vers)
	if tur.b.err != nil {
		tur.err = tur.b.err
		return nil, tur.err
	}
	return e, nil
}

// clone returns a new reader for the type unit.
func (tur *typeUnitReader) clone() typeReader {
	return &typeUnitReader{
		d:  tur.d,
		tu: tur.tu,
		b:  makeBuf(tur.d, tur.tu, tur.tu.name, tur.tu.off, tur.tu.data),
	}
}

// offset returns the current offset.
func (tur *typeUnitReader) offset() Offset {
	return tur.b.off
}
