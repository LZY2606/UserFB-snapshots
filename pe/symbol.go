// Copyright 2016 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pe

import (
	"encoding/binary"
	"fmt"
	"io"
)

const COFFSymbolSize = 18

// COFFSymbol represents single COFF symbol table record.
//
// SectionNumber is a signed 32-bit value even for ordinary 18-byte COFF
// records, where the on-disk field is only 16 bits: BigObj records use a
// full 32-bit section number. Positive values are one-based section numbers,
// 0 marks an undefined (or common) symbol, -1 an absolute symbol and -2 a
// debug symbol; those special values must not be treated as section indexes.
type COFFSymbol struct {
	Name               [8]uint8
	Value              uint32
	SectionNumber      int32
	Type               uint16
	StorageClass       uint8
	NumberOfAuxSymbols uint8
}

func readCOFFSymbols(fh *FileHeader, r io.ReadSeeker) ([]COFFSymbol, error) {
	if fh.PointerToSymbolTable == 0 {
		return nil, nil
	}
	if fh.NumberOfSymbols <= 0 {
		return nil, nil
	}
	if _, err := r.Seek(int64(fh.PointerToSymbolTable), seekStart); err != nil {
		return nil, fmt.Errorf("fail to seek to symbol table: %v", err)
	}
	// COFFSymbol is held in its widened, 20-byte BigObj shape in memory,
	// so decode each 18-byte on-disk record explicitly instead of using
	// binary.Read on the struct slice.
	buf := make([]byte, COFFSymbolSize*fh.NumberOfSymbols)
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, fmt.Errorf("fail to read symbol table: %v", err)
	}
	syms := make([]COFFSymbol, fh.NumberOfSymbols)
	for i := range syms {
		rec := buf[i*COFFSymbolSize : (i+1)*COFFSymbolSize]
		syms[i] = COFFSymbol{
			Value:              binary.LittleEndian.Uint32(rec[8:12]),
			SectionNumber:      int32(int16(binary.LittleEndian.Uint16(rec[12:14]))),
			Type:               binary.LittleEndian.Uint16(rec[14:16]),
			StorageClass:       rec[16],
			NumberOfAuxSymbols: rec[17],
		}
		copy(syms[i].Name[:], rec[0:8])
	}
	return syms, nil
}

// writeCOFFSymbol18 serializes one symbol as the ordinary 18-byte COFF
// record. Only ordinary COFF/PE files are writable, so their 16-bit
// section numbers fit the narrowed field.
func writeCOFFSymbol18(w io.Writer, sym *COFFSymbol) error {
	var buf [COFFSymbolSize]byte
	copy(buf[0:8], sym.Name[:])
	binary.LittleEndian.PutUint32(buf[8:12], sym.Value)
	binary.LittleEndian.PutUint16(buf[12:14], uint16(int16(sym.SectionNumber)))
	binary.LittleEndian.PutUint16(buf[14:16], sym.Type)
	buf[16] = sym.StorageClass
	buf[17] = sym.NumberOfAuxSymbols
	_, err := w.Write(buf[:])
	return err
}

// readBigObjCOFFSymbols reads the 20-byte symbol slots of a BigObj v2
// object. The layout matches an ordinary COFFSymbol except SectionNumber
// occupies four bytes (offset 12) instead of two.
func readBigObjCOFFSymbols(fh *FileHeader, r io.ReaderAt) ([]COFFSymbol, error) {
	if fh.PointerToSymbolTable == 0 {
		return nil, nil
	}
	if fh.NumberOfSymbols <= 0 {
		return nil, nil
	}
	buf := make([]byte, bigObjSymbolSize*fh.NumberOfSymbols)
	if _, err := r.ReadAt(buf, int64(fh.PointerToSymbolTable)); err != nil {
		return nil, fmt.Errorf("fail to read bigobj symbol table: %v", err)
	}
	syms := make([]COFFSymbol, fh.NumberOfSymbols)
	for i := range syms {
		rec := buf[i*bigObjSymbolSize : (i+1)*bigObjSymbolSize]
		syms[i] = COFFSymbol{
			Value:              binary.LittleEndian.Uint32(rec[8:12]),
			SectionNumber:      int32(binary.LittleEndian.Uint32(rec[12:16])),
			Type:               binary.LittleEndian.Uint16(rec[16:18]),
			StorageClass:       rec[18],
			NumberOfAuxSymbols: rec[19],
		}
		copy(syms[i].Name[:], rec[0:8])
	}
	return syms, nil
}

// isSymNameOffset checks symbol name if it is encoded as offset into string table.
func isSymNameOffset(name [8]byte) (bool, uint32) {
	if name[0] == 0 && name[1] == 0 && name[2] == 0 && name[3] == 0 {
		return true, binary.LittleEndian.Uint32(name[4:])
	}
	return false, 0
}

// FullName finds real name of symbol sym. Normally name is stored
// in sym.Name, but if it is longer then 8 characters, it is stored
// in COFF string table st instead.
func (sym *COFFSymbol) FullName(st StringTable) (string, error) {
	if ok, offset := isSymNameOffset(sym.Name); ok {
		return st.String(offset)
	}
	return cstring(sym.Name[:]), nil
}

func removeAuxSymbols(allsyms []COFFSymbol, st StringTable) ([]*Symbol, error) {
	if len(allsyms) == 0 {
		return nil, nil
	}
	syms := make([]*Symbol, 0)
	aux := uint8(0)
	for _, sym := range allsyms {
		if aux > 0 {
			aux--
			continue
		}
		name, err := sym.FullName(st)
		if err != nil {
			return nil, err
		}
		aux = sym.NumberOfAuxSymbols
		s := &Symbol{
			Name:          name,
			Value:         sym.Value,
			SectionNumber: sym.SectionNumber,
			Type:          sym.Type,
			StorageClass:  sym.StorageClass,
		}
		syms = append(syms, s)
	}
	return syms, nil
}

// Symbol is similar to COFFSymbol with Name field replaced
// by Go string. Symbol also does not have NumberOfAuxSymbols.
//
// SectionNumber is signed 32-bit to preserve the full BigObj section number;
// see COFFSymbol for the meaning of 0, -1 and -2.
type Symbol struct {
	Name          string
	Value         uint32
	SectionNumber int32
	Type          uint16
	StorageClass  uint8
}
