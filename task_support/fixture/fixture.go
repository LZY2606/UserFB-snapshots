// Package fixture builds legal, inert ELF records for static reading.
package fixture

import (
	"debug/elf"
	"encoding/binary"
)

type section struct {
	name        string
	typ         uint32
	flags, addr uint64
	data        []byte
	size        uint64
	link, info  uint32
	entsize     uint64
}

// WithType returns a copy of an image with its e_type field set to typ,
// allowing the same layout to pose as, for example, an ET_EXEC.
func WithType(raw []byte, typ elf.Type) []byte {
	out := append([]byte(nil), raw...)
	binary.LittleEndian.PutUint16(out[16:], uint16(typ))
	return out
}

// Relocatable returns a copy of an image rewritten as an ET_REL
// relocatable object. The global relocation section is retargeted to
// .data, since relocatable files require every relocation section to
// name a target section.
func Relocatable(raw []byte) []byte {
	out := WithType(raw, elf.ET_REL)
	bo := binary.LittleEndian
	var shoff, shentsize, infoOff uint64
	if out[4] == 2 { // ELFCLASS64
		shoff = bo.Uint64(out[0x28:])
		shentsize, infoOff = 64, 44
	} else { // ELFCLASS32
		shoff = uint64(bo.Uint32(out[0x20:]))
		shentsize, infoOff = 40, 28
	}
	// Section 8 is the global relocation section; section 2 is .data.
	bo.PutUint32(out[shoff+8*shentsize+infoOff:], 2)
	return out
}

