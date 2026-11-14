// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package obj

import (
	"bytes"
	"debug/elf"
	"testing"

	"github.com/aclements/go-obj/task_support/fixture"
	"github.com/aclements/go-obj/task_support/taskcheck"
)

// openLoadBias opens raw as an object file and checks that it implements
// LoadBiasFile. It returns nil if either check fails.
func openLoadBias(c *taskcheck.C, raw []byte) LoadBiasFile {
	f, err := Open(bytes.NewReader(raw))
	c.Check(err == nil, "Open failed: %v", err)
	if err != nil {
		return nil
	}
	lf, ok := f.(LoadBiasFile)
	c.Check(ok, "opened file does not implement obj.LoadBiasFile")
	if !ok {
		return nil
	}
	return lf
}

func sectionByName(f File, name string) *Section {
	for _, s := range f.Sections() {
		if s.Name == name {
			return s
		}
	}
	return nil
}

// checkFixtureRelocs checks the two relocations that apply to the
// fixture's .data section: one from the global relocation section and
// one from the relocation section targeting .data. Addends and symbol
// references are link-time facts and must not move with the load bias.
func checkFixtureRelocs(c *taskcheck.C, f File, d *Data, addr0, addr1 uint64) {
	c.Check(len(d.R) == 2, "relocations = %d, want 2", len(d.R))
	if len(d.R) != 2 {
		return
	}
	c.Check(d.R[0].Addr == addr0, "relocation 0 addr = %#x, want %#x", d.R[0].Addr, addr0)
	c.Check(d.R[1].Addr == addr1, "relocation 1 addr = %#x, want %#x", d.R[1].Addr, addr1)
	c.Check(d.R[0].Addend == -7 && d.R[1].Addend == 11,
		"addends = %d, %d, want -7, 11", d.R[0].Addend, d.R[1].Addend)
	c.Check(d.R[0].Symbol == 0 && d.R[1].Symbol == 0,
		"symbol IDs = %v, %v, want 0, 0", d.R[0].Symbol, d.R[1].Symbol)
	c.Check(f.Sym(d.R[0].Symbol).Name == "entry",
		"relocation symbol = %q, want %q", f.Sym(d.R[0].Symbol).Name, "entry")
}

