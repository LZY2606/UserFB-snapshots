// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package obj

import (
	"debug/elf"
	"fmt"
	"math/bits"
)

// addLoadBias adds a signed load bias to addr and reports whether the
// result is representable as a uint64.
func addLoadBias(addr uint64, bias int64) (uint64, bool) {
	sum, carry := bits.Add64(addr, uint64(bias), 0)
	if bias >= 0 {
		return sum, carry == 0
	}
	// For a negative bias, the addition carries out exactly when the
	// result is non-negative.
	return sum, carry != 0
}

// Assert that elfFile implements LoadBiasFile.
var _ LoadBiasFile = (*elfFile)(nil)

// LoadBias returns the current load bias of f.
func (f *elfFile) LoadBias() int64 {
	return f.loadBias
}

// SetLoadBias sets the load bias of f. See the LoadBiasFile interface
// for the contract.
func (f *elfFile) SetLoadBias(bias int64) error {
	if f.relocatable {
		return fmt.Errorf("cannot set a load bias on a relocatable object file")
	}
	if bias == f.loadBias {
		return nil
	}
	// Check that every address that will move remains representable
	// before changing any observable state.
	if err := f.checkLoadBias(bias); err != nil {
		return err
	}
	f.loadBias = bias
	for _, es := range f.sections {
		if es.Mapped() {
			// Recompute from the link-time address so repeated
			// calls replace rather than accumulate.
			es.Section.Addr, _ = addLoadBias(es.elf.Addr, bias)
		}
	}
	return nil
}

// checkLoadBias verifies that shifting all mapped sections and the
// symbols defined in them by bias keeps every affected address and
// interval end within the uint64 address range.
func (f *elfFile) checkLoadBias(bias int64) error {
	for _, es := range f.sections {
		if !es.Mapped() {
			continue
		}
		if _, ok := addLoadBias(es.elf.Addr, bias); !ok {
			return fmt.Errorf("load bias %#x moves section %s (at %#x) out of the uint64 address range", uint64(bias), es, es.elf.Addr)
		}
		if _, ok := addLoadBias(es.elf.Addr+es.elf.Size, bias); !ok {
			return fmt.Errorf("load bias %#x moves end of section %s (at %#x) out of the uint64 address range", uint64(bias), es, es.elf.Addr+es.elf.Size)
		}
	}

	// Ordinary defined symbols in mapped sections move with their
	// section, so their values must remain representable, too.
	for i := range f.symTabs {
		tab := &f.symTabs[i]
		if tab.section == nil {
			continue
		}
		r := NewReader(&tab.data)
		for id := tab.start; id < tab.end; id++ {
			r.SetOffset(int(f.symSize * uint64(id-tab.start+1)))
			var value uint64
			var info uint8
			var shn elf.SectionIndex
			switch f.f.Class {
			case elf.ELFCLASS32:
				r.Uint32() // st_name
				value = uint64(r.Uint32())
				r.Uint32() // st_size
				info = r.Uint8()
				r.Uint8() // st_other
				shn = elf.SectionIndex(r.Uint16())
			case elf.ELFCLASS64:
				r.Uint32() // st_name
				info = r.Uint8()
				r.Uint8() // st_other
				shn = elf.SectionIndex(r.Uint16())
				value = r.Uint64()
				r.Uint64() // st_size
			}
			if !f.symShiftsWithLoadBias(info, shn) {
				continue
			}
			if _, ok := addLoadBias(value, bias); !ok {
				return fmt.Errorf("load bias %#x moves symbol %d (value %#x) out of the uint64 address range", uint64(bias), id, value)
			}
		}
	}
	return nil
}

// symShiftsWithLoadBias reports whether a symbol with the given st_info
// and st_shndx has an address-like value that moves with the load bias.
// Undefined, absolute, and common symbols, TLS symbols (whose values are
// TLS offsets, not addresses), and symbols in unmapped sections keep
// their link-time values.
func (f *elfFile) symShiftsWithLoadBias(info uint8, shn elf.SectionIndex) bool {
	if elf.ST_TYPE(info) == elf.STT_TLS {
		return false
	}
	es, ok := f.lookupShn(shn)
	return ok && es.Mapped()
}

// sectionBias returns the load bias that applies to addresses in
// section s. Only mapped sections move with the load bias.
func (f *elfFile) sectionBias(s *elfSection) int64 {
	if s.Mapped() {
		return f.loadBias
	}
	return 0
}