// Image returns a little-endian ELF32/i386 or ELF64/x86-64 shared object.
// Physical section IDs: text=1, data=2, bss=3, meta=4, tls=5,
// strtab=6, symtab=7, global relocations=8, targeted relocations=9, shstrtab=10.
// Symbol slots: null, function, absolute, undefined, common, TLS, metadata.
func Image(bits int) []byte {
	bo := binary.LittleEndian
	put16 := func(b []byte, o int, v uint16) { bo.PutUint16(b[o:], v) }
	put32 := func(b []byte, o int, v uint32) { bo.PutUint32(b[o:], v) }
	put64 := func(b []byte, o int, v uint64) { bo.PutUint64(b[o:], v) }
	data := make([]byte, 32)
	put32(data, 8, 0xfffffff9)
	put32(data, 24, 11)
	strs := []byte("\x00entry\x00absolute\x00external\x00common\x00thread\x00metadata\x00")
	symSize, relSize := 24, 24
	rt := uint32(elf.SHT_RELA)
	rn := ".rela"
	if bits == 32 {
		symSize, relSize = 16, 8
		rt = uint32(elf.SHT_REL)
		rn = ".rel"
	}
	syms := make([]byte, 7*symSize)
	names := []uint32{0, 1, 7, 16, 25, 32, 39}
	values := []uint64{0, 0x1000, 0x42, 0, 8, 4, 0}
	shns := []uint16{0, 1, uint16(elf.SHN_ABS), 0, uint16(elf.SHN_COMMON), 5, 4}
	types := []byte{0, byte(elf.STT_FUNC), byte(elf.STT_NOTYPE), byte(elf.STT_NOTYPE), byte(elf.STT_OBJECT), byte(elf.STT_TLS), byte(elf.STT_OBJECT)}
	for i := 1; i < 7; i++ {
		o := i * symSize
		put32(syms, o, names[i])
		if bits == 64 {
			syms[o+4] = 0x10 | types[i]
			put16(syms, o+6, shns[i])
			put64(syms, o+8, values[i])
			put64(syms, o+16, 4)
		} else {
			put32(syms, o+4, uint32(values[i]))
			put32(syms, o+8, 4)
			syms[o+12] = 0x10 | types[i]
			put16(syms, o+14, shns[i])
		}
	}
	reloc := func(addr uint64, addend int64) []byte {
		b := make([]byte, relSize)
		if bits == 64 {
			put64(b, 0, addr)
			put64(b, 8, 1<<32|uint64(elf.R_X86_64_64))
			put64(b, 16, uint64(addend))
		} else {
			put32(b, 0, uint32(addr))
			put32(b, 4, 1<<8|uint32(elf.R_386_32))
		}
		return b
	}
	ss := []section{{},
		{name: ".text", typ: 1, flags: 6, addr: 0x1000, data: []byte{0x90, 0x90, 0x90, 0xc3}},
		{name: ".data", typ: 1, flags: 3, addr: 0x2000, data: data},
		{name: ".bss", typ: 8, flags: 3, addr: 0x3000, size: 16},
		{name: ".meta", typ: 1, data: []byte{1, 2, 3, 4}},
		{name: ".tls", typ: 1, flags: uint64(elf.SHF_ALLOC | elf.SHF_WRITE | elf.SHF_TLS), addr: 0x4000, data: make([]byte, 8)},
		{name: ".strtab", typ: 3, data: strs},
		{name: ".symtab", typ: 2, data: syms, link: 6, info: 1, entsize: uint64(symSize)},
		{name: rn + ".dyn", typ: rt, data: reloc(0x2008, -7), link: 7, entsize: uint64(relSize)},
		{name: rn + ".data", typ: rt, data: reloc(0x2018, 11), link: 7, info: 2, entsize: uint64(relSize)},
		{name: ".shstrtab", typ: 3}}
	shstr := []byte{0}
	no := make([]uint32, len(ss))
	for i := 1; i < len(ss); i++ {
		no[i] = uint32(len(shstr))
		shstr = append(shstr, []byte(ss[i].name)...)
		shstr = append(shstr, 0)
	}
	ss[10].data = shstr
	eh, sh := 64, 64
	if bits == 32 {
		eh, sh = 52, 40
	}
	out := make([]byte, eh)
	offsets := make([]uint64, len(ss))
	for i := 1; i < len(ss); i++ {
		for len(out)%8 != 0 {
			out = append(out, 0)
		}
		offsets[i] = uint64(len(out))
		if ss[i].typ != 8 {
			ss[i].size = uint64(len(ss[i].data))
			out = append(out, ss[i].data...)
		}
	}
	for len(out)%8 != 0 {
		out = append(out, 0)
	}
	sho := len(out)
	out = append(out, make([]byte, sh*len(ss))...)
	copy(out, []byte{0x7f, 'E', 'L', 'F', 2, 1, 1})
	if bits == 32 {
		out[4] = 1
	}
	put16(out, 16, uint16(elf.ET_DYN))
	machine := elf.EM_X86_64
	if bits == 32 {
		machine = elf.EM_386
	}
	put16(out, 18, uint16(machine))
	put32(out, 20, 1)
	if bits == 64 {
		put64(out, 40, uint64(sho))
		put16(out, 52, uint16(eh))
		put16(out, 58, uint16(sh))
		put16(out, 60, uint16(len(ss)))
		put16(out, 62, 10)
	} else {
		put32(out, 32, uint32(sho))
		put16(out, 40, uint16(eh))
		put16(out, 46, uint16(sh))
		put16(out, 48, uint16(len(ss)))
		put16(out, 50, 10)
	}
	for i, s := range ss {
		o := sho + i*sh
		put32(out, o, no[i])
		put32(out, o+4, s.typ)
		if bits == 64 {
			put64(out, o+8, s.flags)
			put64(out, o+16, s.addr)
			put64(out, o+24, offsets[i])
			put64(out, o+32, s.size)
			put32(out, o+40, s.link)
			put32(out, o+44, s.info)
			put64(out, o+48, 1)
			put64(out, o+56, s.entsize)
		} else {
			put32(out, o+8, uint32(s.flags))
			put32(out, o+12, uint32(s.addr))
			put32(out, o+16, uint32(offsets[i]))
			put32(out, o+20, uint32(s.size))
			put32(out, o+24, s.link)
			put32(out, o+28, s.info)
			put32(out, o+32, 1)
			put32(out, o+36, uint32(s.entsize))
		}
	}
	return out
}
