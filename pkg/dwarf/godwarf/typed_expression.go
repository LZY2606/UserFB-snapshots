package godwarf

import (
	"debug/dwarf"
	"encoding/binary"
	"fmt"

	"github.com/go-delve/delve/pkg/dwarf/op"
)

// dwarf5HeaderSize is the size of a DWARF5 32-bit compilation unit header:
// unit_length(4) + version(2) + unit_type(1) + address_size(1) + abbrev_offset(4).
const dwarf5HeaderSize = 12

// NewBaseTypeResolver returns an op.BaseTypeResolver that resolves
// CU-relative type DIE offsets for the compilation unit whose header starts
// at cuOffset in debugInfo (cuOffset is relative to the start of
// .debug_info). Only DWARF5/DWARF32 compilation units are supported and only
// base types with a signed, signed_char, unsigned or unsigned_char encoding
// are resolved.
//
// The returned resolver only accepts offsets of DW_TAG_base_type DIEs that
// belong to this compilation unit; offsets outside the unit, offsets that do
// not point at the start of a DIE and offsets of non-base-type DIEs are
// rejected. Each resolver keeps its own per-unit table, so identical
// CU-relative offsets in different units never share cached results.
func NewBaseTypeResolver(dw *dwarf.Data, debugInfo []byte, cuOffset dwarf.Offset) (op.BaseTypeResolver, error) {
	if uint64(cuOffset)+dwarf5HeaderSize > uint64(len(debugInfo)) {
		return nil, fmt.Errorf("compilation unit offset %#x out of range", uint64(cuOffset))
	}
	hdr := debugInfo[cuOffset:]
	unitLength := binary.LittleEndian.Uint32(hdr)
	if unitLength == 0xffffffff {
		return nil, fmt.Errorf("compilation unit at %#x: DWARF64 not supported", uint64(cuOffset))
	}
	cuEnd := uint64(cuOffset) + 4 + uint64(unitLength)
	if cuEnd > uint64(len(debugInfo)) {
		return nil, fmt.Errorf("compilation unit at %#x extends past end of .debug_info", uint64(cuOffset))
	}
	if version := binary.LittleEndian.Uint16(hdr[4:]); version != 5 {
		return nil, fmt.Errorf("compilation unit at %#x: unsupported DWARF version %d", uint64(cuOffset), version)
	}

	// Walk the DIEs of this unit once and record every base type, keyed by
	// its CU-relative offset. Because the table is built from actual DIE
	// start offsets, anything that is not the start of a base type DIE in
	// this unit is rejected by construction.
	types := make(map[uint64]op.BaseType)
	r := dw.Reader()
	r.Seek(cuOffset + dwarf5HeaderSize)
	for {
		e, err := r.Next()
		if err != nil {
			return nil, err
		}
		if e == nil || uint64(e.Offset) >= cuEnd {
			break
		}
		if e.Tag != dwarf.TagBaseType {
			continue
		}
		enc, _ := e.Val(dwarf.AttrEncoding).(int64)
		var signed bool
		switch enc {
		case encSigned, encSignedChar:
			signed = true
		case encUnsigned, encUnsignedChar:
			signed = false
		default:
			// not an integer base type, do not resolve it
			continue
		}
		sz, _ := e.Val(dwarf.AttrByteSize).(int64)
		types[uint64(e.Offset)-uint64(cuOffset)] = op.BaseType{ByteSize: int(sz), Signed: signed}
	}

	return func(off uint64) (op.BaseType, error) {
		bt, ok := types[off]
		if !ok {
			return op.BaseType{}, fmt.Errorf("no integer base type DIE at CU-relative offset %#x in compilation unit at %#x", off, uint64(cuOffset))
		}
		return bt, nil
	}, nil
}
