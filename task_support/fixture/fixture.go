// Package fixture builds tiny lawful DWARF sections; it does not decode types.
package fixture

import "encoding/binary"

type Sections struct {
 Abbrev, Info []byte
 TypeOffset, AliasOffset uint32
 Signature uint64
}
func u32(b []byte,x uint32) []byte { return binary.LittleEndian.AppendUint32(b,x) }
func u64(b []byte,x uint64) []byte { return binary.LittleEndian.AppendUint64(b,x) }
func unit(body []byte) []byte { return append(u32(nil,uint32(len(body))),body...) }
// Legacy has a compile unit and a four-byte signed base type.
func Legacy() Sections {
 a:=[]byte{1,0x11,1,0,0, 2,0x24,0,3,8,0x3e,0x0b,0x0b,0x0b,0,0,0}
 b:=[]byte{4,0,0,0,0,0,8,1,2,'i','n','t',0,5,4,0}
 return Sections{Abbrev:a,Info:unit(b),TypeOffset:12}
}
// V5 contains a preceding CU with a ref_sig8 typedef and a following TU.
// Its advertised type offset is relative to its own unit header.
func V5() Sections {
 sig:=uint64(0x1122334455667788)
 a:=[]byte{1,0x11,1,0,0, 2,0x16,0,3,8,0x49,0x20,0,0,
 3,0x41,1,0,0, 4,0x24,0,3,8,0x3e,0x0b,0x0b,0x0b,0,0,0}
 cu:=[]byte{5,0,1,8,0,0,0,0,1,2,'A','l','i','a','s',0}
 cu=u64(cu,sig);cu=append(cu,0)
 info:=unit(cu);base:=uint32(len(info))
 tu:=[]byte{5,0,2,8,0,0,0,0};tu=u64(tu,sig);tu=u32(tu,25)
 tu=append(tu,3,4,'i','n','t',0,5,4,0)
 return Sections{Abbrev:a,Info:append(info,unit(tu)...),AliasOffset:13,TypeOffset:base+25,Signature:sig}
}