func TestLoadBiasCoordinates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, bits := range []int{64, 32} {
			f := openLoadBias(c, fixture.Image(bits))
			if f == nil {
				return
			}
			defer f.Close()

			c.Check(f.LoadBias() == 0, "ELF%d: initial LoadBias = %#x, want 0", bits, f.LoadBias())

			text := sectionByName(f, ".text")
			meta := sectionByName(f, ".meta")
			tls := sectionByName(f, ".tls")
			c.Check(text != nil && meta != nil && tls != nil, "ELF%d: fixture sections missing", bits)
			if text == nil || meta == nil || tls == nil {
				return
			}
			c.Check(text.Addr == 0x1000 && text.Mapped(), "ELF%d: .text at %#x, mapped=%t", bits, text.Addr, text.Mapped())
			c.Check(meta.Addr == 0 && !meta.Mapped(), "ELF%d: .meta should be unmapped at 0", bits)
			textID := text.ID

			// Switch to a new load bias.
			const bias = 0x100000
			err := f.SetLoadBias(bias)
			c.Check(err == nil, "ELF%d: SetLoadBias(%#x): %v", bits, bias, err)
			c.Check(f.LoadBias() == bias, "ELF%d: LoadBias = %#x, want %#x", bits, f.LoadBias(), int64(bias))

			// Section identity is stable; only coordinates move.
			c.Check(f.Section(textID) == text, "ELF%d: Section pointer changed across SetLoadBias", bits)
			c.Check(text.Addr == 0x1000+bias, "ELF%d: .text.Addr = %#x, want %#x", bits, text.Addr, 0x1000+bias)
			c.Check(tls.Addr == 0x4000+bias, "ELF%d: .tls.Addr = %#x, want %#x", bits, tls.Addr, 0x4000+bias)
			c.Check(meta.Addr == 0, "ELF%d: unmapped .meta.Addr = %#x, want 0", bits, meta.Addr)

			// ResolveAddr uses the observed coordinates.
			c.Check(f.ResolveAddr(0x1000+bias) == text, "ELF%d: ResolveAddr(shifted .text) missed", bits)
			c.Check(f.ResolveAddr(0x1000) == nil, "ELF%d: ResolveAddr(link-time .text) should miss after bias", bits)

			// Section.Data reads in observed coordinates.
			d, err := text.Data(text.Addr, 4)
			c.Check(err == nil && d.Addr == text.Addr,
				"ELF%d: .text Data: addr = %#x, err = %v", bits, d.Addr, err)
			c.Check(err == nil && bytes.Equal(d.B, []byte{0x90, 0x90, 0x90, 0xc3}),
				"ELF%d: .text bytes = %x", bits, d.B)

			// Ordinary defined symbols in mapped sections move;
			// absolute, undefined, common, TLS, and unmapped
			// symbols keep their values. Symbol numbering is stable.
			c.Check(f.NumSyms() == 6, "ELF%d: NumSyms = %d, want 6", bits, f.NumSyms())
			entry := f.Sym(0)
			c.Check(entry.Name == "entry" && entry.Value == 0x1000+bias,
				"ELF%d: entry = %q at %#x", bits, entry.Name, entry.Value)
			c.Check(entry.Section == text, "ELF%d: entry section pointer changed", bits)
			c.Check(f.Sym(1).Name == "absolute" && f.Sym(1).Value == 0x42,
				"ELF%d: absolute symbol moved to %#x", bits, f.Sym(1).Value)
			c.Check(f.Sym(2).Kind == SymUndef && f.Sym(2).Value == 0,
				"ELF%d: undefined symbol changed: %v %#x", bits, f.Sym(2).Kind, f.Sym(2).Value)
			c.Check(f.Sym(3).Name == "common" && f.Sym(3).Value == 8,
				"ELF%d: common symbol moved to %#x", bits, f.Sym(3).Value)
			c.Check(f.Sym(4).Name == "thread" && f.Sym(4).Value == 4,
				"ELF%d: TLS symbol moved to %#x", bits, f.Sym(4).Value)
			c.Check(f.Sym(5).Name == "metadata" && f.Sym(5).Value == 0,
				"ELF%d: symbol in unmapped section moved to %#x", bits, f.Sym(5).Value)

			// Setting the bias again replaces it; it must not accumulate.
			err = f.SetLoadBias(0x200000)
			c.Check(err == nil, "ELF%d: second SetLoadBias: %v", bits, err)
			c.Check(text.Addr == 0x1000+0x200000,
				"ELF%d: bias accumulated: .text.Addr = %#x", bits, text.Addr)

			// A negative bias shifts below the link-time addresses.
			err = f.SetLoadBias(-0x800)
			c.Check(err == nil, "ELF%d: SetLoadBias(-0x800): %v", bits, err)
			c.Check(f.LoadBias() == -0x800, "ELF%d: LoadBias = %#x", bits, f.LoadBias())
			c.Check(text.Addr == 0x800, "ELF%d: negative bias: .text.Addr = %#x", bits, text.Addr)
			c.Check(f.ResolveAddr(0x800) == text, "ELF%d: ResolveAddr(negative-bias .text) missed", bits)
			c.Check(f.Sym(0).Value == 0x800, "ELF%d: negative bias: entry = %#x", bits, f.Sym(0).Value)

			// Round trip back to the link-time coordinates.
			err = f.SetLoadBias(0)
			c.Check(err == nil, "ELF%d: SetLoadBias(0): %v", bits, err)
			c.Check(text.Addr == 0x1000 && f.Sym(0).Value == 0x1000,
				"ELF%d: round trip: .text = %#x, entry = %#x", bits, text.Addr, f.Sym(0).Value)
			c.Check(f.ResolveAddr(0x1000) == text, "ELF%d: ResolveAddr after round trip missed", bits)

			// A bias that pushes a mapped address out of range
			// fails and preserves the current observation state.
			err = f.SetLoadBias(-0x2000)
			c.Check(err != nil, "ELF%d: SetLoadBias(-0x2000) should fail", bits)
			c.Check(f.LoadBias() == 0, "ELF%d: LoadBias changed to %#x after failed set", bits, f.LoadBias())
			c.Check(text.Addr == 0x1000, "ELF%d: .text moved to %#x after failed set", bits, text.Addr)
			c.Check(f.Sym(0).Value == 0x1000, "ELF%d: entry moved to %#x after failed set", bits, f.Sym(0).Value)
			c.Check(f.ResolveAddr(0x1000) == text, "ELF%d: ResolveAddr moved after failed set", bits)
		}

		// ET_EXEC supports load bias, too.
		exec := openLoadBias(c, fixture.WithType(fixture.Image(64), elf.ET_EXEC))
		if exec == nil {
			return
		}
		defer exec.Close()
		err := exec.SetLoadBias(0x4000)
		c.Check(err == nil, "ET_EXEC: SetLoadBias: %v", err)
		c.Check(sectionByName(exec, ".text").Addr == 0x5000,
			"ET_EXEC: .text.Addr = %#x, want 0x5000", sectionByName(exec, ".text").Addr)

		// ET_REL has no mapped address space to bias.
		rel := openLoadBias(c, fixture.Relocatable(fixture.Image(64)))
		if rel == nil {
			return
		}
		defer rel.Close()
		err = rel.SetLoadBias(0x4000)
		c.Check(err != nil, "ET_REL: SetLoadBias should fail")
		c.Check(rel.LoadBias() == 0, "ET_REL: LoadBias = %#x, want 0", rel.LoadBias())
	})
}

