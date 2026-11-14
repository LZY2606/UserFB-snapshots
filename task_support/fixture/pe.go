// Package fixture builds a non-executed PE containing only synthetic metadata.
package fixture

import "encoding/binary"

func Disk(bits int) []byte {
	b := make([]byte, 0x800)
	u16 := func(o int, v uint16) { binary.LittleEndian.PutUint16(b[o:], v) }
	u32 := func(o int, v uint32) { binary.LittleEndian.PutUint32(b[o:], v) }
	u64 := func(o int, v uint64) { binary.LittleEndian.PutUint64(b[o:], v) }
	copy(b, "MZ")
	u32(0x3c, 0x80)
	copy(b[0x80:], "PE\x00\x00")
	opt, dirs, sec := 0x98, 0x98+96, 0x178
	base := uint64(0x400000)
	u16(0x84, 0x14c)
	u16(0x86, 1)
	u16(0x94, 224)
	u16(0x96, 0x102)
	u16(opt, 0x10b)
	u32(opt+28, uint32(base))
	u32(opt+92, 16)
	if bits == 64 {
		base = 0x140000000
		dirs = opt + 112
		sec = opt + 240
		u16(0x84, 0x8664)
		u16(0x94, 240)
		u16(0x96, 0x22)
		u16(opt, 0x20b)
		u64(opt+24, base)
		u32(opt+108, 16)
	}
	u32(opt+32, 0x1000)
	u32(opt+36, 0x200)
	u32(opt+56, 0x2000)
	u32(opt+60, 0x200)
	u16(opt+40, 6)
	u16(opt+48, 6)
	u16(opt+68, 3)
	copy(b[sec:], ".rdata")
	u32(sec+8, 0x800)
	u32(sec+12, 0x1000)
	u32(sec+16, 0x600)
	u32(sec+20, 0x200)
	u32(sec+36, 0x40000040)
	copy(b[0x250:], "DATA")
	// A TLS directory with one callback address and a terminating zero.
	u32(dirs+9*8, 0x1100)
	if bits == 64 {
		u32(dirs+9*8+4, 40)
		u64(0x300, base+0x1170)
		u64(0x308, base+0x1174)
		u64(0x310, base+0x1178)
		u64(0x318, base+0x1160)
		u64(0x360, base+0x1500)
	} else {
		u32(dirs+9*8+4, 24)
		u32(0x300, uint32(base+0x1170))
		u32(0x304, uint32(base+0x1174))
		u32(0x308, uint32(base+0x1178))
		u32(0x30c, uint32(base+0x1160))
		u32(0x360, uint32(base+0x1500))
	}
	// One CodeView record, with both RVA and physical-file coordinates.
	u32(dirs+6*8, 0x1200)
	u32(dirs+6*8+4, 28)
	u32(0x400+12, 2)
	u32(0x400+16, 30)
	u32(0x400+20, 0x1240)
	u32(0x400+24, 0x440)
	copy(b[0x440:], "RSDS")
	for i := 0; i < 16; i++ {
		b[0x444+i] = byte(i + 1)
	}
	u32(0x454, 3)
	copy(b[0x458:], "a.pdb\x00")
	return b
}