func TestLoadBiasRelocations(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, bits := range []int{64, 32} {
			const bias = 0x80000

			// Cold relocation cache: set the bias before the
			// first read of .data and its relocations.
			f := openLoadBias(c, fixture.Image(bits))
			if f == nil {
				return
			}
			defer f.Close()
			err := f.SetLoadBias(bias)
			c.Check(err == nil, "ELF%d: SetLoadBias: %v", bits, err)
			data := sectionByName(f, ".data")
			d, err := data.Data(data.Bounds())
			c.Check(err == nil, "ELF%d: cold Data: %v", bits, err)
			c.Check(d.Addr == 0x2000+bias, "ELF%d: cold Data.Addr = %#x", bits, d.Addr)
			checkFixtureRelocs(c, f, d, 0x2008+bias, 0x2018+bias)

			// The cache is now warm; switching the bias again
			// must give the same relocations in the new
			// coordinates without disturbing the old Data.
			err = f.SetLoadBias(2 * bias)
			c.Check(err == nil, "ELF%d: second SetLoadBias: %v", bits, err)
			d2, err := data.Data(data.Bounds())
			c.Check(err == nil, "ELF%d: warm Data: %v", bits, err)
			checkFixtureRelocs(c, f, d2, 0x2008+2*bias, 0x2018+2*bias)
			c.Check(d.Addr == 0x2000+bias && d.R[0].Addr == 0x2008+bias && d.R[1].Addr == 0x2018+bias,
				"ELF%d: earlier Data mutated: addr = %#x, relocs = %#x, %#x",
				bits, d.Addr, d.R[0].Addr, d.R[1].Addr)

			// Warm the relocation cache at link-time
			// coordinates first, then apply a bias.
			f2 := openLoadBias(c, fixture.Image(bits))
			if f2 == nil {
				return
			}
			defer f2.Close()
			data2 := sectionByName(f2, ".data")
			d0, err := data2.Data(data2.Bounds())
			c.Check(err == nil, "ELF%d: unbiased Data: %v", bits, err)
			checkFixtureRelocs(c, f2, d0, 0x2008, 0x2018)
			err = f2.SetLoadBias(bias)
			c.Check(err == nil, "ELF%d: SetLoadBias after warm read: %v", bits, err)
			d1, err := data2.Data(data2.Bounds())
			c.Check(err == nil, "ELF%d: biased Data: %v", bits, err)
			checkFixtureRelocs(c, f2, d1, 0x2008+bias, 0x2018+bias)
			c.Check(d0.Addr == 0x2000 && d0.R[0].Addr == 0x2008 && d0.R[1].Addr == 0x2018,
				"ELF%d: unbiased Data mutated: addr = %#x, relocs = %#x, %#x",
				bits, d0.Addr, d0.R[0].Addr, d0.R[1].Addr)

			// Switching back reproduces the original coordinates.
			err = f2.SetLoadBias(0)
			c.Check(err == nil, "ELF%d: SetLoadBias(0): %v", bits, err)
			d3, err := data2.Data(data2.Bounds())
			c.Check(err == nil, "ELF%d: round-trip Data: %v", bits, err)
			checkFixtureRelocs(c, f2, d3, 0x2008, 0x2018)
		}
	})
}

func TestLoadBiasTransitions(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, bits := range []int{64, 32} {
			f := openLoadBias(c, fixture.Image(bits))
			if f == nil {
				return
			}
			defer f.Close()

			text := sectionByName(f, ".text")
			data := sectionByName(f, ".data")
			textID := text.ID

			// The debug/elf view keeps the file's own coordinates.
			debugElf := f.(AsDebugElf).AsDebugElf()
			c.Check(debugElf != nil, "ELF%d: AsDebugElf returned nil", bits)

			// Transition through a sequence of biases, including
			// a negative one. The first transition happens before
			// any section data or relocation has been read, so
			// the caches are populated under bias.
			biases := []int64{0x100000, -0x800, 0x200000, 0}
			datas := make([]*Data, 0, len(biases))
			for _, b := range biases {
				err := f.SetLoadBias(b)
				c.Check(err == nil, "ELF%d: SetLoadBias(%#x): %v", bits, uint64(b), err)

				wantText := uint64(int64(0x1000) + b)
				c.Check(f.LoadBias() == b, "ELF%d: LoadBias = %#x, want %#x", bits, f.LoadBias(), b)
				c.Check(f.Section(textID) == text, "ELF%d: Section pointer changed at bias %#x", bits, uint64(b))
				c.Check(text.Addr == wantText, "ELF%d: .text.Addr = %#x at bias %#x", bits, text.Addr, uint64(b))
				c.Check(f.Sym(0).Name == "entry" && f.Sym(0).Value == wantText,
					"ELF%d: entry = %q %#x at bias %#x", bits, f.Sym(0).Name, f.Sym(0).Value, uint64(b))
				c.Check(f.ResolveAddr(wantText) == text, "ELF%d: ResolveAddr missed at bias %#x", bits, uint64(b))

				d, err := data.Data(data.Bounds())
				c.Check(err == nil, "ELF%d: Data at bias %#x: %v", bits, uint64(b), err)
				if err == nil {
					checkFixtureRelocs(c, f, d, uint64(int64(0x2008)+b), uint64(int64(0x2018)+b))
					datas = append(datas, d)
				}
			}

			// Every Data returned earlier is frozen in the
			// coordinates that were in effect when it was read.
			for i, b := range biases {
				d := datas[i]
				wantData := uint64(int64(0x2000) + b)
				c.Check(d.Addr == wantData,
					"ELF%d: Data snapshot %d: addr = %#x, want %#x", bits, i, d.Addr, wantData)
				c.Check(len(d.R) == 2 && d.R[0].Addr == uint64(int64(0x2008)+b) && d.R[1].Addr == uint64(int64(0x2018)+b),
					"ELF%d: Data snapshot %d: relocs moved", bits, i)
			}

			// The underlying debug/elf file still uses link-time
			// coordinates.
			c.Check(debugElf.Sections[1].Addr == 0x1000,
				"ELF%d: debug/elf .text moved to %#x", bits, debugElf.Sections[1].Addr)
		}
	})
}
